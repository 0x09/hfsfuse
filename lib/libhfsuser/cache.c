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

#include "libhfsuser/cache.h"

#include <stdlib.h>
#include <pthread.h>

struct ringnode {
	struct ringnode* next,* prev;
	char* path;
	size_t len;
	hfs_catalog_keyed_record_t record;
	hfs_catalog_key_t key;
};

struct hfs_record_cache {
	pthread_rwlock_t lock;
	struct ringnode* head;
	struct ringnode backing[];
};

struct hfs_record_cache* hfs_record_cache_create(size_t length) {
	if(!length)
		return NULL;
	struct hfs_record_cache* buf = malloc(sizeof(*buf) + sizeof(*buf->backing)*length);
	if(!buf || pthread_rwlock_init(&buf->lock,NULL)) {
		free(buf);
		return NULL;
	}

	struct ringnode* tail = buf->head = buf->backing;
	for(size_t i = 0; i <= length; i++) {
		tail->next = buf->backing + i%length;
		tail->next->prev = tail;
		tail = tail->next;
		tail->path = NULL;
	}
	return buf;
}

void hfs_record_cache_destroy(struct hfs_record_cache* buf) {
	if(!buf)
		return;
    pthread_rwlock_wrlock(&buf->lock);
	struct ringnode* tail = buf->head;
	do {
		free(tail->path);
		tail = tail->next;
	} while(tail != buf->head);
	pthread_rwlock_unlock(&buf->lock);
	pthread_rwlock_destroy(&buf->lock);
	free(buf);
}

bool hfs_record_cache_lookup(struct hfs_record_cache* buf, const char* path, size_t len, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key) {
	bool ret = false;
	if(!buf || pthread_rwlock_rdlock(&buf->lock))
		return ret;
	struct ringnode* it = buf->head;
	do {
		if(!it->path) break;
		if(len == it->len && !memcmp(it->path,path,len)) {
			*record = it->record;
			*key = it->key;
			ret = true;
			break;
		}
		it = it->next;
	} while(it != buf->head);
	pthread_rwlock_unlock(&buf->lock);
	return ret;
}

size_t hfs_record_cache_lookup_parents(struct hfs_record_cache* buf, char* path, size_t len, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key) {
	char* c;
	while((c = strrchr(path, '/'))) {
		*c = '\0';
		len = c - path;
		if(*path && hfs_record_cache_lookup(buf, path, len, record, key))
			break;
	}
	return len;
}

void hfs_record_cache_add(struct hfs_record_cache* buf, const char* path, size_t len, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key) {
	if(!buf || pthread_rwlock_wrlock(&buf->lock))
		return;
	struct ringnode* tail = buf->head->prev;
	char* newpath = realloc(tail->path,len);
	if(!newpath) {
		do {
			free(tail->path);
			tail->path = NULL;
			tail = tail->next;
		} while(tail != buf->head->prev);
		goto end;
	}
	memcpy(newpath, path,len);
	tail->path = newpath;
	tail->len = len;
	tail->key = *key;
	tail->record = *record;
	buf->head = tail;
end:
	pthread_rwlock_unlock(&buf->lock);
}
