/*
 * hfsfuse - FUSE driver for HFS+ filesystems
 * This file is part of the hfsfuse project.
 */

#include "hfsuser.h"

#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

#if FUSE_USE_VERSION < 30
#include <fuse.h>
#else
#include <fuse3/fuse.h>
#endif

#include <syslog.h>

#ifndef HFSFUSE_VERSION_STRING
#include "version.h"
#endif

#ifndef ENOATTR
#ifdef ENODATA
#define ENOATTR ENODATA
#else
#define ENOATTR 1
#endif
#endif

#if FUSE_VERSION >= 30
static void* hfsfuse_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
	cfg->use_ino = 1;
	cfg->nullpath_ok = 1;
	return fuse_get_context()->private_data; // the hfs_volume
}
#endif

static void hfsfuse_destroy(void* vol) {
	hfslib_close_volume(vol, NULL);
}

static int hfsfuse_open(const char* path, struct fuse_file_info* info) {
	hfs_volume* vol = fuse_get_context()->private_data;
	int ret;
	struct hfs_file* f = hfs_file_open_path(vol,path,&ret);
	if(!f)
		return ret;

	info->fh = (uint64_t)f;
	info->keep_cache = 1;
	return 0;
}

static int hfsfuse_release(const char* path, struct fuse_file_info* info) {
	hfs_file_close((struct hfs_file*)info->fh);
	return 0;
}

static int hfsfuse_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* info) {
	struct hfs_file* f = (struct hfs_file*)info->fh;
	ssize_t bytes = hfs_file_pread(f,buf,size,offset);
	if(bytes > INT_MAX)
		return -EINVAL;
	return bytes;
}

static int hfsfuse_readlink(const char* path, char* buf, size_t size) {
	if(!size)
		return -EINVAL;

	int ret;
	hfs_volume* vol = fuse_get_context()->private_data;
	struct hfs_file* f = hfs_file_open_path(vol,path,&ret);
	if(!f)
		return ret;

	ssize_t bytes = hfs_file_pread(f,buf,size-1,0);
	hfs_file_close(f);

	if(bytes < 0)
		return bytes;
	buf[bytes] = '\0';
	return 0;
}

#if FUSE_DARWIN_ENABLE_EXTENSIONS
#define stat_to_fuse_darwin_attr(rec,st) ((struct fuse_darwin_attr){\
	.ino = (st).st_ino,\
	.mode = (st).st_mode,\
	.nlink = (st).st_nlink,\
	.uid = (st).st_uid,\
	.gid = (st).st_gid,\
	.rdev = (st).st_rdev,\
	.atimespec.tv_sec = (st).st_atime,\
	.mtimespec.tv_sec = (st).st_mtime,\
	.ctimespec.tv_sec = (st).st_ctime,\
	.btimespec.tv_sec = (st).st_birthtime,\
	.bkuptimespec.tv_sec = HFSTIMETOEPOCH((rec).file.date_backedup),\
	.size = (st).st_size,\
	.blocks = (st).st_blocks,\
	.blksize = (st).st_blksize,\
	.flags = (st).st_flags,\
})
#endif

