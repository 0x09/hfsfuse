[FUSE](https://github.com/libfuse/libfuse) driver for HFS+, based on NetBSD's kernel driver with modifications.

Created for use on FreeBSD which lacks a native driver, but also functions as an alternative to the kernel drivers on Linux and macOS.

This driver is read-only and cannot write to or damage the target filesystem in any way.

hfsfuse also includes a standalone tool, hfsdump, to inspect the contents of an HFS+ volume without FUSE.

**Supported**

* Journaled and non-journaled HFS+
* Unicode normalization for pathnames via utf8proc
* Hard links, including directory hard links (i.e. Time Machine backups)
* Resource fork, Finder info, and creation/backup time access via extended attributes
* birthtime (with compatible FUSE)

**Not supported**

* Writing
* User-defined extended attributes (undocumented)
* HFS (non plus) volumes

# Installation
## Configuring
hfsfuse can use [utf8proc](http://julialang.org/utf8proc/) and [ublio](https://www.freshports.org/devel/libublio/), either bundled or system versions, but it doesn't require them (however, utf8proc is required for working with most non-ASCII pathnames).  
To configure, run `make config` with WITH_DEP=(none/local/system). For example, to build without ublio, and with the system's utf8proc, use

    make config WITH_UBILIO=none WITH_UTF8PROC=system
	
The default behavior is equivalent to `make config WITH_UBLIO=local WITH_UTF8PROC=local`

## Building
    make
    make install

Makefile dialect is GNU, so substitute `gmake` on FreeBSD.

hfsfuse's support libraries can be also built standalone using `make lib` and `make install-lib` and used to read from HFS+ volumes without FUSE by including hfsuser.h and linking with libhfsuser, libhfs, and ublio/utf8proc if configured.

hfsdump is also built by default, but can be built standalone with `make hfsdump`, in which case the FUSE library is not needed.

Some version information is generated from the git repository. For out of tree builds, run `make version` within the repository first or provide your own version.h.

# Use
## hfsfuse
    hfsfuse <opts> <device> <mountpoint>

Where `<opts>` are any series of arguments to be passed along to FUSE. Use `hfsfuse -h` for general options or `hfsfuse -H` for a list of all switches supported by FUSE.  
hfsfuse-specific options are shown below

    usage: hfsfuse [-hHv] [-o options] device mountpoint
    
    general options:
        -o opt,[opt...]        mount options
        -h   --help            this help
        -H   --fullhelp        list all FUSE options
        -V   --version
    
    HFS options:
        --force                force mount volumes with dirty journal
        -o rsrc_only           only mount the resource forks of files
        -o noallow_other       restrict filesystem access to mounting user
        -o cache_size=N        size of lookup cache (1024)
        -o blksize=N           set a custom read size/alignment in bytes
                               you should only set this if you are sure it is being misdetected
        -o rsrc_ext=suffix     special suffix for filenames which can be used to access their resource fork
                               or alternatively their data fork if mounted in rsrc_only mode
    
        -o noublio             disable ublio read layer
        -o ublio_items=N       number of ublio cache entries, 0 for no caching (64)
        -o ublio_grace=N       reclaim cache entries only after N requests (32)
    
Note for Haiku users: under Haiku, FUSE applications cannot be invoked directly. Instead, `make install` will install hfsfuse as a userlandfs add-on, which can be used with `mount` like so:

    mount -t userlandfs -o "hfsfuse <opts> <device>" <mountpoint>

## hfsdump
	hfsdump <device> <command> <node>
	
`command` may be either `stat` or `read`: `stat` prints the record structure, while `read` copies the node's contents to standard out (or lists if node is a directory).  
`node` is either an inode/CNID to lookup, or a full path from the root of the volume being inspected.  
If the command and node are ommitted, hfsdump prints the volume header and exits.  
`/rsrc` may be appended to the path of a read operation to dump the resource fork instead.

## Extended attributes and resource forks
hfsfuse exposes some nonstandard HFS+ attributes as extended attributes. These include:
* hfsfuse.record.date_created: The date created as an ISO-8601 timestamp. Identical to `st_birthtime` on macOS.
* hfsfuse.record.date_backedup: The backup time of a file as an ISO-8601 timestamp.
* com.apple.FinderInfo: The Finder info as binary data, presented the same as with the macOS native driver.
* com.apple.ResourceFork: The resource fork as binary data.

The resource fork may also be accessed during normal use by defining a special file suffix with the `rsrc_ext` option. When this is set, any lookup that ends in this suffix returns resource fork data for the corresponding file. For example, when mounting with `-orsrc_ext=.rsrc`, "image.psd.rsrc" can be used to access the resource fork for image.psd.  
Of course, "image.psd.rsrc" may also exist independently, so this option can be set to anything suitable (`:rsrc`, `-resource`, etc) with the only condition being that it cannot include a path separator (as FUSE intercepts these).  
Because of this, the more familiar `/rsrc` suffix used by previous releases of macOS is not supported in hfsfuse, but may still be used with hfsdump.

Finally, the entire volume may be mounted in resource-fork only mode using the `rsrc_only` option. In this mode, all entries on the filesystem are presented using the size and contents of their resource fork. Files with no resource fork will appear as empty, 0 size entries.  
This option may be combined with the `rsrc_ext` option described above, in which case the special suffix will instead be used to access the regular data fork.

On Linux you may encounter the following error when inspecting xattrs: `user.com.apple.ResourceFork: Argument list too long`  
This occurs when the resource fork is larger than the maximum allowed extended attribute size of 64kb. In this case you can still access the resource fork as described above by setting the `rsrc_ext` option or mounting in `rsrc_only` mode.

Other, user-created extended attributes are not currently supported as their on-disk structure is not fully specified.

# Other
## DMG mounting
Disk images can be mounted using [dmg2img](http://vu1tur.eu.org/dmg2img).

One-liner to extract the HFS+ partition in a DMG to an img:

	dmg2img -p $(dmg2img -l image.dmg | grep Apple_HFS | cut -d' ' -f2 | cut -d: -f1) image.dmg image.img

### FreeBSD

	hfsfuse <opts> /dev/md`mdconfig -f image.img` <mountpoint>

### Linux

	mnt=$(losetup -f)
	losetup $mnt image.img
	hfsfuse <opts> $mnt <mountpoint>

## ID re-mapping
When sharing a disk between systems it's often convenient to establish a mapping between corresponding users/groups. FUSE offers `uid` and `gid` options to force ownership of all files on the mounted system to the provided id, but more involved mappings for multiple users or specific user and group combinations can be done using the [idmap](https://github.com/0x09/fuse-idmap) FUSE module.

# Resources
* [sys/fs/hfs/ in the NetBSD source tree](http://cvsweb.netbsd.org/bsdweb.cgi/src/sys/fs/hfs/)
* [Apple Technical Note 1150](https://developer.apple.com/legacy/library/technotes/tn/tn1150.html)
