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

#ifndef HFSUSER_CACHE_H
#define HFSUSER_CACHE_H

#include "hfsuser.h"

#include <stdbool.h>
#include <stdlib.h>

struct hfs_record_cache;

struct hfs_record_cache* hfs_record_cache_create(size_t length);
void hfs_record_cache_destroy(struct hfs_record_cache*);
bool hfs_record_cache_lookup(struct hfs_record_cache*, const char* path, size_t len, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key);
size_t hfs_record_cache_lookup_parents(struct hfs_record_cache*, char* path, size_t len, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key);
void hfs_record_cache_add(struct hfs_record_cache*, const char* path, size_t len, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key);

#endif
