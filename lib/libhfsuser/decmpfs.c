/*
 * libhfsuser - Userspace support library for NetBSD's libhfs
 * This file is part of the hfsfuse project.
 */

#include "byteorder.h"
#include "features.h"
#include "hfsuser.h"

#include <inttypes.h>
#include <errno.h>
#include <pthread.h>

// note: these values are scaled down from the full decmpfs type to account for inline/rsrc variants,
// e.g. a decmpfs_compression value of 2 corresponds to decmpfs types 3 and 4 (zlib compressed, inline or resource fork data respectively)
// see the decmpfs_compression(type) macro below
enum decmpfs_compression {
	DECMPFS_COMPRESSION_ZLIB   = 2,
	DECMPFS_COMPRESSION_SPARSE = 3,
	DECMPFS_COMPRESSION_LZVN   = 4,
	DECMPFS_COMPRESSION_LZFSE  = 6,
};

#define decmpfs_compression(type) (((type)+1)/2)

#define decmpfs_compression_zlib(type) (decmpfs_compression(type) == DECMPFS_COMPRESSION_ZLIB)
#define decmpfs_compression_lzvn(type) (decmpfs_compression(type) == DECMPFS_COMPRESSION_LZVN)
#define decmpfs_compression_lzfse(type) (decmpfs_compression(type) == DECMPFS_COMPRESSION_LZFSE)

#define decmpfs_storage_inline(type) ((type)%2)

struct hfs_decmpfs_context {
	struct hfs_decmpfs_header header;
	unsigned char* buf;
	size_t buflen;
	uint32_t (*chunk_map)[2];
	uint32_t nchunks;
	// buffered for small reads;
	uint16_t current_chunk;
	size_t current_chunk_len;
	pthread_rwlock_t current_chunk_lock;
	hfs_extent_descriptor_t* extents;
	uint16_t nextents;
};

bool hfs_decmpfs_compression_supported(uint8_t type) {
	switch(decmpfs_compression(type)) {
		case DECMPFS_COMPRESSION_ZLIB:
#if HAVE_ZLIB
			return true;
#endif
			return false;
		case DECMPFS_COMPRESSION_SPARSE:
			return decmpfs_storage_inline(type);
		case DECMPFS_COMPRESSION_LZVN:
		case DECMPFS_COMPRESSION_LZFSE:
#if HAVE_LZFSE
			return true;
#endif
			return false;
	}
	return false;
}

bool hfs_decmpfs_get_header(struct hfs_decmpfs_context* ctx, struct hfs_decmpfs_header* h) {
	if(!(ctx && h))
		return false;
	*h = ctx->header;
	return true;
}

bool hfs_decmpfs_parse_record(struct hfs_decmpfs_header* h, uint32_t length, unsigned char* data) {
	if(!data || length < 16 || memcmp(data,"fpmc",4))
		return false;
	h->type = data[4];
	memcpy(&h->logical_size,data+8,8);
	return true;
}

int hfs_decmpfs_decompress(uint8_t type, unsigned char* decompressed_buf, size_t decompressed_buf_len, unsigned char* compressed_buf, size_t compressed_buf_len, size_t* bytes_read, void* scratch_buffer) {
	if((decmpfs_compression_zlib(type) && compressed_buf[0] == 0xFF) ||
	   ((decmpfs_compression_lzfse(type) || decmpfs_compression_lzvn(type)) && compressed_buf[0] == 0x06)) {
		*bytes_read = compressed_buf_len-1;
		memcpy(decompressed_buf,compressed_buf+1,*bytes_read);
		return 0;
	}

#if HAVE_ZLIB
	if(decmpfs_compression_zlib(type)) {
		unsigned long bytes_decoded = decompressed_buf_len;
		int inflate_ret = uncompress(decompressed_buf, &bytes_decoded, compressed_buf, compressed_buf_len);
		*bytes_read = bytes_decoded;
		return inflate_ret;
	}
#endif

#if HAVE_LZVN
	if(decmpfs_compression_lzvn(type)) {
		*bytes_read = lzvn_decode(decompressed_buf, decompressed_buf_len, compressed_buf, compressed_buf_len);
		return 0;
	}
#endif

#if HAVE_LZFSE
	if(decmpfs_compression_lzfse(type)) {
		*bytes_read = lzfse_decode_buffer(decompressed_buf, decompressed_buf_len, compressed_buf, compressed_buf_len,scratch_buffer);
		return 0;
	}
#endif

	hfslib_error("invalid decmpfs type %" PRIu8 "\n",NULL,0,type);
	return 1;
}

