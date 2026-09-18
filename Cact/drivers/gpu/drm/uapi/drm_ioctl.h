#ifndef CACT_DRM_IOCTL_H
#define CACT_DRM_IOCTL_H

/*
 * _IOC/_IO/_IOR/_IOW/_IOWR encoding for the DRM ioctl numbers.
 *
 * A DRM_IOCTL_* number packs direction, type ('d'), sequence number and
 * payload size into one 32-bit value; the DRM dispatcher decodes it with
 * _IOC_NR()/_IOC_SIZE() to pick a handler and to learn how many bytes to copy.
 * These macros therefore have to produce exactly what userspace (libdrm)
 * produces, or lookup and the copy size disagree.
 */

#define _IOC_NRBITS   8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS  2

#define _IOC_NRMASK   ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK  ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

#define _IOC(dir, type, nr, size)                     \
    (((dir) << _IOC_DIRSHIFT) |                       \
     ((type) << _IOC_TYPESHIFT) |                     \
     ((nr) << _IOC_NRSHIFT) |                         \
     ((size) << _IOC_SIZESHIFT))

#define _IOC_TYPECHECK(t) (sizeof(t))

#define _IO(type, nr)      _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, s)  _IOC(_IOC_READ, (type), (nr), (_IOC_TYPECHECK(s)))
#define _IOW(type, nr, s)  _IOC(_IOC_WRITE, (type), (nr), (_IOC_TYPECHECK(s)))
#define _IOWR(type, nr, s) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (_IOC_TYPECHECK(s)))

#define _IOC_DIR(nr)  (((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr) (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)   (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr) (((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

#endif /* CACT_DRM_IOCTL_H */
