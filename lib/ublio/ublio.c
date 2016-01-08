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

#ifdef linux
#define _XOPEN_SOURCE 500
#endif

#include <sys/uio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <assert.h>
#ifndef __sun
#include <stdint.h>
#endif

#include "queue.h"
#include "tree.h"

#include "ublio.h"
#include "ublio_conf.h"

#ifndef IOV_MAX
#define IOV_MAX UIO_MAXIOV
#endif

#if CHECK_ASSERTIONS
#define ASSERT assert
#else
#define ASSERT(...)
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))

#if WATCH_VALID
#define SHORT_OCCURED(ufh) (ufh)->uf_short
#define MARK_SHORT(ufh) ((ufh)->uf_short = 1)
#else
#define SHORT_OCCURED(ufh) 1 
#define MARK_SHORT(ufh)
#endif

enum {
	BLK_BUF    = -1,
	BLK_FRAG   = -2,
} iov_type;

struct ublio_cache {
	off_t uc_off;
	ub_time_t uc_atime;
	ub_items_t uc_idx;
	char *uc_buf;
	RB_ENTRY(ublio_cache) uc_rnode;
	RB_ENTRY(ublio_cache) uc_cnode;
	LIST_ENTRY(ublio_cache) uc_dirty_link;
	uint8_t uc_dirty:1;
	uint8_t uc_valid:1;
};

struct ublio_filehandle {
	struct ublio_param uf_p;
	struct ublio_cache *uf_cache;
	struct iovec *uf_iovs;
	ub_items_t *uf_iometa;
	ub_time_t uf_time;
	char *uf_fragments;
	RB_HEAD(ublio_recycle_tree, ublio_cache) uf_rroot;
	RB_HEAD(ublio_cache_tree, ublio_cache) uf_croot;
	LIST_HEAD(, ublio_cache) uf_dirty_head;
#if WATCH_VALID
	uint8_t uf_short:1;
#endif
};

static inline size_t
u_blocksize(ublio_filehandle_t ufh)
{
	return ufh->uf_p.up_blocksize;
}

static inline uint8_t
u_sync_io(ublio_filehandle_t ufh)
{
	return ufh->uf_p.up_sync_io;
}

/*
 * Max number of iovs that can be resulted by splitting the range as we do.
 * Worst possible iovec layout:
 * [fragment, buffer] + [cache, buffer] * up_items + [buffer, fragment]
 */
#define IOVITEMS(up) ((2 * (up)->up_items) + 4)

/*
 * Red-black tree generation with Niels Provos' tree macros
 */

#define SORT_BY(x, y, param, escape)				\
	 (((x)param < (y)param) ?				\
	  -1 : (((x)param > (y)param) ? 1 : (escape)))

static inline int
ublio_cache_cmp(struct ublio_cache *ubc1, struct ublio_cache *ubc2)
{
	return SORT_BY(ubc1, ubc2, ->uc_off, 0);
}

static inline int
ublio_recyc_cmp(struct ublio_cache *ubc1, struct ublio_cache *ubc2)
{
	return SORT_BY(ubc1, ubc2, ->uc_atime,
	               SORT_BY(ubc1, ubc2, ->uc_off,
	                       SORT_BY(ubc1, ubc2, ->uc_idx, 0)));
}

RB_GENERATE_STATIC(ublio_recycle_tree, ublio_cache, uc_rnode, ublio_recyc_cmp)
RB_GENERATE_STATIC(ublio_cache_tree, ublio_cache, uc_cnode, ublio_cache_cmp)


