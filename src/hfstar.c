/*
 * hfstar - Convert all or part of an HFS+ volume to various archive file formats
 * This file is part of the hfsfuse project.
 */

#include "hfsuser.h"

#include "uthash.h"
#include "utstack.h"

#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef HFSFUSE_VERSION_STRING
#include "version.h"
#endif

#define STR_(x) #x
#define STR(x) STR_(x)
#define XATTR_NAMESPACE_STR STR(XATTR_NAMESPACE)
#define attrname(name) XATTR_NAMESPACE_STR name

struct dir_hardlink_map {
	UT_hash_handle hash;
	hfs_cnid_t cnid;
	size_t pathlen;
	char path[];
};

struct hfstar_dirent {
	struct hfstar_dirent* next;
	hfs_catalog_keyed_record_t rec;
	size_t pathlen;
	char path[];
};

struct hfstar_archive_context {
	hfs_volume* vol;
	struct archive* archive;
	struct archive_entry_linkresolver* linkresolver;
	struct dir_hardlink_map* dir_hardlink_map;
	char* read_buf;
	size_t read_bufsize;
	char* rsrc_ext;
	size_t rsrc_extlen;
	int archive_err, hfs_err;
	bool stop_on_error, symbolic_dir_links, trim_prefix, print_paths, no_warn;
};

#define hfstar_err(ctx) ((ctx)->hfs_err || (ctx)->archive_err < ARCHIVE_WARN)

#define unrecoverable_err(ctx) \
	((ctx)->archive_err == ARCHIVE_FATAL || ((ctx)->stop_on_error && hfstar_err((ctx))))

#define log_hfs_err(ctx,format,...) \
	((ctx)->hfs_err < 0 ? \
	fprintf(stderr,format ": %s\n",__VA_ARGS__,strerror(-(ctx)->hfs_err)) : \
	fprintf(stderr,format "\n",__VA_ARGS__))

static inline void log_archive_err(struct hfstar_archive_context* ctx) {
	if(ctx->archive_err < ARCHIVE_WARN || (!ctx->no_warn && ctx->archive_err == ARCHIVE_WARN))
		fprintf(stderr,"%s\n",archive_error_string(ctx->archive));
}

static inline char* relative_path(const char* srcpath, const char* dstpath) {
	char* relpath;
	const char* src = srcpath,* dst = dstpath;
	for(const char* srcp = src,* dstp = dst; *srcp && *srcp == *dstp; srcp++, dstp++)
		if(*srcp == '/') {
			src = srcp+1;
			dst = dstp+1;
		}

	size_t depth = 0;
	for(const char* srcp = src; *srcp; srcp++)
		depth += *srcp == '/' && *(srcp+1);

	size_t dstlen = strlen(dst);
	if(!(relpath = malloc(depth*3+dstlen+1)))
		return NULL;

	for(size_t i = 0; i < depth; i++)
		memcpy(relpath+i*3,"../",3);
	memcpy(relpath+depth*3,dst,dstlen+1);

	return relpath;
}

