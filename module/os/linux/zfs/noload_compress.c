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

extern int nvme_algo_run(struct nvme_algo *alg, struct bio *src, u64 src_len,
    struct bio *dst, u64 *dst_len);

/*
 * By default we will no enable Noload compression/decompression offloading.
 * This is avoid overriding QAT compression offloading.
 */
int zfs_noload_compress_enabled = 0;
module_param(zfs_noload_compress_enabled, int, 0644);
MODULE_PARM_DESC(zfs_noload_compress_enabled,
	"Enable/Disable Noload compression");
int zfs_noload_decompress_enabled = 0;
module_param(zfs_noload_decompress_enabled, int, 0644);
MODULE_PARM_DESC(zfs_noload_decompress_enabled,
	"Enable/Disable Noload decompression");

static inline boolean_t
noload_valid_align_size(size_t size)
{
	return (!(size % NOLOAD_ALIGN_REQUIREMENT) &&
	    (size <= NOLOAD_MAX_BUF_SIZE));
}

static void
noload_bio_map_buf(struct bio *bio, void *data, unsigned int len)
{
	unsigned long kaddr = (unsigned long)data;
	unsigned long end = (kaddr + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	unsigned long start = kaddr >> PAGE_SHIFT;
	const int nr_pages = end - start;
	bool is_vmalloc = is_vmalloc_addr(data);
	struct page *page;
	int offset, i;

	offset = offset_in_page(kaddr);
	for (i = 0; i < nr_pages; i++) {
		unsigned int bytes = PAGE_SIZE - offset;

		if (len <= 0)
			break;

		if (bytes > len)
			bytes = len;

		if (!is_vmalloc)
			page = virt_to_page(data);
		else
			page = vmalloc_to_page(data);

		bio_add_page(bio, page, bytes, offset);

		data += bytes;
		len -= bytes;
		offset = 0;
	}
}

static int
noload_abd_to_bio_cb(void *buf, size_t size, void *priv)
{
	struct bio *bio = priv;

	noload_bio_map_buf(bio, buf, size);

	return (0);
}

/*
 * We add one after dividing by PAGE_SIZE to account for any ABD whose offset
 * might require an additional page allocation. As an example, if buffer
 * length is 8KB but has an ABD offset of 512, then we must account for 3
 * pages which will have the following offsets:
 * First page : 512-4095
 * Second page: 4096-8191
 * Third page : 8192-8704
 */
static inline unsigned int
noload_bio_nr_iovecs(size_t len)
{
	return (len / PAGE_SIZE + 1);
}

boolean_t
noload_use_accel_compress(size_t size)
{
	if (!noload_c_alg || !zfs_noload_compress_enabled ||
	    !noload_valid_align_size(size))
		return (B_FALSE);
	return (B_TRUE);
}

boolean_t
noload_use_accel_decompress(size_t size)
{
	if (!noload_d_alg || !zfs_noload_decompress_enabled ||
	    !noload_valid_align_size(size))
		return (B_FALSE);
	return (B_TRUE);
}

/*
 * Main compression function for offloading GZip compression to the Noload
 * accelerators. The module parameter zfs_noload_compress_enabled must be
 * enabled in order to offload compression and zfs_noload_decompress_enabled
 * must be enabled in order offload decompression.
 *
 * There is no decompress function for the Noload as all compression is
 * GZip compatible.
 */
static inline int
__noload_run(struct nvme_algo *alg, abd_t *s_abd, void *dst, size_t s_len,
    size_t d_len, size_t *out_len)
{
	struct bio *bio_src = NULL;
	struct bio *bio_dst = NULL;
	int ret;
	u64 out;
	*out_len = s_len;

	ASSERT3B(noload_valid_align_size(s_len), ==, B_TRUE);
	ASSERT3P(alg, !=, NULL);

	/*
	 * The destination buffer must be LBA aligned in order to offload
	 * compression to the NoLoad CSP.
	 */
	if (!noload_valid_align_size(d_len))
		goto errorout;

	if (!s_abd)
		goto errorout;
	bio_src = bio_kmalloc(GFP_KERNEL, noload_bio_nr_iovecs(s_len));

	if (!dst)
		goto errorout;
	bio_dst = bio_kmalloc(GFP_KERNEL, noload_bio_nr_iovecs(d_len));

	bio_src->bi_end_io = bio_put;
	bio_dst->bi_end_io = bio_put;

	abd_iterate_func(s_abd, 0, s_len, noload_abd_to_bio_cb, bio_src);

	noload_bio_map_buf(bio_dst, dst, d_len);

	ret = nvme_algo_run(alg, bio_src, s_len, bio_dst, &out);

	if (ret) {
		if (ret == -ENODEV) {
			/*
			 * If we are not able to connect to the NoLoad
			 * nameespaces we will disable future requests.
			 */
			noload_fini();
			zfs_dbgmsg("nvme_algo_run returned -ENODEV");
		}
		zfs_dbgmsg("nvme_algo_run returned %d", ret);
		goto errorout;
	} else {
		*out_len = (size_t)out;
	}

	return (0);
errorout:
	if (bio_src)
		bio_put(bio_src);
	if (bio_dst)
		bio_put(bio_dst);
	return (NOLOAD_ERR);
}

int
noload_compress(abd_t *src, void *dst, size_t s_len, size_t d_len,
    size_t *out_len)
{
	return (__noload_run(noload_c_alg, src, dst, s_len, d_len, out_len));
}

int
noload_decompress(void *src, void *dst, size_t s_len, size_t d_len)
{
	int ret;
	size_t out_len;
	abd_t *abd_tmp = abd_get_from_buf(src, s_len);
	ASSERT3P(abd_tmp, !=, NULL);

	ret = __noload_run(noload_d_alg, abd_tmp, dst, s_len, d_len, &out_len);
	abd_free(abd_tmp);
	return (ret);

}

#endif /* _KERNEL && HAVE_NVME_ALGO */
