/*-------------------------------------------------------------------------
 *
 * test_cluster_basic.c
 *	  Basic sanity unit test for the pgrac cluster subsystem.
 *
 *	  Verifies the unit-test framework itself works end-to-end and
 *	  exercises a minimal call into cluster_version.o.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/test/cluster_unit/test_cluster_basic.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Each test_*.c is a standalone executable; see unit_test.h.
 *
 *-------------------------------------------------------------------------
 */
#include "unit_test.h"
#include "cluster/cluster_version.h"

UT_DEFINE_GLOBALS();


UT_TEST(test_framework_assert_true)
{
	/* cppcheck-suppress duplicateExpression
	 * Reason: intentional tautology to verify the UT_ASSERT macro
	 * accepts a true expression without firing the failure path.
	 */
	UT_ASSERT(1 == 1);
}

UT_TEST(test_framework_assert_eq)
{
	UT_ASSERT_EQ(2 + 2, 4);
}

UT_TEST(test_framework_assert_not_null)
{
	const char *s = "hello";

	UT_ASSERT_NOT_NULL(s);
}

UT_TEST(test_pgrac_version_string_callable)
{
	/*
	 * Smoke test: the function must be callable and return something.
	 * Detailed content checks live in test_cluster_version.c.
	 */
	const char *v = pgrac_version_string();

	UT_ASSERT_NOT_NULL(v);
}


int
main(void)
{
	UT_PLAN(4);
	UT_RUN(test_framework_assert_true);
	UT_RUN(test_framework_assert_eq);
	UT_RUN(test_framework_assert_not_null);
	UT_RUN(test_pgrac_version_string_callable);
	UT_DONE();
	return ut_failed_count == 0 ? 0 : 1;
}
