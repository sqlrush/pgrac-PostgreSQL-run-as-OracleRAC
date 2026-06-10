/*-------------------------------------------------------------------------
 *
 * cluster_wal_state.c
 *	  pgrac ClusterWalState registry I/O (spec-4.2).
 *
 *	  File layout, slot classification and the owner-only write protocol
 *	  are documented in cluster_wal_state.h.  This module implements the
 *	  backend file I/O:
 *
 *	    ensure()           postmaster startup, after the spec-4.1 claim
 *	                       validation: create-once (O_EXCL + L47 fsync
 *	                       discipline) or validate the existing header.
 *	                       Fail-closed FATAL 53RA2.
 *	    publish_active()   phase4 -> CLUSTER_PHASE_RUNNING transition:
 *	                       recovery succeeded, the node is about to
 *	                       serve -- ONLY now does the slot say ACTIVE
 *	                       (spec-4.2 v0.2 P1: never publish ACTIVE
 *	                       before recovery succeeded).  FATAL 53RA2.
 *	    publish_stopped()  clean shutdown only (postmaster exit path,
 *	                       Shutdown < ImmediateShutdown && !FatalError);
 *	                       WARNING on failure -- shutdown is never
 *	                       blocked.  Crash / immediate shutdown leaves
 *	                       the slot ACTIVE with a stale timestamp: the
 *	                       raw material for spec-4.3's crashed
 *	                       inference.
 *	    refresh_own_slot() cluster_stats main loop: best-effort liveness
 *	                       stamp + WAL watermarks.  Gated on reading the
 *	                       own slot back as OK/ACTIVE, which both keeps
 *	                       it EXEC_BACKEND-safe (no inherited statics)
 *	                       and naturally orders it after
 *	                       publish_active().
 *	    read_slot()        pread + classify (readers surface CORRUPT /
 *	                       FOREIGN as UNKNOWN -- spec-4.2 §3.3).
 *
 *	  Slots are 512B sector-shaped with CRC32C torn-write detection; no
 *	  sector atomicity is claimed.  v1 writes are buffered pwrite +
 *	  pg_fsync (the voting-disk best-effort O_DIRECT path is a later
 *	  optimisation; correctness rests on the CRC protocol either way).
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_wal_state.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Spec: spec-4.2-wal-thread-metadata-catalog.md FROZEN v1.0
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_PGRAC_CLUSTER

#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog.h" /* GetXLogWriteRecPtr, GetWALInsertionTimeLine */
#include "cluster/cluster_guc.h"
#include "cluster/cluster_inject.h"
#include "cluster/cluster_scn.h"
#include "cluster/cluster_wal_state.h"
#include "cluster/cluster_wal_thread.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

/*
 * Path builders.  The registry co-exists with the per-thread layout:
 * everything here is a no-op unless cluster.wal_threads_dir is set.
 */
static bool
registry_configured(void)
{
	return cluster_wal_threads_dir != NULL && cluster_wal_threads_dir[0] != '\0'
		   && cluster_wal_thread_id() != XLP_THREAD_ID_LEGACY;
}

static void
registry_path(char *buf, size_t buflen)
{
	snprintf(buf, buflen, "%s/%s", cluster_wal_threads_dir, CLUSTER_WAL_STATE_FILENAME);
}

/*
 * read_block -- pread one 512B block at `off`.
 *
 *	Returns 1 on success, 0 when the file does not exist, -1 on any
 *	other failure (short read => errno EIO so the caller's %m stays
 *	meaningful).
 */
static int
read_block(const char *path, off_t off, void *block)
{
	int fd;
	ssize_t nread;

	fd = BasicOpenFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		return (errno == ENOENT) ? 0 : -1;

	pgstat_report_wait_start(WAIT_EVENT_CLUSTER_WAL_STATE_READ);
	nread = pg_pread(fd, block, CLUSTER_WAL_STATE_SLOT_SIZE, off);
	pgstat_report_wait_end();

	if (nread >= 0 && nread < (ssize_t)CLUSTER_WAL_STATE_SLOT_SIZE)
		errno = EIO;
	{
		int save_errno = errno;

		close(fd);
		errno = save_errno;
	}
	return (nread == (ssize_t)CLUSTER_WAL_STATE_SLOT_SIZE) ? 1 : -1;
}

