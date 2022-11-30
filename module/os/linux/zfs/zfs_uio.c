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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T	*/
/*	  All Rights Reserved	*/

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */
/*
 * Copyright (c) 2015 by Chunwei Chen. All rights reserved.
 */

#ifdef _KERNEL

#include <sys/errno.h>
#include <sys/vmem.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/uio_impl.h>
#include <sys/sysmacros.h>
#include <sys/string.h>
#include <sys/zfs_refcount.h>
#include <sys/zfs_debug.h>
#include <linux/kmap_compat.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>
#include <linux/mman.h>

/*
 * Move "n" bytes at byte address "p"; "rw" indicates the direction
 * of the move, and the I/O parameters are provided in "uio", which is
 * update to reflect the data which was moved.  Returns 0 on success or
 * a non-zero errno on failure.
 */
static int
zfs_uiomove_iov(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	const struct iovec *iov = uio->uio_iov;
	size_t skip = uio->uio_skip;
	ulong_t cnt;

	while (n && uio->uio_resid) {
		cnt = MIN(iov->iov_len - skip, n);
		switch (uio->uio_segflg) {
		case UIO_USERSPACE:
			/*
			 * p = kernel data pointer
			 * iov->iov_base = user data pointer
			 */
			if (rw == UIO_READ) {
				if (copy_to_user(iov->iov_base+skip, p, cnt))
					return (EFAULT);
			} else {
				unsigned long b_left = 0;
				if (uio->uio_fault_disable) {
					if (!zfs_access_ok(VERIFY_READ,
					    (iov->iov_base + skip), cnt)) {
						return (EFAULT);
					}
					pagefault_disable();
					b_left =
					    __copy_from_user_inatomic(p,
					    (iov->iov_base + skip), cnt);
					pagefault_enable();
				} else {
					b_left =
					    copy_from_user(p,
					    (iov->iov_base + skip), cnt);
				}
				if (b_left > 0) {
					unsigned long c_bytes =
					    cnt - b_left;
					uio->uio_skip += c_bytes;
					ASSERT3U(uio->uio_skip, <,
					    iov->iov_len);
					uio->uio_resid -= c_bytes;
					uio->uio_loffset += c_bytes;
					return (EFAULT);
				}
			}
			break;
		case UIO_SYSSPACE:
			if (rw == UIO_READ)
				memcpy(iov->iov_base + skip, p, cnt);
			else
				memcpy(p, iov->iov_base + skip, cnt);
			break;
		default:
			ASSERT(0);
		}
		skip += cnt;
		if (skip == iov->iov_len) {
			skip = 0;
			uio->uio_iov = (++iov);
			uio->uio_iovcnt--;
		}
		uio->uio_skip = skip;
		uio->uio_resid -= cnt;
		uio->uio_loffset += cnt;
		p = (caddr_t)p + cnt;
		n -= cnt;
	}
	return (0);
}

static int
zfs_uiomove_bvec_impl(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	const struct bio_vec *bv = uio->uio_bvec;
	size_t skip = uio->uio_skip;
	ulong_t cnt;

	while (n && uio->uio_resid) {
		void *paddr;
		cnt = MIN(bv->bv_len - skip, n);

		paddr = zfs_kmap_local(bv->bv_page);
		if (rw == UIO_READ) {
			/* Copy from buffer 'p' to the bvec data */
			memcpy(paddr + bv->bv_offset + skip, p, cnt);
		} else {
			/* Copy from bvec data to buffer 'p' */
			memcpy(p, paddr + bv->bv_offset + skip, cnt);
		}
		zfs_kunmap_local(paddr);

		skip += cnt;
		if (skip == bv->bv_len) {
			skip = 0;
			uio->uio_bvec = (++bv);
			uio->uio_iovcnt--;
		}
		uio->uio_skip = skip;
		uio->uio_resid -= cnt;
		uio->uio_loffset += cnt;
		p = (caddr_t)p + cnt;
		n -= cnt;
	}
	return (0);
}

#ifdef HAVE_BLK_MQ
static void
zfs_copy_bvec(void *p, size_t skip, size_t cnt, zfs_uio_rw_t rw,
    struct bio_vec *bv)
{
	void *paddr;

	paddr = zfs_kmap_local(bv->bv_page);
	if (rw == UIO_READ) {
		/* Copy from buffer 'p' to the bvec data */
		memcpy(paddr + bv->bv_offset + skip, p, cnt);
	} else {
		/* Copy from bvec data to buffer 'p' */
		memcpy(p, paddr + bv->bv_offset + skip, cnt);
	}
	zfs_kunmap_local(paddr);
}

