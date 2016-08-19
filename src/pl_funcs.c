/* ------------------------------------------------------------------------
 *
 * pl_funcs.c
 *		Utility C functions for stored procedures
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#include "pathman.h"
#include "init.h"
#include "utils.h"

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/xact.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/array.h"
#include "utils/memutils.h"


#include "miscadmin.h"

/* declarations */
PG_FUNCTION_INFO_V1( on_partitions_created );
PG_FUNCTION_INFO_V1( on_partitions_updated );
PG_FUNCTION_INFO_V1( on_partitions_removed );
PG_FUNCTION_INFO_V1( on_enable_parent );
PG_FUNCTION_INFO_V1( on_disable_parent );
PG_FUNCTION_INFO_V1( get_parent_of_partition_pl );
PG_FUNCTION_INFO_V1( get_attribute_type_name );
PG_FUNCTION_INFO_V1( find_or_create_range_partition);
PG_FUNCTION_INFO_V1( get_range_by_idx );
PG_FUNCTION_INFO_V1( get_range_by_part_oid );
PG_FUNCTION_INFO_V1( get_min_range_value );
PG_FUNCTION_INFO_V1( get_max_range_value );
PG_FUNCTION_INFO_V1( get_type_hash_func );
PG_FUNCTION_INFO_V1( get_hash_part_idx );
PG_FUNCTION_INFO_V1( check_overlap );
PG_FUNCTION_INFO_V1( build_range_condition );
PG_FUNCTION_INFO_V1( build_check_constraint_name_attnum );
PG_FUNCTION_INFO_V1( build_check_constraint_name_attname );
PG_FUNCTION_INFO_V1( build_update_trigger_func_name );
PG_FUNCTION_INFO_V1( build_update_trigger_name );
PG_FUNCTION_INFO_V1( is_date_type );
PG_FUNCTION_INFO_V1( is_attribute_nullable );
PG_FUNCTION_INFO_V1( debug_capture );

/* pathman_range type */
typedef struct PathmanRange
{
	Oid			type_oid;
	bool		by_val;
	RangeEntry	range;
} PathmanRange;

typedef struct PathmanHash
{
	Oid			child_oid;
	uint32		hash;
} PathmanHash;

typedef struct PathmanRangeListCtxt
{
	Oid			type_oid;
	bool		by_val;
	RangeEntry *ranges;
	int			nranges;
	int			pos;
} PathmanRangeListCtxt;

PG_FUNCTION_INFO_V1( pathman_range_in );
PG_FUNCTION_INFO_V1( pathman_range_out );

static void on_partitions_created_internal(Oid partitioned_table, bool add_callbacks);
static void on_partitions_updated_internal(Oid partitioned_table, bool add_callbacks);
static void on_partitions_removed_internal(Oid partitioned_table, bool add_callbacks);


/*
 * Callbacks.
 */

static void
on_partitions_created_internal(Oid partitioned_table, bool add_callbacks)
{
	elog(DEBUG2, "on_partitions_created() [add_callbacks = %s] "
				 "triggered for relation %u",
		 (add_callbacks ? "true" : "false"), partitioned_table);
}

static void
on_partitions_updated_internal(Oid partitioned_table, bool add_callbacks)
{
	/* TODO: shall we emit relcache invalidation event here? */
	elog(DEBUG2, "on_partitions_updated() [add_callbacks = %s] "
				 "triggered for relation %u",
		 (add_callbacks ? "true" : "false"), partitioned_table);
}

static void
on_partitions_removed_internal(Oid partitioned_table, bool add_callbacks)
{
	elog(DEBUG2, "on_partitions_removed() [add_callbacks = %s] "
				 "triggered for relation %u",
		 (add_callbacks ? "true" : "false"), partitioned_table);
}

/*
 * Thin layer between pure C and pl/PgSQL.
 */

Datum
on_partitions_created(PG_FUNCTION_ARGS)
{
	on_partitions_created_internal(PG_GETARG_OID(0), true);
	PG_RETURN_NULL();
}

Datum
on_partitions_updated(PG_FUNCTION_ARGS)
{
	on_partitions_updated_internal(PG_GETARG_OID(0), true);
	PG_RETURN_NULL();
}

