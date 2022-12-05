/*
 * arrow_fdw.c
 *
 * Routines to map Apache Arrow files as PG's Foreign-Table.
 * ----
 * Copyright 2011-2021 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2021 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */
#include "pg_strom.h"
#include "arrow_defs.h"
#include "arrow_ipc.h"
#include "xpu_numeric.h"

/*
 * RecordBatchState
 */
typedef struct RecordBatchFieldState
{
	/* common fields with cache */
	Oid			atttypid;
	int			atttypmod;
	ArrowTypeOptions attopts;
	int64		nitems;				/* usually, same with rb_nitems */
	int64		null_count;
	off_t		nullmap_offset;
	size_t		nullmap_length;
	off_t		values_offset;
	size_t		values_length;
	off_t		extra_offset;
	size_t		extra_length;
	/* min/max statistics */
	SQLstat__datum stat_min;
	SQLstat__datum stat_max;
	bool		stat_isnull;
	/* sub-fields if any */
	int			num_children;
	struct RecordBatchFieldState *children;
} RecordBatchFieldState;

typedef struct RecordBatchState
{
	int			rb_index;	/* index number in a file */
	off_t		rb_offset;	/* offset from the head */
	size_t		rb_length;	/* length of the entire RecordBatch */
	int64		rb_nitems;	/* number of items */
	/* per column information */
	int			nfields;
	RecordBatchFieldState fields[FLEXIBLE_ARRAY_MEMBER];
} RecordBatchState;

typedef struct ArrowFileState
{
	const char *filename;
	File		filp;
	struct stat	stat_buf;
	Bitmapset  *optimal_gpus;	/* optimal GPUs, if any */
	DpuStorageEntry *ds_entry;	/* optimal DPU, if any */
	List	   *rb_list;	/* list of RecordBatchState */
} ArrowFileState;

/*
 * Metadata Cache (on shared memory)
 */
#define ARROW_METADATA_BLOCKSZ		(128 * 1024)	/* 128kB */
typedef struct
{
	dlist_node	chain;		/* link to free_blocks; NULL if active */
	int32_t		unitsz;		/* unit size of slab items  */
	int32_t		n_actives;	/* number of active items */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} arrowMetadataCacheBlock;
#define ARROW_METADATA_CACHE_FREE_MAGIC		(0xdeadbeafU)
#define ARROW_METADATA_CACHE_ACTIVE_MAGIC	(0xcafebabeU)

typedef struct arrowMetadataFieldCache	arrowMetadataFieldCache;
typedef struct arrowMetadataCache		arrowMetadataCache;

struct arrowMetadataFieldCache
{
	arrowMetadataCacheBlock *owner;
	dlist_node	chain;				/* link to free/fields[children] list */
	/* common fields with cache */
	Oid			atttypid;
	int			atttypmod;
	ArrowTypeOptions attopts;
	int64		nitems;				/* usually, same with rb_nitems */
	int64		null_count;
	off_t		nullmap_offset;
	size_t		nullmap_length;
	off_t		values_offset;
	size_t		values_length;
	off_t		extra_offset;
	size_t		extra_length;
	/* min/max statistics */
	SQLstat__datum stat_min;
	SQLstat__datum stat_max;
	bool		stat_isnull;
	/* sub-fields if any */
	int			num_children;
	dlist_head	children;
	uint32_t	magic;
};

struct arrowMetadataCache
{
	arrowMetadataCacheBlock *owner;
	dlist_node	chain;		/* link to free/hash list */
	dlist_node	lru_chain;	/* link to lru_list */
	struct timeval lru_tv;	/* last access time */
	arrowMetadataCache *next; /* next record-batch if any */
	struct stat stat_buf;	/* result of stat(2) */
	int			rb_index;	/* index number in a file */
	off_t		rb_offset;	/* offset from the head */
	size_t		rb_length;	/* length of the entire RecordBatch */
	int64		rb_nitems;	/* number of items */
	/* per column information */
	int			nfields;
	dlist_head	fields;		/* list of arrowMetadataFieldCache */
	uint32_t	magic;
};

/*
 * Metadata cache management
 */
#define ARROW_METADATA_HASH_NSLOTS		2000
typedef struct
{
	LWLock		mutex;
	slock_t		lru_lock;		/* protect lru related stuff */
	dlist_head	lru_list;
	dlist_head	free_blocks;	/* list of arrowMetadataCacheBlock */
	dlist_head	free_mcaches;	/* list of arrowMetadataCache */
	dlist_head	free_fcaches;	/* list of arrowMetadataFieldCache */
	dlist_head	hash_slots[ARROW_METADATA_HASH_NSLOTS];
} arrowMetadataCacheHead;

/*
 * Static variables
 */
static FdwRoutine			pgstrom_arrow_fdw_routine;
static shmem_request_hook_type shmem_request_next = NULL;
static shmem_startup_hook_type shmem_startup_next = NULL;
static arrowMetadataCacheHead *arrow_metadata_cache = NULL;
static bool					arrow_fdw_enabled;	/* GUC */
static bool					arrow_fdw_stats_hint_enabled;	/* GUC */
static int					arrow_metadata_cache_size_kb;	/* GUC */

/*
 * Static functions
 */



/* ----------------------------------------------------------------
 *
 * Apache Arrow <--> PG Types Mapping Routines
 *
 * ----------------------------------------------------------------
 */

/*
 * arrowFieldGetPGTypeHint
 */
static Oid
arrowFieldGetPGTypeHint(ArrowField *field)
{
	for (int i=0; i < field->_num_custom_metadata; i++)
	{
		ArrowKeyValue *kv = &field->custom_metadata[i];
		char	   *namebuf, *pos;
		Oid			namespace_oid = PG_CATALOG_NAMESPACE;
		HeapTuple	tup;

		if (strcmp(kv->key, "pg_type") != 0)
			continue;
		namebuf = alloca(kv->_value_len + 10);
		strcpy(namebuf, kv->value);
		pos = strchr(namebuf, '.');
		if (pos)
		{
			*pos++ = '\0';
			namespace_oid = get_namespace_oid(namebuf, true);
			if (!OidIsValid(namespace_oid))
				continue;
			namebuf = pos;
		}
		tup = SearchSysCache2(TYPENAMENSP,
							  PointerGetDatum(namebuf),
							  ObjectIdGetDatum(namespace_oid));
		if (HeapTupleIsValid(tup))
		{
			Oid		hint = ((Form_pg_type) GETSTRUCT(tup))->oid;

			ReleaseSysCache(tup);

			return hint;
		}
	}
	return InvalidOid;
}

/* ------------------------------------------------
 * Metadata Cache Management Routines
 *
 * MEMO: all of them requires the caller must have exclusive lock
 *       on the arrowMetadataCache::mutex
 * ------------------------------------------------
 */
static void
__releaseMetadataFieldCache(arrowMetadataFieldCache *fcache)
{
	arrowMetadataCacheBlock *mc_block = fcache->owner;

	Assert(fcache->magic == ARROW_METADATA_CACHE_ACTIVE_MAGIC);
	/* also release sub-fields if any */
	while (!dlist_is_empty(&fcache->children))
	{
		arrowMetadataFieldCache	*__fcache
			= dlist_container(arrowMetadataFieldCache, chain,
							  dlist_pop_head_node(&fcache->children));
		__releaseMetadataFieldCache(__fcache);
	}
	fcache->magic = ARROW_METADATA_CACHE_FREE_MAGIC;
	dlist_push_tail(&arrow_metadata_cache->free_fcaches,
					&fcache->chain);

	/* also back the owner block if all slabs become free */
	Assert(mc_block->n_actives > 0);
	if (--mc_block->n_actives == 0)
	{
		char   *pos = mc_block->data;
		char   *end = (char *)mc_block + ARROW_METADATA_BLOCKSZ;

		Assert(mc_block->unitsz == MAXALIGN(sizeof(arrowMetadataFieldCache)));
		while (pos + mc_block->unitsz <= end)
		{
			arrowMetadataFieldCache *__fcache = (arrowMetadataFieldCache *)pos;
			Assert(__fcache->owner == mc_block &&
				   __fcache->magic == ARROW_METADATA_CACHE_FREE_MAGIC);
			dlist_delete(&__fcache->chain);
			pos += mc_block->unitsz;
		}
		Assert(!mc_block->chain.prev &&
			   !mc_block->chain.next);	/* must be active block */
		dlist_push_tail(&arrow_metadata_cache->free_blocks,
						&mc_block->chain);
	}
}

static void
__releaseMetadataCache(arrowMetadataCache *mcache)
{
	while (mcache)
	{
		arrowMetadataCacheBlock *mc_block = mcache->owner;
		arrowMetadataCache   *__mcache_next = mcache->next;

		Assert(mcache->magic == ARROW_METADATA_CACHE_ACTIVE_MAGIC);
		/*
		 * MEMO: Caller already detach the leader mcache from the hash-
		 * slot and the LRU-list. The follower mcaches should never be
		 * linked to hash-slot and LRU-list.
		 * So, we just put Assert() here.
		 */
		Assert(!mcache->chain.prev && !mcache->chain.next &&
			   !mcache->lru_chain.prev && !mcache->lru_chain.next);

		/* also release arrowMetadataFieldCache */
		while (!dlist_is_empty(&mcache->fields))
		{
			arrowMetadataFieldCache *fcache
				= dlist_container(arrowMetadataFieldCache, chain,
								  dlist_pop_head_node(&mcache->fields));
			__releaseMetadataFieldCache(fcache);
		}
		mcache->magic = ARROW_METADATA_CACHE_FREE_MAGIC;
		dlist_push_tail(&arrow_metadata_cache->free_mcaches,
						&mcache->chain);
		/* also back the owner block if all slabs become free */
		Assert(mc_block->n_actives > 0);
		if (--mc_block->n_actives == 0)
		{
			char   *pos = mc_block->data;
			char   *end = (char *)mc_block + ARROW_METADATA_BLOCKSZ;

			Assert(mc_block->unitsz == MAXALIGN(sizeof(arrowMetadataCache)));
			while (pos + mc_block->unitsz <= end)
			{
				arrowMetadataCache *__mcache = (arrowMetadataCache *)pos;

				Assert(__mcache->owner == mc_block &&
					   __mcache->magic == ARROW_METADATA_CACHE_FREE_MAGIC);
				dlist_delete(&__mcache->chain);
				pos += mc_block->unitsz;
			}
			Assert(!mc_block->chain.prev &&
				   !mc_block->chain.next);	/* must be active block */
			dlist_push_tail(&arrow_metadata_cache->free_blocks,
							&mc_block->chain);
		}
		mcache = __mcache_next;
	}
}

static bool
__reclaimMetadataCache(void)
{
	SpinLockAcquire(&arrow_metadata_cache->lru_lock);
	if (!dlist_is_empty(&arrow_metadata_cache->lru_list))
	{
		arrowMetadataCache *mcache;
		dlist_node	   *dnode;
		struct timeval	curr_tv;
		int64_t			elapsed;

		gettimeofday(&curr_tv, NULL);
		dnode = dlist_tail_node(&arrow_metadata_cache->lru_list);
		mcache = dlist_container(arrowMetadataCache, lru_chain, dnode);
		elapsed = ((curr_tv.tv_sec - mcache->lru_tv.tv_sec) * 1000000 +
				   (curr_tv.tv_usec - mcache->lru_tv.tv_usec));
		if (elapsed > 30000000UL)	/* > 30s */
		{
			dlist_delete(&mcache->lru_chain);
			memset(&mcache->lru_chain, 0, sizeof(dlist_node));
			SpinLockRelease(&arrow_metadata_cache->lru_lock);
			dlist_delete(&mcache->chain);
			memset(&mcache->chain, 0, sizeof(dlist_node));

			__releaseMetadataCache(mcache);
			return true;
		}
	}
	SpinLockRelease(&arrow_metadata_cache->lru_lock);
	return false;
}

static arrowMetadataFieldCache *
__allocMetadataFieldCache(void)
{
	arrowMetadataFieldCache *fcache;
	dlist_node *dnode;

	while (dlist_is_empty(&arrow_metadata_cache->free_fcaches))
	{
		arrowMetadataCacheBlock *mc_block;
		char   *pos, *end;

		while (dlist_is_empty(&arrow_metadata_cache->free_blocks))
		{
			if (!__reclaimMetadataCache())
				return NULL;
		}
		dnode = dlist_pop_head_node(&arrow_metadata_cache->free_blocks);
		mc_block = dlist_container(arrowMetadataCacheBlock, chain, dnode);
		memset(mc_block, 0, offsetof(arrowMetadataCacheBlock, data));
		mc_block->unitsz = MAXALIGN(sizeof(arrowMetadataFieldCache));
		for (pos = mc_block->data, end = (char *)mc_block + ARROW_METADATA_BLOCKSZ;
			 pos + mc_block->unitsz <= end;
			 pos += mc_block->unitsz)
		{
			fcache = (arrowMetadataFieldCache *)pos;
			fcache->owner = mc_block;
			fcache->magic = ARROW_METADATA_CACHE_FREE_MAGIC;
			dlist_push_tail(&arrow_metadata_cache->free_fcaches,
							&fcache->chain);
		}
	}
	dnode = dlist_pop_head_node(&arrow_metadata_cache->free_fcaches);
	fcache = dlist_container(arrowMetadataFieldCache, chain, dnode);
	fcache->owner->n_actives++;
	Assert(fcache->magic == ARROW_METADATA_CACHE_FREE_MAGIC);
	memset(&fcache->chain, 0, (offsetof(arrowMetadataFieldCache, magic) -
							   offsetof(arrowMetadataFieldCache, chain)));
	fcache->magic = ARROW_METADATA_CACHE_ACTIVE_MAGIC;
	return fcache;
}

static arrowMetadataCache *
__allocMetadataCache(void)
{
	arrowMetadataCache *mcache;
	dlist_node *dnode;

	if (dlist_is_empty(&arrow_metadata_cache->free_mcaches))
	{
		arrowMetadataCacheBlock *mc_block;
		char   *pos, *end;

		while (dlist_is_empty(&arrow_metadata_cache->free_blocks))
		{
			if (!__reclaimMetadataCache())
				return NULL;
		}
		dnode = dlist_pop_head_node(&arrow_metadata_cache->free_blocks);
		mc_block = dlist_container(arrowMetadataCacheBlock, chain, dnode);
		memset(mc_block, 0, offsetof(arrowMetadataCacheBlock, data));
		mc_block->unitsz = MAXALIGN(sizeof(arrowMetadataCache));
		for (pos = mc_block->data, end = (char *)mc_block + ARROW_METADATA_BLOCKSZ;
			 pos + mc_block->unitsz <= end;
			 pos += mc_block->unitsz)
		{
			mcache = (arrowMetadataCache *)pos;
			mcache->owner = mc_block;
			mcache->magic = ARROW_METADATA_CACHE_FREE_MAGIC;
			dlist_push_tail(&arrow_metadata_cache->free_mcaches,
							&mcache->chain);
		}
	}
	dnode = dlist_pop_head_node(&arrow_metadata_cache->free_mcaches);
	mcache = dlist_container(arrowMetadataCache, chain, dnode);
	mcache->owner->n_actives++;
	Assert(mcache->magic == ARROW_METADATA_CACHE_FREE_MAGIC);
	memset(&mcache, 0, (offsetof(arrowMetadataCache, magic) -
						offsetof(arrowMetadataCache, chain)));
	mcache->magic = ARROW_METADATA_CACHE_ACTIVE_MAGIC;
	return mcache;
}

/*
 * lookupArrowMetadataCache
 *
 * caller must hold "at least" shared lock on the arrow_metadata_cache->mutex.
 * if exclusive lock is held, it may invalidate legacy cache if any.
 */
static inline uint32_t
arrowMetadataHashIndex(struct stat *stat_buf)
{
	struct {
		dev_t	st_dev;
		ino_t	st_ino;
	} hkey;
	uint32_t	hash;

	hkey.st_dev = stat_buf->st_dev;
	hkey.st_ino = stat_buf->st_ino;
	hash = hash_bytes((unsigned char *)&hkey, sizeof(hkey));
	return hash % ARROW_METADATA_HASH_NSLOTS;
}

static arrowMetadataCache *
lookupArrowMetadataCache(struct stat *stat_buf, bool has_exclusive)
{
	arrowMetadataCache *mcache;
	uint32_t	hindex;
	dlist_iter	iter;

	hindex = arrowMetadataHashIndex(stat_buf);
	dlist_foreach(iter, &arrow_metadata_cache->hash_slots[hindex])
	{
		mcache = dlist_container(arrowMetadataCache, chain, iter.cur);

		if (stat_buf->st_dev == mcache->stat_buf.st_dev &&
			stat_buf->st_ino == mcache->stat_buf.st_ino)
		{
			/*
			 * Is the metadata cache still valid?
			 */
			if (stat_buf->st_mtim.tv_sec < mcache->stat_buf.st_mtim.tv_sec ||
				(stat_buf->st_mtim.tv_sec == mcache->stat_buf.st_mtim.tv_sec &&
				 stat_buf->st_mtim.tv_nsec <= mcache->stat_buf.st_mtim.tv_nsec))
			{
				/* ok, found */
				SpinLockAcquire(&arrow_metadata_cache->lru_lock);
				gettimeofday(&mcache->lru_tv, NULL);
				dlist_move_head(&arrow_metadata_cache->lru_list,
								&mcache->lru_chain);
				SpinLockRelease(&arrow_metadata_cache->lru_lock);
				return mcache;
			}
			else if (has_exclusive)
			{
				/*
				 * Unfortunatelly, metadata cache is already invalid.
				 * If caller has exclusive lock, we release it.
				 */
				SpinLockAcquire(&arrow_metadata_cache->lru_lock);
				dlist_delete(&mcache->lru_chain);
				memset(&mcache->lru_chain, 0, sizeof(dlist_node));
				SpinLockRelease(&arrow_metadata_cache->lru_lock);
				dlist_delete(&mcache->chain);
				memset(&mcache->chain, 0, sizeof(dlist_node));

				__releaseMetadataCache(mcache);
			}
		}
	}
	return NULL;
}

/* ----------------------------------------------------------------
 *
 * buildArrowStatsBinary
 *
 * ...and, routines related to Arrow Min/Max statistics
 *
 * ----------------------------------------------------------------
 */
typedef struct arrowFieldStatsBinary
{
	uint32	nrooms;		/* number of record-batches */
	int		unitsz;		/* unit size of min/max statistics */
	bool   *isnull;
	char   *min_values;
	char   *max_values;
	int		nfields;	/* if List/Struct data type */
	struct arrowFieldStatsBinary *subfields;
} arrowFieldStatsBinary;

typedef struct
{
	int		nitems;		/* number of record-batches */
	int		nfields;	/* number of columns */
	arrowFieldStatsBinary fields[FLEXIBLE_ARRAY_MEMBER];
} arrowStatsBinary;

static void
__releaseArrowFieldStatsBinary(arrowFieldStatsBinary *bstats)
{
	if (bstats->subfields)
	{
		for (int j=0; j < bstats->nfields; j++)
			__releaseArrowFieldStatsBinary(&bstats->subfields[j]);
		pfree(bstats->subfields);
	}
	if (bstats->isnull)
		pfree(bstats->isnull);
	if (bstats->min_values)
		pfree(bstats->min_values);
	if (bstats->max_values)
		pfree(bstats->max_values);
}

static void
releaseArrowStatsBinary(arrowStatsBinary *arrow_bstats)
{
	if (arrow_bstats)
	{
		for (int j=0; j < arrow_bstats->nfields; j++)
			__releaseArrowFieldStatsBinary(&arrow_bstats->fields[j]);
		pfree(arrow_bstats);
	}
}

static int128_t
__atoi128(const char *tok, bool *p_isnull)
{
	int128_t	ival = 0;
	bool		is_minus = false;

	if (*tok == '-')
	{
		is_minus = true;
		tok++;
	}
	while (isdigit(*tok))
	{
		ival = 10 * ival + (*tok - '0');
		tok++;
	}

	if (*tok != '\0')
		*p_isnull = true;
	if (is_minus)
	{
		if (ival == 0)
			*p_isnull = true;
		ival = -ival;
	}
	return ival;
}

static bool
__parseArrowFieldStatsBinary(arrowFieldStatsBinary *bstats,
							 ArrowField *field,
							 const char *min_tokens,
							 const char *max_tokens)
{
	int			unitsz = -1;
	char	   *min_buffer;
	char	   *max_buffer;
	char	   *min_values = NULL;
	char	   *max_values = NULL;
	bool	   *isnull = NULL;
	char	   *tok1, *pos1;
	char	   *tok2, *pos2;
	uint32		index;

	/* determine the unitsz of datum */
	switch (field->type.node.tag)
	{
		case ArrowNodeTag__Int:
			switch (field->type.Int.bitWidth)
			{
				case 8:
					unitsz = sizeof(uint8_t);
					break;
				case 16:
					unitsz = sizeof(uint16_t);
					break;
				case 32:
					unitsz = sizeof(uint32_t);
					break;
				case 64:
					unitsz = sizeof(uint64_t);
					break;
				default:
					return false;
			}
			break;

		case ArrowNodeTag__FloatingPoint:
			switch (field->type.FloatingPoint.precision)
			{
				case ArrowPrecision__Half:
					unitsz = sizeof(uint16_t);
					break;
				case ArrowPrecision__Single:
					unitsz = sizeof(uint32_t);
					break;
				case ArrowPrecision__Double:
					unitsz = sizeof(uint64_t);
					break;
				default:
					return false;
			}
			break;

		case ArrowNodeTag__Decimal:
			unitsz = sizeof(int128_t);
			break;

		case ArrowNodeTag__Date:
			switch (field->type.Date.unit)
			{
				case ArrowDateUnit__Day:
					unitsz = sizeof(uint32_t);
					break;
				case ArrowDateUnit__MilliSecond:
					unitsz = sizeof(uint64_t);
					break;
				default:
					return false;
			}
			break;

		case ArrowNodeTag__Time:
			switch (field->type.Time.unit)
			{
				case ArrowTimeUnit__Second:
				case ArrowTimeUnit__MilliSecond:
					unitsz = sizeof(uint32_t);
					break;
				case ArrowTimeUnit__MicroSecond:
				case ArrowTimeUnit__NanoSecond:
					unitsz = sizeof(uint64_t);
					break;
				default:
					return false;
			}
			break;

		case ArrowNodeTag__Timestamp:
			switch (field->type.Timestamp.unit)
			{
				case ArrowTimeUnit__Second:
				case ArrowTimeUnit__MilliSecond:
				case ArrowTimeUnit__MicroSecond:
				case ArrowTimeUnit__NanoSecond:
					unitsz = sizeof(uint64_t);
					break;
				default:
					return false;
			}
			break;
		default:
			return false;
	}
	Assert(unitsz > 0);
	/* parse the min_tokens/max_tokens */
	min_buffer = alloca(strlen(min_tokens) + 1);
	max_buffer = alloca(strlen(max_tokens) + 1);
	strcpy(min_buffer, min_tokens);
	strcpy(max_buffer, max_tokens);

	min_values = palloc0(unitsz * bstats->nrooms);
	max_values = palloc0(unitsz * bstats->nrooms);
	isnull     = palloc0(sizeof(bool) * bstats->nrooms);
	for (tok1 = strtok_r(min_buffer, ",", &pos1),
		 tok2 = strtok_r(max_buffer, ",", &pos2), index = 0;
		 tok1 != NULL && tok2 != NULL && index < bstats->nrooms;
		 tok1 = strtok_r(NULL, ",", &pos1),
		 tok2 = strtok_r(NULL, ",", &pos2), index++)
	{
		bool		__isnull = false;
		int128_t	__min = __atoi128(__trim(tok1), &__isnull);
		int128_t	__max = __atoi128(__trim(tok2), &__isnull);

		if (__isnull)
			isnull[index] = true;
		else
		{
			memcpy(min_values + unitsz * index, &__min, unitsz);
			memcpy(max_values + unitsz * index, &__max, unitsz);
		}
	}
	/* sanity checks */
	if (!tok1 && !tok2 && index == bstats->nrooms)
	{
		bstats->unitsz = unitsz;
		bstats->isnull = isnull;
		bstats->min_values = min_values;
		bstats->max_values = max_values;
		return true;
	}
	/* elsewhere, something wrong */
	pfree(min_values);
	pfree(max_values);
	pfree(isnull);
	return false;
}

static bool
__buildArrowFieldStatsBinary(arrowFieldStatsBinary *bstats,
							 ArrowField *field,
							 uint32 numRecordBatches)
{
	const char *min_tokens = NULL;
	const char *max_tokens = NULL;
	int			j, k;
	bool		retval = false;

	for (k=0; k < field->_num_custom_metadata; k++)
	{
		ArrowKeyValue *kv = &field->custom_metadata[k];

		if (strcmp(kv->key, "min_values") == 0)
			min_tokens = kv->value;
		else if (strcmp(kv->key, "max_values") == 0)
			max_tokens = kv->value;
	}

	bstats->nrooms = numRecordBatches;
	bstats->unitsz = -1;
	if (min_tokens && max_tokens)
	{
		if (__parseArrowFieldStatsBinary(bstats, field,
										 min_tokens,
										 max_tokens))
		{
			retval = true;
		}
		else
		{
			/* parse error, ignore the stat */
			if (bstats->isnull)
				pfree(bstats->isnull);
			if (bstats->min_values)
				pfree(bstats->min_values);
			if (bstats->max_values)
				pfree(bstats->max_values);
			bstats->unitsz     = -1;
			bstats->isnull     = NULL;
			bstats->min_values = NULL;
			bstats->max_values = NULL;
		}
	}

	if (field->_num_children > 0)
	{
		bstats->nfields = field->_num_children;
		bstats->subfields = palloc0(sizeof(arrowFieldStatsBinary) * bstats->nfields);
		for (j=0; j < bstats->nfields; j++)
		{
			if (__buildArrowFieldStatsBinary(&bstats->subfields[j],
											 &field->children[j],
											 numRecordBatches))
				retval = true;
		}
	}
	return retval;
}

