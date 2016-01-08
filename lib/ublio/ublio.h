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

#ifndef UBLIO_H
#define UBLIO_H

#define UBLIO_API_VERSION(maj, min) 100 * (maj) + (min)
#define UBLIO_CURRENT_API UBLIO_API_VERSION(0, 1)
#ifndef UBLIO_USE_API
#define UBLIO_USE_API UBLIO_CURRENT_API
#endif

typedef int32_t ub_items_t;
typedef uint64_t ub_time_t;

struct iovec;

/*
 * Parameters needed to set up an ublio session.
 */
struct ublio_param {

        /* private data for doing I/O */
	void *up_priv;

	/*
	 * Basic I/O primitives used by ublio. They look and are
	 * expected to behave similarly to their POSIX-ish counterparts
	 * pread(2), ..., pwritev(2), except that they get a generic
	 * pointer instead of a file descriptor integer. (I say
	 * "POSIX-ish" because preadv/pwritev are not part of any
	 * standard I know of).
	 *
	 * If you set these to NULL, then the stock POSIX-ish counterparts
	 * will be used. In this case you are expected to set up_priv
	 * to the memory address of the file descriptor to be used.
	 *
	 * If your OS doesn't support preadv, pwritev, and these
	 * operations are set to NULL, then they will be emulated with
	 * the help of the up_pread, up_pwrite methods.
	 */
	ssize_t (*up_pread)  (void *priv, void *buf, size_t count, off_t off);
	ssize_t (*up_preadv) (void *priv, struct iovec *iov, int icnt,
	                      off_t off);
	ssize_t (*up_pwrite) (void *priv, void *buf, size_t count, off_t off);
	ssize_t (*up_pwritev)(void *priv, struct iovec *iov, int icnt,
	                      off_t off);

	/* actual reads / writes will be  multiples of this quantity */
	size_t up_blocksize;
	/* 
	 * Number of cache entries. Each can store an up_blocksize amount
	 * of data. Set to 0 to disable caching.
	 */
	ub_items_t up_items;
	/* a cache entry will refuse being recycled this many times */
	ub_time_t up_grace;
	/* if set to 1, all writes will be immediately executed */
	uint8_t up_sync_io:1; 

};

/*
 * Opaque data identifying an ublio session
 */
typedef struct ublio_filehandle* ublio_filehandle_t;

/*
 * takes an ublio_param set up according to your need, gives you an
 * ublio_filehandle ready to use (or NULL if something went wrong)
 */
ublio_filehandle_t
        ublio_open (struct ublio_param *up);

/* Ublio analogues of POSIX I/O functions */
int     ublio_close (ublio_filehandle_t ufh);
ssize_t ublio_pread (ublio_filehandle_t ufh, void *buf, size_t count,
                     off_t off);
ssize_t ublio_pwrite(ublio_filehandle_t ufh, void *buf, size_t count,
                     off_t off);
int     ublio_fsync (ublio_filehandle_t ufh);

#endif