/* Ublio open. */
ublio_filehandle_t 
ublio_open(struct ublio_param *up)
{
	int i = 0;
	ublio_filehandle_t ufh;
	int cachesize    = up->up_items * sizeof(struct ublio_cache);
	int iovsize      = IOVITEMS(up) * sizeof(struct iovec);
	int metasize     = IOVITEMS(up) * sizeof(*ufh->uf_iometa);
	int fragsize     = 2 * up->up_blocksize;
#if ONE_MALLOC
	int cachebufsize = up->up_items * up->up_blocksize;
	char *buf;
#endif

	if (up->up_items < 0) {
		errno = EINVAL;
		return NULL;
	}

	ufh = malloc(sizeof(*ufh));
	if (!ufh)
		return NULL;
#if ONE_MALLOC
	buf = malloc(cachesize + cachebufsize + iovsize + metasize +
	             fragsize);
	if (!buf) {
		free(ufh);
		return NULL;
	}
#else
#define UFH_ALLOC(p, siz) do {		\
	p = malloc(siz);		\
	if (! p) {			\
		ublio_close(ufh);	\
		return NULL;		\
	}				\
} while (0)
	UFH_ALLOC(ufh->uf_cache, cachesize);
	UFH_ALLOC(ufh->uf_iovs, iovsize);
	UFH_ALLOC(ufh->uf_iometa, metasize);
	UFH_ALLOC(ufh->uf_fragments, fragsize);
#endif /* ONE_MALLOC */

	ufh->uf_time = up->up_grace + 1;
	RB_INIT(&ufh->uf_rroot);
	RB_INIT(&ufh->uf_croot);
	LIST_INIT(&ufh->uf_dirty_head);

#if ONE_MALLOC
	ufh->uf_cache  = (struct ublio_cache *)buf;
	for (i = 0; i < up->up_items; i++) {
		(ufh->uf_cache + i)->uc_off = -i - 1;
		(ufh->uf_cache + i)->uc_atime = 0;
		(ufh->uf_cache + i)->uc_idx = i;
		(ufh->uf_cache + i)->uc_dirty = 0;
		(ufh->uf_cache + i)->uc_valid = 1;
		(ufh->uf_cache + i)->uc_buf =
		  buf + cachesize + i * up->up_blocksize;
		RB_INSERT(ublio_recycle_tree, &ufh->uf_rroot,
		          ufh->uf_cache + i);
		RB_INSERT(ublio_cache_tree, &ufh->uf_croot,
		          ufh->uf_cache + i);
	}
	ufh->uf_iovs   = (struct iovec *)(
	                   buf + cachesize + cachebufsize
	                 );
	ufh->uf_iometa = (ub_items_t *)((char *)ufh->uf_iovs + iovsize);
	ufh->uf_fragments = (char *)ufh->uf_iometa + metasize;
#else
	memset(ufh->uf_cache, 0, cachesize);
	for (i = 0; i < up->up_items; i++) {
		(ufh->uf_cache + i)->uc_off = -i - 1;
		(ufh->uf_cache + i)->uc_atime = 0;
		(ufh->uf_cache + i)->uc_idx = i;
		(ufh->uf_cache + i)->uc_dirty = 0;
		(ufh->uf_cache + i)->uc_valid = 1;
		UFH_ALLOC((ufh->uf_cache + i)->uc_buf, up->up_blocksize);
		RB_INSERT(ublio_recycle_tree, &ufh->uf_rroot,
		          ufh->uf_cache + i);
		RB_INSERT(ublio_cache_tree, &ufh->uf_croot,
		          ufh->uf_cache + i);
	}
#undef UFH_ALLOC
#endif /* ONE_MALLOC */

	memcpy(&ufh->uf_p, up, sizeof(*up));

	return ufh;
}

/* Ublio close. */
int
ublio_close(ublio_filehandle_t ufh)
{
	int res;

	res = ublio_fsync(ufh);

#if ! ONE_MALLOC
	if (ufh->uf_cache) {
		int i;

		for (i = 0; i < ufh->uf_p.up_items; i++)
			free((ufh->uf_cache + i)->uc_buf);
	}
	free(ufh->uf_iovs);
	free(ufh->uf_iometa);
	free(ufh->uf_fragments);
#endif
	free(ufh->uf_cache);
	free(ufh);

	return res;
}


