/*	$NetBSD: libhfs.c,v 1.14.18.1 2019/06/10 22:09:00 christos Exp $	*/

/*-
 * Copyright (c) 2005, 2007 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Yevgeny Binder, Dieter Baron, and Pelle Johansson.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */                                     

/*
 *  All functions and variable types have the prefix "hfs_". All constants
 *  have the prefix "HFS_".
 *
 *  Naming convention for functions which read/write raw, linear data
 *	into/from a structured form:
 *
 *  hfs_read/write[d][a]_foo_bar
 *      [d] - read/write from/to [d]isk instead of a memory buffer
 *      [a] - [a]llocate output buffer instead of using an existing one
 *            (not applicable for writing functions)
 *
 *  Most functions do not have either of these options, so they will read from
 *	or write to a memory buffer, which has been previously allocated by the
 *	caller.
 */

#include "rcsid.h"
__KERNEL_RCSID(0, "$NetBSD: libhfs.c,v 1.14.18.1 2019/06/10 22:09:00 christos Exp $");

#include "libhfs.h"
#include "byteorder.h"

#include <inttypes.h>

const char* hfslib_get_rcsid() { return hfs_rcsid; }

/* global private file/folder keys */
hfs_catalog_key_t hfs_gMetadataDirectoryKey; /* contains HFS+ inodes */
hfs_catalog_key_t hfs_gJournalInfoBlockFileKey;
hfs_catalog_key_t hfs_gJournalBufferFileKey;
hfs_catalog_key_t hfs_gDirMetadataDirectoryKey;
hfs_catalog_key_t* hfs_gPrivateObjectKeys[5] = {
	&hfs_gMetadataDirectoryKey,
	&hfs_gJournalInfoBlockFileKey,
	&hfs_gJournalBufferFileKey,
	&hfs_gDirMetadataDirectoryKey,
	NULL
};


extern uint16_t be16tohp(void** inout_ptr);
extern uint32_t be32tohp(void** inout_ptr);
extern uint64_t be64tohp(void** inout_ptr);

hfs_callbacks	hfs_gcb;	/* global callbacks */
 
#ifdef DLO_DEBUG
#include <stdio.h>
#include <ctype.h>
void
dlo_print_key(hfs_catalog_key_t *key)
{
	int i;
	
	printf("%ld:[", (long)key->parent_cnid);
	for (i=0; i<key->name.length; i++) {
		if (key->name.unicode[i] < 256
		    && isprint(key->name.unicode[i]))
			putchar(key->name.unicode[i]);
		else
			printf("<%04x>", key->name.unicode[i]);
	}		    
	printf("]");
}
#endif

void
hfslib_init(hfs_callbacks* in_callbacks)
{
	unichar_t	temp[256];

	memset(&hfs_gcb, 0, sizeof(hfs_callbacks));
	if (in_callbacks != NULL)
		memcpy(&hfs_gcb, in_callbacks, sizeof(hfs_callbacks));

	/*
	 * Create keys for the HFS+ "private" files so we can reuse them whenever
	 * we perform a user-visible operation, such as listing directory contents.
	 */

#define ATOU(str, len) /* quick & dirty ascii-to-unicode conversion */ \
	do{ int i; for(i=0; i<len; i++) temp[i]=str[i]; } \
	while( /*CONSTCOND*/ 0)

	ATOU("\0\0\0\0HFS+ Private Data", 21);
	hfslib_make_catalog_key(HFS_CNID_ROOT_FOLDER, 21, temp, 
		&hfs_gMetadataDirectoryKey);

	ATOU(".journal_info_block", 19);
	hfslib_make_catalog_key(HFS_CNID_ROOT_FOLDER, 19, temp, 
		&hfs_gJournalInfoBlockFileKey);

	ATOU(".journal", 8);
	hfslib_make_catalog_key(HFS_CNID_ROOT_FOLDER, 8, temp, 
		&hfs_gJournalBufferFileKey);

	ATOU(".HFS+ Private Directory Data\xd",29);
	hfslib_make_catalog_key(HFS_CNID_ROOT_FOLDER, 29, temp,
		&hfs_gDirMetadataDirectoryKey);

#undef ATOU
}

void
hfslib_done(void)
{
	/*
	 * no-op previously responsible for freeing the global case folding table
	 * retained for compatibility and any possible future teardown operations
	 */
	return;
}

void
hfslib_init_cbargs(hfs_callback_args* ptr)
{
	memset(ptr, 0, sizeof(hfs_callback_args));
}

hfs_callbacks*
hfslib_callbacks(void)
{
	return &hfs_gcb;
}

#if 0
#pragma mark -
#pragma mark High-Level Routines
#endif

int
hfslib_open_volume(
	const char* in_device,
	int in_readonly,
	hfs_volume* out_vol,
	hfs_callback_args* cbargs)
{
	hfs_catalog_key_t		rootkey;
	hfs_thread_record_t	rootthread;
	hfs_hfs_master_directory_block_t mdb;
	uint16_t	node_rec_sizes[1];
	void*		node_recs[1];
	void*		buffer;
	void*		buffer2;	/* used as temporary pointer for realloc() */
	int			result;
	int		isopen = 0;
	
	result = 1;
	buffer = NULL;

	if (in_device == NULL || out_vol == NULL)
		return 1;

	out_vol->readonly = in_readonly;
	out_vol->offset = 0;

	if (hfslib_openvoldevice(out_vol, in_device, cbargs) != 0)
		HFS_LIBERR("could not open device");
	isopen = 1;

	/*
	 * Read the volume header.
	 */
	buffer = hfslib_malloc(max(sizeof(hfs_volume_header_t),
		sizeof(hfs_hfs_master_directory_block_t)), cbargs);
	if (buffer == NULL)
		HFS_LIBERR("could not allocate volume header");
	if (hfslib_readd(out_vol, buffer, max(sizeof(hfs_volume_header_t),
	    sizeof(hfs_hfs_master_directory_block_t)),
	    HFS_VOLUME_HEAD_RESERVE_SIZE, cbargs) != 0)
		HFS_LIBERR("could not read volume header");

	if (be16toh(*((uint16_t *)buffer)) == HFS_SIG_HFS) {
		if (hfslib_read_master_directory_block(buffer, &mdb) == 0)
			HFS_LIBERR("could not parse master directory block");
		if (mdb.embedded_signature == HFS_SIG_HFSP) {
			/* XXX: is 512 always correct? */
			out_vol->offset =
			    mdb.first_block * 512
			    + mdb.embedded_extent.start_block
			    * (uint64_t)mdb.block_size;

			if (hfslib_readd(out_vol, buffer,
			    sizeof(hfs_volume_header_t),
			    HFS_VOLUME_HEAD_RESERVE_SIZE, cbargs) != 0)
				HFS_LIBERR("could not read volume header");
		} else
			HFS_LIBERR("Plain HFS volumes not currently supported");
	}

	if (hfslib_read_volume_header(buffer, &(out_vol->vh)) == 0)
		HFS_LIBERR("could not parse volume header");

	/*
	 * Check the volume signature to see if this is a legitimate HFS+ or HFSX
	 * volume. If so, set the key comparison function pointers appropriately.
	 */
	switch(out_vol->vh.signature) {
		case HFS_SIG_HFSP:
			out_vol->keycmp = hfslib_compare_catalog_keys_cf;
			break;
		case HFS_SIG_HFSX:
			out_vol->keycmp = NULL; /* will be set below */
			break;
		default:
			HFS_LIBERR("unrecognized volume format");
			goto error;
			break;
	}

	/*
	 * Read the catalog header.
	 */
	buffer2 = hfslib_realloc(buffer, 512, cbargs);
	if (buffer2 == NULL)
		HFS_LIBERR("could not allocate catalog header node");
	buffer = buffer2;

	/*
	 * We are only interested in the node header, so read the first
	 * 512 bytes and construct the node descriptor by hand.
	 */
	if (hfslib_readd(out_vol, buffer, 512,
	    out_vol->vh.catalog_file.extents[0].start_block *
	    (uint64_t)out_vol->vh.block_size, cbargs) != 0)
		HFS_LIBERR("could not read catalog header node");
	node_recs[0] = (char *)buffer+14;
	node_rec_sizes[0] = 120;
	if (hfslib_read_header_node(node_recs, node_rec_sizes, 1,
	    &out_vol->chr, NULL, NULL) == 0)
		HFS_LIBERR("could not parse catalog header node");

	/*
	 * If this is an HFSX volume, the catalog header specifies the type of
	 * key comparison method (case-folding or binary compare) we should
	 * use.
	 */
	if (out_vol->keycmp == NULL) {
		if (out_vol->chr.keycomp_type == HFS_KEY_CASEFOLD)
			out_vol->keycmp = hfslib_compare_catalog_keys_cf;
		else if (out_vol->chr.keycomp_type == HFS_KEY_BINARY)
			out_vol->keycmp = hfslib_compare_catalog_keys_bc;
		else
			HFS_LIBERR("undefined key compare method");
	}

	out_vol->catkeysizefieldsize
	    = (out_vol->chr.attributes & HFS_BIG_KEYS_MASK) ?
	    sizeof(uint16_t) : sizeof(uint8_t);

	/*
	 * Read the extent overflow header.
	 */
	/*
	 * We are only interested in the node header, so read the first
	 * 512 bytes and construct the node descriptor by hand.
	 * buffer is already 512 bytes long.
	 */
	if (hfslib_readd(out_vol, buffer, 512,
	    out_vol->vh.extents_file.extents[0].start_block *
	    (uint64_t)out_vol->vh.block_size, cbargs) != 0)
		HFS_LIBERR("could not read extent header node");

	node_recs[0] = (char *)buffer+14;
	node_rec_sizes[0] = 120;
	if (hfslib_read_header_node(node_recs, node_rec_sizes, 1,
	    &out_vol->ehr, NULL, NULL) == 0)
		HFS_LIBERR("could not parse extent header node");
	out_vol->extkeysizefieldsize
	    = (out_vol->ehr.attributes & HFS_BIG_KEYS_MASK) ?
	    sizeof(uint16_t):sizeof(uint8_t);
	/*
	 * Read the journal info block and journal header (if volume journaled).
	 */
	if (out_vol->vh.attributes & (1<<HFS_VOL_JOURNALED)) {
		/* journal info block */
		buffer2 = hfslib_realloc(buffer, sizeof(hfs_journal_info_t), cbargs);
		if (buffer2 == NULL)
			HFS_LIBERR("could not allocate journal info block");
		buffer = buffer2;

		if (hfslib_readd(out_vol, buffer, sizeof(hfs_journal_info_t),
		    out_vol->vh.journal_info_block * out_vol->vh.block_size,
		    cbargs) != 0)
			HFS_LIBERR("could not read journal info block");

		if (hfslib_read_journal_info(buffer, &out_vol->jib) == 0)
			HFS_LIBERR("could not parse journal info block");

		/* journal header */
		buffer2 = hfslib_realloc(buffer, sizeof(hfs_journal_header_t), cbargs);
		if (buffer2 == NULL)
			HFS_LIBERR("could not allocate journal header");
		buffer = buffer2;

		if (hfslib_readd(out_vol, buffer, sizeof(hfs_journal_header_t),
		    out_vol->jib.offset, cbargs) != 0)
			HFS_LIBERR("could not read journal header");

		if (hfslib_read_journal_header(buffer, &out_vol->jh) == 0)
			HFS_LIBERR("could not parse journal header");

		out_vol->journaled = 1;
	} else {
		out_vol->journaled = 0;
	}

	result = 0;
		
	/*
	 * Find and store the volume name.
	 */	
	if (hfslib_make_catalog_key(HFS_CNID_ROOT_FOLDER, 0, NULL, &rootkey) == 0)
		HFS_LIBERR("could not make root search key");

	if (hfslib_find_catalog_record_with_key(out_vol, &rootkey,
	    (hfs_catalog_keyed_record_t*)&rootthread, cbargs)!=0)
		HFS_LIBERR("could not find root parent");

	memcpy(&out_vol->name, &rootthread.name, sizeof(hfs_unistr255_t));

	/* FALLTHROUGH */
error:	
	if (result != 0 && isopen)
		hfslib_close_volume(out_vol, cbargs);
	if (buffer != NULL)
		hfslib_free(buffer, cbargs);
	return result;
}

void
hfslib_close_volume(hfs_volume* in_vol, hfs_callback_args* cbargs)
{
	if (in_vol == NULL)
		return;
	hfslib_closevoldevice(in_vol, cbargs);
}

int
hfslib_path_to_cnid(hfs_volume* in_vol,
	hfs_cnid_t in_cnid,
	char** out_unicode,
	uint16_t* out_length,
	hfs_callback_args* cbargs)
{
	hfs_thread_record_t	parent_thread;
	hfs_cnid_t	parent_cnid, child_cnid;
	char*		newpath;
	char*		path;
	int			path_offset = 0;
	int			result;
	uint16_t*	ptr;	/* dummy var */
	uint16_t	uchar;	/* dummy var */
	uint16_t	total_path_length;
		
	if (in_vol == NULL || in_cnid == 0 || out_unicode == NULL ||
	    out_length == NULL)
		return 1;

	result = 1;
	*out_unicode = NULL;
	*out_length = 0;
	path = NULL;
	total_path_length = 0;

	path = hfslib_malloc(514, cbargs); /* 256 unichars plus a forward slash */
	if (path == NULL)
		return 1;

	child_cnid = in_cnid;
	parent_cnid = child_cnid; /* skips loop in case in_cnid is root id */
	while (parent_cnid != HFS_CNID_ROOT_FOLDER &&
	    parent_cnid != HFS_CNID_ROOT_PARENT)
	{
		if (child_cnid != in_cnid) {
			newpath = hfslib_realloc(path, 514 + total_path_length*2, cbargs);
			if (newpath == NULL)
				goto exit;
			path = newpath;
			memmove(path + 514, path + path_offset, total_path_length*2);
		}

		parent_cnid = hfslib_find_parent_thread(in_vol, child_cnid,
		    &parent_thread, cbargs);
		if (parent_cnid == 0)
			goto exit;

		path_offset = 512 - parent_thread.name.length*2;

		memcpy(path + path_offset, parent_thread.name.unicode,
			parent_thread.name.length*2);

		/* Add a forward slash. The unicode string was specified in big endian
		 * format, so convert to core format if necessary. */
		path[512] = 0x00;
		path[513] = 0x2F;
		
		ptr = (uint16_t*)path + 256;
		uchar = be16tohp((void*)&ptr);
		*(ptr-1) = uchar;

		total_path_length += parent_thread.name.length + 1;
		child_cnid = parent_cnid;
	}

	/*
	 * At this point, 'path' holds a sequence of unicode characters which
	 * represent the absolute path to the given cnid. This string is missing
	 * a terminating null char and an initial forward slash that represents
	 * the root of the filesystem. It most likely also has extra space in
	 * the beginning, due to the fact that we reserve 512 bytes for each path
	 * component and won't usually use all that space. So, we allocate the
	 * final string based on the actual length of the absolute path, plus four
	 * additional bytes (two unichars) for the forward slash and the null char.
	 */

	*out_unicode = hfslib_malloc((total_path_length+2)*2, cbargs);
	if (*out_unicode == NULL)
		goto exit;

	/* copy only the bytes that are actually used */
	memcpy(*out_unicode + 2, path + path_offset, total_path_length*2);

	/* insert forward slash at start */
	uchar = be16toh(0x2F);
	memcpy(*out_unicode, &uchar, sizeof(uchar));

	/* insert null char at end */
	(*out_unicode)[total_path_length*2+2] = 0x00;
	(*out_unicode)[total_path_length*2+3] = 0x00;

	*out_length = total_path_length + 1 /* extra for forward slash */ ;

	result = 0;

exit:
	if (path != NULL)
		hfslib_free(path, cbargs);
	return result;
}

