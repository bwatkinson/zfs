// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (C) 2007-2010 Lawrence Livermore National Security, LLC.
 *  Copyright (C) 2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Brian Behlendorf <behlendorf1@llnl.gov>.
 *  UCRL-CODE-235197
 *
 *  This file is part of the SPL, Solaris Porting Layer.
 *
 *  The SPL is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  The SPL is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the SPL.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _SPL_CMN_ERR_H
#define	_SPL_CMN_ERR_H

#if defined(_KERNEL) && defined(HAVE_STANDALONE_LINUX_STDARG)
#include <linux/stdarg.h>
#else
#include <stdarg.h>
#endif
#include <sys/atomic.h>

#define	CE_CONT		0 /* continuation */
#define	CE_NOTE		1 /* notice */
#define	CE_WARN		2 /* warning */
#define	CE_PANIC	3 /* panic */
#define	CE_IGNORE	4 /* print nothing */

extern void cmn_err(int, const char *, ...)
    __attribute__((format(printf, 2, 3)));
extern void vcmn_err(int, const char *, va_list)
    __attribute__((format(printf, 2, 0)));
extern void vpanic(const char *, va_list)
    __attribute__((format(printf, 1, 0), __noreturn__));

#define	fm_panic	panic

#define	cmn_err_once(ce, ...)				\
do {							\
	static volatile uint32_t printed = 0;		\
	if (atomic_cas_32(&printed, 0, 1) == 0) {	\
		cmn_err(ce, __VA_ARGS__);		\
	}						\
} while (0)

#define	vcmn_err_once(ce, fmt, ap)			\
do {							\
	static volatile uint32_t printed = 0;		\
	if (atomic_cas_32(&printed, 0, 1) == 0) {	\
		vcmn_err(ce, fmt, ap);			\
	}						\
} while (0)

#endif /* SPL_CMN_ERR_H */
