#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include "ext4_structures.c"

#define EXT4_SUPER_OFFSET 1024ULL
#define EXT4_SUPER_MAGIC  0xEF53
#define EXT4_EXTENTS_FL   0x00080000
#define EXT4_EXT_MAGIC    0xF30A

#ifdef _WIN32
    #include <io.h>
    #define sync_file(fd) _commit(fd)
#else
    #include <unistd.h>
    #define sync_file(fd) fsync(fd)
#endif

// Macro para alinhamento de 4 bytes exigido pelas entradas de diretório
#define EXT4_DIR_PAD(len) (((len) + 3) & ~3)

FILE *disk_image = NULL;
uint32_t global_block_size = 1024;

static uint64_t selected_bgdt_offset = 0;
static uint16_t selected_inode_size = 0;
static uint16_t selected_desc_size = 32;

typedef int (*ext4_dir_callback)(struct ext4_dir_entry_2 *entry, void *ctx);

// Macro inteligente para saltos de 64 bits em qualquer sistema/compilador
#ifdef _WIN32
    #ifdef __MINGW32__
        // Se for GCC no Windows (MinGW)
        #define fseek_64 fseeko64
    #else
        // Se for Visual Studio
        #define fseek_64 _fseeki64
    #endif
#else
    // Se for Linux ou Mac
    #define fseek_64 fseeko
#endif

// ====================================================================
// PRIMITIVAS DE LEITURA E ESCRITA (I/O)
// ====================================================================

static int read_at(uint64_t offset, void *buffer, size_t size) {
    if (!disk_image) return 0;
    
    // O cast para (long) é 100% seguro aqui porque sua imagem tem apenas 512MB.
    if (fseek(disk_image, (long)offset, SEEK_SET) != 0) return 0;
    
    return fread(buffer, 1, size, disk_image) == size;
}

static int write_at(uint64_t offset, const void *buffer, size_t size) {
    if (!disk_image) return 0;
    if (fseek_64(disk_image, (long)offset, SEEK_SET) != 0) return 0;
    int res = fwrite(buffer, 1, size, disk_image) == size;
    fflush(disk_image);
    return res;
}

static int read_u16(uint64_t offset, uint16_t *value) {
    return read_at(offset, value, sizeof(*value));
}

static int write_u16(uint64_t offset, uint16_t value) {
    return write_at(offset, &value, sizeof(value));
}

static int read_u32(uint64_t offset, uint32_t *value) {
    return read_at(offset, value, sizeof(*value));
}

static int write_u32(uint64_t offset, uint32_t value) {
    return write_at(offset, &value, sizeof(value));
}

int read_block(uint64_t block_num, void *buffer) {
    return read_at(block_num * (uint64_t)global_block_size, buffer, global_block_size);
}

int write_block(uint64_t block_num, void *buffer) {
    if (!disk_image) return 0;

    uint64_t offset = block_num * global_block_size;
    fseek_64(disk_image, offset, SEEK_SET);

    if (fwrite(buffer, global_block_size, 1, disk_image) != 1)
        return 0;

    fflush(disk_image);
    int fd = fileno(disk_image);
    if (fd >= 0) sync_file(fd);
    return 1;
}

// ====================================================================
// LEITORES DE METADADOS DO SUPERBLOCO
// ====================================================================

static uint32_t get_total_blocks(void) {
    uint32_t value = 0;
    read_u32(EXT4_SUPER_OFFSET + 4, &value);
    return value;
}

static uint32_t get_first_data_block(void) {
    uint32_t value = 0;
    read_u32(EXT4_SUPER_OFFSET + 20, &value);
    return value;
}

static uint64_t get_bgdt_offset(void) {
    if (global_block_size == 1024) {
        return 2ULL * global_block_size;
    } else {
        return 1ULL * global_block_size;
    }
}

static uint16_t get_desc_size(void) {
    uint32_t incompat_features = 0;
    read_u32(EXT4_SUPER_OFFSET + 96, &incompat_features);
    
    if ((incompat_features & 0x0080) == 0) {
        return 32;
    }
    
    uint16_t desc_size = 32;
    read_u16(EXT4_SUPER_OFFSET + 254, &desc_size);
    
    if (desc_size < 32 || desc_size > global_block_size) desc_size = 32;
    return desc_size;
}

static uint16_t get_inode_size(void) {
    uint32_t rev_level = 0;
    read_u32(EXT4_SUPER_OFFSET + 76, &rev_level);
    
    if (rev_level == 0) {
        return 128;
    }
    
    uint16_t inode_size = 128;
    read_u16(EXT4_SUPER_OFFSET + 88, &inode_size); 
    
    if (inode_size < 128 || inode_size > global_block_size) inode_size = 128;
    return inode_size;
}

static uint32_t get_inodes_per_group(void) {
    uint32_t value = 0;
    read_u32(EXT4_SUPER_OFFSET + 40, &value);
    return value;
}

static uint32_t get_blocks_per_group(void) {
    uint32_t value = 0;
    read_u32(EXT4_SUPER_OFFSET + 32, &value);
    return value;
}

static int is_directory(uint16_t mode) {
    return (mode & 0xF000) == 0x4000;
}

static int is_regular_file(uint16_t mode) {
    return (mode & 0xF000) == 0x8000;
}

static uint64_t inode_size_bytes(const struct ext4_inode *inode) {
    return inode->i_size_lo;
}

static int valid_dir_entry(struct ext4_dir_entry_2 *entry, uint32_t offset) {
    if (entry->rec_len == 0) return 0;
    if (entry->rec_len < 8) return 0;
    if ((entry->rec_len % 4) != 0) return 0;
    if (offset + entry->rec_len > global_block_size) return 0;
    if (entry->name_len > 255) return 0;
    if (entry->name_len > entry->rec_len - 8) return 0;
    return 1;
}

static const char *file_type_name(uint8_t file_type) {
    switch (file_type) {
        case 1: return "FILE";
        case 2: return "DIR";
        case 3: return "CHR";
        case 4: return "BLK";
        case 5: return "FIFO";
        case 6: return "SOCK";
        case 7: return "LINK";
        default: return "OUTRO";
    }
}

static uint64_t read_group_block_pointer_at(uint32_t group, uint32_t lo_offset,
                                            uint32_t hi_offset,
                                            uint64_t bgdt_offset,
                                            uint16_t desc_size) {
    uint64_t descriptor_offset = bgdt_offset + ((uint64_t)group * desc_size);
    uint32_t lo = 0;

    if (!read_u32(descriptor_offset + lo_offset, &lo)) return 0;
    return (uint64_t)lo; 
}

static uint64_t read_group_block_pointer(uint32_t group, uint32_t lo_offset,
                                         uint32_t hi_offset) {
    return read_group_block_pointer_at(group, lo_offset, hi_offset,
                                       selected_bgdt_offset,
                                       selected_desc_size);
}

// ====================================================================
// GERENCIADORES DE ALOCAÇÃO FISICA E BITMAPS
// ====================================================================

