/*-
 * Copyright 2006 Csaba Henk <csaba.henk@creo.hu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Assertions are checked only if CHECK_ASSERTIONS is not 0 */
#define CHECK_ASSERTIONS 1

/* 
 * If ONE_MALLOC is 1, data for an ublio session is allocated in one step.  If
 * it's zero, each chunk of data is allocated via a disctinct malloc(3).
 * Former method is faster, latter is more likely to uncover off-buffer writes.
 */
#define ONE_MALLOC       1

/*
 * If WATCH_VALID is not zero, some extra assertions are enabled in order to
 * check if the "valid" flag for cache items is set properly.
 */
#define WATCH_VALID      0

/*
 * Does the operating system support the preadv(2), pwritev(2) syscalls?  As a
 * rudimentary guess, we simply set this attribute based on the type of the
 * underlying OS, but this neither excludes false positives, nor false
 * negatives.
 */
#ifdef __FreeBSD__
#define HAS_PIOV         1
#else
#define HAS_PIOV         0
#endif