/*
 * write_block -- pwrite one 512B block at `off` + pg_fsync.
 *
 *	Owner-only callers (the macro-addressed own slot, or the header
 *	during ensure()).  Returns false on failure with errno preserved.
 */
static bool
write_block(const char *path, off_t off, const void *block)
{
	int fd;
	ssize_t nwritten;

	fd = BasicOpenFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		return false;

	pgstat_report_wait_start(WAIT_EVENT_CLUSTER_WAL_STATE_WRITE);
	nwritten = pg_pwrite(fd, block, CLUSTER_WAL_STATE_SLOT_SIZE, off);
	if (nwritten == (ssize_t)CLUSTER_WAL_STATE_SLOT_SIZE && pg_fsync(fd) == 0) {
		pgstat_report_wait_end();
		close(fd);
		return true;
	}
	{
		int save_errno
			= (nwritten >= 0 && nwritten < (ssize_t)CLUSTER_WAL_STATE_SLOT_SIZE) ? EIO : errno;

		pgstat_report_wait_end();
		close(fd);
		errno = save_errno;
	}
	return false;
}

/*
 * cluster_wal_state_ensure
 *
 *	Create-once or validate.  Called from cluster_wal_thread_init()
 *	(postmaster, after the claim validation, before StartupXLOG).
 *	FATAL 53RA2 on corruption or I/O failure -- never a silent
 *	fallback, never an automatic rebuild of a corrupt registry.
 */
bool
cluster_wal_state_ensure(void)
{
	char path[MAXPGPATH];
	int fd;
	ClusterWalStateHeader header;
	int got;
	const char *reason = NULL;

	if (!registry_configured())
		return false;

	registry_path(path, sizeof(path));

	fd = BasicOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (fd >= 0) {
		/* First boot of the whole registry: zero file + header. */
		char *zeros = palloc0(CLUSTER_WAL_STATE_FILE_SIZE);
		ssize_t nwritten;
		bool ok;

		cluster_wal_state_header_fill((ClusterWalStateHeader *)zeros, (int64)GetCurrentTimestamp());

		pgstat_report_wait_start(WAIT_EVENT_CLUSTER_WAL_STATE_WRITE);
		nwritten = pg_pwrite(fd, zeros, CLUSTER_WAL_STATE_FILE_SIZE, 0);
		ok = (nwritten == (ssize_t)CLUSTER_WAL_STATE_FILE_SIZE) && pg_fsync(fd) == 0;
		pgstat_report_wait_end();
		pfree(zeros);

		if (!ok) {
			int save_errno = errno;

			close(fd);
			(void)unlink(path);
			errno = save_errno;
			ereport(FATAL, (errcode(ERRCODE_CLUSTER_WAL_STATE_IO_FAILURE),
							errmsg("could not initialise WAL state registry \"%s\": %m", path),
							errhint("Check that the shared WAL storage is writable.")));
		}
		close(fd);

		/* L47 create side: make the new dirent durable too. */
		fd = BasicOpenFile(cluster_wal_threads_dir, O_RDONLY | PG_BINARY);
		if (fd < 0)
			ereport(FATAL,
					(errcode(ERRCODE_CLUSTER_WAL_STATE_IO_FAILURE),
					 errmsg("could not open WAL threads root \"%s\": %m", cluster_wal_threads_dir),
					 errhint("Check that the shared WAL storage is writable.")));
		if (pg_fsync(fd) != 0) {
			int save_errno = errno;

			close(fd);
			errno = save_errno;
			ereport(FATAL,
					(errcode(ERRCODE_CLUSTER_WAL_STATE_IO_FAILURE),
					 errmsg("could not fsync WAL threads root \"%s\": %m", cluster_wal_threads_dir),
					 errhint("Check that the shared WAL storage is writable.")));
		}
		close(fd);

		ereport(LOG, (errmsg("pgrac WAL state registry created at \"%s\"", path)));
		return true;
	}
	if (errno != EEXIST)
		ereport(FATAL, (errcode(ERRCODE_CLUSTER_WAL_STATE_IO_FAILURE),
						errmsg("could not open WAL state registry \"%s\": %m", path),
						errhint("Check that the shared WAL storage is reachable.")));

	/* Exists (possibly created by a concurrent first boot): validate. */
	got = read_block(path, 0, &header);
	if (got <= 0)
		ereport(FATAL, (errcode(ERRCODE_CLUSTER_WAL_STATE_IO_FAILURE),
						errmsg("could not read WAL state registry header \"%s\": %m", path),
						errhint("The registry is unreadable or torn.  After confirming the "
								"shared storage, remove the file and restart (it is rebuilt "
								"empty; slots repopulate as nodes start).")));
	if (!cluster_wal_state_header_validate(&header, &reason))
		ereport(FATAL, (errcode(ERRCODE_CLUSTER_WAL_STATE_IO_FAILURE),
						errmsg("WAL state registry \"%s\" failed validation", path),
						errdetail("Header validation failed: %s.", reason ? reason : "unknown"),
						errhint("After confirming the shared storage, remove the file and restart "
								"(it is rebuilt empty; slots repopulate as nodes start).")));
	return true;
}