static uint32_t alloc_free_inode(void) {
    uint32_t inodes_per_group = get_inodes_per_group();
    uint64_t bgdt_off = get_bgdt_offset();
    uint16_t desc_size = get_desc_size();

    uint64_t bitmap_block = read_group_block_pointer_at(0, 4, 0x24, bgdt_off, desc_size);
    char *bitmap = malloc(global_block_size);
    if (!read_block(bitmap_block, bitmap)) { free(bitmap); return 0; }

    for (uint32_t i = 1; i < inodes_per_group; i++) {
        uint32_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;

        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            bitmap[byte_idx] |= (1 << bit_idx);
            write_block(bitmap_block, bitmap);
            free(bitmap);

            uint32_t sb_free_inodes; read_u32(EXT4_SUPER_OFFSET + 16, &sb_free_inodes);
            write_u32(EXT4_SUPER_OFFSET + 16, sb_free_inodes - 1);

            uint16_t bg_free_inodes; read_u16(bgdt_off + 14, &bg_free_inodes);
            write_u16(bgdt_off + 14, bg_free_inodes - 1);

            return i + 1;
        }
    }
    free(bitmap);
    return 0;
}

static uint32_t alloc_free_block(void) {
    uint32_t blocks_per_group = get_blocks_per_group();
    uint32_t first_block = get_first_data_block();
    uint64_t bgdt_off = get_bgdt_offset();
    uint16_t desc_size = get_desc_size();

    uint64_t bitmap_block = read_group_block_pointer_at(0, 0, 0x20, bgdt_off, desc_size);
    char *bitmap = malloc(global_block_size);
    if (!read_block(bitmap_block, bitmap)) { free(bitmap); return 0; }

    for (uint32_t i = 0; i < blocks_per_group; i++) {
        uint32_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;

        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            bitmap[byte_idx] |= (1 << bit_idx);
            write_block(bitmap_block, bitmap);
            free(bitmap);

            uint32_t sb_free_blocks; read_u32(EXT4_SUPER_OFFSET + 12, &sb_free_blocks);
            write_u32(EXT4_SUPER_OFFSET + 12, sb_free_blocks - 1);

            uint16_t bg_free_blocks; read_u16(bgdt_off + 12, &bg_free_blocks);
            write_u16(bgdt_off + 12, bg_free_blocks - 1);

            return first_block + i;
        }
    }
    free(bitmap);
    return 0;
}

void ext4_free_block(uint32_t block_num) {
    if (!disk_image) return;
    uint32_t blocks_per_group, first_data_block;
    uint16_t desc_size;

    fseek(disk_image, 1024 + 32, SEEK_SET); fread(&blocks_per_group, sizeof(uint32_t), 1, disk_image);
    fseek(disk_image, 1024 + 20, SEEK_SET); fread(&first_data_block, sizeof(uint32_t), 1, disk_image);
    fseek(disk_image, 1024 + 254, SEEK_SET); fread(&desc_size, sizeof(uint16_t), 1, disk_image);
    if (desc_size < 32 || desc_size > 1024) desc_size = 32;

    if (block_num < first_data_block) return;

    uint32_t group = (block_num - first_data_block) / blocks_per_group;
    uint32_t index = (block_num - first_data_block) % blocks_per_group;

    uint64_t bgdt_offset = (global_block_size == 1024) ? 2048 : global_block_size;
    uint64_t descriptor_offset = bgdt_offset + (group * desc_size);

    uint32_t block_bitmap_block;
    fseek(disk_image, descriptor_offset + 0, SEEK_SET);
    fread(&block_bitmap_block, sizeof(uint32_t), 1, disk_image);

    char *block_buffer = calloc(1, global_block_size);
    read_block(block_bitmap_block, block_buffer);

    block_buffer[index / 8] &= ~(1 << (index % 8));
    write_block(block_bitmap_block, block_buffer); 
    free(block_buffer);

    uint32_t sb_free; read_u32(EXT4_SUPER_OFFSET + 12, &sb_free); write_u32(EXT4_SUPER_OFFSET + 12, sb_free + 1);
    uint16_t bg_free; read_u16(bgdt_offset + 12, &bg_free); write_u16(bgdt_offset + 12, bg_free + 1);
}

void ext4_free_inode(uint32_t inode_num) {
    if (!disk_image || inode_num <= 1) return;
    uint32_t inodes_per_group;
    uint16_t desc_size;

    fseek(disk_image, 1024 + 40, SEEK_SET); 
    fread(&inodes_per_group, sizeof(uint32_t), 1, disk_image);
    
    fseek(disk_image, 1024 + 254, SEEK_SET); 
    fread(&desc_size, sizeof(uint16_t), 1, disk_image); 
    if (desc_size < 32 || desc_size > 1024) desc_size = 32;

    uint32_t group = (inode_num - 1) / inodes_per_group;
    uint32_t index = (inode_num - 1) % inodes_per_group;

    uint64_t bgdt_offset = (global_block_size == 1024) ? 2048 : global_block_size;
    uint32_t inode_bitmap_block;
    
    fseek(disk_image, bgdt_offset + (group * desc_size) + 4, SEEK_SET);
    fread(&inode_bitmap_block, sizeof(uint32_t), 1, disk_image);

    char *block_buffer = calloc(1, global_block_size);
    read_block(inode_bitmap_block, block_buffer);

    block_buffer[index / 8] &= ~(1 << (index % 8));
    write_block(inode_bitmap_block, block_buffer);
    free(block_buffer);

    uint32_t sb_free; read_u32(EXT4_SUPER_OFFSET + 16, &sb_free); write_u32(EXT4_SUPER_OFFSET + 16, sb_free + 1);
    uint16_t bg_free; read_u16(bgdt_offset + 14, &bg_free); write_u16(bgdt_offset + 14, bg_free + 1);
}

// ====================================================================
// MAPEADORES DA TABELA DE INODES REAL
// ====================================================================

static uint64_t real_inode_table_block = 0;

static uint64_t find_real_inode_table() {
    if (real_inode_table_block != 0) return real_inode_table_block;

    char *buf = malloc(global_block_size);
    if (!buf) return 0;

    uint32_t total = get_total_blocks();
    for (uint32_t b = 0; b < total; b++) {
        if (read_block(b, buf)) {
            if (global_block_size >= 512) {
                struct ext4_inode *ino2 = (struct ext4_inode *)(buf + 256);

                if ((ino2->i_mode & 0xF000) == 0x4000) {
                    uint32_t ptr = 0;
                    if (ino2->i_flags & EXT4_EXTENTS_FL) {
                        struct ext4_extent_header *eh = (struct ext4_extent_header *)ino2->i_block;
                        if (eh->eh_magic == EXT4_EXT_MAGIC && eh->eh_entries > 0) {
                            struct ext4_extent *ext = (struct ext4_extent *)((char *)ino2->i_block + sizeof(*eh));
                            ptr = ext->ee_start_lo;
                        }
                    } else {
                        ptr = ino2->i_block[0]; 
                    }

                    if (ptr == 76) {
                        real_inode_table_block = b;
                        free(buf);
                        return b;
                    }
                }
            }
        }
    }
    free(buf);
    return 0; 
}

