/*
 * xpu_opcodes.h
 *
 * collection of built-in xPU opcode
 * ----
 * Copyright 2011-2023 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2023 (C) PG-Strom Developers Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License.
 */

/*
 * PostgreSQL Device Types
 */
#ifndef TYPE_OPCODE
#define TYPE_OPCODE(NAME,OID,EXTENSION,FLAGS)
#endif
TYPE_OPCODE(bool, BOOLOID, NULL, 0)
TYPE_OPCODE(int1, INT1OID, "pg_strom", 0)
TYPE_OPCODE(int2, INT2OID, NULL, 0)
TYPE_OPCODE(int4, INT4OID, NULL, 0)
TYPE_OPCODE(int8, INT8OID, NULL, 0)
TYPE_OPCODE(float2, FLOAT2OID, "pg_strom", 0)
TYPE_OPCODE(float4, FLOAT4OID, NULL, 0)
TYPE_OPCODE(float8, FLOAT8OID, NULL, 0)
TYPE_OPCODE(numeric, NUMERICOID, NULL, DEVTYPE__USE_KVARS_SLOTBUF)
TYPE_OPCODE(bytea, BYTEAOID, NULL, 0)
TYPE_OPCODE(text, TEXTOID, NULL, 0)
//TYPE_OPCODE(varchar, VARCHAROID, NULL, 0)
TYPE_OPCODE(bpchar, BPCHAROID, NULL, 0)
TYPE_OPCODE(date, DATEOID, NULL, 0)
TYPE_OPCODE(time, TIMEOID, NULL, 0)
TYPE_OPCODE(timetz, TIMETZOID, NULL, 0)
TYPE_OPCODE(timestamp, TIMESTAMPOID, NULL, 0)
TYPE_OPCODE(timestamptz, TIMESTAMPTZOID, NULL, 0)
TYPE_OPCODE(interval, INTERVALOID, NULL, DEVTYPE__USE_KVARS_SLOTBUF)
TYPE_OPCODE(money, MONEYOID, NULL, 0)
TYPE_OPCODE(uuid, UUIDOID, NULL, 0)
TYPE_OPCODE(macaddr, MACADDROID, NULL, 0)
TYPE_OPCODE(inet, INETOID, NULL, 0)
//TYPE_OPCODE(cidr, CIDROID, NULL)

/*
 * PostgreSQL Device Functions / Operators
 */
#ifndef FUNC_OPCODE
#define FUNC_OPCODE(SQL_NAME,FUNC_ARGS,FUNC_FLAGS,DEV_NAME,FUNC_COST,EXTENSION)
#endif
/* most device functions are sufficient with __FUNC_OPCODE */
#define __FUNC_OPCODE(FUNC_NAME,FUNC_ARGS,FUNC_COST,EXTENSION)			\
	FUNC_OPCODE(FUNC_NAME,FUNC_ARGS,DEVKIND__ANY,FUNC_NAME,FUNC_COST,EXTENSION)
#define __FUNC_LOCALE_OPCODE(FUNC_NAME,FUNC_ARGS,FUNC_COST,EXTENSION)	\
	FUNC_OPCODE(FUNC_NAME,FUNC_ARGS,DEVFUNC__LOCALE_AWARE|DEVKIND__ANY,FUNC_NAME,FUNC_COST,EXTENSION)

/* type cast functions */
FUNC_OPCODE(bool, int4,   DEVKIND__ANY, int4_to_bool, 1, NULL)
FUNC_OPCODE(int1, int2,   DEVKIND__ANY, int2_to_int1, 1, "pg_strom")
FUNC_OPCODE(int1, int4,   DEVKIND__ANY, int4_to_int1, 1, "pg_strom")
FUNC_OPCODE(int1, int8,   DEVKIND__ANY, int8_to_int1, 1, "pg_strom")
FUNC_OPCODE(int1, float2, DEVKIND__ANY, float2_to_int1, 1, "pg_strom")
FUNC_OPCODE(int1, float4, DEVKIND__ANY, float4_to_int1, 1, "pg_strom")
FUNC_OPCODE(int1, float8, DEVKIND__ANY, float8_to_int1, 1, "pg_strom")

FUNC_OPCODE(int2, int1,   DEVKIND__ANY, int1_to_int2, 1, "pg_strom")
FUNC_OPCODE(int2, int4,   DEVKIND__ANY, int4_to_int2, 1, NULL)
FUNC_OPCODE(int2, int8,   DEVKIND__ANY, int8_to_int2, 1, NULL)
FUNC_OPCODE(int2, float2, DEVKIND__ANY, float2_to_int2, 1, "pg_strom")
FUNC_OPCODE(int2, float4, DEVKIND__ANY, float4_to_int2, 1, NULL)
FUNC_OPCODE(int2, float8, DEVKIND__ANY, float8_to_int2, 1, NULL)

FUNC_OPCODE(int4, int1,   DEVKIND__ANY, int1_to_int4, 1, "pg_strom")
FUNC_OPCODE(int4, int2,   DEVKIND__ANY, int2_to_int4, 1, NULL)
FUNC_OPCODE(int4, int8,   DEVKIND__ANY, int8_to_int4, 1, NULL)
FUNC_OPCODE(int4, float2, DEVKIND__ANY, float2_to_int4, 1, "pg_strom")
FUNC_OPCODE(int4, float4, DEVKIND__ANY, float4_to_int4, 1, NULL)
FUNC_OPCODE(int4, float8, DEVKIND__ANY, float8_to_int4, 1, NULL)

