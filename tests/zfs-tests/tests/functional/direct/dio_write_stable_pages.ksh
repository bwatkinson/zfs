#!/bin/ksh -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or https://opensource.org/licenses/CDDL-1.0.
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
# 	Verify stable pages work for O_DIRECT writes.
#
# STRATEGY:
#	1. Start a Direct IO write workload while manipulating the user buffer.
#	2. Verify there is no checksum errors reported from zpool status.
#	3. Repeat 1 and 2 but with compression disabled.
#

verify_runnable "global"

function cleanup
{
	log_must rm -f "$mntpnt/direct-write.iso"
}

log_assert "Verify stable pages work for Direct IO writes."

if is_linux; then
	log_unsupported "Linux does not support stable pages for O_DIRECT \
	     writes"
fi

log_onexit cleanup

mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
log_must zfs set recordsize=128k $TESTPOOL/$TESTFS

for compress in "on" "off";
do
	log_must zfs set compression=$compress $TESTPOOL/$TESTFS
	# Manipulate the user's buffer while running O_DIRECT write worload
	# with the buffer.
	log_must manipulate_user_buffer -o "$mntpnt/direct-write.iso" -r 5

	# Making sure there are no data errors for the zpool
	log_must check_pool_status $TESTPOOL "errors" "No known data errors"
done

log_pass "Verified stable pages work for Direct IO writes."