static arrowStatsBinary *
buildArrowStatsBinary(const ArrowFooter *footer)
{
	arrowStatsBinary *arrow_bstats;
	int		nfields = footer->schema._num_fields;
	bool	found = false;

	arrow_bstats = palloc0(offsetof(arrowStatsBinary,
									fields[nfields]));
	arrow_bstats->nitems = footer->_num_recordBatches;
	arrow_bstats->nfields = nfields;
	for (int j=0; j < nfields; j++)
	{
		if (__buildArrowFieldStatsBinary(&arrow_bstats->fields[j],
										 &footer->schema.fields[j],
										 footer->_num_recordBatches))
			found = true;
	}
	if (!found)
	{
		releaseArrowStatsBinary(arrow_bstats);
		return NULL;
	}
	return arrow_bstats;
}

/*
 * applyArrowStatsBinary
 */
static void
__applyArrowFieldStatsBinary(RecordBatchFieldState *fstate,
							 arrowFieldStatsBinary *bstats,
							 int rb_index)
{
	int		j;

	if (bstats->unitsz > 0 &&
		bstats->isnull != NULL &&
		bstats->min_values != NULL &&
		bstats->max_values != NULL)
	{
		size_t	off = bstats->unitsz * rb_index;

		memcpy(&fstate->stat_min,
			   bstats->min_values + off, bstats->unitsz);
		memcpy(&fstate->stat_max,
			   bstats->max_values + off, bstats->unitsz);
		fstate->stat_isnull = false;
	}
	else
	{
		memset(&fstate->stat_min, 0, sizeof(SQLstat__datum));
		memset(&fstate->stat_max, 0, sizeof(SQLstat__datum));
		fstate->stat_isnull = true;
	}
	
	Assert(fstate->num_children == bstats->nfields);
	for (j=0; j < fstate->num_children; j++)
	{
		RecordBatchFieldState  *__fstate = &fstate->children[j];
		arrowFieldStatsBinary  *__bstats = &bstats->subfields[j];

		__applyArrowFieldStatsBinary(__fstate, __bstats, rb_index);
	}
}

static void
applyArrowStatsBinary(RecordBatchState *rb_state, arrowStatsBinary *arrow_bstats)
{
	Assert(rb_state->nfields == arrow_bstats->nfields &&
		   rb_state->rb_index < arrow_bstats->nitems);
	for (int j=0; j < rb_state->nfields; j++)
	{
		__applyArrowFieldStatsBinary(&rb_state->fields[j],
									 &arrow_bstats->fields[j],
									 rb_state->rb_index);
	}
}









/* ----------------------------------------------------------------
 *
 * BuildArrowFileState
 *
 * It build RecordBatchState based on the metadata-cache, or raw Arrow files.
 * ----------------------------------------------------------------
 */
static void
__buildRecordBatchFieldStateByCache(RecordBatchFieldState *rb_field,
									arrowMetadataFieldCache *fcache)
{
	rb_field->atttypid       = fcache->atttypid;
	rb_field->atttypmod      = fcache->atttypmod;
	rb_field->attopts        = fcache->attopts;
	rb_field->nitems         = fcache->nitems;
	rb_field->null_count     = fcache->null_count;
	rb_field->nullmap_offset = fcache->nullmap_offset;
	rb_field->nullmap_length = fcache->nullmap_length;
	rb_field->values_offset  = fcache->values_offset;
	rb_field->values_length  = fcache->values_length;
	rb_field->extra_offset   = fcache->extra_offset;
	rb_field->extra_length   = fcache->extra_length;
	rb_field->stat_min       = fcache->stat_min;
	rb_field->stat_max       = fcache->stat_max;
	rb_field->stat_isnull	 = fcache->stat_isnull;
	if (fcache->num_children > 0)
	{
		dlist_iter	iter;
		int			j = 0;

		rb_field->num_children = fcache->num_children;
		rb_field->children = palloc0(sizeof(RecordBatchFieldState) *
									 fcache->num_children);
		dlist_foreach(iter, &fcache->children)
		{
			arrowMetadataFieldCache *__fcache
				= dlist_container(arrowMetadataFieldCache, chain, iter.cur);
			__buildRecordBatchFieldStateByCache(&rb_field->children[j++], __fcache);
		}
		Assert(j == rb_field->num_children);
	}
	else
	{
		Assert(dlist_is_empty(&fcache->children));
	}
}

static ArrowFileState *
__buildArrowFileStateByCache(const char *filename, arrowMetadataCache *mcache)
{
	ArrowFileState	   *af_state;

	af_state = palloc0(sizeof(ArrowFileState));
	af_state->filename = pstrdup(filename);
	af_state->filp = -1;
	memcpy(&af_state->stat_buf, &mcache->stat_buf, sizeof(struct stat));

	while (mcache)
	{
		RecordBatchState *rb_state;
		dlist_iter	iter;
		int			j = 0;

		rb_state = palloc0(offsetof(RecordBatchState,
									fields[mcache->nfields]));
		rb_state->rb_index  = mcache->rb_index;
		rb_state->rb_offset = mcache->rb_offset;
		rb_state->rb_length = mcache->rb_length;
		rb_state->rb_nitems = mcache->rb_nitems;
		rb_state->nfields   = mcache->nfields;
		dlist_foreach(iter, &mcache->fields)
		{
			arrowMetadataFieldCache *fcache
				= dlist_container(arrowMetadataFieldCache, chain, iter.cur);
			__buildRecordBatchFieldStateByCache(&rb_state->fields[j++], fcache);
		}
		Assert(j == rb_state->nfields);
		af_state->rb_list = lappend(af_state->rb_list, rb_state);

		mcache = mcache->next;
	}
	return af_state;
}

/*
 * Routines to setup RecordBatchState by raw-file
 */
typedef struct
{
	ArrowBuffer	   *buffer_curr;
	ArrowBuffer	   *buffer_tail;
	ArrowFieldNode *fnode_curr;
	ArrowFieldNode *fnode_tail;
} setupRecordBatchContext;

static void
__assignRecordBatchFieldStateBuffer(RecordBatchFieldState *fstate,
									setupRecordBatchContext *con,
									bool has_extra)
{
	ArrowBuffer	   *buffer_curr;

	/* nullmap */
	buffer_curr = con->buffer_curr++;
	if (buffer_curr >= con->buffer_tail)
		elog(ERROR, "RecordBatch has less buffers than expected");
	if (fstate->null_count > 0)
	{
		fstate->nullmap_offset = buffer_curr->offset;
		fstate->nullmap_length = buffer_curr->length;
		if (fstate->nullmap_length < BITMAPLEN(fstate->nitems))
			elog(ERROR, "nullmap length is smaller than expected");
		if (fstate->nullmap_offset != MAXALIGN(fstate->nullmap_offset))
			elog(ERROR, "nullmap is not aligned well");
	}

	if (fstate->attopts.unitsz != 0)
	{
		size_t		least_length;

		if (fstate->attopts.unitsz < 0)
			least_length = BITMAPLEN(fstate->nitems);
		else if (!has_extra)
			least_length = fstate->attopts.unitsz * fstate->nitems;
		else
			least_length = fstate->attopts.unitsz * (fstate->nitems + 1);
		
		buffer_curr = con->buffer_curr++;
		if (buffer_curr >= con->buffer_tail)
			elog(ERROR, "RecordBatch has less buffers than expected");
		fstate->values_offset = buffer_curr->offset;
		fstate->values_length = buffer_curr->length;
		if (fstate->values_length < MAXALIGN(least_length))
			elog(ERROR, "values array is smaller than expected");
		if (fstate->values_offset != MAXALIGN(fstate->values_offset))
			elog(ERROR, "values array is not aligned well");
	}

	if (has_extra)
	{
		Assert(fstate->attopts.unitsz > 0);
		buffer_curr = con->buffer_curr++;
		if (buffer_curr >= con->buffer_tail)
			elog(ERROR, "RecordBatch has less buffers than expected");
		fstate->extra_offset = buffer_curr->offset;
		fstate->extra_length = buffer_curr->length;
		if (fstate->extra_offset != MAXALIGN(fstate->extra_offset))
			elog(ERROR, "extra buffer is not aligned well");
	}
}

static void
__assignRecordBatchFieldStateComposite(RecordBatchFieldState *fstate, Oid hint_oid)
{
	Relation	rel;
	ScanKeyData	skeys[3];
	SysScanDesc	sscan;
	bool		found = false;

	rel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(&skeys[0],
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(RELKIND_COMPOSITE_TYPE));
	ScanKeyInit(&skeys[1],
				Anum_pg_class_relnatts,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(fstate->num_children));
	ScanKeyInit(&skeys[2],
                Anum_pg_class_oid,
				BTEqualStrategyNumber, F_OIDNE,
				ObjectIdGetDatum(hint_oid));
	sscan = systable_beginscan(rel, InvalidOid, false, NULL,
							   OidIsValid(hint_oid) ? 3 : 2, skeys);
	for (;;)
	{
		HeapTuple	htup;
		TupleDesc	tupdesc;
		Oid			comp_oid;

		if (OidIsValid(hint_oid))
		{
			comp_oid = hint_oid;
			hint_oid = InvalidOid;
		}
		else
		{
			htup = systable_getnext(sscan);
			if (!HeapTupleIsValid(htup))
				break;
			comp_oid = ((Form_pg_type) GETSTRUCT(htup))->oid;
		}

		if (pg_type_aclcheck(comp_oid,
							 GetUserId(),
							 ACL_USAGE) != ACLCHECK_OK)
			continue;

		tupdesc = lookup_rowtype_tupdesc_noerror(comp_oid, -1, true);
		if (!tupdesc)
			continue;
		if (tupdesc->natts == fstate->num_children)
		{
			for (int j=0; j < tupdesc->natts; j++)
			{
				Form_pg_attribute attr = TupleDescAttr(tupdesc, j);
				RecordBatchFieldState *child = &fstate->children[j];

				if (attr->atttypid != child->atttypid)
					goto not_matched;
			}
			fstate->atttypid = comp_oid;
			found = true;
			break;
		}
	not_matched:
		ReleaseTupleDesc(tupdesc);
	}
	systable_endscan(sscan);
	table_close(rel, AccessShareLock);

	if (!found)
		elog(ERROR, "arrow_fdw: didn't find out compatible composite type");
}

static void
__buildRecordBatchFieldState(setupRecordBatchContext *con,
							 RecordBatchFieldState *fstate,
							 ArrowField *field, int depth)
{
	ArrowFieldNode *fnode;
	ArrowType	   *t = &field->type;
	Oid				hint_oid = arrowFieldGetPGTypeHint(field);
	ArrowTypeOptions *attopts = &fstate->attopts;

	if (con->fnode_curr >= con->fnode_tail)
		elog(ERROR, "RecordBatch has less ArrowFieldNode than expected");
	fnode = con->fnode_curr++;
	fstate->atttypid    = InvalidOid;
	fstate->atttypmod   = -1;
	fstate->nitems      = fnode->length;
	fstate->null_count  = fnode->null_count;
	fstate->stat_isnull = true;

	switch (t->node.tag)
	{
		case ArrowNodeTag__Int:
			attopts->tag = ArrowType__Int;
			switch (t->Int.bitWidth)
			{
				case 8:
					attopts->unitsz = sizeof(int8_t);
					fstate->atttypid =
						GetSysCacheOid2(TYPENAMENSP,
										Anum_pg_type_oid,
										CStringGetDatum("int1"),
										ObjectIdGetDatum(PG_CATALOG_NAMESPACE));
					break;
				case 16:
					attopts->unitsz = sizeof(int16_t);
					fstate->atttypid = INT2OID;
					break;
				case 32:
					attopts->unitsz = sizeof(int32_t);
					fstate->atttypid = INT4OID;
					break;
				case 64:
					attopts->unitsz = sizeof(int64_t);
					fstate->atttypid = INT8OID;
					break;
				default:
					elog(ERROR, "Arrow::Int bitWidth=%d is not supported",
						 t->Int.bitWidth);
			}
			attopts->integer.bitWidth  = t->Int.bitWidth;
			attopts->integer.is_signed = t->Int.is_signed;
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			break;

		case ArrowNodeTag__FloatingPoint:
			attopts->tag = ArrowType__FloatingPoint;
			switch (t->FloatingPoint.precision)
			{
				case ArrowPrecision__Half:
					attopts->unitsz = sizeof(float2_t);
					fstate->atttypid =
						GetSysCacheOid2(TYPENAMENSP,
										Anum_pg_type_oid,
										CStringGetDatum("float2"),
										ObjectIdGetDatum(PG_CATALOG_NAMESPACE));
					break;
				case ArrowPrecision__Single:
					attopts->unitsz = sizeof(float4_t);
					fstate->atttypid = FLOAT4OID;
					break;
				case ArrowPrecision__Double:
					attopts->unitsz = sizeof(float8_t);
					fstate->atttypid = FLOAT8OID;
					break;
				default:
					elog(ERROR, "Arrow::FloatingPoint unknown precision (%d)",
						 (int)t->FloatingPoint.precision);
			}
			attopts->floating_point.precision = t->FloatingPoint.precision;
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			break;

		case ArrowNodeTag__Bool:
			attopts->tag = ArrowType__Bool;
			attopts->unitsz = -1;		/* values is bitmap */
			fstate->atttypid = BOOLOID;
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			break;
			
		case ArrowNodeTag__Decimal:
			if (t->Decimal.bitWidth != 128)
				elog(ERROR, "Arrow::Decimal%u is not supported", t->Decimal.bitWidth);
			attopts->tag               = ArrowType__Decimal;
			attopts->unitsz            = sizeof(int128_t);
			attopts->decimal.precision = t->Decimal.precision;
			attopts->decimal.scale     = t->Decimal.scale;
			attopts->decimal.bitWidth  = t->Decimal.bitWidth;
			fstate->atttypid = NUMERICOID;
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			break;

		case ArrowNodeTag__Date:
			attopts->tag = ArrowType__Date;
			switch (t->Date.unit)
			{
				case ArrowDateUnit__Day:
					attopts->unitsz = sizeof(int32_t);
					break;
				case ArrowDateUnit__MilliSecond:
					attopts->unitsz = sizeof(int32_t);
                    break;
				default:
					elog(ERROR, "Arrow::Date unknown unit (%d)",
						 (int)t->Date.unit);
			}
			attopts->date.unit = t->Date.unit;
			fstate->atttypid = DATEOID;
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			break;

		case ArrowNodeTag__Time:
			attopts->tag = ArrowType__Time;
			switch (t->Time.unit)
			{
				case ArrowTimeUnit__Second:
				case ArrowTimeUnit__MilliSecond:
					attopts->unitsz = sizeof(int32_t);
					break;
				case ArrowTimeUnit__MicroSecond:
				case ArrowTimeUnit__NanoSecond:
					attopts->unitsz = sizeof(int64_t);
					break;
				default:
					elog(ERROR, "unknown Time::unit (%d)",
						 (int)t->Time.unit);
			}
			attopts->time.unit = t->Time.unit;
			fstate->atttypid = TIMEOID;
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			break;

		case ArrowNodeTag__Timestamp:
			attopts->tag = ArrowType__Timestamp;
			switch (t->Timestamp.unit)
			{
				case ArrowTimeUnit__Second:
				case ArrowTimeUnit__MilliSecond:
				case ArrowTimeUnit__MicroSecond:
				case ArrowTimeUnit__NanoSecond:
					attopts->unitsz = sizeof(int64_t);
					attopts->timestamp.unit = t->Timestamp.unit;
					break;
				default:
					elog(ERROR, "unknown Timestamp::unit (%d)",
						 (int)t->Timestamp.unit);
			}
			fstate->atttypid = (t->Timestamp.timezone
								? TIMESTAMPTZOID
								: TIMESTAMPOID);
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			break;

		case ArrowNodeTag__Interval:
			attopts->tag = ArrowType__Interval;
			switch (t->Interval.unit)
			{
				case ArrowIntervalUnit__Year_Month:
					attopts->unitsz = sizeof(int32_t);
					break;
				case ArrowIntervalUnit__Day_Time:
					attopts->unitsz = sizeof(int64_t);
					break;
				default:
					elog(ERROR, "unknown Interval::unit (%d)",
                         (int)t->Interval.unit);
			}
			fstate->atttypid = INTERVALOID;
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			break;

		case ArrowNodeTag__FixedSizeBinary:
			attopts->tag = ArrowType__FixedSizeBinary;
			attopts->unitsz = t->FixedSizeBinary.byteWidth;
			attopts->fixed_size_binary.byteWidth = t->FixedSizeBinary.byteWidth;
			if (t->FixedSizeBinary.byteWidth <= 0 ||
				t->FixedSizeBinary.byteWidth > BLCKSZ)
				elog(ERROR, "arrow_fdw: %s with byteWidth=%d is not supported", 
					 t->node.tagName,
					 t->FixedSizeBinary.byteWidth);
			if (hint_oid == MACADDROID &&
				t->FixedSizeBinary.byteWidth == 6)
			{
				fstate->atttypid = MACADDROID;
			}
			else if (hint_oid == INETOID &&
					 (t->FixedSizeBinary.byteWidth == 4 ||
                      t->FixedSizeBinary.byteWidth == 16))
			{
				fstate->atttypid = INETOID;
			}
			else
			{
				fstate->atttypid = BYTEAOID;
				fstate->atttypmod = VARHDRSZ + t->FixedSizeBinary.byteWidth;
			}
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			break;

		case ArrowNodeTag__Utf8:
			attopts->tag = ArrowType__Utf8;
			attopts->unitsz = sizeof(uint32_t);
			fstate->atttypid = TEXTOID;
			__assignRecordBatchFieldStateBuffer(fstate, con, true);
			break;

		case ArrowNodeTag__LargeUtf8:
			attopts->tag = ArrowType__LargeUtf8;
			attopts->unitsz = sizeof(uint64_t);
			fstate->atttypid = TEXTOID;
			__assignRecordBatchFieldStateBuffer(fstate, con, true);
			break;

		case ArrowNodeTag__Binary:
			attopts->tag = ArrowType__Binary;
			attopts->unitsz = sizeof(uint32_t);
			fstate->atttypid = BYTEAOID;
			__assignRecordBatchFieldStateBuffer(fstate, con, true);
			break;

		case ArrowNodeTag__LargeBinary:
			attopts->tag = ArrowType__LargeBinary;
			attopts->unitsz = sizeof(uint64_t);
			fstate->atttypid = BYTEAOID;
			__assignRecordBatchFieldStateBuffer(fstate, con, true);
			break;

		case ArrowNodeTag__List:
		case ArrowNodeTag__LargeList:
			if (field->_num_children != 1)
				elog(ERROR, "Bug? List of arrow type is corrupted");
			if (depth > 0)
				elog(ERROR, "nested array type is not supported");
			if (t->node.tag == ArrowNodeTag__List)
			{
				attopts->tag = ArrowType__List;
				attopts->unitsz = sizeof(uint32_t);
			}
			else
			{
				attopts->tag = ArrowType__LargeList;
				attopts->unitsz = sizeof(uint64_t);
			}
			__assignRecordBatchFieldStateBuffer(fstate, con, false);
			/* setup element of the array */
			fstate->children = palloc0(sizeof(RecordBatchFieldState));
			fstate->num_children = 1;
			__buildRecordBatchFieldState(con,
										 &fstate->children[0],
										 &field->children[0],
										 depth+1);
			/* identify the PG array type */
			fstate->atttypid = get_array_type(fstate->children[0].atttypid);
			if (!OidIsValid(fstate->atttypid))
				elog(ERROR, "type '%s' has no array type",
					 format_type_be(fstate->children[0].atttypid));
			break;

		case ArrowNodeTag__Struct:
			if (depth > 0)
				elog(ERROR, "nested composite type is not supported");
			attopts->tag = ArrowType__Struct;
			attopts->unitsz = 0;		/* only nullmap */
			__assignRecordBatchFieldStateBuffer(fstate, con, false);

			if (field->_num_children > 0)
			{
				fstate->children = palloc0(sizeof(RecordBatchFieldState) *
										   field->_num_children);
				for (int i=0; i < field->_num_children; i++)
				{
					__buildRecordBatchFieldState(con,
												 &fstate->children[i],
												 &field->children[i],
												 depth+1);
				}
			}
			fstate->num_children = field->_num_children;
			__assignRecordBatchFieldStateComposite(fstate, hint_oid);
			break;

		default:
			elog(ERROR, "Bug? ArrowSchema contains unsupported types");
	}
}

static RecordBatchState *
__buildRecordBatchStateOne(ArrowSchema *schema,
						   int rb_index,
						   ArrowBlock *block,
						   ArrowRecordBatch *rbatch)
{
	setupRecordBatchContext con;
	RecordBatchState *rb_state;
	int		j, ncols = schema->_num_fields;

	if (rbatch->compression)
		elog(ERROR, "arrow_fdw: right now, compressed record-batche is not supported");

	rb_state = palloc0(offsetof(RecordBatchState, fields[ncols]));
	rb_state->rb_index = rb_index;
	rb_state->rb_offset = block->offset + block->metaDataLength;
	rb_state->rb_length = block->bodyLength;
	rb_state->rb_nitems = rbatch->length;

	memset(&con, 0, sizeof(setupRecordBatchContext));
	con.buffer_curr = rbatch->buffers;
	con.buffer_tail = rbatch->buffers + rbatch->_num_buffers;
	con.fnode_curr  = rbatch->nodes;
	con.fnode_tail  = rbatch->nodes + rbatch->_num_nodes;
	for (j=0; j < ncols; j++)
	{
		RecordBatchFieldState *fstate = &rb_state->fields[j];
		ArrowField	   *field = &schema->fields[j];

		__buildRecordBatchFieldState(&con, fstate, field, 0);
	}
	if (con.buffer_curr != con.buffer_tail ||
		con.fnode_curr  != con.fnode_tail)
		elog(ERROR, "arrow_fdw: RecordBatch may be corrupted");
	return rb_state;
}

static ArrowFileState *
__buildArrowFileStateByFile(const char *filename)
{
	ArrowFileInfo af_info;
	ArrowFileState *af_state;
	arrowStatsBinary *arrow_bstats;
	File		filp;

	filp = PathNameOpenFile(filename, O_RDONLY | PG_BINARY);
	if (filp < 0)
	{
		if (errno != ENOENT)
			elog(ERROR, "failed to open('%s'): %m", filename);
		elog(DEBUG2, "failed to open('%s'): %m", filename);
		return NULL;
	}

	/* read the metadata */
	readArrowFileDesc(FileGetRawDesc(filp), &af_info);
	if (af_info.dictionaries != NULL)
		elog(ERROR, "DictionaryBatch is not supported at '%s'", filename);
	Assert(af_info.footer._num_dictionaries == 0);
	FileClose(filp);

	if (af_info.recordBatches == NULL)
	{
		elog(DEBUG2, "arrow file '%s' contains no RecordBatch", filename);
		return NULL;
	}
	/* allocate ArrowFileState */
	af_state = palloc0(sizeof(ArrowFileInfo));
	af_state->filename = pstrdup(filename);
	af_state->filp = -1;
	memcpy(&af_state->stat_buf, &af_info.stat_buf, sizeof(struct stat));

	arrow_bstats = buildArrowStatsBinary(&af_info.footer);
	for (int i=0; i < af_info.footer._num_recordBatches; i++)
	{
		ArrowBlock	     *block  = &af_info.footer.recordBatches[i];
		ArrowRecordBatch *rbatch = &af_info.recordBatches[i].body.recordBatch;
		RecordBatchState *rb_state;

		rb_state = __buildRecordBatchStateOne(&af_info.footer.schema,
											  i, block, rbatch);
		if (arrow_bstats)
			applyArrowStatsBinary(rb_state, arrow_bstats);
		af_state->rb_list = lappend(af_state->rb_list, rb_state);
	}
	releaseArrowStatsBinary(arrow_bstats);

	return af_state;
}


static arrowMetadataFieldCache *
__buildArrowMetadataFieldCache(RecordBatchFieldState *fstate)
{
	arrowMetadataFieldCache *fcache;

	fcache = __allocMetadataFieldCache();
	if (!fcache)
		return NULL;
	fcache->atttypid = fstate->atttypid;
	fcache->atttypmod = fstate->atttypmod;
	memcpy(&fcache->attopts, &fstate->attopts, sizeof(ArrowTypeOptions));
	fcache->nitems = fstate->nitems;
	fcache->null_count = fstate->null_count;
	fcache->nullmap_offset = fstate->nullmap_offset;
	fcache->nullmap_length = fstate->nullmap_length;
	fcache->values_offset = fstate->values_offset;
	fcache->values_length = fstate->values_length;
	fcache->extra_offset = fstate->extra_offset;
	fcache->extra_length = fstate->extra_length;
	memcpy(&fcache->stat_min, &fstate->stat_min, sizeof(SQLstat__datum));
	memcpy(&fcache->stat_max, &fstate->stat_max, sizeof(SQLstat__datum));
	fcache->stat_isnull = fstate->stat_isnull;
	fcache->num_children = fstate->num_children;
	dlist_init(&fcache->children);
	for (int j=0; j < fstate->num_children; j++)
	{
		arrowMetadataFieldCache *__fcache;

		__fcache = __buildArrowMetadataFieldCache(&fstate->children[j]);
		if (!__fcache)
		{
			__releaseMetadataFieldCache(fcache);
			return NULL;
		}
		dlist_push_tail(&fcache->children, &__fcache->chain);
	}
	return fcache;
}

/*
 * __buildArrowMetadataCacheNoLock
 *
 * it builds arrowMetadataCache entries according to the supplied
 * ArrowFileState
 */