FUNC_OPCODE(int8, int1,   DEVKIND__ANY, int1_to_int8, 1, "pg_strom")
FUNC_OPCODE(int8, int2,   DEVKIND__ANY, int2_to_int8, 1, NULL)
FUNC_OPCODE(int8, int4,   DEVKIND__ANY, int4_to_int8, 1, NULL)
FUNC_OPCODE(int8, float2, DEVKIND__ANY, float2_to_int8, 1, "pg_strom")
FUNC_OPCODE(int8, float4, DEVKIND__ANY, float4_to_int8, 1, NULL)
FUNC_OPCODE(int8, float8, DEVKIND__ANY, float8_to_int8, 1, NULL)

FUNC_OPCODE(float2, int1, DEVKIND__ANY, int1_to_float2, 1, "pg_strom")
FUNC_OPCODE(float2, int2, DEVKIND__ANY, int2_to_float2, 1, "pg_strom")
FUNC_OPCODE(float2, int4, DEVKIND__ANY, int4_to_float2, 1, "pg_strom")
FUNC_OPCODE(float2, int8, DEVKIND__ANY, int8_to_float2, 1, "pg_strom")
FUNC_OPCODE(float2, float4, DEVKIND__ANY, float4_to_float2, 1, "pg_strom")
FUNC_OPCODE(float2, float8, DEVKIND__ANY, float8_to_float2, 1, "pg_strom")

FUNC_OPCODE(float4, int1, DEVKIND__ANY, int1_to_float4, 1, "pg_strom")
FUNC_OPCODE(float4, int2, DEVKIND__ANY, int2_to_float4, 1, NULL)
FUNC_OPCODE(float4, int4, DEVKIND__ANY, int4_to_float4, 1, NULL)
FUNC_OPCODE(float4, int8, DEVKIND__ANY, int8_to_float4, 1, NULL)
FUNC_OPCODE(float4, float2, DEVKIND__ANY, float2_to_float4, 1, "pg_strom")
FUNC_OPCODE(float4, float8, DEVKIND__ANY, float8_to_float4, 1, NULL)

FUNC_OPCODE(float8, int1, DEVKIND__ANY, int1_to_float8, 1, "pg_strom")
FUNC_OPCODE(float8, int2, DEVKIND__ANY, int2_to_float8, 1, NULL)
FUNC_OPCODE(float8, int4, DEVKIND__ANY, int4_to_float8, 1, NULL)
FUNC_OPCODE(float8, int8, DEVKIND__ANY, int8_to_float8, 1, NULL)
FUNC_OPCODE(float8, float2, DEVKIND__ANY, float2_to_float8, 1, "pg_strom")
FUNC_OPCODE(float8, float4, DEVKIND__ANY, float4_to_float8, 1, NULL)

/* '+' : add operators */
__FUNC_OPCODE(int1pl,  int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int12pl, int1/int2, 1, "pg_strom")
__FUNC_OPCODE(int14pl, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int18pl, int1/int8, 1, "pg_strom")
__FUNC_OPCODE(int21pl, int2/int1, 1, "pg_strom")
__FUNC_OPCODE(int2pl,  int2/int2, 1, NULL)
__FUNC_OPCODE(int24pl, int2/int4, 1, NULL)
__FUNC_OPCODE(int28pl, int2/int8, 1, NULL)
__FUNC_OPCODE(int41pl, int4/int1, 1, "pg_strom")
__FUNC_OPCODE(int42pl, int4/int2, 1, NULL)
__FUNC_OPCODE(int4pl,  int4/int4, 1, NULL)
__FUNC_OPCODE(int48pl, int4/int8, 1, NULL)
__FUNC_OPCODE(int81pl, int8/int1, 1, "pg_strom")
__FUNC_OPCODE(int82pl, int8/int2, 1, NULL)
__FUNC_OPCODE(int84pl, int8/int4, 1, NULL)
__FUNC_OPCODE(int8pl,  int8/int8, 1, NULL)
__FUNC_OPCODE(float2pl,  float2/float2, 1, "pg_strom")
__FUNC_OPCODE(float24pl, float2/float4, 1, "pg_strom")
__FUNC_OPCODE(float28pl, float2/float8, 1, "pg_strom")
__FUNC_OPCODE(float42pl, float4/float2, 1, "pg_strom")
__FUNC_OPCODE(float4pl,  float4/float4, 1, NULL)
__FUNC_OPCODE(float48pl, float4/float8, 1, NULL)
__FUNC_OPCODE(float82pl, float8/float2, 1, "pg_strom")
__FUNC_OPCODE(float84pl, float8/float4, 1, NULL)
__FUNC_OPCODE(float8pl,  float8/float8, 1, NULL)

