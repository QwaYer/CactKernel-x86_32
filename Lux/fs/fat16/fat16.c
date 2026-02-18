#include "fat16.h"
#include "kernel.h"
#include "memory.h"
#include "vfs.h"
#include "ata.h"
#include <stdint.h>

struct fat16_bpb bpb;
uint32_t partition_lba = 0;
uint32_t fat_lba;
uint32_t root_lba;
uint32_t data_lba;
uint32_t root_sectors;

uint32_t cluster_to_lba(uint16_t cluster) {
    return data_lba + (cluster - 2) * bpb.sectors_per_cluster;
}

static char fat_toupper(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

void fat16_format_name(char* name, unsigned char* res) {
    int i;
    for (i = 0; i < 11; i++) res[i] = ' ';
    for (i = 0; i < 8 && name[i] != '.' && name[i] != '\0'; i++)
        res[i] = fat_toupper(name[i]);
    if (name[i] == '.') {
        i++;
        for (int j = 0; j < 3 && name[i] != '\0'; i++, j++)
            res[8 + j] = fat_toupper(name[i]);
    }
}

void fat16_init() {
    unsigned char sector_buf[512];
    int found = 0;

    for (unsigned int lba = 0; lba < 64; lba++) {
        ata_read_sector(lba, sector_buf);
        memory_copy((unsigned char*)&bpb, sector_buf, sizeof(struct fat16_bpb));
        if ((bpb.jmp[0] == 0xEB || bpb.jmp[0] == 0xE9) &&
             bpb.bytes_per_sector == 512 &&
             bpb.sectors_per_cluster > 0 &&
             bpb.reserved_sectors > 0 &&
             bpb.fat_count > 0 &&
             bpb.sectors_per_fat > 0) {
            found = 1;
            partition_lba = lba;
            break;
        }
    }
    if (!found) return;

    fat_lba      = partition_lba + bpb.reserved_sectors;
    root_lba     = fat_lba + (bpb.fat_count * bpb.sectors_per_fat);
    root_sectors = ((bpb.root_entries_count * 32) + (bpb.bytes_per_sector - 1)) / bpb.bytes_per_sector;
    data_lba     = root_lba + root_sectors;

    vfs_root = (struct vfs_node*)kalloc();
    if (!vfs_root) return;

    vfs_root->inode   = 0;
    vfs_root->size    = 0;
    vfs_root->type    = VFS_DIRECTORY;
    vfs_root->read    = fat16_read_file;
    vfs_root->write   = fat16_write_file;
    vfs_root->finddir = fat16_finddir;
    vfs_root->listdir = fat16_list_dir;
    vfs_root->create  = fat16_vfs_create;
    vfs_root->delete  = fat16_vfs_delete;
    vfs_root->mkdir   = fat16_vfs_mkdir;
    vfs_root->open    = 0;
    vfs_root->close   = 0;
    vfs_root->readdir = 0;
    vfs_root->ptr     = 0;
}

unsigned short fat16_get_next_cluster(unsigned short cluster) {
    unsigned char fat_table[512];
    unsigned int sector = fat_lba + (cluster * 2) / 512;
    unsigned int offset = (cluster * 2) % 512;
    ata_read_sector(sector, fat_table);
    return (unsigned short)fat_table[offset] |
           ((unsigned short)fat_table[offset + 1] << 8);
}

void fat16_set_cluster(unsigned short cluster, unsigned short value) {
    unsigned char fat_table[512];
    unsigned int sector = fat_lba + (cluster * 2) / 512;
    unsigned int offset = (cluster * 2) % 512;
    ata_read_sector(sector, fat_table);
    fat_table[offset]     = (unsigned char)(value & 0xFF);
    fat_table[offset + 1] = (unsigned char)((value >> 8) & 0xFF);
    ata_write_sector(sector, fat_table);
    ata_write_sector(sector + bpb.sectors_per_fat, fat_table);
}

unsigned short fat16_find_free_cluster() {
    unsigned char fat_table[512];
    for (int i = 0; i < bpb.sectors_per_fat; i++) {
        ata_read_sector(fat_lba + i, fat_table);
        for (int j = 0; j < 512; j += 2) {
            unsigned short cluster = (i * 512 / 2) + (j / 2);
            if (cluster < 2) continue;
            unsigned short entry = (unsigned short)fat_table[j] |
                                   ((unsigned short)fat_table[j + 1] << 8);
            if (entry == 0x0000) return cluster;
        }
    }
    return 0xFFFF;
}

static int fat16_create_entry(struct vfs_node* parent, char* name, unsigned char attr) {
    unsigned char buf[512];
    unsigned char f_name[11];
    fat16_format_name(name, f_name);

    unsigned short clus = fat16_find_free_cluster();
    if (clus == 0xFFFF) return -1;

    if (attr & 0x10) {
        unsigned char dir_buf[512];
        for (unsigned int s = 0; s < bpb.sectors_per_cluster; s++) {
            for (int i = 0; i < 512; i++) dir_buf[i] = 0;
            if (s == 0) {
                struct fat16_dirent* dot = (struct fat16_dirent*)dir_buf;
                memory_copy(dot->filename, (unsigned char*)".          ", 11);
                dot->attributes    = 0x10;
                dot->first_cluster = clus;

                struct fat16_dirent* dotdot = (struct fat16_dirent*)(dir_buf + 32);
                memory_copy(dotdot->filename, (unsigned char*)"..         ", 11);
                dotdot->attributes    = 0x10;
                dotdot->first_cluster = (parent && parent->inode != 0)
                                        ? (unsigned short)parent->inode : 0;
            }
            ata_write_sector(cluster_to_lba(clus) + s, dir_buf);
        }
    }

    if (!parent || parent->inode == 0) {
        for (unsigned int i = 0; i < root_sectors; i++) {
            ata_read_sector(root_lba + i, buf);
            for (int j = 0; j < 512; j += 32) {
                struct fat16_dirent* d = (struct fat16_dirent*)(buf + j);
                if (d->filename[0] == 0x00 || d->filename[0] == 0xE5) {
                    memory_copy(d->filename, f_name, 11);
                    d->attributes    = attr;
                    d->first_cluster = clus;
                    d->file_size     = 0;
                    ata_write_sector(root_lba + i, buf);
                    fat16_set_cluster(clus, 0xFFFF);
                    return 0;
                }
            }
        }
    } else {
        unsigned short parent_cluster = (unsigned short)parent->inode;
        while (parent_cluster >= 2 && parent_cluster < 0xFFF8) {
            unsigned int lba = cluster_to_lba(parent_cluster);
            for (unsigned int s = 0; s < bpb.sectors_per_cluster; s++) {
                ata_read_sector(lba + s, buf);
                for (int j = 0; j < 512; j += 32) {
                    struct fat16_dirent* d = (struct fat16_dirent*)(buf + j);
                    if (d->filename[0] == 0x00 || d->filename[0] == 0xE5) {
                        memory_copy(d->filename, f_name, 11);
                        d->attributes    = attr;
                        d->first_cluster = clus;
                        d->file_size     = 0;
                        ata_write_sector(lba + s, buf);
                        fat16_set_cluster(clus, 0xFFFF);
                        return 0;
                    }
                }
            }
            unsigned short next = fat16_get_next_cluster(parent_cluster);
            if (next >= 0xFFF8) break;
            parent_cluster = next;
        }
    }
    return -1;
}

int fat16_create_file(struct vfs_node* parent, char* name) {
    return fat16_create_entry(parent, name, 0x20);
}

int fat16_create_dir(struct vfs_node* parent, char* name) {
    return fat16_create_entry(parent, name, 0x10);
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
                while (curr >= 0x0002 && curr < 0xFFF8) {
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

int fat16_read_file(struct vfs_node* node, unsigned int offset,
                    unsigned int size, char* buffer) {
    unsigned short cluster = (unsigned short)node->inode;
    unsigned int bytes_read = 0;
    unsigned char sector_buf[512];

    unsigned int clusters_to_skip = offset / (bpb.sectors_per_cluster * 512);
    for (unsigned int i = 0; i < clusters_to_skip; i++) {
        cluster = fat16_get_next_cluster(cluster);
        if (cluster < 2 || cluster >= 0xFFF8) return 0;
    }

    unsigned int offset_in_cluster = offset % (bpb.sectors_per_cluster * 512);

    while (bytes_read < size && cluster >= 2 && cluster < 0xFFF8) {
        unsigned int lba = cluster_to_lba(cluster);
        for (unsigned int s = 0; s < bpb.sectors_per_cluster && bytes_read < size; s++) {
            ata_read_sector(lba + s, sector_buf);
            unsigned int start_i = 0;
            if (offset_in_cluster > 0) {
                if (offset_in_cluster >= 512) {
                    offset_in_cluster -= 512;
                    continue;
                } else {
                    start_i = offset_in_cluster;
                    offset_in_cluster = 0;
                }
            }
            for (unsigned int i = start_i; i < 512 && bytes_read < size; i++)
                buffer[bytes_read++] = sector_buf[i];
        }
        cluster = fat16_get_next_cluster(cluster);
    }
    return bytes_read;
}

int fat16_write_file(struct vfs_node* node, unsigned int offset,
                     unsigned int size, char* buffer) {
    unsigned short cluster = (unsigned short)node->inode;
    unsigned char sector_buf[512];

    ata_read_sector(cluster_to_lba(cluster), sector_buf);
    for (unsigned int i = 0; i < 512 && i < size; i++)
        sector_buf[i] = buffer[i];
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

static struct vfs_node* fat16_create_vfs_node(struct fat16_dirent* d) {
    struct vfs_node* res = (struct vfs_node*)kalloc();
    if (!res) return 0;
    res->inode   = d->first_cluster;
    res->size    = d->file_size;
    res->type    = (d->attributes & 0x10) ? VFS_DIRECTORY : VFS_FILE;
    res->read    = fat16_read_file;
    res->write   = fat16_write_file;
    res->finddir = (res->type == VFS_DIRECTORY) ? fat16_finddir    : 0;
    res->listdir = (res->type == VFS_DIRECTORY) ? fat16_list_dir   : 0;
    res->create  = (res->type == VFS_DIRECTORY) ? fat16_vfs_create : 0;
    res->delete  = (res->type == VFS_DIRECTORY) ? fat16_vfs_delete : 0;
    res->mkdir   = (res->type == VFS_DIRECTORY) ? fat16_vfs_mkdir  : 0;
    res->open    = 0;
    res->close   = 0;
    res->readdir = 0;
    res->ptr     = 0;
    return res;
}

struct vfs_node* fat16_finddir(struct vfs_node* node, char* name) {
    unsigned char buf[512];
    unsigned char f_name[11];
    fat16_format_name(name, f_name);

    if (node == vfs_root || node->inode == 0) {
        for (unsigned int i = 0; i < root_sectors; i++) {
            ata_read_sector(root_lba + i, buf);
            for (int j = 0; j < 512; j += 32) {
                struct fat16_dirent* d = (struct fat16_dirent*)(buf + j);
                if (d->filename[0] == 0x00) return 0;
                if (d->filename[0] == 0xE5) continue;
                if (memory_compare(d->filename, f_name, 11) == 0)
                    return fat16_create_vfs_node(d);
            }
        }
    } else {
        unsigned short cluster = (unsigned short)node->inode;
        while (cluster >= 2 && cluster < 0xFFF8) {
            unsigned int lba = cluster_to_lba(cluster);
            for (unsigned int s = 0; s < bpb.sectors_per_cluster; s++) {
                ata_read_sector(lba + s, buf);
                for (int j = 0; j < 512; j += 32) {
                    struct fat16_dirent* d = (struct fat16_dirent*)(buf + j);
                    if (d->filename[0] == 0x00) return 0;
                    if (d->filename[0] == 0xE5) continue;
                    if (memory_compare(d->filename, f_name, 11) == 0)
                        return fat16_create_vfs_node(d);
                }
            }
            cluster = fat16_get_next_cluster(cluster);
        }
    }
    return 0;
}

static void fat16_print_dirent(struct fat16_dirent* d) {
    if (d->filename[0] == 0x00 || d->filename[0] == 0xE5 || (d->attributes & 0x08)) return;

    kprint("  ");
    if (d->attributes & 0x10) kprint("<DIR> ");

    for (int k = 0; k < 8; k++) {
        if (d->filename[k] != ' ') {
            char c[2] = {d->filename[k], 0};
            kprint(c);
        }
    }
    if (!(d->attributes & 0x10) && d->ext[0] != ' ') {
        kprint(".");
        for (int k = 0; k < 3; k++) {
            if (d->ext[k] != ' ') {
                char c[2] = {d->ext[k], 0};
                kprint(c);
            }
        }
    }
    if (!(d->attributes & 0x10)) {
        char sz[16];
        itoa(d->file_size, sz);
        kprint("  ("); kprint(sz); kprint(" bytes)");
    }
    kprint("\n");
}

void fat16_list_dir(struct vfs_node* node) {
    unsigned char buf[512];

    if (node == vfs_root || node->inode == 0) {
        for (unsigned int i = 0; i < root_sectors; i++) {
            ata_read_sector(root_lba + i, buf);
            for (int j = 0; j < 512; j += 32) {
                struct fat16_dirent* d = (struct fat16_dirent*)(buf + j);
                if (d->filename[0] == 0x00) return;
                fat16_print_dirent(d);
            }
        }
    } else {
        unsigned short cluster = (unsigned short)node->inode;
        while (cluster >= 2 && cluster < 0xFFF8) {
            unsigned int lba = cluster_to_lba(cluster);
            for (unsigned int s = 0; s < bpb.sectors_per_cluster; s++) {
                ata_read_sector(lba + s, buf);
                for (int j = 0; j < 512; j += 32) {
                    struct fat16_dirent* d = (struct fat16_dirent*)(buf + j);
                    if (d->filename[0] == 0x00) return;
                    fat16_print_dirent(d);
                }
            }
            cluster = fat16_get_next_cluster(cluster);
        }
    }
}

int fat16_vfs_create(struct vfs_node* node, char* name) {
    return fat16_create_file(node, name);
}

int fat16_vfs_delete(struct vfs_node* node, char* name) {
    (void)node;
    return fat16_delete_file(name);
}

int fat16_vfs_mkdir(struct vfs_node* node, char* name) {
    return fat16_create_dir(node, name);
}