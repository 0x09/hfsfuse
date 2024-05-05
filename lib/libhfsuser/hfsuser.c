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

#include "hfsuser.h"
#include "cache.h"
#include "features.h"

#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>

#include "unicode.h"

#ifdef __MINGW32__
// mignw doesn't provide these types but defines the corresponding fields in struct stat as follows
typedef short uid_t;
typedef short gid_t;
#endif

// uid_t, gid_t are only specified as any integer type and UID_MAX is only defined on some platforms, so to find out their range we just check everything
#define generic_int_max(t) _Generic((t),\
	char: CHAR_MAX,\
	signed char: SCHAR_MAX,\
	short: SHRT_MAX,\
	int: INT_MAX,\
	long: LONG_MAX,\
	long long: LLONG_MAX,\
	unsigned char: UCHAR_MAX,\
	unsigned short: USHRT_MAX,\
	unsigned int: UINT_MAX,\
	unsigned long: ULONG_MAX,\
	unsigned long long: ULLONG_MAX \
)

#ifndef UID_MAX
#define UID_MAX generic_int_max((uid_t){0})
#endif

#ifndef GID_MAX
#define GID_MAX generic_int_max((gid_t){0})
#endif

struct hfs_device {
	int fd;
	uint32_t blksize;
	struct hfs_record_cache* cache;
	char* rsrc_suff;
	size_t rsrc_len;
	uint8_t default_fork;
	uint16_t default_file_mode, default_dir_mode;
	uid_t default_uid;
	gid_t default_gid;
#ifdef HAVE_UBLIO
	bool use_ublio;
	ublio_filehandle_t ubfh;
	pthread_mutex_t ubmtx;
#endif
};

void hfs_volume_config_defaults(struct hfs_volume_config* cfg) {
	*cfg = (struct hfs_volume_config) {
		.cache_size = 1024,
		.ublio_items = 64,
		.ublio_grace = 32,
		.default_file_mode = 0755,
		.default_dir_mode = 0777
	};
}

ssize_t hfs_unistr_to_utf8(const hfs_unistr255_t* u16, char* u8) {
	int err;
	ssize_t len = utf16_to_utf8(u8,HFS_NAME_MAX,u16->unicode,u16->length,0,&err);
	if(u8)
		u8[len] = '\0';
	return err ? -err : len;
}

ssize_t hfs_utf8_to_unistr(const char* u8, hfs_unistr255_t* u16) {
	int err;
	u16->length = utf8_to_utf16(u16->unicode, 255, u8, strlen(u8), 0, &err);
	return err ? -err : u16->length;
}

ssize_t hfs_pathname_to_unix(const hfs_unistr255_t* u16, char* u8) {
	ssize_t ret = hfs_unistr_to_utf8(u16, u8);
	if(ret > 0 && u8)
		for(char* rep = u8; (rep = strchr(rep,'/')); rep++)
			*rep = ':';
	return ret;
}

#ifdef HAVE_UTF8PROC

// According to Apple Technical Q&A #QA1173,
// "HFS Plus (Mac OS Extended) uses a variant of Normal Form D in which U+2000 through U+2FFF, U+F900 through U+FAFF, and U+2F800 through U+2FAFF are not decomposed"
// However TN1150 makes no mention of the U+2xxxx range and states that Unicode 2.0 (which predates these) be strictly followed
// experiments suggest that codepoints over U+FFFF are passed through silently and do not even undergo combining character ordering
#define HFSINRANGE(codepoint) ( \
	((codepoint) >= 0x0000 && (codepoint) <= 0xFFFF) &&  \
	!(((codepoint) >= 0x2000 && (codepoint) <= 0x2FFF) ||\
	  ((codepoint) >= 0xF900 && (codepoint) <= 0xFAFF))  \
)