/* '-' : subtract operators */
__FUNC_OPCODE(int1mi,  int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int12mi, int1/int2, 1, "pg_strom")
__FUNC_OPCODE(int14mi, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int18mi, int1/int8, 1, "pg_strom")
__FUNC_OPCODE(int21mi, int2/int1, 1, "pg_strom")
__FUNC_OPCODE(int2mi,  int2/int2, 1, NULL)
__FUNC_OPCODE(int24mi, int2/int4, 1, NULL)
__FUNC_OPCODE(int28mi, int2/int8, 1, NULL)
__FUNC_OPCODE(int41mi, int4/int1, 1, "pg_strom")
__FUNC_OPCODE(int42mi, int4/int2, 1, NULL)
__FUNC_OPCODE(int4mi,  int4/int4, 1, NULL)
__FUNC_OPCODE(int48mi, int4/int8, 1, NULL)
__FUNC_OPCODE(int81mi, int8/int1, 1, "pg_strom")
__FUNC_OPCODE(int82mi, int8/int2, 1, NULL)
__FUNC_OPCODE(int84mi, int8/int4, 1, NULL)
__FUNC_OPCODE(int8mi,  int8/int8, 1, NULL)
__FUNC_OPCODE(float2mi,  float2/float2, 1, "pg_strom")
__FUNC_OPCODE(float24mi, float2/float4, 1, "pg_strom")
__FUNC_OPCODE(float28mi, float2/float8, 1, "pg_strom")
__FUNC_OPCODE(float42mi, float4/float2, 1, "pg_strom")
__FUNC_OPCODE(float4mi,  float4/float4, 1, NULL)
__FUNC_OPCODE(float48mi, float4/float8, 1, NULL)
__FUNC_OPCODE(float82mi, float8/float2, 1, "pg_strom")
__FUNC_OPCODE(float84mi, float8/float4, 1, NULL)
__FUNC_OPCODE(float8mi,  float8/float8, 1, NULL)

/* '*' : subtract operators */
__FUNC_OPCODE(int1mul,  int1/int1, 2, "pg_strom")
__FUNC_OPCODE(int12mul, int1/int2, 2, "pg_strom")
__FUNC_OPCODE(int14mul, int1/int4, 2, "pg_strom")
__FUNC_OPCODE(int18mul, int1/int8, 2, "pg_strom")
__FUNC_OPCODE(int21mul, int2/int1, 2, "pg_strom")
__FUNC_OPCODE(int2mul,  int2/int2, 2, NULL)
__FUNC_OPCODE(int24mul, int2/int4, 2, NULL)
__FUNC_OPCODE(int28mul, int2/int8, 2, NULL)
__FUNC_OPCODE(int41mul, int4/int1, 2, "pg_strom")
__FUNC_OPCODE(int42mul, int4/int2, 2, NULL)
__FUNC_OPCODE(int4mul,  int4/int4, 2, NULL)
__FUNC_OPCODE(int48mul, int4/int8, 2, NULL)
__FUNC_OPCODE(int81mul, int8/int1, 2, "pg_strom")
__FUNC_OPCODE(int82mul, int8/int2, 2, NULL)
__FUNC_OPCODE(int84mul, int8/int4, 2, NULL)
__FUNC_OPCODE(int8mul,  int8/int8, 2, NULL)
__FUNC_OPCODE(float2mul,  float2/float2, 2, "pg_strom")
__FUNC_OPCODE(float24mul, float2/float4, 2, "pg_strom")
__FUNC_OPCODE(float28mul, float2/float8, 2, "pg_strom")
__FUNC_OPCODE(float42mul, float4/float2, 2, "pg_strom")
__FUNC_OPCODE(float4mul,  float4/float4, 2, NULL)
__FUNC_OPCODE(float48mul, float4/float8, 2, NULL)
__FUNC_OPCODE(float82mul, float8/float2, 2, "pg_strom")
__FUNC_OPCODE(float84mul, float8/float4, 2, NULL)
__FUNC_OPCODE(float8mul,  float8/float8, 2, NULL)

/* '/' : divide operators */
__FUNC_OPCODE(int1div,  int1/int1, 4, "pg_strom")
__FUNC_OPCODE(int12div, int1/int2, 4, "pg_strom")
__FUNC_OPCODE(int14div, int1/int4, 4, "pg_strom")
__FUNC_OPCODE(int18div, int1/int8, 4, "pg_strom")
__FUNC_OPCODE(int21div, int2/int1, 4, "pg_strom")
__FUNC_OPCODE(int2div,  int2/int2, 4, NULL)
__FUNC_OPCODE(int24div, int2/int4, 4, NULL)
__FUNC_OPCODE(int28div, int2/int8, 4, NULL)
__FUNC_OPCODE(int41div, int4/int1, 4, "pg_strom")
__FUNC_OPCODE(int42div, int4/int2, 4, NULL)
__FUNC_OPCODE(int4div,  int4/int4, 4, NULL)
__FUNC_OPCODE(int48div, int4/int8, 4, NULL)
__FUNC_OPCODE(int81div, int8/int1, 4, "pg_strom")
__FUNC_OPCODE(int82div, int8/int2, 4, NULL)
__FUNC_OPCODE(int84div, int8/int4, 4, NULL)
__FUNC_OPCODE(int8div,  int8/int8, 4, NULL)
__FUNC_OPCODE(float2div,  float2/float2, 4, "pg_strom")
__FUNC_OPCODE(float24div, float2/float4, 4, "pg_strom")
__FUNC_OPCODE(float28div, float2/float8, 4, "pg_strom")
__FUNC_OPCODE(float42div, float4/float2, 4, "pg_strom")
__FUNC_OPCODE(float4div,  float4/float4, 4, NULL)
__FUNC_OPCODE(float48div, float4/float8, 4, NULL)
__FUNC_OPCODE(float82div, float8/float2, 4, "pg_strom")
__FUNC_OPCODE(float84div, float8/float4, 4, NULL)
__FUNC_OPCODE(float8div,  float8/float8, 4, NULL)

/* '%' : reminder operators */
__FUNC_OPCODE(int1mod, int1/int1, 4, "pg_strom")
__FUNC_OPCODE(int2mod, int2/int2, 4, NULL)
__FUNC_OPCODE(int4mod, int4/int4, 4, NULL)
__FUNC_OPCODE(int8mod, int8/int8, 4, NULL)

