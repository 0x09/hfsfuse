/*
 * hfsdump - Inspect the contents of an HFS+ volume
 * This file is part of the hfsfuse project.
 */

// TODO: interpret more fields, output formatting

#include "hfsuser.h"

#include <time.h>
#include <inttypes.h>
#include <string.h>

char* hfs_ctime_r(time_t clock, char* buf) {
	char* t = ctime(&clock);
	return t ? memcpy(buf,t,26) : NULL;
}

static inline void* hfs_reallocf(void* ptr, size_t size) {
	void* p = realloc(ptr,size);
	if(!p)
		free(ptr);
	return p;
}

static inline void dump_volume_header(hfs_volume_header_t vh) {
	char ctimebuf[4][26] = {0};
	printf(
		"volume header:\n"
		"signature: %s\n"
		"version: %" PRIu16 "\n"
		"attributes: hwlock %d unmounted %d badblocks %d nocache %d dirty %d cnids recycled %d journaled %d swlock %d\n"
		"last_mounting_version: %s\n"
		"journal_info_block: %" PRIu32 "\n"
		"date_created: %s"
		"date_modified: %s"
		"date_backedup: %s"
		"date_checked: %s"
		"file_count: %" PRIu32 "\n"
		"folder_count: %" PRIu32 "\n"
		"block_size: %" PRIu32 "\n"
		"total_blocks: %" PRIu32 "\n"
		"free_blocks: %" PRIu32 "\n"
		"next_alloc_block: %" PRIu32 "\n"
		"rsrc_clump_size: %" PRIu32 "\n"
		"data_clump_size: %" PRIu32 "\n"
		"next_cnid: %" PRIu32 "\n"
		"write_count: %" PRIu32 "\n"
		"encodings: %" PRIu64 "\n"
		"finderinfo:\n"
			"\tBoot directory ID: %" PRIu32 "\n"
			"\tStartup parent directory ID: %" PRIu32 "\n"
			"\tDisplay directory ID: %" PRIu32 "\n"
			"\tOS classic system directory ID: %" PRIu32 "\n"
			"\tOS X system directory ID: %" PRIu32 "\n"
			"\tVolume unique ID: %" PRIx64 "\n",
		(char[3]){vh.signature>>8,vh.signature&0xFF,'\0'},vh.version,
		(vh.attributes >> HFS_VOL_HWLOCK)&1, (vh.attributes >> HFS_VOL_UNMOUNTED)&1, (vh.attributes >> HFS_VOL_BADBLOCKS)&1,
		(vh.attributes >> HFS_VOL_NOCACHE)&1, (vh.attributes >> HFS_VOL_DIRTY)&1, (vh.attributes >> HFS_VOL_CNIDS_RECYCLED)&1,
		(vh.attributes >> HFS_VOL_JOURNALED)&1, (vh.attributes >> HFS_VOL_SWLOCK)&1,
		(char[5]){vh.last_mounting_version>>24,(vh.last_mounting_version>>16)&0xFF,(vh.last_mounting_version>>8)&0xFF,vh.last_mounting_version&0xFF,'\0'},
		vh.journal_info_block,
		hfs_ctime_r(HFSTIMETOEPOCH(vh.date_created),ctimebuf[0]),hfs_ctime_r(HFSTIMETOEPOCH(vh.date_modified),ctimebuf[1]),
		hfs_ctime_r(HFSTIMETOEPOCH(vh.date_backedup),ctimebuf[2]),hfs_ctime_r(HFSTIMETOEPOCH(vh.date_checked),ctimebuf[3]),
		vh.file_count,vh.folder_count,vh.block_size,vh.total_blocks,
		vh.free_blocks,vh.next_alloc_block,vh.rsrc_clump_size,vh.data_clump_size,vh.next_cnid,vh.write_count,
		vh.encodings,
		vh.finder_info[0],vh.finder_info[1],vh.finder_info[2],vh.finder_info[3],vh.finder_info[5],
		(((uint64_t)vh.finder_info[6])<<32)|vh.finder_info[7]
	);
}

