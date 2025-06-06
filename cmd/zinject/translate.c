// SPDX-License-Identifier: CDDL-1.0
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
 * Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2020 by Delphix. All rights reserved.
 */

#include <libzfs.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mntent.h>
#include <sys/mnttab.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dnode.h>
#include <sys/vdev_impl.h>

#include <sys/mkdev.h>

#include "zinject.h"

static int debug;

static void
ziprintf(const char *fmt, ...)
{
	va_list ap;

	if (!debug)
		return;

	va_start(ap, fmt);
	(void) vprintf(fmt, ap);
	va_end(ap);
}

static void
compress_slashes(const char *src, char *dest)
{
	while (*src != '\0') {
		*dest = *src++;
		while (*dest == '/' && *src == '/')
			++src;
		++dest;
	}
	*dest = '\0';
}

/*
 * Given a full path to a file, translate into a dataset name and a relative
 * path within the dataset.  'dataset' must be at least MAXNAMELEN characters,
 * and 'relpath' must be at least MAXPATHLEN characters.  We also pass a stat64
 * buffer, which we need later to get the object ID.
 */
static int
parse_pathname(const char *inpath, char *dataset, char *relpath,
    struct stat64 *statbuf)
{
	struct extmnttab mp;
	const char *rel;
	char fullpath[MAXPATHLEN];

	compress_slashes(inpath, fullpath);

	if (fullpath[0] != '/') {
		(void) fprintf(stderr, "invalid object '%s': must be full "
		    "path\n", fullpath);
		usage();
		return (-1);
	}

	if (getextmntent(fullpath, &mp, statbuf) != 0) {
		(void) fprintf(stderr, "cannot find mountpoint for '%s'\n",
		    fullpath);
		return (-1);
	}

	if (strcmp(mp.mnt_fstype, MNTTYPE_ZFS) != 0) {
		(void) fprintf(stderr, "invalid path '%s': not a ZFS "
		    "filesystem\n", fullpath);
		return (-1);
	}

	if (strncmp(fullpath, mp.mnt_mountp, strlen(mp.mnt_mountp)) != 0) {
		(void) fprintf(stderr, "invalid path '%s': mountpoint "
		    "doesn't match path\n", fullpath);
		return (-1);
	}

	(void) strlcpy(dataset, mp.mnt_special, MAXNAMELEN);

	rel = fullpath + strlen(mp.mnt_mountp);
	if (rel[0] == '/')
		rel++;
	(void) strlcpy(relpath, rel, MAXPATHLEN);

	return (0);
}

/*
 * Convert from a dataset to a objset id. Note that
 * we grab the object number from the inode number.
 */
static int
object_from_path(const char *dataset, uint64_t object, zinject_record_t *record)
{
	zfs_handle_t *zhp;

	if ((zhp = zfs_open(g_zfs, dataset, ZFS_TYPE_DATASET)) == NULL)
		return (-1);

	record->zi_objset = zfs_prop_get_int(zhp, ZFS_PROP_OBJSETID);
	record->zi_object = object;

	zfs_close(zhp);

	return (0);
}

/*
 * Initialize the range based on the type, level, and range given.
 */
static int
initialize_range(err_type_t type, int level, char *range,
    zinject_record_t *record)
{
	/*
	 * Determine the numeric range from the string.
	 */
	if (range == NULL) {
		/*
		 * If range is unspecified, set the range to [0,-1], which
		 * indicates that the whole object should be treated as an
		 * error.
		 */
		record->zi_start = 0;
		record->zi_end = -1ULL;
	} else {
		char *end;

		/* XXX add support for suffixes */
		record->zi_start = strtoull(range, &end, 10);


		if (*end == '\0')
			record->zi_end = record->zi_start + 1;
		else if (*end == ',')
			record->zi_end = strtoull(end + 1, &end, 10);

		if (*end != '\0') {
			(void) fprintf(stderr, "invalid range '%s': must be "
			    "a numeric range of the form 'start[,end]'\n",
			    range);
			return (-1);
		}
	}

	switch (type) {
	default:
		break;

	case TYPE_DATA:
		break;

	case TYPE_DNODE:
		/*
		 * If this is a request to inject faults into the dnode, then we
		 * must translate the current (objset,object) pair into an
		 * offset within the metadnode for the objset.  Specifying any
		 * kind of range with type 'dnode' is illegal.
		 */
		if (range != NULL) {
			(void) fprintf(stderr, "range cannot be specified when "
			    "type is 'dnode'\n");
			return (-1);
		}

		record->zi_start = record->zi_object * sizeof (dnode_phys_t);
		record->zi_end = record->zi_start + sizeof (dnode_phys_t);
		record->zi_object = 0;
		break;
	}

	record->zi_level = level;

	return (0);
}