#if FUSE_DARWIN_ENABLE_EXTENSIONS
static int hfsfuse_fgetattr(const char* path, struct fuse_darwin_attr* darwin_attrs, struct fuse_file_info* info) {
#else
static int hfsfuse_fgetattr(const char* path, struct stat* st, struct fuse_file_info* info) {
#endif
	struct hfs_file* f = (struct hfs_file*)info->fh;

#if FUSE_DARWIN_ENABLE_EXTENSIONS
	struct stat st;
	hfs_catalog_keyed_record_t rec = hfs_file_get_catalog_record(f);
	hfs_file_stat(f,&st);
	*darwin_attrs = stat_to_fuse_darwin_attr(rec,st);
#else
	hfs_file_stat(f,st);
#endif

	return 0;
}


#if FUSE_DARWIN_ENABLE_EXTENSIONS
static int hfsfuse_getattr(const char* path, struct fuse_darwin_attr* st, struct fuse_file_info *fi) {
#elif FUSE_VERSION >= 30
static int hfsfuse_getattr(const char* path, struct stat* st, struct fuse_file_info *fi) {
#else
static int hfsfuse_getattr(const char* path, struct stat* st) {
#endif

#if FUSE_DARWIN_ENABLE_EXTENSIONS || FUSE_VERSION >= 30
	if(fi)
		return hfsfuse_fgetattr(path,st,fi);
#endif

	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key; uint8_t fork;
	int ret = hfs_lookup(vol,path,&rec,&key,&fork);
	if(ret)
		return ret;

#if FUSE_DARWIN_ENABLE_EXTENSIONS
	struct stat statbuf;
	hfs_stat(vol,&rec,&statbuf,fork);
	*st = stat_to_fuse_darwin_attr(rec,statbuf);
#else
	hfs_stat(vol,&rec,st,fork);
#endif

	return 0;
}

#if HAVE_STATX && FUSE_VERSION >= 318
static int hfsfuse_statx(const char* path, int flags, int mask, struct statx* stx, struct fuse_file_info* info) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec;
	struct stat st;
	if(info) {
		struct hfs_file* f = (struct hfs_file*)info->fh;
		hfs_file_stat(f,&st);
		rec = hfs_file_get_catalog_record(f);
	}
	else {
		hfs_catalog_key_t key; uint8_t fork;
		int ret = hfs_lookup(vol,path,&rec,&key,&fork);
		if(ret)
			return ret;
		hfs_stat(vol,&rec,&st,fork,hp);
	}

	stx->stx_mask = STATX_BASIC_STATS | STATX_BTIME;

	stx->stx_blksize = st.st_blksize;
	stx->stx_nlink = st.st_nlink;
	stx->stx_uid = st.st_uid;
	stx->stx_gid = st.st_gid;
	stx->stx_mode = st.st_mode;
	stx->stx_ino = st.st_ino;
	stx->stx_size = st.st_size;
	stx->stx_blocks = st.st_blocks;
	stx->stx_atime.tv_sec = HFSTIMETOEPOCH(rec.file.date_accessed);
	stx->stx_btime.tv_sec = HFSTIMETOEPOCH(rec.file.date_created);
	stx->stx_ctime.tv_sec = HFSTIMETOEPOCH(rec.file.date_attrib_mod);
	stx->stx_mtime.tv_sec = HFSTIMETOEPOCH(rec.file.date_content_mod);

	__u16 mode_mask = 0;
	if(mask & STATX_TYPE)
		mode_mask |= S_IFMT;
	if(mask & STATX_MODE)
		mode_mask |= ~S_IFMT;
	stx->stx_mode &= mode_mask;

	if(hp)
		stx->stx_attributes = stx->stx_attributes_mask = STATX_ATTR_COMPRESSED;

	return 0;
}
#endif

struct hf_dir {
	hfs_catalog_keyed_record_t dir_record;
	hfs_cnid_t parent_cnid;
	hfs_catalog_keyed_record_t* records;
	hfs_unistr255_t* names;
	uint32_t nentries;
	char* path;
	size_t pathlen;
};

static int hfsfuse_opendir(const char* path, struct fuse_file_info* info) {
	hfs_volume* vol = fuse_get_context()->private_data;
	struct hf_dir* d = malloc(sizeof(*d));
	if(!d)
		return -ENOMEM;

	hfs_catalog_key_t key;
	int ret = hfs_lookup(vol,path,&d->dir_record,&key,NULL);
	if(ret)
		goto end;
	d->parent_cnid = key.parent_cnid;

	d->pathlen = strlen(path);
	if(d->pathlen > 1)
		d->pathlen++;
	if(!(d->path = malloc(d->pathlen))) {
		ret = -ENOMEM;
		goto end;
	}
	if(d->pathlen > 1)
		memcpy(d->path,path,d->pathlen-1);
	d->path[d->pathlen-1] = '/';

	if(hfslib_get_directory_contents(vol,d->dir_record.folder.cnid,&d->records,&d->names,&d->nentries,NULL)) {
		ret = -1;
		goto end;
	}
	hfs_catalog_keyed_record_t link;
	for(hfs_catalog_keyed_record_t* record = d->records; record != d->records + d->nentries; record++)
		if(record->type == HFS_REC_FILE && (
		  (record->file.user_info.file_creator == HFS_HFSPLUS_CREATOR &&
		   record->file.user_info.file_type    == HFS_HARD_LINK_FILE_TYPE &&
		   !hfslib_get_hardlink(vol, record->file.bsd.special.inode_num, &link, NULL)) ||
		  (record->file.user_info.file_creator == HFS_MACS_CREATOR &&
		   record->file.user_info.file_type    == HFS_DIR_HARD_LINK_FILE_TYPE &&
		   !hfslib_get_directory_hardlink(vol, record->file.bsd.special.inode_num, &link, NULL))))
			*record = link;

	info->fh = (uint64_t)d;

end:
	if(ret)
		free(d);
	return ret;
}

static int hfsfuse_releasedir(const char* path, struct fuse_file_info* info) {
	struct hf_dir* d = (struct hf_dir*)info->fh;
	free(d->names);
	free(d->records);
	free(d->path);
	free(d);
	return 0;
}

#if FUSE_DARWIN_ENABLE_EXTENSIONS
static int hfsfuse_readdir(const char* path, void* buf, fuse_darwin_fill_dir_t filler, off_t offset, struct fuse_file_info* info, enum fuse_readdir_flags flags) {
#elif FUSE_VERSION >= 30
static int hfsfuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* info, enum fuse_readdir_flags flags) {
#else
static int hfsfuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* info) {
#endif
	hfs_volume* vol = fuse_get_context()->private_data;
	struct hf_dir* d = (struct hf_dir*)info->fh;
	if(offset < 1) {
		struct stat st = {0};
		hfs_stat(vol, &d->dir_record, &st, 0);
		int ret;
#if FUSE_DARWIN_ENABLE_EXTENSIONS
		ret = filler(buf, ".", &stat_to_fuse_darwin_attr(d->dir_record,st), 1, FUSE_FILL_DIR_PLUS);
#elif FUSE_VERSION >= 30
		ret = filler(buf, ".", &st, 1, FUSE_FILL_DIR_PLUS);
#else
		ret = filler(buf, ".", &st, 1);
#endif
		if(ret)
			return 0;
	}
	if(offset < 2) {
		struct stat st = {0};
		struct stat* stp = NULL;
		hfs_catalog_keyed_record_t rec = {0};
		if(d->dir_record.folder.cnid != HFS_CNID_ROOT_FOLDER) {
			hfs_catalog_key_t key;
			hfslib_find_catalog_record_with_cnid(vol, d->parent_cnid, &rec, &key, NULL);
			stp = &st;
			hfs_stat(vol, &rec, stp, 0);
		}
		int ret;
#if FUSE_DARWIN_ENABLE_EXTENSIONS
		ret = filler(buf, "..", stp ? &stat_to_fuse_darwin_attr(rec,*stp) : NULL, 2, stp ? FUSE_FILL_DIR_PLUS : 0);
#elif FUSE_VERSION >= 30
		ret = filler(buf, "..", stp, 2, stp ? FUSE_FILL_DIR_PLUS : 0);
#else
		ret = filler(buf, "..", stp, 2);
#endif
		if(ret)
			return 0;
	}

	char* fullpath = malloc(d->pathlen+HFS_NAME_MAX+1);
	if(!fullpath)
		return -ENOMEM;

	memcpy(fullpath,d->path,d->pathlen);
	char* pelem = fullpath + d->pathlen;
	int ret = 0;
	for(off_t i = max(0,offset-2); i < d->nentries; i++) {
		ssize_t len;
		if((len = hfs_pathname_to_unix(d->names+i,pelem)) < 0) {
			ret = len;
			continue;
		}
		hfs_cache_path(vol,fullpath,d->pathlen+len,d->records+i);

		struct stat st = {0};
		hfs_stat(vol,d->records+i,&st,0);
		int ret;
#if FUSE_DARWIN_ENABLE_EXTENSIONS
		ret = filler(buf,pelem,&stat_to_fuse_darwin_attr(d->records[i],st),i+3,FUSE_FILL_DIR_PLUS);
#elif FUSE_VERSION >= 30
		ret = filler(buf,pelem,&st,i+3,FUSE_FILL_DIR_PLUS);
#else
		ret = filler(buf,pelem,&st,i+3);
#endif
		if(ret)
			break;
	}
	free(fullpath);
	return min(ret,0);
}

#if FUSE_DARWIN_ENABLE_EXTENSIONS
static int hfsfuse_statfs(const char* path, struct statfs* st) {
	hfs_volume* vol = fuse_get_context()->private_data;
	st->f_bsize = vol->vh.block_size;
	st->f_blocks = vol->vh.total_blocks;
	st->f_bfree = vol->vh.free_blocks;
	st->f_bavail = st->f_bfree;
	st->f_files = UINT32_MAX - HFS_CNID_USER;
	st->f_ffree = st->f_files - vol->vh.file_count - vol->vh.folder_count;
	return 0;
}
#else
static int hfsfuse_statfs(const char* path, struct statvfs* st) {
	hfs_volume* vol = fuse_get_context()->private_data;
	st->f_bsize = vol->vh.block_size;
	st->f_frsize = st->f_bsize;
	st->f_blocks = vol->vh.total_blocks;
	st->f_bfree = vol->vh.free_blocks;
	st->f_bavail = st->f_bfree;
	st->f_files = UINT32_MAX - HFS_CNID_USER;
	st->f_ffree = st->f_files - vol->vh.file_count - vol->vh.folder_count;
	st->f_favail = st->f_ffree;
	st->f_flag = ST_RDONLY;
	st->f_namemax = HFS_NAME_MAX;
	return 0;
}
#endif

#if defined(__APPLE__) && FUSE_VERSION < 30
static int hfsfuse_getxtimes(const char* path, struct timespec* bkuptime, struct timespec* crtime) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key;
	int ret = hfs_lookup(vol,path,&rec,&key,NULL);
	if(ret)
		return ret;

	bkuptime->tv_sec = rec.file.date_backedup;
	bkuptime->tv_nsec = 0;
	crtime->tv_sec = rec.file.date_created;
	crtime->tv_nsec = 0;
	return 0;
}
#endif

#define STR_(x) #x
#define STR(x) STR_(x)
#define XATTR_NAMESPACE_STR STR(XATTR_NAMESPACE)
#define attrname(name) XATTR_NAMESPACE_STR name

#define declare_attr(name, buf, bufsize, accum) do {\
	accum += strlen(attrname(name))+1;\
	if(bufsize >= (size_t)accum) {\
		strcpy(buf,attrname(name));\
		buf += strlen(attrname(name))+1;\
	}\
} while(0)