/* '+' : unary plus operators */
__FUNC_OPCODE(int1up, int1, 1, "pg_strom")
__FUNC_OPCODE(int2up, int2, 1, NULL)
__FUNC_OPCODE(int4up, int2, 1, NULL)
__FUNC_OPCODE(int8up, int2, 1, NULL)
__FUNC_OPCODE(float2up, float2, 1, "pg_strom")
__FUNC_OPCODE(float4up, float4, 1, NULL)
__FUNC_OPCODE(float8up, float8, 1, NULL)

/* '-' : unary minus operators */
__FUNC_OPCODE(int1um, int1, 1, "pg_strom")
__FUNC_OPCODE(int2um, int2, 1, NULL)
__FUNC_OPCODE(int4um, int2, 1, NULL)
__FUNC_OPCODE(int8um, int2, 1, NULL)
__FUNC_OPCODE(float2um, float2, 1, "pg_strom")
__FUNC_OPCODE(float4um, float4, 1, NULL)
__FUNC_OPCODE(float8um, float8, 1, NULL)

/* '@' : absolute value operators */
__FUNC_OPCODE(int1abs, int1, 1, "pg_strom")
__FUNC_OPCODE(int2abs, int2, 1, NULL)
__FUNC_OPCODE(int4abs, int2, 1, NULL)
__FUNC_OPCODE(int8abs, int2, 1, NULL)
__FUNC_OPCODE(float2abs, float2, 1, "pg_strom")
__FUNC_OPCODE(float4abs, float4, 1, NULL)
__FUNC_OPCODE(float8abs, float8, 1, NULL)

/* '=' : equal operators */
__FUNC_OPCODE(booleq,  bool/bool, 1, NULL)
__FUNC_OPCODE(int1eq,  int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int12eq, int1/int2, 1, "pg_strom")
__FUNC_OPCODE(int14eq, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int18eq, int1/int8, 1, "pg_strom")
__FUNC_OPCODE(int21eq, int2/int1, 1, "pg_strom")
__FUNC_OPCODE(int2eq,  int2/int2, 1, NULL)
__FUNC_OPCODE(int24eq, int2/int4, 1, NULL)
__FUNC_OPCODE(int28eq, int2/int8, 1, NULL)
__FUNC_OPCODE(int41eq, int4/int1, 1, "pg_strom")
__FUNC_OPCODE(int42eq, int4/int2, 1, NULL)
__FUNC_OPCODE(int4eq,  int4/int4, 1, NULL)
__FUNC_OPCODE(int48eq, int4/int8, 1, NULL)
__FUNC_OPCODE(int81eq, int8/int1, 1, "pg_strom")
__FUNC_OPCODE(int82eq, int8/int2, 1, NULL)
__FUNC_OPCODE(int84eq, int8/int4, 1, NULL)
__FUNC_OPCODE(int8eq,  int8/int8, 1, NULL)
__FUNC_OPCODE(float2eq,  float2/float2, 1, "pg_strom")
__FUNC_OPCODE(float24eq, float2/float4, 1, "pg_strom")
__FUNC_OPCODE(float28eq, float2/float8, 1, "pg_strom")
__FUNC_OPCODE(float42eq, float4/float2, 1, "pg_strom")
__FUNC_OPCODE(float4eq,  float4/float4, 1, NULL)
__FUNC_OPCODE(float48eq, float4/float8, 1, NULL)
__FUNC_OPCODE(float82eq, float8/float2, 1, "pg_strom")
__FUNC_OPCODE(float84eq, float8/float4, 1, NULL)
__FUNC_OPCODE(float8eq,  float8/float8, 1, NULL)

/* '<>' : not equal operators */
__FUNC_OPCODE(int1ne,  int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int12ne, int1/int2, 1, "pg_strom")
__FUNC_OPCODE(int14ne, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int18ne, int1/int8, 1, "pg_strom")
__FUNC_OPCODE(int21ne, int2/int1, 1, "pg_strom")
__FUNC_OPCODE(int2ne,  int2/int2, 1, NULL)
__FUNC_OPCODE(int24ne, int2/int4, 1, NULL)
__FUNC_OPCODE(int28ne, int2/int8, 1, NULL)
__FUNC_OPCODE(int41ne, int4/int1, 1, "pg_strom")
__FUNC_OPCODE(int42ne, int4/int2, 1, NULL)
__FUNC_OPCODE(int4ne,  int4/int4, 1, NULL)
__FUNC_OPCODE(int48ne, int4/int8, 1, NULL)
__FUNC_OPCODE(int81ne, int8/int1, 1, "pg_strom")
__FUNC_OPCODE(int82ne, int8/int2, 1, NULL)
__FUNC_OPCODE(int84ne, int8/int4, 1, NULL)
__FUNC_OPCODE(int8ne,  int8/int8, 1, NULL)
__FUNC_OPCODE(float2ne,  float2/float2, 1, "pg_strom")
__FUNC_OPCODE(float24ne, float2/float4, 1, "pg_strom")
__FUNC_OPCODE(float28ne, float2/float8, 1, "pg_strom")
__FUNC_OPCODE(float42ne, float4/float2, 1, "pg_strom")
__FUNC_OPCODE(float4ne,  float4/float4, 1, NULL)
__FUNC_OPCODE(float48ne, float4/float8, 1, NULL)
__FUNC_OPCODE(float82ne, float8/float2, 1, "pg_strom")
__FUNC_OPCODE(float84ne, float8/float4, 1, NULL)
__FUNC_OPCODE(float8ne,  float8/float8, 1, NULL)

