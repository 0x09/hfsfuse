[FUSE](https://github.com/libfuse/libfuse) driver for HFS+, based on NetBSD's kernel driver with modifications.

Created for FreeBSD where at the time (maybe still?) there was no way of reading HFS+ filesystems.  
Tested and used extensively on FreeBSD, OS X, and Linux.

Unlike the Linux kernel driver, supports reading of Time Machine volumes.

This driver is read-only and cannot write to or damage the target filesystem in any way.

#Supported
* Journaled and non-journaled HFS+
* Unicode normalization for pathnames via utf8proc (but see TODO)
* Hard links, including directory hard links (i.e. Time Machine backups)
* OS X-style resource fork access via /rsrc
* birthtime (with compatible FUSE)

#TODO
* HFS+ NFD special cases (u2000-u2FFF, uF900-uFAFF)
* Extended attributes / Finder info
* UID remapping

###minor TODO
* better record cache
* hook into FUSE options system

#Installation
##Configuring
hfsfuse can use [utf8proc](http://julialang.org/utf8proc/) and [ublio](https://www.freshports.org/devel/libublio/), either bundled or system versions, but it doesn't require them (however, utf8proc is required for working with most non-ASCII pathnames).  
To configure, run `make config` with WITH_DEP=(none/local/system). For example, to build without ublio, and with the system's utf8proc, use

    make config WITH_UBILIO=none WITH_UTF8PROC=system
	
The default behavior is equivalent to `make config WITH_UBLIO=local WITH_UTF8PROC=local`

##Building
    make
    make install

##Use
    hfsfuse <opts> <device> <mountpoint>

Where `<opts>` are any series of arguments to be passed along to FUSE. Use `hfsfuse -h` for a list of switches.

# DMG Mounting
Disk images can be mounted using [dmg2img](http://vu1tur.eu.org/dmg2img).

One-liner to extract the HFS+ partition in a DMG to an img:

	dmg2img -p$(dmg2img -l image.dmg | grep Apple_HFS | cut -d' ' -f2 | cut -d: -f1) image.dmg image.img

## FreeBSD

	hfsfuse <opts> /dev/md`mdconfig -f image.img` <mountpoint>

## Linux

	mnt=$(losetup -f)
	losetup $mnt image.img
	hfsfuse <opts> $mnt <mountpoint>

#Resources
* [sys/fs/hfs/ in the NetBSD source tree](http://cvsweb.netbsd.org/bsdweb.cgi/src/sys/fs/hfs/)
* [Apple Technical Note 1150](http://dubeiko.com/development/FileSystems/HFSPLUS/tn1150.html)
