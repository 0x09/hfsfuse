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

#ifndef HFSLIB_H
#define HFSLIB_H

#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#include "libhfs.h"
#include "unicode.h"

#define HFSTIMETOEPOCH(x) (x>2082844800?x-2082844800:0)

void ringbuffer_init();
void ringbuffer_destroy();

ssize_t hfs_pathname_to_unix(const hfs_unistr255_t* u16, char u8[]);
ssize_t hfs_pathname_from_unix(const char* u8, hfs_unistr255_t* u16);

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
void hfs_vsyslog(const char*,const char*,int,va_list);

#endif
