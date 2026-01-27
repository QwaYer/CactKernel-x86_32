#ifndef FAT16_H
#define FAT16_H

#include "vfs.h"
#include "libc.h"

struct fat16_bpb {
    unsigned char jmp[3];
    unsigned char oem[8];
    unsigned short bytes_per_sector;
    unsigned char sectors_per_cluster;
    unsigned short reserved_sectors;
    unsigned char fat_count;
    unsigned short root_entries_count;
    unsigned short total_sectors_16;
    unsigned char media_type;
    unsigned short sectors_per_fat;
    unsigned short sectors_per_track;
    unsigned short head_count;
    unsigned int hidden_sectors;
    unsigned int total_sectors_32;
    unsigned char drive_number;
    unsigned char reserved;
    unsigned char boot_signature;
    unsigned int volume_id;
    unsigned char volume_label[11];
    unsigned char file_system_type[8];
} __attribute__((packed));

struct fat16_dirent {
    unsigned char filename[8];
    unsigned char ext[3];
    unsigned char attributes;
    unsigned char reserved[10];
    unsigned short modify_time;
    unsigned short modify_date;
    unsigned short first_cluster;
    unsigned int file_size;
} __attribute__((packed));

void fat16_init();
unsigned int cluster_to_lba(unsigned short cluster);
unsigned short fat16_get_next_cluster(unsigned short current_cluster);

struct vfs_node* fat16_finddir(struct vfs_node* node, char* name);
int fat16_read_file(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
void fat16_list_root();

int fat16_write_file(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
int fat16_create_file(char* name);
int fat16_delete_file(char* name);

unsigned short fat16_find_free_cluster();
void fat16_set_cluster(unsigned short cluster, unsigned short value);

#endif