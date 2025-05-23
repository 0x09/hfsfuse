[FUSE](https://github.com/libfuse/libfuse) driver for HFS+ filesystems, based on NetBSD's kernel driver with modifications.

hfsfuse embeds and extends [NetBSD's HFS+ kernel driver](http://cvsweb.netbsd.org/bsdweb.cgi/src/sys/fs/hfs/) into a portable library for use with FUSE and other userspace tools. hfsfuse was created for use on FreeBSD and other Unix-like systems that lack a native HFS+ driver, but can also be used on Linux and macOS as an alternative to their kernel drivers.

hfsfuse also includes two standalone tools, hfsdump and hfstar, which can be used without FUSE.

This driver is read-only and cannot write to or alter the target filesystem.

**Supported**

* Journaled and non-journaled HFS+
* Unicode normalization for pathnames via utf8proc
* Hard links, including directory hard links (i.e. Time Machine backups)
* Resource fork, Finder info, and creation/backup time access via extended attributes
* birthtime (with compatible FUSE)
* User-defined extended attributes
* HFS+ compression with zlib and lzfse

**Not supported**

* HFS without the "+", aka "Mac OS Standard" volumes. For these, try [hfsutils](https://www.mars.org/home/rob/proj/hfs/).
* Writing

# Installation
With the FUSE headers and library for your platform installed, running `make install` (gmake on *BSD) from the project root will build and install hfsfuse and related tools and should be sufficient for most use cases. See below for more details or skip to [usage](#Use).

## Dependencies
hfsfuse aims to be widely portable across Unix-like systems. Build requirements include GNU Make, a C11 compiler with a GCC-compatible frontend, and a POSIX-compatible shell and utilities.

hfsfuse's supporting libraries and standalone hfsdump tool require only a POSIX-2008 compatible libc, and can also be built natively on Windows with either Mingw-w64 or msys2.

The FUSE driver requires a version 2 or 3 compatible FUSE library, and is known to work with the following implementations:

* [libfuse](https://libfuse.github.io) on Linux and *BSD
* [macFUSE](https://osxfuse.github.io)
* [FUSE-T](https://www.fuse-t.org)
* Haiku's userland_fs

hfsfuse optionally uses these additional libraries to enable certain functionality:

* [utf8proc](http://julialang.org/utf8proc/) for working with non-ASCII pathnames
* [ublio](https://www.freshports.org/devel/libublio/) for read caching, which may improve performance
* [zlib](https://www.zlib.net) and [lzfse](https://github.com/lzfse/lzfse) for reading files with HFS+ compression

utf8proc and ublio are both bundled with hfsfuse and built by default. hfsfuse can be configured to use already-installed versions of these if available, or may be built without them entirely if the respective functionality is not needed (see [Configuring](#Configuring)).

hfstar additionally requires [libarchive](https://www.libarchive.org). Like hfsdump it can also be built for Windows with Mingw-w64 or msys2.

## Configuring
hfsfuse is configured by passing options directly to `make`, and separate configure and build steps are not needed. `make showconfig` can be used to print available make options and their current values.  
For repeated builds using the same options, or to more easily edit config values, `make config` can optionally be used to generate a config.mak file which will be used by future invocations.

To configure hfsfuse's optional utf8proc and ublio dependencies, use WITH_*DEP*=(none/local/system). The default behavior with no arguments is to use the bundled versions of these and is the same as using

    make WITH_UBILIO=local WITH_UTF8PROC=local

To ease portability, the Makefile will attempt to detect certain features of the host libc in an autoconf-like way, and creates a series of defines for these labeled HAVE_*FEATURENAME*. To override and skip checks for a given feature, these may be provided directly to `make` or overridden in config.mak.

## Building
The default `make` and `make install` targets build and install hfsfuse, hfsdump, and hfstar. hfsdump and hfstar can also be built standalone with `make hfsdump hfstar`, in which case FUSE is not needed.

hfsfuse's supporting libraries can be built and installed independently using `make lib` and `make install-lib`. Applications can use these to read from HFS+ volumes by including [hfsuser.h](lib/libhfsuser/hfsuser.h) and linking with libhfsuser, libhfs, and ublio/utf8proc if configured.

Some version information is generated from the git repository. For distributions outside of revision control, run `make version` within the repository first or provide your own version.h.

# Use
## hfsfuse
hfsfuse is the FUSE driver and is invoked similarly to the Unix mount command.
hfsfuse accepts an HFS+ volume (device or image), mount point, and a series of options to be passed along to FUSE.  
Use `hfsfuse -h` for general options or `hfsfuse -H` for a list of all switches supported by FUSE. hfsfuse-specific options are shown below.

    usage: hfsfuse [-hHv] [-o options] volume mountpoint

    general options:
        -o opt,[opt...]        mount options
        -h, --help             this help
        -H, --fullhelp         list all FUSE options
        -v, --version
    
    HFS+ options:
        --force                force mount volumes with dirty journal
        -o rsrc_only           only mount the resource forks of files
        -o noallow_other       restrict filesystem access to mounting user
        -o cache_size=N        size of lookup cache (1024)
        -o blksize=N           set a custom read size/alignment in bytes
                               you should only set this if you are sure it is being misdetected
        -o rsrc_ext=suffix     special suffix for filenames which can be used to access their resource fork
                               or alternatively their data fork if mounted in rsrc_only mode
    
        -o default_file_mode=N octal filesystem permissions for Mac OS Classic files (755)
        -o default_dir_mode=N  octal filesystem permissions for Mac OS Classic directories (777)
        -o default_uid=N       unix user ID for Mac OS Classic files (0)
        -o default_gid=N       unix group ID for Mac OS Classic files (0)

        -o disable_symlinks    treat symbolic links as regular files. may be used to view extended attributes
                               of these on systems that don't support symlink xattrs
    
        -o noublio             disable ublio read layer
        -o ublio_items=N       number of ublio cache entries, 0 for no caching (64)
        -o ublio_grace=N       reclaim cache entries only after N requests (32)
    
Note for Haiku users: under Haiku, FUSE applications cannot be invoked directly. Instead, `make install` will install hfsfuse as a userlandfs add-on, which can be loaed with:

    /system/servers/userlandfs_server hfsfuse

Afterwards filesystems may be mounted like so:

    mount -t userlandfs -o "hfsfuse <opts> <device>" <mountpoint>

## hfsdump
hfsdump is a small standalone tool which can be used to inspect the contents of an HFS+ volume by printing record info and file or directory contents to standard out.

    Usage: hfsdump <device> <stat|read|xattr> <path|inode> [xattrname]
	
`stat`, `read`, and `xattr` are commands to perform. `stat` prints the record structure, while `read` copies the node's contents to standard out (or lists if node is a directory).  
The `xattr` command lists extended attribute names for the given node. If an attribute name is provided following the node argument, its value will be printed to standard out.  
`inode` or `path` are either an integer inode/CNID to lookup, or a full path from the root of the volume being inspected.  
If the command and node are ommitted, hfsdump prints the volume header and exits.  
`/rsrc` may be appended to the path of a read operation to dump the resource fork instead.  

## hfstar
hfstar can be used to archive the contents of an HFS+ volume to tar or any other archive format supported by libarchive.

    Usage: hfstar [options] <volume> <archive> [<prefix>]
    
      <volume>   HFS+ image or device to convert.
      <archive>  Output archive file.
      <prefix>   Optional path in the HFS+ volume to archive. Default: /
    
    Options:
      -h, --help
      -v, --version
    
      -b <bufsize>  Size of read buffer. Default: device's block size or 16kb for regular files.
      -s            Archive directory hard links as symbolic links instead of resolving them.
      -t            Trim <prefix> from archived paths. The root entry will be omitted.
      -e            Stop archiving if any entry has an error.
      -p            Print paths being archived.
      -W            Silence warnings.
    
    libarchive options:
      --format <name>   Name of the archive format. May be any format accepted by libarchive.
                        Default: inferred from the output archive file extension.
                        For .tar or no extension, defaults to posix format.
      --filter <name>   Name of a libarchive filter. Default: inferred from the output file extension.
      --options <opts>  String of libarchive options in the format option=value,...
    
    HFS+ options:
      --force           Try to read volumes with a dirty journal
      --blksize <n>     Device block size. Default: autodetected.
      --rsrc-ext <ext>  Archive resource forks as separate files with this extension, instead of as an xattr.
    
      --default-file-mode <mode>  Octal filesystem permissions for Mac OS Classic files. Default: 755
      --default-dir-mode <mode>   Octal filesystem permissions for Mac OS Classic directories. Default: 777
      --default-uid <uid>         Unix user ID for Mac OS Classic files. Default: 0
      --default-gid <gid>         Unix group ID for Mac OS Classic files. Default: 0
    
      --noublio          Disable ublio read layer.
      --ublio-items <N>  Number of ublio cache entries, 0 for no caching. Default: 64
      --ublio-grace <N>  Reclaim cache entries only after N requests. Default: 32


For tar archives, hfstar defaults to the `posix` format for compatibility with the most HFS+ attributes. This can be overridden with the `--format` option.  
When archiving to a format that supports extended attributes, hfstar includes any user-defined extended attributes as well as the automatic xattrs described [below](#extended-attributes-and-resource-forks).
In addition to hfsfuse.record.date_created, file creation times are stored using libarchive's LIBARCHIVE.creationtime attribute.

By default, files' resource fork data is archived in the com.apple.ResourceFork extended attribute. This may optionally instead be archived to independent files with a special suffix using the `--rsrc-ext` option.

Any directory hard links are resolved by default, which duplicates their content in the archive. These can be converted into symbolic links instead with `-s`.

hfstar is about 50% faster than archiving from a filesystem mounted with hfsfuse. hfstar can also be used to extract an HFS+ volume onto the local filesystem with a command like:

```shell
hfstar <volume> - | tar --xattrs -C <dest> -xf -`
```

## Extended attributes and resource forks
hfsfuse exposes some nonstandard HFS+ attributes as extended attributes. These include:

* hfsfuse.record.date\_created: The date created as an ISO-8601 timestamp, displayed in localtime with the time zone offset. Identical to `st_birthtime` on macOS or FreeBSD.
* hfsfuse.record.date\_backedup: The backup time of a file in the same format as hfsfuse.record.date\_created.
* com.apple.FinderInfo: The Finder info as binary data, presented the same as with the macOS native driver.
* com.apple.ResourceFork: The resource fork as binary data.

The resource fork may also be accessed during normal use by defining a special file suffix with the `rsrc_ext` option. When this is set, any lookup that ends in this suffix returns resource fork data for the corresponding file. For example, when mounting with `-orsrc_ext=.rsrc`, "image.psd.rsrc" can be used to access the resource fork for image.psd.  
Of course, "image.psd.rsrc" may also exist independently, so this option can be set to anything suitable (`:rsrc`, `-resource`, etc) with the only condition being that it cannot include a path separator (as FUSE intercepts these).  
Because of this, the more familiar `/rsrc` suffix used by previous releases of macOS is not supported in hfsfuse, but may still be used with hfsdump.

Finally, the entire volume may be mounted in resource-fork only mode using the `rsrc_only` option. In this mode, all entries on the filesystem are presented using the size and contents of their resource fork. Files with no resource fork will appear as empty, 0 size entries.  
This option may be combined with the `rsrc_ext` option described above, in which case the special suffix will instead be used to access the regular data fork.

### OS-specific extended attribute behavior
On Linux you may encounter the following error when inspecting xattrs: `user.com.apple.ResourceFork: Argument list too long`  
This occurs when the resource fork is larger than the maximum allowed extended attribute size of 64kb. In this case you can still access the resource fork as described above by setting the `rsrc_ext` option or mounting in `rsrc_only` mode.

Under Haiku only, all extended attribute values are hex encoded to allow binary attribute data to be passed along from FUSE. Values may be read using a tool like xxd, e.g. to read the resource fork extended attribute for "file":

```shell
catattr -r user.com.apple.ResourceFork file | xxd -r -p
```

Extended attributes are presented in the preferred namespace for the OS, typically `user.`. Alternate namespaces may be chosen when building hfsfuse by setting the `XATTR_NAMESPACE` make var.  
This should include the trailing `.` as an empty value indicates no namespacing (such as on macOS.)

HFS+ symbolic links may contain extended attributes of their own separate from their link destination.
For systems such as Linux that do not support reading user-namespace extended attributes of symlinks, these can be viewed by using the `-o disable_symlinks` mount option, which will cause hfsfuse to display all symlinks as regular files.

## Mac OS Classic file permissions
HFS+ filesystems created on Mac OS Classic do not contain the typical set of Unix ownership and permission information for files and folders.
For these hfsfuse provides the options default_file_mode, default_dir_mode, default_uid, and default_gid to specify fallback values if needed.

These defaults only apply to filesystem entries that are missing this information, they don't affect files or folders with existing permissions.
They are applied before the FUSE uid, gid, and umask options or any fuse-idmap conversions and will still be subject to them.

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