static int hfsfuse_listxattr(const char* path, char* attr, size_t size) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key;
	int ret = hfs_lookup(vol,path,&rec,&key,NULL);
	if(ret)
		return ret;

#ifdef __linux__
	if(!strcmp("user.",XATTR_NAMESPACE_STR)) {
		// only regular files can contain user namespace xattrs on Linux
		struct stat st;
		hfs_stat(vol,&rec,&st,HFS_DATAFORK);
		if(!(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)))
			return 0;
	}
#endif

	declare_attr("hfsfuse.record.date_created", attr, size, ret);
	if(rec.file.date_backedup)
		declare_attr("hfsfuse.record.date_backedup", attr, size, ret);

	if(rec.type == HFS_REC_FILE && rec.file.rsrc_fork.logical_size && rec.file.rsrc_fork.logical_size <= INT_MAX)
		declare_attr("com.apple.ResourceFork", attr, size, ret);

	char finderinfo[32];
	hfs_serialize_finderinfo(&rec,finderinfo);
	if(memcmp(finderinfo,(char[32]){0},32))
		declare_attr("com.apple.FinderInfo", attr, size, ret);

	hfs_attribute_key_t* attr_keys;
	uint32_t nattrs;
	if(hfslib_find_attribute_records_for_cnid(vol,rec.file.cnid,&attr_keys,&nattrs,NULL))
		return -1;
	for(uint32_t i = 0; i < nattrs; i++) {
		char attrname[HFS_NAME_MAX+1];
		ssize_t u8len = hfs_unistr_to_utf8(&attr_keys[i].name, attrname);
		if(u8len <= 0)
			continue;
		ret += u8len + strlen(XATTR_NAMESPACE_STR) + 1;
		if(size >= (size_t)ret) {
			attr = stpcpy(attr, XATTR_NAMESPACE_STR);
			attr = stpcpy(attr, attrname)+1;
		}
	}
	free(attr_keys);

	return ret;
}