int
translate_record(err_type_t type, const char *object, const char *range,
    int level, zinject_record_t *record, char *poolname, char *dataset)
{
	char path[MAXPATHLEN];
	char *slash;
	struct stat64 statbuf;
	int ret = -1;

	debug = (getenv("ZINJECT_DEBUG") != NULL);

	ziprintf("translating: %s\n", object);

	if (MOS_TYPE(type)) {
		/*
		 * MOS objects are treated specially.
		 */
		switch (type) {
		default:
			break;
		case TYPE_MOS:
			record->zi_type = 0;
			break;
		case TYPE_MOSDIR:
			record->zi_type = DMU_OT_OBJECT_DIRECTORY;
			break;
		case TYPE_METASLAB:
			record->zi_type = DMU_OT_OBJECT_ARRAY;
			break;
		case TYPE_CONFIG:
			record->zi_type = DMU_OT_PACKED_NVLIST;
			break;
		case TYPE_BPOBJ:
			record->zi_type = DMU_OT_BPOBJ;
			break;
		case TYPE_SPACEMAP:
			record->zi_type = DMU_OT_SPACE_MAP;
			break;
		case TYPE_ERRLOG:
			record->zi_type = DMU_OT_ERROR_LOG;
			break;
		}

		dataset[0] = '\0';
		(void) strlcpy(poolname, object, MAXNAMELEN);
		return (0);
	}

	/*
	 * Convert a full path into a (dataset, file) pair.
	 */
	if (parse_pathname(object, dataset, path, &statbuf) != 0)
		goto err;

	ziprintf("   dataset: %s\n", dataset);
	ziprintf("      path: %s\n", path);

	/*
	 * Convert (dataset, file) into (objset, object)
	 */
	if (object_from_path(dataset, statbuf.st_ino, record) != 0)
		goto err;

	ziprintf("raw objset: %llu\n", record->zi_objset);
	ziprintf("raw object: %llu\n", record->zi_object);

	/*
	 * For the given object, initialize the range in bytes
	 */
	if (initialize_range(type, level, (char *)range, record) != 0)
		goto err;

	ziprintf("    objset: %llu\n", record->zi_objset);
	ziprintf("    object: %llu\n", record->zi_object);
	if (record->zi_start == 0 &&
	    record->zi_end == -1ULL)
		ziprintf("     range: all\n");
	else
		ziprintf("     range: [%llu, %llu]\n", record->zi_start,
		    record->zi_end);

	/*
	 * Copy the pool name
	 */
	(void) strlcpy(poolname, dataset, MAXNAMELEN);
	if ((slash = strchr(poolname, '/')) != NULL)
		*slash = '\0';

	ret = 0;

err:
	return (ret);
}

int
translate_raw(const char *str, zinject_record_t *record)
{
	/*
	 * A raw bookmark of the form objset:object:level:blkid, where each
	 * number is a hexadecimal value.
	 */
	if (sscanf(str, "%llx:%llx:%x:%llx", (u_longlong_t *)&record->zi_objset,
	    (u_longlong_t *)&record->zi_object, &record->zi_level,
	    (u_longlong_t *)&record->zi_start) != 4) {
		(void) fprintf(stderr, "bad raw spec '%s': must be of the form "
		    "'objset:object:level:blkid'\n", str);
		return (-1);
	}

	record->zi_end = record->zi_start;

	return (0);
}

int
translate_device(const char *pool, const char *device, err_type_t label_type,
    zinject_record_t *record)
{
	char *end;
	zpool_handle_t *zhp;
	nvlist_t *tgt;
	boolean_t isspare, iscache;

	/*
	 * Given a device name or GUID, create an appropriate injection record
	 * with zi_guid set.
	 */
	if ((zhp = zpool_open(g_zfs, pool)) == NULL)
		return (-1);

	record->zi_guid = strtoull(device, &end, 0);
	if (record->zi_guid == 0 || *end != '\0') {
		tgt = zpool_find_vdev(zhp, device, &isspare, &iscache, NULL);

		if (tgt == NULL) {
			(void) fprintf(stderr, "cannot find device '%s' in "
			    "pool '%s'\n", device, pool);
			zpool_close(zhp);
			return (-1);
		}

		verify(nvlist_lookup_uint64(tgt, ZPOOL_CONFIG_GUID,
		    &record->zi_guid) == 0);
	}

	/*
	 * Device faults can take on three different forms:
	 * 1). delayed or hanging I/O
	 * 2). zfs label faults
	 * 3). generic disk faults
	 */
	if (record->zi_timer != 0) {
		record->zi_cmd = ZINJECT_DELAY_IO;
	} else if (label_type != TYPE_INVAL) {
		record->zi_cmd = ZINJECT_LABEL_FAULT;
	} else {
		record->zi_cmd = ZINJECT_DEVICE_FAULT;
	}

	switch (label_type) {
	default:
		break;
	case TYPE_LABEL_UBERBLOCK:
		record->zi_start = offsetof(vdev_label_t, vl_uberblock[0]);
		record->zi_end = record->zi_start + VDEV_UBERBLOCK_RING - 1;
		break;
	case TYPE_LABEL_NVLIST:
		record->zi_start = offsetof(vdev_label_t, vl_vdev_phys);
		record->zi_end = record->zi_start + VDEV_PHYS_SIZE - 1;
		break;
	case TYPE_LABEL_PAD1:
		record->zi_start = offsetof(vdev_label_t, vl_pad1);
		record->zi_end = record->zi_start + VDEV_PAD_SIZE - 1;
		break;
	case TYPE_LABEL_PAD2:
		record->zi_start = offsetof(vdev_label_t, vl_be);
		record->zi_end = record->zi_start + VDEV_PAD_SIZE - 1;
		break;
	}
	zpool_close(zhp);
	return (0);
}