hfs_cnid_t
hfslib_find_parent_thread(
	hfs_volume* in_vol,
	hfs_cnid_t in_child,
	hfs_thread_record_t* out_thread,
	hfs_callback_args* cbargs)
{	
	hfs_catalog_key_t	childkey;

	if (in_vol == NULL || in_child == 0 || out_thread == NULL)
		return 0;

	if (hfslib_make_catalog_key(in_child, 0, NULL, &childkey) == 0)
		return 0;

	if (hfslib_find_catalog_record_with_key(in_vol, &childkey,
		(hfs_catalog_keyed_record_t*)out_thread, cbargs) != 0)
		return 0;

	return out_thread->parent_cnid;
}

/*
 * hfslib_find_catalog_record_with_cnid()
 *
 * Looks up a catalog record by calling hfslib_find_parent_thread() and
 * hfslib_find_catalog_record_with_key(). out_key may be NULL; if not, the key
 * corresponding to this cnid is stuffed in it. Returns 0 on success.
 */
int
hfslib_find_catalog_record_with_cnid(
	hfs_volume* in_vol,
	hfs_cnid_t in_cnid,
	hfs_catalog_keyed_record_t* out_rec,
	hfs_catalog_key_t* out_key,
	hfs_callback_args* cbargs)
{
	hfs_cnid_t					parentcnid;
	hfs_thread_record_t		parentthread;
	hfs_catalog_key_t			key;

	if (in_vol == NULL || in_cnid == 0 || out_rec == NULL)
		return 0;

	parentcnid =
		hfslib_find_parent_thread(in_vol, in_cnid, &parentthread, cbargs);
	if (parentcnid == 0)
		HFS_LIBERR("could not find parent thread for cnid %i", in_cnid);

	if (hfslib_make_catalog_key(parentthread.parent_cnid,
		parentthread.name.length, parentthread.name.unicode, &key) == 0)
		HFS_LIBERR("could not make catalog search key");

	if (out_key != NULL)
		memcpy(out_key, &key, sizeof(key));

	return hfslib_find_catalog_record_with_key(in_vol, &key, out_rec, cbargs);

error:
	return 1;
}

/* Returns 0 on success, 1 on error, and -1 if record was not found. */
int
hfslib_find_catalog_record_with_key(
	hfs_volume* in_vol,
	hfs_catalog_key_t* in_key,
	hfs_catalog_keyed_record_t* out_rec,
	hfs_callback_args* cbargs)
{
	hfs_node_descriptor_t			nd;
	hfs_extent_descriptor_t*		extents;
	hfs_catalog_keyed_record_t		lastrec;
	hfs_catalog_key_t*	curkey;
	void**				recs;
	void*				buffer;
	uint64_t			bytesread;
	uint32_t			curnode;
	uint16_t*			recsizes;
	uint16_t			numextents;
	uint16_t			recnum;
	int16_t				leaftype;
	int					keycompare;
	int					result;

	if (in_key == NULL || out_rec == NULL || in_vol == NULL)
		return 1;

	result = 1;
	buffer = NULL;
	curkey = NULL;
	extents = NULL;
	recs = NULL;
	recsizes = NULL;
	nd.num_recs = 0;

	/* The key takes up over half a kb of ram, which is a lot for the BSD
	 * kernel stack. So allocate it in the heap instead to play it safe. */
	curkey = hfslib_malloc(sizeof(hfs_catalog_key_t), cbargs);
	if (curkey == NULL)
		HFS_LIBERR("could not allocate catalog search key");

	buffer = hfslib_malloc(in_vol->chr.node_size, cbargs);
	if (buffer == NULL)
		HFS_LIBERR("could not allocate node buffer");

	numextents = hfslib_get_file_extents(in_vol, HFS_CNID_CATALOG,
		HFS_DATAFORK, &extents, cbargs);
	if (numextents == 0)
		HFS_LIBERR("could not locate fork extents");

	curnode = in_vol->chr.root_node;

#ifdef DLO_DEBUG
	printf("-> key ");
	dlo_print_key(in_key);
	printf("\n");
#endif

	do {
#ifdef DLO_DEBUG
		printf("--> node %d\n", curnode);
#endif

		if (hfslib_readd_with_extents(in_vol, buffer, 
			&bytesread,in_vol->chr.node_size, curnode * in_vol->chr.node_size, 
			extents, numextents, cbargs) != 0)
			HFS_LIBERR("could not read catalog node #%i", curnode);

		if (hfslib_reada_node(buffer, &nd, &recs, &recsizes, HFS_CATALOG_FILE,
			in_vol, cbargs) == 0)
			HFS_LIBERR("could not parse catalog node #%i", curnode);

		for (recnum = 0; recnum < nd.num_recs; recnum++)
		{
			leaftype = nd.kind;
			if (hfslib_read_catalog_keyed_record(recs[recnum], out_rec,
				&leaftype, curkey, in_vol) == 0)
				HFS_LIBERR("could not read catalog record #%i",recnum);

#ifdef DLO_DEBUG
			printf("---> record %d: ", recnum);
			dlo_print_key(curkey);
			fflush(stdout);
#endif
			keycompare = in_vol->keycmp(in_key, curkey);
#ifdef DLO_DEBUG
			printf(" %c\n",
			       keycompare < 0 ? '<'
			       : keycompare == 0 ? '=' : '>');
#endif

			if (keycompare < 0) {
				/* Check if key is less than *every* record, which should never
				 * happen if the volume is consistent and the key legit. */
				if (recnum == 0)
					HFS_LIBERR("all records greater than key");

				/* Otherwise, we've found the first record that exceeds our key,
				 * so retrieve the previous record, which is still less... */
				memcpy(out_rec, &lastrec,
					sizeof(hfs_catalog_keyed_record_t));

				/* ...unless this is a leaf node, which means we've gone from
				 * a key which is smaller than the search key, in the previous
				 * loop, to a key which is larger, in this loop, and that
				 * implies that our search key does not exist on the volume. */
				if (nd.kind == HFS_LEAFNODE)
					result = -1;
				break;
			} else if (keycompare == 0) {
				/* If leaf node, found an exact match. */
				result = 0;
				break;
			} else if (recnum == nd.num_recs-1 && keycompare > 0) {
				/* If leaf node, we've reached the last record with no match,
				 * which means this key is not present on the volume. */
				result = -1;
				break;
			}

			memcpy(&lastrec, out_rec, sizeof(hfs_catalog_keyed_record_t));
		}

		if (nd.kind == HFS_INDEXNODE)
			curnode = out_rec->child;
		else if (nd.kind == HFS_LEAFNODE)
			break;
		hfslib_free_recs(&recs, &recsizes, &nd.num_recs, cbargs);
	} while (nd.kind != HFS_LEAFNODE);

	/* FALLTHROUGH */
error:
	if (extents != NULL)
		hfslib_free(extents, cbargs);
	hfslib_free_recs(&recs, &recsizes, &nd.num_recs, cbargs);
	if (curkey != NULL)
		hfslib_free(curkey, cbargs);		
	if (buffer != NULL)
		hfslib_free(buffer, cbargs);
	return result;
}

/* returns 0 on success */
/* XXX Need to look this over and make sure it gracefully handles cases where
 * XXX the key is not found. */
int
hfslib_find_extent_record_with_key(hfs_volume* in_vol,
	hfs_extent_key_t* in_key,
	hfs_extent_record_t* out_rec,
	hfs_callback_args* cbargs)
{
	hfs_node_descriptor_t		nd;
	hfs_extent_descriptor_t*	extents;
	hfs_extent_record_t		lastrec;
	hfs_extent_key_t	curkey;
	void**				recs;
	void*				buffer;
	uint64_t			bytesread;
	uint32_t			curnode;
	uint16_t*			recsizes;
	uint16_t			numextents;
	uint16_t			recnum;
	int					keycompare;
	int					result;
	
	if (in_vol == NULL || in_key == NULL || out_rec == NULL)
		return 1;

	result = 1;
	buffer = NULL;
	extents = NULL;
	recs = NULL;
	recsizes = NULL;
	nd.num_recs = 0;

	buffer = hfslib_malloc(in_vol->ehr.node_size, cbargs);
	if (buffer == NULL)
		HFS_LIBERR("could not allocate node buffer");

	numextents = hfslib_get_file_extents(in_vol, HFS_CNID_EXTENTS,
		HFS_DATAFORK, &extents, cbargs);
	if (numextents == 0)
		HFS_LIBERR("could not locate fork extents");

	curnode = in_vol->ehr.root_node;

	do {
		hfslib_free_recs(&recs, &recsizes, &nd.num_recs, cbargs);
		recnum = 0;

		if (hfslib_readd_with_extents(in_vol, buffer, &bytesread, 
			in_vol->ehr.node_size, curnode * in_vol->ehr.node_size, extents, 
			numextents, cbargs) != 0)
			HFS_LIBERR("could not read extents overflow node #%i", curnode);

		if (hfslib_reada_node(buffer, &nd, &recs, &recsizes, HFS_EXTENTS_FILE,
			in_vol, cbargs) == 0)
			HFS_LIBERR("could not parse extents overflow node #%i",curnode);

		for (recnum = 0; recnum < nd.num_recs; recnum++) {
			memcpy(&lastrec, out_rec, sizeof(hfs_extent_record_t));

			if (hfslib_read_extent_record(recs[recnum], out_rec, nd.kind,
				&curkey, in_vol) == 0)
				HFS_LIBERR("could not read extents record #%i",recnum);

			keycompare = hfslib_compare_extent_keys(in_key, &curkey);
			if (keycompare < 0) {
				/* this should never happen for any legitimate key */
				if (recnum == 0)
					return 1;
				memcpy(out_rec, &lastrec, sizeof(hfs_extent_record_t));
				break;
			} else if (keycompare == 0 ||
			    (recnum == nd.num_recs-1 && keycompare > 0))
				break;
		}
		
		if (nd.kind == HFS_INDEXNODE)
			curnode = *((uint32_t *)out_rec); /* out_rec is a node ptr in this case */
		else if (nd.kind == HFS_LEAFNODE)
			break;
		else
		    HFS_LIBERR("unknwon node type for extents overflow node #%i",curnode);
	} while (nd.kind != HFS_LEAFNODE);

	result = 0;

	/* FALLTHROUGH */

error:
	if (buffer != NULL)
		hfslib_free(buffer, cbargs);
	if (extents != NULL)
		hfslib_free(extents, cbargs);
	hfslib_free_recs(&recs, &recsizes, &nd.num_recs, cbargs);
	return result;	
}

/* out_extents may be NULL. */
uint16_t
hfslib_get_file_extents(hfs_volume* in_vol,
	hfs_cnid_t in_cnid,
	uint8_t in_forktype,
	hfs_extent_descriptor_t** out_extents,
	hfs_callback_args* cbargs)
{
	hfs_extent_descriptor_t*	dummy;
	hfs_extent_key_t		extentkey;
	hfs_file_record_t		file;
	hfs_catalog_key_t		filekey;
	hfs_thread_record_t	fileparent;
	hfs_fork_t		fork = {.logical_size = 0};
	hfs_extent_record_t	nextextentrec;
	uint32_t	numblocks;
	uint16_t	numextents, n;

	if (in_vol == NULL || in_cnid == 0)
		return 0;

	if (out_extents != NULL) {
		*out_extents = hfslib_malloc(sizeof(hfs_extent_descriptor_t), cbargs);
		if (*out_extents == NULL)
			return 0;
	}

	switch(in_cnid)
	{
		case HFS_CNID_CATALOG:
			fork = in_vol->vh.catalog_file;
			break;

		case HFS_CNID_EXTENTS:
			fork = in_vol->vh.extents_file;
			break;

		case HFS_CNID_ALLOCATION:
			fork = in_vol->vh.allocation_file;
			break;

		case HFS_CNID_ATTRIBUTES:
			fork = in_vol->vh.attributes_file;
			break;

		case HFS_CNID_STARTUP:
			fork = in_vol->vh.startup_file;
			break;

		default:
			if (hfslib_find_parent_thread(in_vol, in_cnid, &fileparent,
				cbargs) == 0)
				goto error;

			if (hfslib_make_catalog_key(fileparent.parent_cnid,
				fileparent.name.length, fileparent.name.unicode, &filekey) == 0)
				goto error;

			if (hfslib_find_catalog_record_with_key(in_vol, &filekey,
				(hfs_catalog_keyed_record_t*)&file, cbargs) != 0)
				goto error;

			/* only files have extents, not folders or threads */
			if (file.rec_type != HFS_REC_FILE)
				goto error;

			if (in_forktype == HFS_DATAFORK)
				fork = file.data_fork;
			else if (in_forktype == HFS_RSRCFORK)
				fork = file.rsrc_fork;
	}

	numextents = 0;
	numblocks = 0;
	memcpy(&nextextentrec, &fork.extents, sizeof(hfs_extent_record_t));

	while (1) {
		for (n = 0; n < 8; n++) {
			if (numblocks + nextextentrec[n].block_count <= numblocks)
				break;
			numblocks += nextextentrec[n].block_count;
		}
		if (out_extents != NULL) {
			dummy = hfslib_realloc(*out_extents,
			    (numextents+n) * sizeof(hfs_extent_descriptor_t),
			    cbargs);
			if (dummy == NULL)
				goto error;
			*out_extents = dummy;

			memcpy(*out_extents + numextents,
			    &nextextentrec, n*sizeof(hfs_extent_descriptor_t));
		}
		numextents += n;

		if (numblocks >= fork.total_blocks)
			break;

		if (hfslib_make_extent_key(in_cnid, in_forktype, numblocks,
			&extentkey) == 0)
			goto error;

		if (hfslib_find_extent_record_with_key(in_vol, &extentkey,
			&nextextentrec, cbargs) != 0)
			goto error;
	}

	goto exit;
	
error:
	if (out_extents != NULL && *out_extents != NULL) {
		hfslib_free(*out_extents, cbargs);
		*out_extents = NULL;
	}
	return 0;

exit:
	return numextents;
}