struct hfs_decmpfs_context* hfs_decmpfs_create_context(hfs_volume* vol, hfs_cnid_t cnid, uint32_t length, unsigned char* data, int* out_err) {
	int err = 0;
	struct hfs_decmpfs_context* ctx = NULL;

	struct hfs_decmpfs_header h;
	if(!hfs_decmpfs_parse_record(&h,length,data)) {
		err = -EINVAL;
		goto err;
	}

	uint8_t compression_type = decmpfs_compression(h.type);
	// only reject entirely unknown types, allow unsupported ones since these may contain uncompressed data
	switch(compression_type) {
		case DECMPFS_COMPRESSION_ZLIB:
		case DECMPFS_COMPRESSION_SPARSE:
		case DECMPFS_COMPRESSION_LZVN:
		case DECMPFS_COMPRESSION_LZFSE:
			break;
		default:
			err = -EINVAL;
			goto err;
	}

	ctx = malloc(sizeof(*ctx));
	if(!ctx) {
		err = -ENOMEM;
		goto err;
	}
	ctx->header = h;
	ctx->buf = NULL;
	ctx->buflen = 0;
	ctx->chunk_map = NULL;
	ctx->nchunks = 0;
	ctx->current_chunk = 0;
	ctx->current_chunk_len = 0;
	ctx->extents = NULL;
	ctx->nextents = 0;

	if(pthread_rwlock_init(&ctx->current_chunk_lock,NULL)) {
		err = -errno;
		goto err;
	}

	if(compression_type == DECMPFS_COMPRESSION_SPARSE) {
		if(!decmpfs_storage_inline(ctx->header.type)) {
			err = -EINVAL;
			goto err;
		}
	}
	else if(decmpfs_storage_inline(ctx->header.type)) {
		ctx->buflen = ctx->header.logical_size;
		if(!(ctx->buf = malloc(ctx->buflen))) {
			err = -ENOMEM;
			goto err;
		}
		if(hfs_decmpfs_decompress(ctx->header.type, ctx->buf, ctx->buflen, data+16, length-16, &ctx->buflen, NULL)) {
			err = -1;
			goto err;
		}
	}
	else {
		// resource fork
		if(!(ctx->nextents = hfslib_get_file_extents(vol,cnid,HFS_RSRCFORK,&ctx->extents,NULL))) {
			err = -1;
			goto err;
		}
		if(compression_type == DECMPFS_COMPRESSION_ZLIB) {
			uint64_t bytes;
			uint32_t rsrc_start; // usually 256
			if((err = hfslib_readd_with_extents(vol,&rsrc_start,&bytes,4,0,ctx->extents,ctx->nextents,NULL)))
				goto err;
			if(bytes < 4) {
				err = -EIO;
				goto err;
			}
			rsrc_start = be32toh(rsrc_start);

			if((err = hfslib_readd_with_extents(vol,&ctx->nchunks,&bytes,4,rsrc_start+4,ctx->extents,ctx->nextents,NULL)))
				goto err;
			if(bytes < 4) {
				err = -EIO;
				goto err;
			}
			if(!(ctx->chunk_map = malloc(sizeof(*ctx->chunk_map)*ctx->nchunks))) {
				err = -ENOMEM;
				goto err;
			}
			if((err = hfslib_readd_with_extents(vol,ctx->chunk_map,&bytes,ctx->nchunks*sizeof(*ctx->chunk_map),rsrc_start+8,ctx->extents,ctx->nextents,NULL)))
				goto err;
			if(bytes < ctx->nchunks*sizeof(*ctx->chunk_map)) {
				err = -EIO;
				goto err;
			}

			// adjust offets to be relative to start block
			for(size_t i = 0; i < ctx->nchunks; i++)
				ctx->chunk_map[i][0] += rsrc_start+4;
		}
		else {
			uint64_t bytes;
			uint32_t data_start;
			if((err = hfslib_readd_with_extents(vol,&data_start,&bytes,4,0,ctx->extents,ctx->nextents,NULL)))
				goto err;
			if(bytes < 4 || data_start % sizeof(uint32_t)) {
				err = -EIO;
				goto err;
			}
			uint32_t* chunks = malloc(data_start);
			if((err = hfslib_readd_with_extents(vol,chunks,&bytes,data_start,0,ctx->extents,ctx->nextents,NULL)))
				goto err;
			if(bytes < data_start) {
				err = -EIO;
				goto err;
			}

			ctx->nchunks = data_start/4-1;
			if(!(ctx->chunk_map = malloc(sizeof(*ctx->chunk_map)*ctx->nchunks))) {
				err = -ENOMEM;
				goto err;
			}
			// normalize to the same chunk_map format as zlib compressed files
			for(size_t i = 0; i < ctx->nchunks; i++) {
				ctx->chunk_map[i][0] = chunks[i];
				ctx->chunk_map[i][1] = chunks[i+1] - chunks[i];
			}
		}
	}

