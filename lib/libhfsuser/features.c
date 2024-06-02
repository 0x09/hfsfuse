/*
 * libhfsuser - Userspace support library for NetBSD's libhfs
 * This file is part of the hfsfuse project.
 */

#include "features.h"
#include "hfsuser.h"

#include <stdio.h>

enum hfs_lib_features hfs_get_lib_features(void) {
	return HFS_LIB_FEATURES_NONE
#ifdef HAVE_UBLIO
	     | HFS_LIB_FEATURES_UBLIO
#endif
#ifdef HAVE_UTF8PROC
	     | HFS_LIB_FEATURES_UTF8PROC
#endif
#if HAVE_ZLIB
	     | HFS_LIB_FEATURES_ZLIB
#endif
#if HAVE_LZFSE
	     | HFS_LIB_FEATURES_LZFSE
#endif
	;
}

const char* hfs_lib_ublio_version(void) {
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

const char* hfs_lib_utf8proc_version(void) {
#ifdef HAVE_UTF8PROC
	return utf8proc_version();
#else
	return NULL;
#endif
}

const char* hfs_lib_zlib_version(void) {
#if HAVE_ZLIB
	return ZLIB_VERSION;
#else
	return NULL;
#endif
}
