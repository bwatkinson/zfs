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

#if defined(_KERNEL) && defined(HAVE_NVME_ALGO)
#include <sys/zfs_context.h>
#include <sys/abd.h>

struct nvme_algo;

int nvme_algo_run(struct nvme_algo *alg, struct bio *src,
		  u64 src_len, struct bio *dst, u64 *dst_len);
struct nvme_algo *nvme_algo_find(const char *algo_name, const char *dev_name);
void nvme_algo_put(struct nvme_algo *alg);

static struct nvme_algo *noload_alg;
static atomic_t req_count;

static void noload_enable(void)
{
	if (noload_alg)
		return;

	if (!atomic_read(&req_count))
		return;

	noload_alg = nvme_algo_find("deflate", NULL);
	printk(KERN_NOTICE "ZFS: Using Noload Compression\n");
}

void noload_disable(void)
{
	printk(KERN_NOTICE "ZFS: Noload Compression Disabled\n");
	nvme_algo_put(noload_alg);
	noload_alg = NULL;
}

void noload_request(void)
{
	if (atomic_inc_return(&req_count) == 1)
		noload_enable();
}

void noload_release(void)
{
	if (atomic_dec_and_test(&req_count))
		noload_disable();
}

static void bio_map_buf(struct bio *bio, void *data, unsigned int len)
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

static int abd_to_bio_cb(void *buf, size_t size, void *priv)
{
	struct bio *bio = priv;

	bio_map_buf(bio, buf, size);

	return 0;
}

size_t noload_compress(abd_t *src, void *dst, size_t s_len, size_t d_len,
		       int level)
{
	struct bio *bio_src, *bio_dst;
	u64 out_len = s_len;
	int ret;

	if (!noload_alg)
		noload_enable();
	if (!noload_alg)
		return out_len;

	bio_src = bio_kmalloc(GFP_KERNEL, s_len / PAGE_SIZE + 1);
	if (!src)
		return out_len;

	bio_dst = bio_kmalloc(GFP_KERNEL, d_len / PAGE_SIZE + 1);
	if (!dst) {
		bio_put(bio_src);
		return out_len;
	}

	bio_src->bi_end_io = bio_put;
	bio_dst->bi_end_io = bio_put;

	abd_iterate_func(src, 0, s_len, abd_to_bio_cb, bio_src);

	bio_map_buf(bio_dst, dst, d_len);

	ret = nvme_algo_run(noload_alg, bio_src, s_len, bio_dst, &out_len);
	if (ret) {
		if (ret == -ENODEV)
			noload_disable();
		out_len = s_len;
	}

	return out_len;
}

#endif
