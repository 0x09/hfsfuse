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

#include "libhfsuser/hfsuser.h"
#include "libhfsuser/cache.h"
#include "libhfsuser/features.h"

#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/ioctl.h>

#include "libhfs/unicode.h"

struct hfs_device {
	int fd;
	uint32_t blksize;
	struct hfs_record_cache* cache;
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
	};
}

ssize_t hfs_unistr_to_utf8(const hfs_unistr255_t* u16, char u8[512]) {
	int err;
	ssize_t len = utf16_to_utf8(u8,512-1,u16->unicode,u16->length,0,&err);
	if(u8)
		u8[len] = '\0';
	return err ? -err : len;
}

ssize_t hfs_pathname_to_unix(const hfs_unistr255_t* u16, char u8[512]) {
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

	if(result < 0 || !(len && (buf = malloc(sizeof(*buf)*len+1))))
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

ssize_t hfs_pathname_from_unix(const char* u8, hfs_unistr255_t* u16) {
	char* norm = (char*)hfs_utf8proc_NFD((const uint8_t*)u8);
	if(!norm)
		return -ENOMEM;
	char* rep = norm;
	while((rep = strchr(rep,':')))
		*rep++ = '/';
	int err;
	u16->length = utf8_to_utf16(u16->unicode,255,norm,strlen(norm),0,&err);
	free(norm);
	return err ? err : u16->length;
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
		buflen += elements[nelems].length * 2 + 1;
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

int hfs_lookup(hfs_volume* vol, const char* path, hfs_catalog_keyed_record_t* record, hfs_catalog_key_t* key, uint8_t* fork) {
#define RET(val) do{ free(splitpath); return -val; } while(0)
	struct hfs_record_cache* cache = ((struct hfs_device*)vol->cbdata)->cache;
	if(fork) *fork = HFS_DATAFORK;
	if(hfs_record_cache_lookup(cache,path,record,key))
		return 0;

	size_t found_pathlen = hfs_record_cache_lookup_parents(cache, strdup(path), record, key);
	if(!found_pathlen && hfslib_find_catalog_record_with_cnid(vol,HFS_CNID_ROOT_FOLDER,record,key,NULL)) return -7;

	int ret;
	hfs_unistr255_t upath;
	char* splitpath = strdup(path);
	char* splitptr  = splitpath + found_pathlen + 1;
	char* pelem;
	while(record->type == HFS_REC_FLDR && (pelem = strsep(&splitptr,"/")) && *pelem) {
		if(hfs_pathname_from_unix(pelem,&upath) < 0) RET(3);
		if(!hfslib_make_catalog_key(record->folder.cnid,upath.length,upath.unicode,key)) RET(2);
		if((ret = hfslib_find_catalog_record_with_key(vol,key,record,NULL))) RET(ret);
		if(record->type == HFS_REC_FILE &&
		   record->file.user_info.file_creator == HFS_MACS_CREATOR && record->file.user_info.file_type == HFS_DIR_HARD_LINK_FILE_TYPE &&
		   hfslib_get_directory_hardlink(vol, record->file.bsd.special.inode_num, record, NULL))
			RET(7);
	}
	if(splitptr) {
		if(record->type != HFS_REC_FILE || strcmp(splitptr,"rsrc")) RET(5);
		else if(fork) *fork = HFS_RSRCFORK;
	}
	free(splitpath);
	if(record->type == HFS_REC_FILE &&
	   record->file.user_info.file_creator == HFS_HFSPLUS_CREATOR && record->file.user_info.file_type == HFS_HARD_LINK_FILE_TYPE &&
	   hfslib_get_hardlink(vol, record->file.bsd.special.inode_num, record, NULL))
		return -6;
	if(!splitptr) // don't cache rsrc lookups
		hfs_record_cache_add(cache,path,record,key);
	return 0;
#undef RET
}


#define HFSTIMETOSPEC(x) ((struct timespec){ .tv_sec = HFSTIMETOEPOCH(x) })

void hfs_stat(hfs_volume* vol, hfs_catalog_keyed_record_t* key, struct stat* st, uint8_t fork) {
	st->st_mode  = key->file.bsd.file_mode;
	st->st_ino   = key->file.cnid;
	st->st_uid   = key->file.bsd.owner_id;
	st->st_gid   = key->file.bsd.group_id;
#ifndef __linux__
	st->st_flags = (key->file.bsd.admin_flags << 16) | key->file.bsd.owner_flags;
#endif
	if(S_ISBLK(st->st_mode) || S_ISCHR(st->st_mode))
		st->st_rdev  = key->file.bsd.special.raw_device;
	else st->st_nlink = key->file.bsd.special.link_count;

	st->st_atime     = HFSTIMETOEPOCH(key->file.date_accessed);
	st->st_mtime     = HFSTIMETOEPOCH(key->file.date_content_mod);
	st->st_ctime     = HFSTIMETOEPOCH(key->file.date_attrib_mod);
#if HAVE_BIRTHTIME
	st->st_birthtime = HFSTIMETOEPOCH(key->file.date_created);
#endif
	if(key->type == HFS_REC_FILE) {
		hfs_fork_t* f = fork == HFS_DATAFORK ? &key->file.data_fork : &key->file.rsrc_fork;
		st->st_size    = f->logical_size;
		st->st_blocks  = f->total_blocks;
		st->st_blksize = f->clump_size;
	}
	else {
		st->st_nlink = key->folder.valence + 2;
		st->st_size    = vol->vh.block_size;
		st->st_blksize = vol->vh.block_size;
	}
}

static inline char* swapcopy(char* buf, char* src, size_t size) {
	 for(size_t i = 0; i < size; i++)
		 *buf++ = src[size-i-1];
	 return buf;
}
#define SWAPCOPY(buf, src) swapcopy((char*)(buf),(char*)&(src),sizeof((src)))

void hfs_serialize_finderinfo(hfs_catalog_keyed_record_t* rec, char buf[32]) {
	if(rec->type == HFS_REC_FILE) {
		buf = SWAPCOPY(buf, rec->file.user_info.file_type);
		buf = SWAPCOPY(buf, rec->file.user_info.file_creator);
		buf = SWAPCOPY(buf, rec->file.user_info.finder_flags);
		buf = SWAPCOPY(buf, rec->file.user_info.location.v);
		buf = SWAPCOPY(buf, rec->file.user_info.location.h);
		buf = SWAPCOPY(buf, rec->file.user_info.reserved);

		for(int i = 0; i < 4; i++)
			buf = SWAPCOPY(buf, rec->file.finder_info.reserved[i]);
		buf = SWAPCOPY(buf, rec->file.finder_info.extended_finder_flags);
		buf = SWAPCOPY(buf, rec->file.finder_info.reserved2);
		buf = SWAPCOPY(buf, rec->file.finder_info.put_away_folder_cnid);
	}
	else if(rec->type == HFS_REC_FLDR) {
		buf = SWAPCOPY(buf, rec->folder.user_info.window_bounds.t);
		buf = SWAPCOPY(buf, rec->folder.user_info.window_bounds.l);
		buf = SWAPCOPY(buf, rec->folder.user_info.window_bounds.b);
		buf = SWAPCOPY(buf, rec->folder.user_info.window_bounds.r);

		buf = SWAPCOPY(buf, rec->folder.user_info.finder_flags);
		buf = SWAPCOPY(buf, rec->folder.user_info.location.v);
		buf = SWAPCOPY(buf, rec->folder.user_info.location.h);
		buf = SWAPCOPY(buf, rec->folder.user_info.reserved);

		buf = SWAPCOPY(buf, rec->folder.finder_info.scroll_position.v);
		buf = SWAPCOPY(buf, rec->folder.finder_info.scroll_position.h);
		buf = SWAPCOPY(buf, rec->folder.finder_info.reserved);
		buf = SWAPCOPY(buf, rec->folder.finder_info.extended_finder_flags);
		buf = SWAPCOPY(buf, rec->folder.finder_info.reserved2);
		buf = SWAPCOPY(buf, rec->folder.finder_info.put_away_folder_cnid);
	}
}

#ifdef __APPLE__
#include <sys/disk.h>
#define DISKBLOCKSIZE DKIOCGETPHYSICALBLOCKSIZE
#define DISKIDEALSIZE DKIOCGETMAXBYTECOUNTREAD
#elif defined(__FreeBSD__)
#include <sys/disk.h>
#define DISKBLOCKSIZE DIOCGSECTORSIZE
#define DISKIDEALSIZE DIOCGSTRIPESIZE
#elif defined(__linux__)
#include <linux/fs.h>
#define DISKBLOCKSIZE BLKBSZGET
#define DISKIDEALSIZE BLKIOOPT
#endif

#define BAIL(e) do { errno = e; goto error; } while(0)

int hfs_open(hfs_volume* vol, const char* name, hfs_callback_args* cbargs) {
	struct hfs_device* dev = calloc(1,sizeof(*dev));
	if(!dev)
		return -(errno = ENOMEM);

	struct hfs_volume_config cfg;
	hfs_volume_config_defaults(&cfg);
	if(cbargs && cbargs->openvol)
		cfg = *(struct hfs_volume_config*)cbargs->openvol;

	if((dev->fd = open(name,O_RDONLY)) < 0)
		BAIL(errno);

	if(cfg.blksize)
		dev->blksize = cfg.blksize;
	else if(cfg.noublio) {
		struct stat st;
		if(fstat(dev->fd, &st))
			BAIL(errno);
		if(S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
#ifdef DISKBLOCKSIZE
			if(ioctl(dev->fd,DISKIDEALSIZE,&dev->blksize))
				BAIL(errno);
			if(!dev->blksize && ioctl(dev->fd,DISKBLOCKSIZE,&dev->blksize))
				BAIL(errno);
#endif
		}
		else if(S_ISREG(st.st_mode))
			dev->blksize = st.st_blksize;
		else BAIL(EINVAL);
	}

	if(!dev->blksize)
		dev->blksize=512;

	if(cfg.cache_size && !(dev->cache = hfs_record_cache_create(cfg.cache_size)))
		BAIL(ENOMEM);

#ifdef HAVE_UBLIO
	dev->use_ublio = !cfg.noublio;
	if(dev->use_ublio) {
		struct ublio_param p = {
			.up_priv = &dev->fd,
			.up_blocksize = dev->blksize,
			.up_items = cfg.ublio_items,
			.up_grace = cfg.ublio_grace,
		};
		if(!(dev->ubfh = ublio_open(&p)))
			BAIL(errno);
		if((errno = pthread_mutex_init(&dev->ubmtx,NULL)))
			BAIL(errno);
	}
#endif

	vol->cbdata = dev;
	return 0;

error:
	if(dev->fd >= 0)
		close(dev->fd);
	hfs_record_cache_destroy(dev->cache);
#ifdef HAVE_UBLIO
	if(dev->ubfh)
		ublio_close(dev->ubfh);
#endif
	free(dev);
	return -errno;
}

void hfs_close(hfs_volume* vol, hfs_callback_args* cbargs) {
	struct hfs_device* dev = vol->cbdata;
	hfs_record_cache_destroy(dev->cache);
#ifdef HAVE_UBLIO
	if(dev->use_ublio) {
		ublio_close(dev->ubfh);
		pthread_mutex_destroy(&dev->ubmtx);
	}
#endif
	close(dev->fd);
	free(dev);
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

static inline int hfs_read_pread(struct hfs_device* dev, void* outbytes, uint64_t length, uint64_t offset) {
	char* outbuf = outbytes;
	ssize_t ret = 0;
	uint64_t rem = length % dev->blksize;
	length -= rem;
	while(length && (ret = pread(dev->fd,outbuf,length,offset)) > 0) {
		if((ret = min(length,ret)) <= 0)
			break;
		outbuf += ret;
		offset += ret;
		length -= ret;
	}
	if(ret < 0)
		return -errno;
	if(rem) {
		char buf[dev->blksize];
		ret = pread(dev->fd,buf,dev->blksize,offset);
		if((ret = min(rem,ret)) > 0)
			memcpy(outbuf,buf,ret);
	}
	if(ret < 0)
		return -errno;
	return 0;
}

int hfs_read(hfs_volume* vol, void* outbytes, uint64_t length, uint64_t offset, hfs_callback_args* cbargs) {
	struct hfs_device* dev = vol->cbdata;
	offset += vol->offset;
#ifdef HAVE_UBLIO
	if(dev->use_ublio)
		return hfs_read_ublio(dev, outbytes, length, offset);
#endif
	return hfs_read_pread(dev, outbytes, length, offset);
}


void* hfs_malloc(size_t size, hfs_callback_args* cbargs) { return malloc(size); }
void* hfs_realloc(void* data, size_t size, hfs_callback_args* cbargs) { return size ? realloc(data,size) : NULL; }
void  hfs_free(void* data, hfs_callback_args* cbargs) { free(data); }

void  hfs_vprintf(const char* fmt, const char* file, int line, va_list args) { vfprintf(stderr,fmt,args); putc('\n',stderr); }
void  hfs_vsyslog(const char* fmt, const char* file, int line, va_list args) { vsyslog(LOG_ERR,fmt,args); }

