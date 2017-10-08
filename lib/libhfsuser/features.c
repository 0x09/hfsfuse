/*
 * libhfsuser - Userspace support library for NetBSD's libhfs
 * Copyright 2013-2017 0x09.net.
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

#include "libhfsuser/features.h"
#include "libhfsuser/hfsuser.h"

#include <stdio.h>

enum hfs_lib_features hfs_get_lib_features() {
	return HFS_LIB_FEATURES_NONE
#ifdef HAVE_UBLIO
	     | HFS_LIB_FEATURES_UBLIO
#endif
#ifdef HAVE_UTF8PROC
	     | HFS_LIB_FEATURES_UTF8PROC
#endif
	;
}

const char* hfs_lib_ublio_version() {
#ifdef HAVE_UBLIO
	static char ublio_version[14];
	if(!ublio_version[0])
		//undo the UBLIO_API_VERSION macro
		snprintf(ublio_version, sizeof(ublio_version)-1, "%u.%u", (UBLIO_CURRENT_API) / 100, (UBLIO_CURRENT_API) % 100);
	return ublio_version;
#else
	return NULL;
#endif
}

const char* hfs_lib_utf8proc_version() {
#ifdef HAVE_UTF8PROC
	return utf8proc_version();
#else
	return NULL;
#endif
}
