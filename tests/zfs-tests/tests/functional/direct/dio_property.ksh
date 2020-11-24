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
# Copyright (c) 2021 by Lawrence Livermore National Security, LLC.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/direct/dio.cfg
. $STF_SUITE/tests/functional/direct/dio.kshlib

#
# DESCRIPTION:
# 	Verify the direct=always|disabled|standard property
#
# STRATEGY:
#	1. Verify direct=always behavior
#	2. Verify direct=disabled behavior
#	3. Verify direct=standard behavior
#

verify_runnable "global"

if is_freebsd; then
	log_unsupported "Requires /proc/spl/kstat/zfs/*/objset-* file"
fi

function cleanup
{
	zfs set direct=standard $TESTPOOL/$TESTFS
	log_must rm -f $tmp_file
}

function get_objset_stat # pool objset stat
{
	typeset pool=$1
	typeset objset=$2
	typeset stat=$3

	objset_file=/proc/spl/kstat/zfs/$pool/objset-$objset
	val=$(grep -m1 "$stat" $objset_file | awk '{ print $3 }')
	if [[ -z "$val" ]]; then
		log_fail "Unable to read $stat counter"
	fi

	echo "$val"
}

function check_objset_stat_eq # pool objset stat expected
{
	typeset pool=$1
	typeset objset=$2
	typeset stat=$3
	typeset expected=$4

	val=$(get_objset_stat $pool $objset $stat)
	if [[ $expected -ne $val ]]; then
		log_must cat /proc/spl/kstat/zfs/$pool/objset-$objset
		log_fail "Expected $expected $stat got $val"
	fi

	return 0
}

log_assert "Verify the direct=always|disabled|standard property"

log_onexit cleanup

mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
objset=$(get_objset_id $TESTPOOL/$TESTFS)
rs=$(get_prop recordsize $TESTPOOL/$TESTFS)

log_note "objset $TESTPOOL/$TESTFS objset ID $objset"

tmp_file=$mntpnt/tmp_file
page_size=$(getconf PAGESIZE)
file_size=1048576
count=8

#
# Check when "direct=always" any aligned IO is done as direct.
# Note that "flag=direct" is not set in the following calls to dd(1).
#
log_must zfs set direct=always $TESTPOOL/$TESTFS

log_note "Aligned writes (2 buffered, then all direct)"
check_write $TESTPOOL $objset $tmp_file $rs $count 0 "" 2 $((count - 1))

log_note "Aligned overwrites"
check_write $TESTPOOL $objset $tmp_file $rs $count 0 "" 0 $count

log_note "Sub-recordsize unaligned overwrites"
check_write $TESTPOOL $objset $tmp_file $((rs / 2)) $((2 * count)) 0 \
    "" $((2 * count)) 0

log_note "Sub-page size aligned overwrites"
check_write $TESTPOOL $objset $tmp_file 512 $count 0 "" $count 0

log_note "Aligned reads"
check_read $TESTPOOL $objset $tmp_file $rs $count 0 "" 0 $count

log_note "Sub-recordsize unaligned reads"
check_read $TESTPOOL $objset $tmp_file $((rs / 2)) $((count * 2)) 0 \
    "" 0 $((2 * count))

log_note "Sub-page size aligned reads"
check_read $TESTPOOL $objset $tmp_file 512 $count 0 "" $count 0

log_must rm -f $tmp_file


#
# Check when "direct=disabled" there are never any direct requests.
# Note that "flag=direct" is always set in the following calls to dd(1).
#
log_must zfs set direct=disabled $TESTPOOL/$TESTFS

log_note "Aligned writes (all buffered with an extra for create)"
check_write $TESTPOOL $objset $tmp_file $rs $count 0 "direct" $((count + 1)) 0

log_note "Aligned overwrites"
check_write $TESTPOOL $objset $tmp_file $rs $count 0 "direct" $count 0

log_note "Aligned reads"
check_read $TESTPOOL $objset $tmp_file $rs $count 0 "direct" $count 0

log_must rm -f $tmp_file


#
# Check when "direct=standard" only requested direct IO occur.
#
log_must zfs set direct=standard $TESTPOOL/$TESTFS

log_note "Aligned writes/overwrites (buffered / direct)"
check_write $TESTPOOL $objset $tmp_file $rs $count 0 "" $((count + 1)) 0
check_write $TESTPOOL $objset $tmp_file $rs $count 0 "direct" 0 $count

log_note "Aligned reads (buffered / direct)"
check_read $TESTPOOL $objset $tmp_file $rs $count 0 "" $count 0
check_read $TESTPOOL $objset $tmp_file $rs $count 0 "direct" 0 $count

log_pass "Verify the direct=always|disabled|standard property"
