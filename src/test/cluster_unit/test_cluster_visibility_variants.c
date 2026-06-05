/*-------------------------------------------------------------------------
 *
 * test_cluster_visibility_variants.c
 *	  cluster_unit full enumeration of the spec-3.14 §2.2 OBS truth
 *	  tables (Self / Toast / Update xmin / Update xmax / Dirty).
 *
 *	  These are pure status->verdict functions, so the test enumerates
 *	  every ClusterTTStatus (the executable copy of the spec truth
 *	  tables, L212 single source).  No buffer / no page needed.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/test/cluster_unit/test_cluster_visibility_variants.c
 *
 * NOTES
 *	  This is a pgrac-original file.
 *	  Spec: spec-3.14-remaining-visibility-paths.md (FROZEN v0.2) §2.2.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cluster/cluster_tt_status.h"
#include "cluster/cluster_visibility_resolve.h"

/* Drop PG's port.h printf -> pg_printf override; unit_test.h uses
 * stdlib printf and we don't link libpgport in this test binary. */
#undef printf

#include "unit_test.h"

UT_DEFINE_GLOBALS();


/* The six terminal-or-in-progress states the resolver can hand a verdict. */
static const ClusterTTStatus all_states[]
	= { CLUSTER_TT_STATUS_UNKNOWN, CLUSTER_TT_STATUS_IN_PROGRESS, CLUSTER_TT_STATUS_COMMITTED,
		CLUSTER_TT_STATUS_ABORTED, CLUSTER_TT_STATUS_CLEANED_OUT, CLUSTER_TT_STATUS_SUBCOMMITTED };


