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

#include "hfsuser.h"

#include <errno.h>
#include <limits.h>
#include <fuse/fuse.h>

static void* hfsfuse_init(struct fuse_conn_info* conn) {
	ringbuffer_init();
	return fuse_get_context()->private_data;
}

static void hfsfuse_destroy(void* vol) {
	ringbuffer_destroy();
	hfslib_close_volume(vol, NULL);
}


struct hf_file {
	hfs_cnid_t cnid;
	hfs_extent_descriptor_t* extents;
	uint16_t nextents;
	uint8_t fork;
};

static int hfsfuse_open(const char* path, struct fuse_file_info* info) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key; unsigned char fork;
	int ret = hfs_lookup(vol,path,&rec,&key,&fork);
	if(ret > 0) return -ENOENT;
	if(ret) return -errno;
	struct hf_file* f = malloc(sizeof(*f));
	f->cnid = rec.file.cnid;
	f->fork = fork;
	f->nextents = hfslib_get_file_extents(vol,f->cnid,fork,&f->extents,NULL);
	info->fh = (uint64_t)f;
	info->keep_cache = 1;
	return 0;
}

static int hfsfuse_release(const char* path, struct fuse_file_info* info) {
	struct hf_file* f = (struct hf_file*)info->fh;
	free(f->extents);
	free(f);
	return 0;
}

static int hfsfuse_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* info) {
	hfs_volume* vol = fuse_get_context()->private_data;
	struct hf_file* f = (struct hf_file*)info->fh;
	uint64_t bytes;
	int ret = hfslib_readd_with_extents(vol,buf,&bytes,size,offset,f->extents,f->nextents,NULL);
	if(ret < 0)
		return ret;
	return bytes;
}

static int hfsfuse_readlink(const char* path, char* buf, size_t size) {
	struct fuse_file_info info;
	if(hfsfuse_open(path,&info)) return -errno;
	int bytes = hfsfuse_read(path,buf,size,0,&info);
	hfsfuse_release(NULL,&info);
	if(bytes < 0) return -errno;
	return 0;
}

static int hfsfuse_getattr(const char* path, struct stat* st) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key; uint8_t fork;
	int ret = hfs_lookup(vol,path,&rec,&key,&fork);
	if(ret < 0) return ret;
	else if(ret > 0) return -ENOENT;
	hfs_stat(vol, &rec,st,fork);
	return 0;
}

static int hfsfuse_fgetattr(const char* path, struct stat* st, struct fuse_file_info* info) {
	hfs_volume* vol = fuse_get_context()->private_data;
	struct hf_file* f = (struct hf_file*)info->fh;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key;
	int ret = hfslib_find_catalog_record_with_cnid(vol,f->cnid,&rec,&key,NULL);
	if(ret < 0) return ret;
	else if(ret > 0) return -ENOENT;
	hfs_stat(vol,&rec,st,f->fork);
	return 0;
}


struct hf_dir {
	hfs_cnid_t cnid;
	hfs_catalog_keyed_record_t* keys;
	hfs_unistr255_t* paths;
	uint32_t npaths;
};

static int hfsfuse_opendir(const char* path, struct fuse_file_info* info) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key;
	int ret = hfs_lookup(vol,path,&rec,&key,NULL);
	if(ret > 0) return -ENOENT;
	if(ret) return -errno;
	struct hf_dir* d = malloc(sizeof(*d));
	d->cnid = rec.folder.cnid;
	hfslib_get_directory_contents(vol,d->cnid,&d->keys,&d->paths,&d->npaths,NULL);

	hfs_catalog_keyed_record_t link;
	for(hfs_catalog_keyed_record_t* record = d->keys; record != d->keys + d->npaths; record++)
		if(record->type == HFS_REC_FILE && (
		  (record->file.user_info.file_creator == HFS_HFSPLUS_CREATOR &&
		   record->file.user_info.file_type    == HFS_HARD_LINK_FILE_TYPE &&
		   !hfslib_get_hardlink(vol, record->file.bsd.special.inode_num, &link, NULL)) ||
		  (record->file.user_info.file_creator == HFS_MACS_CREATOR &&
		   record->file.user_info.file_type    == HFS_DIR_HARD_LINK_FILE_TYPE &&
		   !hfslib_get_directory_hardlink(vol, record->file.bsd.special.inode_num, &link, NULL))))
			*record = link;

	info->fh = (uint64_t)d;
	return 0;
}