static inline void sort_combining_characters(utf8proc_int32_t* buf, size_t len) {
	if(len <= 1)
		return;

	utf8proc_propval_t rclass = utf8proc_get_property(buf[1])->combining_class;
	if(HFSINRANGE(buf[0]) && HFSINRANGE(buf[1]) && rclass && utf8proc_get_property(buf[0])->combining_class > rclass) {
		utf8proc_int32_t tmp = buf[0];
		buf[0] = buf[1];
		buf[1] = tmp;
	}

	for(size_t i = 1; i < len - 1; ) {
		rclass = utf8proc_get_property(buf[i+1])->combining_class;
		if(!(rclass && HFSINRANGE(buf[i+1])))
			i += 2;
		else if(HFSINRANGE(buf[i]) && utf8proc_get_property(buf[i])->combining_class > rclass) {
			utf8proc_int32_t tmp = buf[i];
			buf[i] = buf[i+1];
			buf[i+1] = tmp;
			i--;
		}
		else i++;
	}
}

static char* hfs_utf8proc_NFD(const uint8_t* u8) {
	utf8proc_int32_t codepoint,* buf;
	utf8proc_ssize_t ct, result;
	size_t len = 0;
	for(const uint8_t* it = u8; *it && (result = utf8proc_iterate(it, -1, &codepoint)) > 0; it += result) {
		if(HFSINRANGE(codepoint)) {
			if((ct = utf8proc_decompose_char(codepoint, NULL, 0, UTF8PROC_DECOMPOSE, NULL)) > 0)
				len += ct;
			else {
				len = 0;
				break;
			}
		}
		else len++;
	}

	if(!len || result < 0 || !(buf = malloc(sizeof(*buf)*len+1)))
		return NULL;

	for(utf8proc_int32_t* it = buf; *u8 && (result = utf8proc_iterate(u8, -1, &codepoint)) > 0; u8 += result)
		if(HFSINRANGE(codepoint))
			it += utf8proc_decompose_char(codepoint, it, buf+len-it, UTF8PROC_DECOMPOSE, NULL);
		else *it++ = codepoint;

	sort_combining_characters(buf, len);
	utf8proc_reencode(buf, len, UTF8PROC_STABLE);
	return (char*)buf;
}

#else
#define hfs_utf8proc_NFD(x) strdup((const char*)(x))
#endif

int hfs_pathname_from_unix(const char* u8, hfs_unistr255_t* u16) {
	char* norm = (char*)hfs_utf8proc_NFD((const uint8_t*)u8);
	if(!norm)
		return -ENOMEM;
	char* rep = norm;
	while((rep = strchr(rep,':')))
		*rep++ = '/';
	ssize_t err = hfs_utf8_to_unistr(norm,u16);
	free(norm);
	return err < 0 ? -EINVAL : 0;
}

// libhfs has `hfslib_path_elements_to_cnid` but we want to be able to use our hfs_pathname_to_unix on the individual elements
char* hfs_get_path(hfs_volume* vol, hfs_cnid_t cnid) {
	hfs_thread_record_t	parent_thread;
	hfs_unistr255_t* elements = NULL,* newelements;
	size_t nelems = 0, buflen = 1, outlen = 1;
	char* out = NULL;

	while(cnid != HFS_CNID_ROOT_FOLDER) {
		if(!(newelements = realloc(elements, sizeof(*elements) * (nelems+1))))
			goto end;
		elements = newelements;
		if(!(cnid = hfslib_find_parent_thread(vol, cnid, &parent_thread, NULL)))
			goto end;
		elements[nelems] = parent_thread.name;
		buflen += elements[nelems].length * 3 + 1;
		nelems++;
	}

	if(!(out = malloc(buflen+1)))
		goto end;

	char* it = out;
	*it++ = '/';
	hfs_unistr255_t* elem = elements+nelems;
	while(elem != elements) {
		elem--;
		size_t utf8len = hfs_pathname_to_unix(elem, it);
		if(utf8len <= 0) {
			free(out);
			out = NULL;
			goto end;
		}

		outlen += utf8len + 1;
		it += utf8len;
		*it++ = '/';
	}
	if(outlen == 1)
		outlen++;
	out[outlen-1] = '\0';

end:
	free(elements);
	return out;
}

static inline void* hfs_memdup(const void* ptr, size_t size) {
	char* p = malloc(size);
	if(!p)
		return NULL;
	return memcpy(p,ptr,size);
}

