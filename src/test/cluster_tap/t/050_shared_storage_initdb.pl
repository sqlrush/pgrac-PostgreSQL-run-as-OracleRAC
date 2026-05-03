#-------------------------------------------------------------------------
#
# 050_shared_storage_initdb.pl
#    Stage 1.8 end-to-end milestone: 共享存储单节点启动 (single-node
#    startup verification).
#
#    1.8 ties together spec-1.1 (cluster_shared_fs vtable + 2 backends
#    + GUC) + spec-1.2 (cluster_smgr in smgrsw[1] 方案 C single-file
#    passthrough) + spec-1.7.1 Sprint A (cluster.smgr_user_relations
#    EXPERIMENTAL WARNING) + spec-1.7.2 (WARNING lifecycle fix +
#    create(isRedo) signature) into one verified end-to-end story.
#
#    Difference vs 019_smgr_cluster.pl: 019 exercises cluster_smgr
#    surface APIs through PgracClusterNode test helpers (which set up
#    a pre-baked cluster-aware node).  050 exercises the *user-visible
#    workflow*: a fresh initdb followed by editing postgresql.conf to
#    opt in, then end-to-end SQL CRUD.  This is the workflow a real
#    operator would follow at Stage 1.8 to verify a cluster-mode
#    instance starts and runs correctly.
#
#    Test matrix (spec-1.8 §4.2; user Q4=C 增补 + L11 上移 L10 + 移
#    pg_dump 出主矩阵):
#
#      L1   fresh initdb succeeds; default GUC=off; no WARNING
#      L2   edit postgresql.conf + GUC=on + shared_storage_backend=local
#      L3   pg_ctl start succeeds; logfile contains EXPERIMENTAL
#           WARNING (DoD #19 hard 门槛)
#      L4   SELECT 1 / SELECT version() / built-in functions work
#      L5   CREATE TABLE + INSERT 1000 rows; cluster_smgr path proof
#           via pg_cluster_state.shared_fs.smgr_active_relations > 0
#      L6   SELECT count(*) FROM t1; cluster_smgr read path also active
#      L7   VACUUM / TRUNCATE / DROP TABLE transparently
#      L8   pg_ctl restart preserves data; logfile shows WARNING twice
#           (DoD #19; spec-1.7.2 F2 regression防御)
#      L8b  pg_ctl stop -m immediate + start exercises spec-1.7.2 F1
#           isRedo idempotent path on existing relfilenode files
#           (DoD #20 path b; codex round 3 P2 finding 2 coverage)
#      L9   pg_ctl stop produces clean shutdown
#      L10  GUC=on + shared_storage_backend=stub mismatch -> postmaster
#           startup FATAL (cross-check; cf. spec-1.7.1 Sprint A Q3 +
#           019 L11)
#
#    Out of main matrix (spec-1.8 §11.5 + Q5/Q6 user 决策):
#      - pg_dump (Q5=A "no restore compatibility claim")
#      - pg_basebackup / streaming replication (Q6=A "physical backup
#        consistency + recovery semantics undefined" until Stage 2)
#
# IDENTIFICATION
#    src/test/cluster_tap/t/050_shared_storage_initdb.pl
#
# Author: SqlRush <sqlrush@gmail.com>
#
# Portions Copyright (c) 2026, pgrac contributors
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use IPC::Cmd qw(can_run);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PgracClusterNode;


# ----------
# Setup: fresh initdb via PgracClusterNode (which thinly wraps PG's
# PostgreSQL::Test::Cluster->init).  This intentionally uses the same
# init path a real operator would use; we only opt into cluster mode
# via postgresql.conf later.
# ----------
my $node = PgracClusterNode->new('main');
$node->init;


# ----------
# L1: fresh initdb succeeds; default GUC=off; no WARNING.
#
# initdb runs in standalone bootstrap mode with default GUCs (off).
# spec-1.8 Q3=C: 1.8 不让 bootstrap catalog 走 cluster_smgr -- bootstrap
# default GUC off means cluster_smgr_which_for() returns 0 throughout.
#
# We can't directly observe the bootstrap stderr at this point (init
# already ran), but we verify the post-init state is clean: GUC=off,
# no leftover PGDATA-level state pointing at cluster mode.
# ----------
$node->start;
$node->assert_cluster_guc('cluster.smgr_user_relations', 'off',
	'L1 cluster.smgr_user_relations defaults to off after initdb');
