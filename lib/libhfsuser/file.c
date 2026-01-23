/*
 * libhfsuser - Userspace support library for NetBSD's libhfs
 * This file is part of the hfsfuse project.
 */

#include "hfsuser.h"

#include <errno.h>
#include <limits.h>

struct hfs_file {
	hfs_volume* vol;
	hfs_catalog_keyed_record_t rec;
	hfs_extent_descriptor_t* extents;
	uint16_t nextents;
	uint8_t fork;
	uint64_t logical_size;
	struct hfs_decmpfs_context* decmpfs;
};

struct hfs_file* hfs_file_open(hfs_volume* vol, hfs_catalog_keyed_record_t* rec, unsigned char fork, int* out_err) {
	int err = 0;

	if(rec->type != HFS_REC_FILE) {
		err = -EISDIR;
		goto error;
	}

	struct hfs_file* f = malloc(sizeof(*f));
	if(!f) {
		err = -ENOMEM;
		goto error;
	}

	f->vol = vol;
	f->rec = *rec;
	f->fork = fork;
	f->extents = NULL;
	f->nextents = 0;
	f->logical_size = (fork == HFS_RSRCFORK ? rec->file.rsrc_fork : rec->file.data_fork).logical_size;
	f->decmpfs = NULL;

	struct hfs_decmpfs_header h;
	uint32_t inlinelength;
	unsigned char* inlinedata;
	if(fork == HFS_DATAFORK && !hfs_decmpfs_lookup(vol,&rec->file,&h,&inlinelength,&inlinedata)) {
		f->logical_size = h.logical_size;
		f->decmpfs = hfs_decmpfs_create_context(vol,rec->file.cnid,inlinelength,inlinedata,&err);
		free(inlinedata);
		if(!f->decmpfs) {
			free(f);
			f = NULL;
			goto error;
		}
	}
	else f->nextents = hfslib_get_file_extents(vol,rec->file.cnid,fork,&f->extents,NULL);

	if(out_err)
		*out_err = 0;
	return f;

error:
	if(out_err)
		*out_err = err;
	return NULL;
}

struct hfs_file* hfs_file_open_path(hfs_volume* vol, const char* path, int* out_err) {
	struct hfs_file* f = NULL;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key; unsigned char fork;
	int err = hfs_lookup(vol,path,&rec,&key,&fork);
	if(!err)
		f = hfs_file_open(vol,&rec,fork,&err);

	if(out_err)
		*out_err = err;
	return f;
}

void hfs_file_close(struct hfs_file* f) {
	if(!f)
		return;
	hfs_decmpfs_destroy_context(f->decmpfs);
	free(f->extents);
	free(f);
}

ssize_t hfs_file_pread(struct hfs_file* f, void* restrict buf, size_t size, off_t offset) {
	uint64_t bytes;
	if(offset < 0)
		return -EINVAL;
	if((uint64_t)offset >= f->logical_size)
		return 0;
	if(size > f->logical_size - offset)
		size = f->logical_size - offset;
	if(f->decmpfs)
		return hfs_decmpfs_read(f->vol,f->decmpfs,buf,size,offset);
	int ret = hfslib_readd_with_extents(f->vol,buf,&bytes,size,offset,f->extents,f->nextents,NULL);
	if(ret < 0)
		return ret;
	if(bytes > SSIZE_MAX)
		return -EINVAL;
	return bytes;
}

void hfs_file_stat(struct hfs_file* f, struct stat* st) {
	struct hfs_decmpfs_header* hp = NULL, h;
	if(f->decmpfs) {
		hfs_decmpfs_get_header(f->decmpfs,&h);
		hp = &h;
	}
	hfs_stat_with_decmpfs_header(f->vol,&f->rec,st,f->fork,hp);
}

size_t hfs_file_ideal_read_size(struct hfs_file* f, size_t fallback) {
	if(f->decmpfs) {
		struct hfs_decmpfs_header h;
		hfs_decmpfs_get_header(f->decmpfs,&h);
		return hfs_decmpfs_buffer_size(&h);
	}
	size_t blksize = hfs_device_block_size(f->vol);
	return blksize ? blksize : fallback;
}

hfs_catalog_keyed_record_t hfs_file_get_catalog_record(struct hfs_file* f) {
	return f->rec;
}
