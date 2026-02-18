#ifndef FAT16_H
#define FAT16_H

#include "vfs.h"
#include "libc.h"
#include <stdint.h>

struct fat16_bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t  drive_number;
    uint8_t  reserved;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  file_system_type[8];
} __attribute__((packed));

struct fat16_dirent {
    uint8_t  filename[8];
    uint8_t  ext[3];
    uint8_t  attributes;
    uint8_t  reserved[10];
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t first_cluster;
    uint32_t file_size;
} __attribute__((packed));

void     fat16_init();

uint32_t cluster_to_lba(uint16_t cluster);
uint16_t fat16_get_next_cluster(uint16_t cluster);
void     fat16_set_cluster(uint16_t cluster, uint16_t value);
uint16_t fat16_find_free_cluster();

int              fat16_read_file (struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
int              fat16_write_file(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
struct vfs_node* fat16_finddir   (struct vfs_node* node, char* name);
void             fat16_list_dir  (struct vfs_node* node);
int              fat16_vfs_create(struct vfs_node* node, char* name);
int              fat16_vfs_delete(struct vfs_node* node, char* name);
int              fat16_vfs_mkdir (struct vfs_node* node, char* name);

int fat16_create_file(struct vfs_node* parent, char* name);
int fat16_create_dir (struct vfs_node* parent, char* name);
int fat16_delete_file(char* name);

#endif