int hfs_lookup(hfs_volume* vol, const char* path, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key, uint8_t* fork) {
	struct hfs_device* dev = vol->cbdata;
	struct hfs_record_cache* cache = dev->cache;
	int ret = 0;

	if(fork)
		*fork = dev->default_fork;

	size_t pathlen = strlen(path);
	if(hfs_record_cache_lookup(cache,path,pathlen,record,key))
		return 0;

	char* pathcpy = hfs_memdup(path, pathlen+1);
	if(!pathcpy)
		return -ENOMEM;
	size_t found_pathlen = hfs_record_cache_lookup_parents(cache, pathcpy, pathlen, record, key);

	if(!found_pathlen && hfslib_find_catalog_record_with_cnid(vol,HFS_CNID_ROOT_FOLDER,record,key,NULL)) {
		ret = -ENOENT;
		goto end;
	}

	memcpy(pathcpy+found_pathlen, path+found_pathlen, pathlen+1-found_pathlen);

	// the alternate fork from the one set in default_fork can be accessed either by setting rsrc_suff,
	// in which case it takes precedence over conflicting paths, or by appending /rsrc to a filename,
	// which is never ambiguous. unfortunately FUSE libs don't really allow the latter.
	bool alt_fork_lookup = dev->rsrc_suff && dev->rsrc_len+1 < pathlen &&
	                       !memcmp(path + pathlen - dev->rsrc_len, dev->rsrc_suff, dev->rsrc_len+1);
	if(alt_fork_lookup)
		pathcpy[pathlen - dev->rsrc_len] = '\0';

	hfs_catalog_keyed_record_t inode_rec; //for resolving hard links

	// lookup normally ends when either the path is exhasuted or a file is found, however there are exactly two cases
	// where a file is permitted as part of the path: hard links to other directories, or when accessing a file's
	// resource fork via the special /rsrc suffix
	for(char* state,* pelem = strtok_r(pathcpy+found_pathlen+1,"/",&state);
	    pelem;
		pelem = strtok_r(NULL,"/",&state)) {

		hfs_unistr255_t upath;
		if((ret = hfs_pathname_from_unix(pelem,&upath)))
			goto end;

		if(!hfslib_make_catalog_key(record->folder.cnid,upath.length,upath.unicode,key)) {
			ret = -EINVAL;
			goto end;
		}

		if(hfslib_find_catalog_record_with_key(vol,key,record,NULL)) {
			ret = -ENOENT;
			goto end;
		}

		if(record->type == HFS_REC_FILE) {
			if(record->file.user_info.file_creator == HFS_MACS_CREATOR &&
			   record->file.user_info.file_type == HFS_DIR_HARD_LINK_FILE_TYPE &&
			   !hfslib_get_directory_hardlink(vol, record->file.bsd.special.inode_num, &inode_rec, NULL)) {
				// resolve directory hard links and resume path traversal
				*record = inode_rec;
				continue;
			}

			if((pelem = strtok_r(NULL,"/",&state)) && !(alt_fork_lookup = !strcmp(pelem,"rsrc"))) {
				// a file was found, but there are trailing path elements
				// only allowed in the case of filename/rsrc for alternate fork lookup
				ret = -ENOTDIR;
				goto end;
			}

			break;
		}
	}

	// resolve regular hard links
	if(record->type == HFS_REC_FILE &&
	   record->file.user_info.file_creator == HFS_HFSPLUS_CREATOR &&
	   record->file.user_info.file_type == HFS_HARD_LINK_FILE_TYPE &&
	   !hfslib_get_hardlink(vol, record->file.bsd.special.inode_num, &inode_rec, NULL))
		   *record = inode_rec;

	if(!alt_fork_lookup) // don't cache alternate fork lookups
		hfs_record_cache_add(cache,path,pathlen,record,key);
	else if(fork)
		*fork = ~*fork;

end:
	free(pathcpy);
	return ret;
}


#define HFSTIMETOSPEC(x) ((struct timespec){ .tv_sec = HFSTIMETOEPOCH(x) })