static int hfsfuse_releasedir(const char* path, struct fuse_file_info* info) {
	struct hf_dir* d = (struct hf_dir*)info->fh;
	free(d->paths);
	free(d->keys);
	free(d);
	return 0;
}

static int hfsfuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* info) {
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	hfs_volume* vol = fuse_get_context()->private_data;
	struct hf_dir* d = (struct hf_dir*)info->fh;
	char pelem[512];
	int ret = 0;
	for(int i = 0; i < d->npaths; i++) {
		if((ret = hfs_pathname_to_unix(d->paths+i,pelem)) < 0)
			break;
		struct stat st;
		hfs_stat(vol,d->keys+i,&st,0);
		if(filler(buf,pelem,&st,0)) {
			ret = -errno;
			break;
		}
	}
	return min(ret,0);
}
// FUSE expects readder to be implemented in one of two ways
// This is the 'alternative' implementation that supports the offset param
static int hfsfuse_readdir2(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* info) {
	hfs_volume* vol = fuse_get_context()->private_data;
	struct hf_dir* d = (struct hf_dir*)info->fh;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key; struct stat st;
	if(offset < 2) {
		hfslib_find_catalog_record_with_cnid(vol, d->cnid, &rec, &key, NULL);
		if(offset < 1) {
			hfs_stat(vol, &rec, &st, 0);
			if(filler(buf, ".", &st, 1))
				return 0;
		}
		hfslib_find_catalog_record_with_cnid(vol, key.parent_cnid, &rec, &key, NULL);
		hfs_stat(vol, &rec, &st, 0);
		if(filler(buf, "..", NULL, 2))
			return 0;
	}
	char pelem[512];
	int ret = 0;
	for(off_t i = max(0,offset-2); i < d->npaths; i++) {
		if((ret = hfs_pathname_to_unix(d->paths+i,pelem)) < 0)
			break;
		hfs_stat(vol,d->keys+i,&st,0);
		if(filler(buf,pelem,&st,i+3))
			break;
	}
	return min(ret,0);
}


static int hfsfuse_statfs(const char* path, struct statvfs* st) {
	hfs_volume* vol = fuse_get_context()->private_data;
	st->f_bsize = vol->vh.block_size;
	st->f_frsize = st->f_bsize;
	st->f_blocks = vol->vh.total_blocks;
	st->f_bfree = vol->vh.free_blocks;
	st->f_bavail = st->f_bfree;
	st->f_files = vol->vh.file_count + vol->vh.folder_count;
	st->f_ffree = UINT_MAX - st->f_files;
	st->f_favail = st->f_ffree;
	st->f_flag = ST_RDONLY;
	st->f_namemax = 255;
	return 0;
}

static int hfsfuse_getxtimes(const char* path, struct timespec* bkuptime, struct timespec* crtime) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key;
	int ret = hfs_lookup(vol,path,&rec,&key,NULL);
	if(ret < 0) return ret;
	else if(ret > 0) return -ENOENT;
	bkuptime->tv_sec = rec.file.date_backedup;
	bkuptime->tv_nsec = 0;
	crtime->tv_sec = rec.file.date_created;
	crtime->tv_nsec = 0;
	return 0;
}

#ifdef __APPLE__
#define attrname(name) name
#else
#define attrname(name) "user." name
#endif

#define declare_attr(name, buf, bufsize, accum) do {\
	accum += strlen(attrname(name))+1;\
	if(bufsize >= accum) {\
		strcpy(buf,attrname(name));\
		buf += strlen(attrname(name))+1;\
	}\
} while(0)

static int hfsfuse_listxattr(const char* path, char* attr, size_t size) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key;
	int ret = hfs_lookup(vol,path,&rec,&key,NULL);
	if(ret < 0) return ret;
	else if(ret > 0) return -ENOENT;

	declare_attr("hfsfuse.record.date_created", attr, size, ret);
	if(rec.file.date_backedup)
		declare_attr("hfsfuse.record.date_backedup", attr, size, ret);

	if(rec.file.rsrc_fork.logical_size)
		declare_attr("com.apple.ResourceFork", attr, size, ret);
	if(memcmp(&rec.file,(char[32]){0},32))
		declare_attr("com.apple.FinderInfo", attr, size, ret);

	return ret;
}