static void hfstar_write_automatic_xattrs(struct hfstar_archive_context* ctx, const char* path, hfs_catalog_keyed_record_t* rec, struct archive_entry* entry) {
	archive_entry_set_birthtime(entry,HFSTIMETOEPOCH(rec->file.date_created),0);

	char timebuf[25];
	struct tm t;
	if(rec->file.date_created) {
		gmtime_r(&(time_t){HFSTIMETOEPOCH(rec->file.date_created)}, &t);
		strftime(timebuf, 25, "%Y-%m-%dT%H:%M:%S+0000", &t);
		archive_entry_xattr_add_entry(entry,attrname("hfsfuse.record.date_created"),timebuf,25);
	}

	if(rec->file.date_backedup) {
		gmtime_r(&(time_t){HFSTIMETOEPOCH(rec->file.date_backedup)}, &t);
		strftime(timebuf, 25, "%Y-%m-%dT%H:%M:%S+0000", &t);
		archive_entry_xattr_add_entry(entry,attrname("hfsfuse.record.date_backedup"),timebuf,25);
	}

	char finderinfo[32];
	hfs_serialize_finderinfo(rec,finderinfo);
	if(memcmp(finderinfo,&(char[32]){0},32))
		archive_entry_xattr_add_entry(entry,attrname("com.apple.FinderInfo"),finderinfo,32);

	struct hfs_decmpfs_header decmpfs_header;
	bool compressed = rec->type == HFS_REC_FILE && !hfs_decmpfs_lookup(ctx->vol,&rec->file,&decmpfs_header,NULL,NULL);

	if(!ctx->rsrc_ext && rec->type == HFS_REC_FILE && rec->file.rsrc_fork.logical_size && !compressed) {
		hfs_extent_descriptor_t* extents = NULL;
		uint16_t nextents = hfslib_get_file_extents(ctx->vol,rec->file.cnid,HFS_RSRCFORK,&extents,NULL);
		if(nextents) {
			uint64_t bytes = 0;
			char* data = malloc(rec->file.rsrc_fork.logical_size);
			if(!data) {
				free(extents);
				ctx->hfs_err = -ENOMEM;
				return;
			}
			if(!(ctx->hfs_err = hfslib_readd_with_extents(ctx->vol,data,&bytes,rec->file.rsrc_fork.logical_size,0,extents,nextents,NULL)))
				archive_entry_xattr_add_entry(entry,attrname("com.apple.ResourceFork"),data,rec->file.rsrc_fork.logical_size);
			else log_hfs_err(ctx,"Can't read resource fork of '%s'",path);

			free(extents);
			free(data);
		}
	}
}

static void hfstar_write_user_defined_xattrs(struct hfstar_archive_context* ctx, const char* path, hfs_catalog_keyed_record_t* rec, struct archive_entry* entry) {
	char* attr_name = malloc(sizeof(XATTR_NAMESPACE_STR)+HFS_NAME_MAX);
	if(!attr_name) {
		ctx->hfs_err = -ENOMEM;
		return;
	}
	hfs_attribute_key_t* attr_keys;
	uint32_t nattrs;
	hfslib_find_attribute_records_for_cnid(ctx->vol,rec->file.cnid,&attr_keys,&nattrs,NULL);
	for(uint32_t i = 0; i < nattrs; i++) {
		void* attr_value = NULL;
		memcpy(attr_name,XATTR_NAMESPACE_STR,sizeof(XATTR_NAMESPACE_STR)-1);
		ssize_t u8len = hfs_unistr_to_utf8(&attr_keys[i].name,attr_name+sizeof(XATTR_NAMESPACE_STR)-1);
		if((ctx->hfs_err = u8len <= 0)) {
			fprintf(stderr,"Can't convert extended attribute name to UTF-8 for '%s'\n",path);
			goto attr_end;
		}

		hfs_attribute_record_t attrec;
		if((ctx->hfs_err = hfslib_find_attribute_record_with_key(ctx->vol,attr_keys+i,&attrec,&attr_value,NULL)))
			goto attr_end;
		switch(attrec.type) {
			case HFS_ATTR_INLINE_DATA:
				archive_entry_xattr_add_entry(entry,attr_name,attr_value,attrec.inline_record.length);
				break;
			case HFS_ATTR_FORK_DATA: {
				attr_value = malloc(attrec.fork_record.fork.logical_size);
				hfs_extent_descriptor_t* extents;
				uint16_t nextents;
				if((ctx->hfs_err = hfslib_get_attribute_extents(ctx->vol,attr_keys+i,&attrec,&nextents,&extents,NULL)))
					goto attr_end;

				if(nextents) {
					uint64_t bytesread;
					if(!(ctx->hfs_err = hfslib_readd_with_extents(ctx->vol,attr_value,&bytesread,attrec.fork_record.fork.logical_size,0,extents,nextents,NULL)))
						archive_entry_xattr_add_entry(entry,attr_name,attr_value,bytesread);
					free(extents);
				}
			}; break;
		}

		if(ctx->hfs_err)
			log_hfs_err(ctx,"Can't read extended attribute '%s' from '%s'",attr_name,path);

attr_end:
		free(attr_value);
		if(ctx->hfs_err && ctx->stop_on_error)
			break;
	}

	free(attr_keys);
	free(attr_name);
}