/*
 * Copy 'n' bytes of data between the buffer p[] and the data represented
 * by the request in the uio.
 */
static int
zfs_uiomove_bvec_rq(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	struct request *rq = uio->rq;
	struct bio_vec bv;
	struct req_iterator iter;
	size_t this_seg_start;	/* logical offset */
	size_t this_seg_end;		/* logical offset */
	size_t skip_in_seg;
	size_t copy_from_seg;
	size_t orig_loffset;
	int copied = 0;

	/*
	 * Get the original logical offset of this entire request (because
	 * uio->uio_loffset will be modified over time).
	 */
	orig_loffset = io_offset(NULL, rq);
	this_seg_start = orig_loffset;

	rq_for_each_segment(bv, rq, iter) {
		/*
		 * Lookup what the logical offset of the last byte of this
		 * segment is.
		 */
		this_seg_end = this_seg_start + bv.bv_len - 1;

		/*
		 * We only need to operate on segments that have data we're
		 * copying.
		 */
		if (uio->uio_loffset >= this_seg_start &&
		    uio->uio_loffset <= this_seg_end) {
			/*
			 * Some, or all, of the data in this segment needs to be
			 * copied.
			 */

			/*
			 * We may be not be copying from the first byte in the
			 * segment.  Figure out how many bytes to skip copying
			 * from the beginning of this segment.
			 */
			skip_in_seg = uio->uio_loffset - this_seg_start;

			/*
			 * Calculate the total number of bytes from this
			 * segment that we will be copying.
			 */
			copy_from_seg = MIN(bv.bv_len - skip_in_seg, n);

			/* Copy the bytes */
			zfs_copy_bvec(p, skip_in_seg, copy_from_seg, rw, &bv);
			p = ((char *)p) + copy_from_seg;

			n -= copy_from_seg;
			uio->uio_resid -= copy_from_seg;
			uio->uio_loffset += copy_from_seg;
			copied = 1;	/* We copied some data */
		}

		this_seg_start = this_seg_end + 1;
	}

	if (!copied) {
		/* Didn't copy anything */
		uio->uio_resid = 0;
	}
	return (0);
}
#endif

static int
zfs_uiomove_bvec(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
#ifdef HAVE_BLK_MQ
	if (uio->rq != NULL)
		return (zfs_uiomove_bvec_rq(p, n, rw, uio));
#else
	ASSERT3P(uio->rq, ==, NULL);
#endif
	return (zfs_uiomove_bvec_impl(p, n, rw, uio));
}

#if defined(HAVE_VFS_IOV_ITER)
static int
zfs_uiomove_iter(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio,
    boolean_t revert)
{
	size_t cnt = MIN(n, uio->uio_resid);

	if (uio->uio_skip)
		iov_iter_advance(uio->uio_iter, uio->uio_skip);

	if (rw == UIO_READ)
		cnt = copy_to_iter(p, cnt, uio->uio_iter);
	else
		cnt = copy_from_iter(p, cnt, uio->uio_iter);

	/*
	 * When operating on a full pipe no bytes are processed.
	 * In which case return EFAULT which is converted to EAGAIN
	 * by the kernel's generic_file_splice_read() function.
	 */
	if (cnt == 0)
		return (EFAULT);

	/*
	 * Revert advancing the uio_iter.  This is set by zfs_uiocopy()
	 * to avoid consuming the uio and its iov_iter structure.
	 */
	if (revert)
		iov_iter_revert(uio->uio_iter, cnt);

	uio->uio_resid -= cnt;
	uio->uio_loffset += cnt;

	return (0);
}
#endif

int
zfs_uiomove(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio)
{
	if (uio->uio_segflg == UIO_BVEC)
		return (zfs_uiomove_bvec(p, n, rw, uio));
#if defined(HAVE_VFS_IOV_ITER)
	else if (uio->uio_segflg == UIO_ITER)
		return (zfs_uiomove_iter(p, n, rw, uio, B_FALSE));
#endif
	else
		return (zfs_uiomove_iov(p, n, rw, uio));
}
EXPORT_SYMBOL(zfs_uiomove);

/*
 * Fault in the pages of the first n bytes specified by the uio structure.
 * 1 byte in each page is touched and the uio struct is unmodified. Any
 * error will terminate the process as this is only a best attempt to get
 * the pages resident.
 */
