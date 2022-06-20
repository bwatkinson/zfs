#!/bin/ksh -p
#
# CDDL HEADER START
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
# Copyright (c) 2022 by Triad National Security, LLC.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/direct/dio.cfg
. $STF_SUITE/tests/functional/direct/dio.kshlib

#
# DESCRIPTION:
# 	Verify checksum verify works for Direct IO writes.
#
# STRATEGY:
#	1. Set the module parameter zio_direct_write_verify to 1.
#	2. Start FIO Direct IO write workload and first make sure we do not
#          have any failures using zpool status -d or zevent.
#	3. Start a Direct IO write workload while manipulating the user buffer.
#	4. Verify there are Direct IO write verify failures using
#          zpool status -d and checking for zevents.
#	5. Make sure there are no reported data errors because enabling
#          Direct IO write verifies will not lead to any data corruption if
#          there was checksum failure before the write was committed to a VDEV.
#

verify_runnable "global"

function cleanup
{
	log_must rm -f "$mntpnt/direct-write.iso"
	# Clearing out DIO VERIFY counts for Zpool
	log_must zpool clear $TESTPOOL
	# Clearing out dio_verify from event logs
	log_must zpool events -c
	log_must eval "echo 0 > \
	    /sys/module/zfs/parameters/zio_direct_write_verify"
}

log_assert "Verify checksum verify works for Direct IO writes."

if is_freebsd; then
	log_unsupported "FeeBSD is capable of stable pages for O_DIRECT writes"
fi

log_onexit cleanup

mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)

# Enabling Direct IO write checksum verify
log_must eval "echo 1 > /sys/module/zfs/parameters/zio_direct_write_verify"

# Compression must be turned off to avoid other ASSERT failures due to
# manipulating the contents of the user buffer while doing Direct IO.
log_must zfs set compression=off $TESTPOOL/$TESTFS

log_must zfs set recordsize=128k $TESTPOOL/$TESTFS

# Getting current Direct IO write count
prev_dio_wr=$(get_iostats_stat $TESTPOOL direct_write_count)

# First verify that we do not have any checksum verify errors with a
# FIO workload.
log_must fio --directory=$mntpnt --name=direct-write --rw=write \
    --size=$DIO_FILESIZE --bs=128k --direct=1 --numjobs=1 \
    --ioengine=psync --fallocate=none --group_reporting --minimal

# Getting new Direct IO write count, Direct IO write checksum verify
# errors and zevents.
curr_dio_wr=$(get_iostats_stat $TESTPOOL direct_write_count)
DIO_VERIFYS=$(zpool status -dp | awk -v d="raidz" '$0 ~ d {print $6}')
DIO_VERIFY_EVENTS=$(zpool events | grep -c dio_verify)

log_must [ $DIO_VERIFYS -eq 0 ]
log_must [ $DIO_VERIFY_EVENTS -eq 0 ]
log_must [ $curr_dio_wr -gt $prev_dio_wr ]

log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_must rm -f "$mntpnt/direct-write*"

prev_dio_wr=$curr_dio_wr

# Now we manipulate the buffer and make sure we do see checksum verify
# errors with zpool status -d and in the zevents.
log_must manipulate_user_buffer -o "$mntpnt/direct-write.iso" -r 5

# Getting new Direct IO write count, Direct IO write checksum verify
# errors and zevents.
curr_dio_wr=$(get_iostats_stat $TESTPOOL direct_write_count)
DIO_VERIFYS=$(zpool status -dp | awk -v d="raidz" '$0 ~ d {print $6}')
DIO_VERIFY_EVENTS=$(zpool events | grep -c dio_verify)

log_must [ $DIO_VERIFYS -gt 0 ]
log_must [ $DIO_VERIFY_EVENTS -gt 0 ]
log_must [ $curr_dio_wr -gt $prev_dio_wr ]

log_must check_pool_status $TESTPOOL "errors" "No known data errors"

log_pass "Correctly saw $DIO_VERIFYS direct write verify errors \
    and $DIO_VERIFY_EVENTS direct write verify events"
