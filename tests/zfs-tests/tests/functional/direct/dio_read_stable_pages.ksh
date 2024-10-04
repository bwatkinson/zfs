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
# Copyright (c) 2024 by Triad National Security, LLC.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/direct/dio.cfg
. $STF_SUITE/tests/functional/direct/dio.kshlib

#
# DESCRIPTION:
# 	Verify stable pages work for O_DIRECT reads.
#
# STRATEGY:
#	1. Start a Direct I/O read workload while manipulating the user
#	   buffer.
#	2. Verify there is no checksum errors reported from zpool status.
#	3. Repeat steps 1 and 2 for 3 iterations.
#	4. Repeat 1-3 but with compression disabled.
#

verify_runnable "global"

function cleanup
{
	log_must rm -f "$mntpnt/direct-read.iso"
}

log_assert "Verify stable pages work for Direct I/O reads."

if is_linux; then
	log_unsupported "Linux does not support stable pages for O_DIRECT \
	     reads"
fi

log_onexit cleanup

ITERATIONS=3
NUMBLOCKS=300
BS=$((128 * 1024)) #128k
mntpnt=$(get_prop mountpoint $TESTPOOL/$TESTFS)
log_must zfs set recordsize=128k $TESTPOOL/$TESTFS

# First create the file using Direct I/O
log_must stride_dd -i /dev/urandom -o $mntpnt/direct-read.iso -b $BS \
   -c $NUMBLOCKS -D

for compress in "on" "off";
do
	log_must zfs set compression=$compress $TESTPOOL/$TESTFS

	for i in $(seq 1 $ITERATIONS); do
		log_note "Verifying stable pages for Direct I/O reads \
		    iteration $i of $ITERATIONS"

		prev_dio_rd=$(get_iostats_stat $TESTPOOL direct_read_count)
		prev_arc_rd=$(get_iostats_stat $TESTPOOL arc_read_count)

		# Manipulate the user's buffer while running O_DIRECT read
		# workload with the buffer.
		log_must manipulate_user_buffer -f "$mntpnt/direct-read.iso" \
		    -n $NUMBLOCKS -b $BS -r

		curr_dio_rd=$(get_iostats_stat $TESTPOOL direct_read_count)
		curr_arc_rd=$(get_iostats_stat $TESTPOOL arc_read_count);
		total_dio_rd=$((curr_dio_rd - prev_dio_rd))
		total_arc_rd=$((curr_arc_rd - prev_arc_rd))

		log_note "Making sure we have Direct I/O reads logged"
		if [[ $total_dio_rd -lt 1 ]]; then
			log_fail "No Direct I/O reads $total_dio_rd"
		fi
		log_note "Making sure there are no ARC reads logged"
		if [[ $total_arc_rd -ne 0 ]]; then
			log_fail "There are ARC reads $total_arc_rd"
		fi

		# Making sure there are no data errors for the zpool
		log_note "Making sure there are no checksum errors with the ZPool"
		log_must check_pool_status $TESTPOOL "errors" \
		    "No known data errors"
		# Making sure there there are no DIO errors reported for zpool
		log_note "Making sure we have no Direct I/O read checksum verify with ZPool"
		check_dio_chksum_verify_failures $TESTPOOL "raidz" 0 "rd"
	done
done

log_pass "Verified stable pages work for Direct I/O reads."
