/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2018 by Delphix. All rights reserved.
 * Copyright (c) 2015 by Chunwei Chen. All rights reserved.
 * Copyright 2017 Nexenta Systems, Inc.
 */

/* Portions Copyright 2007 Jeremy Teo */
/* Portions Copyright 2010 Robert Milkowski */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <sys/uio_impl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_acl.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/dbuf.h>
#include <sys/policy.h>
#include <sys/zfs_vnops.h>
#include <sys/zfs_quota.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_znode.h>


static ulong_t zfs_fsync_sync_cnt = 4;

int
zfs_fsync(znode_t *zp, int syncflag, cred_t *cr)
{
	int error = 0;
	zfsvfs_t *zfsvfs = ZTOZSB(zp);

	(void) tsd_set(zfs_fsyncer_key, (void *)(uintptr_t)zfs_fsync_sync_cnt);

	if (zfsvfs->z_os->os_sync != ZFS_SYNC_DISABLED) {
		if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
			goto out;
		atomic_inc_32(&zp->z_sync_writes_cnt);
		zil_commit(zfsvfs->z_log, zp->z_id);
		atomic_dec_32(&zp->z_sync_writes_cnt);
		zfs_exit(zfsvfs, FTAG);
	}
out:
	tsd_set(zfs_fsyncer_key, NULL);

	return (error);
}


#if defined(SEEK_HOLE) && defined(SEEK_DATA)
/*
 * Lseek support for finding holes (cmd == SEEK_HOLE) and
 * data (cmd == SEEK_DATA). "off" is an in/out parameter.
 */
static int
zfs_holey_common(znode_t *zp, ulong_t cmd, loff_t *off)
{
	zfs_locked_range_t *lr;
	uint64_t noff = (uint64_t)*off; /* new offset */
	uint64_t file_sz;
	int error;
	boolean_t hole;

	file_sz = zp->z_size;
	if (noff >= file_sz)  {
		return (SET_ERROR(ENXIO));
	}

	if (cmd == F_SEEK_HOLE)
		hole = B_TRUE;
	else
		hole = B_FALSE;

	/* Flush any mmap()'d data to disk */
	if (zn_has_cached_data(zp, 0, file_sz - 1))
		zn_flush_cached_data(zp, B_FALSE);

	lr = zfs_rangelock_enter(&zp->z_rangelock, 0, file_sz, RL_READER);
	error = dmu_offset_next(ZTOZSB(zp)->z_os, zp->z_id, hole, &noff);
	zfs_rangelock_exit(lr);

	if (error == ESRCH)
		return (SET_ERROR(ENXIO));

	/* File was dirty, so fall back to using generic logic */
	if (error == EBUSY) {
		if (hole)
			*off = file_sz;

		return (0);
	}

	/*
	 * We could find a hole that begins after the logical end-of-file,
	 * because dmu_offset_next() only works on whole blocks.  If the
	 * EOF falls mid-block, then indicate that the "virtual hole"
	 * at the end of the file begins at the logical EOF, rather than
	 * at the end of the last block.
	 */
	if (noff > file_sz) {
		ASSERT(hole);
		noff = file_sz;
	}

	if (noff < *off)
		return (error);
	*off = noff;
	return (error);
}

int
zfs_holey(znode_t *zp, ulong_t cmd, loff_t *off)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	error = zfs_holey_common(zp, cmd, off);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}
#endif /* SEEK_HOLE && SEEK_DATA */

int
zfs_access(znode_t *zp, int mode, int flag, cred_t *cr)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (flag & V_ACE_MASK)
#if defined(__linux__)
		error = zfs_zaccess(zp, mode, flag, B_FALSE, cr,
		    kcred->user_ns);
#else
		error = zfs_zaccess(zp, mode, flag, B_FALSE, cr,
		    NULL);
#endif
	else
#if defined(__linux__)
		error = zfs_zaccess_rwx(zp, mode, flag, cr, kcred->user_ns);
#else
		error = zfs_zaccess_rwx(zp, mode, flag, cr, NULL);
#endif

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static uint64_t zfs_vnops_read_chunk_size = 1024 * 1024; /* Tunable */

