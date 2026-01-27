#include "fat16.h"
#include "kernel.h"
#include "memory.h"

extern void ata_read_sector(unsigned int lba, unsigned char* buffer);
extern void ata_write_sector(unsigned int lba, unsigned char* buffer);

struct fat16_bpb bpb;
unsigned int fat_lba;
unsigned int root_lba;
unsigned int data_lba;
unsigned int root_sectors;

unsigned int cluster_to_lba(unsigned short cluster) {
    return data_lba + (cluster - 2) * bpb.sectors_per_cluster;
}

static char toupper(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

void fat16_format_name(char* name, unsigned char* res) {
    int i;
    for (i = 0; i < 11; i++) res[i] = ' ';
    for (i = 0; i < 8 && name[i] != '.' && name[i] != '\0'; i++) res[i] = toupper(name[i]);
    if (name[i] == '.') {
        i++;
        for (int j = 0; j < 3 && name[i] != '\0'; i++, j++) res[8 + j] = toupper(name[i]);
    }
}

void fat16_init() {
    unsigned char sector_buf[512];
    
    ata_read_sector(0, sector_buf);
    memory_copy(sector_buf, (unsigned char*)&bpb, sizeof(struct fat16_bpb));
    
    if (bpb.jmp[0] != 0xEB && bpb.jmp[0] != 0xE9) {
        return;
    }

    fat_lba = bpb.reserved_sectors;
    root_lba = fat_lba + (bpb.fat_count * bpb.sectors_per_fat);
    root_sectors = (bpb.root_entries_count * 32) / bpb.bytes_per_sector;
    data_lba = root_lba + root_sectors;

    vfs_root = (struct vfs_node*)kalloc();
    if (!vfs_root) {
        return;
    }
    
    vfs_root->inode = 0;
    vfs_root->size = 0;
    vfs_root->read = fat16_read_file;
    vfs_root->write = fat16_write_file;
}

unsigned short fat16_get_next_cluster(unsigned short current_cluster) {
    unsigned char fat_table[512];
    unsigned int sector = fat_lba + (current_cluster * 2) / 512;
    unsigned int offset = (current_cluster * 2) % 512;
    
    ata_read_sector(sector, fat_table);
    
    unsigned short next_cluster = (unsigned short)fat_table[offset] | 
                                   ((unsigned short)fat_table[offset + 1] << 8);
    return next_cluster;
}

void fat16_set_cluster(unsigned short cluster, unsigned short value) {
    unsigned char fat_table[512];
    unsigned int sector = fat_lba + (cluster * 2) / 512;
    unsigned int offset = (cluster * 2) % 512;
    
    ata_read_sector(sector, fat_table);
    fat_table[offset] = (unsigned char)(value & 0xFF);
    fat_table[offset + 1] = (unsigned char)((value >> 8) & 0xFF);
    ata_write_sector(sector, fat_table);
}

unsigned short fat16_find_free_cluster() {
    unsigned char fat_table[512];
    
    for (int i = 0; i < bpb.sectors_per_fat; i++) {
        ata_read_sector(fat_lba + i, fat_table);
        for (int j = 0; j < 512; j += 2) {
            unsigned short entry = (unsigned short)fat_table[j] | 
                                    ((unsigned short)fat_table[j + 1] << 8);
            if (entry == 0x0000) {
                return (i * 512 / 2) + (j / 2);
            }
        }
    }
    return 0xFFFF;
}

int fat16_create_file(char* name) {
    unsigned char buf[512];
    struct fat16_dirent* d;
    unsigned char f_name[11];
    
    fat16_format_name(name, f_name);

    unsigned short clus = fat16_find_free_cluster();
    if (clus == 0xFFFF) {
        return -1;
    }

    for (unsigned int i = 0; i < root_sectors; i++) {
        ata_read_sector(root_lba + i, buf);
        for (int j = 0; j < 512; j += 32) {
            d = (struct fat16_dirent*)(buf + j);
            if (d->filename[0] == 0x00 || d->filename[0] == 0xE5) {
                memory_copy(f_name, d->filename, 11);
                d->attributes = 0x20;
                d->first_cluster = clus;
                d->file_size = 0;
                ata_write_sector(root_lba + i, buf);
                fat16_set_cluster(clus, 0xFFFF);
                return 0;
            }
        }
    }
    return -1;
}

int fat16_delete_file(char* name) {
    unsigned char buf[512];
    unsigned char f_name[11];
    
    fat16_format_name(name, f_name);

    for (unsigned int i = 0; i < root_sectors; i++) {
        ata_read_sector(root_lba + i, buf);
        for (int j = 0; j < 512; j += 32) {
            struct fat16_dirent* d = (struct fat16_dirent*)(buf + j);
            if (memory_compare(d->filename, f_name, 11) == 0) {
                unsigned short curr = d->first_cluster;
                while (curr < 0xFFF8 && curr >= 0x0002) {
                    unsigned short next = fat16_get_next_cluster(curr);
                    fat16_set_cluster(curr, 0x0000);
                    curr = next;
                }
                d->filename[0] = 0xE5;
                ata_write_sector(root_lba + i, buf);
                return 0;
            }
        }
    }
    return -1;
}

int fat16_read_file(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    unsigned short cluster = node->inode;
    unsigned int bytes_read = 0;
    unsigned char sector_buf[512];
    
    while (bytes_read < size && cluster < 0xFFF8) {
        ata_read_sector(cluster_to_lba(cluster), sector_buf);
        for (int i = 0; i < 512 && bytes_read < size; i++) {
            buffer[bytes_read++] = sector_buf[i];
        }
        cluster = fat16_get_next_cluster(cluster);
    }
    
    return bytes_read;
}

int fat16_write_file(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    unsigned short cluster = node->inode;
    unsigned char sector_buf[512];
    
    ata_read_sector(cluster_to_lba(cluster), sector_buf);
    for (unsigned int i = 0; i < 512 && i < size; i++) {
        sector_buf[i] = buffer[i];
    }
    ata_write_sector(cluster_to_lba(cluster), sector_buf);

    for (unsigned int i = 0; i < root_sectors; i++) {
        ata_read_sector(root_lba + i, sector_buf);
        for (int j = 0; j < 512; j += 32) {
            struct fat16_dirent* d = (struct fat16_dirent*)(sector_buf + j);
            if (d->first_cluster == node->inode && d->filename[0] != 0xE5) {
                d->file_size = size;
                ata_write_sector(root_lba + i, sector_buf);
                return size;
            }
        }
    }
    return size;
}

struct vfs_node* fat16_finddir(struct vfs_node* node, char* name) {
    unsigned char buf[512];
    unsigned char f_name[11];
    
    fat16_format_name(name, f_name);

    for (unsigned int i = 0; i < root_sectors; i++) {
        ata_read_sector(root_lba + i, buf);
        for (int j = 0; j < 512; j += 32) {
            struct fat16_dirent* d = (struct fat16_dirent*)(buf + j);
            if (memory_compare(d->filename, f_name, 11) == 0) {
                struct vfs_node* res = (struct vfs_node*)kalloc();
                if (!res) {
                    return 0;
                }
                res->inode = d->first_cluster;
                res->size = d->file_size;
                res->read = fat16_read_file;
                res->write = fat16_write_file;
                return res;
            }
        }
    }
    return 0;
}

void fat16_list_root() {
    unsigned char buf[512];
    
    for (unsigned int i = 0; i < root_sectors; i++) {
        ata_read_sector(root_lba + i, buf);
        for (int j = 0; j < 512; j += 32) {
            struct fat16_dirent* d = (struct fat16_dirent*)(buf + j);
            if (d->filename[0] == 0x00) {
                return;
            }
            if (d->filename[0] == 0xE5 || (d->attributes & 0x08)) continue;
            
            kprint("  ");
            for(int k = 0; k < 8; k++) {
                if(d->filename[k] != ' ') { 
                    char c[2] = {d->filename[k], 0}; 
                    kprint(c); 
                }
            }
            kprint(".");
            for(int k = 0; k < 3; k++) {
                if(d->ext[k] != ' ') { 
                    char c[2] = {d->ext[k], 0}; 
                    kprint(c); 
                }
            }
            char sz[16]; 
            itoa(d->file_size, sz); 
            kprint(" (");
            kprint(sz);
            kprint(" bytes)\n");
        }
    }
}