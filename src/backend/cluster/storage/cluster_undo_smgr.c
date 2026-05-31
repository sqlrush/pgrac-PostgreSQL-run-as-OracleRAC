/*-------------------------------------------------------------------------
 *
 * cluster_undo_smgr.c
 *	  pgrac undo segment file I/O abstraction layer (spec-3.7 D7 carryover,
 *	  spec-3.8 真 ship implementation).
 *
 *	  Provides block-level read/write/create/fsync of per-instance undo
 *	  segment files.  Wraps the same BasicOpenFile + pg_pread/pwrite +
 *	  pg_fsync I/O pattern used inline by cluster_undo_record.c +
 *	  cluster_undo_alloc.c.
 *
 *	  The spec-3.8 hardening path wires cluster_undo_record.c and the
 *	  lifecycle helpers through this layer so undo I/O has one block-level
 *	  abstraction.  spec-3.9 CR construction + spec-3.10 CR cache will hook
 *	  above this layer.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * Spec: spec-3.8-undo-segment-lifecycle-autoextend.md (FROZEN v0.3 +
 *       Hardening v1.0.1;  D7 carryover from spec-3.7 Hardening v1.0.3)
 *
 * IDENTIFICATION
 *	  src/backend/cluster/storage/cluster_undo_smgr.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/ipc.h" /* before_shmem_exit (fd cache cleanup) */
#include "utils/elog.h"

#include "cluster/cluster_undo_record_api.h" /* smgr syscall counter bumps */
#include "cluster/cluster_undo_smgr.h"
#include "cluster/cluster_undo_segment.h"
#include "cluster/storage/cluster_undo_alloc.h"


/*
 * P0 perf hardening (2026-05-31): per-backend undo segment fd cache.
 *
 *	The hot undo write path called open()+pread/pwrite()+close() per record
 *	(8.5 open + 8.5 close per TPC-B txn).  Cache ONE O_RDWR fd for the
 *	most-recently-used (segment, owner) — O_RDWR serves both read_block and
 *	write_block.  Self-heals on (segment, owner) mismatch (close old + open
 *	new), bounding stale-fd exposure (e.g. a recycled segment) further by an
 *	xact-end reset (cluster_undo_record_xact_reset -> _fd_cache_reset) and a
 *	before_shmem_exit close.  Per-backend fds to a shared segment file are
 *	independent; each pwrites its own offset range, so no cross-backend hazard.
 */
static uint32 cached_fd_segment = 0; /* 0 = empty */
static uint8 cached_fd_owner = 0;
static int cached_fd = -1;
static bool cached_fd_exit_registered = false;

static void
fd_cache_close(void)
{
	if (cached_fd >= 0) {
		close(cached_fd);
		cluster_undo_record_note_smgr_close();
		cached_fd = -1;
		cached_fd_segment = 0;
		cached_fd_owner = 0;
	}
}

static void
fd_cache_on_exit(int code, Datum arg)
{
	(void)code;
	(void)arg;
	fd_cache_close();
}

/*
 * get_segment_fd -- return a cached O_RDWR fd for (segment, owner), opening
 *	(and caching) one on a miss.  Returns -1 on open failure.  The caller MUST
 *	NOT close the returned fd (the cache owns it).
 */
static int
get_segment_fd(uint32 segment_id, uint8 owner_instance)
{
	char path[MAXPGPATH];
	int fd;

	if (cached_fd >= 0 && cached_fd_segment == segment_id && cached_fd_owner == owner_instance)
		return cached_fd; /* hit */

	fd_cache_close(); /* miss: drop the stale fd first */

	if (cluster_undo_path_resolve(owner_instance, segment_id, path, sizeof(path)) != 0)
		return -1;
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		return -1;
	cluster_undo_record_note_smgr_open();

	cached_fd = fd;
	cached_fd_segment = segment_id;
	cached_fd_owner = owner_instance;
	if (!cached_fd_exit_registered) {
		before_shmem_exit(fd_cache_on_exit, (Datum)0);
		cached_fd_exit_registered = true;
	}
	return fd;
}

/*
 * cluster_undo_smgr_fd_cache_reset -- close the cached fd (xact end / segment
 *	recycle).  Called from cluster_undo_record_xact_reset.
 */
void
cluster_undo_smgr_fd_cache_reset(void)
{
	fd_cache_close();
}


bool
cluster_undo_smgr_read_block(uint32 segment_id, uint8 owner_instance, uint32 block_no, char *buf)
{
	int fd;
	off_t offset;
	ssize_t nread;
	bool ok;

	if (buf == NULL || block_no >= UNDO_BLOCKS_PER_SEGMENT)
		return false;

	fd = get_segment_fd(segment_id, owner_instance);
	if (fd < 0)
		return false;

	offset = (off_t)block_no * BLCKSZ;
	nread = pg_pread(fd, buf, BLCKSZ, offset);
	cluster_undo_record_note_smgr_pread();
	ok = (nread == BLCKSZ);
	return ok;
}


bool
cluster_undo_smgr_write_block(uint32 segment_id, uint8 owner_instance, uint32 block_no,
							  const char *buf, bool do_fsync)
{
	int fd;
	off_t offset;
	ssize_t nwritten;
	bool ok = true;

	if (buf == NULL || block_no >= UNDO_BLOCKS_PER_SEGMENT)
		return false;

	fd = get_segment_fd(segment_id, owner_instance);
	if (fd < 0)
		return false;

	offset = (off_t)block_no * BLCKSZ;
	nwritten = pg_pwrite(fd, buf, BLCKSZ, offset);
	cluster_undo_record_note_smgr_pwrite();
	if (nwritten != BLCKSZ)
		ok = false;

	if (ok && do_fsync) {
		if (pg_fsync(fd) != 0)
			ok = false;
	}
	return ok;
}


int
cluster_undo_smgr_create_segment_file(uint32 segment_id, uint8 owner_instance)
{
	if (owner_instance < 1 || owner_instance > UNDO_OWNER_INSTANCE_MAX)
		return -2;

	/*
	 * cluster_undo_segment_allocate is idempotent — if file already
	 * exists with valid header, no rewrite;  if it doesn't, create +
	 * init + WAL-protect.  May ereport(ERROR) on FS error.
	 */
	cluster_undo_segment_allocate(segment_id, owner_instance);

	/* Verify file exists after the call. */
	{
		char path[MAXPGPATH];
		int ret;
		int fd;

		ret = cluster_undo_path_resolve(owner_instance, segment_id, path, sizeof(path));
		if (ret != 0)
			return -2;

		fd = BasicOpenFile(path, O_RDONLY | PG_BINARY);
		if (fd < 0)
			return -1;
		close(fd);
	}

	return 0;
}


bool
cluster_undo_smgr_fsync_segment_file(uint32 segment_id, uint8 owner_instance)
{
	int fd;

	fd = get_segment_fd(segment_id, owner_instance);
	if (fd < 0)
		return false;

	return (pg_fsync(fd) == 0);
}