	if(out_err)
		*out_err = 0;

	return ctx;

err:
	hfs_decmpfs_destroy_context(ctx);
	if(out_err)
		*out_err = err;
	return NULL;
}

void hfs_decmpfs_destroy_context(struct hfs_decmpfs_context* ctx) {
	if(!ctx)
		return;
	pthread_rwlock_destroy(&ctx->current_chunk_lock);
	free(ctx->extents);
	free(ctx->chunk_map);
	free(ctx->buf);
	free(ctx);
}

static const size_t CHUNK_SIZE = 65536;

static int decmpfs_read_rsrc(hfs_volume* vol, struct hfs_decmpfs_context* ctx, char* buf, size_t size, off_t offset) {
	int ret = 0;
	if((uint64_t)offset > ctx->header.logical_size)
		return 0;

	size = min(size,ctx->header.logical_size-offset);

	size_t bytes_written = 0;

	size_t chunk_start = offset/CHUNK_SIZE,
		   chunk_end = chunk_start + size/CHUNK_SIZE + (offset%CHUNK_SIZE + size%CHUNK_SIZE + (CHUNK_SIZE-1))/CHUNK_SIZE; // (offset+size+65535)/65536 with no overflow
	chunk_start = min(chunk_start,ctx->nchunks);
	chunk_end = min(chunk_end,ctx->nchunks);

	size_t decompressed_buf_len = min(ctx->header.logical_size,CHUNK_SIZE);

	unsigned char* compressed_buf = NULL;

	for(size_t i = chunk_start; i < chunk_end && bytes_written < size; i++) {
		uint32_t chunk_len = ctx->chunk_map[i][1],
		      chunk_offset = ctx->chunk_map[i][0];

		size_t bytes_read = 0;
		pthread_rwlock_rdlock(&ctx->current_chunk_lock);
		if(!ctx->buf || ctx->current_chunk != i) {
			pthread_rwlock_unlock(&ctx->current_chunk_lock);
			pthread_rwlock_wrlock(&ctx->current_chunk_lock);
			if(!ctx->buf) {
				if(!(ctx->buf = malloc(decompressed_buf_len))) {
					ret = -ENOMEM;
					break;
				}
				ctx->buflen = decompressed_buf_len;
			}

			if(!compressed_buf) {
				uint32_t max_chunk_len = ctx->chunk_map[i][1];
				for(size_t j = chunk_start+1; j < chunk_end; j++)
					if(max_chunk_len < ctx->chunk_map[j][1])
						max_chunk_len = ctx->chunk_map[j][1];
				if(!(compressed_buf = malloc(max_chunk_len))) {
					ret = -ENOMEM;
					break;
				}
			}

			uint64_t compressed_bytes_read;
			hfslib_readd_with_extents(vol,compressed_buf,&compressed_bytes_read,chunk_len,chunk_offset,ctx->extents,ctx->nextents,NULL);
			if((ret = hfs_decmpfs_decompress(ctx->header.type, ctx->buf, ctx->buflen, compressed_buf, compressed_bytes_read, &bytes_read, NULL)))
				break;
			ctx->current_chunk = i;
			ctx->current_chunk_len = bytes_read;
		}
		else bytes_read = ctx->current_chunk_len;

		size_t decode_offset = i > chunk_start ? 0 : offset%CHUNK_SIZE;
		if(decode_offset < bytes_read) {
			size_t writesize = min(bytes_read-decode_offset,size-bytes_written);
			memcpy(buf+bytes_written,ctx->buf+decode_offset,writesize);
			bytes_written += writesize;
		}
		pthread_rwlock_unlock(&ctx->current_chunk_lock);
	}
	free(compressed_buf);
	if(ret) {
		pthread_rwlock_unlock(&ctx->current_chunk_lock);
		if(ret < 0)
			return ret;
	}
	return bytes_written;
}