static void
__buildArrowMetadataCacheNoLock(ArrowFileState *af_state)
{
	arrowMetadataCache *mcache_head = NULL;
	arrowMetadataCache *mcache_prev = NULL;
	arrowMetadataCache *mcache;
	uint32_t	hindex;
	ListCell   *lc;

	foreach (lc, af_state->rb_list)
	{
		RecordBatchState *rb_state = lfirst(lc);

		mcache = __allocMetadataCache();
		if (!mcache)
		{
			__releaseMetadataCache(mcache_head);
			return;
		}
		memcpy(&mcache->stat_buf,
			   &af_state->stat_buf, sizeof(struct stat));
		mcache->rb_index  = rb_state->rb_index;
		mcache->rb_offset = rb_state->rb_offset;
		mcache->rb_length = rb_state->rb_length;
		mcache->rb_nitems = rb_state->rb_nitems;
		mcache->nfields   = rb_state->nfields;
		dlist_init(&mcache->fields);
		if (!mcache_head)
			mcache_head = mcache;
		else
			mcache_prev->next = mcache;

		for (int j=0; j < rb_state->nfields; j++)
		{
			arrowMetadataFieldCache *fcache;

			fcache = __buildArrowMetadataFieldCache(&rb_state->fields[j]);
			if (!fcache)
			{
				__releaseMetadataCache(mcache_head);
				return;
			}
			dlist_push_tail(&mcache->fields, &fcache->chain);
		}
		mcache_prev = mcache;
	}
	/* chain to the list */
	hindex = arrowMetadataHashIndex(&af_state->stat_buf);
	dlist_push_tail(&arrow_metadata_cache->hash_slots[hindex],
					&mcache_head->chain );
	SpinLockAcquire(&arrow_metadata_cache->lru_lock);
	gettimeofday(&mcache_head->lru_tv, NULL);
	dlist_push_head(&arrow_metadata_cache->lru_list, &mcache_head->lru_chain);
	SpinLockRelease(&arrow_metadata_cache->lru_lock);
}

static ArrowFileState *
BuildArrowFileState(Relation frel, const char *filename)
{
	arrowMetadataCache *mcache;
	ArrowFileState *af_state;
	RecordBatchState *rb_state;
	struct stat		stat_buf;
	TupleDesc		tupdesc;

	if (stat(filename, &stat_buf) != 0)
		elog(ERROR, "failed on stat('%s'): %m", filename);
	LWLockAcquire(&arrow_metadata_cache->mutex, LW_SHARED);
	mcache = lookupArrowMetadataCache(&stat_buf, false);
	if (mcache)
	{
		/* found a valid metadata-cache */
		af_state = __buildArrowFileStateByCache(filename, mcache);
	}
	else
	{
		LWLockRelease(&arrow_metadata_cache->mutex);

		/* here is no valid metadata-cache, so build it from the raw file */
		af_state = __buildArrowFileStateByFile(filename);
		if (!af_state)
			return NULL;	/* file not found? */

		LWLockAcquire(&arrow_metadata_cache->mutex, LW_EXCLUSIVE);
		mcache = lookupArrowMetadataCache(&af_state->stat_buf, true);
		if (!mcache)
			__buildArrowMetadataCacheNoLock(af_state);
	}
	LWLockRelease(&arrow_metadata_cache->mutex);

	/* compatibility checks */
	rb_state = linitial(af_state->rb_list);
	tupdesc = RelationGetDescr(frel);
	if (tupdesc->natts != rb_state->nfields)
		elog(ERROR, "arrow_fdw: foreign table '%s' is not compatible to '%s'",
			 RelationGetRelationName(frel), filename);
	for (int j=0; j < tupdesc->natts; j++)
	{
		Form_pg_attribute	attr = TupleDescAttr(tupdesc, j);
		RecordBatchFieldState *fstate = &rb_state->fields[j];

		if (attr->atttypid != fstate->atttypid)
			elog(ERROR, "arrow_fdw: foreign table '%s' column '%s' (%s) is not compatible to the arrow field (%s) in the '%s'",
				 RelationGetRelationName(frel),
				 NameStr(attr->attname),
				 format_type_be(attr->atttypid),
				 format_type_be(fstate->atttypid),
				 filename);
	}
	/*
	 * Lookup Optimal GPU & DPU for the Arrow file
	 */
	//af_state->optimal_gpus = GpuOptimalGpusForFile();
	af_state->optimal_gpus = NULL;
	af_state->ds_entry = GetOptimalDpuForFile(filename);

	return af_state;
}

/*
 * baseRelIsArrowFdw
 */
bool
baseRelIsArrowFdw(RelOptInfo *baserel)
{
	if ((baserel->reloptkind == RELOPT_BASEREL ||
		 baserel->reloptkind == RELOPT_OTHER_MEMBER_REL) &&
		baserel->rtekind == RTE_RELATION &&
		OidIsValid(baserel->serverid) &&
		baserel->fdwroutine &&
		memcmp(baserel->fdwroutine,
			   &pgstrom_arrow_fdw_routine,
			   sizeof(FdwRoutine)) == 0)
		return true;

	return false;
}

/*
 * RelationIsArrowFdw
 */
bool
RelationIsArrowFdw(Relation frel)
{
    if (RelationGetForm(frel)->relkind == RELKIND_FOREIGN_TABLE)
    {
        FdwRoutine *routine = GetFdwRoutineForRelation(frel, false);

        if (memcmp(routine, &pgstrom_arrow_fdw_routine,
                   sizeof(FdwRoutine)) == 0)
            return true;
    }
    return false;
}

/*
 * arrowFdwExtractFilesList
 */
static List *
arrowFdwExtractFilesList(List *options_list,
						 int *p_parallel_nworkers)
{

	ListCell   *lc;
	List	   *filesList = NIL;
	char	   *dir_path = NULL;
	char	   *dir_suffix = NULL;
	int			parallel_nworkers = -1;

	foreach (lc, options_list)
	{
		DefElem	   *defel = lfirst(lc);

		Assert(IsA(defel->arg, String));
		if (strcmp(defel->defname, "file") == 0)
		{
			char   *temp = strVal(defel->arg);

			if (access(temp, R_OK) != 0)
				elog(ERROR, "arrow_fdw: unable to access '%s': %m", temp);
			filesList = lappend(filesList, makeString(pstrdup(temp)));
		}
		else if (strcmp(defel->defname, "files") == 0)
		{
			char   *temp = pstrdup(strVal(defel->arg));
			char   *saveptr;
			char   *tok;

			while ((tok = strtok_r(temp, ",", &saveptr)) != NULL)
			{
				tok = __trim(tok);

				if (access(tok, R_OK) != 0)
					elog(ERROR, "arrow_fdw: unable to access '%s': %m", tok);
				filesList = lappend(filesList, makeString(pstrdup(tok)));
			}
			pfree(temp);
		}
		else if (strcmp(defel->defname, "dir") == 0)
		{
			dir_path = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "suffix") == 0)
		{
			dir_suffix = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "parallel_workers") == 0)
		{
			if (parallel_nworkers >= 0)
				elog(ERROR, "'parallel_workers' appeared twice");
			parallel_nworkers = atoi(strVal(defel->arg));
		}
		else
			elog(ERROR, "arrow: unknown option (%s)", defel->defname);
	}
	if (dir_suffix && !dir_path)
		elog(ERROR, "arrow: cannot use 'suffix' option without 'dir'");

	if (dir_path)
	{
		struct dirent *dentry;
		DIR	   *dir;
		char   *temp;

		dir = AllocateDir(dir_path);
		while ((dentry = ReadDir(dir, dir_path)) != NULL)
		{
			if (strcmp(dentry->d_name, ".") == 0 ||
				strcmp(dentry->d_name, "..") == 0)
				continue;
			if (dir_suffix)
			{
				char   *pos = strrchr(dentry->d_name, '.');

				if (!pos || strcmp(pos+1, dir_suffix) != 0)
					continue;
			}
			temp = psprintf("%s/%s", dir_path, dentry->d_name);
			if (access(temp, R_OK) != 0)
			{
				elog(DEBUG1, "arrow_fdw: unable to read '%s', so skipped", temp);
				continue;
			}
			filesList = lappend(filesList, makeString(temp));
		}
		FreeDir(dir);
	}

	if (p_parallel_nworkers)
		*p_parallel_nworkers = parallel_nworkers;
	return filesList;
}

/*
 * ArrowGetForeignRelSize
 */
static size_t
__recordBatchFieldLength(RecordBatchFieldState *fstate)
{
	size_t		len = 0;

	if (fstate->null_count > 0)
		len += fstate->nullmap_length;
	len += (fstate->values_length +
			fstate->extra_length);
	for (int j=0; j < fstate->num_children; j++)
		len += __recordBatchFieldLength(&fstate->children[j]);
	return len;
}

static void
ArrowGetForeignRelSize(PlannerInfo *root,
					   RelOptInfo *baserel,
					   Oid foreigntableid)
{
	ForeignTable   *ft = GetForeignTable(foreigntableid);
	Relation		frel = table_open(foreigntableid, NoLock);
	List		   *filesList;
	List		   *results = NIL;
	Bitmapset	   *referenced = NULL;
	ListCell	   *lc1, *lc2;
	size_t			totalLen = 0;
	double			ntuples = 0.0;
	int				parallel_nworkers;

	/* columns to be referenced */
	foreach (lc1, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(lc1);

		pull_varattnos((Node *)rinfo->clause, baserel->relid, &referenced);
	}
	referenced = pickup_outer_referenced(root, baserel, referenced);

	/* read arrow-file metadta */
	filesList = arrowFdwExtractFilesList(ft->options, &parallel_nworkers);
	foreach (lc1, filesList)
	{
		ArrowFileState *af_state;
		char	   *fname = strVal(lfirst(lc1));

		af_state = BuildArrowFileState(frel, fname);
		if (!af_state)
			continue;

		/*
		 * Size calculation based the record-batch metadata
		 */
		foreach (lc2, af_state->rb_list)
		{
			RecordBatchState *rb_state = lfirst(lc2);

			/* whole-row reference? */
			if (bms_is_member(-FirstLowInvalidHeapAttributeNumber, referenced))
			{
				totalLen += rb_state->rb_length;
			}
			else
			{
				int		j, k;

				for (k = bms_next_member(referenced, -1);
					 k >= 0;
					 k = bms_next_member(referenced, k))
				{
					j = k + FirstLowInvalidHeapAttributeNumber;
					if (j <= 0 || j > rb_state->nfields)
						continue;
					totalLen += __recordBatchFieldLength(&rb_state->fields[j-1]);
				}
			}
			ntuples += rb_state->rb_nitems;
		}
		results = lappend(results, af_state);
	}
	table_close(frel, NoLock);

	/* setup baserel */
	baserel->rel_parallel_workers = parallel_nworkers;
	baserel->fdw_private = list_make2(results, referenced);
	baserel->pages = totalLen / BLCKSZ;
	baserel->tuples = ntuples;
	baserel->rows = ntuples *
		clauselist_selectivity(root,
							   baserel->baserestrictinfo,
							   0,
							   JOIN_INNER,
							   NULL);
}

/*
 * cost_arrow_fdw_seqscan
 */
static void
cost_arrow_fdw_seqscan(Path *path,
					   PlannerInfo *root,
					   RelOptInfo *baserel,
					   ParamPathInfo *param_info,
					   int num_workers)
{
	Cost		startup_cost = 0.0;
	Cost		disk_run_cost = 0.0;
	Cost		cpu_run_cost = 0.0;
	QualCost	qcost;
	double		nrows;
	double		spc_seq_page_cost;

	if (param_info)
		nrows = param_info->ppi_rows;
	else
		nrows = baserel->rows;

	/* arrow_fdw.enabled */
	if (!arrow_fdw_enabled)
		startup_cost += disable_cost;

	/*
	 * Storage costs
	 *
	 * XXX - smaller number of columns to read shall have less disk cost
	 * because of columnar format. Right now, we don't discount cost for
	 * the pages not to be read.
	 */
	get_tablespace_page_costs(baserel->reltablespace,
							  NULL,
							  &spc_seq_page_cost);
	disk_run_cost = spc_seq_page_cost * baserel->pages;

	/* CPU costs */
	if (param_info)
	{
		cost_qual_eval(&qcost, param_info->ppi_clauses, root);
		qcost.startup += baserel->baserestrictcost.startup;
        qcost.per_tuple += baserel->baserestrictcost.per_tuple;
	}
	else
		qcost = baserel->baserestrictcost;
	startup_cost += qcost.startup;
	cpu_run_cost = (cpu_tuple_cost + qcost.per_tuple) * baserel->tuples;

	/* tlist evaluation costs */
	startup_cost += path->pathtarget->cost.startup;
	cpu_run_cost += path->pathtarget->cost.per_tuple * path->rows;

	/* adjust cost for CPU parallelism */
	if (num_workers > 0)
	{
		double		leader_contribution;
		double		parallel_divisor = (double) num_workers;

		/* see get_parallel_divisor() */
		leader_contribution = 1.0 - (0.3 * (double)num_workers);
		parallel_divisor += Max(leader_contribution, 0.0);

		/* The CPU cost is divided among all the workers. */
		cpu_run_cost /= parallel_divisor;

		/* Estimated row count per background worker process */
		nrows = clamp_row_est(nrows / parallel_divisor);
	}
	path->rows = nrows;
	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + cpu_run_cost + disk_run_cost;
	path->parallel_workers = num_workers;
}

/*
 * ArrowGetForeignPaths
 */
static void
ArrowGetForeignPaths(PlannerInfo *root,
                     RelOptInfo *baserel,
                     Oid foreigntableid)
{
	ForeignPath	   *fpath;
	ParamPathInfo  *param_info;
	Relids			required_outer = baserel->lateral_relids;

	param_info = get_baserel_parampathinfo(root, baserel, required_outer);
	fpath = create_foreignscan_path(root,
									baserel,
									NULL,	/* default pathtarget */
									-1.0,	/* dummy */
									-1.0,	/* dummy */
									-1.0,	/* dummy */
									NIL,	/* no pathkeys */
									required_outer,
									NULL,	/* no extra plan */
									NIL);	/* no particular private */
	cost_arrow_fdw_seqscan(&fpath->path,
						   root,
						   baserel,
						   param_info, 0);
	add_path(baserel, &fpath->path);

	if (baserel->consider_parallel)
	{
		int		num_workers =
			compute_parallel_worker(baserel,
									baserel->pages, -1.0,
									max_parallel_workers_per_gather);
		if (num_workers == 0)
			return;

		fpath = create_foreignscan_path(root,
										baserel,
										NULL,	/* default pathtarget */
										-1.0,	/* dummy */
										-1.0,	/* dummy */
										-1.0,	/* dummy */
										NIL,	/* no pathkeys */
										required_outer,
										NULL,	/* no extra plan */
										NIL);	/* no particular private */
		fpath->path.parallel_aware = true;
		cost_arrow_fdw_seqscan(&fpath->path,
							   root,
							   baserel,
							   param_info,
							   num_workers);
		add_partial_path(baserel, (Path *)fpath);
	}
}

/*
 * ArrowGetForeignPlan
 */
static ForeignScan *
ArrowGetForeignPlan(PlannerInfo *root,
					RelOptInfo *baserel,
					Oid foreigntableid,
					ForeignPath *best_path,
					List *tlist,
					List *scan_clauses,
					Plan *outer_plan)
{
	Bitmapset  *referenced = lsecond(baserel->fdw_private);
	List	   *ref_list = NIL;
	int			j, k;

	for (k = bms_next_member(referenced, -1);
		 k >= 0;
		 k = bms_next_member(referenced, k))
	{
		j = k + FirstLowInvalidHeapAttributeNumber;
		ref_list = lappend_int(ref_list, j);
	}
	return make_foreignscan(tlist,
							extract_actual_clauses(scan_clauses, false),
							baserel->relid,
							NIL,	/* no expressions to evaluate */
							ref_list, /* list of referenced attnums */
							NIL,	/* no custom tlist */
							NIL,	/* no remote quals */
							outer_plan);
}

/*
 * ArrowBeginForeignScan
 */
static void
ArrowBeginForeignScan(ForeignScanState *node, int eflags)
{}

/*
 * ArrowIterateForeignScan
 */
static TupleTableSlot *
ArrowIterateForeignScan(ForeignScanState *node)
{
	return NULL;
}

/*
 * ArrowReScanForeignScan
 */
static void
ArrowReScanForeignScan(ForeignScanState *node)
{}

/*
 * ArrowEndForeignScan
 */
static void
ArrowEndForeignScan(ForeignScanState *node)
{}

/*
 * ArrowIsForeignScanParallelSafe
 */
static bool
ArrowIsForeignScanParallelSafe(PlannerInfo *root,
							   RelOptInfo *rel,
							   RangeTblEntry *rte)
{
	return true;
}

/*
 * ArrowEstimateDSMForeignScan
 */
static Size
ArrowEstimateDSMForeignScan(ForeignScanState *node,
							ParallelContext *pcxt)
{
	return 0;
}

/*
 * ArrowInitializeDSMForeignScan
 */
static void
ArrowInitializeDSMForeignScan(ForeignScanState *node,
                              ParallelContext *pcxt,
                              void *coordinate)
{}

/*
 * ArrowReInitializeDSMForeignScan
 */
static void
ArrowReInitializeDSMForeignScan(ForeignScanState *node,
                                ParallelContext *pcxt,
                                void *coordinate)
{
}

/*
 * ArrowInitializeWorkerForeignScan
 */
static void
ArrowInitializeWorkerForeignScan(ForeignScanState *node,
								 shm_toc *toc,
								 void *coordinate)
{}

/*
 * ArrowShutdownForeignScan
 */
static void
ArrowShutdownForeignScan(ForeignScanState *node)
{
}

/*
 * ArrowExplainForeignScan
 */
static void
ArrowExplainForeignScan(ForeignScanState *node, ExplainState *es)
{}

/*
 * ArrowAnalyzeForeignTable
 */
static bool
ArrowAnalyzeForeignTable(Relation frel,
                         AcquireSampleRowsFunc *p_sample_rows_func,
                         BlockNumber *p_totalpages)
{
	return false;
}







/*
 * ArrowImportForeignSchema
 */
static List *
ArrowImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	return NIL;
}





/*
 * pgstrom_request_arrow_fdw
 */
static void
pgstrom_request_arrow_fdw(void)
{
	size_t	sz;

	if (shmem_request_next)
		shmem_request_next();
	sz = TYPEALIGN(ARROW_METADATA_BLOCKSZ,
				   (size_t)arrow_metadata_cache_size_kb << 10);
	RequestAddinShmemSpace(MAXALIGN(sizeof(arrowMetadataCacheHead)) + sz);
}

/*
 * pgstrom_startup_arrow_fdw
 */
static void
pgstrom_startup_arrow_fdw(void)
{
	bool	found;
	size_t	sz;
	char   *buffer;
	int		i, n;

	if (shmem_startup_next)
		(*shmem_startup_next)();

	arrow_metadata_cache = ShmemInitStruct("arrowMetadataCache(head)",
										   MAXALIGN(sizeof(arrowMetadataCacheHead)),
										   &found);
	Assert(!found);
	
	LWLockInitialize(&arrow_metadata_cache->mutex, LWLockNewTrancheId());
	SpinLockInit(&arrow_metadata_cache->lru_lock);
	dlist_init(&arrow_metadata_cache->lru_list);
	dlist_init(&arrow_metadata_cache->free_blocks);
	dlist_init(&arrow_metadata_cache->free_mcaches);
	dlist_init(&arrow_metadata_cache->free_fcaches);
	for (i=0; i < ARROW_METADATA_HASH_NSLOTS; i++)
		dlist_init(&arrow_metadata_cache->hash_slots[i]);

	/* slab allocator */
	sz = TYPEALIGN(ARROW_METADATA_BLOCKSZ,
				   (size_t)arrow_metadata_cache_size_kb << 10);
	n = sz / ARROW_METADATA_BLOCKSZ;
	buffer = ShmemInitStruct("arrowMetadataCache(body)", sz, &found);
	Assert(!found);
	for (i=0; i < n; i++)
	{
		arrowMetadataCacheBlock *mc_block = (arrowMetadataCacheBlock *)buffer;

		memset(mc_block, 0, offsetof(arrowMetadataCacheBlock, data));
		dlist_push_tail(&arrow_metadata_cache->free_blocks, &mc_block->chain);

		buffer += ARROW_METADATA_BLOCKSZ;
	}
}

/*
 * pgstrom_init_arrow_fdw
 */