/* '>' : greater than operators */
__FUNC_OPCODE(int1gt,  int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int12gt, int1/int2, 1, "pg_strom")
__FUNC_OPCODE(int14gt, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int18gt, int1/int8, 1, "pg_strom")
__FUNC_OPCODE(int21gt, int2/int1, 1, "pg_strom")
__FUNC_OPCODE(int2gt,  int2/int2, 1, NULL)
__FUNC_OPCODE(int24gt, int2/int4, 1, NULL)
__FUNC_OPCODE(int28gt, int2/int8, 1, NULL)
__FUNC_OPCODE(int41gt, int4/int1, 1, "pg_strom")
__FUNC_OPCODE(int42gt, int4/int2, 1, NULL)
__FUNC_OPCODE(int4gt,  int4/int4, 1, NULL)
__FUNC_OPCODE(int48gt, int4/int8, 1, NULL)
__FUNC_OPCODE(int81gt, int8/int1, 1, "pg_strom")
__FUNC_OPCODE(int82gt, int8/int2, 1, NULL)
__FUNC_OPCODE(int84gt, int8/int4, 1, NULL)
__FUNC_OPCODE(int8gt,  int8/int8, 1, NULL)
__FUNC_OPCODE(float2gt,  float2/float2, 1, "pg_strom")
__FUNC_OPCODE(float24gt, float2/float4, 1, "pg_strom")
__FUNC_OPCODE(float28gt, float2/float8, 1, "pg_strom")
__FUNC_OPCODE(float42gt, float4/float2, 1, "pg_strom")
__FUNC_OPCODE(float4gt,  float4/float4, 1, NULL)
__FUNC_OPCODE(float48gt, float4/float8, 1, NULL)
__FUNC_OPCODE(float82gt, float8/float2, 1, "pg_strom")
__FUNC_OPCODE(float84gt, float8/float4, 1, NULL)
__FUNC_OPCODE(float8gt,  float8/float8, 1, NULL)

/* '<' : less than operators */
__FUNC_OPCODE(int1lt,  int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int12lt, int1/int2, 1, "pg_strom")
__FUNC_OPCODE(int14lt, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int18lt, int1/int8, 1, "pg_strom")
__FUNC_OPCODE(int21lt, int2/int1, 1, "pg_strom")
__FUNC_OPCODE(int2lt,  int2/int2, 1, NULL)
__FUNC_OPCODE(int24lt, int2/int4, 1, NULL)
__FUNC_OPCODE(int28lt, int2/int8, 1, NULL)
__FUNC_OPCODE(int41lt, int4/int1, 1, "pg_strom")
__FUNC_OPCODE(int42lt, int4/int2, 1, NULL)
__FUNC_OPCODE(int4lt,  int4/int4, 1, NULL)
__FUNC_OPCODE(int48lt, int4/int8, 1, NULL)
__FUNC_OPCODE(int81lt, int8/int1, 1, "pg_strom")
__FUNC_OPCODE(int82lt, int8/int2, 1, NULL)
__FUNC_OPCODE(int84lt, int8/int4, 1, NULL)
__FUNC_OPCODE(int8lt,  int8/int8, 1, NULL)
__FUNC_OPCODE(float2lt,  float2/float2, 1, "pg_strom")
__FUNC_OPCODE(float24lt, float2/float4, 1, "pg_strom")
__FUNC_OPCODE(float28lt, float2/float8, 1, "pg_strom")
__FUNC_OPCODE(float42lt, float4/float2, 1, "pg_strom")
__FUNC_OPCODE(float4lt,  float4/float4, 1, NULL)
__FUNC_OPCODE(float48lt, float4/float8, 1, NULL)
__FUNC_OPCODE(float82lt, float8/float2, 1, "pg_strom")
__FUNC_OPCODE(float84lt, float8/float4, 1, NULL)
__FUNC_OPCODE(float8lt,  float8/float8, 1, NULL)

/* '>=' : relational greater-than or equal-to */	
__FUNC_OPCODE(int1ge,  int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int12ge, int1/int2, 1, "pg_strom")
__FUNC_OPCODE(int14ge, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int18ge, int1/int8, 1, "pg_strom")
__FUNC_OPCODE(int21ge, int2/int1, 1, "pg_strom")
__FUNC_OPCODE(int2ge,  int2/int2, 1, NULL)
__FUNC_OPCODE(int24ge, int2/int4, 1, NULL)
__FUNC_OPCODE(int28ge, int2/int8, 1, NULL)
__FUNC_OPCODE(int41ge, int4/int1, 1, "pg_strom")
__FUNC_OPCODE(int42ge, int4/int2, 1, NULL)
__FUNC_OPCODE(int4ge,  int4/int4, 1, NULL)
__FUNC_OPCODE(int48ge, int4/int8, 1, NULL)
__FUNC_OPCODE(int81ge, int8/int1, 1, "pg_strom")
__FUNC_OPCODE(int82ge, int8/int2, 1, NULL)
__FUNC_OPCODE(int84ge, int8/int4, 1, NULL)
__FUNC_OPCODE(int8ge,  int8/int8, 1, NULL)
__FUNC_OPCODE(float2ge,  float2/float2, 1, "pg_strom")
__FUNC_OPCODE(float24ge, float2/float4, 1, "pg_strom")
__FUNC_OPCODE(float28ge, float2/float8, 1, "pg_strom")
__FUNC_OPCODE(float42ge, float4/float2, 1, "pg_strom")
__FUNC_OPCODE(float4ge,  float4/float4, 1, NULL)
__FUNC_OPCODE(float48ge, float4/float8, 1, NULL)
__FUNC_OPCODE(float82ge, float8/float2, 1, "pg_strom")
__FUNC_OPCODE(float84ge, float8/float4, 1, NULL)
__FUNC_OPCODE(float8ge,  float8/float8, 1, NULL)

