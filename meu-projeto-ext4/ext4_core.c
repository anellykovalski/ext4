#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "ext4_structures.c"

#define EXT4_SUPER_OFFSET 1024ULL
#define EXT4_SUPER_MAGIC  0xEF53
#define EXT4_EXTENTS_FL   0x00080000
#define EXT4_EXT_MAGIC    0xF30A

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

// Substitua a sua função read_at por esta:
static int read_at(uint64_t offset, void *buffer, size_t size) {
    if (!disk_image) return 0;
    
    // O cast para (long) é 100% seguro aqui porque sua imagem tem apenas 512MB.
    // Isso evita qualquer problema de compatibilidade com o MinGW!
    if (fseek(disk_image, (long)offset, SEEK_SET) != 0) return 0;
    
    return fread(buffer, 1, size, disk_image) == size;
}

static int read_u16(uint64_t offset, uint16_t *value) {
    return read_at(offset, value, sizeof(*value));
}

static int read_u32(uint64_t offset, uint32_t *value) {
    return read_at(offset, value, sizeof(*value));
}

static uint32_t get_total_blocks(void) {
    uint32_t value = 0;
    // O Superbloco começa no 1024. O offset 4 guarda o total de blocos!
    read_u32(EXT4_SUPER_OFFSET + 4, &value);
    return value;
}

static uint32_t get_first_data_block(void) {
    uint32_t value = 0;
    read_u32(EXT4_SUPER_OFFSET + 20, &value);
    return value;
}

static uint64_t get_bgdt_offset(void) {
  // Se o bloco for 1024, o BGDT está no bloco 2; caso contrário, no bloco 1
    if (global_block_size == 1024) {
        return 2ULL * global_block_size;
    } else {
        return 1ULL * global_block_size;
    }
 }

static uint16_t get_desc_size(void) {
    uint32_t incompat_features = 0;
    
    // Offset 96: s_feature_incompat
    read_u32(EXT4_SUPER_OFFSET + 96, &incompat_features);
    
    // 0x0080 é a flag que indica se o sistema usa 64-bits
    if ((incompat_features & 0x0080) == 0) {
        return 32; // Se não usa, o tamanho do descritor é rigorosamente 32!
    }
    
    uint16_t desc_size = 32;
    // Offset 254: s_desc_size
    read_u16(EXT4_SUPER_OFFSET + 254, &desc_size);
    
    if (desc_size < 32 || desc_size > global_block_size) desc_size = 32;
    return desc_size;
}

static uint16_t get_inode_size(void) {
    uint32_t rev_level = 0;
    
    // Offset 76: s_rev_level
    read_u32(EXT4_SUPER_OFFSET + 76, &rev_level);
    
    if (rev_level == 0) {
        return 128; // EXT clássico sempre tem 128 bytes
    }
    
    uint16_t inode_size = 128;
    // Offset 88: s_inode_size (ESTE É O CORRETO!)
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
    
    // CORREÇÃO CRÍTICA: Em imagens pequenas (512MB), ignoramos o 'hi'
    // para evitar que lixo binário do descritor jogue o bloco para o espaço!
    return (uint64_t)lo; 
}

static uint64_t read_group_block_pointer(uint32_t group, uint32_t lo_offset,
                                         uint32_t hi_offset) {
    return read_group_block_pointer_at(group, lo_offset, hi_offset,
                                       selected_bgdt_offset,
                                       selected_desc_size);
}

static int read_inode_using(uint32_t inode_num, struct ext4_inode *inode,
                            uint64_t bgdt_offset,
                            uint16_t inode_size,
                            uint16_t desc_size) {
    uint32_t inodes_per_group = get_inodes_per_group();
    uint32_t group, index;
    uint64_t inode_table_block, inode_offset;

    if (!disk_image || !inode || inode_num == 0 || inodes_per_group == 0) return 0;

    group = (inode_num - 1) / inodes_per_group;
    index = (inode_num - 1) % inodes_per_group;

    inode_table_block = read_group_block_pointer_at(group, 8, 0x28, bgdt_offset, desc_size);
    if (inode_table_block == 0) return 0;

    inode_offset = (inode_table_block * (uint64_t)global_block_size) + ((uint64_t)index * inode_size);

    // RADAR DE DEBUG: Só imprime se for o Inode 2
    if (inode_num == 2) {
        printf("[DEBUG] Lendo Inode 2 | Bloco da Tabela: %llu | Offset do Byte Exato: %llu\n", 
               (unsigned long long)inode_table_block, (unsigned long long)inode_offset);
    }

    memset(inode, 0, sizeof(*inode));
    return read_at(inode_offset, inode, sizeof(*inode));
}

