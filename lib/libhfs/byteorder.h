/*
 * hfsfuse - FUSE driver for HFS+ filesystems
 * Copyright 2013-2016 0x09.net.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef HFSFUSE_ENDIAN_H
#define HFSFUSE_ENDIAN_H

#include <stdint.h>

#if HAVE_BEXXTOH_ENDIAN_H // linux, haiku, POSIX 202x
#include <endian.h>
#elif HAVE_BEXXTOH_SYS_ENDIAN_H // *BSD
#include <sys/endian.h>
#elif HAVE_OSBYTEORDER_H // macOS
#include <libkern/OSByteOrder.h>
#define be16toh(x) OSSwapBigToHostInt16(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#else
#define big_endian() (!(union { int i; char c; }){1}.c)
#define bytecast(t, ...) ((union { unsigned char b[sizeof(t)]; t val; }){{__VA_ARGS__}}.val)

static inline uint16_t be16toh(uint16_t x) {
	if(big_endian())
		return x;
	return bytecast(uint16_t, x>>8, x);
}

static inline uint32_t be32toh(uint32_t x) {
	if(big_endian())
		return x;
	return bytecast(uint32_t, x>>24, x>>16, x>>8, x);
}

static inline uint64_t be64toh(uint64_t x) {
	if(big_endian())
		return x;
	return bytecast(uint64_t, x>>56, x>>48, x>>40, x>>32, x>>24, x>>16, x>>8, x);
}

#undef big_endian
#undef bytecast

#endif
#endif