static inline void dump_record(hfs_catalog_keyed_record_t rec) {
	char ctimebuf[5][26] = {0};
	hfs_file_record_t file = rec.file; // dump union keys first
	printf(
		"type: %s\n"
		"flags: %" PRIu16 "\n"
		"cnid: %" PRIu32 "\n"
		"date_created: %s"
		"date_content_mod: %s"
		"date_attrib_mod: %s"
		"date_accessed: %s"
		"date_backedup: %s"
		"encoding: %" PRIu32 "\n"
		"permissions.owner_id: %" PRIu32 "\n"
		"permissions.group_id: %" PRIu32 "\n"
		"permissions.admin_flags: %" PRIu8 "\n"
		"permissions.owner_flags: %" PRIu8 "\n"
		"permissions.file_mode: %ho\n"
		"permissions.special: %" PRIu32 "\n",
		(rec.type == HFS_REC_FLDR ? "folder" : "file"),
		file.flags,
		file.cnid,
		hfs_ctime_r(HFSTIMETOEPOCH(file.date_created),ctimebuf[0]),
		hfs_ctime_r(HFSTIMETOEPOCH(file.date_content_mod),ctimebuf[1]),
		hfs_ctime_r(HFSTIMETOEPOCH(file.date_attrib_mod),ctimebuf[2]),
		hfs_ctime_r(HFSTIMETOEPOCH(file.date_accessed),ctimebuf[3]),
		hfs_ctime_r(HFSTIMETOEPOCH(file.date_backedup),ctimebuf[4]),
		file.text_encoding,
		file.bsd.owner_id,
		file.bsd.group_id,
		file.bsd.admin_flags,
		file.bsd.owner_flags,
		file.bsd.file_mode,
		file.bsd.special.inode_num
	);
	if(rec.type == HFS_REC_FLDR) {
		hfs_folder_record_t folder = rec.folder;
		printf(
			"valence: %" PRIu32 "\n"
			"user_info.window_bounds: %" PRIu16 ", %" PRIu16 ", %" PRIu16 ", %" PRIu16 "\n"
			"user_info.finder_flags: %" PRIu16 "\n"
			"user_info.location: %" PRIu16 ", %" PRIu16 "\n"
			"finder_info.scroll_position: %" PRIu16 ", %" PRIu16 "\n"
			"finder_info.extended_finder_flags: %" PRIu16 "\n"
			"finder_info.put_away_folder_cnid: %" PRIu32 "\n",
			folder.valence,
			folder.user_info.window_bounds.t, folder.user_info.window_bounds.l, folder.user_info.window_bounds.b, folder.user_info.window_bounds.r,
			folder.user_info.finder_flags,
			folder.user_info.location.v,folder.user_info.location.h,
			folder.finder_info.scroll_position.v,folder.finder_info.scroll_position.h,
			folder.finder_info.extended_finder_flags,
			folder.finder_info.put_away_folder_cnid
		);
	}
	else {
		printf(
			"user_info.file_type: %c%c%c%c\n"
			"user_info.file_creator: %c%c%c%c\n"
			"user_info.finder_flags: %" PRIu16 "\n"
			"user_info.location:  %" PRIu16 ", %" PRIu16 "\n"
			"finder_info.extended_finder_flags: %" PRIu16 "\n"
			"finder_info.put_away_folder_cnid: %" PRIu32 "\n"
			"data_fork.logical_size: %" PRIu64 "\n"
			"rsrc_fork.logical_size: %" PRIu64 "\n",
			(file.user_info.file_type >> 24) & 0xFF,(file.user_info.file_type >> 16) & 0xFF, (file.user_info.file_type >> 8) & 0xFF, file.user_info.file_type & 0xFF,
			(file.user_info.file_creator >> 24) & 0xFF,(file.user_info.file_creator >> 16) & 0xFF, (file.user_info.file_creator >> 8) & 0xFF, file.user_info.file_creator & 0xFF,
			file.user_info.finder_flags,
			file.user_info.location.v,file.user_info.location.h,
			file.finder_info.extended_finder_flags,
			file.finder_info.put_away_folder_cnid,
			file.data_fork.logical_size,
			file.rsrc_fork.logical_size
		);
	}
}