int
zfs_uio_prefaultpages(ssize_t n, zfs_uio_t *uio)
{
	if (uio->uio_segflg == UIO_SYSSPACE || uio->uio_segflg == UIO_BVEC) {
		/* There's never a need to fault in kernel pages */
		return (0);
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		/*
		 * At least a Linux 4.9 kernel, iov_iter_fault_in_readable()
		 * can be relied on to fault in user pages when referenced.
		 */
		if (iov_iter_fault_in_readable(uio->uio_iter, n))
			return (EFAULT);
#endif
	} else {
		/* Fault in all user pages */
		ASSERT3S(uio->uio_segflg, ==, UIO_USERSPACE);
		const struct iovec *iov = uio->uio_iov;
		int iovcnt = uio->uio_iovcnt;
		size_t skip = uio->uio_skip;
		uint8_t tmp;
		caddr_t p;

		for (; n > 0 && iovcnt > 0; iov++, iovcnt--, skip = 0) {
			ulong_t cnt = MIN(iov->iov_len - skip, n);
			/* empty iov */
			if (cnt == 0)
				continue;
			n -= cnt;
			/* touch each page in this segment. */
			p = iov->iov_base + skip;
			while (cnt) {
				if (copy_from_user(&tmp, p, 1))
					return (EFAULT);
				ulong_t incr = MIN(cnt, PAGESIZE);
				p += incr;
				cnt -= incr;
			}
			/* touch the last byte in case it straddles a page. */
			p--;
			if (copy_from_user(&tmp, p, 1))
				return (EFAULT);
		}
	}

	return (0);
}
EXPORT_SYMBOL(zfs_uio_prefaultpages);

/*
 * The same as zfs_uiomove() but doesn't modify uio structure.
 * return in cbytes how many bytes were copied.
 */
int
zfs_uiocopy(void *p, size_t n, zfs_uio_rw_t rw, zfs_uio_t *uio, size_t *cbytes)
{
	zfs_uio_t uio_copy;
	int ret;

	memcpy(&uio_copy, uio, sizeof (zfs_uio_t));

	if (uio->uio_segflg == UIO_BVEC)
		ret = zfs_uiomove_bvec(p, n, rw, &uio_copy);
#if defined(HAVE_VFS_IOV_ITER)
	else if (uio->uio_segflg == UIO_ITER)
		ret = zfs_uiomove_iter(p, n, rw, &uio_copy, B_TRUE);
#endif
	else
		ret = zfs_uiomove_iov(p, n, rw, &uio_copy);

	*cbytes = uio->uio_resid - uio_copy.uio_resid;

	return (ret);
}
EXPORT_SYMBOL(zfs_uiocopy);

/*
 * Drop the next n chars out of *uio.
 */
void
zfs_uioskip(zfs_uio_t *uio, size_t n)
{
	if (n > uio->uio_resid)
		return;
	/*
	 * When using a uio with a struct request, we simply
	 * use uio_loffset as a pointer to the next logical byte to
	 * copy in the request.  We don't have to do any fancy
	 * accounting with uio_bvec/uio_iovcnt since we don't use
	 * them.
	 */
	if (uio->uio_segflg == UIO_BVEC && uio->rq == NULL) {
		uio->uio_skip += n;
		while (uio->uio_iovcnt &&
		    uio->uio_skip >= uio->uio_bvec->bv_len) {
			uio->uio_skip -= uio->uio_bvec->bv_len;
			uio->uio_bvec++;
			uio->uio_iovcnt--;
		}
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		iov_iter_advance(uio->uio_iter, n);
#endif
	} else {
		uio->uio_skip += n;
		while (uio->uio_iovcnt &&
		    uio->uio_skip >= uio->uio_iov->iov_len) {
			uio->uio_skip -= uio->uio_iov->iov_len;
			uio->uio_iov++;
			uio->uio_iovcnt--;
		}
	}

	uio->uio_loffset += n;
	uio->uio_resid -= n;
}
EXPORT_SYMBOL(zfs_uioskip);

/*
 * Check if the uio is page-aligned in memory.
 */