static void hfstar_write_symlink(struct hfstar_archive_context* ctx, const char* path, hfs_catalog_keyed_record_t* rec, struct archive_entry* entry) {
	char* dest = malloc(rec->file.data_fork.logical_size+1);
	if(!dest) {
		ctx->hfs_err = -ENOMEM;
		return;
	}

	hfs_extent_descriptor_t* extents = NULL;
	uint16_t nextents = hfslib_get_file_extents(ctx->vol,rec->file.cnid,HFS_DATAFORK,&extents,NULL);
	uint64_t bytes = 0;
	if(!(ctx->hfs_err = hfslib_readd_with_extents(ctx->vol,dest,&bytes,rec->file.data_fork.logical_size+1,0,extents,nextents,NULL)) && bytes) {
		dest[rec->file.data_fork.logical_size] = '\0';
		archive_entry_set_symlink_utf8(entry,dest);
	}
	else log_hfs_err(ctx,"Can't read symlink destination for '%s'",path);

	free(dest);
	free(extents);
}

static void hfstar_write_file(struct hfstar_archive_context* ctx, hfs_catalog_keyed_record_t* rec, int fork) {
	struct hfs_file* f = hfs_file_open(ctx->vol,rec,fork,&ctx->hfs_err);
	if(!f)
		return;

	size_t bufsize = hfs_file_ideal_read_size(f,16384);
	if(bufsize > ctx->read_bufsize) {
		char* tmp = realloc(ctx->read_buf,bufsize);
		if(!tmp) {
			ctx->hfs_err = -ENOMEM;
			goto end;
		}
		ctx->read_buf = tmp;
		ctx->read_bufsize = bufsize;
	}

	ssize_t bytes;
	la_ssize_t entry_bytes;
	while((bytes = hfs_file_read(f,ctx->read_buf,bufsize)) > 0)
		if((entry_bytes = archive_write_data(ctx->archive,ctx->read_buf,bytes)) != bytes) {
			if(entry_bytes == ARCHIVE_RETRY)
				continue;
			if(entry_bytes == ARCHIVE_WARN && !ctx->no_warn)
				fprintf(stderr,"%s\n",archive_error_string(ctx->archive));
			if(entry_bytes < ARCHIVE_OK)
				ctx->archive_err = entry_bytes;
			break;
		}

	if(bytes < 0)
		ctx->hfs_err = bytes;

end:
	hfs_file_close(f);
}

static void hfstar_write_rsrc_entry(struct hfstar_archive_context* ctx, const char* path, size_t pathlen, hfs_catalog_keyed_record_t* rec) {
	struct archive_entry* rsrc_entry = archive_entry_new();
	char* rsrc_path = malloc(pathlen+ctx->rsrc_extlen+1);
	if(!(rsrc_entry || rsrc_path)) {
		ctx->hfs_err = -ENOMEM;
		goto entry_end;
	}

	memcpy(rsrc_path,path,pathlen);
	memcpy(rsrc_path+pathlen,ctx->rsrc_ext,ctx->rsrc_extlen+1);
	archive_entry_set_pathname_utf8(rsrc_entry,rsrc_path);

	struct stat st;
	hfs_stat(ctx->vol,rec,&st,HFS_RSRCFORK);
	archive_entry_copy_stat(rsrc_entry,&st);

	if((ctx->archive_err = archive_write_header(ctx->archive,rsrc_entry))) {
		if(ctx->archive_err <= ARCHIVE_FAILED)
			goto entry_end;
		log_archive_err(ctx);
	}

	hfstar_write_file(ctx,rec,HFS_RSRCFORK);

entry_end:
	free(rsrc_path);
	archive_entry_free(rsrc_entry);
}