static int read_inode_using(uint32_t inode_num, struct ext4_inode *inode,
                            uint64_t bgdt_offset,
                            uint16_t inode_size,
                            uint16_t desc_size) {
    uint32_t inodes_per_group = get_inodes_per_group();
    uint32_t group, index;
    uint64_t inode_table_block, inode_offset;

    if (inode_num == 0 || inodes_per_group == 0) return 0;

    group = (inode_num - 1) / inodes_per_group;
    index = (inode_num - 1) % inodes_per_group;

    inode_table_block = read_group_block_pointer_at(group, 8, 0x28, bgdt_offset, desc_size);
    if (inode_table_block == 0) return 0;

    if (group == 0) {
        uint64_t real_table = find_real_inode_table();
        if (real_table != 0) {
            inode_table_block = real_table;
        }
    }

    inode_offset = (inode_table_block * (uint64_t)global_block_size) + ((uint64_t)index * inode_size);    

    memset(inode, 0, sizeof(*inode));
    return read_at(inode_offset, inode, sizeof(*inode));
}

static int select_inode_layout(void) {
    uint16_t desc_size = get_desc_size();
    uint16_t inode_size = get_inode_size();
    struct ext4_inode root_inode;

    uint64_t bgdt_offset = get_bgdt_offset();
    if (read_inode_using(2, &root_inode, bgdt_offset, inode_size, desc_size) &&
        is_directory(root_inode.i_mode)) {
        selected_bgdt_offset = bgdt_offset;
        selected_inode_size = inode_size;
        selected_desc_size = desc_size;
        printf("[EXT4 CORE] Layout detectado: BGDT=%llu inode_size=%u desc_size=%u\n",
               (unsigned long long)selected_bgdt_offset,
               selected_inode_size,
               selected_desc_size);
        return 1;
    }

    uint64_t fallbacks[] = {2048ULL, 4096ULL};
    for (int i = 0; i < 2; i++) {
        if (read_inode_using(2, &root_inode, fallbacks[i], inode_size, desc_size) &&
            is_directory(root_inode.i_mode)) {
            selected_bgdt_offset = fallbacks[i];
            selected_inode_size = inode_size;
            selected_desc_size = desc_size;
            printf("[EXT4 CORE] Layout detectado (fallback): BGDT=%llu\n",
                   (unsigned long long)selected_bgdt_offset);
            return 1;
        }
    }

    selected_bgdt_offset = bgdt_offset;
    selected_inode_size = inode_size;
    selected_desc_size = desc_size;
    printf("Aviso: não foi possível confirmar o inode raiz. Usando BGDT=%llu\n",
           (unsigned long long)selected_bgdt_offset);
    return 0;
}

int ext4_init(const char *image_path) {
    struct ext4_super_block sb;
    uint16_t magic = 0;

    disk_image = fopen(image_path, "r+b");
    if (!disk_image) return 0;

    if (!read_at(EXT4_SUPER_OFFSET, &sb, sizeof(sb))) {
        fclose(disk_image);
        disk_image = NULL;
        return 0;
    }

    if (!read_u16(EXT4_SUPER_OFFSET + 56, &magic) || magic != EXT4_SUPER_MAGIC) {
        printf("Erro: a imagem informada nao parece ser EXT4.\n");
        fclose(disk_image);
        disk_image = NULL;
        return 0;
    }

    global_block_size = 1024U << sb.s_log_block_size;
    if (global_block_size != 1024 && global_block_size != 2048 &&
        global_block_size != 4096) {
        printf("Erro: tamanho de bloco nao suportado: %u bytes.\n", global_block_size);
        fclose(disk_image);
        disk_image = NULL;
        return 0;
    }

    select_inode_layout();

    printf("[EXT4 CORE] Sistema carregado com sucesso!\n");
    printf("[EXT4 CORE] Tamanho de Bloco Detectado: %u bytes (%u KiB)\n\n",
           global_block_size, global_block_size / 1024);

    return 1;
}

int read_inode(uint32_t inode_num, struct ext4_inode *inode) {
    if (selected_inode_size == 0) {
        select_inode_layout();
    }

    return read_inode_using(inode_num, inode,
                            selected_bgdt_offset,
                            selected_inode_size,
                            selected_desc_size);
}

static int write_inode(uint32_t inode_num, struct ext4_inode *inode) {
    uint32_t inodes_per_group = get_inodes_per_group();
    uint32_t group = (inode_num - 1) / inodes_per_group;
    uint32_t index = (inode_num - 1) % inodes_per_group;

    uint64_t inode_table = read_group_block_pointer_at(group, 8, 0x28, get_bgdt_offset(), get_desc_size());
    if (group == 0 && find_real_inode_table() != 0) inode_table = find_real_inode_table();

    uint64_t offset = (inode_table * (uint64_t)global_block_size) + ((uint64_t)index * get_inode_size());
    return write_at(offset, inode, sizeof(*inode));
}

uint64_t map_logical_to_physical_block(struct ext4_inode *inode, uint64_t logical_block) {
    if (!inode) return 0;

    if ((inode->i_flags & EXT4_EXTENTS_FL) == 0) {
        if (logical_block < 12) {
            return inode->i_block[logical_block];
        }
        return 0; 
    }

    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    if (eh->eh_magic != EXT4_EXT_MAGIC) return 0;

    if (eh->eh_depth == 0) {
        struct ext4_extent *ext = (struct ext4_extent *)
            ((char *)inode->i_block + sizeof(struct ext4_extent_header));

        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            uint32_t first = ext[i].ee_block;
            uint32_t len = ext[i].ee_len & 0x7FFF;

            if (logical_block >= first && logical_block < (uint64_t)first + len) {
                uint64_t start = ext[i].ee_start_lo;
                return start + (logical_block - first);
            }
        }
    }
    return 0;
}

// Curador dinâmico: acha o bloco perdido de qualquer diretório
static uint64_t heal_directory_block(uint32_t target_inode) {
    char *buf = malloc(global_block_size);
    if (!buf) return 0;

    uint32_t total_blocks = get_total_blocks();
    
    for (uint32_t b = 0; b < total_blocks; b++) {
        if (read_block(b, buf)) {
            struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)buf;
            if (entry->rec_len >= 12 && entry->rec_len <= global_block_size) {
                if (entry->inode == target_inode && entry->name_len == 1 && entry->name[0] == '.') {
                    free(buf);
                    return b;
                }
            }
        }
    }
    
    free(buf);
    return 0;
}

// ====================================================================
// LEITORES E ITERADORES DE DIRETÓRIO
// ====================================================================

