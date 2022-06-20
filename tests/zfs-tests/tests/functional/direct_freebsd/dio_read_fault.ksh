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

#
# DESCRIPTION:
# 	Just getting trace from dmesg for failure with O_DIRECT reads from
#	vm_fault_quick_hold_pages().
#
# STRATEGY:
#	1. Write 3 blocks using O_DIRECT.
#	2. Read back those 3 blocks using O_DIRECT.
#	3. Fail test so dmesg output from printf statements in code show trace.
#

verify_runnable "global"

function cleanup
{
	log_must rm -f $filename
}

log_assert "Just creating dmesg output to dump for issues for O_DIRECT reads."

if is_linux; then
	log_unsupported "Linux does not support stable pages for O_DIRECT \
	     writes"
fi

log_onexit cleanup

mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
log_must zfs set recordsize=128k $TESTPOOL/$TESTFS
filename="$mntpnt/foobar.dat"

log_must fio --filename=$filename --name=direct-write --rw=write --size=384k \
    --bs=128k --direct=1 --numjobs=1 --ioengine=psync --fallocate=none \
    --minimal
log_must fio --filename=$filename --name=direct-read --rw=read --size=384k \
    --bs=128k --direct=1 --numjobs=1 --ioengine=psync --minimal

log_fail "Dumping dmesg of printf statements for traces."