#define define_attr(attr, name, size, attrsize, block) do {\
	if(!strcmp(attr, attrname(name))) {\
		if(size) {\
			if(size < attrsize) return -ERANGE;\
			else block \
		}\
		return attrsize;\
	}\
} while(0)

static int hfsfuse_getxattr(const char* path, const char* attr, char* value, size_t size) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key;
	int ret = hfs_lookup(vol,path,&rec,&key,NULL);
	if(ret < 0) return ret;
	else if(ret > 0) return -ENOENT;

	define_attr(attr, "com.apple.FinderInfo", size, 32, {
		hfs_serialize_finderinfo(&rec, value);
	});
	ret = rec.file.rsrc_fork.logical_size;
	define_attr(attr, "com.apple.ResourceFork", size, ret, {
		hfs_extent_descriptor_t* extents = NULL;
		uint64_t bytes;
		if(size > ret)
			size = ret;
		uint16_t nextents = hfslib_get_file_extents(vol,rec.file.cnid,HFS_RSRCFORK,&extents,NULL);
		if((ret = hfslib_readd_with_extents(vol,value,&bytes,size,0,extents,nextents,NULL)) >= 0)
			ret = bytes;
		else ret = -EIO;
		free(extents);
	});

	define_attr(attr, "hfsfuse.record.date_created", size, 24, {
		struct tm t;
		localtime_r(&(time_t){HFSTIMETOEPOCH(rec.file.date_created)}, &t);
		strftime(value, 24, "%FT%T%z", &t);
	});

	define_attr(attr, "hfsfuse.record.date_backedup", size, 24, {
		struct tm t;
		localtime_r(&(time_t){HFSTIMETOEPOCH(rec.file.date_backedup)}, &t);
		strftime(value, 24, "%FT%T%z", &t);
	});

	return -1;
}

static int hfsfuse_getxattr_darwin(const char* path, const char* attr, char* value, size_t size, u_int32_t unused) {
	return hfsfuse_getxattr(path, attr, value, size);
}

static struct fuse_operations hfsfuse_ops = {
	.init        = hfsfuse_init,
	.destroy     = hfsfuse_destroy,
	.open        = hfsfuse_open,
	.opendir     = hfsfuse_opendir,
	.read        = hfsfuse_read,
	.readdir     = hfsfuse_readdir2,
	.release     = hfsfuse_release,
	.releasedir  = hfsfuse_releasedir,
	.statfs      = hfsfuse_statfs,
	.getattr     = hfsfuse_getattr,
	.readlink    = hfsfuse_readlink,
	.fgetattr    = hfsfuse_fgetattr,
	.listxattr   = hfsfuse_listxattr,
#ifdef __APPLE__
	.getxattr    = hfsfuse_getxattr_darwin,
#else
	.getxattr    = hfsfuse_getxattr,
#endif
#ifdef __APPLE__
	.getxtimes   = hfsfuse_getxtimes,
#endif
	.flag_nopath = 1,
	.flag_nullpath_ok = 1
};

int main(int argc, char* argv[]) {
	// cheat a lot with option parsing to stay within the fuse_main high level API
	if(argc < 3 || !strcmp(argv[1],"-h")) {
		fuse_main(2,((char*[]){"hfsfuse","-h"}),NULL,NULL);
		return 0;
	}

	const char opts[] = "-oro,allow_other,use_ino,subtype=hfs,fsname=";
	const char* device = argv[argc-2];
	char* mount = argv[argc-1];
	char* argv2[argc+2];
	argv2[0] = "hfsfuse";
	argv2[1] = mount;
	argv2[2] = strcat(strcpy(malloc(strlen(opts)+strlen(device)+1),opts),device);
	argv2[3] = "-s";
	for(int i = 1; i < argc-2; i++)
		argv2[i+3] = argv[i];
	argv2[argc+1] = NULL;

	hfs_callbacks cb = {hfs_vprintf, hfs_malloc, hfs_realloc, hfs_free, hfs_open, hfs_close, hfs_read};
	hfslib_init(&cb);

	// open volume
	hfs_volume vol;
	int ret = hfslib_open_volume(device, 1, &vol, NULL);
	if(ret) {
		perror("Couldn't open volume");
		//goto done;
	}
	hfslib_callbacks()->error = hfs_vsyslog; // prepare to daemonize
	ret = fuse_main(sizeof(argv2)/sizeof(*argv2)-1,argv2,&hfsfuse_ops,&vol);

done:
	hfslib_done();
	return ret;
}