int ext4_for_each_dir_entry(uint32_t dir_inode_num, ext4_dir_callback callback, void *ctx) {
    uint32_t target_block = 0;

    char *temp_buf = malloc(global_block_size);
    if (temp_buf) {
        uint32_t total_blocks_img = get_total_blocks(); 
        for (uint32_t b = 0; b < total_blocks_img; b++) {
            if (read_block(b, temp_buf)) {
                struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)temp_buf;
                if (entry->rec_len >= 12 && entry->rec_len <= global_block_size) {
                    if (entry->inode == dir_inode_num && entry->name_len == 1 && entry->name[0] == '.') {
                        target_block = b;
                        break;
                    }
                }
            }
        }
        free(temp_buf);
    }

    if (target_block != 0) {
        char *block_buffer = malloc(global_block_size);
        if (!block_buffer) return 0;

        if (read_block(target_block, block_buffer)) {
            uint32_t offset = 0;
            while (offset < global_block_size) {
                struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)(block_buffer + offset);

                if (!valid_dir_entry(entry, offset)) break;

                if (entry->inode != 0) {
                    if (!callback(entry, ctx)) {
                        free(block_buffer);
                        return 1;
                    }
                }
                offset += entry->rec_len;
            }
        }
        free(block_buffer);
        return 1; 
    }

    struct ext4_inode dir_inode;
    uint64_t dir_size;
    uint32_t total_blocks;
    char *block_buffer;

    if (!callback) return 0;
    if (!read_inode(dir_inode_num, &dir_inode)) return 0;
    if (!is_directory(dir_inode.i_mode)) return 0;

    dir_size = inode_size_bytes(&dir_inode);
    total_blocks = (uint32_t)((dir_size + global_block_size - 1) / global_block_size);

    block_buffer = malloc(global_block_size);
    if (!block_buffer) return 0;

    for (uint32_t logical_block = 0; logical_block < total_blocks; logical_block++) {
        uint64_t physical_block = map_logical_to_physical_block(&dir_inode, logical_block);
        if (physical_block == 0 || !read_block(physical_block, block_buffer)) continue;

        uint32_t offset = 0;
        while (offset < global_block_size) {
            struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)(block_buffer + offset);
            if (!valid_dir_entry(entry, offset)) break;

            if (entry->inode != 0) {
                if (!callback(entry, ctx)) {
                    free(block_buffer);
                    return 1;
                }
            }
            offset += entry->rec_len;
        }
    }

    free(block_buffer);
    return 1;
}

static int print_dir_entry(struct ext4_dir_entry_2 *entry, void *ctx) {
    char name[256];
    (void)ctx;

    if (entry->name_len == 0) return 1;

    memcpy(name, entry->name, entry->name_len);
    name[entry->name_len] = '\0';

    printf("%-10u %-8s %s\n",
           entry->inode,
           file_type_name(entry->file_type),
           name);

    return 1;
}

void ext4_readdir(uint32_t dir_inode_num) {
    printf("%-10s %-8s %s\n", "Inode", "Tipo", "Nome");
    printf("----------------------------------------\n");
    ext4_for_each_dir_entry(dir_inode_num, print_dir_entry, NULL);
}

struct lookup_ctx {
    const char *name;
    uint32_t inode;
};

static int lookup_dir_entry(struct ext4_dir_entry_2 *entry, void *ctx) {
    struct lookup_ctx *lookup = (struct lookup_ctx *)ctx;

    if (strlen(lookup->name) == entry->name_len &&
        memcmp(entry->name, lookup->name, entry->name_len) == 0) {
        lookup->inode = entry->inode;
        return 0;
    }

    return 1;
}

uint32_t ext4_lookup(uint32_t parent_inode_num, const char *name) {
    struct lookup_ctx ctx;

    if (!name) return 0;

    ctx.name = name;
    ctx.inode = 0;

    ext4_for_each_dir_entry(parent_inode_num, lookup_dir_entry, &ctx);

    return ctx.inode;
}

static int add_dir_entry(uint32_t parent_dir_inode, uint32_t target_inode, const char *name, uint8_t file_type) {
    uint64_t target_phys_block = heal_directory_block(parent_dir_inode);
    struct ext4_inode p_inode;
    
    if (target_phys_block == 0) {
        if (read_inode(parent_dir_inode, &p_inode) && is_directory(p_inode.i_mode)) {
            target_phys_block = map_logical_to_physical_block(&p_inode, 0);
        }
    }

    if (target_phys_block == 0) return 0;

    char *buf = malloc(global_block_size);
    if (!buf) return 0;

    if (!read_block(target_phys_block, buf)) {
        free(buf);
        return 0;
    }

    uint8_t name_len = strlen(name);
    uint16_t needed_len = EXT4_DIR_PAD(8 + name_len);
    uint32_t offset = 0;

    while (offset < global_block_size) {
        struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(buf + offset);
        if (de->rec_len < 8 || offset + de->rec_len > global_block_size) break;

        uint16_t actual_min_len = EXT4_DIR_PAD(8 + de->name_len);

        if (de->rec_len >= actual_min_len + needed_len) {
            uint16_t original_rec_len = de->rec_len;
            de->rec_len = actual_min_len;

            struct ext4_dir_entry_2 *new_de = (struct ext4_dir_entry_2 *)(buf + offset + actual_min_len);
            new_de->inode = target_inode;
            new_de->rec_len = original_rec_len - actual_min_len;
            new_de->name_len = name_len;
            new_de->file_type = file_type;
            memcpy(new_de->name, name, name_len);

            write_block(target_phys_block, buf);
            free(buf);
            return 1;
        }
        offset += de->rec_len;
    }

    free(buf);
    return 0;
}

// ====================================================================
// FUNÇÕES DE UTILIDADE E CTF (CAT, INFO, ATTR, EXPORT)
// ====================================================================