static void hfstar_write_deferred_entry(struct hfstar_archive_context* ctx, struct archive_entry* entry) {
	hfs_catalog_keyed_record_t rec;
	hfs_cnid_t cnid = archive_entry_ino(entry);
	const char* path = archive_entry_pathname(entry);

	if((ctx->archive_err = archive_write_header(ctx->archive,entry))) {
		if(ctx->archive_err <= ARCHIVE_FAILED)
			goto entry_end;
		log_archive_err(ctx);
	}

	if((ctx->hfs_err = hfslib_get_hardlink(ctx->vol,cnid,&rec,NULL)))
		goto entry_end;

	if((rec.file.bsd.file_mode & HFS_S_IFMT) == HFS_S_IFLNK)
		// stored in the archive header in hfstar_write_entry
		goto entry_end;

	hfstar_write_file(ctx,&rec,HFS_DATAFORK);

	if(ctx->rsrc_ext && rec.file.rsrc_fork.logical_size)
		hfstar_write_rsrc_entry(ctx,path,strlen(path),&rec);

entry_end:
	if(ctx->hfs_err)
		log_hfs_err(ctx,"Error reading path '%s'",path);
	if(ctx->archive_err == ARCHIVE_FAILED)
		fprintf(stderr,"Error archiving '%s': %s\n",path,archive_error_string(ctx->archive));

	archive_entry_free(entry);
}

static void hfstar_write_entry(struct hfstar_archive_context* ctx, const char* path, size_t pathlen, hfs_catalog_keyed_record_t* rec, bool* header_only) {
	*header_only = false;

	struct archive_entry* entry = archive_entry_new();
	if(!entry) {
		ctx->hfs_err = -ENOMEM;
		goto entry_end;
	}

	struct dir_hardlink_map* link = NULL;
	bool file_hardlink = false, directory_hardlink = false;
	if(rec->type == HFS_REC_FILE) {
		// directory hard links are either resolved or converted to symbolic links
		if((directory_hardlink = rec->file.user_info.file_creator == HFS_MACS_CREATOR && rec->file.user_info.file_type == HFS_DIR_HARD_LINK_FILE_TYPE)) {
			if(ctx->symbolic_dir_links) {
				hfs_cnid_t linked_cnid = rec->file.bsd.special.inode_num;
				HASH_FIND(hash,ctx->dir_hardlink_map,&linked_cnid,sizeof(hfs_cnid_t),link);
				if(link) {
					*header_only = true;

					char* relpath = relative_path(path,link->path);
					if(!relpath) {
						ctx->hfs_err = -ENOMEM;
						goto entry_end;
					}
					archive_entry_set_symlink_utf8(entry,relpath);
					free(relpath);
				}
				else {
					if((ctx->hfs_err = hfslib_get_directory_hardlink(ctx->vol,linked_cnid,rec,NULL)))
						goto entry_end;

					if(!(link = malloc(sizeof(*link)+pathlen+1))) {
						ctx->hfs_err = -ENOMEM;
						goto entry_end;
					}

					link->cnid = rec->file.cnid;
					link->pathlen = pathlen;
					memcpy(link->path,path,pathlen+1);
					HASH_ADD(hash,ctx->dir_hardlink_map,cnid,sizeof(hfs_cnid_t),link);
				}
			}
			else {
				if((ctx->hfs_err = hfslib_get_directory_hardlink(ctx->vol,rec->file.bsd.special.inode_num,rec,NULL)))
					goto entry_end;
			}
		}
		else if((file_hardlink = rec->file.user_info.file_creator == HFS_HFSPLUS_CREATOR && rec->file.user_info.file_type == HFS_HARD_LINK_FILE_TYPE)) {
			hfs_cnid_t linked_cnid = rec->file.bsd.special.inode_num;
			if((ctx->hfs_err = hfslib_get_hardlink(ctx->vol,linked_cnid,rec,NULL)))
				goto entry_end;
		}
	}

	archive_entry_set_pathname_utf8(entry,path);

	// store stat info
	struct stat st;
	hfs_stat(ctx->vol,rec,&st,HFS_DATAFORK);
	archive_entry_copy_stat(entry,&st);

#if HAVE_STAT_FLAGS
	archive_entry_set_fflags(entry,st.st_flags,0);
#else
	archive_entry_set_fflags(entry,(rec->file.bsd.admin_flags << 16) | rec->file.bsd.owner_flags,0);
#endif

	if(ctx->symbolic_dir_links && directory_hardlink && *header_only)
		archive_entry_set_filetype(entry,AE_IFLNK);
	else archive_entry_set_filetype(entry,st.st_mode & AE_IFMT);

	// extra data exposed as extended attributes
	hfstar_write_automatic_xattrs(ctx,path,rec,entry);
	if(unrecoverable_err(ctx))
		goto entry_end;

	// extended attributes
	hfstar_write_user_defined_xattrs(ctx,path,rec,entry);
	if(unrecoverable_err(ctx))
		goto entry_end;

	// archive symlinks
	if(rec->type == HFS_REC_FILE && (rec->file.bsd.file_mode & HFS_S_IFMT) == HFS_S_IFLNK) {
		*header_only = true;
		hfstar_write_symlink(ctx,path,rec,entry);
	}

	if(file_hardlink) {
		struct archive_entry* spare = NULL;
		archive_entry_linkify(ctx->linkresolver,&entry,&spare);
		if(spare) {
			hfstar_write_deferred_entry(ctx,spare);
			if(unrecoverable_err(ctx))
				goto entry_end;
		}
		if(!entry) {
			*header_only = true;
			goto entry_end;
		}
		 if(!archive_entry_size(entry))
			*header_only = true;
	}

	if((ctx->archive_err = archive_write_header(ctx->archive,entry))) {
		if(ctx->archive_err <= ARCHIVE_FAILED)
			goto entry_end;
		log_archive_err(ctx);
	}

	if(*header_only)
		goto entry_end;

	// actual file data
	if(rec->type == HFS_REC_FILE) {
		hfstar_write_file(ctx,rec,HFS_DATAFORK);

		if(unrecoverable_err(ctx))
			goto entry_end;

		if(ctx->rsrc_ext && rec->file.rsrc_fork.logical_size)
			hfstar_write_rsrc_entry(ctx,path,pathlen,rec);
	}

entry_end:
	if(ctx->hfs_err)
		log_hfs_err(ctx,"Error reading path '%s'",path);
	if(ctx->archive_err == ARCHIVE_FAILED)
		fprintf(stderr,"Error archiving '%s': %s\n",path,archive_error_string(ctx->archive));

	archive_entry_free(entry);
}