void
pgstrom_init_arrow_fdw(void)
{
	FdwRoutine *r = &pgstrom_arrow_fdw_routine;

	memset(r, 0, sizeof(FdwRoutine));
	NodeSetTag(r, T_FdwRoutine);
	/* SCAN support */
	r->GetForeignRelSize			= ArrowGetForeignRelSize;
	r->GetForeignPaths				= ArrowGetForeignPaths;
	r->GetForeignPlan				= ArrowGetForeignPlan;
	r->BeginForeignScan				= ArrowBeginForeignScan;
	r->IterateForeignScan			= ArrowIterateForeignScan;
	r->ReScanForeignScan			= ArrowReScanForeignScan;
	r->EndForeignScan				= ArrowEndForeignScan;
	/* EXPLAIN support */
	r->ExplainForeignScan			= ArrowExplainForeignScan;
	/* ANALYZE support */
	r->AnalyzeForeignTable			= ArrowAnalyzeForeignTable;
	/* CPU Parallel support */
	r->IsForeignScanParallelSafe	= ArrowIsForeignScanParallelSafe;
	r->EstimateDSMForeignScan		= ArrowEstimateDSMForeignScan;
	r->InitializeDSMForeignScan		= ArrowInitializeDSMForeignScan;
	r->ReInitializeDSMForeignScan	= ArrowReInitializeDSMForeignScan;
	r->InitializeWorkerForeignScan	= ArrowInitializeWorkerForeignScan;
	r->ShutdownForeignScan			= ArrowShutdownForeignScan;
	/* IMPORT FOREIGN SCHEMA support */
	r->ImportForeignSchema			= ArrowImportForeignSchema;

	/*
	 * Turn on/off arrow_fdw
	 */
	DefineCustomBoolVariable("arrow_fdw.enabled",
							 "Enables the planner's use of Arrow_Fdw",
							 NULL,
							 &arrow_fdw_enabled,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/*
	 * Turn on/off min/max statistics hint
	 */
	DefineCustomBoolVariable("arrow_fdw.stats_hint_enabled",
							 "Enables min/max statistics hint, if any",
							 NULL,
							 &arrow_fdw_stats_hint_enabled,
							 true,
							 PGC_USERSET,
                             GUC_NOT_IN_SAMPLE,
                             NULL, NULL, NULL);
	/*
	 * Configurations for arrow_fdw metadata cache
	 */
	DefineCustomIntVariable("arrow_fdw.metadata_cache_size",
							"size of shared metadata cache for arrow files",
							NULL,
							&arrow_metadata_cache_size_kb,
							512 * 1024,		/* 512MB */
							32 * 1024,		/* 32MB */
							INT_MAX,
							PGC_POSTMASTER,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_KB,
							NULL, NULL, NULL);
	/* shared memory size */
	shmem_request_next = shmem_request_hook;
	shmem_request_hook = pgstrom_request_arrow_fdw;
	shmem_startup_next = shmem_startup_hook;
	shmem_startup_hook = pgstrom_startup_arrow_fdw;
}

































































#if 0
/* setup of MetadataCacheKey */
static inline int
initMetadataCacheKey(MetadataCacheKey *mkey, struct stat *stat_buf)
{
	memset(mkey, 0, sizeof(MetadataCacheKey));
	mkey->st_dev	= stat_buf->st_dev;
	mkey->st_ino	= stat_buf->st_ino;
	mkey->hash		= hash_any((unsigned char *)mkey,
							   offsetof(MetadataCacheKey, hash));
	return mkey->hash % ARROW_METADATA_HASH_NSLOTS;
}

/*
 * executor hint by min/max statistics per record batch
 */
typedef struct
{
	List		   *orig_quals;
	List		   *eval_quals;
	ExprState	   *eval_state;
	Bitmapset	   *stat_attrs;
	Bitmapset	   *load_attrs;
	ExprContext	   *econtext;
} arrowStatsHint;

/*
 * ArrowFdwState
 */
struct ArrowFdwState
{
//	GpuContext *gcontext;			/* valid if owned by GpuXXX plan */
	void	   *gcontext;  //to be removed
	List	   *gpuDirectFileDescList;	/* list of GPUDirectFileDesc */
	List	   *fdescList;				/* list of File (buffered i/o) */
	Bitmapset  *referenced;
	arrowStatsHint *stats_hint;
	pg_atomic_uint32   *rbatch_index;
	pg_atomic_uint32	__rbatch_index_local;	/* if single process */
	pg_atomic_uint32   *rbatch_nload;
	pg_atomic_uint32	__rbatch_nload_local;	/* if single process */
	pg_atomic_uint32   *rbatch_nskip;
	pg_atomic_uint32	__rbatch_nskip_local;	/* if single process */
	pgstrom_data_store *curr_pds;	/* current focused buffer */
	cl_ulong	curr_index;			/* current index to row on KDS */
	/* state of RecordBatches */
	uint32		num_rbatches;
	RecordBatchState *rbatches[FLEXIBLE_ARRAY_MEMBER];
};

/* ---------- static variables ---------- */
static FdwRoutine		pgstrom_arrow_fdw_routine;
static shmem_request_hook_type shmem_request_next = NULL;
static shmem_startup_hook_type shmem_startup_next = NULL;
static arrowMetadataState *arrow_metadata_state = NULL;
static bool				arrow_fdw_enabled;				/* GUC */
static bool				arrow_fdw_stats_hint_enabled;	/* GUC */
static int				arrow_metadata_cache_size_kb;	/* GUC */
static size_t			arrow_metadata_cache_size;

/* ---------- static functions ---------- */
static Oid		arrowTypeToPGTypeOid(ArrowField *field, int *typmod);
static const char *arrowTypeToPGTypeName(ArrowField *field);
static bool		arrowSchemaCompatibilityCheck(TupleDesc tupdesc,
											  RecordBatchState *rb_state);
static List	   *arrowLookupOrBuildMetadataCache(File fdesc, Bitmapset **p_stat_attrs);
static void		pg_datum_arrow_ref(kern_data_store *kds,
								   kern_colmeta *cmeta,
								   size_t index,
								   Datum *p_datum,
								   bool *p_isnull);

Datum	pgstrom_arrow_fdw_handler(PG_FUNCTION_ARGS);
Datum	pgstrom_arrow_fdw_validator(PG_FUNCTION_ARGS);
Datum	pgstrom_arrow_fdw_precheck_schema(PG_FUNCTION_ARGS);
Datum	pgstrom_arrow_fdw_truncate(PG_FUNCTION_ARGS);
Datum	pgstrom_arrow_fdw_import_file(PG_FUNCTION_ARGS);

/*
 * timespec_comp - compare timespec values
 */
static inline int
timespec_comp(struct timespec *tv1, struct timespec *tv2)
{
	if (tv1->tv_sec < tv2->tv_sec)
		return -1;
	if (tv1->tv_sec > tv2->tv_sec)
		return 1;
	if (tv1->tv_nsec < tv2->tv_nsec)
		return -1;
	if (tv1->tv_nsec > tv2->tv_nsec)
		return 1;
	return 0;
}

/*
 * baseRelIsArrowFdw
 */
bool
baseRelIsArrowFdw(RelOptInfo *baserel)
{
	if ((baserel->reloptkind == RELOPT_BASEREL ||
		 baserel->reloptkind == RELOPT_OTHER_MEMBER_REL) &&
		baserel->rtekind == RTE_RELATION &&
		OidIsValid(baserel->serverid) &&
		baserel->fdwroutine &&
		memcmp(baserel->fdwroutine,
			   &pgstrom_arrow_fdw_routine,
			   sizeof(FdwRoutine)) == 0)
		return true;

	return false;
}

/*
 * RelationIsArrowFdw
 */
bool
RelationIsArrowFdw(Relation frel)
{
	if (RelationGetForm(frel)->relkind == RELKIND_FOREIGN_TABLE)
	{
		FdwRoutine *routine = GetFdwRoutineForRelation(frel, false);

		if (memcmp(routine, &pgstrom_arrow_fdw_routine,
				   sizeof(FdwRoutine)) == 0)
			return true;
	}
	return false;
}

/*
 * RecordBatchFieldCount
 */
static int
__RecordBatchFieldCount(RecordBatchFieldState *fstate)
{
	int		j, count = 1;

	for (j=0; j < fstate->num_children; j++)
		count += __RecordBatchFieldCount(&fstate->children[j]);

	return count;
}

static int
RecordBatchFieldCount(RecordBatchState *rbstate)
{
	int		j, count = 0;

	for (j=0; j < rbstate->ncols; j++)
		count += __RecordBatchFieldCount(&rbstate->columns[j]);

	return count;
}

/*
 * RecordBatchFieldLength
 */
static size_t
RecordBatchFieldLength(RecordBatchFieldState *fstate)
{
	size_t	len;
	int		j;

	len = BLCKALIGN(fstate->nullmap_length +
					fstate->values_length +
					fstate->extra_length);
	for (j=0; j < fstate->num_children; j++)
		len += RecordBatchFieldLength(&fstate->children[j]);
	return len;
}

/*
 * ArrowGetForeignRelSize
 */
static void
ArrowGetForeignRelSize(PlannerInfo *root,
					   RelOptInfo *baserel,
					   Oid foreigntableid)
{
	ForeignTable   *ft = GetForeignTable(foreigntableid);
	List		   *filesList;
	Size			filesSizeTotal = 0;
	Bitmapset	   *referenced = NULL;
	BlockNumber		npages = 0;
	double			ntuples = 0.0;
	ListCell	   *lc;
	int				parallel_nworkers;
	bool			writable;
	Bitmapset	   *optimal_gpus = (void *)(~0UL);
	int				j, k;

	/* columns to be fetched */
	foreach (lc, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(lc);

		pull_varattnos((Node *)rinfo->clause, baserel->relid, &referenced);
	}
	referenced = pgstrom_pullup_outer_refs(root, baserel, referenced);

	filesList = __arrowFdwExtractFilesList(ft->options,
										   &parallel_nworkers,
										   &writable);
	foreach (lc, filesList)
	{
		char	   *fname = strVal(lfirst(lc));
		File		fdesc;
		List	   *rb_cached;
		ListCell   *cell;
		Bitmapset  *__gpus;
		size_t		len = 0;

		fdesc = PathNameOpenFile(fname, O_RDONLY | PG_BINARY);
		if (fdesc < 0)
		{
			if (writable && errno == ENOENT)
				continue;
			elog(ERROR, "failed to open file '%s' on behalf of '%s'",
				 fname, get_rel_name(foreigntableid));
		}
		/* lookup optimal GPUs */
		__gpus = extraSysfsLookupOptimalGpus(fdesc);
		if (optimal_gpus == (void *)(~0UL))
			optimal_gpus = __gpus;
		else
			optimal_gpus = bms_intersect(optimal_gpus, __gpus);
		/* lookup or build metadata cache */
		rb_cached = arrowLookupOrBuildMetadataCache(fdesc, NULL);
		foreach (cell, rb_cached)
		{
			RecordBatchState   *rb_state = lfirst(cell);

			if (cell == list_head(rb_cached))
				filesSizeTotal += BLCKALIGN(rb_state->stat_buf.st_size);

			if (bms_is_member(-FirstLowInvalidHeapAttributeNumber, referenced))
			{
				for (j=0; j < rb_state->ncols; j++)
					len += RecordBatchFieldLength(&rb_state->fields[j]);
			}
			else
			{
				for (k = bms_next_member(referenced, -1);
					 k >= 0;
					 k = bms_next_member(referenced, k))
				{
					j = k + FirstLowInvalidHeapAttributeNumber;
					if (j < 0 || j >= rb_state->ncols)
						continue;
					len += RecordBatchFieldLength(&rb_state->fields[j]);
				}
			}
			ntuples += rb_state->rb_nitems;
		}
		npages = len / BLCKSZ;
		FileClose(fdesc);
	}
	bms_free(referenced);

	if (optimal_gpus == (void *)(~0UL) ||
		filesSizeTotal < pgstrom_gpudirect_threshold())
		optimal_gpus = NULL;

	baserel->rel_parallel_workers = parallel_nworkers;
	baserel->fdw_private = list_make1(optimal_gpus);
	baserel->pages = npages;
	baserel->tuples = ntuples;
	baserel->rows = ntuples *
		clauselist_selectivity(root,
							   baserel->baserestrictinfo,
							   0,
							   JOIN_INNER,
							   NULL);
}

/*
 * GetOptimalGpusForArrowFdw
 *
 * optimal GPUs bitmap is saved at baserel->fdw_private
 */
Bitmapset *
GetOptimalGpusForArrowFdw(PlannerInfo *root, RelOptInfo *baserel)
{
	if (baserel->fdw_private == NIL)
	{
		RangeTblEntry *rte = root->simple_rte_array[baserel->relid];

		ArrowGetForeignRelSize(root, baserel, rte->relid);
	}
	return linitial(baserel->fdw_private);
}

/* ----------------------------------------------------------------
 *
 * Routines related to min/max statistics and scan hint
 *
 * If mapped Apache Arrow files have custome-metadata of "min_values" and
 * "max_values" at the Field, arrow_fdw deals with this comma separated
 * integer values as min/max value for each field, if any.
 * Once we can know min/max value of the field, we can skip record batches
 * that shall not match with WHERE-clause.
 *
 * This min/max array is expected to have as many integer elements or nulls
 * as there are record-batches.
 * ----------------------------------------------------------------
 */

/*
 * buildArrowStatsBinary
 *
 * It reconstruct binary min/max statistics per record-batch
 * from the custom-metadata of ArrowField.
 */

/*
 * applyArrowStatsBinary
 *
 * It applies the fetched min/max values on the cached record-batch metadata
 */
static SQLstat *
__buildArrowFieldStatsList(ArrowField *field, uint32 numRecordBatches)
{
	const char *min_tokens = NULL;
	const char *max_tokens = NULL;
	char	   *min_buffer;
	char	   *max_buffer;
	char	   *tok1, *pos1;
	char	   *tok2, *pos2;
	SQLstat	   *results = NULL;
	int			k, index;

	for (k=0; k < field->_num_custom_metadata; k++)
	{
		ArrowKeyValue *kv = &field->custom_metadata[k];

		if (strcmp(kv->key, "min_values") == 0)
			min_tokens = kv->value;
		else if (strcmp(kv->key, "max_values") == 0)
			max_tokens = kv->value;
	}
	if (!min_tokens || !max_tokens)
		return NULL;
	min_buffer = alloca(strlen(min_tokens) + 1);
	max_buffer = alloca(strlen(max_tokens) + 1);
	strcpy(min_buffer, min_tokens);
	strcpy(max_buffer, max_tokens);

	for (tok1 = strtok_r(min_buffer, ",", &pos1),
		 tok2 = strtok_r(max_buffer, ",", &pos2), index = 0;
		 tok1 && tok2;
		 tok1 = strtok_r(NULL, ",", &pos1),
		 tok2 = strtok_r(NULL, ",", &pos2), index++)
	{
		bool		__isnull = false;
		int128_t	__min = __atoi128(__trim(tok1), &__isnull);
		int128_t	__max = __atoi128(__trim(tok2), &__isnull);

		if (!__isnull)
		{
			SQLstat *item = palloc0(sizeof(SQLstat));

			item->next = results;
			item->rb_index = index;
			item->is_valid = true;
			item->min.i128 = __min;
			item->max.i128 = __max;
			results = item;
		}
	}
	/* sanity checks */
	if (!tok1 && !tok2 && index == numRecordBatches)
		return results;
	/* ah, error... */
	while (results)
	{
		SQLstat *next = results->next;

		pfree(results);
		results = next;
	}
	return NULL;
}

/*
 * execInitArrowStatsHint / execCheckArrowStatsHint / execEndArrowStatsHint
 *
 * ... are executor routines for min/max statistics.
 */
static bool
__buildArrowStatsOper(arrowStatsHint *arange,
					  ScanState *ss,
					  OpExpr *op,
					  bool reverse)
{
	Index		scanrelid = ((Scan *)ss->ps.plan)->scanrelid;
	Oid			opcode;
	Var		   *var;
	Node	   *arg;
	Expr	   *expr;
	Oid			opfamily = InvalidOid;
	StrategyNumber strategy = InvalidStrategy;
	CatCList   *catlist;
	int			i;

	if (!reverse)
	{
		opcode = op->opno;
		var = linitial(op->args);
		arg = lsecond(op->args);
	}
	else
	{
		opcode = get_commutator(op->opno);
		var = lsecond(op->args);
		arg = linitial(op->args);
	}
	/* Is it VAR <OPER> ARG form? */
	if (!IsA(var, Var) || var->varno != scanrelid)
		return false;
	if (!bms_is_member(var->varattno, arange->stat_attrs))
		return false;
	if (contain_var_clause(arg) ||
		contain_volatile_functions(arg))
		return false;

	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opcode));
	for (i=0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop amop = (Form_pg_amop) GETSTRUCT(tuple);

		if (amop->amopmethod == BRIN_AM_OID)
		{
			opfamily = amop->amopfamily;
			strategy = amop->amopstrategy;
			break;
		}
	}
	ReleaseSysCacheList(catlist);

	if (strategy == BTLessStrategyNumber ||
		strategy == BTLessEqualStrategyNumber)
	{
		/* (VAR < ARG) --> (Min < ARG) */
		/* (VAR <= ARG) --> (Min <= ARG) */
		arange->load_attrs = bms_add_member(arange->load_attrs,
											var->varattno);
		expr = make_opclause(opcode,
							 op->opresulttype,
							 op->opretset,
							 (Expr *)makeVar(INNER_VAR,
											 var->varattno,
											 var->vartype,
											 var->vartypmod,
											 var->varcollid,
											 0),
							 (Expr *)copyObject(arg),
							 op->opcollid,
							 op->inputcollid);
		set_opfuncid((OpExpr *)expr);
		arange->eval_quals = lappend(arange->eval_quals, expr);
	}
	else if (strategy == BTGreaterEqualStrategyNumber ||
			 strategy == BTGreaterStrategyNumber)
	{
		/* (VAR >= ARG) --> (Max >= ARG) */
		/* (VAR > ARG) --> (Max > ARG) */
		arange->load_attrs = bms_add_member(arange->load_attrs,
											var->varattno);
		expr = make_opclause(opcode,
							 op->opresulttype,
							 op->opretset,
							 (Expr *)makeVar(OUTER_VAR,
											 var->varattno,
											 var->vartype,
											 var->vartypmod,
											 var->varcollid,
											 0),
							 (Expr *)copyObject(arg),
							 op->opcollid,
							 op->inputcollid);
		set_opfuncid((OpExpr *)expr);
		arange->eval_quals = lappend(arange->eval_quals, expr);
	}
	else if (strategy == BTEqualStrategyNumber)
	{
		/* (VAR = ARG) --> (Max >= ARG && Min <= ARG) */
		opcode = get_opfamily_member(opfamily, var->vartype,
									 exprType((Node *)arg),
									 BTGreaterEqualStrategyNumber);
		expr = make_opclause(opcode,
							 op->opresulttype,
							 op->opretset,
							 (Expr *)makeVar(OUTER_VAR,
											 var->varattno,
											 var->vartype,
											 var->vartypmod,
											 var->varcollid,
											 0),
							 (Expr *)copyObject(arg),
							 op->opcollid,
							 op->inputcollid);
		set_opfuncid((OpExpr *)expr);
		arange->eval_quals = lappend(arange->eval_quals, expr);

		opcode = get_opfamily_member(opfamily, var->vartype,
									 exprType((Node *)arg),
									 BTLessEqualStrategyNumber);
		expr = make_opclause(opcode,
							 op->opresulttype,
							 op->opretset,
							 (Expr *)makeVar(INNER_VAR,
											 var->varattno,
											 var->vartype,
											 var->vartypmod,
											 var->varcollid,
											 0),
							 (Expr *)copyObject(arg),
							 op->opcollid,
							 op->inputcollid);
		set_opfuncid((OpExpr *)expr);
		arange->eval_quals = lappend(arange->eval_quals, expr);
	}
	else
	{
		return false;
	}
	arange->load_attrs = bms_add_member(arange->load_attrs,
										var->varattno);
	return true;
}

static arrowStatsHint *
execInitArrowStatsHint(ScanState *ss,
					   Bitmapset *stat_attrs,
					   List *outer_quals)
{
	Relation		relation = ss->ss_currentRelation;
	TupleDesc		tupdesc = RelationGetDescr(relation);
	ExprContext	   *econtext;
	arrowStatsHint *result, temp;
	Expr		   *eval_expr;
	ListCell	   *lc;

	memset(&temp, 0, sizeof(arrowStatsHint));
	temp.stat_attrs = stat_attrs;
	foreach (lc, outer_quals)
	{
		OpExpr *op = lfirst(lc);

		if (IsA(op, OpExpr) && list_length(op->args) == 2 &&
			(__buildArrowStatsOper(&temp, ss, op, false) ||
			 __buildArrowStatsOper(&temp, ss, op, true)))
		{
			temp.orig_quals = lappend(temp.orig_quals, copyObject(op));
		}
	}
	if (!temp.orig_quals)
		return NULL;

	Assert(list_length(temp.eval_quals) > 0);
	if (list_length(temp.eval_quals) == 1)
		eval_expr = linitial(temp.eval_quals);
	else
		eval_expr = make_andclause(temp.eval_quals);

	econtext = CreateExprContext(ss->ps.state);
	econtext->ecxt_innertuple = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	econtext->ecxt_outertuple = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

	result = palloc0(sizeof(arrowStatsHint));
	result->orig_quals = temp.orig_quals;
	result->eval_quals = temp.eval_quals;
	result->eval_state = ExecInitExpr(eval_expr, &ss->ps);
	result->stat_attrs = bms_copy(stat_attrs);
	result->load_attrs = temp.load_attrs;
	result->econtext   = econtext;

	return result;
}

static bool
__fetchArrowStatsDatum(RecordBatchFieldState *fstate,
					   SQLstat__datum *sval,
					   Datum *p_datum, bool *p_isnull)
{
	Datum		datum;
	int64		shift;

	switch (fstate->atttypid)
	{
		case INT1OID:
			datum = Int8GetDatum(sval->i8);
			break;
		case INT2OID:
		case FLOAT2OID:
			datum = Int16GetDatum(sval->i16);
			break;
		case INT4OID:
		case FLOAT4OID:
			datum = Int32GetDatum(sval->i32);
			break;
		case INT8OID:
		case FLOAT8OID:
			datum = Int64GetDatum(sval->i64);
			break;
		case NUMERICOID:
			{
				Int128_t	decimal;
				int			dscale = fstate->attopts.decimal.scale;
				char	   *result = palloc0(sizeof(struct NumericData));

				decimal.ival = sval->i128;
				while (dscale > 0 && decimal.ival % 10 == 0)
				{
					decimal.ival /= 10;
					dscale--;
				}
				pg_numeric_to_varlena(result, dscale, decimal);

				datum = PointerGetDatum(result);
			}
			break;
		case DATEOID:
			shift = POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE;
			switch (fstate->attopts.date.unit)
			{
				case ArrowDateUnit__Day:
					datum = DateADTGetDatum((DateADT)sval->i32 - shift);
					break;
				case ArrowDateUnit__MilliSecond:
					datum = DateADTGetDatum((DateADT)sval->i64 / 1000L - shift);
					break;
				default:
					return false;
			}
			break;

		case TIMEOID:
			switch (fstate->attopts.time.unit)
			{
				case ArrowTimeUnit__Second:
					datum = TimeADTGetDatum((TimeADT)sval->u32 * 1000000L);
					break;
				case ArrowTimeUnit__MilliSecond:
					datum = TimeADTGetDatum((TimeADT)sval->u32 * 1000L);
					break;
				case ArrowTimeUnit__MicroSecond:
					datum = TimeADTGetDatum((TimeADT)sval->u64);
					break;
				case ArrowTimeUnit__NanoSecond:
					datum = TimeADTGetDatum((TimeADT)sval->u64 / 1000L);
					break;
				default:
					return false;
			}
			break;
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			shift = (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * USECS_PER_DAY;
			switch (fstate->attopts.timestamp.unit)
			{
				case ArrowTimeUnit__Second:
					datum = TimestampGetDatum((Timestamp)sval->i64 * 1000000L - shift);
					break;
				case ArrowTimeUnit__MilliSecond:
					datum = TimestampGetDatum((Timestamp)sval->i64 * 1000L - shift);
					break;
				case ArrowTimeUnit__MicroSecond:
					datum = TimestampGetDatum((Timestamp)sval->i64 - shift);
					break;
				case ArrowTimeUnit__NanoSecond:
					datum = TimestampGetDatum((Timestamp)sval->i64 / 1000L - shift);
					break;
				default:
					return false;
			}
			break;
		default:
			return false;
	}
	*p_datum = datum;
	*p_isnull = false;
	return true;
}

static bool
execCheckArrowStatsHint(arrowStatsHint *stats_hint,
						RecordBatchState *rb_state)
{
	ExprContext	   *econtext = stats_hint->econtext;
	TupleTableSlot *min_values = econtext->ecxt_innertuple;
	TupleTableSlot *max_values = econtext->ecxt_outertuple;
	int				anum;
	Datum			datum;
	bool			isnull;

	/* load the min/max statistics */
	ExecStoreAllNullTuple(min_values);
	ExecStoreAllNullTuple(max_values);
	for (anum = bms_next_member(stats_hint->load_attrs, -1);
		 anum >= 0;
		 anum = bms_next_member(stats_hint->load_attrs, anum))
	{
		RecordBatchFieldState *fstate = &rb_state->columns[anum-1];

		Assert(anum > 0 && anum <= rb_state->ncols);
		/*
		 * In case when min/max statistics are missing, we cannot determine
		 * whether we can skip the current record-batch.
		 */
		if (fstate->stat_isnull)
			return false;

		if (!__fetchArrowStatsDatum(fstate, &fstate->stat_min,
									&min_values->tts_values[anum-1],
									&min_values->tts_isnull[anum-1]))
			return false;

		if (!__fetchArrowStatsDatum(fstate, &fstate->stat_max,
									&max_values->tts_values[anum-1],
									&max_values->tts_isnull[anum-1]))
			return false;
	}
	datum = ExecEvalExprSwitchContext(stats_hint->eval_state, econtext, &isnull);

//	elog(INFO, "file [%s] rb_index=%u datum=%lu isnull=%d",
//		 FilePathName(rb_state->fdesc), rb_state->rb_index, datum, (int)isnull);
	if (!isnull && DatumGetBool(datum))
		return true;
	return false;
}

static void
execEndArrowStatsHint(arrowStatsHint *stats_hint)
{
	ExprContext	   *econtext = stats_hint->econtext;

	ExecDropSingleTupleTableSlot(econtext->ecxt_innertuple);
	ExecDropSingleTupleTableSlot(econtext->ecxt_outertuple);
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;

	FreeExprContext(econtext, true);
}

/*
 * Routines to setup record-batches
 */
typedef struct
{
	ArrowBuffer    *buffer_curr;
	ArrowBuffer    *buffer_tail;
	ArrowFieldNode *fnode_curr;
	ArrowFieldNode *fnode_tail;
} setupRecordBatchContext;

static void
assignArrowTypeOptions(ArrowTypeOptions *attopts, const ArrowType *atype)
{
	memset(attopts, 0, sizeof(ArrowTypeOptions));
	switch (atype->node.tag)
	{
		case ArrowNodeTag__Decimal:
			if (atype->Decimal.precision < SHRT_MIN ||
				atype->Decimal.precision > SHRT_MAX)
				elog(ERROR, "Decimal precision is out of range");
			if (atype->Decimal.scale < SHRT_MIN ||
				atype->Decimal.scale > SHRT_MAX)
				elog(ERROR, "Decimal scale is out of range");
			attopts->decimal.precision = atype->Decimal.precision;
			attopts->decimal.scale     = atype->Decimal.scale;
			break;
		case ArrowNodeTag__Date:
			if (atype->Date.unit == ArrowDateUnit__Day ||
				atype->Date.unit == ArrowDateUnit__MilliSecond)
				attopts->date.unit = atype->Date.unit;
			else
				elog(ERROR, "unknown unit of Date");
			break;
		case ArrowNodeTag__Time:
			if (atype->Time.unit == ArrowTimeUnit__Second ||
				atype->Time.unit == ArrowTimeUnit__MilliSecond ||
				atype->Time.unit == ArrowTimeUnit__MicroSecond ||
				atype->Time.unit == ArrowTimeUnit__NanoSecond)
				attopts->time.unit = atype->Time.unit;
			else
				elog(ERROR, "unknown unit of Time");
			break;
		case ArrowNodeTag__Timestamp:
			if (atype->Timestamp.unit == ArrowTimeUnit__Second ||
				atype->Timestamp.unit == ArrowTimeUnit__MilliSecond ||
				atype->Timestamp.unit == ArrowTimeUnit__MicroSecond ||
				atype->Timestamp.unit == ArrowTimeUnit__NanoSecond)
				attopts->timestamp.unit = atype->Timestamp.unit;
			else
				elog(ERROR, "unknown unit of Timestamp");
			break;
		case ArrowNodeTag__Interval:
			if (atype->Interval.unit == ArrowIntervalUnit__Year_Month ||
				atype->Interval.unit == ArrowIntervalUnit__Day_Time)
				attopts->interval.unit = atype->Interval.unit;
			else
				elog(ERROR, "unknown unit of Interval");
			break;
		case ArrowNodeTag__FixedSizeBinary:
			attopts->fixed_size_binary.byteWidth = atype->FixedSizeBinary.byteWidth;
			break;
		default:
			/* no extra attributes */
			break;
	}
}

/*
 * arrowFieldLength
 */
static size_t
arrowFieldLength(ArrowField *field, int64 nitems)
{
	ArrowType  *type = &field->type;
	size_t		length = 0;

	switch (type->node.tag)
	{
		case ArrowNodeTag__Int:
			switch (type->Int.bitWidth)
			{
				case 8:
					length = nitems;
					break;
				case 16:
					length = 2 * nitems;
					break;
				case 32:
					length = 4 * nitems;
					break;
				case 64:
					length = 8 * nitems;
					break;
				default:
					elog(ERROR, "Not a supported Int width: %d",
						 type->Int.bitWidth);
			}
			break;
		case ArrowNodeTag__FloatingPoint:
			switch (type->FloatingPoint.precision)
			{
				case ArrowPrecision__Half:
					length = sizeof(cl_short) * nitems;
					break;
				case ArrowPrecision__Single:
					length = sizeof(cl_float) * nitems;
					break;
				case ArrowPrecision__Double:
					length = sizeof(cl_double) * nitems;
					break;
				default:
					elog(ERROR, "Not a supported FloatingPoint precision");
			}
			break;
		case ArrowNodeTag__Utf8:
		case ArrowNodeTag__Binary:
		case ArrowNodeTag__List:
			length = sizeof(cl_uint) * (nitems + 1);
			break;
		case ArrowNodeTag__Bool:
			length = BITMAPLEN(nitems);
			break;
		case ArrowNodeTag__Decimal:
			length = sizeof(int128) * nitems;
			break;
		case ArrowNodeTag__Date:
			switch (type->Date.unit)
			{
				case ArrowDateUnit__Day:
					length = sizeof(cl_int) * nitems;
					break;
				case ArrowDateUnit__MilliSecond:
					length = sizeof(cl_long) * nitems;
					break;
				default:
					elog(ERROR, "Not a supported Date unit");
			}
			break;
		case ArrowNodeTag__Time:
			switch (type->Time.unit)
			{
				case ArrowTimeUnit__Second:
				case ArrowTimeUnit__MilliSecond:
					length = sizeof(cl_int) * nitems;
					break;
				case ArrowTimeUnit__MicroSecond:
				case ArrowTimeUnit__NanoSecond:
					length = sizeof(cl_long) * nitems;
					break;
				default:
					elog(ERROR, "Not a supported Time unit");
			}
			break;
		case ArrowNodeTag__Timestamp:
			length = sizeof(cl_long) * nitems;
			break;
		case ArrowNodeTag__Interval:
			switch (type->Interval.unit)
			{
				case ArrowIntervalUnit__Year_Month:
					length = sizeof(cl_uint) * nitems;
					break;
				case ArrowIntervalUnit__Day_Time:
					length = sizeof(cl_long) * nitems;
					break;
				default:
					elog(ERROR, "Not a supported Interval unit");
			}
			break;
		case ArrowNodeTag__Struct:	//to be supported later
			length = 0;		/* only nullmap */
			break;
		case ArrowNodeTag__FixedSizeBinary:
			length = (size_t)type->FixedSizeBinary.byteWidth * nitems;
			break;
		default:
			elog(ERROR, "Arrow Type '%s' is not supported now",
				 type->node.tagName);
			break;
	}
	return length;
}

static void
setupRecordBatchField(setupRecordBatchContext *con,
					  RecordBatchFieldState *fstate,
					  ArrowField  *field,
					  int depth)
{
	ArrowBuffer	   *buffer_curr;
	ArrowFieldNode *fnode;

	if (con->fnode_curr >= con->fnode_tail)
		elog(ERROR, "RecordBatch has less ArrowFieldNode than expected");
	fnode = con->fnode_curr++;
	fstate->atttypid   = arrowTypeToPGTypeOid(field, &fstate->atttypmod);
	fstate->nitems     = fnode->length;
	fstate->null_count = fnode->null_count;
	fstate->stat_isnull = true;

	switch (field->type.node.tag)
	{
		case ArrowNodeTag__Int:
		case ArrowNodeTag__FloatingPoint:
		case ArrowNodeTag__Bool:
		case ArrowNodeTag__Decimal:
		case ArrowNodeTag__Date:
		case ArrowNodeTag__Time:
		case ArrowNodeTag__Timestamp:
		case ArrowNodeTag__Interval:
		case ArrowNodeTag__FixedSizeBinary:
			/* fixed length values */
			if (con->buffer_curr + 2 > con->buffer_tail)
				elog(ERROR, "RecordBatch has less buffers than expected");
			buffer_curr = con->buffer_curr++;
			if (fstate->null_count > 0)
			{
				fstate->nullmap_offset = buffer_curr->offset;
				fstate->nullmap_length = buffer_curr->length;
				if (fstate->nullmap_length < BITMAPLEN(fstate->nitems))
					elog(ERROR, "nullmap length is smaller than expected");
				if ((fstate->nullmap_offset & (MAXIMUM_ALIGNOF - 1)) != 0)
					elog(ERROR, "nullmap is not aligned well");
			}
			buffer_curr = con->buffer_curr++;
			fstate->values_offset = buffer_curr->offset;
			fstate->values_length = buffer_curr->length;
			if (fstate->values_length < arrowFieldLength(field,fstate->nitems))
				elog(ERROR, "values array is smaller than expected");
			if ((fstate->values_offset & (MAXIMUM_ALIGNOF - 1)) != 0)
				elog(ERROR, "values array is not aligned well");
			break;

		case ArrowNodeTag__List:
			if (field->_num_children != 1)
				elog(ERROR, "Bug? List of arrow type is corrupted");
			if (depth > 0)
				elog(ERROR, "nested array type is not supported");
			/* nullmap */
			if (con->buffer_curr + 1 > con->buffer_tail)
				elog(ERROR, "RecordBatch has less buffers than expected");
			buffer_curr = con->buffer_curr++;
			if (fstate->null_count > 0)
			{
				fstate->nullmap_offset = buffer_curr->offset;
				fstate->nullmap_length = buffer_curr->length;
				if (fstate->nullmap_length < BITMAPLEN(fstate->nitems))
					elog(ERROR, "nullmap length is smaller than expected");
				if ((fstate->nullmap_offset & (MAXIMUM_ALIGNOF - 1)) != 0)
					elog(ERROR, "nullmap is not aligned well");
			}
			/* offset values */
			buffer_curr = con->buffer_curr++;
			fstate->values_offset = buffer_curr->offset;
			fstate->values_length = buffer_curr->length;
			if (fstate->values_length < arrowFieldLength(field,fstate->nitems))
				elog(ERROR, "offset array is smaller than expected");
			if ((fstate->values_offset & (MAXIMUM_ALIGNOF - 1)) != 0)
				elog(ERROR, "offset array is not aligned well");
			/* setup array element */
			fstate->children = palloc0(sizeof(RecordBatchFieldState));
			setupRecordBatchField(con,
								  &fstate->children[0],
								  &field->children[0],
								  depth+1);
			fstate->num_children = 1;
			break;

		case ArrowNodeTag__Utf8:
		case ArrowNodeTag__Binary:
			/* variable length values */
			if (con->buffer_curr + 3 > con->buffer_tail)
				elog(ERROR, "RecordBatch has less buffers than expected");
			buffer_curr = con->buffer_curr++;
			if (fstate->null_count > 0)
			{
				fstate->nullmap_offset = buffer_curr->offset;
				fstate->nullmap_length = buffer_curr->length;
				if (fstate->nullmap_length < BITMAPLEN(fstate->nitems))
					elog(ERROR, "nullmap length is smaller than expected");
				if ((fstate->nullmap_offset & (MAXIMUM_ALIGNOF - 1)) != 0)
					elog(ERROR, "nullmap is not aligned well");
			}

			buffer_curr = con->buffer_curr++;
			fstate->values_offset = buffer_curr->offset;
			fstate->values_length = buffer_curr->length;
			if (fstate->values_length < arrowFieldLength(field,fstate->nitems))
				elog(ERROR, "offset array is smaller than expected");
			if ((fstate->values_offset & (MAXIMUM_ALIGNOF - 1)) != 0)
				elog(ERROR, "offset array is not aligned well (%lu %lu)", fstate->values_offset, fstate->values_length);

			buffer_curr = con->buffer_curr++;
			fstate->extra_offset = buffer_curr->offset;
			fstate->extra_length = buffer_curr->length;
			if ((fstate->extra_offset & (MAXIMUM_ALIGNOF - 1)) != 0)
				elog(ERROR, "extra buffer is not aligned well");
			break;

		case ArrowNodeTag__Struct:
			if (depth > 0)
				elog(ERROR, "nested composite type is not supported");
			/* only nullmap */
			if (con->buffer_curr + 1 > con->buffer_tail)
				elog(ERROR, "RecordBatch has less buffers than expected");
			buffer_curr = con->buffer_curr++;
			if (fstate->null_count > 0)
			{
				fstate->nullmap_offset = buffer_curr->offset;
				fstate->nullmap_length = buffer_curr->length;
				if (fstate->nullmap_length < BITMAPLEN(fstate->nitems))
					elog(ERROR, "nullmap length is smaller than expected");
				if ((fstate->nullmap_offset & (MAXIMUM_ALIGNOF - 1)) != 0)
					elog(ERROR, "nullmap is not aligned well");
			}

			if (field->_num_children > 0)
			{
				int		i;

				fstate->children = palloc0(sizeof(RecordBatchFieldState) *
									  field->_num_children);
				for (i=0; i < field->_num_children; i++)
				{
					setupRecordBatchField(con,
										  &fstate->children[i],
										  &field->children[i],
										  depth+1);
				}
			}
			fstate->num_children = field->_num_children;
			break;
		default:
			elog(ERROR, "Bug? ArrowSchema contains unsupported types");
	}
	/* assign extra attributes (precision, unitsz, ...) */
	assignArrowTypeOptions(&fstate->attopts, &field->type);
}

static RecordBatchState *
makeRecordBatchState(ArrowSchema *schema,
					 ArrowBlock *block,
					 ArrowRecordBatch *rbatch)
{
	setupRecordBatchContext con;
	RecordBatchState *result;
	int			j, ncols = schema->_num_fields;

	/*
	 * Right now, we have no support for compressed RecordBatches
	 */
	if (rbatch->compression)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("arrow_fdw: compressed record-batches are not supported")));
	
	result = palloc0(offsetof(RecordBatchState, columns[ncols]));
	result->ncols = ncols;
	result->rb_offset = block->offset + block->metaDataLength;
	result->rb_length = block->bodyLength;
	result->rb_nitems = rbatch->length;

	memset(&con, 0, sizeof(setupRecordBatchContext));
	con.buffer_curr = rbatch->buffers;
	con.buffer_tail = rbatch->buffers + rbatch->_num_buffers;
	con.fnode_curr  = rbatch->nodes;
	con.fnode_tail  = rbatch->nodes + rbatch->_num_nodes;

	for (j=0; j < ncols; j++)
	{
		RecordBatchFieldState *fstate = &result->columns[j];
		ArrowField	   *field = &schema->fields[j];

		setupRecordBatchField(&con, fstate, field, 0);
	}
	if (con.buffer_curr != con.buffer_tail ||
		con.fnode_curr  != con.fnode_tail)
		elog(ERROR, "arrow_fdw: RecordBatch may have corruption.");

	return result;
}