#define define_attr(attr, name, size, attrsize, block) do {\
	if(!strcmp(attr, attrname(name))) {\
		if(size) {\
			if(size < (size_t)attrsize) return -ERANGE;\
			else block \
		}\
		return attrsize;\
	}\
} while(0)

// apple supports an offset argument to getxattr, but this is only used for resource fork attributes
static int hfsfuse_getxattr_offset(const char* path, const char* attr, char* value, size_t size, uint32_t offset) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key;
	int ret = hfs_lookup(vol,path,&rec,&key,NULL);
	if(ret)
		return ret;

	define_attr(attr, "com.apple.FinderInfo", size, 32, {
		hfs_serialize_finderinfo(&rec, value);
	});
	if(rec.type == HFS_REC_FILE && rec.file.rsrc_fork.logical_size && rec.file.rsrc_fork.logical_size <= INT_MAX) {
		ret = rec.file.rsrc_fork.logical_size;
		define_attr(attr, "com.apple.ResourceFork", size, ret, {
			hfs_extent_descriptor_t* extents = NULL;
			uint64_t bytes;
			if(offset >= (uint32_t)ret)
				return 0;
			if(size > ret - offset)
				size = ret - offset;
			uint16_t nextents = hfslib_get_file_extents(vol,rec.file.cnid,HFS_RSRCFORK,&extents,NULL);
			if((ret = hfslib_readd_with_extents(vol,value,&bytes,size,offset,extents,nextents,NULL)) >= 0)
				ret = bytes;
			else ret = -EIO;
			free(extents);
		});
	}

	define_attr(attr, "hfsfuse.record.date_created", size, 24, {
		// some strftime implementations require room for the null terminator
		// but we don't want this in the returned attribute value
		char timebuf[25];
		struct tm t;
		localtime_r(&(time_t){HFSTIMETOEPOCH(rec.file.date_created)}, &t);
		strftime(timebuf, 25, "%FT%T%z", &t);
		memcpy(value, timebuf, 24);
	});

	define_attr(attr, "hfsfuse.record.date_backedup", size, 24, {
		char timebuf[25];
		struct tm t;
		localtime_r(&(time_t){HFSTIMETOEPOCH(rec.file.date_backedup)}, &t);
		strftime(timebuf, 25, "%FT%T%z", &t);
		memcpy(value, timebuf, 24);
	});

	if(!strncmp(attr,XATTR_NAMESPACE_STR,strlen(XATTR_NAMESPACE_STR)))
		attr += strlen(XATTR_NAMESPACE_STR);

	hfs_attribute_record_t attrec;
	hfs_unistr255_t attrname;
	// xattr names have no normalization applied unlike catalog keys
	if(hfs_utf8_to_unistr(attr,&attrname) <= 0)
		return -EINVAL;
	hfs_attribute_key_t attrkey;
	if(!hfslib_make_attribute_key(rec.file.cnid,0,attrname.length,attrname.unicode,&attrkey))
		return -EFAULT; // cnid was 0
	void* inlinedata = NULL;
	if(hfslib_find_attribute_record_with_key(vol,&attrkey,&attrec,(size ? &inlinedata : NULL),NULL))
		 return -ENOATTR;

	size_t attrsize = 0;
	switch(attrec.type) {
		case HFS_ATTR_INLINE_DATA:
			attrsize = attrec.inline_record.length;
			break;
		case HFS_ATTR_FORK_DATA:
			attrsize = attrec.fork_record.fork.logical_size;
			break;
		case HFS_ATTR_EXTENTS:
			hfslib_error("unexpected extent attr found in getxattr. attr: %s path: %s\n", NULL, 0, attr, path);
			return -EFAULT;
	}
	if(size) {
		if(size < attrsize) {
			ret = -ERANGE;
			goto end;
		}

		switch(attrec.type) {
			case HFS_ATTR_INLINE_DATA:
				memcpy(value,inlinedata,attrsize);
				break;
			case HFS_ATTR_FORK_DATA: {
				hfs_extent_descriptor_t* extents;
				uint16_t nextents;
				if(hfslib_get_attribute_extents(vol,&attrkey,&attrec,&nextents,&extents,NULL))
					return -1;
				uint64_t bytesread;
				ret = hfslib_readd_with_extents(vol,value,&bytesread,attrsize,0,extents,nextents,NULL);
				free(extents);
				if(ret)
					return ret;
				attrsize = bytesread;
			}; break;
		}
	}

	if(attrsize > (size_t)INT_MAX)
		ret = -ERANGE;
	else
		ret = attrsize;