static void hfstar_archive_records(struct hfstar_archive_context* ctx, const char* path, hfs_catalog_keyed_record_t* root_rec) {
	ctx->hfs_err = 0;
	ctx->archive_err = ARCHIVE_OK;

	size_t initial_pathlen = strlen(path);
	struct hfstar_dirent* stack = NULL,* initial = malloc(sizeof(*initial)+initial_pathlen+1);
	if(!initial) {
		ctx->hfs_err = -ENOMEM;
		return;
	}

	initial->pathlen = initial_pathlen;
	memcpy(initial->path,path,initial_pathlen+1);
	memcpy(&initial->rec,root_rec,sizeof(initial->rec));
	STACK_PUSH(stack,initial);

	do {
		struct hfstar_dirent* cur;
		STACK_POP(stack,cur);

		if(unrecoverable_err(ctx))
			goto dirent_end;

		ctx->archive_err = ARCHIVE_OK;

		if(!ctx->trim_prefix || cur != initial || cur->rec.type == HFS_REC_FILE) {
			if(ctx->print_paths)
				puts(cur->path);

			bool header_only;
			hfstar_write_entry(ctx,cur->path,cur->pathlen,&cur->rec,&header_only);
			if(header_only || hfstar_err(ctx))
				goto dirent_end;
		}
		if(cur->rec.type == HFS_REC_FLDR) {
			hfs_catalog_keyed_record_t* recs = NULL;
			hfs_unistr255_t* names = NULL;
			uint32_t entries = 0;

			ctx->hfs_err = hfslib_get_directory_contents(ctx->vol,cur->rec.folder.cnid,&recs,&names,&entries,NULL);
			for(uint32_t i = 0; i < entries; i++) {
				struct hfstar_dirent* next = malloc(sizeof(*next)+cur->pathlen+1+names[i].length*3+1);
				if(!next) {
					ctx->hfs_err = -ENOMEM;
					break;
				}

				memcpy(next->path,cur->path,cur->pathlen);
				next->pathlen = cur->pathlen;
				if(next->pathlen && next->path[next->pathlen-1] != '/')
					next->path[next->pathlen++] = '/';

				ssize_t len = hfs_pathname_to_unix(names+i,next->path+next->pathlen);
				if((ctx->hfs_err = len <= 0)) {
					fprintf(stderr,"Error converting path for CNID %" PRIu32 ": %zd\n",recs[i].file.cnid,len);
					free(next);
					if(ctx->stop_on_error)
						break;
					continue;
				}

				next->pathlen += len;
				memcpy(&next->rec,recs+i,sizeof(hfs_catalog_keyed_record_t));
				STACK_PUSH(stack,next);
			}
			free(recs);
			free(names);
		}

dirent_end:
		free(cur);
	} while(!STACK_EMPTY(stack));

	// process deferred hard link entries
	struct archive_entry* entry = NULL,* spare = NULL;
	archive_entry_linkify(ctx->linkresolver,&entry,&spare);
	while(entry || spare) {
		if(entry) {
			hfstar_write_deferred_entry(ctx,entry);
			if(unrecoverable_err(ctx)) {
				archive_entry_free(spare);
				break;
			}
		}
		if(spare) {
			hfstar_write_deferred_entry(ctx,spare);
			if(unrecoverable_err(ctx))
				break;
		}
		entry = spare = NULL;
		archive_entry_linkify(ctx->linkresolver,&entry,&spare);
	}
}

