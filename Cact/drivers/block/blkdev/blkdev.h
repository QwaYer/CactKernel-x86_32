#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>

// Block device layer limits
#define BLKDEV_MAX       8
#define BLKDEV_NAME_MAX  16

// Generic block device descriptor
typedef struct blkdev {
    char     name[BLKDEV_NAME_MAX];
    uint32_t max_lba;

    void (*read_sector) (uint32_t lba, uint8_t *buf);
    void (*write_sector)(uint32_t lba, uint8_t *buf);
} blkdev_t;

// Public API
void      blkdev_init         (void);
blkdev_t *blkdev_get_boot     (void);
blkdev_t *blkdev_find         (const char *name);
int       blkdev_count        (void);
void      blkdev_dump         (void);

void blkdev_read_sector (uint32_t lba, uint8_t *buf);
void blkdev_write_sector(uint32_t lba, uint8_t *buf);

/* Register a boot-capable block device (typically from a storage kmod).
 * First successful registration becomes blkdev_get_boot(). */
int register_blkdev(const char *name, uint32_t max_lba,
                    void (*read_sector)(uint32_t lba, uint8_t *buf),
                    void (*write_sector)(uint32_t lba, uint8_t *buf));

void unregister_blkdev(const char *name);

#endif