/*
 * libhfsuser - Userspace support library for NetBSD's libhfs
 * This file is part of the hfsfuse project.
 */

#ifndef HFSUSER_DECMPFS_H
#define HFSUSER_DECMPFS_H

#include "hfsuser.h"

bool hfs_decmpfs_parse_record(struct hfs_decmpfs_header*, uint32_t length, unsigned char* data);
size_t hfs_decmpfs_buffer_size(struct hfs_decmpfs_header* h);

struct hfs_decmpfs_context* hfs_decmpfs_create_context(hfs_volume*, hfs_cnid_t, uint32_t length, unsigned char* data, int* err);
void hfs_decmpfs_destroy_context(struct hfs_decmpfs_context*);

bool hfs_decmpfs_get_header(struct hfs_decmpfs_context*, struct hfs_decmpfs_header*);

int hfs_decmpfs_decompress(uint8_t type, unsigned char* decompressed_buf, size_t decompressed_buf_len, unsigned char* compressed_buf, size_t compressed_buf_len, size_t* bytes_read, void* scratch_buffer);
int hfs_decmpfs_read(hfs_volume* vol, struct hfs_decmpfs_context*, char* buf, size_t size, off_t offset);

#endif