static void version(void) {
	fprintf(
		stderr,
		"hfstar version " HFSFUSE_VERSION_STRING "\n"
		"Built with:\n"
		"    libhfs RCSIDs %s; %s\n"
		"    %s\n",
		hfs_rcsid_libhfs,
		hfs_rcsid_unicode,
		archive_version_string()
	);

	if(hfs_get_lib_features() & HFS_LIB_FEATURES_UBLIO)
		fprintf(stderr, "    ublio v%s\n", hfs_lib_ublio_version());
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_UTF8PROC)
		fprintf(stderr, "    utf8proc v%s\n", hfs_lib_utf8proc_version());
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_ZLIB)
		fprintf(stderr, "    zlib v%s\n", hfs_lib_zlib_version());
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_LZFSE)
		fprintf(stderr, "    LZFSE\n");

	exit(0);
}

static void usage(void) {
	fprintf(stderr,"Usage: hfstar [options] <device> <archive> [<prefix>]\n");
	exit(2);
}

static void help(struct hfs_volume_config* cfg) {
	printf(
		"Usage: hfstar [options] <volume> <archive> [<prefix>]\n"
		"\n"
		"  <volume>   HFS+ image or device to convert.\n"
		"  <archive>  Output archive file.\n"
		"  <prefix>   Optional path in the HFS+ volume to archive. Default: /\n"
		"\n"
		"Options:\n"
		"  -h, --help\n"
		"  -v, --version\n"
		"\n"
		"  -b <bufsize>  Size of read buffer. Default: device's block size or 16kb for regular files.\n"
		"  -s            Archive directory hard links as symbolic links instead of resolving them.\n"
		"  -t            Trim <prefix> from archived paths. The root entry will be omitted.\n"
		"  -e            Stop archiving if any entry has an error.\n"
		"  -p            Print paths being archived.\n"
		"  -W            Silence warnings.\n"
		"\n"
		"libarchive options:\n"
		"  --format <name>   Name of the archive format. May be any format accepted by libarchive.\n"
		"                    Default: inferred from the output archive file extension.\n"
		"                    For .tar or no extension, defaults to posix format."
		"  --filter <name>   Name of a libarchive filter. Default: inferred from the output file extension.\n"
		"  --options <opts>  String of libarchive options in the format option=value,...\n"
		"\n"
		"HFS+ options:\n"
		"  --force           Try to read volumes with a dirty journal\n"
		"  --blksize <n>     Device block size. Default: autodetected.\n"
		"  --rsrc-ext <ext>  Archive resource forks as separate files with this extension, instead of as an xattr.\n"
		"\n"
		"  --default-file-mode <mode>  Octal filesystem permissions for Mac OS Classic files. Default: %" PRIo16 "\n"
		"  --default-dir-mode <mode>   Octal filesystem permissions for Mac OS Classic directories. Default: %" PRIo16 "\n"
		"  --default-uid <uid>         Unix user ID for Mac OS Classic files. Default: %" PRIu32 "\n"
		"  --default-gid <gid>         Unix group ID for Mac OS Classic files. Default: %" PRIu32 "\n"
		"\n",
		cfg->default_file_mode,
		cfg->default_dir_mode,
		cfg->default_uid,
		cfg->default_gid
	);
	if(hfs_get_lib_features() & HFS_LIB_FEATURES_UBLIO) {
		printf(
			"  --noublio          Disable ublio read layer.\n"
			"  --ublio-items <N>  Number of ublio cache entries, 0 for no caching. Default: %" PRId32 "\n"
			"  --ublio-grace <N>  Reclaim cache entries only after N requests. Default: %" PRIu64 "\n"
			"\n",
			cfg->ublio_items,
			cfg->ublio_grace
		);
	}
	exit(0);
}