end:
	free(inlinedata);
	return ret;
}

#if !(FUSE_DARWIN_ENABLE_EXTENSIONS || (defined(__APPLE__) && FUSE_VERSION < 30))
#ifdef __HAIKU__
static inline char hex4b(uint8_t dec) {
	return dec < 10 ? '0' + dec : 'a' + (dec-10);
}

// Haiku's userlandfs FUSE implementation copies attr values using strlcpy, so any binary attr values that contain NULs will be truncated
// to support this we just encode all xattr values as hex strings that can be decoded with e.g. xxd -t -p
static int hfsfuse_getxattr(const char* path, const char* attr, char* value, size_t size) {
	int attrsize = hfsfuse_getxattr_offset(path, attr, value, size, 0);
	if(attrsize > INT_MAX/2-1)
		return -ENOTSUP;
	if(size) {
		for(int rsize = attrsize-1; rsize >= 0; rsize--) {
			value[rsize*2+1] = hex4b((uint8_t)value[rsize] & 0xF);
			value[rsize*2] = hex4b((uint8_t)value[rsize] >> 4);
		}
		value[attrsize*2] = '\0';
	}
	return attrsize*2+1;
}
#else
static int hfsfuse_getxattr(const char* path, const char* attr, char* value, size_t size) {
	return hfsfuse_getxattr_offset(path, attr, value, size, 0);
}
#endif
#endif