/*
 * Auxiliary inline functions
 */

static inline void
touch_cache(ublio_filehandle_t ufh, struct ublio_cache *ubc)
{
	if (ubc->uc_atime == ufh->uf_time)
		return;
	RB_REMOVE(ublio_recycle_tree, &ufh->uf_rroot, ubc);
	ubc->uc_atime = ufh->uf_time;
	RB_INSERT(ublio_recycle_tree, &ufh->uf_rroot, ubc);
}

static inline void
adj_dirty(ublio_filehandle_t ufh, struct ublio_cache *ubc, uint8_t state)
{
	if (state && ! ubc->uc_dirty)
		LIST_INSERT_HEAD(&ufh->uf_dirty_head, ubc, uc_dirty_link);
	if (! state && ubc->uc_dirty)
		LIST_REMOVE(ubc, uc_dirty_link);

	ubc->uc_dirty = state;
}
	
static inline int
withinbound(struct ublio_cache *ubc, off_t boff, size_t count)
{
	return (ubc->uc_off >= boff && ubc->uc_off < boff + count);
}

static inline struct ublio_cache *
atime_next(ublio_filehandle_t ufh, struct ublio_cache *ubc, off_t boff,
           size_t count)
{
	struct ublio_cache ubc_aux;

	ubc = RB_NEXT(ublio_recycle_tree, &ufh->uf_rroot, ubc);
	if (ubc == NULL || ! withinbound(ubc, boff, count))
		return ubc;

	ubc_aux.uc_atime = ubc->uc_atime;
	ubc_aux.uc_off = boff + count;
	ubc_aux.uc_idx = -1;
	return RB_NFIND(ublio_recycle_tree, &ufh->uf_rroot, &ubc_aux);
}

static inline ssize_t
u_pread(ublio_filehandle_t ufh, void *buf, size_t count, off_t off)
{
	return (ufh->uf_p.up_pread ?
	          ufh->uf_p.up_pread(ufh->uf_p.up_priv, buf, count, off) :
	          pread(*(int *)ufh->uf_p.up_priv, buf, count, off));
}

static inline ssize_t
u_preadv(ublio_filehandle_t ufh, struct iovec *iov, int icnt, off_t off)
{
#if HAS_PIOV
	return ufh->uf_p.up_preadv ?
	       ufh->uf_p.up_preadv(ufh->uf_p.up_priv, iov, icnt, off) :
	       preadv(*(int *)ufh->uf_p.up_priv, iov, icnt, off);
#else
	int i = 0;
	ssize_t pres, res = 0;

	if (ufh->uf_p.up_preadv)
		return ufh->uf_p.up_preadv(ufh->uf_p.up_priv, iov, icnt, off);

	for (; i < icnt; i++) {
		pres = u_pread(ufh, iov[i].iov_base, iov[i].iov_len,
		               off + res);

		if (pres == -1)
			return -1;

		res += pres;

		if (pres < iov[i].iov_len)
			break;
	}

	return res;
#endif
}

static inline ssize_t
u_pwrite(ublio_filehandle_t ufh, void *buf, size_t count, off_t off)
{
	return ufh->uf_p.up_pwrite ?
	       ufh->uf_p.up_pwrite(ufh->uf_p.up_priv, buf, count, off) :
	       pwrite(*(int *)ufh->uf_p.up_priv, buf, count, off);
}

static inline ssize_t
u_pwritev(ublio_filehandle_t ufh, struct iovec *iov, int icnt, off_t off)
{
#if HAS_PIOV
	return ufh->uf_p.up_pwritev ?
	       ufh->uf_p.up_pwritev(ufh->uf_p.up_priv, iov, icnt, off) :
	       pwritev(*(int *)ufh->uf_p.up_priv, iov, icnt, off);
#else
	int i = 0;
	ssize_t pres, res = 0;

	if (ufh->uf_p.up_pwritev)
		return ufh->uf_p.up_pwritev(ufh->uf_p.up_priv, iov, icnt, off);

	for (; i < icnt; i++) {
		pres = u_pwrite(ufh, iov[i].iov_base, iov[i].iov_len,
		                off + res);

		if (pres == -1)
			return -1;

		res += pres;

		if (pres < iov[i].iov_len)
			break;
	}

	return res;
#endif
}