int main(int argc, char* argv[]) {
	setlocale(LC_CTYPE, "UTF-8");

	const char* format = NULL,* filter = NULL,* options = NULL;

	struct hfstar_archive_context ctx = {0};
	struct hfs_volume_config cfg;
	int force = 0;

	hfs_volume_config_defaults(&cfg);

	const struct option opts[] = {
		{"help",no_argument,NULL,'h'},
		{"version",no_argument,NULL,'v'},
		{"format",required_argument,NULL,1},
		{"filter",required_argument,NULL,2},
		{"options",required_argument,NULL,3},
		{"force",no_argument,&force,1},
		{"blksize",required_argument,NULL,4},
		{"rsrc-ext",required_argument,NULL,5},
		{"default-file-mode",required_argument,NULL,6},
		{"default-dir-mode",required_argument,NULL,7},
		{"default-uid",required_argument,NULL,8},
		{"default-gid",required_argument,NULL,9},
		{"noublio",no_argument,&cfg.noublio,1},
		{"ublio-items",required_argument,NULL,10},
		{"ublio-grace",required_argument,NULL,11},
	};

	int c;
	int longoptind = 0;
	while((c = getopt_long(argc,argv,":vhb:stepW",opts,&longoptind)) != -1)
		switch(c) {
			case 0: break;
			case 'v': version();  break;
			case 'h': help(&cfg); break;
			case 'b': ctx.read_bufsize = strtoul(optarg,NULL,10); break;
			case 'e': ctx.stop_on_error = true; break;
			case 's': ctx.symbolic_dir_links = true; break;
			case 't': ctx.trim_prefix = true; break;
			case 'p': ctx.print_paths = true; break;
			case 'W': ctx.no_warn = true; break;
			case 1: format = optarg; break;
			case 2: filter = optarg; break;
			case 3: options = optarg; break;
			case 4: cfg.blksize = strtoul(optarg,NULL,10); break;
			case 5:
				ctx.rsrc_ext = optarg;
				ctx.rsrc_extlen = strlen(optarg);
				break;
			case 6: cfg.default_file_mode = strtoul(optarg,NULL,8); break;
			case 7: cfg.default_dir_mode = strtoul(optarg,NULL,8); break;
			case 8: cfg.default_uid = strtoul(optarg,NULL,10); break;
			case 9: cfg.default_gid = strtoul(optarg,NULL,10); break;
			case 10: cfg.ublio_items = strtoul(optarg,NULL,10); break;
			case 11: cfg.ublio_grace = strtoul(optarg,NULL,10); break;
			default: usage();
		}
	argv += optind;
	argc -= optind;

	if(argc < 2)
		usage();

	int err;

	ctx.vol = malloc(sizeof(*ctx.vol));
	ctx.archive = archive_write_new();
	ctx.linkresolver = archive_entry_linkresolver_new();

	if(!(ctx.vol && ctx.archive && ctx.linkresolver)) {
		ctx.hfs_err = -ENOMEM;
		goto end;
	}

	hfs_catalog_keyed_record_t root_rec;

	cfg.cache_size = 0;

	if((ctx.hfs_err = hfs_open_volume(argv[0],ctx.vol,&cfg))) {
		log_hfs_err(&ctx,"Couldn't open volume '%s'",argv[1]);
		goto end;
	}

	if(!hfslib_is_journal_clean(ctx.vol)) {
		fprintf(stderr,"Journal is dirty!");
		if(force)
			fprintf(stderr," Attempting to archive anyway (--force).\n");
		else {
			fprintf(stderr," Canceling archival. Use --force to ignore.\n");
			ctx.hfs_err = 1;
			goto end;
		}

	}

	if(!ctx.read_bufsize && !(ctx.read_bufsize = hfs_device_block_size(ctx.vol)))
		ctx.read_bufsize = 16384;

	if(!(ctx.read_buf = malloc(ctx.read_bufsize))) {
		ctx.hfs_err = -ENOMEM;
		goto end;
	}

	char* path = argc < 3 ? "/" : argv[2];
	if((ctx.hfs_err = hfs_lookup(ctx.vol,path,&root_rec,NULL,NULL))) {
		log_hfs_err(&ctx,"Path lookup failure for '%s'",argv[2]);
		goto end;
	}

	if(format)
		ctx.archive_err = archive_write_set_format_by_name(ctx.archive,format);
	else {
		ctx.archive_err = archive_write_set_format_filter_by_ext_def(ctx.archive,argv[1],".tar");
		// override libarchive's default of pax restricted to full pax unless explicitly set
		if(archive_format(ctx.archive) == ARCHIVE_FORMAT_TAR_PAX_RESTRICTED)
			archive_write_set_format_pax(ctx.archive);
	}
	if(ctx.archive_err) {
		log_archive_err(&ctx);
		if(ctx.archive_err < ARCHIVE_WARN)
			goto end;
	}

	archive_entry_linkresolver_set_strategy(ctx.linkresolver,archive_format(ctx.archive));

	if(filter && (ctx.archive_err = archive_write_add_filter_by_name(ctx.archive,filter))) {
		log_archive_err(&ctx);
		if(ctx.archive_err < ARCHIVE_WARN)
			goto end;
	}

	if((ctx.archive_err = archive_write_set_options(ctx.archive,options))) {
		log_archive_err(&ctx);
		if(ctx.archive_err < ARCHIVE_WARN)
			goto end;
	}

	if(!strcmp(argv[1],"-")) {
		ctx.archive_err = archive_write_open_FILE(ctx.archive,stdout);
		if(ctx.print_paths) {
			fprintf(stderr,"Archiving to stdout, path printing will be disabled.\n");
			ctx.print_paths = false;
		}
	}
	else ctx.archive_err = archive_write_open_filename(ctx.archive,argv[1]);

	if(ctx.archive_err) {
		log_archive_err(&ctx);
		if(ctx.archive_err < ARCHIVE_WARN)
			goto end;
	}

	if(ctx.trim_prefix) {
		if(root_rec.type == HFS_REC_FILE)
			path = strrchr(path,'/')+1;
		else path = "";
	}

	hfstar_archive_records(&ctx,path,&root_rec);

	if(ctx.archive_err == ARCHIVE_FATAL)
		log_archive_err(&ctx);

	struct dir_hardlink_map* link,* tmplink;
	HASH_ITER(hash,ctx.dir_hardlink_map,link,tmplink) {
		HASH_DELETE(hash,ctx.dir_hardlink_map,link);
		free(link);
	}

	archive_entry_linkresolver_free(ctx.linkresolver);

end:
	if((err = hfstar_err(&ctx)) && unrecoverable_err(&ctx))
		fprintf(stderr,"Archiving aborted due to errors.\n");

	if((ctx.archive_err = archive_write_close(ctx.archive))) {
		err = err || ctx.archive_err < ARCHIVE_WARN;
		log_archive_err(&ctx);
	}

	archive_write_free(ctx.archive);

	hfslib_close_volume(ctx.vol,NULL);

	free(ctx.read_buf);
	free(ctx.vol);

	return err;
}