Datum
on_partitions_removed(PG_FUNCTION_ARGS)
{
	on_partitions_removed_internal(PG_GETARG_OID(0), true);
	PG_RETURN_NULL();
}


/*
 * Get parent of a specified partition.
 */
Datum
get_parent_of_partition_pl(PG_FUNCTION_ARGS)
{
	Oid					partition = PG_GETARG_OID(0);
	PartParentSearch	parent_search;
	Oid					parent;

	/* Fetch parent & write down search status */
	parent = get_parent_of_partition(partition, &parent_search);

	/* We MUST be sure :) */
	Assert(parent_search != PPS_NOT_SURE);

	/* It must be parent known by pg_pathman */
	if (parent_search == PPS_ENTRY_PART_PARENT)
		PG_RETURN_OID(parent);
	else
	{
		elog(ERROR, "\%s\" is not pg_pathman's partition",
			 get_rel_name_or_relid(partition));

		PG_RETURN_NULL();
	}
}

/*
 * Get type (as text) of a given attribute.
 */
Datum
get_attribute_type_name(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	text	   *attname = PG_GETARG_TEXT_P(1);
	char	   *result;
	HeapTuple	tp;

	/* NOTE: for now it's the most efficient way */
	tp = SearchSysCacheAttName(relid, text_to_cstring(attname));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		result = format_type_be(att_tup->atttypid);
		ReleaseSysCache(tp);

		PG_RETURN_TEXT_P(cstring_to_text(result));
	}
	else
		elog(ERROR, "Cannot find type name for attribute \"%s\" "
					"of relation \"%s\"",
			 text_to_cstring(attname), get_rel_name_or_relid(relid));

	PG_RETURN_NULL(); /* keep compiler happy */
}

Datum
on_enable_parent(PG_FUNCTION_ARGS)
{
	Oid		relid = DatumGetObjectId(PG_GETARG_DATUM(0));

	set_enable_parent(relid, true);
	PG_RETURN_NULL();
}

Datum
on_disable_parent(PG_FUNCTION_ARGS)
{
	Oid		relid = DatumGetObjectId(PG_GETARG_DATUM(0));

	set_enable_parent(relid, false);
	PG_RETURN_NULL();
}

/*
 * Returns partition oid for specified parent relid and value.
 * In case when partition doesn't exist try to create one.
 */
Datum
find_or_create_range_partition(PG_FUNCTION_ARGS)
{
	Oid						parent_oid = PG_GETARG_OID(0);
	Datum					value = PG_GETARG_DATUM(1);
	Oid						value_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
	const PartRelationInfo *prel;
	FmgrInfo				cmp_func;
	RangeEntry				found_rentry;
	search_rangerel_result	search_state;

	prel = get_pathman_relation_info(parent_oid);

	if (!prel)
		PG_RETURN_NULL();

	fill_type_cmp_fmgr_info(&cmp_func, value_type, prel->atttype);

	/* FIXME: does this function even work? */
	search_state = search_range_partition_eq(value, &cmp_func,prel,
											 &found_rentry);

	/*
	 * If found then just return oid, else create new partitions
	 */
	if (search_state == SEARCH_RANGEREL_FOUND)
		PG_RETURN_OID(found_rentry.child_oid);
	/*
	 * If not found and value is between first and last partitions
	 */
	else if (search_state == SEARCH_RANGEREL_GAP)
		PG_RETURN_NULL();
	else
	{
		Oid child_oid = InvalidOid;

		/* FIXME: useless double-checked lock (no new data) */
		LWLockAcquire(pmstate->load_config_lock, LW_EXCLUSIVE);
		LWLockAcquire(pmstate->edit_partitions_lock, LW_EXCLUSIVE);

		/*
		 * Check if someone else has already created partition.
		 */
		search_state = search_range_partition_eq(value, &cmp_func, prel,
												 &found_rentry);
		if (search_state == SEARCH_RANGEREL_FOUND)
		{
			LWLockRelease(pmstate->load_config_lock);
			LWLockRelease(pmstate->edit_partitions_lock);

			PG_RETURN_OID(found_rentry.child_oid);
		}

		child_oid = create_partitions(parent_oid, value, value_type);

		LWLockRelease(pmstate->load_config_lock);
		LWLockRelease(pmstate->edit_partitions_lock);

		PG_RETURN_OID(child_oid);
	}
}

