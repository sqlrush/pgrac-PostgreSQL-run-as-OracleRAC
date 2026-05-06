/*-------------------------------------------------------------------------
 *
 * cluster_undo_xlog.c
 *	  pgrac undo segment WAL resource manager (RM_CLUSTER_UNDO).
 *
 *	  Stage 1.22 ships ONE subtype: XLOG_UNDO_SEGMENT_INIT, emitted by
 *	  cluster_undo_segment_allocate when block 0 of a fresh seg_<id>.dat
 *	  file is initialized.  redo handler reads the payload and pwrites
 *	  the page image directly to the segment file via cluster_undo_path
 *	  (no PG buffer manager / smgr involvement -- pg_undo files live
 *	  outside PG's RelFileLocator namespace).
 *
 *	  The payload identification scheme (instance + segment_id) is the
 *	  v0.2 P1-A 修订: XLOG_FPI assumes RelFileLocator/ForkNumber/BlockNumber
 *	  block tags routed via XLogReadBufferForRedo() -> smgr -> relpath(),
 *	  which is incompatible with $PGDATA/pg_undo/instance_N/seg_M.dat.
 *	  Custom RM record bypasses that routing layer entirely.
 *
 *	  Spec: spec-1.22-undo-tablespace-bootstrap.md §D14a.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/backend/cluster/storage/cluster_undo_xlog.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "cluster/storage/cluster_undo_xlog.h"
#include "miscadmin.h"
#include "storage/bufpage.h"
#include "storage/fd.h"


/*
 * Build a path: $PGDATA/pg_undo/instance_<N>/seg_<segment_id>.dat
 *
 *   Both encoder (cluster_undo_emit_segment_init) and decoder
 *   (cluster_undo_redo) use this layout.  pg_undo subdir is
 *   established by initdb (D4).  instance_<N> subdir is created
 *   on demand (Stage 1.22 ships only instance_0 from initdb).
 *
 *   Returns 0 on success, -1 on path-too-long.  Caller supplies
 *   buf with capacity >= MAXPGPATH.
 */
static int
build_undo_segment_path(uint8 instance, uint32 segment_id, char *buf, size_t buf_size)
{
	int ret;

	ret = snprintf(buf, buf_size, "%s/pg_undo/instance_%u/seg_%u.dat", DataDir, (unsigned)instance,
				   (unsigned)segment_id);
	if (ret < 0 || (size_t)ret >= buf_size)
		return -1;
	return 0;
}


/*
 * cluster_undo_emit_segment_init
 *
 *   Backend caller emits XLOG_UNDO_SEGMENT_INIT for the just-written
 *   segment header block.  page_image is the 8 KB page bytes (typically
 *   just produced by PageInitUndoSegmentHeader on a locked buffer; the
 *   caller copies them here so the WAL record is self-contained -- the
 *   redo handler doesn't need a buffer manager lookup).
 */
XLogRecPtr
cluster_undo_emit_segment_init(uint8 instance, uint32 segment_id, const char *page_image)
{
	xl_cluster_undo_segment_init hdr;
	XLogRecPtr lsn;

	Assert(instance >= 1);
	Assert(page_image != NULL);

	memset(&hdr, 0, sizeof(hdr));
	hdr.instance = instance;
	hdr.segment_id = segment_id;

	XLogBeginInsert();
	XLogRegisterData((char *)&hdr, sizeof(hdr));
	XLogRegisterData((char *)page_image, BLCKSZ);

	lsn = XLogInsert(RM_CLUSTER_UNDO_ID, XLOG_UNDO_SEGMENT_INIT);

	return lsn;
}


/*
 * Replay XLOG_UNDO_SEGMENT_INIT.
 *
 *   Direct pwrite to $PGDATA/pg_undo/instance_<N>/seg_<id>.dat block 0.
 *   Bypasses PG buffer manager + smgr.  Idempotent: if the segment file
 *   already exists, overwrites block 0 with the WAL-shipped image.
 *
 *   Errors during recovery promote to PANIC per the standard recovery
 *   contract (a half-replayed undo segment is corruption).
 */
static void
cluster_undo_redo_segment_init(XLogReaderState *record)
{
	xl_cluster_undo_segment_init *hdr;
	char *payload;
	const char *page_image;
	char path[MAXPGPATH];
	int fd;
	ssize_t written;

	payload = XLogRecGetData(record);
	if (XLogRecGetDataLen(record) != sizeof(*hdr) + BLCKSZ)
		ereport(PANIC, (errmsg("invalid XLOG_UNDO_SEGMENT_INIT record length: %u",
							   XLogRecGetDataLen(record))));

	hdr = (xl_cluster_undo_segment_init *)payload;
	page_image = payload + sizeof(*hdr);

	if (build_undo_segment_path(hdr->instance, hdr->segment_id, path, sizeof(path)) != 0)
		ereport(PANIC, (errmsg("undo segment path too long: instance=%u seg=%u", hdr->instance,
							   hdr->segment_id)));

	/*
	 * Open the segment file.  Stage 1.22 only redos block 0 init for an
	 * existing segment; segment file creation is the allocator's job.
	 * If the segment file is missing during recovery, the allocator's
	 * own WAL records (or initdb-time seed) should have appeared earlier
	 * in the WAL stream -- if they didn't, this is corruption.
	 */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		ereport(PANIC, (errcode_for_file_access(),
						errmsg("could not open undo segment file \"%s\": %m", path)));

	written = pg_pwrite(fd, page_image, BLCKSZ, 0);
	if (written != BLCKSZ) {
		int save_errno = errno;

		close(fd);
		errno = save_errno;
		ereport(PANIC, (errcode_for_file_access(),
						errmsg("could not write undo segment header for \"%s\": "
							   "wrote %zd of %d bytes",
							   path, written, BLCKSZ)));
	}

	if (pg_fsync(fd) != 0) {
		int save_errno = errno;

		close(fd);
		errno = save_errno;
		ereport(PANIC, (errcode_for_file_access(),
						errmsg("could not fsync undo segment file \"%s\": %m", path)));
	}

	if (close(fd) != 0)
		ereport(PANIC, (errcode_for_file_access(),
						errmsg("could not close undo segment file \"%s\": %m", path)));
}


/*
 * cluster_undo_redo -- RM_CLUSTER_UNDO redo handler entry point.
 *
 *   Dispatches by xl_info & XLR_INFO_MASK after stripping the framework
 *   bits (XLR_SPECIAL_REL_UPDATE etc.; we don't use those in stage 1.22).
 */
void
cluster_undo_redo(XLogReaderState *record)
{
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info) {
	case XLOG_UNDO_SEGMENT_INIT:
		cluster_undo_redo_segment_init(record);
		break;
	default:
		ereport(PANIC, (errmsg("cluster_undo_redo: unknown op code %u", info)));
	}
}


/*
 * NOTE: cluster_undo_desc + cluster_undo_identify live in
 * src/backend/access/rmgrdesc/clusterundodesc.c so they are picked up
 * by both the backend xlog.o linker and the frontend pg_waldump build
 * (which globs src/backend/access/rmgrdesc/*desc.c into its OBJS).
 */
