/*
 * libhfsuser - Userspace support library for NetBSD's libhfs
 * This file is part of the hfsfuse project.
 */

#ifndef HFSUSER_H
#define HFSUSER_H

#include <sys/stat.h>
#include <stdbool.h>

#include "libhfs.h"

#define HFSTIMETOEPOCH(x) (x>2082844800?x-2082844800:0)

// maximum bytes an HFS+ path element can occupy in UTF-8
#define HFS_NAME_MAX 765

extern const char hfs_rcsid_libhfs[], hfs_rcsid_unicode[];

enum hfs_lib_features {
	HFS_LIB_FEATURES_NONE = 0,
	HFS_LIB_FEATURES_UBLIO = 1 << 0,
	HFS_LIB_FEATURES_UTF8PROC = 1 << 1,
	HFS_LIB_FEATURES_ZLIB = 1 << 2,
	HFS_LIB_FEATURES_LZFSE = 1 << 3,
};

enum hfs_lib_features hfs_get_lib_features(void);

const char* hfs_lib_ublio_version(void);
const char* hfs_lib_utf8proc_version(void);
const char* hfs_lib_zlib_version(void);
// lzfse has no embedded version info

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

	int disable_symlinks;
};

// HFS+ compression support
struct hfs_decmpfs_header {
	uint8_t type;
	uint64_t logical_size;
};

struct hfs_decmpfs_context;

void hfs_volume_config_defaults(struct hfs_volume_config*);

ssize_t hfs_unistr_to_utf8(const hfs_unistr255_t* u16, char* u8);
ssize_t hfs_utf8_to_unistr(const char* u8, hfs_unistr255_t* u16);
ssize_t hfs_pathname_to_unix(const hfs_unistr255_t* u16, char* u8);
int hfs_pathname_from_unix(const char* u8, hfs_unistr255_t* u16);

char* hfs_get_path(hfs_volume* vol, hfs_cnid_t cnid);
void hfs_cache_path(hfs_volume*, const char* path, size_t len, hfs_catalog_keyed_record_t*);
int  hfs_lookup(hfs_volume* vol, const char* path, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key, uint8_t* fork);
void hfs_stat(hfs_volume* vol, hfs_catalog_keyed_record_t* key, struct stat* st, uint8_t fork, struct hfs_decmpfs_header*);
void hfs_serialize_finderinfo(hfs_catalog_keyed_record_t*, char[32]);

// libhfs callbacks
int  hfs_open(hfs_volume*,const char*,hfs_callback_args*);
void hfs_close(hfs_volume*,hfs_callback_args*);
int  hfs_read(hfs_volume*,void*,uint64_t,uint64_t,hfs_callback_args*);
void*hfs_malloc(size_t,hfs_callback_args*);
void*hfs_realloc(void*,size_t,hfs_callback_args*);
void hfs_free(void*,hfs_callback_args*);
void hfs_vprintf(const char*,const char*,int,va_list);

bool hfs_decmpfs_parse_record(struct hfs_decmpfs_header*, uint32_t length, unsigned char* data);
bool hfs_decmpfs_type_supported(uint8_t type);
// not required, but useful as a hint for the ideal size to call hfs_decmpfs_read with
size_t hfs_decmpfs_buffer_size(struct hfs_decmpfs_header* h);

struct hfs_decmpfs_context* hfs_decmpfs_create_context(hfs_volume*, hfs_cnid_t, uint32_t length, unsigned char* data, int* err);
void hfs_decmpfs_destroy_context(struct hfs_decmpfs_context*);

bool hfs_decmpfs_get_header(struct hfs_decmpfs_context*, struct hfs_decmpfs_header*);

int hfs_decmpfs_decompress(uint8_t type, unsigned char* decompressed_buf, size_t decompressed_buf_len, unsigned char* compressed_buf, size_t compressed_buf_len, size_t* bytes_read, void* scratch_buffer);
int hfs_decmpfs_read(hfs_volume* vol, struct hfs_decmpfs_context*, char* buf, size_t size, off_t offset);

// convenience wrapper to look up and parse the decmpfs attribute for a file if it exists and is supported. the returned data and length may be passed to hfs_decmpfs_create_context
// returns 0 if this is a compressed file, 1 if not, or negative errno on error
int hfs_decmpfs_lookup(hfs_volume*, hfs_file_record_t*, struct hfs_decmpfs_header*, uint32_t* length, unsigned char** data);

#endif