// POSIX 08 specifies values for all file modes below 07777 but leaves the following to the implementation
// so for these we translate to the system's modes from the definitions given in TN1150
#define HFS_S_IFMT 0170000

#ifndef S_IFLNK
#define S_IFLNK 0
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0
#endif
#ifndef S_IFWHT // BSD specific
#define S_IFWHT 0
#endif

#define HFS_IFMODES\
	X(S_IFIFO, 0010000)\
	X(S_IFCHR, 0020000)\
	X(S_IFDIR, 0040000)\
	X(S_IFBLK, 0060000)\
	X(S_IFREG, 0100000)\
	X(S_IFLNK, 0120000)\
	X(S_IFSOCK,0140000)\
	X(S_IFWHT, 0160000)

void hfs_stat(hfs_volume* vol, hfs_catalog_keyed_record_t* key, struct stat* st, uint8_t fork, struct hfs_decmpfs_header* decmpfs_header) {
	st->st_ino = key->file.cnid;

	struct hfs_device* dev = vol->cbdata;

	// per TN1150, in this case the mode, user, and group are treated as uninitialized and should use defaults
	if(!(key->file.bsd.file_mode & HFS_S_IFMT)) {
		if(key->type == HFS_REC_FILE) {
			st->st_mode = dev->default_file_mode | S_IFREG;
		} else {
			st->st_mode = dev->default_dir_mode | S_IFDIR;
		}
		st->st_uid = dev->default_uid;
		st->st_gid = dev->default_gid;
	} else {
		st->st_mode  = key->file.bsd.file_mode & 0xFFF;

		#define X(mode,mask) if((key->file.bsd.file_mode & mask) == mask) st->st_mode |= mode;
		HFS_IFMODES
		#undef X

		if(key->file.bsd.owner_id > UID_MAX) {
			hfslib_error("hfs_stat: owner_id %" PRIu32 " too large for CNID %" PRIu32 ", using default",NULL,0,key->file.bsd.owner_id,key->file.cnid);
			st->st_uid = dev->default_uid;
		}
		else st->st_uid = key->file.bsd.owner_id;

		if(key->file.bsd.group_id > GID_MAX) {
			hfslib_error("hfs_stat: group_id %" PRIu32 " too large for CNID %" PRIu32 ", using default",NULL,0,key->file.bsd.group_id,key->file.cnid);
			st->st_gid = dev->default_gid;
		}
		else st->st_gid = key->file.bsd.group_id;
	}

#if HAVE_STAT_FLAGS
	st->st_flags = (key->file.bsd.admin_flags << 16) | key->file.bsd.owner_flags;
#ifdef UF_HIDDEN
	//infer UF_HIDDEN from the kIsInvisible Finder flag
	if(key->file.user_info.finder_flags & 0x4000)
		st->st_flags |= UF_HIDDEN;
#endif
#endif
	if(S_ISBLK(st->st_mode) || S_ISCHR(st->st_mode))
		st->st_rdev = key->file.bsd.special.raw_device;
	else st->st_nlink = key->file.bsd.special.link_count;

	st->st_atime = HFSTIMETOEPOCH(key->file.date_accessed);
	st->st_mtime = HFSTIMETOEPOCH(key->file.date_content_mod);
	st->st_ctime = HFSTIMETOEPOCH(key->file.date_attrib_mod);
#if HAVE_BIRTHTIME
	st->st_birthtime = HFSTIMETOEPOCH(key->file.date_created);
#endif
	if(key->type == HFS_REC_FILE) {
		hfs_fork_t* f = fork == HFS_DATAFORK ? &key->file.data_fork : &key->file.rsrc_fork;
		uint64_t logical_size = decmpfs_header ? decmpfs_header->logical_size : f->logical_size;
		if(generic_int_max(st->st_size) < logical_size)
			hfslib_error("hfs_stat: logical_size %" PRIu64 " too large for CNID %" PRIu32,NULL,0,logical_size,key->file.cnid);
		else
			st->st_size = logical_size;
#if HAVE_STAT_BLOCKS
		uint64_t nblocks = f->total_blocks * (uint64_t)(vol->vh.block_size/512);
		if(generic_int_max(st->st_blocks) < nblocks)
			hfslib_error("hfs_stat: total_blocks %" PRIu64 " too large for CNID %" PRIu32,NULL,0,nblocks,key->file.cnid);
		else
			st->st_blocks = nblocks;
#endif
#if HAVE_STAT_BLKSIZE
		size_t blksize = decmpfs_header ? hfs_decmpfs_buffer_size(decmpfs_header) : vol->vh.block_size;
		if(generic_int_max(st->st_blksize) < blksize)
			hfslib_error("hfs_stat: block_size %zu too large for CNID %" PRIu32,NULL,0,blksize,key->file.cnid);
		else
			st->st_blksize = blksize;
#endif
	}
	else {
		if(generic_int_max(st->st_nlink)-2 < key->folder.valence)
			st->st_nlink = generic_int_max(st->st_nlink);
		else {
			//valence must be cast to the type of st_nlink to really guarantee no overflow here, but nlink_t is not always defined (e.g. mingw) hence separate ops
			st->st_nlink = key->folder.valence;
			st->st_nlink += 2;
		}

		st->st_size = vol->vh.block_size;
#if HAVE_STAT_BLKSIZE
		st->st_blksize = vol->vh.block_size;
#endif
	}
}

