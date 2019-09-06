/*
 * null - a null compression algorithm
 * Header File
 * Copyright (C) 2019, Eideticom
 * BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <sys/zfs_context.h>

/*ARGSUSED*/
size_t
nullcp_compress(void *s_start, void *d_start, size_t s_len,
    size_t d_len, int n)
{

	/*
	 * Perform a s_len memcpy from s_start to d_start and return
	 * s_len. Note, this will cause a compression abort in the
	 * layer above but that does not matter since the SW path has
	 * at least been tested.
	 */
	memcpy(d_start, s_start, s_len);
	return (s_len);
}

/*ARGSUSED*/
int
nullcp_decompress(void *s_start, void *d_start, size_t s_len,
    size_t d_len, int n)
{
	/*
	 * Perform a s_len memcpy from s_start to d_start and return
	 * s_len.
	*/
	memcpy(d_start, s_start, s_len);
	return (s_len);
}

/*ARGSUSED*/
size_t
null_compress(void *s_start, void *d_start, size_t s_len,
    size_t d_len, int n)
{

	/*
	 * Return s_len. Note, this will cause a compression abort in
	 * the layer above but that does not matter since the SW path
	 * has at least been tested.
	 */
	return (s_len);
}

/*ARGSUSED*/
int
null_decompress(void *s_start, void *d_start, size_t s_len,
    size_t d_len, int n)
{
	/*
	 * Return s_len. This probably causes odd things to happen so
	 * you may prefer to use nullcp.
	 */
	return (s_len);
}
