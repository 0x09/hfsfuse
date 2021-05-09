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

#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <fuse.h>

#ifndef HFSFUSE_VERSION_STRING
#include "version.h"
#endif

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

static void hfsfuse_destroy(void* vol) {
	hfslib_close_volume(vol, NULL);
}


struct hf_file {
	hfs_cnid_t cnid;
	hfs_extent_descriptor_t* extents;
	uint16_t nextents;
	uint8_t fork;
	bool is_empty;
};

static int hfsfuse_open(const char* path, struct fuse_file_info* info) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key; unsigned char fork;
	int ret = hfs_lookup(vol,path,&rec,&key,&fork);
	if(ret)
		return ret;

	struct hf_file* f = malloc(sizeof(*f));
	if(!f)
		return -ENOMEM;

	f->cnid = rec.file.cnid;
	f->fork = fork;
	f->is_empty = (fork == HFS_RSRCFORK ? rec.file.rsrc_fork : rec.file.data_fork).logical_size == 0;
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
	if(f->is_empty)  // empty files have no extents, which to hfslib_readd_with_extents() is an error
		return 0;  // so skip it and just return 0 for EOF
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
	if(ret)
		return ret;

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
	if(ret)
		return ret;

	struct hf_dir* d = malloc(sizeof(*d));
	if(!d)
		return -ENOMEM;

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

#if 0
static int hfsfuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* info) {
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	hfs_volume* vol = fuse_get_context()->private_data;
	struct hf_dir* d = (struct hf_dir*)info->fh;
	char pelem[512];
	int ret = 0;
	for(size_t i = 0; i < d->npaths; i++) {
		int err;
		if((err = hfs_pathname_to_unix(d->paths+i,pelem)) < 0) {
			ret = err;
			continue;
		}
		struct stat st;
		hfs_stat(vol,d->keys+i,&st,0);
		if(filler(buf,pelem,&st,0)) {
			ret = -errno;
			break;
		}
	}
	return min(ret,0);
}
#endif

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

		struct stat* stp = NULL;
		if(d->cnid != HFS_CNID_ROOT_FOLDER) {
			stp = &st;
			hfslib_find_catalog_record_with_cnid(vol, key.parent_cnid, &rec, &key, NULL);
			hfs_stat(vol, &rec, stp, 0);
		}
		if(filler(buf, "..", stp, 2))
			return 0;
	}
	char pelem[512];
	int ret = 0;
	for(off_t i = max(0,offset-2); i < d->npaths; i++) {
		int err;
		if((err = hfs_pathname_to_unix(d->paths+i,pelem)) < 0) {
			ret = err;
			continue;
		}
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

#ifdef __APPLE__
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

#ifdef __APPLE__
#define attrname(name) name
#else
#define attrname(name) "user." name
#endif

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
			if(size < (size_t)attrsize) return -ERANGE;\
			else block \
		}\
		return attrsize;\
	}\
} while(0)

static int hfsfuse_getxattr(const char* path, const char* attr, char* value, size_t size) {
	hfs_volume* vol = fuse_get_context()->private_data;
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key;
	int ret = hfs_lookup(vol,path,&rec,&key,NULL);
	if(ret)
		return ret;

	define_attr(attr, "com.apple.FinderInfo", size, 32, {
		hfs_serialize_finderinfo(&rec, value);
	});
	ret = rec.file.rsrc_fork.logical_size;
	define_attr(attr, "com.apple.ResourceFork", size, ret, {
		hfs_extent_descriptor_t* extents = NULL;
		uint64_t bytes;
		if(size > (size_t)ret)
			size = ret;
		uint16_t nextents = hfslib_get_file_extents(vol,rec.file.cnid,HFS_RSRCFORK,&extents,NULL);
		if((ret = hfslib_readd_with_extents(vol,value,&bytes,size,0,extents,nextents,NULL)) >= 0)
			ret = bytes;
		else ret = -EIO;
		free(extents);
	});

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

	return -ENOATTR;
}

#ifdef __APPLE__
static int hfsfuse_getxattr_darwin(const char* path, const char* attr, char* value, size_t size, u_int32_t unused) {
	return hfsfuse_getxattr(path, attr, value, size);
}
#endif

static struct fuse_operations hfsfuse_ops = {
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
#if FUSE_VERSION >= 29
	.flag_nopath = 1,
#endif
#if FUSE_VERSION >= 28
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
	HFS_OPTION("blksize=%" PRIu32,blksize),
	HFS_OPTION("noublio", noublio),
	HFS_OPTION("ublio_items=%u",  ublio_items),
	HFS_OPTION("ublio_grace=%llu",ublio_grace),
	HFS_OPTION("rsrc_ext=%s",rsrc_suff),
	HFS_OPTION("rsrc_only",rsrc_only),
	FUSE_OPT_END
};

static void usage(const char* self) {
	fprintf(stderr,"usage: %s [-hHv] [-o options] device mountpoint\n\n",self);
}

static void help(const char* self, struct hfsfuse_config* cfg) {
	usage(self);
	fprintf(
		stderr,
		"general options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h   --help            this help\n"
		"    -H   --fullhelp        list all FUSE options\n"
		"    -v   --version\n"
		"\n"
		"HFS options:\n"
		"    --force                force mount volumes with dirty journal\n"
		"    -o rsrc_only           only mount the resource forks of files\n"
		"    -o noallow_other       restrict filesystem access to mounting user\n"
		"    -o cache_size=N        size of lookup cache (%zu)\n"
		"    -o blksize=N           set a custom read size/alignment in bytes\n"
		"                           you should only set this if you are sure it is being misdetected\n"
		"    -o rsrc_ext=suffix     special suffix for filenames which can be used to access their resource fork\n"
		"                           or alternatively their data fork if mounted in rsrc_only mode\n"
		"\n",
		cfg->volume_config.cache_size
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

static void version() {
	fprintf(
		stderr,
		"hfsfuse version " HFSFUSE_VERSION_STRING "\n"
		"Built with:\n"
		"    FUSE API v%d.%d\n"
		"    libhfs RCSID %s\n",
		FUSE_MAJOR_VERSION,
		FUSE_MINOR_VERSION,
		hfslib_get_rcsid()
	);

	if(hfs_get_lib_features() & HFS_LIB_FEATURES_UBLIO)
		fprintf(stderr, "    ublio v%s\n", hfs_lib_ublio_version());
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_UTF8PROC)
		fprintf(stderr, "    utf8proc v%s\n", hfs_lib_utf8proc_version());
}

#if FUSE_VERSION < 28
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
			fuse_opt_add_arg(args, "-ho");
#ifndef __HAIKU__ // this is declared in the userlandfs headers but isn't actually in the library
			fuse_parse_cmdline(args, NULL, NULL, NULL);
#endif
			if(key == HFSFUSE_OPT_KEY_FULLHELP) {
				// fuse_mount and fuse_new print their own set of options
				fuse_mount(NULL, args);
				fuse_new(NULL, args, NULL, 0, NULL);
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
	fuse_opt_add_opt(&opts, "use_ino");
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
	hfslib_callbacks()->error = hfs_vsyslog; // prepare to daemonize
	ret = fuse_main(args.argc, args.argv, &hfsfuse_ops, &vol);

done:
	hfslib_done();
	free((void*)cfg.device);
opt_err:
	fuse_opt_free_args(&args);
	return ret;
}