static inline int
sync_cache(ublio_filehandle_t ufh, struct ublio_cache *ubc)
{
	ssize_t res;

	if (! ubc->uc_dirty)
		return 0;
	res = u_pwrite(ufh, ubc->uc_buf, ufh->uf_p.up_blocksize,
	                 ubc->uc_off);
	if (res >= 0)
		adj_dirty(ufh, ubc, 0);
	return res;
}


#define MOD(x) ((x) % u_blocksize(ufh))
#define NEGMOD(x) MOD((u_blocksize(ufh) - MOD(x)))
#define FLOOR(x) ((x) - MOD(x))
#define CEIL(x) ((x) + NEGMOD(x))

/* 
 * Generic ublio I/O backend.
 *
 * Packs up different kind of storages for doing I/O on the
 * smallest range which encloses the specified I/O range and is a
 * multiple of the blocksize (valid cache entries within the range,
 * to-be-reused cache entries, the buffer as-is, and fragments on
 * the edges to provide ephemeral completions for the specified range).
 *
 * This collection then will be passed on to the specific read / write
 * backends.
 */
static ssize_t 
ublio_pio(ublio_filehandle_t ufh, void *buf, size_t count, off_t boff,
          ssize_t (*iof)(ublio_filehandle_t ufh, ub_items_t iovcnt,
	                 void *buf, size_t count, off_t off))
{
#define PUSH_IOV(base, len, type) do {				\
	ASSERT(type < 0 ||					\
	       (ufh->uf_cache[type].uc_off >= boff &&		\
	        ufh->uf_cache[type].uc_off < boff + count));	\
	ufh->uf_iovs[i].iov_base = base;			\
	ufh->uf_iovs[i].iov_len = len;				\
	ufh->uf_iometa[i] = type;				\
	i++;							\
} while (0)

	off_t last_off;
	size_t frag;
	struct ublio_cache *ubc, *ubc_oldest;
	ub_items_t i = 0;
	ssize_t res;

	if (count == 0)
		return 0;