/*
 * ExecInitArrowFdw
 */
ArrowFdwState *
ExecInitArrowFdw(ScanState *ss,
				 GpuContext *gcontext,
				 List *outer_quals,
				 Bitmapset *outer_refs)
{
	Relation		relation = ss->ss_currentRelation;
	TupleDesc		tupdesc = RelationGetDescr(relation);
	ForeignTable   *ft = GetForeignTable(RelationGetRelid(relation));
	List		   *filesList = NIL;
	List		   *fdescList = NIL;
	List		   *gpuDirectFileDescList = NIL;
	Bitmapset	   *referenced = NULL;
	Bitmapset	   *stat_attrs = NULL;
	bool			whole_row_ref = false;
	ArrowFdwState  *af_state;
	List		   *rb_state_list = NIL;
	ListCell	   *lc;
	bool			writable;
	int				i, num_rbatches;

	Assert(RelationGetForm(relation)->relkind == RELKIND_FOREIGN_TABLE &&
		   memcmp(GetFdwRoutineForRelation(relation, false),
				  &pgstrom_arrow_fdw_routine, sizeof(FdwRoutine)) == 0);
	/* expand 'referenced' if it has whole-row reference */
	if (bms_is_member(-FirstLowInvalidHeapAttributeNumber, outer_refs))
		whole_row_ref = true;
	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupleDescAttr(tupdesc, i);
		int		k = attr->attnum - FirstLowInvalidHeapAttributeNumber;

		if (attr->attisdropped)
			continue;
		if (whole_row_ref || bms_is_member(k, outer_refs))
			referenced = bms_add_member(referenced, k);
	}

	filesList = __arrowFdwExtractFilesList(ft->options,
										   NULL,
										   &writable);
	foreach (lc, filesList)
	{
		char	   *fname = strVal(lfirst(lc));
		File		fdesc;
		List	   *rb_cached = NIL;
		ListCell   *cell;
		GPUDirectFileDesc *dfile = NULL;

		fdesc = PathNameOpenFile(fname, O_RDONLY | PG_BINARY);
		if (fdesc < 0)
		{
			if (writable && errno == ENOENT)
				continue;
			elog(ERROR, "failed to open '%s' on behalf of '%s'",
				 fname, RelationGetRelationName(relation));
		}
		fdescList = lappend_int(fdescList, fdesc);

		/*
		 * Open file for GPUDirect I/O
		 */
		if (gcontext)
		{
			dfile = palloc0(sizeof(GPUDirectFileDesc));

			gpuDirectFileDescOpen(dfile, fdesc);
			if (!trackRawFileDesc(gcontext, dfile, __FILE__, __LINE__))
			{
				gpuDirectFileDescClose(dfile);
				elog(ERROR, "out of memory");
			}
			gpuDirectFileDescList = lappend(gpuDirectFileDescList, dfile);
		}

		rb_cached = arrowLookupOrBuildMetadataCache(fdesc, &stat_attrs);
		/* check schema compatibility */
		foreach (cell, rb_cached)
		{
			RecordBatchState   *rb_state = lfirst(cell);

			if (!arrowSchemaCompatibilityCheck(tupdesc, rb_state))
				elog(ERROR, "arrow file '%s' on behalf of foreign table '%s' has incompatible schema definition",
					 fname, RelationGetRelationName(relation));
			/* GPUDirect I/O state, if any */
			rb_state->dfile = dfile;
		}
		rb_state_list = list_concat(rb_state_list, rb_cached);
	}
	num_rbatches = list_length(rb_state_list);
	af_state = palloc0(offsetof(ArrowFdwState, rbatches[num_rbatches]));
	af_state->gcontext = gcontext;
	af_state->gpuDirectFileDescList = gpuDirectFileDescList;
	af_state->fdescList = fdescList;
	af_state->referenced = referenced;
	if (arrow_fdw_stats_hint_enabled)
		af_state->stats_hint = execInitArrowStatsHint(ss, stat_attrs,
													  outer_quals);
	af_state->rbatch_index = &af_state->__rbatch_index_local;
	af_state->rbatch_nload = &af_state->__rbatch_nload_local;
	af_state->rbatch_nskip = &af_state->__rbatch_nskip_local;
	i = 0;
	foreach (lc, rb_state_list)
		af_state->rbatches[i++] = (RecordBatchState *)lfirst(lc);
	af_state->num_rbatches = num_rbatches;

	return af_state;
}

/*
 * ArrowBeginForeignScan
 */
static void
ArrowBeginForeignScan(ForeignScanState *node, int eflags)
{
	Relation		relation = node->ss.ss_currentRelation;
	TupleDesc		tupdesc = RelationGetDescr(relation);
	ForeignScan	   *fscan = (ForeignScan *) node->ss.ps.plan;
	ListCell	   *lc;
	Bitmapset	   *referenced = NULL;

	foreach (lc, fscan->fdw_private)
	{
		int		j = lfirst_int(lc);

		if (j >= 0 && j <= tupdesc->natts)
			referenced = bms_add_member(referenced, j -
										FirstLowInvalidHeapAttributeNumber);
	}
	node->fdw_state = ExecInitArrowFdw(&node->ss,
									   NULL,
									   fscan->scan.plan.qual,
									   referenced);
}

typedef struct
{
	off_t		rb_offset;
	off_t		f_offset;
	off_t		m_offset;
	cl_int		io_index;
	cl_int      depth;
	strom_io_chunk ioc[FLEXIBLE_ARRAY_MEMBER];
} arrowFdwSetupIOContext;

/*
 * arrowFdwSetupIOvectorField
 */
static void
__setupIOvectorField(arrowFdwSetupIOContext *con,
					 off_t chunk_offset,
					 size_t chunk_length,
					 cl_uint *p_cmeta_offset,
					 cl_uint *p_cmeta_length)
{
	off_t		f_pos = con->rb_offset + chunk_offset;
	size_t		__length = MAXALIGN(chunk_length);

	Assert((con->m_offset & (MAXIMUM_ALIGNOF - 1)) == 0);

	if (f_pos == con->f_offset)
	{
		/* good, buffer is fully continuous */
		*p_cmeta_offset = __kds_packed(con->m_offset);
		*p_cmeta_length = __kds_packed(__length);

		con->m_offset += __length;
		con->f_offset += __length;
	}
	else if (f_pos > con->f_offset &&
			 (f_pos & ~PAGE_MASK) == (con->f_offset & ~PAGE_MASK) &&
			 ((f_pos - con->f_offset) & (MAXIMUM_ALIGNOF-1)) == 0)
	{
		/*
		 * we can also consolidate the i/o of two chunks, if file position
		 * of the next chunk (f_pos) and the current file tail position
		 * (con->f_offset) locate within the same file page, and if gap bytes
		 * on the file does not break alignment.
		 */
		size_t	__gap = (f_pos - con->f_offset);

		/* put gap bytes */
		Assert(__gap < PAGE_SIZE);
		con->m_offset += __gap;
		con->f_offset += __gap;

		*p_cmeta_offset = __kds_packed(con->m_offset);
		*p_cmeta_length = __kds_packed(__length);

		con->m_offset += __length;
		con->f_offset += __length;
	}
	else
	{
		/*
		 * Elsewhere, we have no chance to consolidate this chunk to
		 * the previous i/o-chunk. So, make a new i/o-chunk.
		 */
		off_t		f_base = TYPEALIGN_DOWN(PAGE_SIZE, f_pos);
		off_t		f_tail;
		off_t		shift = f_pos - f_base;
		strom_io_chunk *ioc;

		if (con->io_index < 0)
			con->io_index = 0;	/* no previous i/o chunks */
		else
		{
			ioc = &con->ioc[con->io_index++];

			f_tail = TYPEALIGN(PAGE_SIZE, con->f_offset);
			ioc->nr_pages = f_tail / PAGE_SIZE - ioc->fchunk_id;
			con->m_offset += (f_tail - con->f_offset); //safety margin;
		}
		ioc = &con->ioc[con->io_index];
		/* adjust position if con->m_offset is not aligned well */
		if (con->m_offset + shift != MAXALIGN(con->m_offset + shift))
			con->m_offset = MAXALIGN(con->m_offset + shift) - shift;
		ioc->m_offset   = con->m_offset;
		ioc->fchunk_id  = f_base / PAGE_SIZE;

		*p_cmeta_offset = __kds_packed(con->m_offset + shift);
		*p_cmeta_length = __kds_packed(__length);

		con->m_offset  += shift + __length;
		con->f_offset   = f_pos + __length;
	}
}

static void
arrowFdwSetupIOvectorField(arrowFdwSetupIOContext *con,
						   RecordBatchFieldState *fstate,
						   kern_data_store *kds,
						   kern_colmeta *cmeta)
{
	//int		index = cmeta - kds->colmeta;

	if (fstate->nullmap_length > 0)
	{
		Assert(fstate->null_count > 0);
		__setupIOvectorField(con,
							 fstate->nullmap_offset,
							 fstate->nullmap_length,
							 &cmeta->nullmap_offset,
							 &cmeta->nullmap_length);
		//elog(INFO, "D%d att[%d] nullmap=%lu,%lu m_offset=%lu f_offset=%lu", con->depth, index, fstate->nullmap_offset, fstate->nullmap_length, con->m_offset, con->f_offset);
	}
	if (fstate->values_length > 0)
	{
		__setupIOvectorField(con,
							 fstate->values_offset,
							 fstate->values_length,
							 &cmeta->values_offset,
							 &cmeta->values_length);
		//elog(INFO, "D%d att[%d] values=%lu,%lu m_offset=%lu f_offset=%lu", con->depth, index, fstate->values_offset, fstate->values_length, con->m_offset, con->f_offset);
	}
	if (fstate->extra_length > 0)
	{
		__setupIOvectorField(con,
							 fstate->extra_offset,
							 fstate->extra_length,
							 &cmeta->extra_offset,
							 &cmeta->extra_length);
		//elog(INFO, "D%d att[%d] extra=%lu,%lu m_offset=%lu f_offset=%lu", con->depth, index, fstate->extra_offset, fstate->extra_length, con->m_offset, con->f_offset);
	}

	/* nested sub-fields if composite types */
	if (cmeta->atttypkind == TYPE_KIND__ARRAY ||
		cmeta->atttypkind == TYPE_KIND__COMPOSITE)
	{
		kern_colmeta *subattr;
		int		j;

		Assert(fstate->num_children == cmeta->num_subattrs);
		con->depth++;
		for (j=0, subattr = &kds->colmeta[cmeta->idx_subattrs];
			 j < cmeta->num_subattrs;
			 j++, subattr++)
		{
			RecordBatchFieldState *child = &fstate->children[j];

			arrowFdwSetupIOvectorField(con, child, kds, subattr);
		}
		con->depth--;
	}
}

/*
 * arrowFdwSetupIOvector
 */
static strom_io_vector *
arrowFdwSetupIOvector(kern_data_store *kds,
					  RecordBatchState *rb_state,
					  Bitmapset *referenced)
{
	arrowFdwSetupIOContext *con;
	strom_io_vector *iovec = NULL;
	int			j, nr_chunks = 0;

	Assert(kds->nr_colmeta >= kds->ncols);
	con = alloca(offsetof(arrowFdwSetupIOContext,
						  ioc[3 * kds->nr_colmeta]));
	con->rb_offset = rb_state->rb_offset;
	con->f_offset  = ~0UL;	/* invalid offset */
	con->m_offset  = TYPEALIGN(PAGE_SIZE, KERN_DATA_STORE_HEAD_LENGTH(kds));
	con->io_index  = -1;
	for (j=0; j < kds->ncols; j++)
	{
		RecordBatchFieldState *fstate = &rb_state->columns[j];
		kern_colmeta *cmeta = &kds->colmeta[j];
		int			attidx = j + 1 - FirstLowInvalidHeapAttributeNumber;

		if (referenced && bms_is_member(attidx, referenced))
			arrowFdwSetupIOvectorField(con, fstate, kds, cmeta);
		else
			cmeta->atttypkind = TYPE_KIND__NULL;	/* unreferenced */
	}
	if (con->io_index >= 0)
	{
		/* close the last I/O chunks */
		strom_io_chunk *ioc = &con->ioc[con->io_index];

		ioc->nr_pages = (TYPEALIGN(PAGE_SIZE, con->f_offset) / PAGE_SIZE -
						 ioc->fchunk_id);
		con->m_offset = ioc->m_offset + PAGE_SIZE * ioc->nr_pages;
		nr_chunks = con->io_index + 1;
	}
	kds->length = con->m_offset;

	iovec = palloc0(offsetof(strom_io_vector, ioc[nr_chunks]));
	iovec->nr_chunks = nr_chunks;
	if (nr_chunks > 0)
		memcpy(iovec->ioc, con->ioc, sizeof(strom_io_chunk) * nr_chunks);
	return iovec;
}

/*
 * __dump_kds_and_iovec - just for debug
 */
static inline void
__dump_kds_and_iovec(kern_data_store *kds, strom_io_vector *iovec)
{
#if 0
	int		j;

	elog(INFO, "nchunks = %d", iovec->nr_chunks);
	for (j=0; j < iovec->nr_chunks; j++)
	{
		strom_io_chunk *ioc = &iovec->ioc[j];

		elog(INFO, "io[%d] [ m_offset=%lu, f_read=%lu...%lu, nr_pages=%u}",
			 j,
			 ioc->m_offset,
			 ioc->fchunk_id * PAGE_SIZE,
			 (ioc->fchunk_id + ioc->nr_pages) * PAGE_SIZE,
			 ioc->nr_pages);
	}

	elog(INFO, "kds {length=%zu nitems=%u typeid=%u typmod=%u table_oid=%u}",
		 kds->length, kds->nitems,
		 kds->tdtypeid, kds->tdtypmod, kds->table_oid);
	for (j=0; j < kds->nr_colmeta; j++)
	{
		kern_colmeta *cmeta = &kds->colmeta[j];

		elog(INFO, "%ccol[%d] nullmap=%lu,%lu values=%lu,%lu extra=%lu,%lu",
			 j < kds->ncols ? ' ' : '*', j,
			 __kds_unpack(cmeta->nullmap_offset),
			 __kds_unpack(cmeta->nullmap_length),
			 __kds_unpack(cmeta->values_offset),
			 __kds_unpack(cmeta->values_length),
			 __kds_unpack(cmeta->extra_offset),
			 __kds_unpack(cmeta->extra_length));

	}
#endif
}

/*
 * arrowFdwLoadRecordBatch
 */
static void
__arrowFdwAssignTypeOptions(kern_data_store *kds,
							int base, int ncols,
							RecordBatchFieldState *rb_fstate)
{
	int		i;

	for (i=0; i < ncols; i++)
	{
		kern_colmeta   *cmeta = &kds->colmeta[base+i];

		cmeta->attopts = rb_fstate[i].attopts;
		if (cmeta->atttypkind == TYPE_KIND__ARRAY)
		{
			Assert(cmeta->idx_subattrs >= kds->ncols &&
				   cmeta->num_subattrs == 1 &&
				   cmeta->idx_subattrs + cmeta->num_subattrs <= kds->nr_colmeta);
			Assert(rb_fstate[i].num_children == 1);
			__arrowFdwAssignTypeOptions(kds,
										cmeta->idx_subattrs,
										cmeta->num_subattrs,
										rb_fstate[i].children);
		}
		else if (cmeta->atttypkind == TYPE_KIND__COMPOSITE)
		{
			Assert(cmeta->idx_subattrs >= kds->ncols &&
				   cmeta->idx_subattrs + cmeta->num_subattrs <= kds->nr_colmeta);
			Assert(rb_fstate[i].num_children == cmeta->num_subattrs);
			__arrowFdwAssignTypeOptions(kds,
										cmeta->idx_subattrs,
										cmeta->num_subattrs,
										rb_fstate[i].children);
		}
	}
}