/*
 * fill_own_slot -- collect the live fields for this node's slot.
 */
static void
fill_own_slot(ClusterWalStateSlot *slot, uint32 state, int64 started_at)
{
	cluster_wal_state_slot_fill(slot, cluster_wal_thread_id(), cluster_node_id, state,
								(uint32)GetWALInsertionTimeLine(), started_at,
								(int64)GetCurrentTimestamp(), (uint64)GetXLogWriteRecPtr(),
								(uint64)cluster_scn_current());
}

static bool
write_own_slot(const ClusterWalStateSlot *slot)
{
	char path[MAXPGPATH];

	/* Decision-style injection (spec-4.2 D5): simulate a write failure. */
	if (cluster_injection_should_skip("cluster-wal-state-write-fail"))
		return false;

	registry_path(path, sizeof(path));
	return write_block(path, CLUSTER_WAL_STATE_SLOT_OFFSET(slot->thread_id), slot);
}

/*
 * cluster_wal_state_publish_active
 *
 *	phase4 -> CLUSTER_PHASE_RUNNING transition (cluster_startup_phase.c):
 *	recovery has succeeded and the node is about to serve SQL.  A node
 *	that dies in recovery never reaches this point, so its slot keeps
 *	the previous incarnation's content (or EMPTY on a first boot) --
 *	spec-4.2 v0.2 P1.
 */
void
cluster_wal_state_publish_active(void)
{
	ClusterWalStateSlot slot;

	if (!registry_configured())
		return;

	fill_own_slot(&slot, CLUSTER_WAL_SLOT_STATE_ACTIVE, (int64)GetCurrentTimestamp());
	if (!write_own_slot(&slot))
		ereport(FATAL, (errcode(ERRCODE_CLUSTER_WAL_STATE_IO_FAILURE),
						errmsg("could not publish ACTIVE to the WAL state registry: %m"),
						errhint("Check that the shared WAL storage is writable.")));
	ereport(LOG, (errmsg("pgrac WAL thread %u published ACTIVE in the WAL state registry",
						 (unsigned)slot.thread_id)));
}

/*
 * cluster_wal_state_publish_stopped
 *
 *	Clean shutdown only (the postmaster exit path gates on
 *	Shutdown < ImmediateShutdown && !FatalError).  Failure must never
 *	block a shutdown: WARNING + carry on.  started_at is preserved
 *	from the slot on disk (same incarnation).
 */
