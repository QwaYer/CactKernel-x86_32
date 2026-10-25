#ifndef MQUEUE_H
#define MQUEUE_H

#include <stdint.h>
#include "vfs.h"

// Create a POSIX message-queue filesystem instance rooted at `name`
// (heap-allocated).  mntfs mounts it at /dev/mqueue; the files it creates are
// the queues, so the file names are the mq name space and mq_unlink() is an
// ordinary unlink().
vfs_node_t *mqueue_create_root(const char *name);

#endif