zfs_direct_enabled_t
zfs_check_direct_enabled(znode_t *zp, int ioflags, int *error)
{
	zfs_direct_enabled_t is_direct = ZFS_DIRECT_IO_DISABLED;
	zfsvfs_t *zfsvfs = ZTOZSB(zp);

	if ((*error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (ZFS_DIRECT_IO_ERR);

	if (ioflags & O_DIRECT &&
	    zfsvfs->z_os->os_direct != ZFS_DIRECT_DISABLED) {
		is_direct = ZFS_DIRECT_IO_ENABLED;
	} else if (zfsvfs->z_os->os_direct == ZFS_DIRECT_ALWAYS) {
		is_direct = ZFS_DIRECT_IO_ENABLED;
	}

	zfs_exit(zfsvfs, FTAG);

	return (is_direct);
}

/*
 * Determine if direct IO has been requested (either via the O_DIRECT flag or
 * the "direct" dataset property). When inherited by the property only apply
 * the O_DIRECT flag to correctly aligned IO requests. The rational for this
 * is it allows the property to be safely set on a dataset without forcing
 * all of the applications to be aware of the alignment restrictions. When
 * O_DIRECT is explicitly requested by an application return EINVAL if the
 * request is unaligned.  In all cases, if the file has been accessed via
 * mmap(2) then perform buffered IO to keep the mapped region synchronized.
 */
int
zfs_setup_direct(struct znode *zp, zfs_uio_t *uio, zfs_uio_rw_t rw,
    int *ioflagp)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	objset_t *os = zfsvfs->z_os;
	int ioflag = *ioflagp;
	int error = 0;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (os->os_direct == ZFS_DIRECT_DISABLED) {
		error = EAGAIN;
		goto out;

	} else if (os->os_direct == ZFS_DIRECT_ALWAYS &&
	    zfs_uio_page_aligned(uio) &&
	    zfs_uio_blksz_aligned(uio, SPA_MINBLOCKSIZE)) {
		if ((rw == UIO_WRITE && zfs_uio_resid(uio) >= zp->z_blksz) ||
		    (rw == UIO_READ)) {
			ioflag |= O_DIRECT;
		}
	}

	if (ioflag & O_DIRECT) {
		if (!zfs_uio_page_aligned(uio) ||
		    !zfs_uio_blksz_aligned(uio, SPA_MINBLOCKSIZE)) {
			error = SET_ERROR(EINVAL);
			goto out;
		}

		if (zn_has_cached_data(zp, zfs_uio_offset(uio),
		    zfs_uio_offset(uio) + zfs_uio_resid(uio) - 1)) {
			error = SET_ERROR(EAGAIN);
			goto out;
		}

		error = zfs_uio_get_dio_pages_alloc(uio, rw);
		if (error)
			goto out;
	} else {
		error = EAGAIN;
		goto out;
	}

	IMPLY(ioflag & O_DIRECT, uio->uio_extflg & UIO_DIRECT);
	ASSERT0(error);

	*ioflagp = ioflag;
out:
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Read bytes from specified file into supplied buffer.
 *
 *	IN:	zp	- inode of file to be read from.
 *		uio	- structure supplying read location, range info,
 *			  and return buffer.
 *		ioflag	- O_SYNC flags; used to provide FRSYNC semantics.
 *			  O_DIRECT flag; used to bypass page cache.
 *		cr	- credentials of caller.
 *
 *	OUT:	uio	- updated offset and range, buffer filled.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Side Effects:
 *	inode - atime updated if byte count > 0
 */
int
zfs_read(struct znode *zp, zfs_uio_t *uio, int ioflag, cred_t *cr)
{
	(void) cr;
	int error = 0;
	boolean_t frsync = B_FALSE;

	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (zp->z_pflags & ZFS_AV_QUARANTINED) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EACCES));
	}

	/* We don't copy out anything useful for directories. */
	if (Z_ISDIR(ZTOTYPE(zp))) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EISDIR));
	}

	/*
	 * Validate file offset
	 */
	if (zfs_uio_offset(uio) < (offset_t)0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Fasttrack empty reads
	 */
	if (zfs_uio_resid(uio) == 0) {
		zfs_exit(zfsvfs, FTAG);
		return (0);
	}

#ifdef FRSYNC
	/*
	 * If we're in FRSYNC mode, sync out this znode before reading it.
	 * Only do this for non-snapshots.
	 *
	 * Some platforms do not support FRSYNC and instead map it
	 * to O_SYNC, which results in unnecessary calls to zil_commit. We
	 * only honor FRSYNC requests on platforms which support it.
	 */
	frsync = !!(ioflag & FRSYNC);
#endif
	if (zfsvfs->z_log &&
	    (frsync || zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS))
		zil_commit(zfsvfs->z_log, zp->z_id);

	/*
	 * Lock the range against changes.
	 */
	zfs_locked_range_t *lr = zfs_rangelock_enter(&zp->z_rangelock,
	    zfs_uio_offset(uio), zfs_uio_resid(uio), RL_READER);

	/*
	 * If we are reading past end-of-file we can skip
	 * to the end; but we might still need to set atime.
	 */
	if (zfs_uio_offset(uio) >= zp->z_size) {
		error = 0;
		goto out;
	}

	ASSERT(zfs_uio_offset(uio) < zp->z_size);
#if defined(__linux__)
	ssize_t start_offset = zfs_uio_offset(uio);
#endif
	ssize_t chunk_size = zfs_vnops_read_chunk_size;
	ssize_t n = MIN(zfs_uio_resid(uio), zp->z_size - zfs_uio_offset(uio));
	ssize_t start_resid = n;
	ssize_t dio_remaining_resid = 0;

	if (uio->uio_extflg & UIO_DIRECT) {
		/*
		 * All pages for an O_DIRECT request ahve already been mapped
		 * so there's no compelling reason to handle this uio in
		 * smaller chunks.
		 */
		chunk_size = DMU_MAX_ACCESS;

		/*
		 * In the event that the O_DIRECT request is reading the entire
		 * file, it is possible file's length is not page sized
		 * aligned. However, lower layers expect that the Direct I/O
		 * request is page-aligned. In this case, as much of the file
		 * that can be read using Direct I/O happens and the remaining
		 * amount will be read through the ARC.
		 *
		 * This is still consistent with the semantics of Direct I/O in
		 * ZFS as at a minimum the I/O request must be page-aligned.
		 */
		dio_remaining_resid = n - P2ALIGN(n, PAGE_SIZE);
		if (dio_remaining_resid != 0)
			n -= dio_remaining_resid;
	}

	while (n > 0) {
		ssize_t nbytes = MIN(n, chunk_size -
		    P2PHASE(zfs_uio_offset(uio), chunk_size));
#ifdef UIO_NOCOPY
		if (zfs_uio_segflg(uio) == UIO_NOCOPY)
			error = mappedread_sf(zp, nbytes, uio);
		else
#endif
		if (zn_has_cached_data(zp, zfs_uio_offset(uio),
		    zfs_uio_offset(uio) + nbytes - 1) &&
		    !(uio->uio_extflg & UIO_DIRECT)) {
			error = mappedread(zp, nbytes, uio);
		} else {
			error = dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl),
			    uio, nbytes);
		}

		if (error) {
			/* convert checksum errors into IO errors */
			if (error == ECKSUM)
				error = SET_ERROR(EIO);

#if defined(__linux__)
			/*
			 * if we actually read some bytes, bubbling EFAULT
			 * up to become EAGAIN isn't what we want here...
			 *
			 * ...on Linux, at least. On FBSD, doing this breaks.
			 */
			if (error == EFAULT &&
			    (zfs_uio_offset(uio) - start_offset) != 0)
				error = 0;
#endif
			break;
		}

		n -= nbytes;
	}

	int64_t nread = start_resid;
	if (error == 0 && (uio->uio_extflg & UIO_DIRECT) &&
	    dio_remaining_resid != 0) {
		/*
		 * Temporarily remove the UIO_DIRECT flag from the UIO so the
		 * remainder of the file can be read using the ARC.
		 */
		uio->uio_extflg &= ~UIO_DIRECT;
		error = dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl), uio,
		    dio_remaining_resid);
		uio->uio_extflg |= UIO_DIRECT;

		if (error != 0)
			n -= dio_remaining_resid;
	}
	nread -= n;

	dataset_kstats_update_read_kstats(&zfsvfs->z_kstat, nread);