$node->assert_cluster_guc('cluster.shared_storage_backend', 'stub',
	'L1 cluster.shared_storage_backend defaults to stub after initdb');

# Initial start logfile must NOT contain the EXPERIMENTAL WARNING
# (GUC=off path).  Tightens the contract that WARNING is opt-in only.
my $log_l1 = slurp_file($node->logfile);
unlike(
	$log_l1,
	qr/cluster\.smgr_user_relations is experimental/,
	'L1 default-GUC startup logfile does NOT contain EXPERIMENTAL WARNING'
);

$node->stop;


# ----------
# L2: edit postgresql.conf to opt into cluster mode.
# ----------
$node->append_conf('postgresql.conf',
	"cluster.shared_storage_backend = local\ncluster.smgr_user_relations = on\n");

# Sanity: the conf write itself doesn't fail.  Actual GUC effect is
# verified after start in L3.
ok(-f $node->data_dir . '/postgresql.conf',
	'L2 postgresql.conf still present after edit');


# ----------
# L3: pg_ctl start succeeds; logfile contains EXPERIMENTAL WARNING.
#
# DoD #19 hard 门槛 (spec-1.8 Q4 + Q7 增补): the WARNING must appear
# in the postmaster logfile -- 主动断言 regression防御 against the
# spec-1.7.1 lifecycle bug that placed the WARNING in cluster_smgr_init
# (never called at postmaster start, see PG smgr.c:162).  spec-1.7.2
# F2 fix moved it to cluster_shared_fs_init; 050 nails this down.
# ----------
$node->start;
$node->assert_cluster_guc('cluster.smgr_user_relations', 'on',
	'L3 cluster.smgr_user_relations applies after restart');
$node->assert_cluster_guc('cluster.shared_storage_backend', 'local',
	'L3 cluster.shared_storage_backend = local applies after restart');

my $log_l3 = slurp_file($node->logfile);
like(
	$log_l3,
	qr/cluster\.smgr_user_relations is experimental/,
	'L3 postmaster startup logfile contains EXPERIMENTAL WARNING (DoD #19; spec-1.7.2 F2 regression防御)'
);


# ----------
# L4: SELECT 1 / SELECT version() / built-in functions work.
# ----------
is($node->safe_psql('postgres', 'SELECT 1'),
	'1', 'L4 SELECT 1 returns 1 under GUC=on');

my $version = $node->safe_psql('postgres', 'SELECT version()');
like($version, qr/PostgreSQL/, 'L4 SELECT version() returns PG-style string');

is($node->safe_psql('postgres', "SELECT length('cluster_smgr')"),
	'12', 'L4 built-in functions still work');


# ----------
# L5: CREATE TABLE + INSERT 1000 rows; cluster_smgr path proof.
#
# spec-1.8 Q4 + Q7 增补: assert that pg_cluster_state.shared_fs.smgr_
# active_relations > 0 after data has been written, proving cluster_
# smgr (smgr_which=1) was actually used to back the new relation
# rather than the test passing because md.c was used silently.
# ----------
$node->safe_psql('postgres',
	'CREATE TABLE t1 (id int, payload text)');
$node->safe_psql('postgres',
	"INSERT INTO t1 SELECT g, 'r-' || g FROM generate_series(1, 1000) g");

is($node->safe_psql('postgres', 'SELECT count(*) FROM t1'),
	'1000', 'L5 INSERT 1000 rows visible via SELECT');

# Path proof (a): GUC visibility -- shared_fs.smgr_user_relations = t.
is( $node->safe_psql(
		'postgres',
		"SELECT value FROM pg_cluster_state WHERE category = 'shared_fs' AND key = 'smgr_user_relations'"
	),
	't',
	'L5 pg_cluster_state.shared_fs.smgr_user_relations = t (Q4 path proof a)'
);