/*	Trust the caller...
	if (boff < 0)
		return -1;
 */

	frag = MOD(boff);
	/* boff is set to the bottom of the range to be processed */
	boff -= frag;
	last_off = boff;
	count += frag;

	ufh->uf_time++;

	/* look up the cache entry of the smallest offset in the range */
	ubc = RB_NFIND(ublio_cache_tree, &ufh->uf_croot,
	               (struct ublio_cache *)&boff);
	ASSERT(! ubc || SHORT_OCCURED(ufh) || ubc->uc_valid);
	if (ubc && ubc->uc_off >= boff + count)
		ubc = NULL;
	if (ubc)
		touch_cache(ufh, ubc);
	/*
	 * we look up the oldest cache entry among those who are
	 * out of the range
	 */
	ubc_oldest = RB_MIN(ublio_recycle_tree, &ufh->uf_rroot);
	if (ubc_oldest && withinbound(ubc_oldest, boff, count))
		ubc_oldest = atime_next(ufh, ubc_oldest, boff, count);

	for(;;) {
		/*
		 * we traverse entries in order 'till we get to
		 * the end of the range
		 */
		off_t xoff, curr_off;

		curr_off = ubc ? ubc->uc_off : boff + count;
		ASSERT(curr_off <= boff + count);

		for (xoff = last_off; xoff < curr_off;
		     xoff += u_blocksize(ufh)) {
			/*
			 * try to fill the subrange from the last processed
			 * to the current one with reusable cache entries
			 */

			struct ublio_cache *xubc;

			if (! ubc_oldest ||
			    ubc_oldest->uc_atime + ufh->uf_p.up_grace >=
			      ufh->uf_time)
				/*
				 * even the oldest candidate is young enough
				 * to be left intact -- can't utilize cache
				 */
				break;

			xubc = ubc_oldest;
			ASSERT(xubc->uc_off == -1 || SHORT_OCCURED(ufh) ||
			       xubc->uc_valid);
			ubc_oldest = atime_next(ufh, ubc_oldest, boff, count);

			if (sync_cache(ufh, xubc) < 0)
				return -1;
			/*
			 * It seems to be safe to modify the tree during
			 * traversal. The traversal is not even affected --
			 * we re-insert the node to a place which is already
			 * left behind.
			 */
			RB_REMOVE(ublio_cache_tree, &ufh->uf_croot, xubc); 
			xubc->uc_off = xoff;
			xubc->uc_valid = 0;
			RB_INSERT(ublio_cache_tree, &ufh->uf_croot, xubc); 

			touch_cache(ufh, xubc);

			/* push in a recycled (invalid)  cache entry */
			PUSH_IOV(xubc->uc_buf, u_blocksize(ufh),
			         xubc - ufh->uf_cache);
		}

		if (xoff < curr_off) {
			/*
			 * we found no reusable entry, I/O must
			 * be done directly to target buffer
			 */
			off_t bot = xoff, top = curr_off;
			char* xf = ufh->uf_fragments;

			if (xoff == boff && frag != 0) {
				/*
				 * we are at the first block, and it's only
				 * partly covered by the target buffer -- we
				 * utilize an ephemeral buffer for the missing
				 * fragment
				 */
				PUSH_IOV(xf, u_blocksize(ufh), BLK_FRAG);
				ASSERT(ufh->uf_iovs->iov_base == ufh->uf_fragments);
				bot += u_blocksize(ufh);
				xf  += u_blocksize(ufh);
			}

			if (curr_off == boff + count)
				top = FLOOR(top);

			if (top > bot)
				PUSH_IOV(buf + bot - (boff + frag),
				         top - bot, BLK_BUF);

			if (top >= bot && curr_off == boff + count &&
			    MOD(curr_off))
				/*
				 * At the top end of the range.
				 * I/O is to be done w/ an epehemeral
				 * buffer if top of the range is not
				 * offset aligned.
				 */
				PUSH_IOV(xf, u_blocksize(ufh), BLK_FRAG);
		}

		if (ubc) {
			/*
			 * push in a cache entry which is already at the right
			 * location (it's also valid unless we had some short
			 * reads/writes)
			 */
			ASSERT(SHORT_OCCURED(ufh) || ubc->uc_valid);
			PUSH_IOV(ubc->uc_buf, u_blocksize(ufh),
			         ubc - ufh->uf_cache);

			last_off = ubc->uc_off + u_blocksize(ufh);
			ubc = RB_NEXT(ublio_cache_tree, &ufh->uf_croot, ubc);
			if (ubc && ubc->uc_off >= boff + count)
				ubc = NULL;
			if (ubc)
				touch_cache(ufh, ubc);
		} else
			break;
	}

	/* do the I/O */
	ASSERT(i <= IOVITEMS(&ufh->uf_p));
	ASSERT(i <= CEIL(count) / u_blocksize(ufh));
	ASSERT(i > 0);
	res = iof(ufh, i, buf, count - frag, boff + frag);

	if (res == -1)
		return -1;
	ASSERT(res <= CEIL(count));

	return MAX(MIN(res, count) - frag, 0);

#undef PUSH_IOV
}


#define CENTRY(i) (ufh->uf_cache + ufh->uf_iometa[i])

/*
 * Backend for reading
 */