static inline char* swapcopy(char* buf, char* src, size_t size) {
	 for(size_t i = 0; i < size; i++)
		 *buf++ = src[size-i-1];
	 return buf;
}
#define SWAPCOPY(buf, src) swapcopy((char*)(buf),(char*)&(src),sizeof((src)))

void hfs_serialize_finderinfo(hfs_catalog_keyed_record_t* rec, char buf[32]) {
	memset(buf,0,32);
	if(rec->type == HFS_REC_FILE) {
		buf = SWAPCOPY(buf, rec->file.user_info.file_type);
		buf = SWAPCOPY(buf, rec->file.user_info.file_creator);
		buf = SWAPCOPY(buf, rec->file.user_info.finder_flags);
		buf = SWAPCOPY(buf, rec->file.user_info.location.v);
		buf = SWAPCOPY(buf, rec->file.user_info.location.h);
		buf += 10;
		buf = SWAPCOPY(buf, rec->file.finder_info.extended_finder_flags);
	}
	else if(rec->type == HFS_REC_FLDR) {
		buf = SWAPCOPY(buf, rec->folder.user_info.window_bounds.t);
		buf = SWAPCOPY(buf, rec->folder.user_info.window_bounds.l);
		buf = SWAPCOPY(buf, rec->folder.user_info.window_bounds.b);
		buf = SWAPCOPY(buf, rec->folder.user_info.window_bounds.r);
		buf = SWAPCOPY(buf, rec->folder.user_info.finder_flags);
		buf = SWAPCOPY(buf, rec->folder.user_info.location.v);
		buf = SWAPCOPY(buf, rec->folder.user_info.location.h);
		buf += 10;
		buf = SWAPCOPY(buf, rec->folder.finder_info.extended_finder_flags);
	}
}

#ifdef __APPLE__
#include <sys/ioctl.h>
#include <sys/disk.h>
#define DISKBLOCKSIZE DKIOCGETPHYSICALBLOCKSIZE
#define DISKIDEALSIZE DKIOCGETMAXBYTECOUNTREAD
#elif defined(__FreeBSD__)
#if _BSD_SOURCE
#include <sys/ioctl.h>
#include <sys/disk.h>
#define DISKBLOCKSIZE DIOCGSECTORSIZE
#define DISKIDEALSIZE DIOCGSTRIPESIZE
#endif
#elif defined(__linux__)
#include <sys/ioctl.h>
#include <linux/fs.h>
#define DISKBLOCKSIZE BLKBSZGET
#define DISKIDEALSIZE BLKIOOPT
#elif defined(__NetBSD__)
#include <sys/ioctl.h>
#include <sys/disk.h>
#define DISKBLOCKSIZE DIOCGSECTORSIZE
#elif defined(__OpenBSD__)
#include <sys/ioctl.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#define DISKINFO DIOCGDINFO
typedef struct disklabel diskinfo_type;
#define diskinfo_blocksize(d) (d).d_secsize
#elif defined(__DragonFly__)
#include <sys/ioctl.h>
#include <sys/diskslice.h>
#define DISKINFO DIOCGPART
typedef struct partinfo diskinfo_type;
#define diskinfo_blocksize(d) (d).media_blksize
#endif

