/*
 * libhfsuser - Userspace support library for NetBSD's libhfs
 * This file is part of the hfsfuse project.
 */

#ifndef HFSUSER_FEATURES_H
#define HFSUSER_FEATURES_H

#ifdef HAVE_UTF8PROC
#include "utf8proc.h"
#endif

#ifdef HAVE_UBLIO
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>
#include "ublio.h"
#endif

#if HAVE_ZLIB
#include <zlib.h>
#endif

#if HAVE_LZFSE
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#endif
#include <lzfse.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

#endif