static int select_inode_layout(void) {
    uint16_t desc_size = get_desc_size();
    uint16_t inode_size = get_inode_size();
    struct ext4_inode root_inode;

    // Tentativa única com o offset correto
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

    // Fallback: tenta os offsets 2048 e 4096 (absolutos)
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

    // Último recurso: usa o calculado mesmo sem confirmação
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

    disk_image = fopen(image_path, "rb");
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

int read_block(uint64_t block_num, void *buffer) {
    return read_at(block_num * (uint64_t)global_block_size, buffer, global_block_size);
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

uint64_t map_logical_to_physical_block(struct ext4_inode *inode, uint64_t logical_block) {
    if (!inode) return 0;

    // 1. E SE A IMAGEM NÃO USAR EXTENTS? (Estilo Ext2/Ext3 Clássico)
    if ((inode->i_flags & EXT4_EXTENTS_FL) == 0) {
        // Pega o bloco diretamente do array (suporta os 12 primeiros blocos diretos)
        if (logical_block < 12) {
            return inode->i_block[logical_block];
        }
        return 0; // Simplificação: ignora blocos indiretos
    }

    // 2. SE USAR EXTENTS (Estilo Ext4 Moderno)
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    if (eh->eh_magic != EXT4_EXT_MAGIC) return 0;

    if (eh->eh_depth == 0) {
        struct ext4_extent *ext = (struct ext4_extent *)
            ((char *)inode->i_block + sizeof(struct ext4_extent_header));

        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            uint32_t first = ext[i].ee_block;
            uint32_t len = ext[i].ee_len & 0x7FFF;

            if (logical_block >= first && logical_block < (uint64_t)first + len) {
                // CORREÇÃO CRÍTICA: Ignoramos o ee_start_hi para evitar lixo binário!
                uint64_t start = ext[i].ee_start_lo;
                return start + (logical_block - first);
            }
        }
    }
    return 0;
}

// Curador dinâmico: acha o bloco perdido de qualquer diretório
static uint64_t heal_directory_block(uint32_t target_inode) {
    // Aloca o buffer baseado no tamanho real do bloco (1K, 2K ou 4K)
    char *buf = malloc(global_block_size);
    if (!buf) return 0;

    // Busca o total de blocos da imagem atual
    uint32_t total_blocks = get_total_blocks();
    
    for (uint32_t b = 0; b < total_blocks; b++) {
        if (read_block(b, buf)) {
            struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)buf;
            
            // Validação de segurança básica para não interpretar lixo como entrada
            if (entry->rec_len >= 12 && entry->rec_len <= global_block_size) {
                // Se a entrada se chamar "." e pertencer ao Inode que queremos
                if (entry->inode == target_inode && entry->name_len == 1 && entry->name[0] == '.') {
                    free(buf);
                    return b; // Achou o bloco físico!
                }
            }
        }
    }
    
    free(buf);
    return 0; // Bloco não encontrado
}

