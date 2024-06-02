/*
 * System-independent big endian to host conversion.
 * This file is part of the hfsfuse project.
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
#undef be16toh
#undef be32toh
#undef be64toh

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