/*
 * Returns range entry (min, max) (in form of array).
 *
 * arg #1 is the parent's Oid.
 * arg #2 is the partition's Oid.
 */
Datum
get_range_by_part_oid(PG_FUNCTION_ARGS)
{
	Oid						parent_oid = PG_GETARG_OID(0);
	Oid						child_oid = PG_GETARG_OID(1);
	uint32					i;
	RangeEntry			   *ranges;
	const PartRelationInfo *prel;

	prel = get_pathman_relation_info(parent_oid);
	if (!prel)
		elog(ERROR, "Relation \"%s\" is not partitioned by pg_pathman",
			 get_rel_name_or_relid(parent_oid));

	ranges = PrelGetRangesArray(prel);

	/* Look for the specified partition */
	for (i = 0; i < PrelChildrenCount(prel); i++)
		if (ranges[i].child_oid == child_oid)
		{
			ArrayType  *arr;
			Datum		elems[2] = { ranges[i].min, ranges[i].max };

			arr = construct_array(elems, 2, prel->atttype,
								  prel->attlen, prel->attbyval,
								  prel->attalign);

			PG_RETURN_ARRAYTYPE_P(arr);
		}

	elog(ERROR, "Relation \"%s\" has no partition \"%s\"",
		 get_rel_name_or_relid(parent_oid),
		 get_rel_name_or_relid(child_oid));

	PG_RETURN_NULL(); /* keep compiler happy */
}

/*
 * Returns N-th range entry (min, max) (in form of array).
 *
 * arg #1 is the parent's Oid.
 * arg #2 is the index of the range
 *		(if it is negative then the last range will be returned).
 */
Datum
get_range_by_idx(PG_FUNCTION_ARGS)
{
	Oid						parent_oid = PG_GETARG_OID(0);
	int						idx = PG_GETARG_INT32(1);
	Datum					elems[2];
	RangeEntry			   *ranges;
	const PartRelationInfo *prel;

	prel = get_pathman_relation_info(parent_oid);
	if (!prel)
		elog(ERROR, "Relation \"%s\" is not partitioned by pg_pathman",
			 get_rel_name_or_relid(parent_oid));

	if (((uint32) abs(idx)) >= PrelChildrenCount(prel))
		elog(ERROR, "Partition #%d does not exist (total amount is %u)",
			 idx, PrelChildrenCount(prel));

	ranges = PrelGetRangesArray(prel);

	if (idx == -1)
		idx = PrelChildrenCount(prel) - 1;
	else if (idx < -1)
		elog(ERROR, "Negative indices other than -1 (last partition) are not allowed");

	elems[0] = ranges[idx].min;
	elems[1] = ranges[idx].max;

	PG_RETURN_ARRAYTYPE_P(construct_array(elems, 2,
										  prel->atttype,
										  prel->attlen,
										  prel->attbyval,
										  prel->attalign));
}

/*
 * Returns min value of the first range for relation.
 */
Datum
get_min_range_value(PG_FUNCTION_ARGS)
{
	Oid						parent_oid = PG_GETARG_OID(0);
	RangeEntry			   *ranges;
	const PartRelationInfo *prel;

	prel = get_pathman_relation_info(parent_oid);
	if (!prel)
		elog(ERROR, "Relation \"%s\" is not partitioned by pg_pathman",
			 get_rel_name_or_relid(parent_oid));

	if (prel->parttype != PT_RANGE)
		if (!prel)
			elog(ERROR, "Relation \"%s\" is not partitioned by RANGE",
				 get_rel_name_or_relid(parent_oid));

	ranges = PrelGetRangesArray(prel);

	PG_RETURN_DATUM(ranges[0].min);
}

/*
 * Returns max value of the last range for relation.
 */