out:
	zfs_rangelock_exit(lr);

	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static void
zfs_clear_setid_bits_if_necessary(zfsvfs_t *zfsvfs, znode_t *zp, cred_t *cr,
    uint64_t *clear_setid_bits_txgp, dmu_tx_t *tx)
{
	zilog_t *zilog = zfsvfs->z_log;
	const uint64_t uid = KUID_TO_SUID(ZTOUID(zp));

	ASSERT(clear_setid_bits_txgp != NULL);
	ASSERT(tx != NULL);

	/*
	 * Clear Set-UID/Set-GID bits on successful write if not
	 * privileged and at least one of the execute bits is set.
	 *
	 * It would be nice to do this after all writes have
	 * been done, but that would still expose the ISUID/ISGID
	 * to another app after the partial write is committed.
	 *
	 * Note: we don't call zfs_fuid_map_id() here because
	 * user 0 is not an ephemeral uid.
	 */
	mutex_enter(&zp->z_acl_lock);
	if ((zp->z_mode & (S_IXUSR | (S_IXUSR >> 3) | (S_IXUSR >> 6))) != 0 &&
	    (zp->z_mode & (S_ISUID | S_ISGID)) != 0 &&
	    secpolicy_vnode_setid_retain(zp, cr,
	    ((zp->z_mode & S_ISUID) != 0 && uid == 0)) != 0) {
		uint64_t newmode;

		zp->z_mode &= ~(S_ISUID | S_ISGID);
		newmode = zp->z_mode;
		(void) sa_update(zp->z_sa_hdl, SA_ZPL_MODE(zfsvfs),
		    (void *)&newmode, sizeof (uint64_t), tx);

		mutex_exit(&zp->z_acl_lock);

		/*
		 * Make sure SUID/SGID bits will be removed when we replay the
		 * log. If the setid bits are keep coming back, don't log more
		 * than one TX_SETATTR per transaction group.
		 */
		if (*clear_setid_bits_txgp != dmu_tx_get_txg(tx)) {
			vattr_t va = {0};

			va.va_mask = ATTR_MODE;
			va.va_nodeid = zp->z_id;
			va.va_mode = newmode;
			zfs_log_setattr(zilog, tx, TX_SETATTR, zp, &va,
			    ATTR_MODE, NULL);
			*clear_setid_bits_txgp = dmu_tx_get_txg(tx);
		}
	} else {
		mutex_exit(&zp->z_acl_lock);
	}
}