int hfs_decmpfs_read(hfs_volume* vol, struct hfs_decmpfs_context* ctx, char* buf, size_t size, off_t offset) {
	if(offset < 0)
		return -EINVAL;

	if(decmpfs_compression(ctx->header.type) == DECMPFS_COMPRESSION_SPARSE) {
		if((uint64_t)offset >= ctx->header.logical_size)
			return 0;
		size_t bytes = min(size,ctx->header.logical_size-offset);
		memset(buf,0,bytes);
		return bytes;
	}

	if(!decmpfs_storage_inline(ctx->header.type))
		return decmpfs_read_rsrc(vol,ctx,buf,size,offset);

	if(ctx->buf && (uint64_t)offset < ctx->buflen) {
		size_t bytes = min(size,ctx->buflen-offset);
		memcpy(buf,ctx->buf+offset,bytes);
		return bytes;
	}

	return 0;
}

size_t hfs_decmpfs_buffer_size(struct hfs_decmpfs_header* h) {
	if(!h)
		return 0;
	return decmpfs_storage_inline(h->type) ? h->logical_size : min(h->logical_size,CHUNK_SIZE);
}

int hfs_decmpfs_lookup(hfs_volume* vol, hfs_file_record_t* file, struct hfs_decmpfs_header* h, uint32_t* length, unsigned char** data) {
	if(data)
		*data = NULL;
	if(length)
		*length = 0;

	if(!(file->bsd.owner_flags & HFS_UF_COMPRESSED) || file->data_fork.logical_size)
		return 1;

	hfs_attribute_key_t attrkey;
	hfslib_make_attribute_key(file->cnid,0,strlen("com.apple.decmpfs"),u"com.apple.decmpfs",&attrkey);
	hfs_attribute_record_t attr;
	unsigned char* buf = NULL;
	if(hfslib_find_attribute_record_with_key(vol,&attrkey,&attr,(void*)&buf,NULL))
		return 1;

	// if this is a zlib or lzfse compressed file and hfsfuse wasn't built with these libraries, continue to treat it as a zero-length file
	// the com.apple.decmpfs xattr may still be inspected directly to access the compressed data
	if(attr.type != HFS_ATTR_INLINE_DATA || !hfs_decmpfs_parse_record(h,attr.inline_record.length,buf)) {
		free(buf);
		return -EINVAL;
	}
	if(!hfs_decmpfs_compression_supported(h->type)) {
		free(buf);
		hfslib_error("unsupported decmpfs type %" PRIu8 " for cnid %" PRIu32 "\n",NULL,0,h->type,file->cnid);
		return -ENOTSUP;
	}

	if(length)
		*length = attr.inline_record.length;
	if(data)
		*data = buf;
	else
		free(buf);

	return 0;
}