Datum
get_max_range_value(PG_FUNCTION_ARGS)
{
	Oid						parent_oid = PG_GETARG_OID(0);
	RangeEntry			   *ranges;
	const PartRelationInfo *prel;

	prel = get_pathman_relation_info(parent_oid);
	if (!prel)
		elog(ERROR, "Relation \"%s\" is not partitioned by pg_pathman",
			 get_rel_name_or_relid(parent_oid));

	if (prel->parttype != PT_RANGE)
		if (!prel)
			elog(ERROR, "Relation \"%s\" is not partitioned by RANGE",
				 get_rel_name_or_relid(parent_oid));

	ranges = PrelGetRangesArray(prel);

	PG_RETURN_DATUM(ranges[PrelChildrenCount(prel) - 1].max);
}

/*
 * Checks if range overlaps with existing partitions.
 * Returns TRUE if overlaps and FALSE otherwise.
 */
Datum
check_overlap(PG_FUNCTION_ARGS)
{
	Oid						parent_oid = PG_GETARG_OID(0);

	Datum					p1 = PG_GETARG_DATUM(1),
							p2 = PG_GETARG_DATUM(2);

	Oid						p1_type = get_fn_expr_argtype(fcinfo->flinfo, 1),
							p2_type = get_fn_expr_argtype(fcinfo->flinfo, 2);

	FmgrInfo				cmp_func_1,
							cmp_func_2;

	uint32					i;
	RangeEntry			   *ranges;
	const PartRelationInfo *prel;

	prel = get_pathman_relation_info(parent_oid);
	if (!prel)
		elog(ERROR, "Relation \"%s\" is not partitioned by pg_pathman",
			 get_rel_name_or_relid(parent_oid));

	if (prel->parttype != PT_RANGE)
		if (!prel)
			elog(ERROR, "Relation \"%s\" is not partitioned by RANGE",
				 get_rel_name_or_relid(parent_oid));

	/* comparison functions */
	fill_type_cmp_fmgr_info(&cmp_func_1, p1_type, prel->atttype);
	fill_type_cmp_fmgr_info(&cmp_func_2, p2_type, prel->atttype);

	ranges = PrelGetRangesArray(prel);
	for (i = 0; i < PrelChildrenCount(prel); i++)
	{
		int c1 = FunctionCall2(&cmp_func_1, p1, ranges[i].max);
		int c2 = FunctionCall2(&cmp_func_2, p2, ranges[i].min);

		if (c1 < 0 && c2 > 0)
			PG_RETURN_BOOL(true);
	}

	PG_RETURN_BOOL(false);
}


/*
 * HASH-related stuff.
 */

/* Returns hash function's OID for a specified type. */
Datum
get_type_hash_func(PG_FUNCTION_ARGS)
{
	TypeCacheEntry *tce;
	Oid 			type_oid = PG_GETARG_OID(0);

	tce = lookup_type_cache(type_oid, TYPECACHE_HASH_PROC);

	PG_RETURN_OID(tce->hash_proc);
}

/* Wrapper for hash_to_part_index() */
Datum
get_hash_part_idx(PG_FUNCTION_ARGS)
{
	uint32	value = PG_GETARG_UINT32(0),
			part_count = PG_GETARG_UINT32(1);

	PG_RETURN_UINT32(hash_to_part_index(value, part_count));
}

/*
 * Traits.
 */

Datum
is_date_type(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(is_date_type_internal(PG_GETARG_OID(0)));
}

Datum
is_attribute_nullable(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	text	   *attname = PG_GETARG_TEXT_P(1);
	bool		result = true;
	HeapTuple	tp;

	tp = SearchSysCacheAttName(relid, text_to_cstring(attname));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		result = !att_tup->attnotnull;
		ReleaseSysCache(tp);
	}
	else
		elog(ERROR, "Cannot find type name for attribute \"%s\" "
					"of relation \"%s\"",
			 text_to_cstring(attname), get_rel_name_or_relid(relid));

	PG_RETURN_BOOL(result); /* keep compiler happy */
}


/*
 * Useful string builders.
 */