boolean_t
zfs_uio_page_aligned(zfs_uio_t *uio)
{
	boolean_t aligned = B_TRUE;

	if (uio->uio_segflg == UIO_USERSPACE ||
	    uio->uio_segflg == UIO_SYSSPACE) {
		const struct iovec *iov = uio->uio_iov;
		size_t skip = uio->uio_skip;

		for (int i = uio->uio_iovcnt; i > 0; iov++, i--) {
			unsigned long addr =
			    (unsigned long)(iov->iov_base + skip);
			size_t size = iov->iov_len - skip;
			if ((addr & (PAGE_SIZE - 1)) ||
			    (size & (PAGE_SIZE - 1))) {
				aligned = B_FALSE;
				break;
			}
			skip = 0;
		}
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		unsigned long alignment =
		    iov_iter_alignment(uio->uio_iter);
		aligned = IS_P2ALIGNED(alignment, PAGE_SIZE);
#endif
	} else {
		/* Currently not supported */
		aligned = B_FALSE;
	}

	return (aligned);
}


#if defined(HAVE_ZERO_PAGE_GPL_ONLY) || !defined(_LP64)
#define	ZFS_MARKEED_PAGE	0x0
#define	IS_ZFS_MARKED_PAGE(_p)	0
#define	zfs_mark_page(_p)
#define	zfs_unmark_page(_p)
#define	IS_ZERO_PAGE(_p)	0

#else
/*
 * Mark pages to know if they were allocated to replace ZERO_PAGE() for
 * Direct IO writes.
 */
#define	ZFS_MARKED_PAGE		0x5a465350414745 /* ASCII: ZFSPAGE */
#define	IS_ZFS_MARKED_PAGE(_p) \
	(page_private(_p) == (unsigned long)ZFS_MARKED_PAGE)
#define	IS_ZERO_PAGE(_p) ((_p) == ZERO_PAGE(0))

static inline void
zfs_mark_page(struct page *page)
{
	ASSERT3P(page, !=, NULL);
	get_page(page);
	SetPagePrivate(page);
	set_page_private(page, ZFS_MARKED_PAGE);
}

static inline void
zfs_unmark_page(struct page *page)
{
	ASSERT3P(page, !=, NULL);
	set_page_private(page, 0UL);
	ClearPagePrivate(page);
	put_page(page);
}
#endif /* HAVE_ZERO_PAGE_GPL_ONLY || !_LP64 */

static void
zfs_uio_dio_check_for_zero_page(zfs_uio_t *uio)
{
	ASSERT3P(uio->uio_dio.pages, !=, NULL);

	for (int i = 0; i < uio->uio_dio.npages; i++) {
		struct page *p = uio->uio_dio.pages[i];
		lock_page(p);

		if (IS_ZERO_PAGE(p)) {
			/*
			 * If the user page points the kernels ZERO_PAGE() a
			 * new zero filled page will just be allocated so the
			 * contents of the page can not be changed by the user
			 * while a Direct I/O write is taking place.
			 */
			gfp_t gfp_zero_page  = __GFP_NOWARN | GFP_NOIO |
			    __GFP_ZERO | GFP_KERNEL;

			ASSERT0(IS_ZFS_MARKED_PAGE(p));
			unlock_page(p);
			put_page(p);

			p = __page_cache_alloc(gfp_zero_page);
			zfs_mark_page(p);
		} else {
			unlock_page(p);
		}
	}
}

void
zfs_uio_free_dio_pages(zfs_uio_t *uio, zfs_uio_rw_t rw)
{

	ASSERT(uio->uio_extflg & UIO_DIRECT);
	ASSERT3P(uio->uio_dio.pages, !=, NULL);

	for (int i = 0; i < uio->uio_dio.npages; i++) {
		struct page *p = uio->uio_dio.pages[i];

		if (IS_ZFS_MARKED_PAGE(p)) {
			zfs_unmark_page(p);
			__free_page(p);
			continue;
		}

		put_page(p);
	}

	vmem_free(uio->uio_dio.pages,
	    uio->uio_dio.npages * sizeof (struct page *));
}

/*
 * zfs_uio_iov_step() is just a modified version of the STEP function of Linux's
 * iov_iter_get_pages().
 */
static size_t
zfs_uio_iov_step(struct iovec v, zfs_uio_rw_t rw, zfs_uio_t *uio, int *numpages)
{
	unsigned long addr = (unsigned long)(v.iov_base);
	size_t len = v.iov_len;
	int n = DIV_ROUND_UP(len, PAGE_SIZE);

	int res = zfs_get_user_pages(P2ALIGN(addr, PAGE_SIZE), n,
	    rw == UIO_READ, &uio->uio_dio.pages[uio->uio_dio.npages]);
	if (res < 0) {
		*numpages = -1;
		return (-res);
	} else if (len != (res * PAGE_SIZE)) {
		*numpages = -1;
		return (len);
	}

	ASSERT3S(len, ==, res * PAGE_SIZE);
	*numpages = res;
	return (len);
}