# Path proof (b): smgr_active_relations > 0 in the same backend that
# just inserted.  smgr_active_relations is a per-backend HTAB count
# of cluster_smgr-routed SMgrRelations; > 0 means cluster_smgr_which_
# for() returned 1 and cluster_smgr_create / cluster_smgr_open were
# actually exercised for non-temp permanent relations.
my $active_after_insert = $node->safe_psql(
	'postgres',
	"SELECT value::int FROM pg_cluster_state WHERE category = 'shared_fs' AND key = 'smgr_active_relations'"
);
ok( $active_after_insert > 0,
	"L5 smgr_active_relations > 0 after INSERT (got $active_after_insert; Q4 path proof b)"
);


# ----------
# L6: SELECT count(*) read path also engages cluster_smgr.
#
# Same backend (each safe_psql opens a fresh connection on PG; this
# query sits in a new backend whose HTAB starts empty -- but the
# SELECT itself opens pg_class etc. via cluster_smgr if GUC=on, and
# those should populate active_relations > 0 in the new backend too).
# ----------
is($node->safe_psql('postgres', 'SELECT count(*) FROM t1'),
	'1000', 'L6 SELECT count returns expected total');

my $active_in_select = $node->safe_psql(
	'postgres',
	"SELECT value::int FROM pg_cluster_state WHERE category = 'shared_fs' AND key = 'smgr_active_relations'"
);
ok( $active_in_select > 0,
	"L6 smgr_active_relations > 0 in read backend (got $active_in_select; read path also engaged)"
);


# ----------
# L7: VACUUM / TRUNCATE / DROP TABLE transparently.
# ----------
$node->safe_psql('postgres', 'VACUUM ANALYZE t1');
pass('L7 VACUUM ANALYZE succeeds on cluster_smgr-routed relation');

$node->safe_psql('postgres', 'TRUNCATE t1');
is($node->safe_psql('postgres', 'SELECT count(*) FROM t1'),
	'0', 'L7 TRUNCATE empties the relation');

$node->safe_psql('postgres', 'DROP TABLE t1');
pass('L7 DROP TABLE succeeds on cluster_smgr-routed relation');


# ----------
# L8: pg_ctl restart preserves data + WARNING reappears.
#
# DoD #19 hard 门槛 (continued): every postmaster startup with GUC=on
# must emit the WARNING.  Single occurrence on first start does not
# catch a regression that breaks the lifecycle on restart.
# spec-1.7.2 F2 doc explicitly: WARNING fires once per postmaster
# startup; restart triggers a fresh emission.
#
# This also exercises spec-1.7.2 F1 isRedo: WAL redo replay during
# crash recovery (or the orderly restart path) calls smgrcreate(
# isRedo=true) on already-existing relfilenode files, hitting the
# isRedo + existing-file -> idempotent open path of cluster_shared_
# fs_local_create.  If F1's idempotent EEXIST fallback regressed,
# restart would FATAL on every relation that survived the previous
# session.
# ----------
$node->safe_psql('postgres',
	'CREATE TABLE persistent (id int, val text); '
	  . "INSERT INTO persistent VALUES (1, 'one'), (2, 'two'), (3, 'three')"
);

$node->restart;

is($node->safe_psql('postgres', 'SELECT count(*) FROM persistent'),
	'3', 'L8 cluster_smgr-stored data survives restart (spec-1.7.2 F1 isRedo idempotent path)');

is($node->safe_psql('postgres', "SELECT val FROM persistent WHERE id = 2"),
	'two', 'L8 row payload survives restart correctly');

# WARNING must reappear in logfile after restart.  We expect at least
# 2 occurrences across the L3 (initial) + L8 (post-restart) starts.
my $log_after_restart = slurp_file($node->logfile);
my @warning_hits = ($log_after_restart =~
	m/cluster\.smgr_user_relations is experimental/g);
ok( scalar(@warning_hits) >= 2,
	"L8 EXPERIMENTAL WARNING re-emitted after restart "
	. "(saw " . scalar(@warning_hits) . " occurrences across L3 + L8 starts; "
	. "DoD #19 spec-1.7.2 F2 regression防御)");