static ssize_t
ublio_block_pread(ublio_filehandle_t ufh, ub_items_t icnt, void *buf,
                  size_t count, off_t off)
{
	ub_items_t i0 = 0, i1;
	ssize_t res = 0, pres = -1, xpres = 0;

	for (;;) {
		ub_items_t i;
		off_t xoff = -1;

#define transferitem(i) 				\
/* signedness problems, duh...				\
ASSERT(CENTRY(i)->uc_off - off < count));		\
ASSERT(CENTRY(i)->uc_off - off > -u_blocksize(ufh));	\
 */							\
ASSERT(CENTRY(i)->uc_off - off < (off_t)count);		\
ASSERT(CENTRY(i)->uc_off + u_blocksize(ufh) > off);	\
memcpy(buf + MAX(0, CENTRY(i)->uc_off - off),		\
       CENTRY(i)->uc_buf +				\
	 MAX(0, off - CENTRY(i)->uc_off),		\
       MIN(u_blocksize(ufh) -				\
             MAX(0, off - CENTRY(i)->uc_off),		\
           count - MAX(0, CENTRY(i)->uc_off - off)));

		/*
		 * advance lower index 'till we found an iov where
		 * there is I/O to be done (ie., skip valid cache
		 * entries)
		 */
		while (i0 < icnt && ufh->uf_iometa[i0] >= 0 &&
		       CENTRY(i0)->uc_valid) {
			transferitem(i0);
			res += u_blocksize(ufh);
			i0++;
		}
		if (i0 == icnt)
			break;

		i1 = i0;
		/*
		 * starting from the lower index, advance upper index
		 * until we arrive to an entry where there is no I/O to be
		 * done
		 */
		xpres = 0;
		while (i1 < icnt &&
		       (ufh->uf_iometa[i1] < 0 || ! CENTRY(i1)->uc_valid) &&
		       i1 - i0 <= IOV_MAX) {
			xpres += ufh->uf_iovs[i1].iov_len;
			i1++;
		}

		/*
		 * find out the offset for reading into the iovs between
		 * the two indices
		 */
		if (ufh->uf_iometa[i0] == BLK_FRAG)
			xoff = FLOOR(off + (!!i0) * count);
		if (ufh->uf_iometa[i0] == BLK_BUF)
			xoff = off + 
			         ((void *)ufh->uf_iovs[i0].iov_base - buf);
		if (ufh->uf_iometa[i0] >= 0)
			xoff = CENTRY(i0)->uc_off;
		/* read! */
		pres = u_preadv(ufh, ufh->uf_iovs + i0, i1 - i0, xoff);
		res += pres;
		if (pres < xpres) {
			MARK_SHORT(ufh);
			if (pres == -1)
				return -1;
			res = FLOOR(res);

			break;
		}

		for (i = i0; i < i1; i++) {
			/* transfer cached data to the target buffer */

			if (ufh->uf_iometa[i] < 0)
				continue;

			CENTRY(i)->uc_valid = 1;
			transferitem(i);
#undef transferitem
		}

		i0 = i1;
	}

	if (ufh->uf_iometa[0] == BLK_FRAG && res > 0) {
		ASSERT(ufh->uf_iovs[0].iov_base == ufh->uf_fragments);
		ASSERT(MOD(off) || count < u_blocksize(ufh));
		memcpy(buf, ufh->uf_iovs[0].iov_base + MOD(off),
		       MIN(u_blocksize(ufh) - MOD(off), count));
	}
	if (icnt > 1 && ufh->uf_iometa[icnt - 1] == BLK_FRAG &&
	    pres == xpres) {
		memcpy(buf + count - MOD(off + count),
		       ufh->uf_iovs[icnt - 1].iov_base,
		       MOD(off + count));
	}

	return res;
}

/*
 * Backend for writing
 */
