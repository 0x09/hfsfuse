/*
 * libhfsuser - Userspace support library for NetBSD's libhfs
 * This file is part of the hfsfuse project.
 */

#ifndef HFSUSER_CACHE_H
#define HFSUSER_CACHE_H

#include "hfsuser.h"

#include <stdbool.h>
#include <stdlib.h>

struct hfs_record_cache;

struct hfs_record_cache* hfs_record_cache_create(size_t length);
void hfs_record_cache_destroy(struct hfs_record_cache*);
bool hfs_record_cache_lookup(struct hfs_record_cache*, const char* path, size_t len, hfs_catalog_keyed_record_t* record);
size_t hfs_record_cache_lookup_parents(struct hfs_record_cache*, char* path, size_t len, hfs_catalog_keyed_record_t* record);
void hfs_record_cache_add(struct hfs_record_cache*, const char* path, size_t len, hfs_catalog_keyed_record_t* record);

#endif
