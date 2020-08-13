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

#include <sys/noload.h>

#if defined(_KERNEL) && defined(HAVE_NVME_ALGO)

extern struct nvme_algo *nvme_algo_find(const char *algo_name);
extern void nvme_algo_put(struct nvme_algo *alg);

struct nvme_algo *noload_alg = NULL;

void
noload_init(void)
{
	ASSERT3P(noload_alg, ==, NULL);

	noload_alg = nvme_algo_find("deflate");
	if (noload_alg) {
		zfs_dbgmsg("Loaded noload deflate");
	} else {
		zfs_dbgmsg("Could not load noload deflate "
		    "(may not be supported)");
		zfs_noload_compress_enabled = 0;
	}
}

void
noload_fini(void)
{
	if (noload_alg != NULL) {
		nvme_algo_put(noload_alg);
		noload_alg = NULL;
		zfs_noload_compress_enabled = 0;
	}
}

#endif /* _KERNEL && HAVE_NVME_ALGO */