static struct fuse_operations hfsfuse_ops = {
#if FUSE_VERSION >= 30
	.init        = hfsfuse_init,
#endif
	.destroy     = hfsfuse_destroy,
	.open        = hfsfuse_open,
	.opendir     = hfsfuse_opendir,
	.read        = hfsfuse_read,
	.readdir     = hfsfuse_readdir,
	.release     = hfsfuse_release,
	.releasedir  = hfsfuse_releasedir,
	.statfs      = hfsfuse_statfs,
	.getattr     = hfsfuse_getattr,
	.readlink    = hfsfuse_readlink,
#if FUSE_VERSION < 30
	.fgetattr    = hfsfuse_fgetattr,
#endif
#if HAVE_STATX && FUSE_VERSION >= 318
	.statx       = hfsfuse_statx,
#endif
	.listxattr   = hfsfuse_listxattr,
#if FUSE_DARWIN_ENABLE_EXTENSIONS || (defined(__APPLE__) && FUSE_VERSION < 30)
	.getxattr    = hfsfuse_getxattr_offset,
#else
	.getxattr    = hfsfuse_getxattr,
#endif
#if defined(__APPLE__) && FUSE_VERSION < 30
	.getxtimes   = hfsfuse_getxtimes,
#endif
#if FUSE_VERSION >= 29 && FUSE_VERSION < 30
	.flag_nopath = 1,
#endif
#if FUSE_VERSION >= 28 && FUSE_VERSION < 30
	.flag_nullpath_ok = 1
#endif
};

enum {
	HFSFUSE_OPT_KEY_HELP,
	HFSFUSE_OPT_KEY_FULLHELP,
	HFSFUSE_OPT_KEY_VERSION
};

struct hfsfuse_config {
	struct hfs_volume_config volume_config;
	char* device;
	int noallow_other;
	int force;
};

#define HFS_OPTION(t, p) { t, offsetof(struct hfs_volume_config, p), 1 }
#define HFSFUSE_OPTION(t, p) { t, offsetof(struct hfsfuse_config, p), 1 }
static struct fuse_opt hfsfuse_opts[] = {
	FUSE_OPT_KEY("-h",        HFSFUSE_OPT_KEY_HELP),
	FUSE_OPT_KEY("--help",    HFSFUSE_OPT_KEY_HELP),
	FUSE_OPT_KEY("-H",        HFSFUSE_OPT_KEY_FULLHELP),
	FUSE_OPT_KEY("--fullhelp",HFSFUSE_OPT_KEY_FULLHELP),
	FUSE_OPT_KEY("-v",        HFSFUSE_OPT_KEY_VERSION),
	FUSE_OPT_KEY("--version", HFSFUSE_OPT_KEY_VERSION),
	HFSFUSE_OPTION("--force",force),
	HFSFUSE_OPTION("noallow_other",noallow_other),
	HFS_OPTION("cache_size=%zu",cache_size),
	HFS_OPTION("blksize=%" SCNu32,blksize),
	HFS_OPTION("noublio", noublio),
	HFS_OPTION("ublio_items=%" SCNd32, ublio_items),
	HFS_OPTION("ublio_grace=%" SCNu64,ublio_grace),
	HFS_OPTION("rsrc_ext=%s",rsrc_suff),
	HFS_OPTION("rsrc_only",rsrc_only),
	HFS_OPTION("default_file_mode=%" SCNo16,default_file_mode),
	HFS_OPTION("default_dir_mode=%"SCNo16,default_dir_mode),
	HFS_OPTION("default_uid=%" SCNu32,default_uid),
	HFS_OPTION("default_gid=%" SCNu32,default_gid),
	HFS_OPTION("disable_symlinks", disable_symlinks),
	FUSE_OPT_END
};

