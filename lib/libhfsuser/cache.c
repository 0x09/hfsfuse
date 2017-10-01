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

#include "cache.h"

#include <stdlib.h>
#include <pthread.h>

struct ringnode {
	struct ringnode* next,* prev;
	char* path;
	hfs_catalog_keyed_record_t record;
	hfs_catalog_key_t key;
};

struct hfs_record_cache {
	struct ringnode* head;
	pthread_rwlock_t lock;
};

struct hfs_record_cache* hfs_record_cache_create(size_t length) {
	if(!length)
		return NULL;
	struct hfs_record_cache* cache = malloc(sizeof(*cache));
	pthread_rwlock_init(&cache->lock,NULL);
	pthread_rwlock_wrlock(&cache->lock);
	struct ringnode* tail = cache->head = malloc(sizeof(*cache->head));
	cache->head->path = NULL;
	for(int i = 0; i < length; i++) {
		tail->next = malloc(sizeof(*tail));
		tail->next->prev = tail;
		tail = tail->next;
		tail->path = NULL;
	}
	tail->next = cache->head;
	cache->head->prev = tail;
	pthread_rwlock_unlock(&cache->lock);
	return cache;
}

void hfs_record_cache_destroy(struct hfs_record_cache* cache) {
	if(!cache)
		return;
	pthread_rwlock_wrlock(&cache->lock);
	struct ringnode* head = cache->head;
	do {
		free(head->path);
		struct ringnode* tmp = head->next;
		free(head);
		head = tmp;
	} while(head != cache->head);
	pthread_rwlock_unlock(&cache->lock);
	pthread_rwlock_destroy(&cache->lock);
	free(cache);
}

bool hfs_record_cache_lookup(struct hfs_record_cache* cache, const char* path, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key) {
	if(!cache)
		return false;
	bool ret = false;
	pthread_rwlock_rdlock(&cache->lock);
	struct ringnode* it = cache->head;
	do {
		if(!it->path) break;
		if(!strcmp(it->path,path)) {
			*record = it->record;
			*key = it->key;
			ret = true;
			break;
		}
		it = it->next;
	} while(it != cache->head);
	pthread_rwlock_unlock(&cache->lock);
	return ret;
}

void hfs_record_cache_add(struct hfs_record_cache* cache, const char* path, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key) {
	if(!cache)
		return;
	pthread_rwlock_wrlock(&cache->lock);
	struct ringnode* tail = cache->head->prev;
	tail->path = realloc(tail->path,strlen(path)+1);
	strcpy(tail->path,path);
	tail->key = *key;
	tail->record = *record;
	cache->head = tail;
	pthread_rwlock_unlock(&cache->lock);
}