/*
 * Write the bytes to a file.
 *
 *	IN:	zp	- znode of file to be written to.
 *		uio	- structure supplying write location, range info,
 *			  and data buffer.
 *		ioflag	- O_APPEND flag set if in append mode.
 *			  O_DIRECT flag; used to bypass page cache.
 *		cr	- credentials of caller.
 *
 *	OUT:	uio	- updated offset and range.
 *
 *	RETURN:	0 if success
 *		error code if failure
 *
 * Timestamps:
 *	ip - ctime|mtime updated if byte count > 0
 */
int
zfs_write(znode_t *zp, zfs_uio_t *uio, int ioflag, cred_t *cr)
{
	int error = 0, error1;
	ssize_t start_resid = zfs_uio_resid(uio);
	uint64_t clear_setid_bits_txg = 0;
	boolean_t o_direct_defer = B_FALSE;

	/*
	 * Fasttrack empty write
	 */
	ssize_t n = start_resid;
	if (n == 0)
		return (0);

	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	sa_bulk_attr_t bulk[4];
	int count = 0;
	uint64_t mtime[2], ctime[2];
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL, &mtime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL, &ctime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_SIZE(zfsvfs), NULL,
	    &zp->z_size, 8);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, 8);

	/*
	 * Callers might not be able to detect properly that we are read-only,
	 * so check it explicitly here.
	 */
	if (zfs_is_readonly(zfsvfs)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EROFS));
	}

	/*
	 * If immutable or not appending then return EPERM.
	 * Intentionally allow ZFS_READONLY through here.
	 * See zfs_zaccess_common()
	 */
	if ((zp->z_pflags & ZFS_IMMUTABLE) ||
	    ((zp->z_pflags & ZFS_APPENDONLY) && !(ioflag & O_APPEND) &&
	    (zfs_uio_offset(uio) < zp->z_size))) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	/*
	 * Validate file offset
	 */
	offset_t woff = ioflag & O_APPEND ? zp->z_size : zfs_uio_offset(uio);
	if (woff < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	const uint64_t max_blksz = zfsvfs->z_max_blksz;

	/*
	 * Pre-fault the pages to ensure slow (eg NFS) pages
	 * don't hold up txg.
	 * Skip this if uio contains loaned arc_buf.
	 */
	if (zfs_uio_prefaultpages(MIN(n, max_blksz), uio)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EFAULT));
	}

	/*
	 * If in append mode, set the io offset pointer to eof.
	 */
	zfs_locked_range_t *lr;
	if (ioflag & O_APPEND) {
		/*
		 * Obtain an appending range lock to guarantee file append
		 * semantics.  We reset the write offset once we have the lock.
		 */
		lr = zfs_rangelock_enter(&zp->z_rangelock, 0, n, RL_APPEND);
		woff = lr->lr_offset;
		if (lr->lr_length == UINT64_MAX) {
			/*
			 * We overlocked the file because this write will cause
			 * the file block size to increase.
			 * Note that zp_size cannot change with this lock held.
			 */
			woff = zp->z_size;
		}
		zfs_uio_setoffset(uio, woff);
		/*
		 * We need to update the starting offset as well because it is
		 * set previously in the ZPL (Linux) and VNOPS (FreeBSD)
		 * layers.
		 */
		zfs_uio_setsoffset(uio, woff);
	} else {
		/*
		 * Note that if the file block size will change as a result of
		 * this write, then this range lock will lock the entire file
		 * so that we can re-write the block safely.
		 */
		lr = zfs_rangelock_enter(&zp->z_rangelock, woff, n, RL_WRITER);
	}

	if (zn_rlimit_fsize(zp, uio)) {
		zfs_rangelock_exit(lr);
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EFBIG));
	}

	const rlim64_t limit = MAXOFFSET_T;

	if (woff >= limit) {
		zfs_rangelock_exit(lr);
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EFBIG));
	}

	if (n > limit - woff)
		n = limit - woff;

	uint64_t end_size = MAX(zp->z_size, woff + n);
	zilog_t *zilog = zfsvfs->z_log;

	const uint64_t uid = KUID_TO_SUID(ZTOUID(zp));
	const uint64_t gid = KGID_TO_SGID(ZTOGID(zp));
	const uint64_t projid = zp->z_projid;

	/*
	 * In the event we are increasing the file block size
	 * (lr_length == UINT64_MAX), we will direct the write to the ARC.
	 * Because zfs_grow_blocksize() will read from the ARC in order to
	 * grow the dbuf, we avoid doing Direct IO here as that would cause
	 * data written to disk to be overwritten by data in the ARC during
	 * the sync phase. Besides writing data twice to disk, we also
	 * want to avoid consistency concerns between data in the the ARC and
	 * on disk while growing the file's blocksize.
	 *
	 * We will only temporarily remove Direct IO and put it back after
	 * we have grown the blocksize. We do this in the event a request
	 * is larger than max_blksz, so further requests to
	 * dmu_write_uio_dbuf() will still issue the requests using Direct
	 * IO.
	 *
	 * As an example:
	 * The first block to file is being written as a 4k request with
	 * a recorsize of 1K. The first 1K issued in the loop below will go
	 * through the ARC; however, the following 3 1K requests will
	 * use Direct IO.
	 */
	if (uio->uio_extflg & UIO_DIRECT && lr->lr_length == UINT64_MAX) {
		uio->uio_extflg &= ~UIO_DIRECT;
		o_direct_defer = B_TRUE;
	}

	/*
	 * Write the file in reasonable size chunks.  Each chunk is written
	 * in a separate transaction; this keeps the intent log records small
	 * and allows us to do more fine-grained space accounting.
	 */
	while (n > 0) {
		woff = zfs_uio_offset(uio);

		if (zfs_id_overblockquota(zfsvfs, DMU_USERUSED_OBJECT, uid) ||
		    zfs_id_overblockquota(zfsvfs, DMU_GROUPUSED_OBJECT, gid) ||
		    (projid != ZFS_DEFAULT_PROJID &&
		    zfs_id_overblockquota(zfsvfs, DMU_PROJECTUSED_OBJECT,
		    projid))) {
			error = SET_ERROR(EDQUOT);
			break;
		}

		arc_buf_t *abuf = NULL;
		if (n >= max_blksz && woff >= zp->z_size &&
		    P2PHASE(woff, max_blksz) == 0 &&
		    zp->z_blksz == max_blksz &&
		    !(uio->uio_extflg & UIO_DIRECT)) {
			/*
			 * This write covers a full block.  "Borrow" a buffer
			 * from the dmu so that we can fill it before we enter
			 * a transaction.  This avoids the possibility of
			 * holding up the transaction if the data copy hangs
			 * up on a pagefault (e.g., from an NFS server mapping).
			 */
			size_t cbytes;

			abuf = dmu_request_arcbuf(sa_get_db(zp->z_sa_hdl),
			    max_blksz);
			ASSERT(abuf != NULL);
			ASSERT(arc_buf_size(abuf) == max_blksz);
			if ((error = zfs_uiocopy(abuf->b_data, max_blksz,
			    UIO_WRITE, uio, &cbytes))) {
				dmu_return_arcbuf(abuf);
				break;
			}
			ASSERT3S(cbytes, ==, max_blksz);
		}

		/*
		 * Start a transaction.
		 */
		dmu_tx_t *tx = dmu_tx_create(zfsvfs->z_os);
		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
		dmu_buf_impl_t *db = (dmu_buf_impl_t *)sa_get_db(zp->z_sa_hdl);
		DB_DNODE_ENTER(db);
		dmu_tx_hold_write_by_dnode(tx, DB_DNODE(db), woff,
		    MIN(n, max_blksz));
		DB_DNODE_EXIT(db);
		zfs_sa_upgrade_txholds(tx, zp);
		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error) {
			dmu_tx_abort(tx);
			if (abuf != NULL)
				dmu_return_arcbuf(abuf);
			break;
		}

		/*
		 * NB: We must call zfs_clear_setid_bits_if_necessary before
		 * committing the transaction!
		 */

		/*
		 * If rangelock_enter() over-locked we grow the blocksize
		 * and then reduce the lock range.  This will only happen
		 * on the first iteration since rangelock_reduce() will
		 * shrink down lr_length to the appropriate size.
		 */
		if (lr->lr_length == UINT64_MAX) {
			uint64_t new_blksz;

			if (zp->z_blksz > max_blksz) {
				/*
				 * File's blocksize is already larger than the
				 * "recordsize" property.  Only let it grow to
				 * the next power of 2.
				 */
				ASSERT(!ISP2(zp->z_blksz));
				new_blksz = MIN(end_size,
				    1 << highbit64(zp->z_blksz));
			} else {
				new_blksz = MIN(end_size, max_blksz);
			}
			zfs_grow_blocksize(zp, new_blksz, tx);
			zfs_rangelock_reduce(lr, woff, n);
		}

		/*
		 * XXX - should we really limit each write to z_max_blksz?
		 * Perhaps we should use SPA_MAXBLOCKSIZE chunks?
		 */
		ssize_t nbytes = MIN(n, max_blksz - P2PHASE(woff, max_blksz));

		ssize_t tx_bytes;
		if (abuf == NULL) {
			tx_bytes = zfs_uio_resid(uio);
			zfs_uio_fault_disable(uio, B_TRUE);
			error = dmu_write_uio_dbuf(sa_get_db(zp->z_sa_hdl),
			    uio, nbytes, tx);
			zfs_uio_fault_disable(uio, B_FALSE);
#ifdef __linux__
			if (error == EFAULT) {
				zfs_clear_setid_bits_if_necessary(zfsvfs, zp,
				    cr, &clear_setid_bits_txg, tx);
				dmu_tx_commit(tx);
				/*
				 * Account for partial writes before
				 * continuing the loop.
				 * Update needs to occur before the next
				 * zfs_uio_prefaultpages, or prefaultpages may
				 * error, and we may break the loop early.
				 */
				if (tx_bytes != zfs_uio_resid(uio))
					n -= tx_bytes - zfs_uio_resid(uio);
				if (zfs_uio_prefaultpages(MIN(n, max_blksz),
				    uio)) {
					break;
				}
				continue;
			}
#endif
			/*
			 * On FreeBSD, EFAULT should be propagated back to the
			 * VFS, which will handle faulting and will retry.
			 */
			if (error != 0 && error != EFAULT) {
				zfs_clear_setid_bits_if_necessary(zfsvfs, zp,
				    cr, &clear_setid_bits_txg, tx);
				dmu_tx_commit(tx);
				break;
			}
			tx_bytes -= zfs_uio_resid(uio);
		} else {
			/* Implied by abuf != NULL: */
			ASSERT3S(n, >=, max_blksz);
			ASSERT0(P2PHASE(woff, max_blksz));
			/*
			 * We can simplify nbytes to MIN(n, maxblksz) since
			 * P2PHASE(woff, max_blksz) is 0, and knowing
			 * n >= max_blksz lets us simplify further:
			 */
			ASSERT3S(nbytes, ==, max_blksz);
			/*
			 * Thus, we're writing a full block at a block-aligned
			 * offset and extending the file past EOF.
			 *
			 * dmu_assign_arcbuf_by_dbuf() will directly assign the
			 * arc buffer to a dbuf.
			 */
			error = dmu_assign_arcbuf_by_dbuf(
			    sa_get_db(zp->z_sa_hdl), woff, abuf, tx);
			if (error != 0) {
				/*
				 * XXX This might not be necessary if
				 * dmu_assign_arcbuf_by_dbuf is guaranteed
				 * to be atomic.
				 */
				zfs_clear_setid_bits_if_necessary(zfsvfs, zp,
				    cr, &clear_setid_bits_txg, tx);
				dmu_return_arcbuf(abuf);
				dmu_tx_commit(tx);
				break;
			}
			ASSERT3S(nbytes, <=, zfs_uio_resid(uio));
			zfs_uioskip(uio, nbytes);
			tx_bytes = nbytes;
		}

		/*
		 * There is a a window where a file's pages can be mmap'ed after
		 * the write has started. We may have temporarily removed the
		 * UIO_DIRECT flag as we are still growing the blocksize but
		 * the O_DIRECT flag is still present. This check for the
		 * O_DIRECT flag has always been present before calling
		 * update_pages(). If we remove the check for O_DIRECT we can
		 * wind up in a deadlock between update_pages() and
		 * zpl_writepages() -> write_cache_pages().
		 *
		 * XXX - In reality, this can probably be fixed by adding a
		 * rangelock to zfs_fillpage(). There has always been a window
		 * where we can start a write, but the pages are not mmapp'ed
		 * till later. Ideally we would want to only check using
		 * zn_has_cached_data() while holding the rangelock to remove
		 * this window and the addition of also grabbing a rangelock
		 * in zfs_fillpage().
		 */
		if (tx_bytes &&
		    zn_has_cached_data(zp, woff, woff + tx_bytes - 1) &&
		    !(ioflag & O_DIRECT)) {
			update_pages(zp, woff, tx_bytes, zfsvfs->z_os);
		}

		/*
		 * If we made no progress, we're done.  If we made even
		 * partial progress, update the znode and ZIL accordingly.
		 */
		if (tx_bytes == 0) {
			(void) sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
			    (void *)&zp->z_size, sizeof (uint64_t), tx);
			dmu_tx_commit(tx);
			ASSERT(error != 0);
			break;
		}

		zfs_clear_setid_bits_if_necessary(zfsvfs, zp, cr,
		    &clear_setid_bits_txg, tx);

		zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, mtime, ctime);

		/*
		 * Update the file size (zp_size) if it has changed;
		 * account for possible concurrent updates.
		 */
		while ((end_size = zp->z_size) < zfs_uio_offset(uio)) {
			(void) atomic_cas_64(&zp->z_size, end_size,
			    zfs_uio_offset(uio));
			ASSERT(error == 0 || error == EFAULT);
		}
		/*
		 * If we are replaying and eof is non zero then force
		 * the file size to the specified eof. Note, there's no
		 * concurrency during replay.
		 */
		if (zfsvfs->z_replay && zfsvfs->z_replay_eof != 0)
			zp->z_size = zfsvfs->z_replay_eof;

		error1 = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);
		if (error1 != 0)
			/* Avoid clobbering EFAULT. */
			error = error1;

		/*
		 * NB: During replay, the TX_SETATTR record logged by
		 * zfs_clear_setid_bits_if_necessary must precede any of
		 * the TX_WRITE records logged here.
		 */
		zfs_log_write(zilog, tx, TX_WRITE, zp, woff, tx_bytes,
		    o_direct_defer ? ioflag & ~O_DIRECT : ioflag, NULL, NULL);

		dmu_tx_commit(tx);

		/*
		 * Direct IO was deferred in order to grow the first block.
		 * At this point it can be re-enabled for subsequent writes.
		 */
		if (o_direct_defer) {
			ASSERT(ioflag & O_DIRECT);
			uio->uio_extflg |= UIO_DIRECT;
			o_direct_defer = B_FALSE;
		}

		if (error != 0)
			break;
		ASSERT3S(tx_bytes, ==, nbytes);
		n -= nbytes;

		if (n > 0) {
			if (zfs_uio_prefaultpages(MIN(n, max_blksz), uio)) {
				error = SET_ERROR(EFAULT);
				break;
			}
		}
	}

	if (o_direct_defer) {
		ASSERT(ioflag & O_DIRECT);
		uio->uio_extflg |= UIO_DIRECT;
		o_direct_defer = B_FALSE;
	}

	zfs_znode_update_vfs(zp);
	zfs_rangelock_exit(lr);

	/*
	 * If we're in replay mode, or we made no progress, or the
	 * uio data is inaccessible return an error.  Otherwise, it's
	 * at least a partial write, so it's successful.
	 */
	if (zfsvfs->z_replay || zfs_uio_resid(uio) == start_resid ||
	    error == EFAULT) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if (ioflag & (O_SYNC | O_DSYNC) ||
	    zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS) {
		zil_commit(zilog, zp->z_id);
	}

	int64_t nwritten = start_resid - zfs_uio_resid(uio);
	dataset_kstats_update_write_kstats(&zfsvfs->z_kstat, nwritten);

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