static pgstrom_data_store *
__arrowFdwLoadRecordBatch(RecordBatchState *rb_state,
						  Relation relation,
						  Bitmapset *referenced,
						  GpuContext *gcontext,
						  MemoryContext mcontext,
						  const Bitmapset *optimal_gpus)
{
	TupleDesc			tupdesc = RelationGetDescr(relation);
	pgstrom_data_store *pds;
	kern_data_store	   *kds;
	strom_io_vector	   *iovec;
	size_t				head_sz;
	CUresult			rc;

	/* setup KDS and I/O-vector */
	head_sz = KDS_calculateHeadSize(tupdesc);
	kds = alloca(head_sz);
	init_kernel_data_store(kds, tupdesc, 0, KDS_FORMAT_ARROW, 0);
	kds->nitems = rb_state->rb_nitems;
	kds->nrooms = rb_state->rb_nitems;
	kds->table_oid = RelationGetRelid(relation);
	Assert(head_sz == KERN_DATA_STORE_HEAD_LENGTH(kds));
	Assert(kds->ncols == rb_state->ncols);
	__arrowFdwAssignTypeOptions(kds, 0, kds->ncols, rb_state->columns);
	iovec = arrowFdwSetupIOvector(kds, rb_state, referenced);
	__dump_kds_and_iovec(kds, iovec);

	/*
	 * If SSD-to-GPU Direct SQL is available on the arrow file, setup a small
	 * PDS on host-pinned memory, with strom_io_vector.
	 */
	if (gcontext &&
		bms_is_member(gcontext->cuda_dindex, optimal_gpus) &&
		iovec->nr_chunks > 0 &&
		kds->length <= gpuMemAllocIOMapMaxLength() &&
		rb_state->dfile != NULL)
	{
		size_t	iovec_sz = offsetof(strom_io_vector, ioc[iovec->nr_chunks]);

		rc = gpuMemAllocHost(gcontext, (void **)&pds,
							 offsetof(pgstrom_data_store, kds) +
							 head_sz + iovec_sz);
		if (rc != CUDA_SUCCESS)
			elog(ERROR, "failed on gpuMemAllocHost: %s", errorText(rc));

		pds->gcontext = gcontext;
		pg_atomic_init_u32(&pds->refcnt, 1);
		pds->nblocks_uncached = 0;
		memcpy(&pds->filedesc, rb_state->dfile, sizeof(GPUDirectFileDesc));
		pds->iovec = (strom_io_vector *)((char *)&pds->kds + head_sz);
		memcpy(&pds->kds, kds, head_sz);
		memcpy(pds->iovec, iovec, iovec_sz);
	}
	else
	{
		/* Elsewhere, load RecordBatch by filesystem */
		int		fdesc = FileGetRawDesc(rb_state->fdesc);

		if (gcontext)
		{
			rc = gpuMemAllocManaged(gcontext,
									(CUdeviceptr *)&pds,
									offsetof(pgstrom_data_store,
											 kds) + kds->length,
									CU_MEM_ATTACH_GLOBAL);
			if (rc != CUDA_SUCCESS)
				elog(ERROR, "failed on gpuMemAllocManaged: %s", errorText(rc));
		}
		else
		{
			pds = MemoryContextAllocHuge(mcontext,
										 offsetof(pgstrom_data_store,
												  kds) + kds->length);
		}
		__PDS_fillup_arrow(pds, gcontext, kds, fdesc, iovec);
	}
	pfree(iovec);
	return pds;
}

static pgstrom_data_store *
arrowFdwLoadRecordBatch(ArrowFdwState *af_state,
						Relation relation,
						EState *estate,
						GpuContext *gcontext,
						const Bitmapset *optimal_gpus)
{
	RecordBatchState *rb_state;
	uint32		rb_index;

retry:
	/* fetch next RecordBatch */
	rb_index = pg_atomic_fetch_add_u32(af_state->rbatch_index, 1);
	if (rb_index >= af_state->num_rbatches)
		return NULL;	/* no more RecordBatch to read */
	rb_state = af_state->rbatches[rb_index];

	if (af_state->stats_hint)
	{
		if (execCheckArrowStatsHint(af_state->stats_hint, rb_state))
			pg_atomic_fetch_add_u32(af_state->rbatch_nload, 1);
		else
		{
			pg_atomic_fetch_add_u32(af_state->rbatch_nskip, 1);
			goto retry;
		}
	}
	return __arrowFdwLoadRecordBatch(rb_state,
									 relation,
									 af_state->referenced,
									 gcontext,
									 estate->es_query_cxt,
									 optimal_gpus);
}

/*
 * ExecScanChunkArrowFdw
 */
pgstrom_data_store *
ExecScanChunkArrowFdw(GpuTaskState *gts)
{
	pgstrom_data_store *pds;

	InstrStartNode(&gts->outer_instrument);
	pds = arrowFdwLoadRecordBatch(gts->af_state,
								  gts->css.ss.ss_currentRelation,
								  gts->css.ss.ps.state,
								  gts->gcontext,
								  gts->optimal_gpus);
	InstrStopNode(&gts->outer_instrument,
				  !pds ? 0.0 : (double)pds->kds.nitems);
	return pds;
}

/*
 * ArrowIterateForeignScan
 */
static TupleTableSlot *
ArrowIterateForeignScan(ForeignScanState *node)
{
	ArrowFdwState  *af_state = node->fdw_state;
	Relation		relation = node->ss.ss_currentRelation;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	pgstrom_data_store *pds;

	while ((pds = af_state->curr_pds) == NULL ||
		   af_state->curr_index >= pds->kds.nitems)
	{
		EState	   *estate = node->ss.ps.state;

		/* unload the previous RecordBatch, if any */
		if (pds)
			PDS_release(pds);
		af_state->curr_index = 0;
		af_state->curr_pds = arrowFdwLoadRecordBatch(af_state,
													 relation,
													 estate,
													 NULL,
													 NULL);
		if (!af_state->curr_pds)
			return NULL;
	}
	Assert(pds && af_state->curr_index < pds->kds.nitems);
	if (KDS_fetch_tuple_arrow(slot, &pds->kds, af_state->curr_index++))
		return slot;
	return NULL;
}

/*
 * ArrowReScanForeignScan
 */
void
ExecReScanArrowFdw(ArrowFdwState *af_state)
{
	/* rewind the current scan state */
	pg_atomic_write_u32(af_state->rbatch_index, 0);
	if (af_state->curr_pds)
		PDS_release(af_state->curr_pds);
	af_state->curr_pds = NULL;
	af_state->curr_index = 0;
}

static void
ArrowReScanForeignScan(ForeignScanState *node)
{
	ExecReScanArrowFdw((ArrowFdwState *)node->fdw_state);
}

/*
 * ArrowEndForeignScan
 */
void
ExecEndArrowFdw(ArrowFdwState *af_state)
{
	ListCell   *lc;

	foreach (lc, af_state->fdescList)
		FileClose((File)lfirst_int(lc));
	foreach (lc, af_state->gpuDirectFileDescList)
	{
		GPUDirectFileDesc *dfile = lfirst(lc);

		untrackRawFileDesc(af_state->gcontext, dfile);
		gpuDirectFileDescClose(dfile);
	}
	if (af_state->stats_hint)
		execEndArrowStatsHint(af_state->stats_hint);
}

static void
ArrowEndForeignScan(ForeignScanState *node)
{
	ExecEndArrowFdw((ArrowFdwState *)node->fdw_state);
}

/*
 * ArrowExplainForeignScan 
 */
void
ExplainArrowFdw(ArrowFdwState *af_state,
				Relation frel,
				ExplainState *es,
				List *dcontext)
{
	TupleDesc	tupdesc = RelationGetDescr(frel);
	ListCell   *lc;
	int			fcount = 0;
	char		label[80];
	size_t	   *chunk_sz = alloca(sizeof(size_t) * tupdesc->natts);
	int			i, j, k;
	StringInfoData	buf;

	/* shows referenced columns */
	initStringInfo(&buf);
	for (k = bms_next_member(af_state->referenced, -1);
		 k >= 0;
		 k = bms_next_member(af_state->referenced, k))
	{
		j = k + FirstLowInvalidHeapAttributeNumber - 1;

		if (j >= 0)
		{
			Form_pg_attribute	attr = tupleDescAttr(tupdesc, j);
			const char		   *attName = NameStr(attr->attname);
			if (buf.len > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, quote_identifier(attName));
		}
	}
	ExplainPropertyText("referenced", buf.data, es);

	/* shows stats hint if any */
	if (af_state->stats_hint)
	{
		arrowStatsHint *stats_hint = af_state->stats_hint;

		resetStringInfo(&buf);

		if (dcontext == NIL)
		{
			int		anum;

			for (anum = bms_next_member(stats_hint->load_attrs, -1);
				 anum >= 0;
				 anum = bms_next_member(stats_hint->load_attrs, anum))
			{
				Form_pg_attribute attr = tupleDescAttr(tupdesc, anum-1);
				const char *attName = NameStr(attr->attname);

				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, quote_identifier(attName));
			}
		}
		else
		{
			ListCell   *lc;

			foreach (lc, stats_hint->orig_quals)
			{
				Node   *qual = lfirst(lc);
				char   *temp;

				temp = deparse_expression(qual, dcontext, es->verbose, false);
				if (buf.len > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, temp);
				pfree(temp);
			}
		}
		if (es->analyze)
			appendStringInfo(&buf, "  [loaded: %u, skipped: %u]",
							 pg_atomic_read_u32(af_state->rbatch_nload),
							 pg_atomic_read_u32(af_state->rbatch_nskip));
		ExplainPropertyText("Stats-Hint", buf.data, es);
	}

	/* shows files on behalf of the foreign table */
	foreach (lc, af_state->fdescList)
	{
		File		fdesc = (File)lfirst_int(lc);
		const char *fname = FilePathName(fdesc);
		int			rbcount = 0;
		size_t		read_sz = 0;
		char	   *pos = label;
		struct stat	st_buf;

		pos += snprintf(label, sizeof(label), "files%d", fcount++);
		if (fstat(FileGetRawDesc(fdesc), &st_buf) != 0)
			memset(&st_buf, 0, sizeof(struct stat));

		/* size count per chunk */
		memset(chunk_sz, 0, sizeof(size_t) * tupdesc->natts);
		for (i=0; i < af_state->num_rbatches; i++)
		{
			RecordBatchState *rb_state = af_state->rbatches[i];
			size_t		sz;

			if (rb_state->fdesc != fdesc)
				continue;

			for (k = bms_next_member(af_state->referenced, -1);
				 k >= 0;
				 k = bms_next_member(af_state->referenced, k))
			{
				j = k + FirstLowInvalidHeapAttributeNumber - 1;
				if (j < 0 || j >= tupdesc->natts)
					continue;
				sz = RecordBatchFieldLength(&rb_state->columns[j]);
				read_sz += sz;
				chunk_sz[j] += sz;
			}
			rbcount++;
		}

		/* file size and read size */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			resetStringInfo(&buf);
			if (st_buf.st_size == 0)
				appendStringInfoString(&buf, fname);
			else
				appendStringInfo(&buf, "%s (read: %s, size: %s)",
								 fname,
								 format_bytesz(read_sz),
								 format_bytesz(st_buf.st_size));
			ExplainPropertyText(label, buf.data, es);
		}
		else
		{
			ExplainPropertyText(label, fname, es);

			sprintf(pos, "-size");
			ExplainPropertyText(label, format_bytesz(st_buf.st_size), es);

			sprintf(pos, "-read");
			ExplainPropertyText(label, format_bytesz(read_sz), es);
		}

		/* read-size per column (verbose mode only)  */
		if (es->verbose && rbcount >= 0)
		{
			for (k = bms_next_member(af_state->referenced, -1);
                 k >= 0;
                 k = bms_next_member(af_state->referenced, k))
            {
				Form_pg_attribute attr;

				j = k + FirstLowInvalidHeapAttributeNumber - 1;
				if (j < 0 || j >= tupdesc->natts)
					continue;
				attr = tupleDescAttr(tupdesc, j);
				snprintf(label, sizeof(label),
						 "  %s", NameStr(attr->attname));
				ExplainPropertyText(label, format_bytesz(chunk_sz[j]), es);
			}
		}
	}
	pfree(buf.data);
}

static void
ArrowExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	Relation	frel = node->ss.ss_currentRelation;

	ExplainArrowFdw((ArrowFdwState *)node->fdw_state, frel, es, NIL);
}

/*
 * readArrowFile
 */
static bool
readArrowFile(const char *pathname, ArrowFileInfo *af_info, bool missing_ok)
{
    File	filp = PathNameOpenFile(pathname, O_RDONLY | PG_BINARY);

	if (filp < 0)
	{
		if (missing_ok && errno == ENOENT)
			return false;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", pathname)));
	}
	readArrowFileDesc(FileGetRawDesc(filp), af_info);
	FileClose(filp);
	return true;
}

/*
 * RecordBatchAcquireSampleRows - random sampling
 */
static int
RecordBatchAcquireSampleRows(Relation relation,
							 RecordBatchState *rb_state,
							 HeapTuple *rows,
							 int nsamples)
{
	TupleDesc		tupdesc = RelationGetDescr(relation);
	pgstrom_data_store *pds;
	Bitmapset	   *referenced = NULL;
	Datum		   *values;
	bool		   *isnull;
	int				count;
	int				i, j, nwords;

	/* ANALYZE needs to fetch all the attributes */
	nwords = (tupdesc->natts - FirstLowInvalidHeapAttributeNumber +
			  BITS_PER_BITMAPWORD - 1) / BITS_PER_BITMAPWORD;
	referenced = alloca(offsetof(Bitmapset, words[nwords]));
	referenced->nwords = nwords;
	memset(referenced->words, -1, sizeof(bitmapword) * nwords);
	
	pds = __arrowFdwLoadRecordBatch(rb_state,
									relation,
									referenced,
									NULL,
									CurrentMemoryContext,
									NULL);
	values = alloca(sizeof(Datum) * tupdesc->natts);
	isnull = alloca(sizeof(bool)  * tupdesc->natts);
	for (count = 0; count < nsamples; count++)
	{
		/* fetch a row randomly */
		i = (double)pds->kds.nitems * drand48();
		Assert(i < pds->kds.nitems);

		for (j=0; j < pds->kds.ncols; j++)
		{
			kern_colmeta   *cmeta = &pds->kds.colmeta[j];
			
			pg_datum_arrow_ref(&pds->kds,
							   cmeta,
							   i,
							   values + j,
							   isnull + j);
		}
		rows[count] = heap_form_tuple(tupdesc, values, isnull);
	}
	PDS_release(pds);

	return count;
}

/*
 * ArrowAcquireSampleRows
 */
static int
ArrowAcquireSampleRows(Relation relation,
					   int elevel,
					   HeapTuple *rows,
					   int nrooms,
					   double *p_totalrows,
					   double *p_totaldeadrows)
{
	TupleDesc		tupdesc = RelationGetDescr(relation);
	ForeignTable   *ft = GetForeignTable(RelationGetRelid(relation));
	List		   *filesList = NIL;
	List		   *fdescList = NIL;
	List		   *rb_state_list = NIL;
	ListCell	   *lc;
	bool			writable;
	int64			total_nrows = 0;
	int64			count_nrows = 0;
	int				nsamples_min = nrooms / 100;
	int				nitems = 0;

	filesList = __arrowFdwExtractFilesList(ft->options,
										   NULL,
										   &writable);
	foreach (lc, filesList)
	{
		char	   *fname = strVal(lfirst(lc));
		File		fdesc;
		List	   *rb_cached;
		ListCell   *cell;

		fdesc = PathNameOpenFile(fname, O_RDONLY | PG_BINARY);
        if (fdesc < 0)
		{
			if (writable && errno == ENOENT)
				continue;
			elog(ERROR, "failed to open file '%s' on behalf of '%s'",
				 fname, RelationGetRelationName(relation));
		}
		fdescList = lappend_int(fdescList, fdesc);
		
		rb_cached = arrowLookupOrBuildMetadataCache(fdesc, NULL);
		foreach (cell, rb_cached)
		{
			RecordBatchState *rb_state = lfirst(cell);

			if (!arrowSchemaCompatibilityCheck(tupdesc, rb_state))
				elog(ERROR, "arrow file '%s' on behalf of foreign table '%s' has incompatible schema definition",
					 fname, RelationGetRelationName(relation));
			if (rb_state->rb_nitems == 0)
				continue;	/* not reasonable to sample, skipped */
			total_nrows += rb_state->rb_nitems;

			rb_state_list = lappend(rb_state_list, rb_state);
		}
	}
	nrooms = Min(nrooms, total_nrows);

	/* fetch samples for each record-batch */
	foreach (lc, rb_state_list)
	{
		RecordBatchState *rb_state = lfirst(lc);
		int			nsamples;

		count_nrows += rb_state->rb_nitems;
		nsamples = (double)nrooms * ((double)count_nrows /
									 (double)total_nrows) - nitems;
		if (nitems + nsamples > nrooms)
			nsamples = nrooms - nitems;
		if (nsamples > nsamples_min)
			nitems += RecordBatchAcquireSampleRows(relation,
												   rb_state,
												   rows + nitems,
												   nsamples);
	}
	foreach (lc, fdescList)
		FileClose((File)lfirst_int(lc));

	*p_totalrows = total_nrows;
	*p_totaldeadrows = 0.0;

	return nitems;
}

/*
 * ArrowAnalyzeForeignTable
 */
static bool
ArrowAnalyzeForeignTable(Relation frel,
						 AcquireSampleRowsFunc *p_sample_rows_func,
						 BlockNumber *p_totalpages)
{
	ForeignTable   *ft = GetForeignTable(RelationGetRelid(frel));
	List		   *filesList = arrowFdwExtractFilesList(ft->options);
	ListCell	   *lc;
	Size			totalpages = 0;

	foreach (lc, filesList)
	{
		const char *fname = strVal(lfirst(lc));
		struct stat	statbuf;

		if (stat(fname, &statbuf) != 0)
		{
			elog(NOTICE, "failed on stat('%s') on behalf of '%s', skipped",
				 fname, get_rel_name(ft->relid));
			continue;
		}
		totalpages += (statbuf.st_size + BLCKSZ - 1) / BLCKSZ;
	}

	if (totalpages > MaxBlockNumber)
		totalpages = MaxBlockNumber;

	*p_sample_rows_func = ArrowAcquireSampleRows;
	*p_totalpages = totalpages;

	return true;
}

/*
 * ArrowImportForeignSchema
 */
static List *
ArrowImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	ArrowSchema	schema;
	List	   *filesList;
	ListCell   *lc;
	int			j;
	StringInfoData	cmd;

	/* sanity checks */
	switch (stmt->list_type)
	{
		case FDW_IMPORT_SCHEMA_ALL:
			break;
		case FDW_IMPORT_SCHEMA_LIMIT_TO:
			elog(ERROR, "arrow_fdw does not support LIMIT TO clause");
			break;
		case FDW_IMPORT_SCHEMA_EXCEPT:
			elog(ERROR, "arrow_fdw does not support EXCEPT clause");
			break;
		default:
			elog(ERROR, "arrow_fdw: Bug? unknown list-type");
			break;
	}
	filesList = arrowFdwExtractFilesList(stmt->options);
	if (filesList == NIL)
		ereport(ERROR,
				(errmsg("No valid apache arrow files are specified"),
				 errhint("Use 'file' or 'dir' option to specify apache arrow files on behalf of the foreign table")));

	/* read the schema */
	memset(&schema, 0, sizeof(ArrowSchema));
	foreach (lc, filesList)
	{
		const char   *fname = strVal(lfirst(lc));
		ArrowFileInfo af_info;

		readArrowFile(fname, &af_info, false);
		if (lc == list_head(filesList))
		{
			copyArrowNode(&schema.node, &af_info.footer.schema.node);
		}
		else
		{
			/* compatibility checks */
			ArrowSchema	   *stemp = &af_info.footer.schema;

			if (schema.endianness != stemp->endianness ||
				schema._num_fields != stemp->_num_fields)
				elog(ERROR, "file '%s' has incompatible schema definition", fname);
			for (j=0; j < schema._num_fields; j++)
			{
				if (arrowFieldTypeIsEqual(&schema.fields[j],
										  &stemp->fields[j]))
					elog(ERROR, "file '%s' has incompatible schema definition", fname);
			}
		}
	}

	/* makes a command to define foreign table */
	initStringInfo(&cmd);
	appendStringInfo(&cmd, "CREATE FOREIGN TABLE %s (\n",
					 quote_identifier(stmt->remote_schema));
	for (j=0; j < schema._num_fields; j++)
	{
		ArrowField *field = &schema.fields[j];
		const char *type_name = arrowTypeToPGTypeName(field);

		if (j > 0)
			appendStringInfo(&cmd, ",\n");
		if (!field->name || field->_name_len == 0)
		{
			elog(NOTICE, "field %d has no name, so \"__col%02d\" is used",
				 j+1, j+1);
			appendStringInfo(&cmd, "  __col%02d  %s", j+1, type_name);
		}
		else
			appendStringInfo(&cmd, "  %s %s",
							 quote_identifier(field->name), type_name);
	}
	appendStringInfo(&cmd,
					 "\n"
					 ") SERVER %s\n"
					 "  OPTIONS (", stmt->server_name);
	foreach (lc, stmt->options)
	{
		DefElem	   *defel = lfirst(lc);

		if (lc != list_head(stmt->options))
			appendStringInfo(&cmd, ",\n           ");
		appendStringInfo(&cmd, "%s '%s'",
						 defel->defname,
						 strVal(defel->arg));
	}
	appendStringInfo(&cmd, ")");

	return list_make1(cmd.data);
}

/*
 * pgstrom_arrow_fdw_import_file
 *
 * NOTE: Due to historical reason, PostgreSQL does not allow to define
 * columns more than MaxHeapAttributeNumber (1600) for foreign-tables also,
 * not only heap-tables. This restriction comes from NULL-bitmap length
 * in HeapTupleHeaderData and width of t_hoff.
 * However, it is not a reasonable restriction for foreign-table, because
 * it does not use heap-format internally.
 */
static void
__insertPgAttributeTuple(Relation pg_attr_rel,
						 CatalogIndexState pg_attr_index,
						 Oid ftable_oid,
						 AttrNumber attnum,
						 ArrowField *field)
{
	Oid			type_oid;
	int32		type_mod;
	int16		type_len;
	bool		type_byval;
	char		type_align;
	int32		type_ndims;
	char		type_storage;
	Datum		values[Natts_pg_attribute];
	bool		isnull[Natts_pg_attribute];
	HeapTuple	tup;
	ObjectAddress myself, referenced;

	type_oid = arrowTypeToPGTypeOid(field, &type_mod);
	get_typlenbyvalalign(type_oid,
						 &type_len,
						 &type_byval,
						 &type_align);
	type_ndims = (type_is_array(type_oid) ? 1 : 0);
	type_storage = get_typstorage(type_oid);

	memset(values, 0, sizeof(values));
	memset(isnull, 0, sizeof(isnull));

	values[Anum_pg_attribute_attrelid - 1] = ObjectIdGetDatum(ftable_oid);
	values[Anum_pg_attribute_attname - 1] = CStringGetDatum(field->name);
	values[Anum_pg_attribute_atttypid - 1] = ObjectIdGetDatum(type_oid);
	values[Anum_pg_attribute_attstattarget - 1] = Int32GetDatum(-1);
	values[Anum_pg_attribute_attlen - 1] = Int16GetDatum(type_len);
	values[Anum_pg_attribute_attnum - 1] = Int16GetDatum(attnum);
	values[Anum_pg_attribute_attndims - 1] = Int32GetDatum(type_ndims);
	values[Anum_pg_attribute_attcacheoff - 1] = Int32GetDatum(-1);
	values[Anum_pg_attribute_atttypmod - 1] = Int32GetDatum(type_mod);
	values[Anum_pg_attribute_attbyval - 1] = BoolGetDatum(type_byval);
	values[Anum_pg_attribute_attstorage - 1] = CharGetDatum(type_storage);
	values[Anum_pg_attribute_attalign - 1] = CharGetDatum(type_align);
	values[Anum_pg_attribute_attnotnull - 1] = BoolGetDatum(!field->nullable);
	values[Anum_pg_attribute_attislocal - 1] = BoolGetDatum(true);
	isnull[Anum_pg_attribute_attacl - 1] = true;
	isnull[Anum_pg_attribute_attoptions - 1] = true;
	isnull[Anum_pg_attribute_attfdwoptions - 1] = true;
	isnull[Anum_pg_attribute_attmissingval - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(pg_attr_rel), values, isnull);
	CatalogTupleInsertWithInfo(pg_attr_rel, tup, pg_attr_index);

	/* add dependency */
	myself.classId = RelationRelationId;
	myself.objectId = ftable_oid;
	myself.objectSubId = attnum;
	referenced.classId = TypeRelationId;
	referenced.objectId = type_oid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	heap_freetuple(tup);
}

