#!/bin/ksh -p
#
# DDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2020 by Lawrence Livermore National Security, LLC.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/direct/dio_io.cfg

#
# DESCRIPTION:
# 	Verify different recordsizes are supported for dRAID using Direct IO.
#
# STRATEGY:
#	1. Create dRAID pool.
#	2. Start some mixed readwrite direct IO.
#	3. Verify the results are as expected using buffered IO.
#

verify_runnable "global"

function cleanup
{
	if poolexists $TESTPOOL1; then
		destroy_pool $TESTPOOL1
	fi

 	rm -f $VDEVS
}

log_assert "Verify different recordsizes are supported for dRAID using Direct IO."

log_onexit cleanup

VDEV1=$TEST_BASE_DIR/file1
VDEV2=$TEST_BASE_DIR/file2
VDEV3=$TEST_BASE_DIR/file3
VDEVS="$VDEV1 $VDEV2 $VDEV3"

log_must truncate -s $MINVDEVSIZE $VDEV1 $VDEV2 $VDEV3

for recsize in "1024" "4096" "128k" "1m"; do
	create_pool $TESTPOOL1 draid $VDEVS
	log_must eval "zfs create \
	    -o recordsize=$recsize $TESTPOOL1/$TESTFS1"

	mntpnt=$(get_prop mountpoint $TESTPOOL1/$TESTFS1)
	dir="--directory=$mntpnt"

	for bs in "4k" "128k" "1m"; do
		args="--bs=$bs"

		# Mixed readwrite direct IO workload with verify,
		# then re-verify using standard buffered IO.
		log_must fio $dir $args $FIO_RW_ARGS
		log_must fio $dir $args $FIO_VERIFY_ARGS
		log_must rm -f "$mntpnt/rw*"

		# Mixed random readwrite direct IO workload with
		# verify, then re-verify using standard buffered IO.
		log_must fio $dir $args $FIO_RANDRW_ARGS
		log_must fio $dir $args $FIO_VERIFY_ARGS
		log_must rm -f "$mntpnt/rw*"
	done

	destroy_pool $TESTPOOL1
done

log_pass "Verified different recordsizes are supported for dRAID using Direct IO."