/* '<=' : relational less-than or equal-to */
__FUNC_OPCODE(int1le,  int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int12le, int1/int2, 1, "pg_strom")
__FUNC_OPCODE(int14le, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int18le, int1/int8, 1, "pg_strom")
__FUNC_OPCODE(int21le, int2/int1, 1, "pg_strom")
__FUNC_OPCODE(int2le,  int2/int2, 1, NULL)
__FUNC_OPCODE(int24le, int2/int4, 1, NULL)
__FUNC_OPCODE(int28le, int2/int8, 1, NULL)
__FUNC_OPCODE(int41le, int4/int1, 1, "pg_strom")
__FUNC_OPCODE(int42le, int4/int2, 1, NULL)
__FUNC_OPCODE(int4le,  int4/int4, 1, NULL)
__FUNC_OPCODE(int48le, int4/int8, 1, NULL)
__FUNC_OPCODE(int81le, int8/int1, 1, "pg_strom")
__FUNC_OPCODE(int82le, int8/int2, 1, NULL)
__FUNC_OPCODE(int84le, int8/int4, 1, NULL)
__FUNC_OPCODE(int8le,  int8/int8, 1, NULL)
__FUNC_OPCODE(float2le,  float2/float2, 1, "pg_strom")
__FUNC_OPCODE(float24le, float2/float4, 1, "pg_strom")
__FUNC_OPCODE(float28le, float2/float8, 1, "pg_strom")
__FUNC_OPCODE(float42le, float4/float2, 1, "pg_strom")
__FUNC_OPCODE(float4le,  float4/float4, 1, NULL)
__FUNC_OPCODE(float48le, float4/float8, 1, NULL)
__FUNC_OPCODE(float82le, float8/float2, 1, "pg_strom")
__FUNC_OPCODE(float84le, float8/float4, 1, NULL)
__FUNC_OPCODE(float8le,  float8/float8, 1, NULL)

/* '&' : bitwise and */
__FUNC_OPCODE(int1and, int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int2and, int2/int2, 1, NULL)
__FUNC_OPCODE(int4and, int4/int4, 1, NULL)
__FUNC_OPCODE(int8and, int8/int8, 1, NULL)

/* '|'  : bitwise or */
__FUNC_OPCODE(int1or, int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int2or, int2/int2, 1, NULL)
__FUNC_OPCODE(int4or, int4/int4, 1, NULL)
__FUNC_OPCODE(int8or, int8/int8, 1, NULL)

/* '#'  : bitwise xor */
__FUNC_OPCODE(int1xor, int1/int1, 1, "pg_strom")
__FUNC_OPCODE(int2xor, int2/int2, 1, NULL)
__FUNC_OPCODE(int4xor, int4/int4, 1, NULL)
__FUNC_OPCODE(int8xor, int8/int8, 1, NULL)	
	
/* '~'  : bitwise not */
__FUNC_OPCODE(int1not, int1, 1, "pg_strom")
__FUNC_OPCODE(int2not, int2, 1, NULL)
__FUNC_OPCODE(int4not, int4, 1, NULL)
__FUNC_OPCODE(int8not, int8, 1, NULL)	

/* '>>' : right shift */
__FUNC_OPCODE(int1shr, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int2shr, int2/int4, 1, NULL)
__FUNC_OPCODE(int4shr, int4/int4, 1, NULL)
__FUNC_OPCODE(int8shr, int8/int4, 1, NULL)

/* '<<' : left shift */
__FUNC_OPCODE(int1shl, int1/int4, 1, "pg_strom")
__FUNC_OPCODE(int2shl, int2/int4, 1, NULL)
__FUNC_OPCODE(int4shl, int4/int4, 1, NULL)
__FUNC_OPCODE(int8shl, int8/int4, 1, NULL)