Datum
pgstrom_arrow_fdw_import_file(PG_FUNCTION_ARGS)
{
	CreateForeignTableStmt stmt;
	ArrowSchema	schema;
	List	   *tableElts = NIL;
	char	   *ftable_name;
	char	   *file_name;
	char	   *namespace_name;
	DefElem	   *defel;
	int			j, nfields;
	Oid			ftable_oid;
	Oid			type_oid;
	int			type_mod;
	ObjectAddress myself;
	ArrowFileInfo af_info;

	/* read schema of the file */
	if (PG_ARGISNULL(0))
		elog(ERROR, "foreign table name is not supplied");
	ftable_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (PG_ARGISNULL(1))
		elog(ERROR, "arrow filename is not supplied");
	file_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	defel = makeDefElem("file", (Node *)makeString(file_name), -1);

	if (PG_ARGISNULL(2))
		namespace_name = NULL;
	else
		namespace_name = text_to_cstring(PG_GETARG_TEXT_PP(2));

	readArrowFile(file_name, &af_info, false);
	copyArrowNode(&schema.node, &af_info.footer.schema.node);
	if (schema._num_fields > SHRT_MAX)
		Elog("Arrow file '%s' has too much fields: %d",
			 file_name, schema._num_fields);

	/* setup CreateForeignTableStmt */
	memset(&stmt, 0, sizeof(CreateForeignTableStmt));
	NodeSetTag(&stmt, T_CreateForeignTableStmt);
	stmt.base.relation = makeRangeVar(namespace_name, ftable_name, -1);

	nfields = Min(schema._num_fields, 100);
	for (j=0; j < nfields; j++)
	{
		ColumnDef  *cdef;

		type_oid = arrowTypeToPGTypeOid(&schema.fields[j], &type_mod);
		cdef = makeColumnDef(schema.fields[j].name,
							 type_oid,
							 type_mod,
							 InvalidOid);
		tableElts = lappend(tableElts, cdef);
	}
	stmt.base.tableElts = tableElts;
	stmt.base.oncommit = ONCOMMIT_NOOP;
	stmt.servername = "arrow_fdw";
	stmt.options = list_make1(defel);

	myself = DefineRelation(&stmt.base,
							RELKIND_FOREIGN_TABLE,
							InvalidOid,
							NULL,
							__FUNCTION__);
	ftable_oid = myself.objectId;
	CreateForeignTable(&stmt, ftable_oid);

	if (nfields < schema._num_fields)
	{
		Relation	c_rel = table_open(RelationRelationId, RowExclusiveLock);
		Relation	a_rel = table_open(AttributeRelationId, RowExclusiveLock);
		CatalogIndexState c_index = CatalogOpenIndexes(c_rel);
		CatalogIndexState a_index = CatalogOpenIndexes(a_rel);
		HeapTuple	tup;

		tup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(ftable_oid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for relation %u", ftable_oid);
		
		for (j=nfields; j < schema._num_fields; j++)
		{
			__insertPgAttributeTuple(a_rel,
									 a_index,
									 ftable_oid,
									 j+1,
                                     &schema.fields[j]);
		}
		/* update relnatts also */
		((Form_pg_class) GETSTRUCT(tup))->relnatts = schema._num_fields;
		CatalogTupleUpdate(c_rel, &tup->t_self, tup);
		
		CatalogCloseIndexes(a_index);
		CatalogCloseIndexes(c_index);
		table_close(a_rel, RowExclusiveLock);
		table_close(c_rel, RowExclusiveLock);

		CommandCounterIncrement();
	}	
	PG_RETURN_VOID();
}
PG_FUNCTION_INFO_V1(pgstrom_arrow_fdw_import_file);

/*
 * ArrowIsForeignScanParallelSafe
 */
static bool
ArrowIsForeignScanParallelSafe(PlannerInfo *root,
							   RelOptInfo *rel,
							   RangeTblEntry *rte)
{
	return true;
}

/*
 * ArrowEstimateDSMForeignScan 
 */
static Size
ArrowEstimateDSMForeignScan(ForeignScanState *node,
							ParallelContext *pcxt)
{
	return MAXALIGN(sizeof(pg_atomic_uint32) * 3);
}

/*
 * ArrowInitializeDSMForeignScan
 */
static inline void
__ExecInitDSMArrowFdw(ArrowFdwState *af_state,
					  pg_atomic_uint32 *rbatch_index,
					  pg_atomic_uint32 *rbatch_nload,
					  pg_atomic_uint32 *rbatch_nskip)
{
	pg_atomic_init_u32(rbatch_index, 0);
	af_state->rbatch_index = rbatch_index;
	pg_atomic_init_u32(rbatch_nload, 0);
	af_state->rbatch_nload = rbatch_nload;
	pg_atomic_init_u32(rbatch_nskip, 0);
	af_state->rbatch_nskip = rbatch_nskip;
}

void
ExecInitDSMArrowFdw(ArrowFdwState *af_state, GpuTaskSharedState *gtss)
{
	__ExecInitDSMArrowFdw(af_state,
						  &gtss->af_rbatch_index,
						  &gtss->af_rbatch_nload,
						  &gtss->af_rbatch_nskip);
}

static void
ArrowInitializeDSMForeignScan(ForeignScanState *node,
							  ParallelContext *pcxt,
							  void *coordinate)
{
	pg_atomic_uint32 *atomic_buffer = coordinate;

	__ExecInitDSMArrowFdw((ArrowFdwState *)node->fdw_state,
						  atomic_buffer,
						  atomic_buffer + 1,
						  atomic_buffer + 2);
}

/*
 * ArrowReInitializeDSMForeignScan
 */
static void
__ExecReInitDSMArrowFdw(ArrowFdwState *af_state)
{
	pg_atomic_write_u32(af_state->rbatch_index, 0);
}

void
ExecReInitDSMArrowFdw(ArrowFdwState *af_state)
{
	__ExecReInitDSMArrowFdw(af_state);
}


static void
ArrowReInitializeDSMForeignScan(ForeignScanState *node,
								ParallelContext *pcxt,
								void *coordinate)
{
	__ExecReInitDSMArrowFdw((ArrowFdwState *)node->fdw_state);
}

/*
 * ArrowInitializeWorkerForeignScan
 */
static inline void
__ExecInitWorkerArrowFdw(ArrowFdwState *af_state,
						 pg_atomic_uint32 *rbatch_index,
						 pg_atomic_uint32 *rbatch_nload,
						 pg_atomic_uint32 *rbatch_nskip)
{
	af_state->rbatch_index = rbatch_index;
	af_state->rbatch_nload = rbatch_nload;
	af_state->rbatch_nskip = rbatch_nskip;
}

void
ExecInitWorkerArrowFdw(ArrowFdwState *af_state,
					   GpuTaskSharedState *gtss)
{
	__ExecInitWorkerArrowFdw(af_state,
							 &gtss->af_rbatch_index,
							 &gtss->af_rbatch_nload,
							 &gtss->af_rbatch_nskip);
}

static void
ArrowInitializeWorkerForeignScan(ForeignScanState *node,
								 shm_toc *toc,
								 void *coordinate)
{
	pg_atomic_uint32 *atomic_buffer = coordinate;

	__ExecInitWorkerArrowFdw((ArrowFdwState *)node->fdw_state,
							 atomic_buffer,
							 atomic_buffer + 1,
							 atomic_buffer + 2);
}

/*
 * ArrowShutdownForeignScan
 */
static inline void
__ExecShutdownArrowFdw(ArrowFdwState *af_state)
{
	uint32		temp;

	temp = pg_atomic_read_u32(af_state->rbatch_index);
	pg_atomic_write_u32(&af_state->__rbatch_index_local, temp);
	af_state->rbatch_index = &af_state->__rbatch_index_local;

	temp = pg_atomic_read_u32(af_state->rbatch_nload);
	pg_atomic_write_u32(&af_state->__rbatch_nload_local, temp);
	af_state->rbatch_nload = &af_state->__rbatch_nload_local;

	temp = pg_atomic_read_u32(af_state->rbatch_nskip);
	pg_atomic_write_u32(&af_state->__rbatch_nskip_local, temp);
	af_state->rbatch_nskip = &af_state->__rbatch_nskip_local;
}

void
ExecShutdownArrowFdw(ArrowFdwState *af_state)
{
	__ExecShutdownArrowFdw(af_state);
}

static void
ArrowShutdownForeignScan(ForeignScanState *node)
{
	__ExecShutdownArrowFdw((ArrowFdwState *)node->fdw_state);
}

/*
 * handler of Arrow_Fdw
 */
Datum
pgstrom_arrow_fdw_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&pgstrom_arrow_fdw_routine);
}
PG_FUNCTION_INFO_V1(pgstrom_arrow_fdw_handler);



static const char *
arrowTypeToPGTypeName(ArrowField *field)
{
	Oid			typoid;
	int			typmod;
	HeapTuple	tup;
	Form_pg_type type;
	char	   *schema;
	char	   *result;

	typoid = arrowTypeToPGTypeOid(field, &typmod);
	if (!OidIsValid(typoid))
		return NULL;
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typoid);
	type = (Form_pg_type) GETSTRUCT(tup);
	schema = get_namespace_name(type->typnamespace);
	if (typmod < 0)
		result = psprintf("%s.%s",
						  quote_identifier(schema),
						  quote_identifier(NameStr(type->typname)));
	else
		result = psprintf("%s.%s(%d)",
						  quote_identifier(schema),
						  quote_identifier(NameStr(type->typname)),
						  typmod);
	ReleaseSysCache(tup);

	return result;
}

#if 0
//no longer needed?

/*
 * arrowTypeIsConvertible
 */
static bool
arrowTypeIsConvertible(Oid type_oid, int typemod)
{
	HeapTuple		tup;
	Form_pg_type	typeForm;
	bool			retval = false;

	switch (type_oid)
	{
		case INT1OID:		/* Int8 */
		case INT2OID:		/* Int16 */
		case INT4OID:		/* Int32 */
		case INT8OID:		/* Int64 */
		case FLOAT2OID:		/* FP16 */
		case FLOAT4OID:		/* FP32 */
		case FLOAT8OID:		/* FP64 */
		case TEXTOID:		/* Utf8 */
		case BYTEAOID:		/* Binary */
		case BOOLOID:		/* Bool */
		case NUMERICOID:	/* Decimal */
		case DATEOID:		/* Date */
		case TIMEOID:		/* Time */
		case TIMESTAMPOID:	/* Timestamp */
		case TIMESTAMPTZOID:/* TimestampTz */
		case INTERVALOID:	/* Interval */
		case BPCHAROID:		/* FixedSizeBinary */
			return true;
		default:
			tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
			if (!HeapTupleIsValid(tup))
				elog(ERROR, "cache lookup failed for type %u", type_oid);
			typeForm = (Form_pg_type) GETSTRUCT(tup);

			if (OidIsValid(typeForm->typelem) && typeForm->typlen == -1)
			{
				retval = arrowTypeIsConvertible(typeForm->typelem, typemod);
			}
			else if (typeForm->typtype == TYPTYPE_COMPOSITE)
			{
				Relation	rel;
				TupleDesc	tupdesc;
				int			j;

				rel = relation_open(typeForm->typrelid, AccessShareLock);
				tupdesc = RelationGetDescr(rel);
				for (j=0; j < tupdesc->natts; j++)
				{
					Form_pg_attribute	attr = tupleDescAttr(tupdesc, j);

					if (!arrowTypeIsConvertible(attr->atttypid,
												attr->atttypmod))
						break;
				}
				if (j >= tupdesc->natts)
					retval = true;
				relation_close(rel, AccessShareLock);
			}
			ReleaseSysCache(tup);
	}
	return retval;
}
#endif


/*
 * arrowSchemaCompatibilityCheck
 */
static bool
__arrowSchemaCompatibilityCheck(TupleDesc tupdesc,
								RecordBatchFieldState *rb_fstate)
{
	int		j;

	for (j=0; j < tupdesc->natts; j++)
	{
		RecordBatchFieldState *fstate = &rb_fstate[j];
		Form_pg_attribute attr = tupleDescAttr(tupdesc, j);

		if (!fstate->children)
		{
			/* shortcut, it should be a scalar built-in type */
			Assert(fstate->num_children == 0);
			if (attr->atttypid != fstate->atttypid)
				return false;
		}
		else
		{
			Form_pg_type	typ;
			HeapTuple		tup;
			bool			type_is_ok = true;

			tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(attr->atttypid));
			if (!HeapTupleIsValid(tup))
				elog(ERROR, "cache lookup failed for type %u", attr->atttypid);
			typ = (Form_pg_type) GETSTRUCT(tup);
			if (OidIsValid(typ->typelem) && typ->typlen == -1 &&
				fstate->num_children == 1)
			{
				/* Arrow::List */
				RecordBatchFieldState *cstate = &fstate->children[0];

				if (typ->typelem == cstate->atttypid)
				{
					/*
					 * overwrite typoid / typmod because a same arrow file
					 * can be reused, and it may be on behalf of different
					 * user defined data type.
					 */
					fstate->atttypid = attr->atttypid;
					fstate->atttypmod = attr->atttypmod;
				}
				else
				{
					type_is_ok = false;
				}
			}
			else if (typ->typlen == -1 && OidIsValid(typ->typrelid))
			{
				/* Arrow::Struct */
				TupleDesc	sdesc = lookup_rowtype_tupdesc(attr->atttypid,
														   attr->atttypmod);
				if (sdesc->natts == fstate->num_children &&
					__arrowSchemaCompatibilityCheck(sdesc, fstate->children))
				{
					/* see comment above */
					fstate->atttypid = attr->atttypid;
					fstate->atttypmod = attr->atttypmod;
				}
				else
				{
					type_is_ok = false;
				}
				DecrTupleDescRefCount(sdesc);

			}
			else
			{
				/* unknown */
				type_is_ok = false;
			}
			ReleaseSysCache(tup);
			if (!type_is_ok)
				return false;
		}
	}
	return true;
}

static bool
arrowSchemaCompatibilityCheck(TupleDesc tupdesc, RecordBatchState *rb_state)
{
	if (tupdesc->natts != rb_state->ncols)
		return false;
	return __arrowSchemaCompatibilityCheck(tupdesc, rb_state->columns);
}

/*
 * pg_XXX_arrow_ref
 */
static Datum
pg_varlena_arrow_ref(kern_data_store *kds,
					 kern_colmeta *cmeta, size_t index)
{
	cl_uint	   *offset = (cl_uint *)
		((char *)kds + __kds_unpack(cmeta->values_offset));
	char	   *extra = (char *)kds + __kds_unpack(cmeta->extra_offset);
	cl_uint		len;
	struct varlena *res;

	if (sizeof(uint32) * (index+2) > __kds_unpack(cmeta->values_length))
		elog(ERROR, "corruption? varlena index out of range");
	len = offset[index+1] - offset[index];
	if (offset[index] > offset[index+1] ||
		offset[index+1] > __kds_unpack(cmeta->extra_length))
		elog(ERROR, "corruption? varlena points out of extra buffer");

	res = palloc(VARHDRSZ + len);
	SET_VARSIZE(res, VARHDRSZ + len);
	memcpy(VARDATA(res), extra + offset[index], len);

	return PointerGetDatum(res);
}

static Datum
pg_bpchar_arrow_ref(kern_data_store *kds,
					kern_colmeta *cmeta, size_t index)
{
	cl_char	   *values = ((char *)kds + __kds_unpack(cmeta->values_offset));
	size_t		length = __kds_unpack(cmeta->values_length);
	cl_int		unitsz = cmeta->atttypmod - VARHDRSZ;
	struct varlena *res;

	if (unitsz <= 0)
		elog(ERROR, "CHAR(%d) is not expected", unitsz);
	if (unitsz * index >= length)
		elog(ERROR, "corruption? bpchar points out of range");
	res = palloc(VARHDRSZ + unitsz);
	memcpy((char *)res + VARHDRSZ, values + unitsz * index, unitsz);
	SET_VARSIZE(res, VARHDRSZ + unitsz);

	return PointerGetDatum(res);
}

static Datum
pg_bool_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	uint8  *bitmap = (uint8 *)kds + __kds_unpack(cmeta->values_offset);
	size_t	length = __kds_unpack(cmeta->values_length);
	uint8	mask = (1 << (index & 7));

	index >>= 3;
	if (sizeof(uint8) * index >= length)
		elog(ERROR, "corruption? bool points out of range");
	return BoolGetDatum((bitmap[index] & mask) != 0 ? true : false);
}

static Datum
pg_int1_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	int8   *values = (int8 *)((char *)kds + __kds_unpack(cmeta->values_offset));
	size_t	length = __kds_unpack(cmeta->values_length);

	if (sizeof(int8) * index >= length)
		elog(ERROR, "corruption? int8 points out of range");
	return values[index];
}

static Datum
pg_int2_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	int16  *values = (int16 *)((char *)kds + __kds_unpack(cmeta->values_offset));
	size_t	length = __kds_unpack(cmeta->values_length);

	if (sizeof(int16) * index >= length)
		elog(ERROR, "corruption? int16 points out of range");
	return values[index];
}

static Datum
pg_int4_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	int32  *values = (int32 *)((char *)kds + __kds_unpack(cmeta->values_offset));
	size_t  length = __kds_unpack(cmeta->values_length);

	if (sizeof(int32) * index >= length)
		elog(ERROR, "corruption? int32 points out of range");
	return values[index];
}

static Datum
pg_int8_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	int64  *values = (int64 *)((char *)kds + __kds_unpack(cmeta->values_offset));
	size_t	length = __kds_unpack(cmeta->values_length);

	if (sizeof(int64) * index >= length)
		elog(ERROR, "corruption? int64 points out of range");
	return values[index];
}

static Datum
pg_numeric_arrow_ref(kern_data_store *kds,
					 kern_colmeta *cmeta, size_t index)
{
	char	   *result = palloc0(sizeof(struct NumericData));
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	size_t		length = __kds_unpack(cmeta->values_length);
	int			dscale = cmeta->attopts.decimal.scale;
	Int128_t	decimal;

	if (sizeof(int128) * index >= length)
		elog(ERROR, "corruption? numeric points out of range");
	decimal.ival = ((int128 *)base)[index];

	while (dscale > 0 && decimal.ival % 10 == 0)
	{
		decimal.ival /= 10;
		dscale--;
	}
	pg_numeric_to_varlena(result, dscale, decimal);

	return PointerGetDatum(result);
}

static Datum
pg_date_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	size_t		length = __kds_unpack(cmeta->values_length);
	DateADT		dt;

	switch (cmeta->attopts.date.unit)
	{
		case ArrowDateUnit__Day:
			if (sizeof(uint32) * index >= length)
				elog(ERROR, "corruption? Date[day] points out of range");
			dt = ((uint32 *)base)[index];
			break;
		case ArrowDateUnit__MilliSecond:
			if (sizeof(uint64) * index >= length)
				elog(ERROR, "corruption? Date[ms] points out of range");
			dt = ((uint64 *)base)[index] / 1000;
			break;
		default:
			elog(ERROR, "Bug? unexpected unit of Date type");
	}
	/* convert UNIX epoch to PostgreSQL epoch */
	dt -= (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE);
	return DateADTGetDatum(dt);
}

static Datum
pg_time_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	size_t		length = __kds_unpack(cmeta->values_length);
	TimeADT		tm;

	switch (cmeta->attopts.time.unit)
	{
		case ArrowTimeUnit__Second:
			if (sizeof(uint32) * index >= length)
				elog(ERROR, "corruption? Time[sec] points out of range");
			tm = ((uint32 *)base)[index] * 1000000L;
			break;
		case ArrowTimeUnit__MilliSecond:
			if (sizeof(uint32) * index >= length)
				elog(ERROR, "corruption? Time[ms] points out of range");
			tm = ((uint32 *)base)[index] * 1000L;
			break;
		case ArrowTimeUnit__MicroSecond:
			if (sizeof(uint64) * index >= length)
				elog(ERROR, "corruption? Time[us] points out of range");
			tm = ((uint64 *)base)[index];
			break;
		case ArrowTimeUnit__NanoSecond:
			if (sizeof(uint64) * index >= length)
				elog(ERROR, "corruption? Time[ns] points out of range");
			tm = ((uint64 *)base)[index] / 1000L;
			break;
		default:
			elog(ERROR, "Bug? unexpected unit of Time type");
			break;
	}
	return TimeADTGetDatum(tm);
}

static Datum
pg_timestamp_arrow_ref(kern_data_store *kds,
					   kern_colmeta *cmeta, size_t index)
{
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	size_t		length = __kds_unpack(cmeta->values_length);
	Timestamp	ts;

	switch (cmeta->attopts.timestamp.unit)
	{
		case ArrowTimeUnit__Second:
			if (sizeof(uint64) * index >= length)
				elog(ERROR, "corruption? Timestamp[sec] points out of range");
			ts = ((uint64 *)base)[index] * 1000000UL;
			break;
		case ArrowTimeUnit__MilliSecond:
			if (sizeof(uint64) * index >= length)
				elog(ERROR, "corruption? Timestamp[ms] points out of range");
			ts = ((uint64 *)base)[index] * 1000UL;
			break;
		case ArrowTimeUnit__MicroSecond:
			if (sizeof(uint64) * index >= length)
				elog(ERROR, "corruption? Timestamp[us] points out of range");
			ts = ((uint64 *)base)[index];
			break;
		case ArrowTimeUnit__NanoSecond:
			if (sizeof(uint64) * index >= length)
				elog(ERROR, "corruption? Timestamp[ns] points out of range");
			ts = ((uint64 *)base)[index] / 1000UL;
			break;
		default:
			elog(ERROR, "Bug? unexpected unit of Timestamp type");
			break;
	}
	/* convert UNIX epoch to PostgreSQL epoch */
	ts -= (POSTGRES_EPOCH_JDATE -
		   UNIX_EPOCH_JDATE) * USECS_PER_DAY;
	return TimestampGetDatum(ts);
}

static Datum
pg_interval_arrow_ref(kern_data_store *kds,
					  kern_colmeta *cmeta, size_t index)
{
	char	   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	size_t		length = __kds_unpack(cmeta->values_length);
	Interval   *iv = palloc0(sizeof(Interval));

	switch (cmeta->attopts.interval.unit)
	{
		case ArrowIntervalUnit__Year_Month:
			/* 32bit: number of months */
			if (sizeof(uint32) * index >= length)
				elog(ERROR, "corruption? Interval[Year/Month] points out of range");
			iv->month = ((uint32 *)base)[index];
			break;
		case ArrowIntervalUnit__Day_Time:
			/* 32bit+32bit: number of days and milliseconds */
			if (2 * sizeof(uint32) * index >= length)
				elog(ERROR, "corruption? Interval[Day/Time] points out of range");
			iv->day  = ((int32 *)base)[2 * index];
			iv->time = ((int32 *)base)[2 * index + 1] * 1000;
			break;
		default:
			elog(ERROR, "Bug? unexpected unit of Interval type");
	}
	return PointerGetDatum(iv);
}

static Datum
pg_macaddr_arrow_ref(kern_data_store *kds,
					 kern_colmeta *cmeta, size_t index)
{
	char   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	size_t	length = __kds_unpack(cmeta->values_length);

	if (cmeta->attopts.fixed_size_binary.byteWidth != sizeof(macaddr))
		elog(ERROR, "Bug? wrong FixedSizeBinary::byteWidth(%d) for macaddr",
			 cmeta->attopts.fixed_size_binary.byteWidth);
	if (sizeof(macaddr) * index >= length)
		elog(ERROR, "corruption? Binary[macaddr] points out of range");

	return PointerGetDatum(base + sizeof(macaddr) * index);
}

static Datum
pg_inet_arrow_ref(kern_data_store *kds,
				  kern_colmeta *cmeta, size_t index)
{
	char   *base = (char *)kds + __kds_unpack(cmeta->values_offset);
	size_t	length = __kds_unpack(cmeta->values_length);
	inet   *ip = palloc(sizeof(inet));

	if (cmeta->attopts.fixed_size_binary.byteWidth == 4)
	{
		if (4 * index >= length)
			elog(ERROR, "corruption? Binary[inet4] points out of range");
		ip->inet_data.family = PGSQL_AF_INET;
		ip->inet_data.bits = 32;
		memcpy(ip->inet_data.ipaddr, base + 4 * index, 4);
	}
	else if (cmeta->attopts.fixed_size_binary.byteWidth == 16)
	{
		if (16 * index >= length)
			elog(ERROR, "corruption? Binary[inet6] points out of range");
		ip->inet_data.family = PGSQL_AF_INET6;
		ip->inet_data.bits = 128;
		memcpy(ip->inet_data.ipaddr, base + 16 * index, 16);
	}
	else
		elog(ERROR, "Bug? wrong FixedSizeBinary::byteWidth(%d) for inet",
			 cmeta->attopts.fixed_size_binary.byteWidth);

	SET_INET_VARSIZE(ip);
	return PointerGetDatum(ip);
}

static Datum
pg_array_arrow_ref(kern_data_store *kds,
				   kern_colmeta *smeta,
				   cl_uint start, cl_uint end)
{
	ArrayType  *res;
	size_t		sz;
	cl_uint		i, nitems = end - start;
	bits8	   *nullmap = NULL;
	size_t		usage, __usage;

	/* sanity checks */
	if (start > end)
		elog(ERROR, "Bug? array index has reversed order [%u..%u]", start, end);

	/* allocation of the result buffer */
	if (smeta->nullmap_offset != 0)
		sz = ARR_OVERHEAD_WITHNULLS(1, nitems);
	else
		sz = ARR_OVERHEAD_NONULLS(1);

	if (smeta->attlen > 0)
	{
		sz += TYPEALIGN(smeta->attalign,
						smeta->attlen) * nitems;
	}
	else if (smeta->attlen == -1)
	{
		sz += 400;		/* tentative allocation */
	}
	else
		elog(ERROR, "Bug? corrupted kernel column metadata");

	res = palloc0(sz);
	res->ndim = 1;
	if (smeta->nullmap_offset != 0)
	{
		res->dataoffset = ARR_OVERHEAD_WITHNULLS(1, nitems);
		nullmap = ARR_NULLBITMAP(res);
	}
	res->elemtype = smeta->atttypid;
	ARR_DIMS(res)[0] = nitems;
	ARR_LBOUND(res)[0] = 1;
	usage = ARR_DATA_OFFSET(res);
	for (i=0; i < nitems; i++)
	{
		Datum	datum;
		bool	isnull;

		pg_datum_arrow_ref(kds, smeta, start+i, &datum, &isnull);
		if (isnull)
		{
			if (!nullmap)
				elog(ERROR, "Bug? element item should not be NULL");
		}
		else if (smeta->attlen > 0)
		{
			if (nullmap)
				nullmap[i>>3] |= (1<<(i&7));
			__usage = TYPEALIGN(smeta->attalign, usage);
			while (__usage + smeta->attlen > sz)
			{
				sz += sz;
				res = repalloc(res, sz);
			}
			if (__usage > usage)
				memset((char *)res + usage, 0, __usage - usage);
			memcpy((char *)res + __usage, &datum, smeta->attlen);
			usage = __usage + smeta->attlen;
		}
		else if (smeta->attlen == -1)
		{
			cl_int		vl_len = VARSIZE(datum);

			if (nullmap)
				nullmap[i>>3] |= (1<<(i&7));
			__usage = TYPEALIGN(smeta->attalign, usage);
			while (__usage + vl_len > sz)
			{
				sz += sz;
				res = repalloc(res, sz);
			}
			if (__usage > usage)
				memset((char *)res + usage, 0, __usage - usage);
			memcpy((char *)res + __usage, DatumGetPointer(datum), vl_len);
			usage = __usage + vl_len;

			pfree(DatumGetPointer(datum));
		}
		else
			elog(ERROR, "Bug? corrupted kernel column metadata");
	}
	SET_VARSIZE(res, usage);

	return PointerGetDatum(res);
}

