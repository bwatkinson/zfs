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
# 	Verify compression works using Direct IO.
#
# STRATEGY:
#	1. Create multidisk pool.
#	2. Start some mixed readwrite direct IO.
#	3. Verify the results are as expected using buffered IO.
#

verify_runnable "global"

function cleanup
{
	log_must rm -f "$mntpnt/rw*"
	# Just resetting the dataset back to the standard recordsize
	zfs set recordsize=128k $TESTPOOL/$TESTFS
	# Turning compression back off
	log_must zfs set compression=off $TESTPOOL/$TESTFS
}

log_assert "Verify compression works using Direct IO."

log_onexit cleanup

mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
dir="--directory=$mntpnt"

log_must zfs set compression=lz4 $TESTPOOL/$TESTFS

for recsize in "1024" "4096" "128k" "1m"; do
	log_must zfs set recordsize=$recsize $TESTPOOL/$TESTFS

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
done

log_pass "Verfied compression works using Direct IO"