/* ---- OBS-4 Self ---- */
UT_TEST(test_obs4_self_full_table)
{
	UT_ASSERT_EQ((int)cluster_vis_self_verdict(CLUSTER_TT_STATUS_COMMITTED), (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_self_verdict(CLUSTER_TT_STATUS_CLEANED_OUT), (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_self_verdict(CLUSTER_TT_STATUS_IN_PROGRESS), (int)CVV_INVISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_self_verdict(CLUSTER_TT_STATUS_SUBCOMMITTED), (int)CVV_INVISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_self_verdict(CLUSTER_TT_STATUS_ABORTED), (int)CVV_INVISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_self_verdict(CLUSTER_TT_STATUS_UNKNOWN),
				 (int)CVV_FAILCLOSED_UNKNOWN);
}

/* ---- OBS-5 Toast (permissive: only ABORTED hides; UNKNOWN must be heard) ---- */
UT_TEST(test_obs5_toast_full_table)
{
	UT_ASSERT_EQ((int)cluster_vis_toast_verdict(CLUSTER_TT_STATUS_ABORTED), (int)CVV_INVISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_toast_verdict(CLUSTER_TT_STATUS_UNKNOWN),
				 (int)CVV_FAILCLOSED_UNKNOWN);
	UT_ASSERT_EQ((int)cluster_vis_toast_verdict(CLUSTER_TT_STATUS_COMMITTED), (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_toast_verdict(CLUSTER_TT_STATUS_CLEANED_OUT), (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_toast_verdict(CLUSTER_TT_STATUS_IN_PROGRESS), (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_toast_verdict(CLUSTER_TT_STATUS_SUBCOMMITTED), (int)CVV_VISIBLE);
}

/* ---- OBS-2 Update xmin gate ---- */
UT_TEST(test_obs2_update_xmin_full_table)
{
	UT_ASSERT_EQ((int)cluster_vis_update_xmin_verdict(CLUSTER_TT_STATUS_COMMITTED),
				 (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_update_xmin_verdict(CLUSTER_TT_STATUS_CLEANED_OUT),
				 (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_update_xmin_verdict(CLUSTER_TT_STATUS_IN_PROGRESS),
				 (int)CVV_INVISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_update_xmin_verdict(CLUSTER_TT_STATUS_SUBCOMMITTED),
				 (int)CVV_INVISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_update_xmin_verdict(CLUSTER_TT_STATUS_ABORTED),
				 (int)CVV_INVISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_update_xmin_verdict(CLUSTER_TT_STATUS_UNKNOWN),
				 (int)CVV_FAILCLOSED_UNKNOWN);
}

/* ---- OBS-2 Update xmax outcome (update vs delete) ---- */
UT_TEST(test_obs2_update_xmax_full_table)
{
	/* update writer */
	UT_ASSERT_EQ((int)cluster_vis_update_xmax_verdict(CLUSTER_TT_STATUS_ABORTED, false),
				 (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_update_xmax_verdict(CLUSTER_TT_STATUS_COMMITTED, false),
				 (int)CVV_GONE_UPDATED);
	UT_ASSERT_EQ((int)cluster_vis_update_xmax_verdict(CLUSTER_TT_STATUS_CLEANED_OUT, false),
				 (int)CVV_GONE_UPDATED);
	UT_ASSERT_EQ((int)cluster_vis_update_xmax_verdict(CLUSTER_TT_STATUS_IN_PROGRESS, false),
				 (int)CVV_BEING_MODIFIED);
	UT_ASSERT_EQ((int)cluster_vis_update_xmax_verdict(CLUSTER_TT_STATUS_SUBCOMMITTED, false),
				 (int)CVV_BEING_MODIFIED);
	UT_ASSERT_EQ((int)cluster_vis_update_xmax_verdict(CLUSTER_TT_STATUS_UNKNOWN, false),
				 (int)CVV_FAILCLOSED_UNKNOWN);
	/* delete writer: committed -> TM_Deleted */
	UT_ASSERT_EQ((int)cluster_vis_update_xmax_verdict(CLUSTER_TT_STATUS_COMMITTED, true),
				 (int)CVV_GONE_DELETED);
	UT_ASSERT_EQ((int)cluster_vis_update_xmax_verdict(CLUSTER_TT_STATUS_ABORTED, true),
				 (int)CVV_VISIBLE);
}

/* ---- OBS-3 Dirty: in-progress -> 53R9H conflict (no wait layer) ---- */
UT_TEST(test_obs3_dirty_full_table)
{
	/* xmin side */
	UT_ASSERT_EQ((int)cluster_vis_dirty_verdict(CLUSTER_TT_STATUS_IN_PROGRESS, false, false),
				 (int)CVV_FAILCLOSED_CONFLICT);
	UT_ASSERT_EQ((int)cluster_vis_dirty_verdict(CLUSTER_TT_STATUS_SUBCOMMITTED, false, false),
				 (int)CVV_FAILCLOSED_CONFLICT);
	UT_ASSERT_EQ((int)cluster_vis_dirty_verdict(CLUSTER_TT_STATUS_COMMITTED, false, false),
				 (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_dirty_verdict(CLUSTER_TT_STATUS_ABORTED, false, false),
				 (int)CVV_INVISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_dirty_verdict(CLUSTER_TT_STATUS_UNKNOWN, false, false),
				 (int)CVV_FAILCLOSED_UNKNOWN);
	/* xmax side */
	UT_ASSERT_EQ((int)cluster_vis_dirty_verdict(CLUSTER_TT_STATUS_IN_PROGRESS, true, false),
				 (int)CVV_FAILCLOSED_CONFLICT);
	UT_ASSERT_EQ((int)cluster_vis_dirty_verdict(CLUSTER_TT_STATUS_ABORTED, true, false),
				 (int)CVV_VISIBLE);
	UT_ASSERT_EQ((int)cluster_vis_dirty_verdict(CLUSTER_TT_STATUS_COMMITTED, true, false),
				 (int)CVV_GONE_UPDATED);
	UT_ASSERT_EQ((int)cluster_vis_dirty_verdict(CLUSTER_TT_STATUS_COMMITTED, true, true),
				 (int)CVV_GONE_DELETED);
}

/* ---- meta: no status maps to an out-of-range verdict (exhaustive sweep) ---- */
UT_TEST(test_all_verdicts_in_range)
{
	int i;

	for (i = 0; i < (int)(sizeof(all_states) / sizeof(all_states[0])); i++) {
		ClusterTTStatus st = all_states[i];

		UT_ASSERT_EQ(cluster_vis_self_verdict(st) <= CVV_FAILCLOSED_CONFLICT, 1);
		UT_ASSERT_EQ(cluster_vis_toast_verdict(st) <= CVV_FAILCLOSED_CONFLICT, 1);
		UT_ASSERT_EQ(cluster_vis_update_xmin_verdict(st) <= CVV_FAILCLOSED_CONFLICT, 1);
		UT_ASSERT_EQ(cluster_vis_update_xmax_verdict(st, false) <= CVV_FAILCLOSED_CONFLICT, 1);
		UT_ASSERT_EQ(cluster_vis_dirty_verdict(st, false, false) <= CVV_FAILCLOSED_CONFLICT, 1);
	}
}


int
main(void)
{
	UT_RUN(test_obs4_self_full_table);
	UT_RUN(test_obs5_toast_full_table);
	UT_RUN(test_obs2_update_xmin_full_table);
	UT_RUN(test_obs2_update_xmax_full_table);
	UT_RUN(test_obs3_dirty_full_table);
	UT_RUN(test_all_verdicts_in_range);

	UT_DONE();
	return ut_failed_count != 0 ? 1 : 0;
}
