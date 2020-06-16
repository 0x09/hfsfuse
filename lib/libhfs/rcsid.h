#ifndef LIBHFS_RCSID_H
#define LIBHFS_RCSID_H
#ifndef __KERNEL_RCSID
#define __KERNEL_RCSID(sec, string) __attribute__((used)) static const char hfs_rcsid[] = string
#endif
#endif