static int
zfs_uio_get_dio_pages_iov(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	const struct iovec *iovp = uio->uio_iov;
	size_t skip = uio->uio_skip;
	size_t wanted, maxsize;

	ASSERT(uio->uio_segflg != UIO_SYSSPACE);
	wanted = maxsize = uio->uio_resid - skip;

	for (int i = 0; i < uio->uio_iovcnt; i++) {
		struct iovec iov;
		int numpages = 0;

		if (iovp->iov_len == 0) {
			iovp++;
			skip = 0;
			continue;
		}
		iov.iov_len = MIN(maxsize, iovp->iov_len - skip);
		iov.iov_base = iovp->iov_base + skip;
		ssize_t left = zfs_uio_iov_step(iov, rw, uio, &numpages);

		if (numpages == -1) {
			return (left);
		}

		ASSERT3U(left, ==, iov.iov_len);
		uio->uio_dio.npages += numpages;
		maxsize -= iov.iov_len;
		wanted -= left;
		skip = 0;
		iovp++;
	}

	ASSERT0(wanted);
	return (0);
}

#if defined(HAVE_VFS_IOV_ITER)
static int
zfs_uio_get_dio_pages_iov_iter(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	size_t skip = uio->uio_skip;
	size_t wanted = uio->uio_resid - uio->uio_skip;
	size_t rollback = 0;
	size_t cnt;
	size_t maxpages = DIV_ROUND_UP(wanted, PAGE_SIZE);

	while (wanted) {
#if defined(HAVE_IOV_ITER_GET_PAGES2)
		cnt = iov_iter_get_pages2(uio->uio_iter,
		    &uio->uio_dio.pages[uio->uio_dio.npages],
		    wanted, maxpages, &skip);
#else
		cnt = iov_iter_get_pages(uio->uio_iter,
		    &uio->uio_dio.pages[uio->uio_dio.npages],
		    wanted, maxpages, &skip);
#endif
		if (cnt < 0) {
			iov_iter_revert(uio->uio_iter, rollback);
			return (SET_ERROR(-cnt));
		}
		uio->uio_dio.npages += DIV_ROUND_UP(cnt, PAGE_SIZE);
		rollback += cnt;
		wanted -= cnt;
		skip = 0;
#if !defined(HAVE_IOV_ITER_GET_PAGES2)
		/*
		 * iov_iter_get_pages2() advances the iov_iter on success.
		 */
		iov_iter_advance(uio->uio_iter, cnt);
#endif

	}
	ASSERT3U(rollback, ==, uio->uio_resid - uio->uio_skip);
	iov_iter_revert(uio->uio_iter, rollback);

	return (0);
}
#endif /* HAVE_VFS_IOV_ITER */

/*
 * This function maps user pages into the kernel. In the event that the user
 * pages were not mapped successfully an error value is returned.
 *
 * On success, 0 is returned.
 */
int
zfs_uio_get_dio_pages_alloc(zfs_uio_t *uio, zfs_uio_rw_t rw)
{
	int error = 0;
	size_t npages = DIV_ROUND_UP(uio->uio_resid, PAGE_SIZE);
	size_t size = npages * sizeof (struct page *);

	if (uio->uio_segflg == UIO_USERSPACE) {
		uio->uio_dio.pages = vmem_alloc(size, KM_SLEEP);
		error = zfs_uio_get_dio_pages_iov(uio, rw);
		ASSERT3S(uio->uio_dio.npages, ==, npages);
#if defined(HAVE_VFS_IOV_ITER)
	} else if (uio->uio_segflg == UIO_ITER) {
		uio->uio_dio.pages = vmem_alloc(size, KM_SLEEP);
		error = zfs_uio_get_dio_pages_iov_iter(uio, rw);
		ASSERT3S(uio->uio_dio.npages, ==, npages);
#endif
	} else {
		return (SET_ERROR(EOPNOTSUPP));
	}

	if (error) {
		vmem_free(uio->uio_dio.pages, size);
		return (error);
	}

	if (rw == UIO_WRITE) {
		zfs_uio_dio_check_for_zero_page(uio);
	}

	uio->uio_extflg |= UIO_DIRECT;

	return (0);
}

#endif /* _KERNEL */