int main(int argc, char* argv[]) {
	if(argc < 2) {
		fprintf(stderr,"Usage: hfsdump <device> [<stat|read|xattr> <path|inode> [xattrname]\n");
		return 0;
	}

	hfs_callbacks cb = {hfs_vprintf, hfs_malloc, hfs_realloc, hfs_free, hfs_open, hfs_close, hfs_read};
	hfslib_init(&cb);
	hfs_volume vol = {0};
	hfs_catalog_keyed_record_t rec; hfs_catalog_key_t key; unsigned char fork;

	struct hfs_volume_config cfg;
	hfs_volume_config_defaults(&cfg);
	cfg.cache_size = 0;

	int ret = 0;
	if((ret = hfslib_open_volume(argv[1],1,&vol,&(hfs_callback_args){ .openvol = &cfg }))) {
		fprintf(stderr,"Couldn't open volume\n");
		hfslib_done();
		return ret;
	}

	if(argc < 4) {
		char name[HFS_NAME_MAX+1];
		hfs_unistr_to_utf8(&vol.name, name);
		printf("Volume name: %s\nJournaled? %d\nReadonly? %d\nOffset: %" PRIu64 "\n",name,vol.journaled, vol.readonly, vol.offset);
		dump_volume_header(vol.vh);
		goto end;
	}

	char* endptr;
	uint32_t cnid = strtoul(argv[3], &endptr, 10);
	if(!*endptr) {
		if((ret = hfslib_find_catalog_record_with_cnid(&vol, cnid, &rec, &key, NULL))) {
			fprintf(stderr,"CNID lookup failure: %" PRIu32 "\n", cnid);
			goto end;
		}
		fork = HFS_DATAFORK;
	}
	else if((ret = hfs_lookup(&vol,argv[3],&rec,&key,&fork))) {
		fprintf(stderr,"Path lookup failure: %s\n", argv[3]);
		puts(strerror(-ret));
		goto end;
	}

	if(!strcmp(argv[2], "stat")) {
		printf("path: ");
		char* path = hfs_get_path(&vol, rec.folder.cnid);
		if(path)
			printf("%s", path);
		printf("\n");
		free(path);
		dump_record(rec);
		struct hfs_decmpfs_header h;
		if(fork == HFS_DATAFORK && !hfs_decmpfs_lookup(&vol,&rec.file,&h,NULL,NULL))
			printf("decmpfs.type: %" PRIu8 "\ndecmpfs.logical_size: %" PRIu64 "\n", h.type, h.logical_size);
	}
	else if(!strcmp(argv[2], "read")) {
		if(rec.type == HFS_REC_FLDR) {
			hfs_catalog_keyed_record_t* keys;
			hfs_unistr255_t* names;
			uint32_t count;
			if((ret = hfslib_get_directory_contents(&vol,rec.folder.cnid,&keys,&names,&count,NULL)))
				goto end;
			for(size_t i = 0; i < count; i++) {
				char name[HFS_NAME_MAX+1];
				hfs_pathname_to_unix(names+i,name);
				puts(name);
			}
			free(names);
			free(keys);
		}
		else if(rec.type == HFS_REC_FILE) {
			struct hfs_decmpfs_header h;
			uint32_t inlinelength;
			unsigned char* data;
			if(fork == HFS_DATAFORK && !hfs_decmpfs_lookup(&vol,&rec.file,&h,&inlinelength,&data)) {
				struct hfs_decmpfs_context* decmpfs = hfs_decmpfs_create_context(&vol,rec.file.cnid,inlinelength,data);

				size_t bufsize = hfs_decmpfs_buffer_size(&h);
				if(!(data = hfs_reallocf(data,bufsize)))
					goto end;

				off_t offset = 0;
				int bytes;
				for(bytes = 0; (bytes = hfs_decmpfs_read(&vol,decmpfs,(char*)data,bufsize,offset)) > 0; offset += bytes)
					fwrite(data,bytes,1,stdout);
				if(bytes < 0)
					ret = 1;

				free(data);
				hfs_decmpfs_destroy_context(decmpfs);
			}
			else {
				hfs_extent_descriptor_t* extents = NULL;
				uint16_t nextents = hfslib_get_file_extents(&vol,rec.file.cnid,fork,&extents,NULL);
				uint64_t bytes = 0, offset = 0, size = fork == HFS_DATAFORK ? rec.file.data_fork.logical_size : rec.file.rsrc_fork.logical_size;
				char data[4096];
				while(!(ret = hfslib_readd_with_extents(&vol,data,&bytes,4096,offset,extents,nextents,NULL)) && offset < size) {
					fwrite(data,(size-offset < bytes ? size-offset : bytes),1,stdout);
					offset += bytes;
				}
				free(extents);
			}
		}
	}
	else if(!strcmp(argv[2], "xattr")) {
		if(argc < 5) {
			hfs_attribute_key_t* attr_keys;
			uint32_t nattrs;
			ret = hfslib_find_attribute_records_for_cnid(&vol,rec.file.cnid,&attr_keys,&nattrs,NULL);
			for(uint32_t i = 0; i < nattrs; i++) {
				char attrname[HFS_NAME_MAX+1];
				ssize_t u8len = hfs_unistr_to_utf8(&attr_keys[i].name, attrname);
				if(u8len > 0)
					printf("%s\n",attrname);
			}
		}
		else {
			hfs_attribute_record_t attrec;
			hfs_unistr255_t attrname;
			if(hfs_utf8_to_unistr(argv[4],&attrname) <= 0)
				goto end;
			hfs_attribute_key_t attrkey;
			void* inlinedata;
			if(hfslib_make_attribute_key(rec.file.cnid,0,attrname.length,attrname.unicode,&attrkey) &&
			   (!(ret = hfslib_find_attribute_record_with_key(&vol,&attrkey,&attrec,&inlinedata,NULL)))) {
				switch(attrec.type) {
					case HFS_ATTR_INLINE_DATA:
						fwrite(inlinedata,attrec.inline_record.length,1,stdout);
						free(inlinedata);
						break;
					case HFS_ATTR_FORK_DATA: {
						hfs_extent_descriptor_t* extents;
						uint16_t nextents;
						if(hfslib_get_attribute_extents(&vol,&attrkey,&attrec,&nextents,&extents,NULL))
							break;
						uint64_t bytes = 0, offset = 0, size = attrec.fork_record.fork.logical_size;
						char data[4096];
						while(!(ret = hfslib_readd_with_extents(&vol,data,&bytes,4096,offset,extents,nextents,NULL)) && offset < size) {
							fwrite(data,(size-offset < bytes ? size-offset : bytes),1,stdout);
							offset += bytes;
						}
						free(extents);
					}; break;
				}
			}
		}
	}
	else fprintf(stderr,"valid commands: stat, read, xattr\n");

end:
	hfslib_close_volume(&vol,NULL);
	hfslib_done();
	return ret;
}
