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
 * Copyright (c) 2022 by Triad National Security, LLC.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifndef MIN
#define	MIN(a, b)	((a) < (b)) ? (a) : (b)
#endif

static char *outputfile = NULL;
static int blocksize = 131072; /* 128K */
static int numblocks = 8;
static double runtime = 10; /* 10 seconds */
static char *execname = NULL;
static int print_usage = 0;
static int randompattern = 0;
static int ofd;
char *buf = NULL;
static int entire_file_written = 0;

static void
usage(void)
{
	(void) fprintf(stderr,
	    "usage %s -o outputfile [-b blocksize] [-n numblocks]\n"
	    "         [-p randpattern] [-r runtime] [-h help]\n"
	    "\n"
	    "Testing whether checksum verify works correctly for O_DIRECT.\n"
	    "when manipulating the contents of a userspace buffer.\n"
	    "\n"
	    "    outputfile:  File to write to.\n"
	    "    blocksize:   Size of each block to write (must be at least \n"
	    "                 >= 512).\n"
	    "    numblocks:   Total number of blocksized blocks to write.\n"
	    "    randpattern: Fill data buffer with random data. Default \n"
	    "                 behavior is to fill the buffer with the known \n"
	    "                 data pattern (0xdeadbeef).\n"
	    "    runtime:     Total amount of time to run test in seconds.\n"
	    "                 Test will run till the specified runtime or\n"
	    "                 when blocksize * numblocks data is written.\n"
	    "    help:        Print usage information and exit.\n"
	    "\n"
	    "    Required parameters:\n"
	    "    outputfile\n"
	    "\n"
	    "    Default Values:\n"
	    "    blocksize   -> 131072\n"
	    "    numblocks   -> 8\n"
	    "    randpattern -> false\n"
	    "    runtime     -> 10 seconds\n",
	    execname);
	(void) exit(1);
}

static void
parse_options(int argc, char *argv[])
{
	int c;
	int errflag = 0;
	extern char *optarg;
	extern int optind, optopt;
	execname = argv[0];

	while ((c = getopt(argc, argv, "b:hn:o:pr:")) != -1) {
		switch (c) {
			case 'b':
				blocksize = atoi(optarg);
				break;

			case 'h':
				print_usage = 1;
				break;

			case 'n':
				numblocks = atoi(optarg);
				break;

			case 'o':
				outputfile = optarg;
				break;

			case 'p':
				randompattern = 1;
				break;

			case 'r':
				runtime = (double)atoi(optarg);
				break;

			case ':':
				(void) fprintf(stderr,
				    "Option -%c requires an opertand\n",
				    optopt);
				errflag++;
				break;
			case '?':
			default:
				(void) fprintf(stderr,
				    "Unrecognized option: -%c\n", optopt);
				errflag++;
				break;
		}
	}

	if (errflag || print_usage == 1)
		(void) usage();

	if (blocksize < 512 || outputfile == NULL || runtime <= 0 ||
	    numblocks <= 0) {
		(void) fprintf(stderr,
		    "Required paramater(s) missing or invalid.\n");
		(void) usage();
	}
}

/*
 * Continually write to the file using O_DIRECT from the range of 0 to
 * blocsize * numblocks for the requested runtime.
 */
static void *
write_thread(void *arg)
{
	time_t start;
	size_t offset = 0;
	int total_data = blocksize * numblocks;
	int left = total_data;
	(void) arg;

	time(&start);
	while (difftime(time(NULL), start) < runtime ||
	    !entire_file_written) {
		pwrite(ofd, buf, blocksize, offset);
		offset = ((offset + blocksize) % total_data);
		if (left > 0)
			left -= blocksize;
		else
			entire_file_written = 1;
	}

	pthread_exit(NULL);
}

/*
 * Update the buffers contents with random data.
 */
static void *
manipulate_buf_thread(void *arg)
{
	time_t start;
	size_t rand_offset;
	char rand_char;
	(void) arg;

	time(&start);
	while (difftime(time(NULL), start) < runtime ||
	    !entire_file_written) {
		rand_offset = (rand() % blocksize);
		rand_char = (rand() % (126 - 33) + 33);
		buf[rand_offset] = rand_char;
	}

	pthread_exit(NULL);
}

int
main(int argc, char *argv[])
{
	char *datapattern = "0xdeadbeef";
	int ofd_flags = O_WRONLY | O_CREAT | O_DIRECT;
	mode_t mode = S_IRUSR | S_IWUSR;
	pthread_t write_thr;
	pthread_t manipul_thr;
	int left = blocksize;
	int offset = 0;
	int rc;
	int ifd;
	int ifd_flags = O_RDONLY;
	char *output_buf = NULL;
	struct stat st;

	parse_options(argc, argv);

	ofd = open(outputfile, ofd_flags, mode);
	if (ofd == -1) {
		(void) fprintf(stderr, "%s, %s\n", execname, outputfile);
		perror("open");
		exit(2);
	}

	int err = posix_memalign((void **)&buf, sysconf(_SC_PAGE_SIZE),
	    blocksize);
	if (err != 0) {
		(void) fprintf(stderr,
		    "%s: %s\n", execname, strerror(err));
		exit(2);
	}

	output_buf = (char *)malloc(sizeof (char) * blocksize * numblocks);
	if (output_buf == NULL) {
		(void) fprintf(stderr,
		    "%s: failed to allocate output_buf\n", execname);
		exit(2);
	}

	if (!randompattern) {
		/* Putting known data pattern in buffer */
		while (left) {
			size_t amt = MIN(strlen(datapattern), left);
			memcpy(&buf[offset], datapattern, amt);
			offset += amt;
			left -= amt;
		}
	} else {
		/* Putting random data in buffer */
		for (int i = 0; i < blocksize; i++)
			buf[i] = rand();
	}

	if ((rc = pthread_create(&write_thr, NULL, write_thread, NULL))) {
		fprintf(stderr, "error: pthreads_create, write_thr, "
		    "rc: %d\n", rc);
		exit(2);
	}

	if ((rc = pthread_create(&manipul_thr, NULL, manipulate_buf_thread,
	    NULL))) {
		fprintf(stderr, "error: pthreads_create, manipul_thr, "
		    "rc: %d\n", rc);
		exit(2);
	}

	pthread_join(write_thr, NULL);
	pthread_join(manipul_thr, NULL);

	(void) close(ofd);

	/*
	 * Now just going to read the contents of the file to check for
	 * checksum errors.
	 */
	stat(outputfile, &st);
	ssize_t outputfile_size = st.st_size;
	ifd = open(outputfile, ifd_flags);
	if (ifd == -1) {
		(void) fprintf(stderr, "%s, %s\n", execname, outputfile);
		perror("open");
		exit(2);
	}
	int c = read(ifd, output_buf, outputfile_size);
	if (c != outputfile_size) {
		if (c < 0) {
			perror("read");
		} else {
			(void) fprintf(stderr,
			    "%s: unexpected short read, read %d "
			    "bytes, expepted %d\n", execname,
			    c, blocksize * numblocks);
		}
		exit(2);
	}

	(void) close(ifd);


	free(buf);
	free(output_buf);

	return (0);
}