/*
 * pg_datum_arrow_ref
 */
static void
pg_datum_arrow_ref(kern_data_store *kds,
				   kern_colmeta *cmeta,
				   size_t index,
				   Datum *p_datum,
				   bool *p_isnull)
{
	Datum		datum = 0;
	bool		isnull = true;

	if (cmeta->nullmap_offset != 0)
	{
		size_t	nullmap_offset = __kds_unpack(cmeta->nullmap_offset);
		uint8  *nullmap = (uint8 *)kds + nullmap_offset;

		if (att_isnull(index, nullmap))
			goto out;
	}

	if (cmeta->atttypkind == TYPE_KIND__ARRAY)
	{
		/* array type */
		kern_colmeta   *smeta;
		uint32		   *offset;

		if (cmeta->num_subattrs != 1 ||
			cmeta->idx_subattrs < kds->ncols ||
			cmeta->idx_subattrs >= kds->nr_colmeta)
			elog(ERROR, "Bug? corrupted kernel column metadata");
		if (sizeof(uint32) * (index+2) > __kds_unpack(cmeta->values_length))
			elog(ERROR, "Bug? array index is out of range");
		smeta = &kds->colmeta[cmeta->idx_subattrs];
		offset = (uint32 *)((char *)kds + __kds_unpack(cmeta->values_offset));
		datum = pg_array_arrow_ref(kds, smeta,
								   offset[index],
								   offset[index+1]);
		isnull = false;
	}
	else if (cmeta->atttypkind == TYPE_KIND__COMPOSITE)
	{
		/* composite type */
		TupleDesc	tupdesc = lookup_rowtype_tupdesc(cmeta->atttypid, -1);
		Datum	   *sub_values = alloca(sizeof(Datum) * tupdesc->natts);
		bool	   *sub_isnull = alloca(sizeof(bool)  * tupdesc->natts);
		HeapTuple	htup;
		int			j;

		if (tupdesc->natts != cmeta->num_subattrs)
			elog(ERROR, "Struct definition is conrrupted?");
		if (cmeta->idx_subattrs < kds->ncols ||
			cmeta->idx_subattrs + cmeta->num_subattrs > kds->nr_colmeta)
			elog(ERROR, "Bug? strange kernel column metadata");

		for (j=0; j < tupdesc->natts; j++)
		{
			kern_colmeta *sub_meta = &kds->colmeta[cmeta->idx_subattrs + j];

			pg_datum_arrow_ref(kds, sub_meta, index,
							   sub_values + j,
							   sub_isnull + j);
		}
		htup = heap_form_tuple(tupdesc, sub_values, sub_isnull);

		ReleaseTupleDesc(tupdesc);

		datum = PointerGetDatum(htup->t_data);
		isnull = false;
	}
	else if (cmeta->atttypkind != TYPE_KIND__NULL)
	{
		/* anything else, except for unreferenced column */
		int		i;

		switch (cmeta->atttypid)
		{
			case INT1OID:
				datum = pg_int1_arrow_ref(kds, cmeta, index);
				break;
			case INT2OID:
			case FLOAT2OID:
				datum = pg_int2_arrow_ref(kds, cmeta, index);
				break;
			case INT4OID:
			case FLOAT4OID:
				datum = pg_int4_arrow_ref(kds, cmeta, index);
				break;
			case INT8OID:
			case FLOAT8OID:
				datum = pg_int8_arrow_ref(kds, cmeta, index);
				break;
			case TEXTOID:
			case BYTEAOID:
				datum = pg_varlena_arrow_ref(kds, cmeta, index);
				break;
			case BPCHAROID:
				datum = pg_bpchar_arrow_ref(kds, cmeta, index);
				break;
			case BOOLOID:
				datum = pg_bool_arrow_ref(kds, cmeta, index);
				break;
			case NUMERICOID:
				datum = pg_numeric_arrow_ref(kds, cmeta, index);
				break;
			case DATEOID:
				datum = pg_date_arrow_ref(kds, cmeta, index);
				break;
			case TIMEOID:
				datum = pg_time_arrow_ref(kds, cmeta, index);
				break;
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				datum = pg_timestamp_arrow_ref(kds, cmeta, index);
				break;
			case INTERVALOID:
				datum = pg_interval_arrow_ref(kds, cmeta, index);
				break;
			case MACADDROID:
				datum = pg_macaddr_arrow_ref(kds, cmeta, index);
				break;
			case INETOID:
				datum = pg_inet_arrow_ref(kds, cmeta, index);
				break;
			default:
				for (i=0; i < pgstrom_num_users_extra; i++)
				{
					pgstromUsersExtraDescriptor *extra = &pgstrom_users_extra_desc[i];

					if (extra->arrow_datum_ref &&
						extra->arrow_datum_ref(kds, cmeta, index, &datum, &isnull))
					{
						goto out;
					}
				}
				elog(ERROR, "Bug? unexpected datum type: %u", cmeta->atttypid);
				break;
		}
		isnull = false;
	}
out:
	*p_datum  = datum;
	*p_isnull = isnull;
}

/*
 * KDS_fetch_tuple_arrow
 */
bool
KDS_fetch_tuple_arrow(TupleTableSlot *slot,
					  kern_data_store *kds,
					  size_t index)
{
	int		j;

	if (index >= kds->nitems)
		return false;
	ExecStoreAllNullTuple(slot);
	for (j=0; j < kds->ncols; j++)
	{
		kern_colmeta   *cmeta = &kds->colmeta[j];

		pg_datum_arrow_ref(kds, cmeta,
						   index,
						   slot->tts_values + j,
						   slot->tts_isnull + j);
	}
	return true;
}

/*
 * validator of Arrow_Fdw
 */
Datum
pgstrom_arrow_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);

	if (catalog == ForeignTableRelationId)
	{
		List	   *filesList;
		ListCell   *lc;

		filesList = arrowFdwExtractFilesList(options_list);
		foreach (lc, filesList)
		{
			ArrowFileInfo	af_info;
			const char	   *fname = strVal(lfirst(lc));

			readArrowFile(fname, &af_info, true);
		}
	}
	else if (options_list != NIL)
	{
		const char	   *label;
		char			temp[80];

		switch (catalog)
		{
			case ForeignDataWrapperRelationId:
				label = "FOREIGN DATA WRAPPER";
				break;
			case ForeignServerRelationId:
				label = "SERVER";
				break;
			case UserMappingRelationId:
				label = "USER MAPPING";
				break;
			case AttributeRelationId:
				label = "attribute of FOREIGN TABLE";
				break;
			default:
				snprintf(temp, sizeof(temp),
						 "[unexpected object catalog=%u]", catalog);
				label = temp;
				break;
		}
		elog(ERROR, "Arrow_Fdw does not support any options for %s", label);
	}
	PG_RETURN_VOID();
}
PG_FUNCTION_INFO_V1(pgstrom_arrow_fdw_validator);

/*
 * pgstrom_arrow_fdw_precheck_schema
 */
static void
arrow_fdw_precheck_schema(Relation rel)
{
	TupleDesc		tupdesc = RelationGetDescr(rel);
	ForeignTable   *ft = GetForeignTable(RelationGetRelid(rel));
	List		   *filesList;
	ListCell	   *lc;
	bool			writable;
#if 0
	int				j;

	/* check schema definition is supported by Apache Arrow */
	for (j=0; j < tupdesc->natts; j++)
	{
		Form_pg_attribute	attr = tupleDescAttr(tupdesc, j);

		if (!arrowTypeIsConvertible(attr->atttypid,
									attr->atttypmod))
			elog(ERROR, "column %s of foreign table %s has %s type that is not convertible any supported Apache Arrow types",
				 NameStr(attr->attname),
				 RelationGetRelationName(rel),
				 format_type_be(attr->atttypid));
	}
#endif
	filesList = __arrowFdwExtractFilesList(ft->options,
										   NULL,
										   &writable);
	foreach (lc, filesList)
	{
		const char *fname = strVal(lfirst(lc));
		File		filp;
		List	   *rb_cached = NIL;
		ListCell   *cell;

		filp = PathNameOpenFile(fname, O_RDONLY | PG_BINARY);
		if (filp < 0)
		{
			if (writable && errno == ENOENT)
				continue;
			elog(ERROR, "failed to open '%s' on behalf of '%s': %m",
				 fname, RelationGetRelationName(rel));
		}
		/* check schema compatibility */
		rb_cached = arrowLookupOrBuildMetadataCache(filp, NULL);
		foreach (cell, rb_cached)
		{
			RecordBatchState *rb_state = lfirst(cell);

			if (!arrowSchemaCompatibilityCheck(tupdesc, rb_state))
				elog(ERROR, "arrow file '%s' on behalf of the foreign table '%s' has incompatible schema definition",
					 fname, RelationGetRelationName(rel));
		}
		list_free(rb_cached);
	}
}

Datum
pgstrom_arrow_fdw_precheck_schema(PG_FUNCTION_ARGS)
{
	EventTriggerData   *trigdata;
	
	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		elog(ERROR, "%s: must be called as EventTrigger",
			 __FUNCTION__);
	trigdata = (EventTriggerData *) fcinfo->context;
	if (strcmp(trigdata->event, "ddl_command_end") != 0)
		elog(ERROR, "%s: must be called on ddl_command_end event",
			 __FUNCTION__);
	if (strcmp(GetCommandTagName(trigdata->tag),
			   "CREATE FOREIGN TABLE") == 0)
	{
		CreateStmt	   *stmt = (CreateStmt *)trigdata->parsetree;
		Relation		rel;

		rel = relation_openrv_extended(stmt->relation, AccessShareLock, true);
		if (!rel)
			PG_RETURN_NULL();
		if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE &&
			GetFdwRoutineForRelation(rel, false) == &pgstrom_arrow_fdw_routine)
		{
			arrow_fdw_precheck_schema(rel);
		}
		relation_close(rel, AccessShareLock);
	}
	else if (strcmp(GetCommandTagName(trigdata->tag),
					"ALTER FOREIGN TABLE") == 0 &&
			 IsA(trigdata->parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *)trigdata->parsetree;
		Relation		rel;
		ListCell	   *lc;
		bool			has_schema_change = false;

		rel = relation_openrv_extended(stmt->relation, AccessShareLock, true);
		if (!rel)
			PG_RETURN_NULL();
		if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE &&
			GetFdwRoutineForRelation(rel, false) == &pgstrom_arrow_fdw_routine)
		{
			foreach (lc, stmt->cmds)
			{
				AlterTableCmd  *cmd = lfirst(lc);

				if (cmd->subtype == AT_AddColumn ||
					cmd->subtype == AT_DropColumn ||
					cmd->subtype == AT_AlterColumnType)
				{
					has_schema_change = true;
					break;
				}
			}
			if (has_schema_change)
				arrow_fdw_precheck_schema(rel);
		}
		relation_close(rel, AccessShareLock);
	}
	PG_RETURN_NULL();
}
PG_FUNCTION_INFO_V1(pgstrom_arrow_fdw_precheck_schema);

/*
 * arrowInvalidateMetadataCache
 *
 * NOTE: caller must have lock_slots[] with EXCLUSIVE mode
 */
static uint64
__arrowInvalidateMetadataCache(arrowMetadataCache *mcache, bool detach_lru)
{
	arrowMetadataCache *mtemp;
	dlist_node	   *dnode;
	uint64			released = 0;

	while (!dlist_is_empty(&mcache->siblings))
	{
		dnode = dlist_pop_head_node(&mcache->siblings);
		mtemp = dlist_container(arrowMetadataCache, chain, dnode);
		Assert(dlist_is_empty(&mtemp->siblings) &&
			   !mtemp->lru_chain.prev && !mtemp->lru_chain.next);
		dlist_delete(&mtemp->chain);
		released += MAXALIGN(offsetof(arrowMetadataCache,
									  fstate[mtemp->nfields]));
		pfree(mtemp);
	}
	released += MAXALIGN(offsetof(arrowMetadataCache,
								  fstate[mcache->nfields]));
	if (detach_lru)
	{
		SpinLockAcquire(&arrow_metadata_state->lru_lock);
		dlist_delete(&mcache->lru_chain);
		SpinLockRelease(&arrow_metadata_state->lru_lock);
	}
	dlist_delete(&mcache->chain);
	pfree(mcache);

	return pg_atomic_sub_fetch_u64(&arrow_metadata_state->consumed, released);
}

static void
arrowInvalidateMetadataCache(MetadataCacheKey *mkey, bool detach_lru)
{
	dlist_mutable_iter miter;
	int		index = mkey->hash % ARROW_METADATA_HASH_NSLOTS;

	dlist_foreach_modify(miter, &arrow_metadata_state->hash_slots[index])
	{
		arrowMetadataCache *mcache
			= dlist_container(arrowMetadataCache, chain, miter.cur);

		if (mcache->stat_buf.st_dev == mkey->st_dev &&
			mcache->stat_buf.st_ino == mkey->st_ino)
		{
			elog(DEBUG2, "arrow_fdw: metadata cache invalidation for the file (st_dev=%lu/st_ino=%lu)",
				 mkey->st_dev, mkey->st_ino);
			__arrowInvalidateMetadataCache(mcache, true);
		}
	}
}

/*
 * copyMetadataFieldCache - copy for nested structure
 */
static int
copyMetadataFieldCache(RecordBatchFieldState *dest_curr,
					   RecordBatchFieldState *dest_tail,
					   int nattrs,
					   RecordBatchFieldState *columns,
					   Bitmapset **p_stat_attrs)
{
	RecordBatchFieldState *dest_next = dest_curr + nattrs;
	int		j, k, nslots = nattrs;

	if (dest_next > dest_tail)
		return -1;

	for (j=0; j < nattrs; j++)
	{
		RecordBatchFieldState *__dest = dest_curr + j;
		RecordBatchFieldState *__orig = columns + j;

		memcpy(__dest, __orig, sizeof(RecordBatchFieldState));
		if (__dest->num_children == 0)
			Assert(__dest->children == NULL);
		else
		{
			__dest->children = dest_next;
			k = copyMetadataFieldCache(dest_next,
									   dest_tail,
									   __orig->num_children,
									   __orig->children,
									   NULL);
			if (k < 0)
				return -1;
			dest_next += k;
			nslots += k;
		}
		if (p_stat_attrs && !__orig->stat_isnull)
			*p_stat_attrs = bms_add_member(*p_stat_attrs, j+1);
	}
	return nslots;
}

/*
 * makeRecordBatchStateFromCache
 *   - setup RecordBatchState from arrowMetadataCache
 */
static RecordBatchState *
makeRecordBatchStateFromCache(arrowMetadataCache *mcache,
							  File fdesc,
							  Bitmapset **p_stat_attrs)
{
	RecordBatchState   *rbstate;

	rbstate = palloc0(offsetof(RecordBatchState,
							   columns[mcache->nfields]));
	rbstate->fdesc = fdesc;
	memcpy(&rbstate->stat_buf, &mcache->stat_buf, sizeof(struct stat));
	rbstate->rb_index  = mcache->rb_index;
	rbstate->rb_offset = mcache->rb_offset;
	rbstate->rb_length = mcache->rb_length;
	rbstate->rb_nitems = mcache->rb_nitems;
	rbstate->ncols = mcache->ncols;
	copyMetadataFieldCache(rbstate->columns,
						   rbstate->columns + mcache->nfields,
						   mcache->ncols,
						   mcache->fstate,
						   p_stat_attrs);
	return rbstate;
}

/*
 * arrowReclaimMetadataCache
 */
static void
arrowReclaimMetadataCache(void)
{
	arrowMetadataCache *mcache;
	LWLock	   *lock = NULL;
	dlist_node *dnode;
	uint32		lru_hash;
	uint32		lru_index;
	uint64		consumed;

	consumed = pg_atomic_read_u64(&arrow_metadata_state->consumed);
	if (consumed <= arrow_metadata_cache_size)
		return;

	SpinLockAcquire(&arrow_metadata_state->lru_lock);
	if (dlist_is_empty(&arrow_metadata_state->lru_list))
	{
		SpinLockRelease(&arrow_metadata_state->lru_lock);
		return;
	}
	dnode = dlist_tail_node(&arrow_metadata_state->lru_list);
	mcache = dlist_container(arrowMetadataCache, lru_chain, dnode);
	lru_hash = mcache->hash;
	SpinLockRelease(&arrow_metadata_state->lru_lock);

	do {
		lru_index = lru_hash % ARROW_METADATA_HASH_NSLOTS;
		lock = &arrow_metadata_state->lock_slots[lru_index];

		LWLockAcquire(lock, LW_EXCLUSIVE);
		SpinLockAcquire(&arrow_metadata_state->lru_lock);
		if (dlist_is_empty(&arrow_metadata_state->lru_list))
		{
			SpinLockRelease(&arrow_metadata_state->lru_lock);
			LWLockRelease(lock);
			break;
		}
		dnode = dlist_tail_node(&arrow_metadata_state->lru_list);
		mcache = dlist_container(arrowMetadataCache, lru_chain, dnode);
		if (mcache->hash == lru_hash)
		{
			dlist_delete(&mcache->lru_chain);
			memset(&mcache->lru_chain, 0, sizeof(dlist_node));
			SpinLockRelease(&arrow_metadata_state->lru_lock);
			consumed = __arrowInvalidateMetadataCache(mcache, false);
		}
		else
		{
			/* LRU-tail was referenced by someone, try again */
			lru_hash = mcache->hash;
            SpinLockRelease(&arrow_metadata_state->lru_lock);
		}
		LWLockRelease(lock);
	} while (consumed > arrow_metadata_cache_size);
}

/*
 * __arrowBuildMetadataCache
 *
 * NOTE: caller must have exclusive lock on arrow_metadata_state->lock_slots[]
 */
static arrowMetadataCache *
__arrowBuildMetadataCache(List *rb_state_list, uint32 hash)
{
	arrowMetadataCache *mcache = NULL;
	arrowMetadataCache *mtemp;
	dlist_node *dnode;
	Size		sz, consumed = 0;
	int			nfields;
	ListCell   *lc;

	foreach (lc, rb_state_list)
	{
		RecordBatchState *rbstate = lfirst(lc);

		if (!mcache)
			nfields = RecordBatchFieldCount(rbstate);
		else
			Assert(nfields == RecordBatchFieldCount(rbstate));

		sz = offsetof(arrowMetadataCache, fstate[nfields]);
		mtemp = MemoryContextAllocZero(TopSharedMemoryContext, sz);
		if (!mtemp)
		{
			/* !!out of memory!! */
			if (mcache)
			{
				while (!dlist_is_empty(&mcache->siblings))
				{
					dnode = dlist_pop_head_node(&mcache->siblings);
					mtemp = dlist_container(arrowMetadataCache,
											chain, dnode);
					pfree(mtemp);
				}
				pfree(mcache);
			}
			return NULL;
		}

		dlist_init(&mtemp->siblings);
		memcpy(&mtemp->stat_buf, &rbstate->stat_buf, sizeof(struct stat));
		mtemp->hash      = hash;
		mtemp->rb_index  = rbstate->rb_index;
        mtemp->rb_offset = rbstate->rb_offset;
        mtemp->rb_length = rbstate->rb_length;
        mtemp->rb_nitems = rbstate->rb_nitems;
        mtemp->ncols     = rbstate->ncols;
		mtemp->nfields   =
			copyMetadataFieldCache(mtemp->fstate,
								   mtemp->fstate + nfields,
								   rbstate->ncols,
								   rbstate->columns,
								   NULL);
		Assert(mtemp->nfields == nfields);

		if (!mcache)
			mcache = mtemp;
		else
			dlist_push_tail(&mcache->siblings, &mtemp->chain);
		consumed += MAXALIGN(sz);
	}
	pg_atomic_add_fetch_u64(&arrow_metadata_state->consumed, consumed);

	return mcache;
}

/*
 * arrowLookupOrBuildMetadataCache
 */
List *
arrowLookupOrBuildMetadataCache(File fdesc, Bitmapset **p_stat_attrs)
{
	MetadataCacheKey key;
	struct stat	stat_buf;
	uint32		index;
	LWLock	   *lock;
	dlist_head *hash_slot;
	dlist_head *mvcc_slot;
	dlist_iter	iter1, iter2;
	bool		has_exclusive = false;
	List	   *results = NIL;

	if (fstat(FileGetRawDesc(fdesc), &stat_buf) != 0)
		elog(ERROR, "failed on fstat('%s'): %m", FilePathName(fdesc));

	index = initMetadataCacheKey(&key, &stat_buf);
	lock = &arrow_metadata_state->lock_slots[index];
	hash_slot = &arrow_metadata_state->hash_slots[index];

	LWLockAcquire(lock, LW_SHARED);
retry:
	dlist_foreach(iter1, hash_slot)
	{
	   arrowMetadataCache *mcache
			= dlist_container(arrowMetadataCache, chain, iter1.cur);
		if (mcache->stat_buf.st_dev == stat_buf.st_dev &&
			mcache->stat_buf.st_ino == stat_buf.st_ino)
		{
			RecordBatchState *rbstate;

			Assert(mcache->hash == key.hash);
			if (timespec_comp(&mcache->stat_buf.st_mtim,
							  &stat_buf.st_mtim) < 0 ||
				timespec_comp(&mcache->stat_buf.st_ctim,
							  &stat_buf.st_ctim) < 0)
			{
				char	buf1[80], buf2[80], buf3[80], buf4[80];
				char   *tail;

				if (!has_exclusive)
				{
					LWLockRelease(lock);
					LWLockAcquire(lock, LW_EXCLUSIVE);
					has_exclusive = true;
					goto retry;
				}
				ctime_r(&mcache->stat_buf.st_mtime, buf1);
				ctime_r(&mcache->stat_buf.st_ctime, buf2);
				ctime_r(&stat_buf.st_mtime, buf3);
				ctime_r(&stat_buf.st_ctime, buf4);
				for (tail=buf1+strlen(buf1)-1; isspace(*tail); *tail--='\0');
				for (tail=buf2+strlen(buf2)-1; isspace(*tail); *tail--='\0');
				for (tail=buf3+strlen(buf3)-1; isspace(*tail); *tail--='\0');
				for (tail=buf4+strlen(buf4)-1; isspace(*tail); *tail--='\0');
				elog(DEBUG2, "arrow_fdw: metadata cache for '%s' (m:%s, c:%s) is older than the latest file (m:%s, c:%s), so invalidated",
					 FilePathName(fdesc), buf1, buf2, buf3, buf4);
				__arrowInvalidateMetadataCache(mcache, true);
				break;
			}
			/*
			 * Ok, arrow file metadata cache found and still valid
			 *
			 * NOTE: we currently support min/max statistics on the top-
			 * level variables only, not sub-field of the composite values.
			 */
			rbstate = makeRecordBatchStateFromCache(mcache, fdesc,
													p_stat_attrs);
			results = list_make1(rbstate);
			dlist_foreach (iter2, &mcache->siblings)
			{
				arrowMetadataCache *__mcache
					= dlist_container(arrowMetadataCache, chain, iter2.cur);
				rbstate = makeRecordBatchStateFromCache(__mcache, fdesc,
														p_stat_attrs);
				results = lappend(results, rbstate);
			}
			SpinLockAcquire(&arrow_metadata_state->lru_lock);
			dlist_move_head(&arrow_metadata_state->lru_list,
							&mcache->lru_chain);
			SpinLockRelease(&arrow_metadata_state->lru_lock);
			LWLockRelease(lock);

			return results;
		}
	}

	/*
	 * Hmm... no valid metadata cache was not found, so build a new entry
	 * under the exclusive lock on the arrow file.
	 */
	if (!has_exclusive)
	{
		LWLockRelease(lock);
		LWLockAcquire(lock, LW_EXCLUSIVE);
		has_exclusive = true;
		goto retry;
	}
	else
	{
		ArrowFileInfo	af_info;
		arrowMetadataCache *mcache;
		arrowStatsBinary *arrow_bstats;
		List		   *rb_state_any = NIL;

		readArrowFileDesc(FileGetRawDesc(fdesc), &af_info);
		if (af_info.dictionaries != NULL)
			elog(ERROR, "DictionaryBatch is not supported");
		Assert(af_info.footer._num_dictionaries == 0);

		if (af_info.recordBatches == NULL)
			elog(DEBUG2, "arrow file '%s' contains no RecordBatch",
				 FilePathName(fdesc));

		arrow_bstats = buildArrowStatsBinary(&af_info.footer, p_stat_attrs);
		for (index = 0; index < af_info.footer._num_recordBatches; index++)
		{
			RecordBatchState *rb_state;
			ArrowBlock       *block
				= &af_info.footer.recordBatches[index];
			ArrowRecordBatch *rbatch
			   = &af_info.recordBatches[index].body.recordBatch;

			rb_state = makeRecordBatchState(&af_info.footer.schema,
											block, rbatch);
			rb_state->fdesc = fdesc;
			memcpy(&rb_state->stat_buf, &stat_buf, sizeof(struct stat));
			rb_state->rb_index = index;

			if (arrow_bstats)
				applyArrowStatsBinary(rb_state, arrow_bstats);

			results = lappend(results, rb_state);
			rb_state_any = lappend(rb_state_any, rb_state);
		}
		releaseArrowStatsBinary(arrow_bstats);
		/* try to build a metadata cache for further references */
		mcache = __arrowBuildMetadataCache(rb_state_any, key.hash);
		if (mcache)
		{
			dlist_push_head(hash_slot, &mcache->chain);
			SpinLockAcquire(&arrow_metadata_state->lru_lock);
            dlist_push_head(&arrow_metadata_state->lru_list,
							&mcache->lru_chain);
			SpinLockRelease(&arrow_metadata_state->lru_lock);
		}
	}
	LWLockRelease(lock);
	/*
	 * reclaim unreferenced metadata cache entries based on LRU, if shared-
	 * memory consumption exceeds the configured threshold.
	 */
	arrowReclaimMetadataCache();

	return results;
}

#endif
