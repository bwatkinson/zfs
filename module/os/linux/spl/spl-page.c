/*
 *  Copyright (C) 2007-2010 Lawrence Livermore National Security, LLC.
 *  Copyright (C) 2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Brian Behlendorf <behlendorf1@llnl.gov>.
 *  UCRL-CODE-235197
 *
 *  This file is part of the SPL, Solaris Porting Layer.
 *  For details, see <http://zfsonlinux.org/>.
 *
 *  The SPL is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  The SPL is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the SPL.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Solaris Porting Layer (SPL) Atomic Implementation.
 */

#include <sys/page.h>
#include <sys/errno.h>
#include <sys/zfs_context.h>

static void
zfs_set_user_page_dirty(zfs_page_p page, int read)
{
	if (read && page)
		set_page_dirty(page);
}
EXPORT_SYMBOL(zfs_set_user_page_dirty);

void
zfs_put_user_pages(zfs_page_p *pages, unsigned long nr_pages,
    boolean_t read)
{
	for (int i = 0; i < nr_pages; i++) {
		zfs_page_p page = pages[i];
		if (page) {
			zfs_set_user_page_dirty(page, read);
			put_page(page);
		}
	}
}
EXPORT_SYMBOL(zfs_put_user_pages);

static inline unsigned int
zfs_get_num_pages(size_t len)
{
	return (len / PAGE_SIZE);
}

void
zfs_set_page_to_stable(zfs_page_p page)
{
	ASSERT3P(page, !=, NULL);
	lock_page(page);
	set_page_dirty(page);

	if (PageWriteback(page)) {
		wait_on_page_bit(page, PG_writeback);
	}
	TestSetPageWriteback(page);
}
EXPORT_SYMBOL(zfs_set_page_to_stable);

void
zfs_release_stable_page(zfs_page_p page)
{
	ASSERT3P(page, !=, NULL);
	ASSERT(PageLocked(page));
	end_page_writeback(page);
	unlock_page(page);
}
EXPORT_SYMBOL(zfs_release_stable_page);

/*
 * Both uio_iov_step() and uio_get_user_pages() are merely modified
 * functions of the Linux kernel function iov_iter_get_pages().
 *
 * iov_iter_get_pages() was not introduced until the 3.15 kernel, so
 * this code is used instead of directly calling iov_get_get_pages()
 * to make sure we can pinning user pages from an uio_t struct iovec.
 */
static size_t
uio_iov_step(struct iovec *v, unsigned maxpages, enum uio_rw rw,
    zfs_page_p *pages, int *nr_pages)
{
	size_t start;
	unsigned long addr = (unsigned long)(v->iov_base);
	size_t len = v->iov_len + (start = addr & (PAGE_SIZE - 1));
	int n;
	int res;

	if (len > maxpages * PAGE_SIZE)
		len = maxpages * PAGE_SIZE;
	addr &= ~(PAGE_SIZE - 1);
	n = DIV_ROUND_UP(len, PAGE_SIZE);
	res = zfs_get_user_pages(addr, n, rw != UIO_WRITE, pages);
	if (res < 0)
		return (res);
	*nr_pages = res;
	return ((res == n ? len : res * PAGE_SIZE) - start);
}

/*
 * This function returns the total number of pages pinned on success.
 * In the case of a uio with bvec is passed, then ENOTSUP will be
 * returned. It is callers responsiblity to check for ENOTSUP.
 */
int
zfs_uio_get_user_pages(uio_t *uio, zfs_page_p *pages, unsigned maxpages,
    enum uio_rw rw)
{
	size_t n = maxpages * PAGE_SIZE;
	size_t left;
	int pinned_pages = 0;
	int local_pin;
	struct iovec v;

	/*
	 * Currently we only support pinning iovec's. It is possibly
	 * to allow for bvec's as well, it would just mean adding the kernel
	 * code in iov_iter_get_pages() in the kernel to handle the correct
	 * step function.
	 */
	if (uio->uio_segflg == UIO_BVEC)
		return (ENOTSUP);

	if (n > uio->uio_resid)
		n = uio->uio_resid;

	const struct iovec *p = uio->uio_iov;
	size_t skip = uio->uio_skip;
	v.iov_len = MIN(n, p->iov_len - skip);
	if (v.iov_len) {
		v.iov_base = p->iov_base + skip;
		left = uio_iov_step(&v, maxpages, rw != UIO_WRITE, pages,
		    &local_pin);
		v.iov_len -= left;
		skip += v.iov_len;
		n -= v.iov_len;
		pinned_pages += local_pin;
	} else {
		left = 0;
	}

	while (!left && n) {
		p++;
		v.iov_len = MIN(n, p->iov_len);
		if (!v.iov_len)
			continue;
		v.iov_base = p->iov_base;
		left = uio_iov_step(&v, maxpages, rw != UIO_WRITE, pages,
		    &local_pin);
		v.iov_len -= left;
		skip = v.iov_len;
		n -= v.iov_len;
		pinned_pages += local_pin;
	}

	return (pinned_pages);
}
EXPORT_SYMBOL(zfs_uio_get_user_pages);