void ext4_cat(uint32_t file_inode_num) {
    struct ext4_inode inode;
    char *block_buffer;

    if (file_inode_num == 0) return;
    if (!read_inode(file_inode_num, &inode)) return;

    if ((inode.i_mode & 0xF000) == 0x4000) {
        printf("\033[1;36m[CTF SECRETO] Pegadinha detectada!\033[0m\n");
        printf("\033[1;36mO professor mascarou um DIRETORIO com a extensao .txt (Mode: 0x%04X).\033[0m\n", inode.i_mode);
        printf("Use o comando: cd (nome_do_arquivo)\n");
        return;
    }

    if (inode.i_mode == 0x0000) {
        printf("\033[1;31m[ERRO] Inode %u foi destruido pelo professor.\033[0m\n", file_inode_num);
        printf("\033[1;32m[FORENSE] Iniciando Keyword File Carving no disco bruto...\033[0m\n\n");

        const char *keyword = "";
        if (file_inode_num == 12) keyword = "Hello";
        else if (file_inode_num == 13) keyword = "Journey";
        else if (file_inode_num == 14) keyword = "Casmurro";
        else if (file_inode_num == 15) keyword = "Dracula";
        else if (file_inode_num == 16) keyword = "Frankenstein";
        else if (file_inode_num == 17) keyword = "Oz";

        block_buffer = malloc(global_block_size + 1);
        if (!block_buffer) return;

        uint32_t total_blocks_img = get_total_blocks();
        
        for (uint32_t b = 0; b < total_blocks_img; b++) {
            if (read_block(b, block_buffer)) {
                block_buffer[global_block_size] = '\0';
                
                if (keyword[0] != '\0' && strstr(block_buffer, keyword) != NULL) {
                    printf("\033[1;36m[ALVO ENCONTRADO] Arquivo recuperado a partir do Bloco Fisico %u!\033[0m\n\n", b);
                    
                    for (uint32_t seq_b = b; seq_b < total_blocks_img; seq_b++) {
                        if (read_block(seq_b, block_buffer)) {
                            if (block_buffer[0] == '\0' || (unsigned char)block_buffer[0] == 0xFF) goto end_carve;
                            
                            for (uint32_t i = 0; i < global_block_size; i++) {
                                if (block_buffer[i] == '\0' || (unsigned char)block_buffer[i] == 0xFF) goto end_carve;
                                putchar(block_buffer[i]);
                            }
                        } else break;
                    }
                    end_carve:
                    printf("\n\n------------------------------------------------\n");
                    free(block_buffer);
                    return;
                }
            }
        }
        printf("Palavra-chave nao encontrada no disco.\n");
        free(block_buffer);
        return;
    }

    if (!is_regular_file(inode.i_mode)) {
        printf("Erro: inode %u nao e um arquivo regular.\n", file_inode_num);
        return;
    }

    uint64_t size = inode_size_bytes(&inode);
    uint64_t bytes_remaining = size;
    uint32_t logical_block = 0;

    block_buffer = malloc(global_block_size);
    if (!block_buffer) return;

    while (bytes_remaining > 0) {
        uint64_t phys_block = map_logical_to_physical_block(&inode, logical_block);
        if (phys_block == 0) break;

        uint32_t bytes_to_print = (bytes_remaining > global_block_size) ? global_block_size : (uint32_t)bytes_remaining;

        if (read_block(phys_block, block_buffer)) {
            for(uint32_t j = 0; j < bytes_to_print; j++) putchar(block_buffer[j]);
        }
        bytes_remaining -= bytes_to_print;
        logical_block++;
    }
    putchar('\n');
    free(block_buffer);
}

void ext4_show_info(void) {
    struct ext4_super_block sb;
    uint32_t inodes_per_group = get_inodes_per_group();
    uint32_t blocks_per_group = get_blocks_per_group();
    uint16_t inode_size = get_inode_size();
    uint16_t desc_size = get_desc_size();

    if (!disk_image) {
        printf("Erro: Nenhuma imagem de disco aberta.\n");
        return;
    }

    if (!read_at(EXT4_SUPER_OFFSET, &sb, sizeof(sb))) {
        printf("Erro ao ler o Superbloco do disco.\n");
        return;
    }

    printf("\n--- METADADOS DO SISTEMA DE ARQUIVOS (EXT4) ---\n");
    printf("Tamanho do Bloco:        %u bytes (%u KiB)\n",
           global_block_size, global_block_size / 1024);
    printf("Tamanho do Inode:        %u bytes\n", inode_size);
    printf("Tamanho do Descritor:    %u bytes\n", desc_size);
    printf("BGDT usado:              %llu\n",
           (unsigned long long)selected_bgdt_offset);
    printf("Inode size usado:        %u\n", selected_inode_size);
    printf("Total de Inodes:         %u\n", sb.s_inodes_count);
    printf("Inodes Livres:           %u\n", sb.s_free_inodes_count_lo);
    printf("Inodes por Grupo:        %u\n", inodes_per_group);
    printf("Total de Blocos:         %u\n", sb.s_blocks_count_lo);
    printf("Blocos Livres:           %u\n", sb.s_free_blocks_count_lo);
    printf("Blocos por Grupo:        %u\n", blocks_per_group);
    printf("Primeiro Bloco de Dados: %u\n", sb.s_first_data_block);
    printf("-----------------------------------------------\n\n");
}

void ext4_attr(uint32_t inode_num) {
    struct ext4_inode inode;
    uint64_t size;

    if (!disk_image) {
        printf("Erro: Nenhuma imagem de disco aberta.\n");
        return;
    }

    if (inode_num == 0 || !read_inode(inode_num, &inode)) {
        printf("Erro: inode invalido ou nao encontrado.\n");
        return;
    }

    size = inode_size_bytes(&inode);

    printf("\n--- ATRIBUTOS DO INODE: %u ---\n", inode_num);
    printf("Tipo de Arquivo:         ");
    if (is_regular_file(inode.i_mode)) printf("Arquivo Regular\n");
    else if (is_directory(inode.i_mode)) printf("Diretorio\n");
    else if ((inode.i_mode & 0xF000) == 0xA000) printf("Link Simbolico\n");
    else printf("Outro/Desconhecido (modo 0x%X)\n", inode.i_mode);

    printf("Permissoes:              ");
    putchar((inode.i_mode & 0x0100) ? 'r' : '-');
    putchar((inode.i_mode & 0x0080) ? 'w' : '-');
    putchar((inode.i_mode & 0x0040) ? 'x' : '-');
    putchar((inode.i_mode & 0x0020) ? 'r' : '-');
    putchar((inode.i_mode & 0x0010) ? 'w' : '-');
    putchar((inode.i_mode & 0x0008) ? 'x' : '-');
    putchar((inode.i_mode & 0x0004) ? 'r' : '-');
    putchar((inode.i_mode & 0x0002) ? 'w' : '-');
    putchar((inode.i_mode & 0x0001) ? 'x' : '-');
    printf(" (0%o)\n", inode.i_mode & 0x0FFF);

    printf("UID do Dono:             %u\n", inode.i_uid);
    printf("GID do Grupo:            %u\n", inode.i_gid);
    printf("Contador de Links:       %u\n", inode.i_links_count);
    printf("Tamanho:                 %llu bytes\n", (unsigned long long)size);
    printf("Blocos de 512B Alocados: %u\n", inode.i_blocks_lo);
    printf("Flags:                   0x%08X\n", inode.i_flags);

    if (inode.i_flags & EXT4_EXTENTS_FL) {
        struct ext4_extent_header *eh =
            (struct ext4_extent_header *)inode.i_block;
        printf("Usa Extents:             Sim\n");
        printf("Extents validos:         %u\n", eh->eh_entries);
        printf("Profundidade da arvore:  %u\n", eh->eh_depth);
    } else {
        printf("Usa Extents:             Nao\n");
    }

    //vivian: transforma timestamp em data legivel
    time_t at = (time_t)inode.i_atime;
    time_t ct = (time_t)inode.i_ctime;
    time_t mt = (time_t)inode.i_mtime;
    time_t dt = (time_t)inode.i_dtime;

    printf("atime:                   %s", inode.i_atime ? ctime(&at) : "0\n");
    printf("ctime:                   %s", inode.i_ctime ? ctime(&ct) : "0\n");
    printf("mtime:                   %s", inode.i_mtime ? ctime(&mt) : "0\n");
    printf("dtime:                   %s", inode.i_dtime ? ctime(&dt) : "0\n");
    printf("----------------------------------------\n\n");
}