void
cluster_wal_state_publish_stopped(void)
{
	ClusterWalStateSlot cur;
	ClusterWalStateSlot slot;
	int64 started_at;

	if (!registry_configured())
		return;

	started_at = (cluster_wal_state_read_slot(cluster_wal_thread_id(), &cur) == CLUSTER_WAL_SLOT_OK)
					 ? cur.started_at
					 : (int64)GetCurrentTimestamp();

	fill_own_slot(&slot, CLUSTER_WAL_SLOT_STATE_STOPPED, started_at);
	if (!write_own_slot(&slot))
		ereport(WARNING, (errcode(ERRCODE_CLUSTER_WAL_STATE_IO_FAILURE),
						  errmsg("could not publish STOPPED to the WAL state registry: %m"),
						  errhint("The slot stays ACTIVE; spec-4.3 readers treat it via the "
								  "staleness inference.")));
}

/*
 * cluster_wal_state_refresh_own_slot
 *
 *	cluster_stats main-loop tick.  Best-effort: gated on reading the
 *	own slot back as OK + ACTIVE (EMPTY before publish_active, or any
 *	classification failure, skips quietly -- ordering and
 *	EXEC_BACKEND safety in one check).  Failures LOG once and bump the
 *	shared counter; they never escalate (spec-4.2 §3.4).
 */
void
cluster_wal_state_refresh_own_slot(void)
{
	ClusterWalStateSlot cur;
	ClusterWalStateSlot slot;

	if (!registry_configured())
		return;

	if (cluster_wal_state_read_slot(cluster_wal_thread_id(), &cur) != CLUSTER_WAL_SLOT_OK
		|| cur.state != CLUSTER_WAL_SLOT_STATE_ACTIVE || cur.node_id != cluster_node_id)
		return;

	fill_own_slot(&slot, CLUSTER_WAL_SLOT_STATE_ACTIVE, cur.started_at);
	if (!write_own_slot(&slot)) {
		if (cluster_wal_thread_refresh_fail_fetch_add() == 0)
			ereport(LOG, (errcode(ERRCODE_CLUSTER_WAL_STATE_IO_FAILURE),
						  errmsg("could not refresh the WAL state registry slot: %m"),
						  errhint("Further refresh failures are counted, not logged "
								  "(cluster.wal_thread.wal_state_refresh_fail_count).")));
	}
}

/*
 * cluster_wal_state_read_slot
 *
 *	Reader-mode pread + classify (expect_node = -1: any owner).
 *	Callers MUST surface CORRUPT/FOREIGN as UNKNOWN.
 */
ClusterWalSlotVerdict
cluster_wal_state_read_slot(uint16 thread_id, ClusterWalStateSlot *slot_out)
{
	char path[MAXPGPATH];
	int got;

	Assert(thread_id >= XLP_THREAD_ID_FIRST_REAL && thread_id <= CLUSTER_WAL_THREAD_MAX);
	if (thread_id < XLP_THREAD_ID_FIRST_REAL || thread_id > CLUSTER_WAL_THREAD_MAX
		|| !registry_configured())
		return CLUSTER_WAL_SLOT_EMPTY;

	registry_path(path, sizeof(path));
	got = read_block(path, CLUSTER_WAL_STATE_SLOT_OFFSET(thread_id), slot_out);
	if (got == 0)
		return CLUSTER_WAL_SLOT_EMPTY;
	if (got < 0)
		return CLUSTER_WAL_SLOT_CORRUPT;

	return cluster_wal_state_slot_classify(slot_out, thread_id, -1, NULL);
}

/*
 * Dump accessors (live reads; no shmem state of their own).
 */
bool
cluster_wal_state_registry_ready(void)
{
	char path[MAXPGPATH];
	ClusterWalStateHeader header;

	if (!registry_configured())
		return false;
	registry_path(path, sizeof(path));
	if (read_block(path, 0, &header) != 1)
		return false;
	return cluster_wal_state_header_validate(&header, NULL);
}

uint64
cluster_wal_state_refresh_fail_count(void)
{
	return cluster_wal_thread_refresh_fail_read();
}

#else /* !USE_PGRAC_CLUSTER */

/* Disable-cluster build: this file compiles to nothing. */

#endif /* USE_PGRAC_CLUSTER */