static ssize_t
ublio_block_pwrite(ublio_filehandle_t ufh, ub_items_t icnt, void *buf,
                   size_t count, off_t off)
{
	ub_items_t i = 0, low = 0, hi = icnt;
	ssize_t res = 0, pres = 0, xpres;

	/*
	 * XXX We treat edges in a distinguished way. This implies
	 * jumping back and forth between the two edges a few times,
	 * before going over the bulk of the range linearly.
	 * Can this cause a noticeable performace degradation, when
	 * doing big writes ?
	 */

	/* read data on edges if necessary */

#define RETURN_ON_SHORT_READ		\
	if (pres < u_blocksize(ufh)) {	\
		MARK_SHORT(ufh);	\
		return MIN(pres, 0);	\
	}

	if (ufh->uf_iometa[0] == BLK_FRAG) {
		pres = u_pread(ufh, ufh->uf_fragments, u_blocksize(ufh),
		               FLOOR(off));

		RETURN_ON_SHORT_READ

		memcpy(ufh->uf_fragments + MOD(off), buf,
		       MIN(u_blocksize(ufh) - MOD(off), count));

	}
	if (ufh->uf_iometa[0] >= 0 && ! CENTRY(0)->uc_valid &&
	    (CENTRY(0)->uc_off < off ||
	     off + count < CENTRY(0)->uc_off + u_blocksize(ufh))) {
		ASSERT(ufh->uf_iovs->iov_base == CENTRY(0)->uc_buf);
		pres = u_preadv(ufh, ufh->uf_iovs, 1, CENTRY(0)->uc_off);

		RETURN_ON_SHORT_READ
	}
	if (MOD(off) + count <= u_blocksize(ufh))
		goto done_with_tailread;
	i = icnt - 1;
	ASSERT(i > 0 || ufh->uf_iometa[i] == BLK_BUF);
	if (ufh->uf_iometa[i] == BLK_FRAG) {
		pres = u_pread(ufh,
		               ufh->uf_fragments +
		                 (ufh->uf_iometa[0] == BLK_FRAG) *
		                   u_blocksize(ufh),
		               u_blocksize(ufh),
		               FLOOR(off + count));

		RETURN_ON_SHORT_READ

		ASSERT(count > MOD(off + count));
		memcpy(ufh->uf_fragments +
		         (ufh->uf_iometa[0] == BLK_FRAG) * u_blocksize(ufh),
		       buf + count - MOD(off + count),
		       MOD(off + count));
	}
	if (ufh->uf_iometa[i] >= 0 && ! CENTRY(i)->uc_valid &&
	    CENTRY(i)->uc_off + u_blocksize(ufh) > off + count) {
		ASSERT(ufh->uf_iovs[i].iov_base == CENTRY(i)->uc_buf);
		pres = u_preadv(ufh, ufh->uf_iovs + i, 1, CENTRY(i)->uc_off);

		RETURN_ON_SHORT_READ
	}
#undef RETURN_ON_SHORT_READ

done_with_tailread:

	if (u_sync_io(ufh)) {
		/*
		 * The synchronous case.
		 * Here we don't use the pre-prepared iovec array for
		 * I/O. We directly write out the whole buffer, with the
		 * necessary padding at the edges, provided by either
		 * ephemeral fragments or cache entries.
		 */

		struct iovec iovs[3];
		int j = 0;

		xpres = 0;

		/* lower edge ... */
		if (MOD(off)) {
			ASSERT(ufh->uf_iometa[0] != BLK_BUF);
			memcpy(iovs, ufh->uf_iovs, sizeof(struct iovec));
			xpres += iovs[j].iov_len;
			j++;

			if (ufh->uf_iometa[0] >= 0) {
				ASSERT(CENTRY(0)->uc_off - off == -MOD(off));
				memcpy(CENTRY(0)->uc_buf + MOD(off),
				       buf,
				       MIN(u_blocksize(ufh) - MOD(off), count));
				low++;
			}
		}

		/* ... the buffer ... */
		iovs[j].iov_base = buf + NEGMOD(off);
		iovs[j].iov_len = count - NEGMOD(off) - MOD(off + count);
		if ((ssize_t)iovs[j].iov_len > 0) {
			xpres += iovs[j].iov_len;
			j++;
		}

		/* ... upper edge */
		if (MOD(off + count) &&
		    (icnt > 1 || ! MOD(off))) {
			ASSERT(ufh->uf_iometa[icnt - 1] != BLK_BUF);
			memcpy(iovs + j, ufh->uf_iovs + icnt - 1,
			       sizeof(struct iovec));
			xpres += iovs[j].iov_len;
			j++;

			if (ufh->uf_iometa[icnt - 1] >= 0) {
				memcpy(CENTRY(icnt - 1)->uc_buf,
				       buf + count - MOD(off + count),
				       MOD(off + count));
				hi--;
			}
		}

		/* sync write! */
		res = u_pwritev(ufh, iovs, j, FLOOR(off));
		if (res < xpres) {
			MARK_SHORT(ufh);
			if (res >= 0)
				res = FLOOR(res);
			return res;
		}
		if (low > 0)
			CENTRY(0)->uc_valid = 1;
		if (hi < icnt)
			CENTRY(icnt - 1)->uc_valid = 1;
	}

	for (i = low; i < hi; i++) {
		xpres = 0;
		if (ufh->uf_iometa[i] < 0 && ! u_sync_io(ufh)) {
			/*
			 * writing out non-cached subranges one by one
			 * in the async case...
			 */
			off_t xoff = 0;
			int i0 = i;

			ASSERT(low == 0 && hi == icnt);

			if (ufh->uf_iometa[i] == BLK_FRAG)
				xoff = FLOOR(off + (!!i) * count);
			else {
				ASSERT(ufh->uf_iometa[i] == BLK_BUF);
				xoff = off +
				       ((void *)ufh->uf_iovs[i].iov_base - buf);
			}

			for (; i < icnt && ufh->uf_iometa[i] < 0; i++)
				xpres += ufh->uf_iovs[i].iov_len;

			/*
			 * write!
			 */
			pres = u_pwritev(ufh, ufh->uf_iovs + i0, i - i0, xoff);
			if (pres == -1) {
				MARK_SHORT(ufh);
				return -1;
			}
			i--;
			res += pres;
		}
		if (ufh->uf_iometa[i] >= 0) {
			/*
			 * copy data from buffer to cache if it's
			 * available
			 */
			size_t fbot = 0, ftop = 0;

			if (! u_sync_io(ufh)) {
				if (pres < xpres) {
					MARK_SHORT(ufh);
					return FLOOR(res);
				}

				/*
				 * in the async case, these cache entries
				 * won't be written out now, so set them dirty
				 */
				adj_dirty(ufh, CENTRY(i), 1);
				res += u_blocksize(ufh);
			}
			if (i == 0)
				fbot = MOD(off);
			if (i == icnt - 1)
				ftop = NEGMOD(off + count);
			memcpy(CENTRY(i)->uc_buf + fbot,
			       buf + (CENTRY(i)->uc_off - off) + fbot,
			       u_blocksize(ufh) - fbot - ftop);
			CENTRY(i)->uc_valid = 1;
		}
	}

	return res;
}
#undef CENTRY

#undef MOD
#undef NEGMOD
#undef FLOOR
#undef CEIL


/*
 * API functions
 */

ssize_t 
ublio_pread(ublio_filehandle_t ufh, void *buf, size_t count, off_t off)
{
	return ublio_pio(ufh, buf, count, off, ublio_block_pread);
}

ssize_t 
ublio_pwrite(ublio_filehandle_t ufh, void *buf, size_t count, off_t off)
{
	return ublio_pio(ufh, buf, count, off, ublio_block_pwrite);
}

int
ublio_fsync(ublio_filehandle_t ufh)
{
	while (! LIST_EMPTY(&ufh->uf_dirty_head)) {
		if (sync_cache(ufh, LIST_FIRST(&ufh->uf_dirty_head)) == -1)
			return -1;
	}

	return 0;
}