static void usage(const char* self) {
	fprintf(stderr,"usage: %s [-hHv] [-o options] volume mountpoint\n\n",self);
}

static void help(const char* self, struct hfsfuse_config* cfg) {
	usage(self);
	fprintf(
		stderr,
		"general options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h, --help             this help\n"
		"    -H, --fullhelp         list all FUSE options\n"
		"    -v, --version\n"
		"\n"
		"HFS+ options:\n"
		"    --force                force mount volumes with dirty journal\n"
		"    -o rsrc_only           only mount the resource forks of files\n"
		"    -o noallow_other       restrict filesystem access to mounting user\n"
		"    -o cache_size=N        size of lookup cache (%zu)\n"
		"    -o blksize=N           set a custom read size/alignment in bytes\n"
		"                           you should only set this if you are sure it is being misdetected\n"
		"    -o rsrc_ext=suffix     special suffix for filenames which can be used to access their resource fork\n"
		"                           or alternatively their data fork if mounted in rsrc_only mode\n"
		"\n"
		"    -o default_file_mode=N octal filesystem permissions for Mac OS Classic files (%" PRIo16 ")\n"
		"    -o default_dir_mode=N  octal filesystem permissions for Mac OS Classic directories (%" PRIo16 ")\n"
		"    -o default_uid=N       unix user ID for Mac OS Classic files (%" PRIu32 ")\n"
		"    -o default_gid=N       unix group ID for Mac OS Classic files (%" PRIu32 ")\n"
		"\n"
		"    -o disable_symlinks    treat symbolic links as regular files. may be used to view extended attributes\n"
		"                           of these on systems that don't support symlink xattrs\n"
		"\n",
		cfg->volume_config.cache_size,
		cfg->volume_config.default_file_mode,
		cfg->volume_config.default_dir_mode,
		cfg->volume_config.default_uid,
		cfg->volume_config.default_gid
	);
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_UBLIO) {
		fprintf(
			stderr,
			"    -o noublio             disable ublio read layer\n"
			"    -o ublio_items=N       number of ublio cache entries, 0 for no caching (%" PRId32 ")\n"
			"    -o ublio_grace=N       reclaim cache entries only after N requests (%" PRIu64 ")\n"
			"\n",
			cfg->volume_config.ublio_items,
			cfg->volume_config.ublio_grace
		);
	}
}

static void version(void) {
	fprintf(
		stderr,
		"hfsfuse version " HFSFUSE_VERSION_STRING "\n"
		"Built with:\n"
		"    FUSE API v%d.%d\n"
		"    libhfs RCSIDs %s; %s\n",
		FUSE_MAJOR_VERSION,
		FUSE_MINOR_VERSION,
		hfs_rcsid_libhfs,
		hfs_rcsid_unicode
	);

	if(hfs_get_lib_features() & HFS_LIB_FEATURES_UBLIO)
		fprintf(stderr, "    ublio v%s\n", hfs_lib_ublio_version());
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_UTF8PROC)
		fprintf(stderr, "    utf8proc v%s\n", hfs_lib_utf8proc_version());
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_ZLIB)
		fprintf(stderr, "    zlib v%s\n", hfs_lib_zlib_version());
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_LZFSE)
		fprintf(stderr, "    lzfse\n");
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_LZVN)
		fprintf(stderr, "    lzvn\n");
}

#if FUSE_VERSION < 28 || defined(__HAIKU__)
static int hfsfuse_opt_add_opt_escaped(char** opts, const char* opt) {
	char* escaped = malloc(strlen(opt)*2+1);
	if(!escaped)
		return -1;
	char* eit = escaped;
	for(const char* oit = opt; (oit = strpbrk(oit, ",\\")); opt = oit) {
		memcpy(eit,opt,oit-opt);
		eit += oit-opt;
		*eit++ = '\\';
		*eit++ = *oit++;
	}
	memcpy(eit,opt,strlen(opt)+1);
	int ret = fuse_opt_add_opt(opts, escaped);
	free(escaped);
	return ret;
}
#else
#define hfsfuse_opt_add_opt_escaped(opts,opt) fuse_opt_add_opt_escaped(opts,opt)
#endif

