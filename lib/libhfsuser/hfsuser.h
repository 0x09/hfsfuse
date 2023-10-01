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

#ifndef HFSUSER_H
#define HFSUSER_H

#include <sys/stat.h>

#include "libhfs.h"

#define HFSTIMETOEPOCH(x) (x>2082844800?x-2082844800:0)

// maximum bytes an HFS+ path element can occupy in UTF-8
#define HFS_NAME_MAX 765

enum hfs_lib_features {
	HFS_LIB_FEATURES_NONE = 0,
	HFS_LIB_FEATURES_UBLIO = 1 << 0,
	HFS_LIB_FEATURES_UTF8PROC = 1 << 1
};

enum hfs_lib_features hfs_get_lib_features(void);

const char* hfs_lib_ublio_version(void);
const char* hfs_lib_utf8proc_version(void);

struct hfs_volume_config {
	size_t cache_size;
	uint32_t blksize;
	char* rsrc_suff;
	int rsrc_only;
	// Unused if not built with ublio
	int noublio;
	int32_t ublio_items;
	uint64_t ublio_grace;

	uint16_t default_file_mode, default_dir_mode;
	uint32_t default_uid, default_gid;
};

void hfs_volume_config_defaults(struct hfs_volume_config*);

ssize_t hfs_unistr_to_utf8(const hfs_unistr255_t* u16, char* u8);
ssize_t hfs_pathname_to_unix(const hfs_unistr255_t* u16, char* u8);
int hfs_pathname_from_unix(const char* u8, hfs_unistr255_t* u16);

char* hfs_get_path(hfs_volume* vol, hfs_cnid_t cnid);
int  hfs_lookup(hfs_volume* vol, const char* path, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key, uint8_t* fork);
void hfs_stat(hfs_volume* vol, hfs_catalog_keyed_record_t* key, struct stat* st, uint8_t fork);
void hfs_serialize_finderinfo(hfs_catalog_keyed_record_t*, char[32]);

// libhfs callbacks
int  hfs_open(hfs_volume*,const char*,hfs_callback_args*);
void hfs_close(hfs_volume*,hfs_callback_args*);
int  hfs_read(hfs_volume*,void*,uint64_t,uint64_t,hfs_callback_args*);
void*hfs_malloc(size_t,hfs_callback_args*);
void*hfs_realloc(void*,size_t,hfs_callback_args*);
void hfs_free(void*,hfs_callback_args*);
void hfs_vprintf(const char*,const char*,int,va_list);

#endif