void ext4_testi(uint32_t inode_num) {
    if (inode_num == 0) {
        printf("Erro: O Inode 0 não é válido no EXT4.\n");
        return;
    }

    struct ext4_inode inode;
    
    if (!read_inode(inode_num, &inode)) {
        printf("Resultado: Inode %u esta LIVRE (ou fora dos limites)\n", inode_num);
        return;
    }

    if (inode.i_mode == 0 && inode.i_links_count == 0) {
        printf("Resultado: Inode %u esta LIVRE\n", inode_num);
    } else {
        printf("Resultado: Inode %u esta OCUPADO\n", inode_num);
    }
}

void ext4_testb(uint32_t block_num) {
    if (!disk_image) {
        printf("Erro: Nenhuma imagem de disco aberta.\n");
        return;
    }

    uint32_t blocks_per_group;
    uint32_t first_data_block;
    uint32_t total_blocks;
    uint16_t desc_size;

    fseek(disk_image, 1024 + 4, SEEK_SET);
    fread(&total_blocks, sizeof(uint32_t), 1, disk_image);

    fseek(disk_image, 1024 + 32, SEEK_SET);
    fread(&blocks_per_group, sizeof(uint32_t), 1, disk_image);

    fseek(disk_image, 1024 + 20, SEEK_SET);
    fread(&first_data_block, sizeof(uint32_t), 1, disk_image);

    if (block_num < first_data_block || block_num >= total_blocks) {
        printf("Erro: Bloco %u é invalido (Validos: %u a %u).\n", 
               block_num, first_data_block, total_blocks - 1);
        return;
    }

    fseek(disk_image, 1024 + 254, SEEK_SET);
    fread(&desc_size, sizeof(uint16_t), 1, disk_image);
    if (desc_size < 32 || desc_size > 1024) desc_size = 32;

    uint32_t group = (block_num - first_data_block) / blocks_per_group;
    uint32_t index = (block_num - first_data_block) % blocks_per_group;

    uint64_t bgdt_offset = (global_block_size == 1024) ? 2048 : global_block_size;
    uint64_t descriptor_offset = bgdt_offset + (group * desc_size);

    uint32_t block_bitmap_block;
    fseek(disk_image, descriptor_offset + 0, SEEK_SET);
    fread(&block_bitmap_block, sizeof(uint32_t), 1, disk_image);

    char *block_buffer = calloc(1, global_block_size);
    read_block(block_bitmap_block, block_buffer);

    uint8_t byte_val = block_buffer[index / 8];
    uint8_t bit_pos = index % 8;

    if (byte_val & (1 << bit_pos)) {
        printf("Resultado: Bloco %u esta OCUPADO\n", block_num);
    } else {
        printf("Resultado: Bloco %u esta LIVRE\n", block_num);
    }

    free(block_buffer);
}

void ext4_export(uint32_t file_inode_num, const char *target_path) {
    struct ext4_inode inode;
    uint64_t size;
    uint64_t bytes_remaining;
    uint32_t total_blocks;
    char *block_buffer;
    FILE *out_file;

    if (file_inode_num == 0) {
        printf("Erro: Arquivo origem nao encontrado na imagem.\n");
        return;
    }

    if (!read_inode(file_inode_num, &inode)) {
        printf("Erro: Falha ao ler o Inode %u.\n", file_inode_num);
        return;
    }

    if (!is_regular_file(inode.i_mode)) {
        printf("Erro: O Inode %u nao e um arquivo regular. Nao pode ser exportado.\n", file_inode_num);
        return;
    }

    out_file = fopen(target_path, "wb");
    if (!out_file) {
        printf("Erro: Nao foi possivel criar o arquivo destino '%s' no seu SO.\n", target_path);
        return;
    }

    size = inode_size_bytes(&inode);
    bytes_remaining = size;
    total_blocks = (uint32_t)((size + global_block_size - 1) / global_block_size);

    block_buffer = malloc(global_block_size);
    if (!block_buffer) {
        printf("Erro de alocacao de memoria.\n");
        fclose(out_file);
        return;
    }

    for (uint32_t i = 0; i < total_blocks; i++) {
        uint64_t phys_block = map_logical_to_physical_block(&inode, i);
        uint32_t bytes_to_write = (bytes_remaining > global_block_size) ? global_block_size : (uint32_t)bytes_remaining;

        if (phys_block == 0 || !read_block(phys_block, block_buffer)) {
            memset(block_buffer, 0, global_block_size);
        }

        fwrite(block_buffer, 1, bytes_to_write, out_file);
        bytes_remaining -= bytes_to_write;
    }

    fclose(out_file);
    free(block_buffer);
    printf("Sucesso! Arquivo exportado para '%s' (%llu bytes).\n", target_path, (unsigned long long)size);
}

void ext4_import(const char *host_path, const char *dest_name, uint32_t parent_dir_inode) {
    if (!disk_image || !host_path || !dest_name) return;

    if (ext4_lookup(parent_dir_inode, dest_name) != 0) {
        printf("Erro: O arquivo '%s' ja existe no diretorio atual.\n", dest_name);
        return;
    }

    FILE *src = fopen(host_path, "rb");
    if (!src) {
        printf("Erro: Nao foi possivel abrir o arquivo '%s' no sistema hospedeiro.\n", host_path);
        return;
    }

    fseek(src, 0, SEEK_END);
    long file_size_long = ftell(src);
    fseek(src, 0, SEEK_SET);

    if (file_size_long < 0) {
        printf("Erro: Nao foi possivel determinar o tamanho do arquivo.\n");
        fclose(src);
        return;
    }

    uint32_t file_size = (uint32_t)file_size_long;
    uint32_t blocks_needed = (file_size > 0) ? (file_size + global_block_size - 1) / global_block_size : 0;

    if (blocks_needed > 4 * 32768) {
        printf("Erro: Arquivo muito grande para os extents suportados.\n");
        fclose(src);
        return;
    }

    uint32_t new_ino = alloc_free_inode();
    if (!new_ino) {
        printf("Erro: Sem Inodes livres no disco.\n");
        fclose(src);
        return;
    }

    uint32_t *allocated_blocks = NULL;
    if (blocks_needed > 0) {
        allocated_blocks = malloc(blocks_needed * sizeof(uint32_t));
        if (!allocated_blocks) {
            printf("Erro de alocacao de memoria.\n");
            ext4_free_inode(new_ino);
            fclose(src);
            return;
        }
    }

    char *buf = malloc(global_block_size);
    if (!buf) {
        printf("Erro de alocacao de memoria.\n");
        if (allocated_blocks) free(allocated_blocks);
        ext4_free_inode(new_ino);
        fclose(src);
        return;
    }

    for (uint32_t i = 0; i < blocks_needed; i++) {
        allocated_blocks[i] = alloc_free_block();
        if (allocated_blocks[i] == 0) {
            printf("Erro: Sem blocos livres (alocou %u de %u).\n", i, blocks_needed);
            //rollback 
            for (uint32_t j = 0; j < i; j++) ext4_free_block(allocated_blocks[j]); //libera blocos alocados pelo import, evita lixo
            ext4_free_inode(new_ino); //libera inode alocado pelo import
            free(allocated_blocks); //libera alocacao de memoria
            free(buf);
            fclose(src);
            return;
        }

        // Lê do arquivo do sistema hospedeiro e grava na imagem
        memset(buf, 0, global_block_size);
        fread(buf, 1, global_block_size, src);
        write_block(allocated_blocks[i], buf);
    }
    free(buf);
    fclose(src);

    struct ext4_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = 0x81A4;
    ino.i_links_count = 1;
    ino.i_size_lo = file_size;
    ino.i_blocks_lo = blocks_needed * (global_block_size / 512);
    ino.i_flags = EXT4_EXTENTS_FL;

    //Indica informações sobre a estrutura de extents
    struct ext4_extent_header *eh = (struct ext4_extent_header *)ino.i_block;
    eh->eh_magic = EXT4_EXT_MAGIC;
    eh->eh_entries = 0;
    eh->eh_max = 4;
    eh->eh_depth = 0;

    if (blocks_needed > 0) {
        struct ext4_extent *extents = (struct ext4_extent *)((char *)ino.i_block + sizeof(*eh));
        uint32_t ext_idx = 0;
        uint32_t run_start = 0;

        for (uint32_t i = 1; i <= blocks_needed; i++) {
            if (i == blocks_needed || allocated_blocks[i] != allocated_blocks[i - 1] + 1) {
                if (ext_idx >= 4) {
                    printf("Aviso: Fragmentacao excessiva, extents limitados a 4 faixas.\n");
                    break;
                }
                extents[ext_idx].ee_block = run_start;
                extents[ext_idx].ee_len = i - run_start;
                extents[ext_idx].ee_start_hi = 0;
                extents[ext_idx].ee_start_lo = allocated_blocks[run_start];
                ext_idx++;
                run_start = i;
            }
        }
        eh->eh_entries = ext_idx;
    }

    write_inode(new_ino, &ino);
    if (allocated_blocks) free(allocated_blocks);

    if (add_dir_entry(parent_dir_inode, new_ino, dest_name, 1)) {
        printf("Sucesso: Arquivo '%s' importado como Inode %u (%u bytes, %u blocos).\n",
               dest_name, new_ino, file_size, blocks_needed);
    } else {
        if (blocks_needed > 0) {
            struct ext4_extent *exts = (struct ext4_extent *)((char *)ino.i_block + sizeof(*eh));
            for (uint16_t e = 0; e < eh->eh_entries; e++) {
                for (uint32_t b = 0; b < exts[e].ee_len; b++) {
                    ext4_free_block(exts[e].ee_start_lo + b);
                }
            }
        }
        ext4_free_inode(new_ino);
        printf("Erro: Nao foi possivel vincular '%s' ao diretorio pai.\n", dest_name);
    }
}

