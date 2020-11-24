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
 */

#ifndef _SPL_PAGE_H
#define	_SPL_PAGE_H

#include <linux/page-flags.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <sys/types.h>
#include <sys/uio.h>

/*
 * read returning FOLL_WRITE is due to the fact that we are stating
 * that the kernel will have write access to the user pages. So, when
 * a Direct IO read request is issued, the kernel must write to the user
 * pages.
 *
 * get_user_pages_unlocked was not available to 4.0, so we also check
 * for get_user_pages on older kernels.
 */
/* 4.9 API change - for and read flag is passed as gup flags */
#if defined(HAVE_GET_USER_PAGES_UNLOCKED_GUP_FLAGS)
#define	zfs_get_user_pages(addr, numpages, read, pages) \
	get_user_pages_unlocked(addr, numpages, pages, read ? FOLL_WRITE : 0)

/* 4.8 API change - no longer takes struct task_struct as arguement */
#elif defined(HAVE_GET_USER_PAGES_UNLOCKED_WRITE_FLAG)
#define	zfs_get_user_pages(addr, numpages, read, pages) \
	get_user_pages_unlocked(addr, numpages, read, 0, pages)

/* 4.0 API */
#elif defined(HAVE_GET_USER_PAGES_UNLOCKED_TASK_STRUCT)
#define	zfs_get_user_pages(addr, numpages, read, pages) \
	get_user_pages_unlocked(current, current->mm, addr, numpages, read, 0, \
	    pages)

/* Using get_user_pages if kernel is < 4.0 */
#elif	defined(HAVE_GET_USER_PAGES_TASK_STRUCT)
#define	zfs_get_user_pages(addr, numpages, read, pages) \
	get_user_pages(current, current->mm, addr, numpages, read, 0, pages,  \
	    NULL)
#else
/*
 * This case is unreachable. We must be able to use either
 * get_user_pages_unlocked() or get_user_pages() to map user pages into
 * the kernel.
 */
#error	"Unknown Direct IO interface"
#endif

typedef struct page *zfs_page_p;

void zfs_put_user_pages(zfs_page_p *pages, unsigned long nr_pages,
    boolean_t read);
void zfs_set_page_to_stable(zfs_page_p page);
void zfs_release_stable_page(zfs_page_p page);
int zfs_uio_get_user_pages(uio_t *uio, zfs_page_p *pages, unsigned maxpages,
    enum uio_rw rw);

#endif /* _SPL_PAGE_H */