/* Build range condition for a CHECK CONSTRAINT. */
Datum
build_range_condition(PG_FUNCTION_ARGS)
{
	text   *attname = PG_GETARG_TEXT_P(0);

	Datum	min_bound = PG_GETARG_DATUM(1),
			max_bound = PG_GETARG_DATUM(2);

	Oid		min_bound_type = get_fn_expr_argtype(fcinfo->flinfo, 1),
			max_bound_type = get_fn_expr_argtype(fcinfo->flinfo, 2);

	char   *subst_str; /* substitution string */
	char   *result;

	/* This is not going to trigger (not now, at least), just for the safety */
	if (min_bound_type != max_bound_type)
		elog(ERROR, "Cannot build range condition: "
					"boundaries should be of the same type");

	/* Check if we need single quotes */
	/* TODO: check for primitive types instead, that would be better */
	if (is_date_type_internal(min_bound_type) ||
		is_string_type_internal(min_bound_type))
	{
		subst_str = "%1$s >= '%2$s' AND %1$s < '%3$s'";
	}
	else
		subst_str = "%1$s >= %2$s AND %1$s < %3$s";

	/* Create range condition CSTRING */
	result = psprintf(subst_str,
					  text_to_cstring(attname),
					  datum_to_cstring(min_bound, min_bound_type),
					  datum_to_cstring(max_bound, max_bound_type));

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

Datum
build_check_constraint_name_attnum(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	AttrNumber	attnum = PG_GETARG_INT16(1);
	const char *result;

	if (get_rel_type_id(relid) == InvalidOid)
		elog(ERROR, "Invalid relation %u", relid);

	/* We explicitly do not support system attributes */
	if (attnum == InvalidAttrNumber || attnum < 0)
		elog(ERROR, "Cannot build check constraint name: "
					"invalid attribute number %i", attnum);

	result = build_check_constraint_name_internal(relid, attnum);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

Datum
build_check_constraint_name_attname(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	text	   *attname = PG_GETARG_TEXT_P(1);
	AttrNumber	attnum = get_attnum(relid, text_to_cstring(attname));
	const char *result;

	if (get_rel_type_id(relid) == InvalidOid)
		elog(ERROR, "Invalid relation %u", relid);

	if (attnum == InvalidAttrNumber)
		elog(ERROR, "Relation \"%s\" has no column '%s'",
			 get_rel_name_or_relid(relid), text_to_cstring(attname));

	result = build_check_constraint_name_internal(relid, attnum);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

Datum
build_update_trigger_func_name(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0),
				nspid;
	const char *result;

	/* Check that relation exists */
	if (get_rel_type_id(relid) == InvalidOid)
		elog(ERROR, "Invalid relation %u", relid);

	nspid = get_rel_namespace(relid);
	result = psprintf("%s.%s",
					  quote_identifier(get_namespace_name(nspid)),
					  quote_identifier(psprintf("%s_upd_trig_func",
												get_rel_name(relid))));

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

Datum
build_update_trigger_name(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	const char *result; /* trigger's name can't be qualified */

	/* Check that relation exists */
	if (get_rel_type_id(relid) == InvalidOid)
		elog(ERROR, "Invalid relation %u", relid);

	result = quote_identifier(psprintf("%s_upd_trig", get_rel_name(relid)));

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * NOTE: used for DEBUG, set breakpoint here.
 */
Datum
debug_capture(PG_FUNCTION_ARGS)
{
	static float8 sleep_time = 0;
	DirectFunctionCall1(pg_sleep, Float8GetDatum(sleep_time));

	/* Write something (doesn't really matter) */
	elog(WARNING, "debug_capture [%u]", MyProcPid);

	PG_RETURN_VOID();
}

Datum
pathman_range_in(PG_FUNCTION_ARGS)
{
	elog(ERROR, "Not implemented");
}

Datum
pathman_range_out(PG_FUNCTION_ARGS)
{
	PathmanRange *rng = (PathmanRange *) PG_GETARG_POINTER(0);
	char	   *result;
	char	   *left,
			   *right;
	Oid			outputfunc;
	bool		typisvarlena;

	getTypeOutputInfo(rng->type_oid, &outputfunc, &typisvarlena);
	left = OidOutputFunctionCall(outputfunc, PATHMAN_GET_DATUM(rng->range.min, rng->by_val));
	right = OidOutputFunctionCall(outputfunc, PATHMAN_GET_DATUM(rng->range.max, rng->by_val));

	result = psprintf("[%s: %s)", left, right);
	PG_RETURN_CSTRING(result);
}
