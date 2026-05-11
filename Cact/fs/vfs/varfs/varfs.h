#ifndef VARFS_H
#define VARFS_H

#include <stdint.h>
#include "vfs.h"

/* varfs — выделенный writable namespace для runtime-данных пользовательских
 * сервисов (логи, локальный state). Ring-3 init (cgoct и пр.) не должны
 * создавать сами /var и /var/log: ядро гарантирует их существование на
 * boot. Сервисы лишь пишут в уже подготовленную структуру.
 *
 * Слой реализации: forward на ext4 /var (если есть boot-disk). В nodisk-
 * режиме это noop — записать ничего нельзя, но точка монтирования всё ещё
 * присутствует в VFS root, чтобы пути из userspace не валились на walk. */

/* Initialise varfs and ensure /var (+ /var/log) on the backing ext4 root. */
void        varfs_init    (vfs_node_t *ext4_root);

/* Return the varfs root node for registering in the mount table. */
vfs_node_t *varfs_get_root(void);

#endif