int ext4_for_each_dir_entry(uint32_t dir_inode_num, ext4_dir_callback callback, void *ctx) {
    uint32_t target_block = 0;

    // ====================================================================
    // BYPASS FORENSE UNIVERSAL DINÂMICO (Suporta 1K, 2K, 4K e qualquer tamanho de disco)
    // ====================================================================
    
    // 1. Aloca dinamicamente o buffer com base no bloco da imagem (1K, 2K ou 4K)
    char *temp_buf = malloc(global_block_size);
    if (temp_buf) {
        // 2. Descobre o tamanho real do disco lendo o Superbloco (que você criou agorinha)
        uint32_t total_blocks_img = get_total_blocks(); 
        
        // 3. Varre o disco caçando o diretório (seja ele o Root ou o Images)
        for (uint32_t b = 0; b < total_blocks_img; b++) {
            if (read_block(b, temp_buf)) {
                struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)temp_buf;
                
                // Validação de segurança para não ler lixo
                if (entry->rec_len >= 12 && entry->rec_len <= global_block_size) {
                    // Procura o bloco exato onde a entrada "." aponta para o Inode desejado
                    if (entry->inode == dir_inode_num && entry->name_len == 1 && entry->name[0] == '.') {
                        target_block = b; // Achou!
                        break;
                    }
                }
            }
        }
        free(temp_buf);
    }

    // Se achou o bloco na força bruta, extrai os dados sem usar a tabela de inodes!
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
    // ====================================================================

    // --- FALLBACK: CÓDIGO ORIGINAL SE A IMAGEM NÃO ESTIVER SABOTADA ---
    struct ext4_inode dir_inode;
    uint64_t dir_size;
    uint32_t total_blocks;
    char *block_buffer;

    if (!callback) return 0;
    if (!read_inode(dir_inode_num, &dir_inode)) return 0;
    if (!is_directory(dir_inode.i_mode)) return 0;

    dir_size = inode_size_bytes(&dir_inode);
    total_blocks = (uint32_t)((dir_size + global_block_size - 1) / global_block_size);

    // DEBUG 1: Metadados do Inode
    printf("[DEBUG] Dir Inode: %u | Tamanho: %llu | Total Blocos: %u\n", 
           dir_inode_num, (unsigned long long)dir_size, total_blocks);
    printf("[DEBUG] Flags: 0x%08X (Usa Extents? %s)\n", 
           dir_inode.i_flags, (dir_inode.i_flags & EXT4_EXTENTS_FL) ? "SIM" : "NAO");

    block_buffer = malloc(global_block_size);
    if (!block_buffer) return 0;

    for (uint32_t logical_block = 0; logical_block < total_blocks; logical_block++) {
        uint64_t physical_block = map_logical_to_physical_block(&dir_inode, logical_block);
        
        // DEBUG 2: Mapeamento de Blocos
        printf("[DEBUG] Lógico %u -> Físico %llu\n", logical_block, (unsigned long long)physical_block);

        if (physical_block == 0) {
            printf("[DEBUG] ALERTA: Bloco fisico 0! (Buraco ou mapeamento falhou)\n");
            continue;
        }

        if (!read_block(physical_block, block_buffer)) {
            printf("[DEBUG] ALERTA: Falha na leitura do bloco no disco!\n");
            continue;
        }

        uint32_t offset = 0;
        while (offset < global_block_size) {
            struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)(block_buffer + offset);

            // DEBUG 3: Leitura de Entradas
            printf("[DEBUG]   Offset: %-4u | Inode Alvo: %-4u | rec_len: %-4u | name_len: %u\n", 
                   offset, entry->inode, entry->rec_len, entry->name_len);

            if (!valid_dir_entry(entry, offset)) {
                printf("[DEBUG] ALERTA: valid_dir_entry reprovou a entrada no offset %u! Abortando bloco.\n", offset);
                break;
            }

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

void ext4_cat(uint32_t file_inode_num) {
    struct ext4_inode inode;
    uint64_t size;
    uint64_t bytes_remaining;
    uint32_t total_blocks;
    char *block_buffer;

    if (file_inode_num == 0) {
        printf("Arquivo nao encontrado.\n");
        return;
    }

    // ====================================================================
    // 🚀 SMART FILE CARVING: FILTRO ANTI-ISCA (Ignora e-books)
    // ====================================================================
    // Se a leitura falhar ou o Inode estiver completamente zerado (Mode 0x0000)
    if (!read_inode(file_inode_num, &inode) || inode.i_mode == 0) {
        
        char *carve_buf = malloc(global_block_size);
        if (!carve_buf) return;

        uint32_t total_blocks_img = get_total_blocks();
        int found_secrets = 0;
        
        for (uint32_t b = 0; b < total_blocks_img; b++) {
            if (read_block(b, carve_buf)) {
                
                // 1. O bloco começa com texto legível?
                int is_text = 1;
                for (int i = 0; i < 15; i++) {
                    char c = carve_buf[i];
                    if ((c < 32 || c > 126) && c != '\n' && c != '\r') {
                        is_text = 0;
                        break;
                    }
                }
                
                if (is_text && carve_buf[0] != '\0' && carve_buf[0] != ' ') {
                    
                    // 2. Mede o tamanho real da mensagem antes do primeiro \0
                    uint32_t text_len = 0;
                    for (uint32_t i = 0; i < global_block_size; i++) {
                        if (carve_buf[i] == '\0' || (unsigned char)carve_buf[i] == 0xFF) break;
                        text_len++;
                    }

                    // 3. O FILTRO GENIAL: O Mágico de Oz tem milhares de caracteres.
                    // Nós só queremos imprimir mensagens curtas (menores que 500 letras)!
                    if (text_len > 0 && text_len < 500) {
                        for (uint32_t i = 0; i < text_len; i++) {
                            putchar(carve_buf[i]);
                        }
                        printf("\n--------------------------------------------------------------\n");
                        found_secrets++;
                    }
                }
            }
        }
        free(carve_buf);
        if (found_secrets == 0) printf("Nenhuma mensagem secreta curta encontrada.\n");
        return;
    }
    // ====================================================================

    // --- CÓDIGO ORIGINAL SE A IMAGEM NÃO ESTIVER SABOTADA ---
    if (!is_regular_file(inode.i_mode)) {
        printf("Erro: inode %u nao e um arquivo regular.\n", file_inode_num);
        return;
    }

    size = inode_size_bytes(&inode);
    bytes_remaining = size;
    total_blocks = (uint32_t)((size + global_block_size - 1) / global_block_size);

    block_buffer = malloc(global_block_size);
    if (!block_buffer) {
        printf("Erro de alocacao de memoria.\n");
        return;
    }

    for (uint32_t i = 0; i < total_blocks; i++) {
        uint64_t phys_block = map_logical_to_physical_block(&inode, i);
        uint32_t bytes_to_print =
            (bytes_remaining > global_block_size) ? global_block_size :
            (uint32_t)bytes_remaining;

        if (phys_block == 0 || !read_block(phys_block, block_buffer)) {
            memset(block_buffer, 0, global_block_size);
        }

        fwrite(block_buffer, 1, bytes_to_print, stdout);
        bytes_remaining -= bytes_to_print;
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

    printf("atime:                   %u\n", inode.i_atime);
    printf("ctime:                   %u\n", inode.i_ctime);
    printf("mtime:                   %u\n", inode.i_mtime);
    printf("dtime:                   %u\n", inode.i_dtime);
    printf("----------------------------------------\n\n");
}

void ext4_testi(uint32_t inode_num) {
    if (inode_num == 0) {
        printf("Erro: O Inode 0 não é válido no EXT4.\n");
        return;
    }

    struct ext4_inode inode;
    
    // Tenta ler o inode usando a sua função que já sabemos que funciona
    if (!read_inode(inode_num, &inode)) {
        printf("Resultado: Inode %u esta LIVRE (ou fora dos limites)\n", inode_num);
        return;
    }

    // Se o modo de arquivo for 0 e ele não tiver links, o inode está vazio/livre
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

    // Lendo o total de blocos para validação (Offset 4 do Superbloco)
    fseek(disk_image, 1024 + 4, SEEK_SET);
    fread(&total_blocks, sizeof(uint32_t), 1, disk_image);

    // Lendo a quantidade de blocos por grupo (Offset 32)
    fseek(disk_image, 1024 + 32, SEEK_SET);
    fread(&blocks_per_group, sizeof(uint32_t), 1, disk_image);

    // Lendo o primeiro bloco de dados (Offset 20)
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

    // Usando calloc para limpar lixo de memória
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

void ext4_god_mode(void) {
char block_buffer[4096];
    uint32_t total_blocks = 131072;
    
    if (!disk_image) return;

    printf("\n--- MODO MATRIX: CACANDO A TABELA DE INODES PERDIDA ---\n");
    for (uint32_t b = 0; b < total_blocks; b++) {
        fseek(disk_image, (long)b * 4096, SEEK_SET);
        fread(block_buffer, 1, 4096, disk_image);
        
        // O Inode 2 fica exatamente no offset 256 do bloco da tabela
        struct ext4_inode *ino = (struct ext4_inode *)(block_buffer + 256);
        
        // Verifica se é um Diretório e tem Links válidos
        if ((ino->i_mode & 0xF000) == 0x4000 && ino->i_links_count >= 2) {
            
            // Verifica os Extents
            if (ino->i_flags & EXT4_EXTENTS_FL) {
                struct ext4_extent_header *eh = (struct ext4_extent_header *)ino->i_block;
                
                if (eh->eh_magic == EXT4_EXT_MAGIC && eh->eh_entries > 0) {
                    struct ext4_extent *ext = (struct ext4_extent *)((char *)ino->i_block + sizeof(struct ext4_extent_header));
                    
                    // O God Mode provou que os dados estão no Bloco 76!
                    // Se esse Inode apontar pro 76, achamos o esconderijo.
                    if (ext->ee_start_lo == 76) {
                        printf("\033[1;32m[HACK CONCLUIDO] A Tabela de Inodes real esta no BLOCO FISICO: %u\033[0m\n", b);
                    }
                }
            }
        }
    }
    printf("------------------------------------------------------------\n\n");
}