/*
 * hfslib_get_directory_contents()
 *
 * Finds the immediate children of a given directory CNID and places their 
 * CNIDs in an array allocated here. The first child is found by doing a
 * catalog search that only compares parent CNIDs (ignoring file/folder names)
 * and skips over thread records. Then the remaining children are listed in 
 * ascending order by name, according to the HFS+ spec, so just read off each
 * successive leaf node until a different parent CNID is found.
 * 
 * If out_childnames is not NULL, it will be allocated and set to an array of
 * hfs_unistr255_t's which correspond to the name of the child with that same
 * index.
 *
 * out_children may be NULL.
 *
 * Returns 0 on success.
 */
int
hfslib_get_directory_contents(
	hfs_volume* in_vol,
	hfs_cnid_t in_dir,
	hfs_catalog_keyed_record_t** out_children,
	hfs_unistr255_t** out_childnames,
	uint32_t* out_numchildren,
	hfs_callback_args* cbargs)
{
	hfs_node_descriptor_t			nd;
	hfs_extent_descriptor_t*		extents;
	hfs_catalog_keyed_record_t		currec;
	hfs_catalog_key_t	curkey;
	void**				recs;
	void*				buffer;
	void*				ptr; /* temporary pointer for realloc() */
	uint64_t			bytesread;
	uint32_t			curnode;
	uint32_t			lastnode;
	uint16_t*			recsizes;
	uint16_t			numextents;
	uint16_t			recnum;
	int16_t				leaftype;
	int					keycompare;
	int					result;

	if (in_vol == NULL || in_dir == 0 || out_numchildren == NULL)
		return 1;

	result = 1;
	buffer = NULL;
	extents = NULL;
	lastnode = 0;
	recs = NULL;
	recsizes = NULL;
	*out_numchildren = 0;
	if (out_children != NULL)
		*out_children = NULL;
	if (out_childnames != NULL)
		*out_childnames = NULL;
	nd.num_recs = 0;

	buffer = hfslib_malloc(in_vol->chr.node_size, cbargs);
	if (buffer == NULL)
		HFS_LIBERR("could not allocate node buffer");

	numextents = hfslib_get_file_extents(in_vol, HFS_CNID_CATALOG,
		HFS_DATAFORK, &extents, cbargs);
	if (numextents == 0)
		HFS_LIBERR("could not locate fork extents");

	curnode = in_vol->chr.root_node;

	while (1)
	{
		hfslib_free_recs(&recs, &recsizes, &nd.num_recs, cbargs);
		recnum = 0;

		if (hfslib_readd_with_extents(in_vol, buffer, &bytesread, 
			in_vol->chr.node_size, curnode * in_vol->chr.node_size, extents, 
			numextents, cbargs) != 0)
			HFS_LIBERR("could not read catalog node #%i", curnode);

		if (hfslib_reada_node(buffer, &nd, &recs, &recsizes, HFS_CATALOG_FILE,
			in_vol, cbargs) == 0)
			HFS_LIBERR("could not parse catalog node #%i", curnode);

		for (recnum = 0; recnum < nd.num_recs; recnum++)
		{
			leaftype = nd.kind; /* needed b/c leaftype might be modified now */
			if (hfslib_read_catalog_keyed_record(recs[recnum], &currec,
				&leaftype, &curkey, in_vol) == 0)
				HFS_LIBERR("could not read cat record %i:%i", curnode, recnum);

			if (nd.kind == HFS_INDEXNODE)
			{
				keycompare = in_dir - curkey.parent_cnid;
				if (keycompare < 0) {
					/* Check if key is less than *every* record, which should 
					 * never happen if the volume and key are good. */
					if (recnum == 0)
						HFS_LIBERR("all records greater than key");

					/* Otherwise, we've found the first record that exceeds our 
					 * key, so retrieve the previous, lesser record. */
					curnode = lastnode;
					break;
				} else if (keycompare == 0) {
					/*
					 * Normally, if we were doing a typical catalog lookup with
					 * both a parent cnid AND a name, keycompare==0 would be an
					 * exact match. However, since we are ignoring object names
					 * in this case and only comparing parent cnids, a direct
					 * match on only a parent cnid could mean that we've found
					 * an object with that parent cnid BUT which is NOT the
					 * first object (according to the HFS+ spec) with that
					 * parent cnid. Thus, when we find a parent cnid match, we 
					 * still go back to the previously found leaf node and start
					 * checking it for a possible prior instance of an object
					 * with our desired parent cnid.
					 */
					curnode = lastnode;
					break;					 
				} else if (recnum == nd.num_recs-1 && keycompare > 0) {
					/* Descend to child node if we found an exact match, or if
					 * this is the last pointer record. */
					curnode = currec.child;
					break;
				}

				lastnode = currec.child;
			} else {
				/*
				 * We have now descended down the hierarchy of index nodes into
				 * the leaf node that contains the first catalog record with a
				 * matching parent CNID. Since all leaf nodes are chained
				 * through their flink/blink, we can simply walk forward through
				 * this chain, copying every matching non-thread record, until 
				 * we hit a record with a different parent CNID. At that point,
				 * we've retrieved all of our directory's items, if any.
				 */
				curnode = nd.flink;

				if (curkey.parent_cnid < in_dir) {
					continue;
				} else if (curkey.parent_cnid == in_dir) {
					/* Hide files/folders which are supposed to be invisible
					 * to users, according to the hfs+ spec. */
					if (hfslib_is_private_file(&curkey))
						continue;

					/* leaftype has now been set to the catalog record type */
					if (leaftype == HFS_REC_FLDR || leaftype == HFS_REC_FILE)
					{
						(*out_numchildren)++;

						if (out_children != NULL) {
							ptr = hfslib_realloc(*out_children, 
								*out_numchildren *
								sizeof(hfs_catalog_keyed_record_t), cbargs);
							if (ptr == NULL)
								HFS_LIBERR("could not allocate child record");
							*out_children = ptr;

							memcpy(&((*out_children)[*out_numchildren-1]), 
								&currec, sizeof(hfs_catalog_keyed_record_t));
						}

						if (out_childnames != NULL) {
							ptr = hfslib_realloc(*out_childnames,
								*out_numchildren * sizeof(hfs_unistr255_t),
								cbargs);
							if (ptr == NULL)
								HFS_LIBERR("could not allocate child name");
							*out_childnames = ptr;

							memcpy(&((*out_childnames)[*out_numchildren-1]), 
								&curkey.name, sizeof(hfs_unistr255_t));
						}
					}
				} else {
					result = 0;
					/* We have just now passed the last item in the desired
					 * folder (or the folder was empty), so exit. */
					goto exit;
				}
			}
		}
	}

	result = 0;
	goto exit;

error:
	if (out_children != NULL && *out_children != NULL)
		hfslib_free(*out_children, cbargs);
	if (out_childnames != NULL && *out_childnames != NULL)
		hfslib_free(*out_childnames, cbargs);
	/* FALLTHROUGH */

exit:
	if (extents != NULL)
		hfslib_free(extents, cbargs);
	hfslib_free_recs(&recs, &recsizes, &nd.num_recs, cbargs);
	if (buffer != NULL)
		hfslib_free(buffer, cbargs);
	return result;
}

int
hfslib_is_journal_clean(hfs_volume* in_vol)
{
	if (in_vol == NULL)
		return 0;

	/* return true if no journal */
	if (!(in_vol->vh.attributes & (1<<HFS_VOL_JOURNALED)))
		return 1;

	return (in_vol->jh.start == in_vol->jh.end);
}

/*
 * hfslib_is_private_file()
 *
 * Given a file/folder's key and parent CNID, determines if it should be hidden
 * from the user (e.g., the journal header file or the HFS+ Private Data folder)
 */
int
hfslib_is_private_file(hfs_catalog_key_t *filekey)
{
	hfs_catalog_key_t* curkey = NULL;
	int i = 0;

	/*
	 * According to the HFS+ spec to date, all special objects are located in
	 * the root directory of the volume, so don't bother going further if the
	 * requested object is not.
	 */
	if (filekey->parent_cnid != HFS_CNID_ROOT_FOLDER)
		return 0;

	while ((curkey = hfs_gPrivateObjectKeys[i]) != NULL) {
		/* XXX Always use binary compare here, or use volume's specific key 
		 * XXX comparison routine? */
		if (filekey->name.length == curkey->name.length &&
		    memcmp(filekey->name.unicode, curkey->name.unicode,
				2 * curkey->name.length) == 0)
			return 1;
		i++;
	}

	return 0;
}


/* bool
hfslib_is_journal_valid(hfs_volume* in_vol)
{
	- check magic numbers
	- check Other Things
}*/

#if 0
#pragma mark -
#pragma mark Major Structures
#endif

/*
 *	hfslib_read_volume_header()
 *	
 *	Reads in_bytes, formats the data appropriately, and places the result
 *	in out_header, which is assumed to be previously allocated. Returns number
 *	of bytes read, 0 if failed.
 */