int
zfs_getsecattr(znode_t *zp, vsecattr_t *vsecp, int flag, cred_t *cr)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;
	boolean_t skipaclchk = (flag & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	error = zfs_getacl(zp, vsecp, skipaclchk, cr);
	zfs_exit(zfsvfs, FTAG);

	return (error);
}

int
zfs_setsecattr(znode_t *zp, vsecattr_t *vsecp, int flag, cred_t *cr)
{
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;
	boolean_t skipaclchk = (flag & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;
	zilog_t	*zilog = zfsvfs->z_log;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	error = zfs_setacl(zp, vsecp, skipaclchk, cr);

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

#ifdef ZFS_DEBUG
static int zil_fault_io = 0;
#endif

static void zfs_get_done(zgd_t *zgd, int error);

/*
 * Get data to generate a TX_WRITE intent log record.
 */
int
zfs_get_data(void *arg, uint64_t gen, lr_write_t *lr, char *buf,
    struct lwb *lwb, zio_t *zio)
{
	zfsvfs_t *zfsvfs = arg;
	objset_t *os = zfsvfs->z_os;
	znode_t *zp;
	uint64_t object = lr->lr_foid;
	uint64_t offset = lr->lr_offset;
	uint64_t size = lr->lr_length;
	zgd_t *zgd;
	int error = 0;
	uint64_t zp_gen;

	ASSERT3P(lwb, !=, NULL);
	ASSERT3P(zio, !=, NULL);
	ASSERT3U(size, !=, 0);

	/*
	 * Nothing to do if the file has been removed
	 */
	if (zfs_zget(zfsvfs, object, &zp) != 0)
		return (SET_ERROR(ENOENT));
	if (zp->z_unlinked) {
		/*
		 * Release the vnode asynchronously as we currently have the
		 * txg stopped from syncing.
		 */
		zfs_zrele_async(zp);
		return (SET_ERROR(ENOENT));
	}
	/* check if generation number matches */
	if (sa_lookup(zp->z_sa_hdl, SA_ZPL_GEN(zfsvfs), &zp_gen,
	    sizeof (zp_gen)) != 0) {
		zfs_zrele_async(zp);
		return (SET_ERROR(EIO));
	}
	if (zp_gen != gen) {
		zfs_zrele_async(zp);
		return (SET_ERROR(ENOENT));
	}

	zgd = kmem_zalloc(sizeof (zgd_t), KM_SLEEP);
	zgd->zgd_lwb = lwb;
	zgd->zgd_private = zp;

	/*
	 * Write records come in two flavors: immediate and indirect.
	 * For small writes it's cheaper to store the data with the
	 * log record (immediate); for large writes it's cheaper to
	 * sync the data and get a pointer to it (indirect) so that
	 * we don't have to write the data twice.
	 */
	if (buf != NULL) { /* immediate write */
		zgd->zgd_lr = zfs_rangelock_enter(&zp->z_rangelock,
		    offset, size, RL_READER);
		/* test for truncation needs to be done while range locked */
		if (offset >= zp->z_size) {
			error = SET_ERROR(ENOENT);
		} else {
			error = dmu_read(os, object, offset, size, buf,
			    DMU_READ_NO_PREFETCH);
		}
		ASSERT(error == 0 || error == ENOENT);
	} else { /* indirect write */
		/*
		 * Have to lock the whole block to ensure when it's
		 * written out and its checksum is being calculated
		 * that no one can change the data. We need to re-check
		 * blocksize after we get the lock in case it's changed!
		 */
		for (;;) {
			uint64_t blkoff;
			size = zp->z_blksz;
			blkoff = ISP2(size) ? P2PHASE(offset, size) : offset;
			offset -= blkoff;
			zgd->zgd_lr = zfs_rangelock_enter(&zp->z_rangelock,
			    offset, size, RL_READER);
			if (zp->z_blksz == size)
				break;
			offset += blkoff;
			zfs_rangelock_exit(zgd->zgd_lr);
		}
		/* test for truncation needs to be done while range locked */
		if (lr->lr_offset >= zp->z_size)
			error = SET_ERROR(ENOENT);
#ifdef ZFS_DEBUG
		if (zil_fault_io) {
			error = SET_ERROR(EIO);
			zil_fault_io = 0;
		}
#endif
		if (error) {
			zfs_get_done(zgd, error);
			return (error);
		}

		dmu_buf_t *dbp;
		error = dmu_buf_hold_noread(os, object, offset, zgd, &dbp);
		if (error) {
			zfs_get_done(zgd, error);
			return (error);
		}

		zgd->zgd_db = dbp;

		ASSERT3U(dbp->db_offset, ==, offset);
		ASSERT3U(dbp->db_size, ==, size);

		/*
		 * All O_DIRECT writes will have already completed and the
		 * block pointer can be immediately stored in the log record.
		 */
		dmu_buf_impl_t *db = (dmu_buf_impl_t *)dbp;
		mutex_enter(&db->db_mtx);

		dbuf_dirty_record_t *dr = dbuf_find_dirty_eq(db,
		    lr->lr_common.lrc_txg);

		if (dr != NULL && dr->dt.dl.dr_data == NULL &&
		    dr->dt.dl.dr_override_state == DR_OVERRIDDEN) {
			lr->lr_blkptr = dr->dt.dl.dr_overridden_by;
			mutex_exit(&db->db_mtx);
			zfs_get_done(zgd, 0);
			return (0);
		}
		mutex_exit(&db->db_mtx);

		/*
		 * Buffered writes may still need to be synced.
		 */
		error = dbuf_read(db, NULL, DB_RF_CANFAIL | DB_RF_NOPREFETCH);
		if (error == 0) {
			blkptr_t *bp = &lr->lr_blkptr;
			zgd->zgd_bp = bp;

			error = dmu_sync(zio, lr->lr_common.lrc_txg,
			    zfs_get_done, zgd);
			ASSERT(error || lr->lr_length <= size);

			/*
			 * On success, we need to wait for the write I/O
			 * initiated by dmu_sync() to complete before we can
			 * release this dbuf.  We will finish everything up
			 * in the zfs_get_done() callback.
			 */
			if (error == 0)
				return (0);

			if (error == EALREADY) {
				lr->lr_common.lrc_txtype = TX_WRITE2;
				/*
				 * TX_WRITE2 relies on the data previously
				 * written by the TX_WRITE that caused
				 * EALREADY.  We zero out the BP because
				 * it is the old, currently-on-disk BP.
				 */
				zgd->zgd_bp = NULL;
				BP_ZERO(bp);
				error = 0;
			}
		}
	}

	zfs_get_done(zgd, error);

	return (error);
}


static void
zfs_get_done(zgd_t *zgd, int error)
{
	(void) error;
	znode_t *zp = zgd->zgd_private;

	if (zgd->zgd_db)
		dmu_buf_rele(zgd->zgd_db, zgd);

	zfs_rangelock_exit(zgd->zgd_lr);

	/*
	 * Release the vnode asynchronously as we currently have the
	 * txg stopped from syncing.
	 */
	zfs_zrele_async(zp);

	kmem_free(zgd, sizeof (zgd_t));
}

EXPORT_SYMBOL(zfs_access);
EXPORT_SYMBOL(zfs_fsync);
EXPORT_SYMBOL(zfs_holey);
EXPORT_SYMBOL(zfs_read);
EXPORT_SYMBOL(zfs_write);
EXPORT_SYMBOL(zfs_getsecattr);
EXPORT_SYMBOL(zfs_setsecattr);

ZFS_MODULE_PARAM(zfs_vnops, zfs_vnops_, read_chunk_size, U64, ZMOD_RW,
	"Bytes to read per chunk");