/* numeric functions */
FUNC_OPCODE(int1, numeric, DEVKIND__ANY, numeric_to_int1, 50, "pg_strom")
FUNC_OPCODE(int2, numeric, DEVKIND__ANY, numeric_to_int2, 50, NULL)
FUNC_OPCODE(int4, numeric, DEVKIND__ANY, numeric_to_int4, 50, NULL)
FUNC_OPCODE(int8, numeric, DEVKIND__ANY, numeric_to_int8, 50, NULL)
FUNC_OPCODE(float2, numeric, DEVKIND__ANY, numeric_to_float2, 50, "pg_strom")
FUNC_OPCODE(float4, numeric, DEVKIND__ANY, numeric_to_float4, 50, NULL)
FUNC_OPCODE(float8, numeric, DEVKIND__ANY, numeric_to_float8, 50, NULL)
FUNC_OPCODE(money, numeric, DEVKIND__ANY, numeric_to_money, 50, NULL)
FUNC_OPCODE(numeric, int1, DEVKIND__ANY, int1_to_numeric, 50, "pg_strom")
FUNC_OPCODE(numeric, int2, DEVKIND__ANY, int2_to_numeric, 50, NULL)
FUNC_OPCODE(numeric, int4, DEVKIND__ANY, int4_to_numeric, 50, NULL)
FUNC_OPCODE(numeric, int8, DEVKIND__ANY, int8_to_numeric, 50, NULL)
FUNC_OPCODE(numeric, float2, DEVKIND__ANY, float2_to_numeric, 50, "pg_strom")
FUNC_OPCODE(numeric, float4, DEVKIND__ANY, float4_to_numeric, 50, NULL)
FUNC_OPCODE(numeric, float8, DEVKIND__ANY, float8_to_numeric, 50, NULL)
FUNC_OPCODE(numeric, money, DEVKIND__ANY, money_to_numeric, 50, NULL)
__FUNC_OPCODE(numeric_add, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_sub, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_mul, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_div, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_mod, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_uplus, numeric, 30, NULL)
__FUNC_OPCODE(numeric_uminus, numeric, 30, NULL)	
__FUNC_OPCODE(numeric_abs, numeric, 30, NULL)
__FUNC_OPCODE(numeric_eq, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_ne, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_lt, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_le, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_gt, numeric/numeric, 30, NULL)
__FUNC_OPCODE(numeric_ge, numeric/numeric, 30, NULL)

/* Date and time functions */
FUNC_OPCODE(timestamp, date, DEVKIND__ANY, date_to_timestamp, 5, NULL)
FUNC_OPCODE(timestamp, timestamptz, DEVKIND__ANY, timestamptz_to_timestamp, 5, NULL)
FUNC_OPCODE(timestamptz, date, DEVKIND__ANY|DEVKERN__SESSION_TIMEZONE, date_to_timestamptz, 5, NULL)
FUNC_OPCODE(timestamptz, timestamp, DEVKIND__ANY, timestamp_to_timestamptz, 5, NULL)

__FUNC_OPCODE(date_eq, date/date, 1, NULL)
__FUNC_OPCODE(date_ne, date/date, 1, NULL)
__FUNC_OPCODE(date_lt, date/date, 1, NULL)
__FUNC_OPCODE(date_le, date/date, 1, NULL)
__FUNC_OPCODE(date_gt, date/date, 1, NULL)
__FUNC_OPCODE(date_ge, date/date, 1, NULL)

__FUNC_OPCODE(time_eq, time/time, 1, NULL)
__FUNC_OPCODE(time_ne, time/time, 1, NULL)
__FUNC_OPCODE(time_lt, time/time, 1, NULL)
__FUNC_OPCODE(time_le, time/time, 1, NULL)
__FUNC_OPCODE(time_gt, time/time, 1, NULL)
__FUNC_OPCODE(time_ge, time/time, 1, NULL)

__FUNC_OPCODE(timetz_eq, timetz/timetz, 1, NULL)
__FUNC_OPCODE(timetz_ne, timetz/timetz, 1, NULL)
__FUNC_OPCODE(timetz_lt, timetz/timetz, 1, NULL)
__FUNC_OPCODE(timetz_le, timetz/timetz, 1, NULL)
__FUNC_OPCODE(timetz_gt, timetz/timetz, 1, NULL)
__FUNC_OPCODE(timetz_ge, timetz/timetz, 1, NULL)

__FUNC_OPCODE(timestamp_eq, timestamp/timestamp, 1, NULL)
__FUNC_OPCODE(timestamp_ne, timestamp/timestamp, 1, NULL)
__FUNC_OPCODE(timestamp_lt, timestamp/timestamp, 1, NULL)
__FUNC_OPCODE(timestamp_le, timestamp/timestamp, 1, NULL)
__FUNC_OPCODE(timestamp_gt, timestamp/timestamp, 1, NULL)
__FUNC_OPCODE(timestamp_ge, timestamp/timestamp, 1, NULL)

__FUNC_OPCODE(timestamptz_eq, timestamptz/timestamptz, 1, NULL)
__FUNC_OPCODE(timestamptz_ne, timestamptz/timestamptz, 1, NULL)
__FUNC_OPCODE(timestamptz_lt, timestamptz/timestamptz, 1, NULL)
__FUNC_OPCODE(timestamptz_le, timestamptz/timestamptz, 1, NULL)
__FUNC_OPCODE(timestamptz_gt, timestamptz/timestamptz, 1, NULL)
__FUNC_OPCODE(timestamptz_ge, timestamptz/timestamptz, 1, NULL)

__FUNC_OPCODE(date_eq_timestamp, date/timestamp, 1, NULL)
__FUNC_OPCODE(date_ne_timestamp, date/timestamp, 1, NULL)
__FUNC_OPCODE(date_lt_timestamp, date/timestamp, 1, NULL)
__FUNC_OPCODE(date_le_timestamp, date/timestamp, 1, NULL)
__FUNC_OPCODE(date_gt_timestamp, date/timestamp, 1, NULL)
__FUNC_OPCODE(date_ge_timestamp, date/timestamp, 1, NULL)

__FUNC_OPCODE(timestamp_eq_date, timestamp/date, 1, NULL)
__FUNC_OPCODE(timestamp_ne_date, timestamp/date, 1, NULL)
__FUNC_OPCODE(timestamp_lt_date, timestamp/date, 1, NULL)
__FUNC_OPCODE(timestamp_le_date, timestamp/date, 1, NULL)
__FUNC_OPCODE(timestamp_gt_date, timestamp/date, 1, NULL)
__FUNC_OPCODE(timestamp_ge_date, timestamp/date, 1, NULL)