#define BAIL(e) do { err = e; goto error; } while(0)

int hfs_open(hfs_volume* vol, const char* name, hfs_callback_args* cbargs) {
	int err = 0;

	struct hfs_device* dev = calloc(1,sizeof(*dev));
	if(!(vol->cbdata = dev))
		BAIL(ENOMEM);

	struct hfs_volume_config cfg;
	hfs_volume_config_defaults(&cfg);
	if(cbargs && cbargs->openvol)
		cfg = *(struct hfs_volume_config*)cbargs->openvol;

	if((dev->fd = open(name,O_RDONLY)) < 0)
		BAIL(errno);

	dev->default_fork = cfg.rsrc_only ? HFS_RSRCFORK : HFS_DATAFORK;
	if(cfg.rsrc_suff) {
		dev->rsrc_len = strlen(cfg.rsrc_suff);
		if(!(dev->rsrc_suff = hfs_memdup(cfg.rsrc_suff,dev->rsrc_len+1)))
			BAIL(errno);
	}

	if(cfg.blksize)
		dev->blksize = cfg.blksize;
	else {
		struct stat st;
		if(fstat(dev->fd, &st))
			BAIL(errno);
		if(S_ISCHR(st.st_mode)) {
#ifdef DISKBLOCKSIZE
#ifdef DISKIDEALSIZE
			if(ioctl(dev->fd,DISKIDEALSIZE,&dev->blksize))
				BAIL(errno);
#endif
			if(!dev->blksize && ioctl(dev->fd,DISKBLOCKSIZE,&dev->blksize))
				BAIL(errno);
#elif defined(DISKINFO)
			diskinfo_type d;
			if(ioctl(dev->fd,DISKINFO,&d))
				BAIL(errno);
			dev->blksize = diskinfo_blocksize(d);
#endif
			if(!dev->blksize)
				dev->blksize = 512;
		}
	}

	if(cfg.cache_size && !(dev->cache = hfs_record_cache_create(cfg.cache_size)))
		BAIL(ENOMEM);

	dev->default_file_mode = cfg.default_file_mode & 0777;
	dev->default_dir_mode = cfg.default_dir_mode & 0777;

	if(cfg.default_uid > UID_MAX)
		BAIL(ERANGE);
	dev->default_uid = cfg.default_uid;
	if(cfg.default_gid > GID_MAX)
		BAIL(ERANGE);
	dev->default_gid = cfg.default_gid;

#ifdef HAVE_UBLIO
	dev->use_ublio = !cfg.noublio;
	if(dev->use_ublio) {
		struct ublio_param p = {
			.up_priv = &dev->fd,
			.up_blocksize = dev->blksize,
			.up_items = cfg.ublio_items,
			.up_grace = cfg.ublio_grace,
		};
		if(!p.up_blocksize)
			p.up_blocksize = 512;
		if(!(dev->ubfh = ublio_open(&p)))
			BAIL(errno);
		int ubmtx_err = pthread_mutex_init(&dev->ubmtx,NULL);
		if(ubmtx_err) {
			ublio_close(dev->ubfh);
			dev->ubfh = NULL;
			BAIL(ubmtx_err);
		}
	}
#endif

	return 0;

error:
	hfs_close(vol,NULL);
	return -(errno = err);
}

void hfs_close(hfs_volume* vol, hfs_callback_args* cbargs) {
	struct hfs_device* dev = vol->cbdata;
	if(!dev)
		return;

	hfs_record_cache_destroy(dev->cache);
	free(dev->rsrc_suff);
#ifdef HAVE_UBLIO
	if(dev->ubfh) {
		ublio_close(dev->ubfh);
		pthread_mutex_destroy(&dev->ubmtx);
	}
#endif
	if(dev->fd >= 0)
		close(dev->fd);
	free(dev);
	vol->cbdata = NULL;
}