size_t
hfslib_read_volume_header(void* in_bytes, hfs_volume_header_t* out_header)
{
	void*	ptr;
	size_t	last_bytes_read;
	int		i;

	if (in_bytes == NULL || out_header == NULL)
		return 0;

	ptr = in_bytes;

	out_header->signature = be16tohp(&ptr);
	out_header->version = be16tohp(&ptr);
	out_header->attributes = be32tohp(&ptr);
	out_header->last_mounting_version = be32tohp(&ptr);
	out_header->journal_info_block = be32tohp(&ptr);

	out_header->date_created = be32tohp(&ptr);
	out_header->date_modified = be32tohp(&ptr);
	out_header->date_backedup = be32tohp(&ptr);
	out_header->date_checked = be32tohp(&ptr);

	out_header->file_count = be32tohp(&ptr);
	out_header->folder_count = be32tohp(&ptr);

	out_header->block_size = be32tohp(&ptr);
	out_header->total_blocks = be32tohp(&ptr);
	out_header->free_blocks = be32tohp(&ptr);
	out_header->next_alloc_block = be32tohp(&ptr);
	out_header->rsrc_clump_size = be32tohp(&ptr);
	out_header->data_clump_size = be32tohp(&ptr);
	out_header->next_cnid = be32tohp(&ptr);

	out_header->write_count = be32tohp(&ptr);
	out_header->encodings = be64tohp(&ptr);

	for (i =0 ; i < 8; i++)
		out_header->finder_info[i] = be32tohp(&ptr);

	if ((last_bytes_read = hfslib_read_fork_descriptor(ptr,
		&out_header->allocation_file)) == 0)
		return 0;
	ptr = (uint8_t*)ptr + last_bytes_read;

	if ((last_bytes_read = hfslib_read_fork_descriptor(ptr,
		&out_header->extents_file)) == 0)
		return 0;
	ptr = (uint8_t*)ptr + last_bytes_read;

	if ((last_bytes_read = hfslib_read_fork_descriptor(ptr,
		&out_header->catalog_file)) == 0)
		return 0;
	ptr = (uint8_t*)ptr + last_bytes_read;

	if ((last_bytes_read = hfslib_read_fork_descriptor(ptr,
		&out_header->attributes_file)) == 0)
		return 0;
	ptr = (uint8_t*)ptr + last_bytes_read;

	if ((last_bytes_read = hfslib_read_fork_descriptor(ptr,
		&out_header->startup_file)) == 0)
		return 0;
	ptr = (uint8_t*)ptr + last_bytes_read;

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

/*
 *      hfsplib_read_master_directory_block()
 *      
 *      Reads in_bytes, formats the data appropriately, and places the result
 *      in out_header, which is assumed to be previously allocated. Returns numb
er
 *      of bytes read, 0 if failed.
 */

size_t
hfslib_read_master_directory_block(void* in_bytes,
    hfs_hfs_master_directory_block_t* out_mdr)
{
	void*   ptr;
	int     i;

	if (in_bytes == NULL || out_mdr == NULL)
		return 0;

	ptr = in_bytes;

	out_mdr->signature = be16tohp(&ptr);

	out_mdr->date_created = be32tohp(&ptr);
	out_mdr->date_modified = be32tohp(&ptr);

	out_mdr->attributes = be16tohp(&ptr);
	out_mdr->root_file_count = be16tohp(&ptr);
	out_mdr->volume_bitmap = be16tohp(&ptr);

	out_mdr->next_alloc_block = be16tohp(&ptr);
	out_mdr->total_blocks = be16tohp(&ptr);
	out_mdr->block_size = be32tohp(&ptr);

	out_mdr->clump_size = be32tohp(&ptr);
	out_mdr->first_block = be16tohp(&ptr);
	out_mdr->next_cnid = be32tohp(&ptr);
	out_mdr->free_blocks = be16tohp(&ptr);

	memcpy(out_mdr->volume_name, ptr, 28);
	ptr = (char *)ptr + 28;

	out_mdr->date_backedup = be32tohp(&ptr);
	out_mdr->backup_seqnum = be16tohp(&ptr);

	out_mdr->write_count = be32tohp(&ptr);

	out_mdr->extents_clump_size = be32tohp(&ptr);
	out_mdr->catalog_clump_size = be32tohp(&ptr);

	out_mdr->root_folder_count = be16tohp(&ptr);
	out_mdr->file_count = be32tohp(&ptr);
	out_mdr->folder_count = be32tohp(&ptr);

	for (i = 0; i < 8; i++)
		out_mdr->finder_info[i] = be32tohp(&ptr);

	out_mdr->embedded_signature = be16tohp(&ptr);
	out_mdr->embedded_extent.start_block = be16tohp(&ptr);
	out_mdr->embedded_extent.block_count = be16tohp(&ptr);

	out_mdr->extents_size = be32tohp(&ptr);
	for (i = 0; i < 3; i++) {
		out_mdr->extents_extents[i].start_block = be16tohp(&ptr);
		out_mdr->extents_extents[i].block_count = be16tohp(&ptr);
	}

	out_mdr->catalog_size = be32tohp(&ptr);
	for (i = 0; i < 3; i++) {
		out_mdr->catalog_extents[i].start_block = be16tohp(&ptr);
		out_mdr->catalog_extents[i].block_count = be16tohp(&ptr);
	}

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

/*
 *	hfslib_reada_node()
 *	
 *	Given the pointer to and size of a buffer containing the entire, raw
 *	contents of any b-tree node from the disk, this function will:
 *
 *		1.	determine the type of node and read its contents
 *		2.	allocate memory for each record and fill it appropriately
 *		3.	set out_record_ptrs_array to point to an array (which it allocates)
 *			which has out_node_descriptor->num_recs many pointers to the
 *			records themselves
 *		4.	allocate out_record_ptr_sizes_array and fill it with the sizes of
 *			each record
 *		5.	return the number of bytes read (i.e., the size of the node)
 *			or 0 on failure
 *	
 *	out_node_descriptor must be allocated by the caller and may not be NULL.
 *
 *	out_record_ptrs_array and out_record_ptr_sizes_array must both be specified,
 *	or both be NULL if the caller is not interested in reading the records.
 *
 *	out_record_ptr_sizes_array may be NULL if the caller is not interested in
 *	reading the records, but must not be NULL if out_record_ptrs_array is not.
 *
 *	in_parent_file is HFS_CATALOG_FILE, HFS_EXTENTS_FILE, or
 *	HFS_ATTRIBUTES_FILE, depending on the special file in which this node
 *	resides.
 *
 *	inout_volume must have its catnodesize or extnodesize field (depending on
 *	the parent file) set to the correct value if this is an index, leaf, or map
 *	node. If this is a header node, the field will be set to its correct value.
 */
size_t
hfslib_reada_node(void* in_bytes,
	hfs_node_descriptor_t* out_node_descriptor,
	void** out_record_ptrs_array[],
	uint16_t* out_record_ptr_sizes_array[],
	hfs_btree_file_type in_parent_file,
	hfs_volume* inout_volume,
	hfs_callback_args* cbargs)
{
	void*		ptr;
	uint16_t*	rec_offsets;
	size_t		last_bytes_read;
	uint16_t	nodesize;
	uint16_t	numrecords;
	uint16_t	free_space_offset;	/* offset to free space in node */
	int			keysizefieldsize;
	int			i;

	numrecords = 0;
	rec_offsets = NULL;
	if (out_record_ptrs_array != NULL)
		*out_record_ptrs_array = NULL;
	if (out_record_ptr_sizes_array != NULL)
		*out_record_ptr_sizes_array = NULL;

	if (in_bytes == NULL || inout_volume == NULL || out_node_descriptor == NULL
		|| (out_record_ptrs_array == NULL && out_record_ptr_sizes_array != NULL)
		|| (out_record_ptrs_array != NULL && out_record_ptr_sizes_array == NULL) )
		goto error;

	ptr = in_bytes;

	out_node_descriptor->flink = be32tohp(&ptr);
	out_node_descriptor->blink = be32tohp(&ptr);
	out_node_descriptor->kind = *(((int8_t*)ptr));
	ptr = (uint8_t*)ptr + 1;
	out_node_descriptor->height = *(((uint8_t*)ptr));
	ptr = (uint8_t*)ptr + 1;
	out_node_descriptor->num_recs = be16tohp(&ptr);
	out_node_descriptor->reserved = be16tohp(&ptr);

	numrecords = out_node_descriptor->num_recs;
	if (numrecords == 0)
		HFS_LIBERR("node contains no records");

	/*
	 *	To go any further, we will need to know the size of this node, as well
	 *	as the width of keyed records' key_len parameters for this btree. If
	 *	this is an index, leaf, or map node, inout_volume already has the node
	 *	size set in its catnodesize or extnodesize field and the key length set
	 *	in the catkeysizefieldsize or extkeysizefieldsize for catalog files and
	 *	extent files, respectively. However, if this is a header node, this
	 *	information has not yet been determined, so this is the place to do it.
	 */
	if (out_node_descriptor->kind == HFS_HEADERNODE)
	{
		hfs_header_record_t	hr;
		void*		header_rec_offset[1];
		uint16_t	header_rec_size[1];

		/* sanity check to ensure this is a good header node */
		if (numrecords != 3)
			HFS_LIBERR("header node does not have exactly 3 records");

		header_rec_offset[0] = ptr;
		header_rec_size[0] = sizeof(hfs_header_record_t);

		last_bytes_read = hfslib_read_header_node(header_rec_offset,
			header_rec_size, 1, &hr, NULL, NULL);
		if (last_bytes_read == 0)
			HFS_LIBERR("could not read header node");

		switch(in_parent_file)
		{
			case HFS_CATALOG_FILE:
				inout_volume->chr.node_size = hr.node_size;
				inout_volume->catkeysizefieldsize =
					(hr.attributes & HFS_BIG_KEYS_MASK) ?
						sizeof(uint16_t):sizeof(uint8_t);
				break;

			case HFS_EXTENTS_FILE:
				inout_volume->ehr.node_size = hr.node_size;
				inout_volume->extkeysizefieldsize =
					(hr.attributes & HFS_BIG_KEYS_MASK) ?
						sizeof(uint16_t):sizeof(uint8_t);
				break;

			case HFS_ATTRIBUTES_FILE:
			default:
				HFS_LIBERR("invalid parent file type specified");
				/* NOTREACHED */
		}
	}
	
	switch (in_parent_file)
	{
		case HFS_CATALOG_FILE:
			nodesize = inout_volume->chr.node_size;
			keysizefieldsize = inout_volume->catkeysizefieldsize;
			break;

		case HFS_EXTENTS_FILE:
			nodesize = inout_volume->ehr.node_size;
			keysizefieldsize = inout_volume->extkeysizefieldsize;
			break;

		case HFS_ATTRIBUTES_FILE:
		default:
			HFS_LIBERR("invalid parent file type specified");
			/* NOTREACHED */
	}
	
	/*
	 *	Don't care about records so just exit after getting the node descriptor.
	 *	Note: This happens after the header node code, and not before it, in
	 *	case the caller calls this function and ignores the record data just to
	 *	get at the node descriptor, but then tries to call it again on a non-
	 *	header node without first setting inout_volume->cat/extnodesize.
	 */
	if (out_record_ptrs_array == NULL)
		return ((uint8_t*)ptr - (uint8_t*)in_bytes);

	if (nodesize < (numrecords+1) * sizeof(uint16_t))
		HFS_LIBERR("nodesize %" PRIu16 " too small for %" PRIu16 " records", nodesize, numrecords);

	rec_offsets = hfslib_malloc(numrecords * sizeof(uint16_t), cbargs);
	*out_record_ptr_sizes_array =
		hfslib_malloc(numrecords * sizeof(uint16_t), cbargs);
	if (rec_offsets == NULL || *out_record_ptr_sizes_array == NULL)
		HFS_LIBERR("could not allocate node record offsets");

	*out_record_ptrs_array = hfslib_malloc(numrecords * sizeof(void*), cbargs);
	if (*out_record_ptrs_array == NULL)
		HFS_LIBERR("could not allocate node records");

	last_bytes_read = hfslib_reada_node_offsets((uint8_t*)in_bytes + nodesize -
			numrecords * sizeof(uint16_t), rec_offsets, numrecords);
	if (last_bytes_read == 0)
		HFS_LIBERR("could not read node record offsets");

	/*	The size of the last record (i.e. the first one listed in the offsets)
	 *	must be determined using the offset to the node's free space. */
	free_space_offset = be16toh(*(uint16_t*)((uint8_t*)in_bytes + nodesize -
			(numrecords+1) * sizeof(uint16_t)));

	if (free_space_offset <= rec_offsets[0])
		HFS_LIBERR("corrupt record offsets %" PRIu16 "-%" PRIu16, free_space_offset, rec_offsets[0]);

	(*out_record_ptr_sizes_array)[numrecords-1] =
		free_space_offset - rec_offsets[0];
	for (i = 1; i < numrecords; i++) {
		if (rec_offsets[i-1] <= rec_offsets[i])
			HFS_LIBERR("corrupt record offsets %" PRIu16 "-%" PRIu16, rec_offsets[i-1], rec_offsets[i]);

		(*out_record_ptr_sizes_array)[numrecords-i-1] = 
			rec_offsets[i-1] - rec_offsets[i];
	}

	for (i = 0; i < numrecords; i++)
	{
		(*out_record_ptrs_array)[i] =
			hfslib_malloc((*out_record_ptr_sizes_array)[i], cbargs);

		if ((*out_record_ptrs_array)[i] == NULL)
			HFS_LIBERR("could not allocate node record #%i",i);

		/*	
		 *	If this is a keyed node (i.e., a leaf or index node), there are two
		 *	boundary rules that each record must obey:
		 *
		 *		1.	A pad byte must be placed between the key and data if the
		 *			size of the key plus the size of the key_len field is odd.
		 *
		 *		2.	A pad byte must be placed after the data if the data size
		 *			is odd.
		 *	
		 *	So in the first case we increment the starting point of the data
		 *	and correspondingly decrement the record size. In the second case
		 *	we decrement the record size.
		 */			
		if (out_node_descriptor->kind == HFS_LEAFNODE ||
		    out_node_descriptor->kind == HFS_INDEXNODE)
		{
			hfs_catalog_key_t	reckey;
			int16_t	rectype;

			rectype = out_node_descriptor->kind;
			last_bytes_read = hfslib_read_catalog_keyed_record(ptr, NULL,
				&rectype, &reckey, inout_volume);
			if (last_bytes_read == 0)
				HFS_LIBERR("could not read node record");

			if ((reckey.key_len + keysizefieldsize) % 2 == 1) {
				ptr = (uint8_t*)ptr + 1;
				(*out_record_ptr_sizes_array)[i]--;
			}

			if ((*out_record_ptr_sizes_array)[i] % 2 == 1)
				(*out_record_ptr_sizes_array)[i]--;
		}

		if ((ptr - in_bytes) + (*out_record_ptr_sizes_array)[i] > nodesize)
			HFS_LIBERR("record offset outside of node bounds %" PRIu16, (*out_record_ptr_sizes_array)[i]);

		memcpy((*out_record_ptrs_array)[i], ptr,
				(*out_record_ptr_sizes_array)[i]);
		ptr = (uint8_t*)ptr + (*out_record_ptr_sizes_array)[i];
	}

	goto exit;

error:
	hfslib_free_recs(out_record_ptrs_array, out_record_ptr_sizes_array,
		&numrecords, cbargs);

	ptr = in_bytes;
	
	/* warn("error occurred in hfslib_reada_node()"); */
	
	/* FALLTHROUGH */

exit:	
	if (rec_offsets != NULL)
		hfslib_free(rec_offsets, cbargs);
	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

/*
 *	hfslib_reada_node_offsets()
 *	
 *	Sets out_offset_array to contain the offsets to each record in the node,
 *	in reverse order. Does not read the free space offset.
 */
size_t
hfslib_reada_node_offsets(void* in_bytes, uint16_t* out_offset_array, uint16_t numrecords)
{
	void*		ptr;
	int i = 0;

	if (in_bytes == NULL || out_offset_array == NULL || numrecords == 0)
		return 0;

	ptr = in_bytes;

	/*
	 * The offset for record 0 (which is the very last offset in the node) is
	 * always equal to 14, the size of the node descriptor. So, once we hit
	 * offset=14, we know this is the last offset.
	 */
	do {
		*out_offset_array = be16tohp(&ptr);
	} while (*out_offset_array++ != (uint16_t)14 && ++i < numrecords);

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

/*	hfslib_read_header_node()
 *	
 *	out_header_record and/or out_map_record may be NULL if the caller doesn't
 *	care about their contents.
 */
size_t
hfslib_read_header_node(void** in_recs,
	uint16_t* in_rec_sizes,
	uint16_t in_num_recs,
	hfs_header_record_t* out_hr,
	void* out_userdata,
	void* out_map)
{
	void*	ptr;
	int		i;

	KASSERT(out_hr != NULL);

	if (in_recs == NULL || in_rec_sizes == NULL)
		return 0;

	ptr = in_recs[0];
	out_hr->tree_depth = be16tohp(&ptr);
	out_hr->root_node = be32tohp(&ptr);
	out_hr->leaf_recs = be32tohp(&ptr);
	out_hr->first_leaf = be32tohp(&ptr);
	out_hr->last_leaf = be32tohp(&ptr);
	out_hr->node_size = be16tohp(&ptr);
	out_hr->max_key_len = be16tohp(&ptr);
	out_hr->total_nodes = be32tohp(&ptr);
	out_hr->free_nodes = be32tohp(&ptr);
	out_hr->reserved = be16tohp(&ptr);
	out_hr->clump_size = be32tohp(&ptr);
	out_hr->btree_type = *(((uint8_t*)ptr));
	ptr = (uint8_t*)ptr + 1;
	out_hr->keycomp_type = *(((uint8_t*)ptr));
	ptr = (uint8_t*)ptr + 1;
	out_hr->attributes = be32tohp(&ptr);
	for (i = 0; i < 16; i++)
		out_hr->reserved2[i] = be32tohp(&ptr);

	if (in_num_recs > 1) {
		if (out_userdata != NULL)
			memcpy(out_userdata, in_recs[1], in_rec_sizes[1]);
		ptr = (uint8_t*)ptr + in_rec_sizes[1]; /* size of user data record */

		if (in_num_recs > 2) {
			if (out_map != NULL)
				memcpy(out_map, in_recs[2], in_rec_sizes[2]);
			ptr = (uint8_t*)ptr + in_rec_sizes[2]; /* size of map record */
		}
	}

	return ((uint8_t*)ptr - (uint8_t*)in_recs[0]);
}

/*
 *	hfslib_read_catalog_keyed_record()
 *	
 *	out_recdata can be NULL. inout_rectype must be set to either HFS_LEAFNODE
 *	or HFS_INDEXNODE upon calling this function, and will be set by the
 *	function to one of HFS_REC_FLDR, HFS_REC_FILE, HFS_REC_FLDR_THREAD, or
 *	HFS_REC_FLDR_THREAD upon return if the node is a leaf node. If it is an
 *	index node, inout_rectype will not be changed.
 */
size_t
hfslib_read_catalog_keyed_record(
	void* in_bytes,
	hfs_catalog_keyed_record_t* out_recdata,
	int16_t* inout_rectype,
	hfs_catalog_key_t* out_key,
	hfs_volume* in_volume)
{
	void*		ptr;
	size_t		last_bytes_read;

	if (in_bytes == NULL || out_key == NULL || inout_rectype == NULL)
		return 0;

	ptr = in_bytes;

	/*	For HFS+, the key length is always a 2-byte number. This is indicated
	 *	by the HFS_BIG_KEYS_MASK bit in the attributes field of the catalog
	 *	header record. However, we just assume this bit is set, since all HFS+
	 *	volumes should have it set anyway. */
	if (in_volume->catkeysizefieldsize == sizeof(uint16_t))
		out_key->key_len = be16tohp(&ptr);
	else if (in_volume->catkeysizefieldsize == sizeof(uint8_t)) {
		out_key->key_len = *(((uint8_t*)ptr));
		ptr = (uint8_t*)ptr + 1;
	}

	out_key->parent_cnid = be32tohp(&ptr);

	last_bytes_read = hfslib_read_unistr255(ptr, &out_key->name);
	if (last_bytes_read == 0)
		return 0;
	ptr = (uint8_t*)ptr + last_bytes_read;

	/* don't waste time if the user just wanted the key and/or record type */
	if (out_recdata == NULL) {
		if (*inout_rectype == HFS_LEAFNODE)
			*inout_rectype = be16tohp(&ptr);
		else if (*inout_rectype != HFS_INDEXNODE)
			return 0;	/* should not happen if we were given valid arguments */

		return ((uint8_t*)ptr - (uint8_t*)in_bytes);
	}

	if (*inout_rectype == HFS_INDEXNODE) {
		out_recdata->child = be32tohp(&ptr);
	} else {
		/* first need to determine what kind of record this is */
		*inout_rectype = be16tohp(&ptr);
		out_recdata->type = *inout_rectype;

		switch(out_recdata->type)
		{
			case HFS_REC_FLDR:
			{
				out_recdata->folder.flags = be16tohp(&ptr);
				out_recdata->folder.valence = be32tohp(&ptr);
				out_recdata->folder.cnid = be32tohp(&ptr);
				out_recdata->folder.date_created = be32tohp(&ptr);
				out_recdata->folder.date_content_mod = be32tohp(&ptr);
				out_recdata->folder.date_attrib_mod = be32tohp(&ptr);
				out_recdata->folder.date_accessed = be32tohp(&ptr);
				out_recdata->folder.date_backedup = be32tohp(&ptr);

				last_bytes_read = hfslib_read_bsd_data(ptr,
					&out_recdata->folder.bsd);
				if (last_bytes_read == 0)
					return 0;
				ptr = (uint8_t*)ptr + last_bytes_read;

				last_bytes_read = hfslib_read_folder_userinfo(ptr,
					&out_recdata->folder.user_info);
				if (last_bytes_read == 0)
					return 0;
				ptr = (uint8_t*)ptr + last_bytes_read;

				last_bytes_read = hfslib_read_folder_finderinfo(ptr,
					&out_recdata->folder.finder_info);
				if (last_bytes_read == 0)
					return 0;
				ptr = (uint8_t*)ptr + last_bytes_read;

				out_recdata->folder.text_encoding = be32tohp(&ptr);
				out_recdata->folder.reserved = be32tohp(&ptr);
			}
			break;

			case HFS_REC_FILE:
			{
				out_recdata->file.flags = be16tohp(&ptr);
				out_recdata->file.reserved = be32tohp(&ptr);
				out_recdata->file.cnid = be32tohp(&ptr);
				out_recdata->file.date_created = be32tohp(&ptr);
				out_recdata->file.date_content_mod = be32tohp(&ptr);
				out_recdata->file.date_attrib_mod = be32tohp(&ptr);
				out_recdata->file.date_accessed = be32tohp(&ptr);
				out_recdata->file.date_backedup = be32tohp(&ptr);

				last_bytes_read = hfslib_read_bsd_data(ptr,
					&out_recdata->file.bsd);
				if (last_bytes_read == 0)
					return 0;
				ptr = (uint8_t*)ptr + last_bytes_read;

				last_bytes_read = hfslib_read_file_userinfo(ptr,
					&out_recdata->file.user_info);
				if (last_bytes_read == 0)
					return 0;
				ptr = (uint8_t*)ptr + last_bytes_read;

				last_bytes_read = hfslib_read_file_finderinfo(ptr,
					&out_recdata->file.finder_info);
				if (last_bytes_read == 0)
					return 0;
				ptr = (uint8_t*)ptr + last_bytes_read;

				out_recdata->file.text_encoding = be32tohp(&ptr);
				out_recdata->file.reserved2 = be32tohp(&ptr);

				last_bytes_read = hfslib_read_fork_descriptor(ptr,
					&out_recdata->file.data_fork);
				if (last_bytes_read == 0)
					return 0;
				ptr = (uint8_t*)ptr + last_bytes_read;

				last_bytes_read = hfslib_read_fork_descriptor(ptr,
					&out_recdata->file.rsrc_fork);
				if (last_bytes_read == 0)
					return 0;
				ptr = (uint8_t*)ptr + last_bytes_read;
			}
			break;

			case HFS_REC_FLDR_THREAD:
			case HFS_REC_FILE_THREAD:
			{
				out_recdata->thread.reserved = be16tohp(&ptr);
				out_recdata->thread.parent_cnid = be32tohp(&ptr);

				last_bytes_read = hfslib_read_unistr255(ptr,
					&out_recdata->thread.name);
				if (last_bytes_read == 0)
					return 0;
				ptr = (uint8_t*)ptr + last_bytes_read;
			}
			break;

			default:
				return 1;
				/* NOTREACHED */
		}
	}

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

/* out_rec may be NULL */
size_t
hfslib_read_extent_record(
	void* in_bytes,
	hfs_extent_record_t* out_rec,
	hfs_node_kind in_nodekind,
	hfs_extent_key_t* out_key,
	hfs_volume* in_volume)
{
	void*		ptr;
	size_t		last_bytes_read;

	if (in_bytes == NULL || out_key == NULL
	    || (in_nodekind!=HFS_LEAFNODE && in_nodekind!=HFS_INDEXNODE))
		return 0;

	ptr = in_bytes;

	/* For HFS+, the key length is always a 2-byte number. This is indicated
	 * by the HFS_BIG_KEYS_MASK bit in the attributes field of the extent
	 * overflow header record. However, we just assume this bit is set, since
	 * all HFS+ volumes should have it set anyway. */
	if (in_volume->extkeysizefieldsize == sizeof(uint16_t))
		out_key->key_length = be16tohp(&ptr);
	else if (in_volume->extkeysizefieldsize == sizeof(uint8_t)) {
		out_key->key_length = *(((uint8_t*)ptr));
		ptr = (uint8_t*)ptr + 1;
	}

	out_key->fork_type = *(((uint8_t*)ptr));
	ptr = (uint8_t*)ptr + 1;
	out_key->padding = *(((uint8_t*)ptr));
	ptr = (uint8_t*)ptr + 1;
	out_key->file_cnid = be32tohp(&ptr);
	out_key->start_block = be32tohp(&ptr);

	/* don't waste time if the user just wanted the key */
	if (out_rec == NULL)
		return ((uint8_t*)ptr - (uint8_t*)in_bytes);

	if (in_nodekind == HFS_LEAFNODE) {
		last_bytes_read = hfslib_read_extent_descriptors(ptr, out_rec);
		if (last_bytes_read == 0)
			return 0;
		ptr = (uint8_t*)ptr + last_bytes_read;
	} else {
		/* XXX: this is completely bogus */
		/*      (uint32_t*)*out_rec = be32tohp(&ptr); */
	    uint32_t *ptr_32 = (uint32_t *)out_rec;
		*ptr_32 = be32tohp(&ptr);
		/* (*out_rec)[0].start_block = be32tohp(&ptr); */
	}

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

void
hfslib_free_recs(
	void*** inout_node_recs,
	uint16_t** inout_rec_sizes,
	uint16_t* inout_num_recs,
	hfs_callback_args* cbargs)
{
	uint16_t	i;

	if (inout_num_recs == NULL || *inout_num_recs == 0)
		return;

	if (inout_node_recs != NULL && *inout_node_recs != NULL) {
		for (i = 0 ; i < *inout_num_recs; i++) {
			if ((*inout_node_recs)[i] != NULL) {
				hfslib_free((*inout_node_recs)[i], cbargs);
				(*inout_node_recs)[i] = NULL;
			}		
		}
		hfslib_free(*inout_node_recs, cbargs);
		*inout_node_recs = NULL;
	}

	if (inout_rec_sizes != NULL && *inout_rec_sizes != NULL) {
		hfslib_free(*inout_rec_sizes, cbargs);
		*inout_rec_sizes = NULL;
	}

	*inout_num_recs = 0;
}

#if 0
#pragma mark -
#pragma mark Individual Fields
#endif

size_t
hfslib_read_fork_descriptor(void* in_bytes, hfs_fork_t* out_forkdata)
{
	void*	ptr;
	size_t	last_bytes_read;

	if (in_bytes == NULL || out_forkdata == NULL)
		return 0;

	ptr = in_bytes;

	out_forkdata->logical_size = be64tohp(&ptr);
	out_forkdata->clump_size = be32tohp(&ptr);
	out_forkdata->total_blocks = be32tohp(&ptr);

	if ((last_bytes_read = hfslib_read_extent_descriptors(ptr,
		&out_forkdata->extents)) == 0)
		return 0;
	ptr = (uint8_t*)ptr + last_bytes_read;

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

size_t
hfslib_read_extent_descriptors(
	void* in_bytes,
	hfs_extent_record_t* out_extentrecord)
{
	void*	ptr;
	int		i;

	if (in_bytes == NULL || out_extentrecord == NULL)
		return 0;

	ptr = in_bytes;

	for (i = 0; i < 8; i++) {
		(((hfs_extent_descriptor_t*)*out_extentrecord)[i]).start_block =
			be32tohp(&ptr);
		(((hfs_extent_descriptor_t*)*out_extentrecord)[i]).block_count =
			be32tohp(&ptr);
	}

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

size_t
hfslib_read_unistr255(void* in_bytes, hfs_unistr255_t* out_string)
{
	void*		ptr;
	uint16_t	i, length;

	if (in_bytes == NULL || out_string == NULL)
		return 0;

	ptr = in_bytes;

	length = be16tohp(&ptr);
	if (length > 255)
		length = 255; /* hfs+ folder/file names have a limit of 255 chars */
	out_string->length = length;

	for (i = 0; i < length; i++) {
		out_string->unicode[i] = be16tohp(&ptr);
	}

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

size_t
hfslib_read_bsd_data(void* in_bytes, hfs_bsd_data_t* out_perms)
{
	void*	ptr;

	if (in_bytes == NULL || out_perms == NULL)
		return 0;

	ptr = in_bytes;

	out_perms->owner_id = be32tohp(&ptr);
	out_perms->group_id = be32tohp(&ptr);
	out_perms->admin_flags = *(((uint8_t*)ptr));
	ptr = (uint8_t*)ptr + 1;
	out_perms->owner_flags = *(((uint8_t*)ptr));
	ptr = (uint8_t*)ptr + 1;
	out_perms->file_mode = be16tohp(&ptr);
	out_perms->special.inode_num = be32tohp(&ptr); /* this field is a union */

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

size_t
hfslib_read_file_userinfo(void* in_bytes, hfs_macos_file_info_t* out_info)
{
	void*	ptr;

	if (in_bytes == NULL || out_info == NULL)
		return 0;

	ptr = in_bytes;

	out_info->file_type = be32tohp(&ptr);
	out_info->file_creator = be32tohp(&ptr);
	out_info->finder_flags = be16tohp(&ptr);
	out_info->location.v = be16tohp(&ptr);
	out_info->location.h = be16tohp(&ptr);
	out_info->reserved = be16tohp(&ptr);

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

size_t
hfslib_read_file_finderinfo(
	void* in_bytes,
	hfs_macos_extended_file_info_t* out_info)
{
	void*	ptr;

	if (in_bytes == NULL || out_info == NULL)
		return 0;

	ptr = in_bytes;

#if 0
	#pragma warn Fill in with real code!
#endif
	/* FIXME: Fill in with real code! */
	memset(out_info, 0, sizeof(*out_info));
	ptr = (uint8_t*)ptr + sizeof(hfs_macos_extended_file_info_t);

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

size_t
hfslib_read_folder_userinfo(void* in_bytes, hfs_macos_folder_info_t* out_info)
{
	void*	ptr;

	if (in_bytes == NULL || out_info == NULL)
		return 0;

	ptr = in_bytes;

#if 0
	#pragma warn Fill in with real code!
#endif
	/* FIXME: Fill in with real code! */
	memset(out_info, 0, sizeof(*out_info));
	ptr = (uint8_t*)ptr + sizeof(hfs_macos_folder_info_t);

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

size_t
hfslib_read_folder_finderinfo(
	void* in_bytes,
	hfs_macos_extended_folder_info_t* out_info)
{
	void*	ptr;

	if (in_bytes == NULL || out_info == NULL)
		return 0;

	ptr = in_bytes;

#if 0
	#pragma warn Fill in with real code!
#endif
	/* FIXME: Fill in with real code! */
	memset(out_info, 0, sizeof(*out_info));
	ptr = (uint8_t*)ptr + sizeof(hfs_macos_extended_folder_info_t);

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

size_t
hfslib_read_journal_info(void* in_bytes, hfs_journal_info_t* out_info)
{
	void*	ptr;
	int		i;

	if (in_bytes == NULL || out_info == NULL)
		return 0;

	ptr = in_bytes;

	out_info->flags = be32tohp(&ptr);
	for (i = 0; i < 8; i++) {
		out_info->device_signature[i] = be32tohp(&ptr);
	}
	out_info->offset = be64tohp(&ptr);
	out_info->size = be64tohp(&ptr);
	for (i = 0; i < 32; i++) {
		out_info->reserved[i] = be32tohp(&ptr);
	}

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

size_t
hfslib_read_journal_header(void* in_bytes, hfs_journal_header_t* out_header)
{
	void*	ptr;

	if (in_bytes == NULL || out_header == NULL)
		return 0;

	ptr = in_bytes;

	out_header->magic = be32tohp(&ptr);
	out_header->endian = be32tohp(&ptr);
	out_header->start = be64tohp(&ptr);
	out_header->end = be64tohp(&ptr);
	out_header->size = be64tohp(&ptr);
	out_header->blocklist_header_size = be32tohp(&ptr);
	out_header->checksum = be32tohp(&ptr);
	out_header->journal_header_size = be32tohp(&ptr);

	return ((uint8_t*)ptr - (uint8_t*)in_bytes);
}

#if 0
#pragma mark -
#pragma mark Disk Access
#endif

/*
 *	hfslib_readd_with_extents()
 *	
 *	This function reads the contents of a file from the volume, given an array
 *	of extent descriptors which specify where every extent of the file is
 *	located (in addition to the usual pread() arguments). out_bytes is presumed
 *  to exist and be large enough to hold in_length number of bytes. Returns 0
 *	on success.
 */
int
hfslib_readd_with_extents(
	hfs_volume*	in_vol,
	void*		out_bytes,
	uint64_t*	out_bytesread,
	uint64_t	in_length,
	uint64_t	in_offset,
	hfs_extent_descriptor_t in_extents[],
	uint16_t	in_numextents,
	hfs_callback_args*	cbargs)
{
	uint64_t	ext_length, last_offset;
	uint16_t	i;
	int			error;

	if (in_vol == NULL || out_bytes == NULL || in_extents == NULL ||
	    in_numextents == 0 || out_bytesread == NULL)
		return -1;

	*out_bytesread = 0;
	last_offset = 0;

	for (i = 0; i < in_numextents; i++)
	{
		if (in_extents[i].block_count == 0)
			continue;

		ext_length = in_extents[i].block_count * in_vol->vh.block_size;

		if (last_offset + ext_length < last_offset)
			break;

		if (in_offset < last_offset+ext_length
			&& in_offset+in_length >= last_offset)
		{
			uint64_t	isect_start, isect_end;

			isect_start = max(in_offset, last_offset);
			isect_end = min(in_offset+in_length, last_offset+ext_length);

			error = hfslib_readd(in_vol, out_bytes, isect_end-isect_start,
				isect_start - last_offset + (uint64_t)in_extents[i].start_block
					* in_vol->vh.block_size, cbargs);

			if (error != 0)
				return error;

			*out_bytesread += isect_end-isect_start;
			out_bytes = (uint8_t*)out_bytes + isect_end-isect_start;
		}

		last_offset += ext_length;
	}

	return 0;
}

#if 0
#pragma mark -
#pragma mark Callback Wrappers
#endif

void
hfslib_error(const char* in_format, const char* in_file, int in_line, ...)
{
	va_list		ap;

	if (in_format == NULL)
		return;

	if (hfs_gcb.error != NULL) {
		va_start(ap, in_line);
		hfs_gcb.error(in_format, in_file, in_line, ap);
		va_end(ap);
	}
}

void*
hfslib_malloc(size_t size, hfs_callback_args* cbargs)
{
	if (hfs_gcb.allocmem != NULL)
		return hfs_gcb.allocmem(size, cbargs);

	return NULL;
}

void*
hfslib_realloc(void* ptr, size_t size, hfs_callback_args* cbargs)
{
	if (hfs_gcb.reallocmem != NULL)
		return hfs_gcb.reallocmem(ptr, size, cbargs);

	return NULL;
}

void
hfslib_free(void* ptr, hfs_callback_args* cbargs)
{
	if (hfs_gcb.freemem != NULL && ptr != NULL)
		hfs_gcb.freemem(ptr, cbargs);
}

int
hfslib_openvoldevice(
	hfs_volume* in_vol,
	const char* in_device,
	hfs_callback_args* cbargs)
{
	if (hfs_gcb.openvol != NULL && in_device != NULL)
		return hfs_gcb.openvol(in_vol, in_device, cbargs);

	return 1;
}

void
hfslib_closevoldevice(hfs_volume* in_vol, hfs_callback_args* cbargs)
{
	if (hfs_gcb.closevol != NULL)
		hfs_gcb.closevol(in_vol, cbargs);
}

int
hfslib_readd(
	hfs_volume* in_vol,
	void* out_bytes,
	uint64_t in_length,
	uint64_t in_offset,
	hfs_callback_args* cbargs)
{
	if (in_vol == NULL || out_bytes == NULL)
		return -1;

	if (hfs_gcb.read != NULL)
		return hfs_gcb.read(in_vol, out_bytes, in_length, in_offset, cbargs);

	return -1;
}

#if 0
#pragma mark -
#pragma mark Other
#endif

/* returns key length */
uint16_t
hfslib_make_catalog_key(
	hfs_cnid_t in_parent_cnid,
	uint16_t in_name_len,
	unichar_t* in_unicode,
	hfs_catalog_key_t* out_key)
{
	if (in_parent_cnid == 0 || (in_name_len > 0 && in_unicode == NULL) ||
	    out_key == 0)
		return 0;

	if (in_name_len > 255)
		in_name_len = 255;

	out_key->key_len = 6 + 2 * in_name_len;
	out_key->parent_cnid = in_parent_cnid;
	out_key->name.length = in_name_len;
	if (in_name_len > 0)
		memcpy(&out_key->name.unicode, in_unicode, in_name_len*2);

	return out_key->key_len;
}

/* returns key length */
uint16_t
hfslib_make_extent_key(
	hfs_cnid_t in_cnid,
	uint8_t in_forktype,
	uint32_t in_startblock,
	hfs_extent_key_t* out_key)
{
	if (in_cnid == 0 || out_key == 0)
		return 0;

	out_key->key_length = HFS_MAX_EXT_KEY_LEN;
	out_key->fork_type = in_forktype;
	out_key->padding = 0;
	out_key->file_cnid = in_cnid;
	out_key->start_block = in_startblock;

	return out_key->key_length;
}

/* case-folding */
int
hfslib_compare_catalog_keys_cf (
	const void *ap,
	const void *bp)
{
	const hfs_catalog_key_t	*a, *b;
	unichar_t	ac, bc; /* current character from a, b */
	unichar_t	lc; /* lowercase version of current character */
	uint8_t		apos, bpos; /* current character indices */

	a = (const hfs_catalog_key_t*)ap;
	b = (const hfs_catalog_key_t*)bp;

	if (a->parent_cnid != b->parent_cnid) {
		return (a->parent_cnid - b->parent_cnid);
	} else {
		/*
		 * The following code implements the pseudocode suggested by
		 * the HFS+ technote.
		 */

/*
 * XXX These need to be revised to be endian-independent!
 */
#define hbyte(x) ((x) >> 8)
#define lbyte(x) ((x) & 0x00FF)

		apos = bpos = 0;
		while (1)
		{
			/* get next valid character from a */
			for (lc = 0; lc == 0 && apos < a->name.length; apos++) {
				ac = a->name.unicode[apos];
				lc = hfs_gcft[hbyte(ac)];
				if (lc == 0)
					lc = ac;
				else
					lc = hfs_gcft[lc + lbyte(ac)];
			};
			ac = lc;

			/* get next valid character from b */
			for (lc = 0; lc == 0 && bpos < b->name.length; bpos++) {
				bc = b->name.unicode[bpos];
				lc = hfs_gcft[hbyte(bc)];
				if (lc == 0)
					lc = bc;
				else
					lc = hfs_gcft[lc + lbyte(bc)];
			};
			bc = lc;

			/* on end of string ac/bc are 0, otherwise > 0 */
			if (ac != bc || (ac == 0 && bc == 0))
				return ac - bc;
		}
#undef hbyte
#undef lbyte
	}
}

static int
unichar_cmp (
	const unichar_t* a,
	const unichar_t* b,
	size_t num_chars)
{
	if (num_chars == 0)
		return 0;
	const unichar_t* stop = a + num_chars;
	while (*a == *b) {
		++a;
		++b;
		if (a == stop)
			return 0;
	}
	return *a - *b;
}

/* binary compare (i.e., not case folding) */
int
hfslib_compare_catalog_keys_bc (
	const void *ap,
	const void *bp)
{
	int c;
	const hfs_catalog_key_t *a, *b;

	a = (const hfs_catalog_key_t *) ap;
	b = (const hfs_catalog_key_t *) bp;

	if (a->parent_cnid == b->parent_cnid)
	{
		c = unichar_cmp(a->name.unicode, b->name.unicode,
			min(a->name.length, b->name.length));
		if (c != 0)
			return c;
		/*
		 * all other things being equal, the key with the shorter name sorts first.
		 * this also handles the case where the keys are equivalent (a.len - b.len = 0).
		 */
		return a->name.length - b->name.length;
	} else {
		return (a->parent_cnid - b->parent_cnid);
	}
}

int
hfslib_compare_extent_keys (
	const void *ap,
	const void *bp)
{
	/*
	 *	Comparison order, in descending importance:
	 *
	 *		CNID -> fork type -> start block
	 */

	const hfs_extent_key_t *a, *b;
	a = (const hfs_extent_key_t *) ap;
	b = (const hfs_extent_key_t *) bp;

	if (a->file_cnid == b->file_cnid)
	{
		if (a->fork_type == b->fork_type)
		{
			if (a->start_block == b->start_block)
			{
				return 0;
			} else {
				return (a->start_block - b->start_block);
			}
		} else {
			return (a->fork_type - b->fork_type);
		}
	} else {
		return (a->file_cnid - b->file_cnid);
	}
}

/* 1+10 tables of 16 rows and 16 columns, each 2 bytes wide = 5632 bytes */
const unichar_t hfs_gcft[] = {
    /* high byte indices */
    0x0100,0x0200,0x0000,0x0300,0x0400,0x0500,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0600,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0700,0x0800,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0900,0x0a00,
    /* table 1 (high byte 00) */
    0xffff,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,0x0008,0x0009,0x000a,0x000b,0x000c,0x000d,0x000e,0x000f,
    0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,0x0018,0x0019,0x001a,0x001b,0x001c,0x001d,0x001e,0x001f,
    0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002a,0x002b,0x002c,0x002d,0x002e,0x002f,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003a,0x003b,0x003c,0x003d,0x003e,0x003f,
    0x0040,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006a,0x006b,0x006c,0x006d,0x006e,0x006f,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007a,0x005b,0x005c,0x005d,0x005e,0x005f,
    0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006a,0x006b,0x006c,0x006d,0x006e,0x006f,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007a,0x007b,0x007c,0x007d,0x007e,0x007f,
    0x0080,0x0081,0x0082,0x0083,0x0084,0x0085,0x0086,0x0087,0x0088,0x0089,0x008a,0x008b,0x008c,0x008d,0x008e,0x008f,
    0x0090,0x0091,0x0092,0x0093,0x0094,0x0095,0x0096,0x0097,0x0098,0x0099,0x009a,0x009b,0x009c,0x009d,0x009e,0x009f,
    0x00a0,0x00a1,0x00a2,0x00a3,0x00a4,0x00a5,0x00a6,0x00a7,0x00a8,0x00a9,0x00aa,0x00ab,0x00ac,0x00ad,0x00ae,0x00af,
    0x00b0,0x00b1,0x00b2,0x00b3,0x00b4,0x00b5,0x00b6,0x00b7,0x00b8,0x00b9,0x00ba,0x00bb,0x00bc,0x00bd,0x00be,0x00bf,
    0x00c0,0x00c1,0x00c2,0x00c3,0x00c4,0x00c5,0x00e6,0x00c7,0x00c8,0x00c9,0x00ca,0x00cb,0x00cc,0x00cd,0x00ce,0x00cf,
    0x00f0,0x00d1,0x00d2,0x00d3,0x00d4,0x00d5,0x00d6,0x00d7,0x00f8,0x00d9,0x00da,0x00db,0x00dc,0x00dd,0x00fe,0x00df,
    0x00e0,0x00e1,0x00e2,0x00e3,0x00e4,0x00e5,0x00e6,0x00e7,0x00e8,0x00e9,0x00ea,0x00eb,0x00ec,0x00ed,0x00ee,0x00ef,
    0x00f0,0x00f1,0x00f2,0x00f3,0x00f4,0x00f5,0x00f6,0x00f7,0x00f8,0x00f9,0x00fa,0x00fb,0x00fc,0x00fd,0x00fe,0x00ff,
    /* table 2 (high byte 01) */
    0x0100,0x0101,0x0102,0x0103,0x0104,0x0105,0x0106,0x0107,0x0108,0x0109,0x010a,0x010b,0x010c,0x010d,0x010e,0x010f,
    0x0111,0x0111,0x0112,0x0113,0x0114,0x0115,0x0116,0x0117,0x0118,0x0119,0x011a,0x011b,0x011c,0x011d,0x011e,0x011f,
    0x0120,0x0121,0x0122,0x0123,0x0124,0x0125,0x0127,0x0127,0x0128,0x0129,0x012a,0x012b,0x012c,0x012d,0x012e,0x012f,
    0x0130,0x0131,0x0133,0x0133,0x0134,0x0135,0x0136,0x0137,0x0138,0x0139,0x013a,0x013b,0x013c,0x013d,0x013e,0x0140,
    0x0140,0x0142,0x0142,0x0143,0x0144,0x0145,0x0146,0x0147,0x0148,0x0149,0x014b,0x014b,0x014c,0x014d,0x014e,0x014f,
    0x0150,0x0151,0x0153,0x0153,0x0154,0x0155,0x0156,0x0157,0x0158,0x0159,0x015a,0x015b,0x015c,0x015d,0x015e,0x015f,
    0x0160,0x0161,0x0162,0x0163,0x0164,0x0165,0x0167,0x0167,0x0168,0x0169,0x016a,0x016b,0x016c,0x016d,0x016e,0x016f,
    0x0170,0x0171,0x0172,0x0173,0x0174,0x0175,0x0176,0x0177,0x0178,0x0179,0x017a,0x017b,0x017c,0x017d,0x017e,0x017f,
    0x0180,0x0253,0x0183,0x0183,0x0185,0x0185,0x0254,0x0188,0x0188,0x0256,0x0257,0x018c,0x018c,0x018d,0x01dd,0x0259,
    0x025b,0x0192,0x0192,0x0260,0x0263,0x0195,0x0269,0x0268,0x0199,0x0199,0x019a,0x019b,0x026f,0x0272,0x019e,0x0275,
    0x01a0,0x01a1,0x01a3,0x01a3,0x01a5,0x01a5,0x01a6,0x01a8,0x01a8,0x0283,0x01aa,0x01ab,0x01ad,0x01ad,0x0288,0x01af,
    0x01b0,0x028a,0x028b,0x01b4,0x01b4,0x01b6,0x01b6,0x0292,0x01b9,0x01b9,0x01ba,0x01bb,0x01bd,0x01bd,0x01be,0x01bf,
    0x01c0,0x01c1,0x01c2,0x01c3,0x01c6,0x01c6,0x01c6,0x01c9,0x01c9,0x01c9,0x01cc,0x01cc,0x01cc,0x01cd,0x01ce,0x01cf,
    0x01d0,0x01d1,0x01d2,0x01d3,0x01d4,0x01d5,0x01d6,0x01d7,0x01d8,0x01d9,0x01da,0x01db,0x01dc,0x01dd,0x01de,0x01df,
    0x01e0,0x01e1,0x01e2,0x01e3,0x01e5,0x01e5,0x01e6,0x01e7,0x01e8,0x01e9,0x01ea,0x01eb,0x01ec,0x01ed,0x01ee,0x01ef,
    0x01f0,0x01f3,0x01f3,0x01f3,0x01f4,0x01f5,0x01f6,0x01f7,0x01f8,0x01f9,0x01fa,0x01fb,0x01fc,0x01fd,0x01fe,0x01ff,
    /* table 3 (high byte 03) */
    0x0300,0x0301,0x0302,0x0303,0x0304,0x0305,0x0306,0x0307,0x0308,0x0309,0x030a,0x030b,0x030c,0x030d,0x030e,0x030f,
    0x0310,0x0311,0x0312,0x0313,0x0314,0x0315,0x0316,0x0317,0x0318,0x0319,0x031a,0x031b,0x031c,0x031d,0x031e,0x031f,
    0x0320,0x0321,0x0322,0x0323,0x0324,0x0325,0x0326,0x0327,0x0328,0x0329,0x032a,0x032b,0x032c,0x032d,0x032e,0x032f,
    0x0330,0x0331,0x0332,0x0333,0x0334,0x0335,0x0336,0x0337,0x0338,0x0339,0x033a,0x033b,0x033c,0x033d,0x033e,0x033f,
    0x0340,0x0341,0x0342,0x0343,0x0344,0x0345,0x0346,0x0347,0x0348,0x0349,0x034a,0x034b,0x034c,0x034d,0x034e,0x034f,
    0x0350,0x0351,0x0352,0x0353,0x0354,0x0355,0x0356,0x0357,0x0358,0x0359,0x035a,0x035b,0x035c,0x035d,0x035e,0x035f,
    0x0360,0x0361,0x0362,0x0363,0x0364,0x0365,0x0366,0x0367,0x0368,0x0369,0x036a,0x036b,0x036c,0x036d,0x036e,0x036f,
    0x0370,0x0371,0x0372,0x0373,0x0374,0x0375,0x0376,0x0377,0x0378,0x0379,0x037a,0x037b,0x037c,0x037d,0x037e,0x037f,
    0x0380,0x0381,0x0382,0x0383,0x0384,0x0385,0x0386,0x0387,0x0388,0x0389,0x038a,0x038b,0x038c,0x038d,0x038e,0x038f,
    0x0390,0x03b1,0x03b2,0x03b3,0x03b4,0x03b5,0x03b6,0x03b7,0x03b8,0x03b9,0x03ba,0x03bb,0x03bc,0x03bd,0x03be,0x03bf,
    0x03c0,0x03c1,0x03a2,0x03c3,0x03c4,0x03c5,0x03c6,0x03c7,0x03c8,0x03c9,0x03aa,0x03ab,0x03ac,0x03ad,0x03ae,0x03af,
    0x03b0,0x03b1,0x03b2,0x03b3,0x03b4,0x03b5,0x03b6,0x03b7,0x03b8,0x03b9,0x03ba,0x03bb,0x03bc,0x03bd,0x03be,0x03bf,
    0x03c0,0x03c1,0x03c2,0x03c3,0x03c4,0x03c5,0x03c6,0x03c7,0x03c8,0x03c9,0x03ca,0x03cb,0x03cc,0x03cd,0x03ce,0x03cf,
    0x03d0,0x03d1,0x03d2,0x03d3,0x03d4,0x03d5,0x03d6,0x03d7,0x03d8,0x03d9,0x03da,0x03db,0x03dc,0x03dd,0x03de,0x03df,
    0x03e0,0x03e1,0x03e3,0x03e3,0x03e5,0x03e5,0x03e7,0x03e7,0x03e9,0x03e9,0x03eb,0x03eb,0x03ed,0x03ed,0x03ef,0x03ef,
    0x03f0,0x03f1,0x03f2,0x03f3,0x03f4,0x03f5,0x03f6,0x03f7,0x03f8,0x03f9,0x03fa,0x03fb,0x03fc,0x03fd,0x03fe,0x03ff,
    /* table 4 (high byte 04) */
    0x0400,0x0401,0x0452,0x0403,0x0454,0x0455,0x0456,0x0407,0x0458,0x0459,0x045a,0x045b,0x040c,0x040d,0x040e,0x045f,
    0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,0x0438,0x0419,0x043a,0x043b,0x043c,0x043d,0x043e,0x043f,
    0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,0x0448,0x0449,0x044a,0x044b,0x044c,0x044d,0x044e,0x044f,
    0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,0x0438,0x0439,0x043a,0x043b,0x043c,0x043d,0x043e,0x043f,
    0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,0x0448,0x0449,0x044a,0x044b,0x044c,0x044d,0x044e,0x044f,
    0x0450,0x0451,0x0452,0x0453,0x0454,0x0455,0x0456,0x0457,0x0458,0x0459,0x045a,0x045b,0x045c,0x045d,0x045e,0x045f,
    0x0461,0x0461,0x0463,0x0463,0x0465,0x0465,0x0467,0x0467,0x0469,0x0469,0x046b,0x046b,0x046d,0x046d,0x046f,0x046f,
    0x0471,0x0471,0x0473,0x0473,0x0475,0x0475,0x0476,0x0477,0x0479,0x0479,0x047b,0x047b,0x047d,0x047d,0x047f,0x047f,
    0x0481,0x0481,0x0482,0x0483,0x0484,0x0485,0x0486,0x0487,0x0488,0x0489,0x048a,0x048b,0x048c,0x048d,0x048e,0x048f,
    0x0491,0x0491,0x0493,0x0493,0x0495,0x0495,0x0497,0x0497,0x0499,0x0499,0x049b,0x049b,0x049d,0x049d,0x049f,0x049f,
    0x04a1,0x04a1,0x04a3,0x04a3,0x04a5,0x04a5,0x04a7,0x04a7,0x04a9,0x04a9,0x04ab,0x04ab,0x04ad,0x04ad,0x04af,0x04af,
    0x04b1,0x04b1,0x04b3,0x04b3,0x04b5,0x04b5,0x04b7,0x04b7,0x04b9,0x04b9,0x04bb,0x04bb,0x04bd,0x04bd,0x04bf,0x04bf,
    0x04c0,0x04c1,0x04c2,0x04c4,0x04c4,0x04c5,0x04c6,0x04c8,0x04c8,0x04c9,0x04ca,0x04cc,0x04cc,0x04cd,0x04ce,0x04cf,
    0x04d0,0x04d1,0x04d2,0x04d3,0x04d4,0x04d5,0x04d6,0x04d7,0x04d8,0x04d9,0x04da,0x04db,0x04dc,0x04dd,0x04de,0x04df,
    0x04e0,0x04e1,0x04e2,0x04e3,0x04e4,0x04e5,0x04e6,0x04e7,0x04e8,0x04e9,0x04ea,0x04eb,0x04ec,0x04ed,0x04ee,0x04ef,
    0x04f0,0x04f1,0x04f2,0x04f3,0x04f4,0x04f5,0x04f6,0x04f7,0x04f8,0x04f9,0x04fa,0x04fb,0x04fc,0x04fd,0x04fe,0x04ff,
    /* table 5 (high byte 05) */
    0x0500,0x0501,0x0502,0x0503,0x0504,0x0505,0x0506,0x0507,0x0508,0x0509,0x050a,0x050b,0x050c,0x050d,0x050e,0x050f,
    0x0510,0x0511,0x0512,0x0513,0x0514,0x0515,0x0516,0x0517,0x0518,0x0519,0x051a,0x051b,0x051c,0x051d,0x051e,0x051f,
    0x0520,0x0521,0x0522,0x0523,0x0524,0x0525,0x0526,0x0527,0x0528,0x0529,0x052a,0x052b,0x052c,0x052d,0x052e,0x052f,
    0x0530,0x0561,0x0562,0x0563,0x0564,0x0565,0x0566,0x0567,0x0568,0x0569,0x056a,0x056b,0x056c,0x056d,0x056e,0x056f,
    0x0570,0x0571,0x0572,0x0573,0x0574,0x0575,0x0576,0x0577,0x0578,0x0579,0x057a,0x057b,0x057c,0x057d,0x057e,0x057f,
    0x0580,0x0581,0x0582,0x0583,0x0584,0x0585,0x0586,0x0557,0x0558,0x0559,0x055a,0x055b,0x055c,0x055d,0x055e,0x055f,
    0x0560,0x0561,0x0562,0x0563,0x0564,0x0565,0x0566,0x0567,0x0568,0x0569,0x056a,0x056b,0x056c,0x056d,0x056e,0x056f,
    0x0570,0x0571,0x0572,0x0573,0x0574,0x0575,0x0576,0x0577,0x0578,0x0579,0x057a,0x057b,0x057c,0x057d,0x057e,0x057f,
    0x0580,0x0581,0x0582,0x0583,0x0584,0x0585,0x0586,0x0587,0x0588,0x0589,0x058a,0x058b,0x058c,0x058d,0x058e,0x058f,
    0x0590,0x0591,0x0592,0x0593,0x0594,0x0595,0x0596,0x0597,0x0598,0x0599,0x059a,0x059b,0x059c,0x059d,0x059e,0x059f,
    0x05a0,0x05a1,0x05a2,0x05a3,0x05a4,0x05a5,0x05a6,0x05a7,0x05a8,0x05a9,0x05aa,0x05ab,0x05ac,0x05ad,0x05ae,0x05af,
    0x05b0,0x05b1,0x05b2,0x05b3,0x05b4,0x05b5,0x05b6,0x05b7,0x05b8,0x05b9,0x05ba,0x05bb,0x05bc,0x05bd,0x05be,0x05bf,
    0x05c0,0x05c1,0x05c2,0x05c3,0x05c4,0x05c5,0x05c6,0x05c7,0x05c8,0x05c9,0x05ca,0x05cb,0x05cc,0x05cd,0x05ce,0x05cf,
    0x05d0,0x05d1,0x05d2,0x05d3,0x05d4,0x05d5,0x05d6,0x05d7,0x05d8,0x05d9,0x05da,0x05db,0x05dc,0x05dd,0x05de,0x05df,
    0x05e0,0x05e1,0x05e2,0x05e3,0x05e4,0x05e5,0x05e6,0x05e7,0x05e8,0x05e9,0x05ea,0x05eb,0x05ec,0x05ed,0x05ee,0x05ef,
    0x05f0,0x05f1,0x05f2,0x05f3,0x05f4,0x05f5,0x05f6,0x05f7,0x05f8,0x05f9,0x05fa,0x05fb,0x05fc,0x05fd,0x05fe,0x05ff,
    /* table 6 (high byte 10) */
    0x1000,0x1001,0x1002,0x1003,0x1004,0x1005,0x1006,0x1007,0x1008,0x1009,0x100a,0x100b,0x100c,0x100d,0x100e,0x100f,
    0x1010,0x1011,0x1012,0x1013,0x1014,0x1015,0x1016,0x1017,0x1018,0x1019,0x101a,0x101b,0x101c,0x101d,0x101e,0x101f,
    0x1020,0x1021,0x1022,0x1023,0x1024,0x1025,0x1026,0x1027,0x1028,0x1029,0x102a,0x102b,0x102c,0x102d,0x102e,0x102f,
    0x1030,0x1031,0x1032,0x1033,0x1034,0x1035,0x1036,0x1037,0x1038,0x1039,0x103a,0x103b,0x103c,0x103d,0x103e,0x103f,
    0x1040,0x1041,0x1042,0x1043,0x1044,0x1045,0x1046,0x1047,0x1048,0x1049,0x104a,0x104b,0x104c,0x104d,0x104e,0x104f,
    0x1050,0x1051,0x1052,0x1053,0x1054,0x1055,0x1056,0x1057,0x1058,0x1059,0x105a,0x105b,0x105c,0x105d,0x105e,0x105f,
    0x1060,0x1061,0x1062,0x1063,0x1064,0x1065,0x1066,0x1067,0x1068,0x1069,0x106a,0x106b,0x106c,0x106d,0x106e,0x106f,
    0x1070,0x1071,0x1072,0x1073,0x1074,0x1075,0x1076,0x1077,0x1078,0x1079,0x107a,0x107b,0x107c,0x107d,0x107e,0x107f,
    0x1080,0x1081,0x1082,0x1083,0x1084,0x1085,0x1086,0x1087,0x1088,0x1089,0x108a,0x108b,0x108c,0x108d,0x108e,0x108f,
    0x1090,0x1091,0x1092,0x1093,0x1094,0x1095,0x1096,0x1097,0x1098,0x1099,0x109a,0x109b,0x109c,0x109d,0x109e,0x109f,
    0x10d0,0x10d1,0x10d2,0x10d3,0x10d4,0x10d5,0x10d6,0x10d7,0x10d8,0x10d9,0x10da,0x10db,0x10dc,0x10dd,0x10de,0x10df,
    0x10e0,0x10e1,0x10e2,0x10e3,0x10e4,0x10e5,0x10e6,0x10e7,0x10e8,0x10e9,0x10ea,0x10eb,0x10ec,0x10ed,0x10ee,0x10ef,
    0x10f0,0x10f1,0x10f2,0x10f3,0x10f4,0x10f5,0x10c6,0x10c7,0x10c8,0x10c9,0x10ca,0x10cb,0x10cc,0x10cd,0x10ce,0x10cf,
    0x10d0,0x10d1,0x10d2,0x10d3,0x10d4,0x10d5,0x10d6,0x10d7,0x10d8,0x10d9,0x10da,0x10db,0x10dc,0x10dd,0x10de,0x10df,
    0x10e0,0x10e1,0x10e2,0x10e3,0x10e4,0x10e5,0x10e6,0x10e7,0x10e8,0x10e9,0x10ea,0x10eb,0x10ec,0x10ed,0x10ee,0x10ef,
    0x10f0,0x10f1,0x10f2,0x10f3,0x10f4,0x10f5,0x10f6,0x10f7,0x10f8,0x10f9,0x10fa,0x10fb,0x10fc,0x10fd,0x10fe,0x10ff,
    /* table 7 (high byte 20) */
    0x2000,0x2001,0x2002,0x2003,0x2004,0x2005,0x2006,0x2007,0x2008,0x2009,0x200a,0x200b,0x0000,0x0000,0x0000,0x0000,
    0x2010,0x2011,0x2012,0x2013,0x2014,0x2015,0x2016,0x2017,0x2018,0x2019,0x201a,0x201b,0x201c,0x201d,0x201e,0x201f,
    0x2020,0x2021,0x2022,0x2023,0x2024,0x2025,0x2026,0x2027,0x2028,0x2029,0x0000,0x0000,0x0000,0x0000,0x0000,0x202f,
    0x2030,0x2031,0x2032,0x2033,0x2034,0x2035,0x2036,0x2037,0x2038,0x2039,0x203a,0x203b,0x203c,0x203d,0x203e,0x203f,
    0x2040,0x2041,0x2042,0x2043,0x2044,0x2045,0x2046,0x2047,0x2048,0x2049,0x204a,0x204b,0x204c,0x204d,0x204e,0x204f,
    0x2050,0x2051,0x2052,0x2053,0x2054,0x2055,0x2056,0x2057,0x2058,0x2059,0x205a,0x205b,0x205c,0x205d,0x205e,0x205f,
    0x2060,0x2061,0x2062,0x2063,0x2064,0x2065,0x2066,0x2067,0x2068,0x2069,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x2070,0x2071,0x2072,0x2073,0x2074,0x2075,0x2076,0x2077,0x2078,0x2079,0x207a,0x207b,0x207c,0x207d,0x207e,0x207f,
    0x2080,0x2081,0x2082,0x2083,0x2084,0x2085,0x2086,0x2087,0x2088,0x2089,0x208a,0x208b,0x208c,0x208d,0x208e,0x208f,
    0x2090,0x2091,0x2092,0x2093,0x2094,0x2095,0x2096,0x2097,0x2098,0x2099,0x209a,0x209b,0x209c,0x209d,0x209e,0x209f,
    0x20a0,0x20a1,0x20a2,0x20a3,0x20a4,0x20a5,0x20a6,0x20a7,0x20a8,0x20a9,0x20aa,0x20ab,0x20ac,0x20ad,0x20ae,0x20af,
    0x20b0,0x20b1,0x20b2,0x20b3,0x20b4,0x20b5,0x20b6,0x20b7,0x20b8,0x20b9,0x20ba,0x20bb,0x20bc,0x20bd,0x20be,0x20bf,
    0x20c0,0x20c1,0x20c2,0x20c3,0x20c4,0x20c5,0x20c6,0x20c7,0x20c8,0x20c9,0x20ca,0x20cb,0x20cc,0x20cd,0x20ce,0x20cf,
    0x20d0,0x20d1,0x20d2,0x20d3,0x20d4,0x20d5,0x20d6,0x20d7,0x20d8,0x20d9,0x20da,0x20db,0x20dc,0x20dd,0x20de,0x20df,
    0x20e0,0x20e1,0x20e2,0x20e3,0x20e4,0x20e5,0x20e6,0x20e7,0x20e8,0x20e9,0x20ea,0x20eb,0x20ec,0x20ed,0x20ee,0x20ef,
    0x20f0,0x20f1,0x20f2,0x20f3,0x20f4,0x20f5,0x20f6,0x20f7,0x20f8,0x20f9,0x20fa,0x20fb,0x20fc,0x20fd,0x20fe,0x20ff,
    /* table 8 (high byte 21) */
    0x2100,0x2101,0x2102,0x2103,0x2104,0x2105,0x2106,0x2107,0x2108,0x2109,0x210a,0x210b,0x210c,0x210d,0x210e,0x210f,
    0x2110,0x2111,0x2112,0x2113,0x2114,0x2115,0x2116,0x2117,0x2118,0x2119,0x211a,0x211b,0x211c,0x211d,0x211e,0x211f,
    0x2120,0x2121,0x2122,0x2123,0x2124,0x2125,0x2126,0x2127,0x2128,0x2129,0x212a,0x212b,0x212c,0x212d,0x212e,0x212f,
    0x2130,0x2131,0x2132,0x2133,0x2134,0x2135,0x2136,0x2137,0x2138,0x2139,0x213a,0x213b,0x213c,0x213d,0x213e,0x213f,
    0x2140,0x2141,0x2142,0x2143,0x2144,0x2145,0x2146,0x2147,0x2148,0x2149,0x214a,0x214b,0x214c,0x214d,0x214e,0x214f,
    0x2150,0x2151,0x2152,0x2153,0x2154,0x2155,0x2156,0x2157,0x2158,0x2159,0x215a,0x215b,0x215c,0x215d,0x215e,0x215f,
    0x2170,0x2171,0x2172,0x2173,0x2174,0x2175,0x2176,0x2177,0x2178,0x2179,0x217a,0x217b,0x217c,0x217d,0x217e,0x217f,
    0x2170,0x2171,0x2172,0x2173,0x2174,0x2175,0x2176,0x2177,0x2178,0x2179,0x217a,0x217b,0x217c,0x217d,0x217e,0x217f,
    0x2180,0x2181,0x2182,0x2183,0x2184,0x2185,0x2186,0x2187,0x2188,0x2189,0x218a,0x218b,0x218c,0x218d,0x218e,0x218f,
    0x2190,0x2191,0x2192,0x2193,0x2194,0x2195,0x2196,0x2197,0x2198,0x2199,0x219a,0x219b,0x219c,0x219d,0x219e,0x219f,
    0x21a0,0x21a1,0x21a2,0x21a3,0x21a4,0x21a5,0x21a6,0x21a7,0x21a8,0x21a9,0x21aa,0x21ab,0x21ac,0x21ad,0x21ae,0x21af,
    0x21b0,0x21b1,0x21b2,0x21b3,0x21b4,0x21b5,0x21b6,0x21b7,0x21b8,0x21b9,0x21ba,0x21bb,0x21bc,0x21bd,0x21be,0x21bf,
    0x21c0,0x21c1,0x21c2,0x21c3,0x21c4,0x21c5,0x21c6,0x21c7,0x21c8,0x21c9,0x21ca,0x21cb,0x21cc,0x21cd,0x21ce,0x21cf,
    0x21d0,0x21d1,0x21d2,0x21d3,0x21d4,0x21d5,0x21d6,0x21d7,0x21d8,0x21d9,0x21da,0x21db,0x21dc,0x21dd,0x21de,0x21df,
    0x21e0,0x21e1,0x21e2,0x21e3,0x21e4,0x21e5,0x21e6,0x21e7,0x21e8,0x21e9,0x21ea,0x21eb,0x21ec,0x21ed,0x21ee,0x21ef,
    0x21f0,0x21f1,0x21f2,0x21f3,0x21f4,0x21f5,0x21f6,0x21f7,0x21f8,0x21f9,0x21fa,0x21fb,0x21fc,0x21fd,0x21fe,0x21ff,
    /* table 9 (high byte FE) */
    0xfe00,0xfe01,0xfe02,0xfe03,0xfe04,0xfe05,0xfe06,0xfe07,0xfe08,0xfe09,0xfe0a,0xfe0b,0xfe0c,0xfe0d,0xfe0e,0xfe0f,
    0xfe10,0xfe11,0xfe12,0xfe13,0xfe14,0xfe15,0xfe16,0xfe17,0xfe18,0xfe19,0xfe1a,0xfe1b,0xfe1c,0xfe1d,0xfe1e,0xfe1f,
    0xfe20,0xfe21,0xfe22,0xfe23,0xfe24,0xfe25,0xfe26,0xfe27,0xfe28,0xfe29,0xfe2a,0xfe2b,0xfe2c,0xfe2d,0xfe2e,0xfe2f,
    0xfe30,0xfe31,0xfe32,0xfe33,0xfe34,0xfe35,0xfe36,0xfe37,0xfe38,0xfe39,0xfe3a,0xfe3b,0xfe3c,0xfe3d,0xfe3e,0xfe3f,
    0xfe40,0xfe41,0xfe42,0xfe43,0xfe44,0xfe45,0xfe46,0xfe47,0xfe48,0xfe49,0xfe4a,0xfe4b,0xfe4c,0xfe4d,0xfe4e,0xfe4f,
    0xfe50,0xfe51,0xfe52,0xfe53,0xfe54,0xfe55,0xfe56,0xfe57,0xfe58,0xfe59,0xfe5a,0xfe5b,0xfe5c,0xfe5d,0xfe5e,0xfe5f,
    0xfe60,0xfe61,0xfe62,0xfe63,0xfe64,0xfe65,0xfe66,0xfe67,0xfe68,0xfe69,0xfe6a,0xfe6b,0xfe6c,0xfe6d,0xfe6e,0xfe6f,
    0xfe70,0xfe71,0xfe72,0xfe73,0xfe74,0xfe75,0xfe76,0xfe77,0xfe78,0xfe79,0xfe7a,0xfe7b,0xfe7c,0xfe7d,0xfe7e,0xfe7f,
    0xfe80,0xfe81,0xfe82,0xfe83,0xfe84,0xfe85,0xfe86,0xfe87,0xfe88,0xfe89,0xfe8a,0xfe8b,0xfe8c,0xfe8d,0xfe8e,0xfe8f,
    0xfe90,0xfe91,0xfe92,0xfe93,0xfe94,0xfe95,0xfe96,0xfe97,0xfe98,0xfe99,0xfe9a,0xfe9b,0xfe9c,0xfe9d,0xfe9e,0xfe9f,
    0xfea0,0xfea1,0xfea2,0xfea3,0xfea4,0xfea5,0xfea6,0xfea7,0xfea8,0xfea9,0xfeaa,0xfeab,0xfeac,0xfead,0xfeae,0xfeaf,
    0xfeb0,0xfeb1,0xfeb2,0xfeb3,0xfeb4,0xfeb5,0xfeb6,0xfeb7,0xfeb8,0xfeb9,0xfeba,0xfebb,0xfebc,0xfebd,0xfebe,0xfebf,
    0xfec0,0xfec1,0xfec2,0xfec3,0xfec4,0xfec5,0xfec6,0xfec7,0xfec8,0xfec9,0xfeca,0xfecb,0xfecc,0xfecd,0xfece,0xfecf,
    0xfed0,0xfed1,0xfed2,0xfed3,0xfed4,0xfed5,0xfed6,0xfed7,0xfed8,0xfed9,0xfeda,0xfedb,0xfedc,0xfedd,0xfede,0xfedf,
    0xfee0,0xfee1,0xfee2,0xfee3,0xfee4,0xfee5,0xfee6,0xfee7,0xfee8,0xfee9,0xfeea,0xfeeb,0xfeec,0xfeed,0xfeee,0xfeef,
    0xfef0,0xfef1,0xfef2,0xfef3,0xfef4,0xfef5,0xfef6,0xfef7,0xfef8,0xfef9,0xfefa,0xfefb,0xfefc,0xfefd,0xfefe,0x0000,
    /* table 10 (high byte FF) */
    0xff00,0xff01,0xff02,0xff03,0xff04,0xff05,0xff06,0xff07,0xff08,0xff09,0xff0a,0xff0b,0xff0c,0xff0d,0xff0e,0xff0f,
    0xff10,0xff11,0xff12,0xff13,0xff14,0xff15,0xff16,0xff17,0xff18,0xff19,0xff1a,0xff1b,0xff1c,0xff1d,0xff1e,0xff1f,
    0xff20,0xff41,0xff42,0xff43,0xff44,0xff45,0xff46,0xff47,0xff48,0xff49,0xff4a,0xff4b,0xff4c,0xff4d,0xff4e,0xff4f,
    0xff50,0xff51,0xff52,0xff53,0xff54,0xff55,0xff56,0xff57,0xff58,0xff59,0xff5a,0xff3b,0xff3c,0xff3d,0xff3e,0xff3f,
    0xff40,0xff41,0xff42,0xff43,0xff44,0xff45,0xff46,0xff47,0xff48,0xff49,0xff4a,0xff4b,0xff4c,0xff4d,0xff4e,0xff4f,
    0xff50,0xff51,0xff52,0xff53,0xff54,0xff55,0xff56,0xff57,0xff58,0xff59,0xff5a,0xff5b,0xff5c,0xff5d,0xff5e,0xff5f,
    0xff60,0xff61,0xff62,0xff63,0xff64,0xff65,0xff66,0xff67,0xff68,0xff69,0xff6a,0xff6b,0xff6c,0xff6d,0xff6e,0xff6f,
    0xff70,0xff71,0xff72,0xff73,0xff74,0xff75,0xff76,0xff77,0xff78,0xff79,0xff7a,0xff7b,0xff7c,0xff7d,0xff7e,0xff7f,
    0xff80,0xff81,0xff82,0xff83,0xff84,0xff85,0xff86,0xff87,0xff88,0xff89,0xff8a,0xff8b,0xff8c,0xff8d,0xff8e,0xff8f,
    0xff90,0xff91,0xff92,0xff93,0xff94,0xff95,0xff96,0xff97,0xff98,0xff99,0xff9a,0xff9b,0xff9c,0xff9d,0xff9e,0xff9f,
    0xffa0,0xffa1,0xffa2,0xffa3,0xffa4,0xffa5,0xffa6,0xffa7,0xffa8,0xffa9,0xffaa,0xffab,0xffac,0xffad,0xffae,0xffaf,
    0xffb0,0xffb1,0xffb2,0xffb3,0xffb4,0xffb5,0xffb6,0xffb7,0xffb8,0xffb9,0xffba,0xffbb,0xffbc,0xffbd,0xffbe,0xffbf,
    0xffc0,0xffc1,0xffc2,0xffc3,0xffc4,0xffc5,0xffc6,0xffc7,0xffc8,0xffc9,0xffca,0xffcb,0xffcc,0xffcd,0xffce,0xffcf,
    0xffd0,0xffd1,0xffd2,0xffd3,0xffd4,0xffd5,0xffd6,0xffd7,0xffd8,0xffd9,0xffda,0xffdb,0xffdc,0xffdd,0xffde,0xffdf,
    0xffe0,0xffe1,0xffe2,0xffe3,0xffe4,0xffe5,0xffe6,0xffe7,0xffe8,0xffe9,0xffea,0xffeb,0xffec,0xffed,0xffee,0xffef,
    0xfff0,0xfff1,0xfff2,0xfff3,0xfff4,0xfff5,0xfff6,0xfff7,0xfff8,0xfff9,0xfffa,0xfffb,0xfffc,0xfffd,0xfffe,0xffff
};

int
hfslib_get_hardlink(hfs_volume *vol, uint32_t inode_num,
		     hfs_catalog_keyed_record_t *rec,
		     hfs_callback_args *cbargs)
{
	hfs_catalog_keyed_record_t metadata;
	hfs_catalog_key_t key;
	char name[16];
	unichar_t name_uni[16];
	int i, len;

	/* XXX: cache this */
	if (hfslib_find_catalog_record_with_key(vol,
						 &hfs_gMetadataDirectoryKey,
						 &metadata, cbargs) != 0
		|| metadata.type != HFS_REC_FLDR)
		return -1;

	len = snprintf(name, sizeof(name), "iNode%d", inode_num);
	for (i = 0; i < len; i++)
		name_uni[i] = name[i];

	if (hfslib_make_catalog_key(metadata.folder.cnid, len, name_uni,
				     &key) == 0)
		return -1;

	return hfslib_find_catalog_record_with_key(vol, &key, rec, cbargs);
}

int
hfslib_get_directory_hardlink(hfs_volume *vol, uint32_t inode_num,
		     hfs_catalog_keyed_record_t *rec,
		     hfs_callback_args *cbargs)
{
	hfs_catalog_keyed_record_t metadata;
	hfs_catalog_key_t key;
	char name[16];
	unichar_t name_uni[16];
	int i, len;

	/* XXX: cache this */
	if (hfslib_find_catalog_record_with_key(vol,
						 &hfs_gDirMetadataDirectoryKey,
						 &metadata, cbargs) != 0
		|| metadata.type != HFS_REC_FLDR)
		return -1;

	len = snprintf(name, sizeof(name), "dir_%d", inode_num);
	for (i = 0; i < len; i++)
		name_uni[i] = name[i];

	if (hfslib_make_catalog_key(metadata.folder.cnid, len, name_uni,
				     &key) == 0)
		return -1;

	return hfslib_find_catalog_record_with_key(vol, &key, rec, cbargs);
}