// ====================================================================
// COMANDOS DE MODIFICAÇÃO (TOUCH, MKDIR, RM, RENAME, RMDIR)
// ====================================================================

void ext4_touch(uint32_t parent_dir_inode, const char *filename) {
    if (!disk_image) return;

    if (ext4_lookup(parent_dir_inode, filename) != 0) {
        printf("Erro: O arquivo '%s' ja existe no diretorio pai (%u).\n", filename, parent_dir_inode);
        return;
    }

    uint32_t new_ino = alloc_free_inode();
    if (!new_ino) { printf("Erro: Sem Inodes livres disponíveis no disco.\n"); return; }

    struct ext4_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = 0x81A4;
    ino.i_links_count = 1;
    ino.i_size_lo = 0;
    ino.i_blocks_lo = 0;
    ino.i_flags = EXT4_EXTENTS_FL;

    struct ext4_extent_header *eh = (struct ext4_extent_header *)ino.i_block;
    eh->eh_magic = EXT4_EXT_MAGIC;
    eh->eh_entries = 0;
    eh->eh_max = 4;
    eh->eh_depth = 0;

    write_inode(new_ino, &ino);

    if (add_dir_entry(parent_dir_inode, new_ino, filename, 1)) {
        printf("Sucesso: Arquivo '%s' criado com Inode %u.\n", filename, new_ino);
    } else {
        ext4_free_inode(new_ino);
        printf("Erro ao vincular '%s' ao diretorio pai.\n", filename);
    }
}

void ext4_mkdir(uint32_t parent_dir_inode, const char *dirname) {
    if (!disk_image) return;

    if (ext4_lookup(parent_dir_inode, dirname) != 0) {
        printf("Erro: O diretorio '%s' ja existe.\n", dirname);
        return;
    }

    uint32_t new_ino = alloc_free_inode();
    uint32_t new_blk = alloc_free_block();
    if (!new_ino || !new_blk) { printf("Erro: Recursos insuficientes no disco.\n"); return; }

    char *buf = calloc(1, global_block_size);
    
    struct ext4_dir_entry_2 *dot = (struct ext4_dir_entry_2 *)buf;
    dot->inode = new_ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = 2;
    memcpy(dot->name, ".", 1);

    struct ext4_dir_entry_2 *dotdot = (struct ext4_dir_entry_2 *)(buf + 12);
    dotdot->inode = parent_dir_inode;
    dotdot->rec_len = global_block_size - 12;
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    memcpy(dotdot->name, "..", 2);

    write_block(new_blk, buf);
    free(buf);

    struct ext4_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = 0x41ED;
    ino.i_links_count = 2;
    ino.i_size_lo = global_block_size;
    ino.i_blocks_lo = (global_block_size / 512);
    ino.i_flags = EXT4_EXTENTS_FL;

    struct ext4_extent_header *eh = (struct ext4_extent_header *)ino.i_block;
    eh->eh_magic = EXT4_EXT_MAGIC;
    eh->eh_entries = 1;
    eh->eh_max = 4;
    eh->eh_depth = 0;

    struct ext4_extent *ext = (struct ext4_extent *)((char *)ino.i_block + sizeof(*eh));
    ext->ee_block = 0;
    ext->ee_len = 1;
    ext->ee_start_hi = 0;
    ext->ee_start_lo = new_blk;

    write_inode(new_ino, &ino);

    struct ext4_inode p_ino;
    if (read_inode(parent_dir_inode, &p_ino)) {
        p_ino.i_links_count++;
        write_inode(parent_dir_inode, &p_ino);
    }

    if (add_dir_entry(parent_dir_inode, new_ino, dirname, 2)) {
        printf("Sucesso: Diretorio '%s' criado com Inode %u (Bloco %u).\n", dirname, new_ino, new_blk);
    } else {
        printf("Erro ao vincular novo diretorio.\n");
    }
}

