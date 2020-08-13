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
 * Copyright (c) 2019, Eidetic Communications Inc.
 * Use is subject to license terms.
 */

#ifndef	_SYS_NOLOAD_H
#define	_SYS_NOLOAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/zfs_context.h>
#include <sys/abd.h>

/*
 * The NoLoad CSP requires all data to be PAGE_SIZE aligned.
 * The maximum buffer it can compress is limited by the maximum
 * allowable bio.
 */
#define	NOLOAD_ALIGN_REQUIREMENT	(PAGE_SIZE)
#define	NOLOAD_MAX_BUF_SIZE		(BIO_MAX_PAGES * PAGE_SIZE)
#define	NOLOAD_COMPRESS_ERR		(-1)

#if defined(_KERNEL) && defined(__linux__) && defined(HAVE_NVME_ALGO)

extern int zfs_noload_compress_enabled;

extern void noload_init(void);
extern void noload_fini(void);
extern int noload_compress(abd_t *, void *, size_t, size_t, size_t *);
boolean_t noload_use_accel(size_t size);

struct nvme_algo; /* forward declaration */
extern struct nvme_algo *noload_alg;

#else

#define	noload_init()					((void)0)
#define	noload_fini()					((void)0)
#define	noload_compress(s, d, s_l, d_l, dest)		(NOLOAD_COMPRESS_ERR)
#define	noload_use_accel(s)				(B_FALSE)

#endif /* _KERNEL && __linux__ && HAVE_NVME_ALGO */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_NOLOAD_H */
