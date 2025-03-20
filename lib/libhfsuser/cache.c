/*
 * libhfsuser - Userspace support library for NetBSD's libhfs
 * This file is part of the hfsfuse project.
 */

#include "cache.h"

#include <stdlib.h>
#include <pthread.h>

struct ringnode {
	struct ringnode* next,* prev;
	char* path;
	size_t len;
	hfs_catalog_keyed_record_t record;
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

bool hfs_record_cache_lookup(struct hfs_record_cache* buf, const char* path, size_t len, hfs_catalog_keyed_record_t* record) {
	bool ret = false;
	if(!buf || pthread_rwlock_rdlock(&buf->lock))
		return ret;
	struct ringnode* it = buf->head;
	do {
		if(!it->path) break;
		if(len == it->len && !memcmp(it->path,path,len)) {
			*record = it->record;
			ret = true;
			break;
		}
		it = it->next;
	} while(it != buf->head);
	pthread_rwlock_unlock(&buf->lock);
	return ret;
}

size_t hfs_record_cache_lookup_parents(struct hfs_record_cache* buf, char* path, size_t len, hfs_catalog_keyed_record_t* record) {
	len = 0;
	char* c;
	while((c = strrchr(path, '/'))) {
		*c = '\0';
		len = c - path;
		if(*path && hfs_record_cache_lookup(buf, path, len, record))
			break;
	}
	return len;
}

void hfs_record_cache_add(struct hfs_record_cache* buf, const char* path, size_t len, hfs_catalog_keyed_record_t* record) {
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
	tail->record = *record;
	buf->head = tail;
end:
	pthread_rwlock_unlock(&buf->lock);
}