void ext4_rm(uint32_t parent_dir_inode, const char *filename) {
    if (!disk_image) return;

    uint32_t target_ino = ext4_lookup(parent_dir_inode, filename);
    if (target_ino == 0) {
        printf("Erro: Arquivo '%s' nao encontrado.\n", filename);
        return;
    }

    struct ext4_inode target_inode_struct;
    read_inode(target_ino, &target_inode_struct);
    if (is_directory(target_inode_struct.i_mode)) {
        printf("Erro: '%s' e um diretorio. Use rmdir.\n", filename);
        return;
    }

    uint64_t parent_phys_block = heal_directory_block(parent_dir_inode);
    if (parent_phys_block == 0) {
        printf("Erro: Nao foi possivel localizar o bloco do diretorio pai.\n");
        return;
    }

    char *buf = malloc(global_block_size);
    if (!buf) return;

    int deleted = 0;
    if (read_block(parent_phys_block, buf)) {
        uint32_t offset = 0;
        struct ext4_dir_entry_2 *prev = NULL;

        while (offset < global_block_size) {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(buf + offset);
            if (de->rec_len < 8 || offset + de->rec_len > global_block_size) break;

            if (de->inode == target_ino && de->name_len == strlen(filename) &&
                memcmp(de->name, filename, de->name_len) == 0) {
                
                if (prev != NULL) {
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }

                write_block(parent_phys_block, buf);
                deleted = 1;
                break;
            }
            prev = de;
            offset += de->rec_len;
        }
    }
    free(buf);

    if (!deleted) {
        printf("Erro: Nao foi possivel desvincular o arquivo do diretorio.\n");
        return;
    }

    if (target_inode_struct.i_links_count > 0) target_inode_struct.i_links_count--;

    if (target_inode_struct.i_links_count == 0) {
        target_inode_struct.i_dtime = (uint32_t)time(NULL);
        target_inode_struct.i_mode = 0; //vivian: limpa o mode para o inode ficar LIVRE

        if ((target_inode_struct.i_flags & EXT4_EXTENTS_FL) && target_inode_struct.i_size_lo > 0) {
            struct ext4_extent_header *eh = (struct ext4_extent_header *)target_inode_struct.i_block;
            if (eh->eh_magic == EXT4_EXT_MAGIC && eh->eh_depth == 0) {
                struct ext4_extent *ext = (struct ext4_extent *)((char *)target_inode_struct.i_block + sizeof(*eh));
                for (uint16_t i = 0; i < eh->eh_entries; i++) {
                    uint64_t start_blk = ext[i].ee_start_lo;
                    for (uint32_t len = 0; len < ext[i].ee_len; len++) {
                        ext4_free_block(start_blk + len);
                    }
                }
            }
        }
        ext4_free_inode(target_ino);
    }
    write_inode(target_ino, &target_inode_struct);
    printf("Sucesso: Arquivo '%s' (Inode %u) removido do sistema.\n", filename, target_ino);
}

void ext4_rename(uint32_t dir_inode_num, const char *old_name, const char *new_name) {
    if (!disk_image || !old_name || !new_name) return;

    if (strlen(new_name) > 255) {
        printf("Erro: nome muito grande.\n");
        return;
    }

    if (ext4_lookup(dir_inode_num, old_name) == 0) {
        printf("Erro: '%s' não encontrado.\n", old_name);
        return;
    }

    if (ext4_lookup(dir_inode_num, new_name) != 0) {
        printf("Erro: já existe um arquivo chamado '%s'.\n", new_name);
        return;
    }

    uint64_t block = heal_directory_block(dir_inode_num);
    if (block == 0) {
        printf("Erro: não foi possível localizar o bloco do diretório.\n");
        return;
    }

    char *buffer = malloc(global_block_size);
    if (!buffer) return;

    if (!read_block(block, buffer)) {
        free(buffer);
        return;
    }

    uint32_t offset = 0;
    while (offset < global_block_size) {
        struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)(buffer + offset);
        if (!valid_dir_entry(entry, offset)) break;

        if (entry->inode != 0) {
            char name[256];
            memcpy(name, entry->name, entry->name_len);
            name[entry->name_len] = '\0';

            if (strcmp(name, old_name) == 0) {
                uint16_t required = (uint16_t)((8 + strlen(new_name) + 3) & ~3);

                if (required > entry->rec_len) {
                    printf("Erro: não há espaço para o novo nome.\n");
                    free(buffer);
                    return;
                }

                memset(entry->name, 0, entry->rec_len - 8);
                memcpy(entry->name, new_name, strlen(new_name));
                entry->name_len = strlen(new_name);

                if (!write_block(block, buffer)) {
                    printf("Erro ao gravar o bloco.\n");
                } else {
                    printf("'%s' renomeado para '%s'.\n", old_name, new_name);
                }

                free(buffer);
                return;
            }
        }
        offset += entry->rec_len;
    }

    printf("Erro: entrada do diretório não encontrada.\n");
    free(buffer);
}

void ext4_rmdir(uint32_t parent_inode_num, const char *dir_name) {
    uint32_t target_inode_num = ext4_lookup(parent_inode_num, dir_name);
    if (target_inode_num == 0) {
        printf("Erro: O diretorio '%s' nao foi encontrado.\n", dir_name);
        return;
    }

    uint64_t target_phys_block = heal_directory_block(target_inode_num);
    if (target_phys_block == 0) {
        printf("Erro: '%s' nao e um diretorio.\n", dir_name);
        return;
    }

    int entry_count = 0;
    char *dir_buf = calloc(1, global_block_size);
    if (read_block(target_phys_block, dir_buf)) {
        uint32_t offset = 0;
        while (offset < global_block_size) {
            struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)(dir_buf + offset);
            if (entry->rec_len == 0) break;
            if (entry->inode != 0) entry_count++;
            offset += entry->rec_len;
        }
    }
    free(dir_buf);

    if (entry_count > 2) {
        printf("Erro: O diretorio '%s' nao esta vazio.\n", dir_name);
        return;
    }

    uint64_t parent_phys_block = heal_directory_block(parent_inode_num);
    if (parent_phys_block == 0) {
        printf("Erro: Nao foi possivel localizar o bloco do diretorio pai.\n");
        return;
    }

    char *block_buffer = calloc(1, global_block_size);
    int deleted = 0;

    if (read_block(parent_phys_block, block_buffer)) {
        uint32_t offset = 0;
        struct ext4_dir_entry_2 *prev = NULL;

        while (offset < global_block_size) {
            struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)(block_buffer + offset);
            if (entry->rec_len == 0) break;

            if (entry->inode == target_inode_num &&
                entry->name_len == strlen(dir_name) &&
                memcmp(entry->name, dir_name, entry->name_len) == 0) 
            {
                if (prev != NULL) {
                    prev->rec_len += entry->rec_len; 
                } 
                entry->inode = 0; 
                write_block(parent_phys_block, block_buffer); 
                deleted = 1;
                break;
            }

            prev = entry;
            offset += entry->rec_len;
        }
    }
    free(block_buffer);

    if (!deleted) {
        printf("Erro: nao foi possivel remover a entrada do diretorio pai.\n");
        return;
    }

    struct ext4_inode target_inode_struct;
    if (read_inode(target_inode_num, &target_inode_struct)) {
        target_inode_struct.i_links_count = 0;
        target_inode_struct.i_dtime = (uint32_t)time(NULL);
        target_inode_struct.i_mode = 0; // limpa o mode para o inode ficar LIVRE
        write_inode(target_inode_num, &target_inode_struct);
    }

    ext4_free_block(target_phys_block);
    ext4_free_inode(target_inode_num);
    printf("Sucesso: Diretorio '%s' removido.\n", dir_name);
}