#ifdef HAVE_UBLIO
static inline int hfs_read_ublio(struct hfs_device* dev, void* outbytes, uint64_t length, uint64_t offset) {
	int ret = 0;
	pthread_mutex_lock(&dev->ubmtx);
	if(ublio_pread(dev->ubfh, outbytes, length, offset) < 0)
		ret = -errno;
	pthread_mutex_unlock(&dev->ubmtx);
	return ret;
}
#endif

#if HAVE_PREAD
#define hfs_pread(d,buf,nbyte,offset) pread(d,buf,nbyte,offset)
#else
static inline ssize_t hfs_pread(int d, void* buf, size_t nbyte, off_t offset) {
	static pthread_mutex_t pread_mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&pread_mutex);
	lseek(d,offset,SEEK_SET);
	ssize_t ret = read(d,buf,nbyte);
	pthread_mutex_unlock(&pread_mutex);
	return ret;
}
#endif

static inline bool hfs_preadall(int d, void* buf, size_t nbyte, off_t offset) {
	ssize_t bytesread;
	while((bytesread = hfs_pread(d,buf,nbyte,offset)) > 0) {
		buf = (char*)buf + bytesread;
		offset += bytesread;
		nbyte -= bytesread;
	}
	if(!bytesread && nbyte) {
		errno = EINVAL; // requested read beyond EOF
		return false;
	}
	return !bytesread;
}

static inline int hfs_read_pread(struct hfs_device* dev, void* outbytes, uint64_t length, uint64_t offset) {
	if(!dev->blksize)
		return hfs_preadall(dev->fd,outbytes,length,offset) ? 0 : -errno;

	char* outbuf = outbytes;
	uint32_t leading_padding = offset % dev->blksize;
	char buf[dev->blksize];
	if(leading_padding) {
		if(!hfs_preadall(dev->fd,buf,dev->blksize,offset-leading_padding))
			return -errno;
		uint32_t leading_bytes = dev->blksize - leading_padding;
		memcpy(outbuf,buf+leading_padding,min(leading_bytes,length));
		if(leading_bytes >= length)
			return 0;
		offset += leading_bytes;
		outbuf += leading_bytes;
		length -= leading_bytes;
	}
	uint32_t trailing_bytes = length % dev->blksize;
	length -= trailing_bytes;
	if(length && !hfs_preadall(dev->fd,outbuf,length,offset))
		return -errno;
	if(trailing_bytes) {
		if(!hfs_preadall(dev->fd,buf,dev->blksize,offset+length))
			return -errno;
		memcpy(outbuf+length,buf,trailing_bytes);
	}
	return 0;
}

int hfs_read(hfs_volume* vol, void* outbytes, uint64_t length, uint64_t offset, hfs_callback_args* cbargs) {
	struct hfs_device* dev = vol->cbdata;
	offset += vol->offset;
	int ret;
#ifdef HAVE_UBLIO
	if(dev->use_ublio) {
		ret = hfs_read_ublio(dev, outbytes, length, offset);
		goto end;
	}
#endif
	ret = hfs_read_pread(dev, outbytes, length, offset);

#ifdef HAVE_UBLIO
end:
#endif
	if(ret)
		hfslib_error("read of %" PRIu64 " bytes at offset %" PRIu64 " failed (block size %" PRIu32 "): %s",
		             NULL, 0, length, offset, dev->blksize, strerror(-ret));
	return ret;
}


void* hfs_malloc(size_t size, hfs_callback_args* cbargs) { return malloc(size); }
void* hfs_realloc(void* data, size_t size, hfs_callback_args* cbargs) { return size ? realloc(data,size) : NULL; }
void  hfs_free(void* data, hfs_callback_args* cbargs) { free(data); }

void  hfs_vprintf(const char* fmt, const char* file, int line, va_list args) { vfprintf(stderr,fmt,args); putc('\n',stderr); }