static int hfsfuse_opt_proc(void* data, const char* arg, int key, struct fuse_args* args) {
	struct hfsfuse_config* cfg = data;
	switch(key) {
		case HFSFUSE_OPT_KEY_HELP:
		case HFSFUSE_OPT_KEY_FULLHELP: {
			help(args->argv[0], cfg);
#if FUSE_VERSION < 30 && !defined(__HAIKU__) // this is declared in the userlandfs headers but isn't actually in the library
			fuse_parse_cmdline(args, NULL, NULL, NULL);
#endif
			if(key == HFSFUSE_OPT_KEY_FULLHELP) {
#if FUSE_VERSION < 30
				fuse_opt_add_arg(args, "-ho");
				// fuse_mount and fuse_new print their own set of options
				fuse_mount("", args);
				fuse_new(NULL, args, NULL, 0, NULL);
#else
				fuse_opt_add_arg(args, "-h");
				fuse_main(args->argc, args->argv, &hfsfuse_ops, NULL);
#endif
			}
			fuse_opt_free_args(args);
			exit(0);
		}
		case HFSFUSE_OPT_KEY_VERSION: {
			version();
			fuse_opt_free_args(args);
			exit(0);
		}
		case FUSE_OPT_KEY_NONOPT:
			if(!cfg->device) {
				cfg->device = strdup(arg);
				return 0;
			}
			// fallthrough
		default: return 1;
	}
}

static void hfs_vsyslog(const char* fmt, const char* file, int line, va_list args) {
#if HAVE_VSYSLOG
	vsyslog(LOG_ERR,fmt,args);
#else
	va_list argscpy;
	va_copy(argscpy,args);
	int len = vsnprintf(NULL,0,fmt,argscpy);
	va_end(argscpy);

	char* output;
	if(len < 0 || !(output = malloc((size_t)len+1)))
		return;
	vsnprintf(output,len+1,fmt,args);
	syslog(LOG_ERR,"%s",output);
	free(output);
#endif
}

int main(int argc, char* argv[]) {
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	struct hfsfuse_config cfg = {0};
	hfs_volume_config_defaults(&cfg.volume_config);

	int ret = 1;
	if(fuse_opt_parse(&args, &cfg, hfsfuse_opts, hfsfuse_opt_proc) == -1)
		goto opt_err;

	if(!cfg.device) {
		usage(args.argv[0]);
		goto opt_err;
	}

	if(cfg.volume_config.rsrc_suff && strchr(cfg.volume_config.rsrc_suff,'/')) {
		// FUSE tokenizes paths before lookup, so lookup would end at the file 'file.ext' before ever seeing e.g. 'file.ext/rsrc'.
		fprintf(stderr, "Error: rsrc_ext option may not include path separator: %s\n", cfg.volume_config.rsrc_suff);
		goto opt_err;
	}

	char* fsname = malloc(strlen("fsname=") + strlen(cfg.device) + 1);
	if(!fsname)
		goto opt_err;

	strcpy(fsname, "fsname=");
	strcat(fsname, cfg.device);

	char* opts = NULL;
	fuse_opt_add_opt(&opts, "ro");
	if(!cfg.noallow_other)
		fuse_opt_add_opt(&opts, "allow_other");
#if FUSE_VERSION < 30
	fuse_opt_add_opt(&opts, "use_ino");
#endif
	fuse_opt_add_opt(&opts, "subtype=hfs");
	hfsfuse_opt_add_opt_escaped(&opts, fsname);
	fuse_opt_add_arg(&args, "-o");
	fuse_opt_add_arg(&args, opts);
	fuse_opt_add_arg(&args, "-s");
	free(fsname);

	hfslib_init(&(hfs_callbacks){hfs_vprintf, hfs_malloc, hfs_realloc, hfs_free, hfs_open, hfs_close, hfs_read});

	// open volume
	hfs_volume vol;
	if(hfslib_open_volume(cfg.device, 1, &vol, &(hfs_callback_args){ .openvol = &cfg.volume_config })) {
		perror("Couldn't open volume");
		ret = errno;
		goto done;
	}
	if(!hfslib_is_journal_clean(&vol)) {
		fprintf(stderr,"Journal is dirty!");
		if(cfg.force)
			fprintf(stderr," Attempting to mount anyway (--force).\n");
		else {
			fprintf(stderr," Canceling mount. Use --force to ignore.\n");
			ret = EIO;
			goto done;
		}
	}
	hfs_gcb.error = hfs_vsyslog; // prepare to daemonize
	ret = fuse_main(args.argc, args.argv, &hfsfuse_ops, &vol);

done:
	hfslib_done();
	free((void*)cfg.device);
opt_err:
	fuse_opt_free_args(&args);
	return ret;
}