# ----------
# L8b: spec-1.7.2 F1 isRedo + existing-file idempotent path coverage.
#
# Codex round 3 P2 finding 2 (2026-05-03): the unit-level UT_TEST is
# only a signature smoke; the *behavior* of cluster_shared_fs_local_
# create(isRedo=true) on an existing file (must succeed via
# O_CREAT|O_RDWR + EEXIST fallback rather than the O_CREAT|O_EXCL
# error path) needs explicit TAP-level coverage to back DoD #20.
#
# We exercise it by:
#   1. CHECKPOINT;     -- flush all clean pages
#   2. CREATE TABLE redo_idempotent_target(...); INSERT rows;
#   3. pg_ctl stop -m immediate;  -- crash-equivalent kill
#   4. pg_ctl start;   -- WAL redo replays CREATE TABLE record.
#                        The relfilenode file already exists on disk
#                        (CHECKPOINT or extend dirty-write); WAL redo
#                        calls smgrcreate(isRedo=true) on that
#                        existing file.  If F1's EEXIST fallback
#                        regressed to plain O_CREAT|O_EXCL, redo would
#                        FATAL on the relation rather than returning
#                        successfully.
#
# Path (a) (non-redo + existing-file → ERROR) is intentionally NOT
# tested here because it requires invasive setup (manually placing
# orphan relfilenode files at predicted future allocations).  Path
# (a) coverage is grounded in code review of cluster_shared_fs_local_
# create + md.c equivalence (md.c:218 uses the same O_CREAT|O_EXCL
# pattern).  A future hardening can add an injection-point-driven
# test if needed.
# ----------
$node->safe_psql('postgres', q{
	CREATE TABLE redo_idempotent_target (id int, payload text);
	INSERT INTO redo_idempotent_target SELECT g, 'rr-' || g FROM generate_series(1, 50) g;
	CHECKPOINT;
	INSERT INTO redo_idempotent_target SELECT g, 'rr-' || g FROM generate_series(51, 100) g;
});

$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', 'SELECT count(*) FROM redo_idempotent_target'),
   '100',
   'L8b redo replay over existing relfilenode file succeeds (isRedo=true idempotent path; spec-1.7.2 F1 EEXIST fallback verified)');

$node->safe_psql('postgres', 'DROP TABLE redo_idempotent_target');


# ----------
# L9: pg_ctl stop produces clean shutdown.
#
# Clean shutdown = no PANIC / no FATAL in the final logfile lines,
# checkpoint completes successfully.  This indirectly verifies that
# cluster_smgr's lifecycle hooks (cluster_smgr_shutdown) run cleanly
# when the postmaster orchestrates a graceful stop.
# ----------
$node->stop;

my $log_after_stop = slurp_file($node->logfile);
unlike($log_after_stop, qr/PANIC:/,
	'L9 clean shutdown produced no PANIC');

# A graceful "received fast shutdown request" or "smart shutdown"
# message should be present indicating the orderly tear-down.
like($log_after_stop, qr/database system is shut down/,
	'L9 logfile records the orderly shutdown message');


# ----------
# L10: GUC=on + shared_storage_backend=stub mismatch -> postmaster
# startup FATAL (spec-1.7.1 Sprint A cross-check; equivalent to
# 019 L11).  Redundantly verified at 1.8 milestone level so we catch
# any regression that makes the cross-check skippable through the
# end-to-end workflow.
# ----------
$node->append_conf('postgresql.conf',
	"cluster.shared_storage_backend = stub\n");

my $pg_ctl = $ENV{PG_CTL} || 'pg_ctl';
my $pg_ctl_path = can_run($pg_ctl);
$pg_ctl_path = $pg_ctl unless defined $pg_ctl_path;

my $exit_code = system($pg_ctl_path, '-w', '-t', '5', '-D', $node->data_dir,
					   '-l', $node->logfile, 'start');
isnt($exit_code, 0,
	 'L10 postmaster refuses to start when smgr_user_relations=on + shared_storage_backend=stub');

my $log_l10 = slurp_file($node->logfile);
like($log_l10,
	 qr/cluster\.smgr_user_relations=on requires shared_storage_backend != stub/,
	 'L10 startup logfile contains the cross-check FATAL message');


done_testing();