__FUNC_OPCODE(date_eq_timestamptz, date/timestamptz, 5, NULL)
__FUNC_OPCODE(date_ne_timestamptz, date/timestamptz, 5, NULL)
__FUNC_OPCODE(date_lt_timestamptz, date/timestamptz, 5, NULL)
__FUNC_OPCODE(date_le_timestamptz, date/timestamptz, 5, NULL)
__FUNC_OPCODE(date_gt_timestamptz, date/timestamptz, 5, NULL)
__FUNC_OPCODE(date_ge_timestamptz, date/timestamptz, 5, NULL)

__FUNC_OPCODE(timestamptz_eq_date, timestamptz/date, 5, NULL)
__FUNC_OPCODE(timestamptz_ne_date, timestamptz/date, 5, NULL)
__FUNC_OPCODE(timestamptz_lt_date, timestamptz/date, 5, NULL)
__FUNC_OPCODE(timestamptz_le_date, timestamptz/date, 5, NULL)
__FUNC_OPCODE(timestamptz_gt_date, timestamptz/date, 5, NULL)
__FUNC_OPCODE(timestamptz_ge_date, timestamptz/date, 5, NULL)

__FUNC_OPCODE(timestamp_eq_timestamptz, timestamp/timestamptz, 5, NULL)
__FUNC_OPCODE(timestamp_ne_timestamptz, timestamp/timestamptz, 5, NULL)
__FUNC_OPCODE(timestamp_lt_timestamptz, timestamp/timestamptz, 5, NULL)
__FUNC_OPCODE(timestamp_le_timestamptz, timestamp/timestamptz, 5, NULL)
__FUNC_OPCODE(timestamp_gt_timestamptz, timestamp/timestamptz, 5, NULL)
__FUNC_OPCODE(timestamp_ge_timestamptz, timestamp/timestamptz, 5, NULL)

__FUNC_OPCODE(timestamptz_eq_timestamp, timestamptz/timestamp, 5, NULL)
__FUNC_OPCODE(timestamptz_ne_timestamp, timestamptz/timestamp, 5, NULL)
__FUNC_OPCODE(timestamptz_lt_timestamp, timestamptz/timestamp, 5, NULL)
__FUNC_OPCODE(timestamptz_le_timestamp, timestamptz/timestamp, 5, NULL)
__FUNC_OPCODE(timestamptz_gt_timestamp, timestamptz/timestamp, 5, NULL)
__FUNC_OPCODE(timestamptz_ge_timestamp, timestamptz/timestamp, 5, NULL)

__FUNC_OPCODE(interval_eq, interval/interval, 5, NULL)
__FUNC_OPCODE(interval_ne, interval/interval, 5, NULL)
__FUNC_OPCODE(interval_lt, interval/interval, 5, NULL)
__FUNC_OPCODE(interval_le, interval/interval, 5, NULL)
__FUNC_OPCODE(interval_gt, interval/interval, 5, NULL)
__FUNC_OPCODE(interval_ge, interval/interval, 5, NULL)

/*
 * Text functions/operators
 */
__FUNC_OPCODE(bpchareq, bpchar/bpchar, 99, NULL)
__FUNC_OPCODE(bpcharne, bpchar/bpchar, 99, NULL)
__FUNC_LOCALE_OPCODE(bpcharlt, bpchar/bpchar, 99, NULL)
__FUNC_LOCALE_OPCODE(bpcharle, bpchar/bpchar, 99, NULL)
__FUNC_LOCALE_OPCODE(bpchargt, bpchar/bpchar, 99, NULL)
__FUNC_LOCALE_OPCODE(bpcharge, bpchar/bpchar, 99, NULL)
FUNC_OPCODE(length, bpchar, DEVFUNC__LOCALE_AWARE|DEVKIND__ANY, bpcharlen, 2, NULL)

__FUNC_OPCODE(texteq, text/text, 99, NULL)
__FUNC_OPCODE(textne, text/text, 99, NULL)
__FUNC_LOCALE_OPCODE(text_lt, text/text, 99, NULL)
__FUNC_LOCALE_OPCODE(text_le, text/text, 99, NULL)
__FUNC_LOCALE_OPCODE(text_gt, text/text, 99, NULL)
__FUNC_LOCALE_OPCODE(text_ge, text/text, 99, NULL)
FUNC_OPCODE(length, text, DEVKIND__ANY, textlen, 99, NULL)

/* LIKE operators */
__FUNC_OPCODE(like, text/text, 800, NULL)
__FUNC_OPCODE(textlike, text/text, 800, NULL)
__FUNC_OPCODE(bpcharlike, bpchar/text, 800, NULL)
__FUNC_OPCODE(notlike, text/text, 800, NULL)
__FUNC_OPCODE(textnlike, text/text, 800, NULL)
__FUNC_OPCODE(bpcharnlike, bpchar/text, 800, NULL)	
__FUNC_OPCODE(texticlike, text/text, 800, NULL)
__FUNC_OPCODE(bpchariclike, bpchar/text, 800, NULL)
__FUNC_OPCODE(texticnlike, text/text, 800, NULL)
__FUNC_OPCODE(bpcharicnlike, bpchar/text, 800, NULL)

/* String operations */
//__FUNC_OPCODE(textcat, text/text, NULL)
//__FUNC_OPCODE(concat, __text__, NULL)

#undef EXPR_OPCODE
#undef TYPE_OPCODE
#undef FUNC_OPCODE
