/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
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

/* Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T */
/* All Rights Reserved   */

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
 * $FreeBSD$
 */

#include <sys/page.h>
#include <sys/byteorder.h>
#include <sys/lock.h>
#include <sys/vm.h>
#include <vm/vm_map.h>
#include <sys/zfs_context.h>

/*
 *
 * if the operation is marked as read then we are stating the kernel will
 * be writing to pages and must give it write access.
 *
 *
 * This code comes from the FreeBSD's linuxkpi and models their implementation
 * of get_user_pages(), set_page_dirty(), and put_page().
 */
long
zfs_hold_pages(unsigned long start, unsigned long nr_pages, int read,
    zfs_page_p *pages)
{
	vm_map_t map;
	vm_prot_t prot;
	size_t len;
	int count;
	int write = read ? 1 : 0;

	map = &curthread->td_proc->p_vmspace->vm_map;

	prot = write ? (VM_PROT_READ | VM_PROT_WRITE) : VM_PROT_READ;
	len = ((size_t)nr_pages) << PAGE_SHIFT;
	count = vm_fault_quick_hold_pages(map, start, len, prot, pages,
	    nr_pages);

	if (count == -1)
		return (-EFAULT);
	return (count);
}

long
zfs_get_user_pages(unsigned long start, unsigned long nr_pages, int read,
    zfs_page_p *pages)
{
	int count;

	count = zfs_hold_pages(start, nr_pages, read, pages);

	if (count == -1)
		return (-EFAULT);

	ASSERT(count == nr_pages);

	for (int i = 0; i != nr_pages; i++) {
		zfs_page_p pg = pages[i];
		vm_page_lock(pg);
		vm_page_wire(pg);
		vm_page_unhold(pg);
		vm_page_unlock(pg);
	}

	return (nr_pages);
}

static void
zfs_set_user_page_dirty(zfs_page_p pp, int read)
{
	if (read && pp)
		vm_page_dirty(pp);
}

void
zfs_put_user_pages(zfs_page_p *pages, unsigned long nr_pages,
    boolean_t read)
{
	int count;

	count = zfs_hold_pages((unsigned long)pages[0], nr_pages, read, pages);
	if (count == -1)
		return;

	for (int i = 0; i != nr_pages; i++) {
		zfs_page_p pg = pages[i];
		vm_page_lock(pg);
		zfs_set_user_page_dirty(pg, read);
		vm_page_unwire_noq(pg);
		vm_page_unhold(pg);
		vm_page_unlock(pg);
	}
}

void
zfs_set_page_to_stable(zfs_page_p page)
{
	ASSERT3P(page, !=, NULL);
	vm_page_xbusy(page);
	pmap_remove_write(page);
}

void
zfs_release_stable_page(zfs_page_p page)
{
	ASSERT3P(page, !=, NULL);
	ASSERT(vm_page_xbusied(page));
	vm_page_xunbusy(page);
	vm_page_unhold(page);
}

/*
 * Both uio_iov_step() and zfs_uio_get_user_pages() are merely modified
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

	if (n > uio->uio_resid)
		n = uio->uio_resid;

	const struct iovec *p = uio->uio_iov;
	v.iov_len = MIN(n, p->iov_len);
	if (v.iov_len) {
		v.iov_base = p->iov_base;
		left = uio_iov_step(&v, maxpages, rw != UIO_WRITE, pages,
		    &local_pin);
		v.iov_len -= left;
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
		n -= v.iov_len;
		pinned_pages += local_pin;
	}

	return (pinned_pages);
}
