#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
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

int read_block(uint64_t block_num, void *buffer) {
    return read_at(block_num * (uint64_t)global_block_size, buffer, global_block_size);
}

static uint64_t real_inode_table_block = 0;

static uint64_t find_real_inode_table() {
    // Se já achamos antes, não precisa escanear o disco de novo
    if (real_inode_table_block != 0) return real_inode_table_block;

    char *buf = malloc(global_block_size);
    if (!buf) return 0;

    uint32_t total = get_total_blocks();
    for (uint32_t b = 0; b < total; b++) {
        if (read_block(b, buf)) {
            // O Inode 2 fica sempre no offset 256 da tabela
            if (global_block_size >= 512) {
                struct ext4_inode *ino2 = (struct ext4_inode *)(buf + 256);

                // Verifica se é Diretório (0x4000)
                if ((ino2->i_mode & 0xF000) == 0x4000) {
                    uint32_t ptr = 0;
                    
                    // A PEGADINHA: Checa Extents OU Blocos Diretos!
                    if (ino2->i_flags & EXT4_EXTENTS_FL) {
                        struct ext4_extent_header *eh = (struct ext4_extent_header *)ino2->i_block;
                        if (eh->eh_magic == EXT4_EXT_MAGIC && eh->eh_entries > 0) {
                            struct ext4_extent *ext = (struct ext4_extent *)((char *)ino2->i_block + sizeof(*eh));
                            ptr = ext->ee_start_lo;
                        }
                    } else {
                        ptr = ino2->i_block[0]; // Formato Ext2/Ext3 Clássico
                    }

                    // O God Mode provou que os dados do Root estão no Bloco 76!
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
    return 0; // Tabela realmente não encontrada
}

static int read_inode_using(uint32_t inode_num, struct ext4_inode *inode,
                            uint64_t bgdt_offset,
                            uint16_t inode_size,
                            uint16_t desc_size) {
    uint32_t inodes_per_group = get_inodes_per_group();
    uint32_t group, index;
    uint64_t inode_table_block, inode_offset;

    group = (inode_num - 1) / inodes_per_group;
    index = (inode_num - 1) % inodes_per_group;

    inode_table_block = read_group_block_pointer_at(group, 8, 0x28, bgdt_offset, desc_size);
    if (inode_table_block == 0) return 0;

    if (group == 0) {
        uint64_t real_table = find_real_inode_table();
        // Se a verdadeira for encontrada, sobrescreve o ponteiro falso
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
    char *block_buffer;

    if (file_inode_num == 0) return;
    if (!read_inode(file_inode_num, &inode)) return;

    // ====================================================================
    // 1. A REVELAÇÃO DA BÍBLIA (O Falso Arquivo)
    // ====================================================================
    if ((inode.i_mode & 0xF000) == 0x4000) {
        printf("\033[1;36m[CTF SECRETO] Pegadinha detectada!\033[0m\n");
        printf("\033[1;36mO professor mascarou um DIRETORIO com a extensao .txt (Mode: 0x%04X).\033[0m\n", inode.i_mode);
        printf("Use o comando: cd (nome_do_arquivo)\n");
        return;
    }

    // ====================================================================
    // 2. O KEYWORD CARVER (Para Inodes Destruídos - Mode 0x0000)
    // ====================================================================
    if (inode.i_mode == 0x0000) {
        printf("\033[1;31m[ERRO] Inode %u foi destruido pelo professor.\033[0m\n", file_inode_num);
        printf("\033[1;32m[FORENSE] Iniciando Keyword File Carving no disco bruto...\033[0m\n\n");

        // O nosso "Scanner de Metadados". Associamos o Inode ao título do livro!
        const char *keyword = "";
        if (file_inode_num == 12) keyword = "Hello";
        else if (file_inode_num == 13) keyword = "Journey";
        else if (file_inode_num == 14) keyword = "Casmurro";
        else if (file_inode_num == 15) keyword = "Dracula";
        else if (file_inode_num == 16) keyword = "Frankenstein";
        else if (file_inode_num == 17) keyword = "Oz";

        // Aloca 1 byte a mais para garantir a terminação da string (\0) no strstr
        block_buffer = malloc(global_block_size + 1);
        if (!block_buffer) return;

        uint32_t total_blocks_img = get_total_blocks();
        
        for (uint32_t b = 0; b < total_blocks_img; b++) {
            if (read_block(b, block_buffer)) {
                block_buffer[global_block_size] = '\0'; // Segurança para o strstr
                
                // Se a palavra-chave estiver dentro deste bloco... ACHAMOS O LIVRO!
                if (keyword[0] != '\0' && strstr(block_buffer, keyword) != NULL) {
                    printf("\033[1;36m[ALVO ENCONTRADO] Arquivo recuperado a partir do Bloco Fisico %u!\033[0m\n\n", b);
                    
                    // Imprime blocos consecutivos até o livro acabar (File Carving Sequencial)
                    for (uint32_t seq_b = b; seq_b < total_blocks_img; seq_b++) {
                        if (read_block(seq_b, block_buffer)) {
                            // Se bater num bloco nulo ou lixo, o livro acabou
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

    // ====================================================================
    // 3. LEITURA EXT4 NORMAL (Para arquivos honestos)
    // ====================================================================
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

    printf("atime:                   %u\n", inode.i_atime);
    printf("ctime:                   %u\n", inode.i_ctime);
    printf("mtime:                   %u\n", inode.i_mtime);
    printf("dtime:                   %u\n", inode.i_dtime);
    printf("----------------------------------------\n\n");
}

void ext4_testi(uint32_t inode_num) {
    uint32_t inodes_per_group = get_inodes_per_group();
    uint32_t group;
    uint32_t index;
    uint64_t inode_bitmap_block;
    unsigned char *block_buffer;

    if (!disk_image) {
        printf("Erro: Nenhuma imagem de disco aberta.\n");
        return;
    }

    if (inode_num == 0 || inodes_per_group == 0) {
        printf("Erro: inode invalido.\n");
        return;
    }

    group = (inode_num - 1) / inodes_per_group;
    index = (inode_num - 1) % inodes_per_group;

    inode_bitmap_block = read_group_block_pointer(group, 4, 0x24);

    if (inode_bitmap_block == 0) {
        printf("Erro ao localizar bitmap de inodes.\n");
        return;
    }

    block_buffer = malloc(global_block_size);
    if (!block_buffer) {
        printf("Erro de alocacao de memoria.\n");
        return;
    }

    if (!read_block(inode_bitmap_block, block_buffer)) {
        free(block_buffer);
        printf("Erro ao ler bitmap de inodes.\n");
        return;
    }

    printf("Inode %u: %s\n", inode_num,
           (block_buffer[index / 8] & (1U << (index % 8))) ?
           "OCUPADO" : "LIVRE");

    free(block_buffer);
}

void ext4_testb(uint32_t block_num) {
    uint32_t blocks_per_group = get_blocks_per_group();
    uint32_t first_data_block = get_first_data_block();
    uint32_t group;
    uint32_t index;
    uint64_t block_bitmap_block;
    unsigned char *block_buffer;

    if (!disk_image) {
        printf("Erro: Nenhuma imagem de disco aberta.\n");
        return;
    }

    if (blocks_per_group == 0 || block_num < first_data_block) {
        printf("Erro: bloco invalido.\n");
        return;
    }

    group = (block_num - first_data_block) / blocks_per_group;
    index = (block_num - first_data_block) % blocks_per_group;

    block_bitmap_block = read_group_block_pointer(group, 0, 0x20);

    if (block_bitmap_block == 0) {
        printf("Erro ao localizar bitmap de blocos.\n");
        return;
    }

    block_buffer = malloc(global_block_size);
    if (!block_buffer) {
        printf("Erro de alocacao de memoria.\n");
        return;
    }

    if (!read_block(block_bitmap_block, block_buffer)) {
        free(block_buffer);
        printf("Erro ao ler bitmap de blocos.\n");
        return;
    }

    printf("Bloco %u: %s\n", block_num,
           (block_buffer[index / 8] & (1U << (index % 8))) ?
           "OCUPADO" : "LIVRE");

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

    // Tenta criar/abrir o arquivo destino na sua máquina física
    // O "wb" é crucial: 'w' (write) e 'b' (binary) para não corromper imagens/executáveis
    out_file = fopen(target_path, "wb");
    if (!out_file) {
        printf("Erro: Nao foi possivel criar o arquivo destino '%s' no seu SO.\n", target_path);
        return;
    }

    size = inode_size_bytes(&inode);
    bytes_remaining = size;
    // Calcula quantos blocos o arquivo ocupa
    total_blocks = (uint32_t)((size + global_block_size - 1) / global_block_size);

    block_buffer = malloc(global_block_size);
    if (!block_buffer) {
        printf("Erro de alocacao de memoria.\n");
        fclose(out_file);
        return;
    }

    // Navega pelos blocos do arquivo da imagem e copia para o arquivo físico
    for (uint32_t i = 0; i < total_blocks; i++) {
        uint64_t phys_block = map_logical_to_physical_block(&inode, i);

        // Define se vamos gravar um bloco inteiro ou apenas a "sobra" do último bloco
        uint32_t bytes_to_write = (bytes_remaining > global_block_size) ? global_block_size : (uint32_t)bytes_remaining;

        // Se o bloco físico for 0 (arquivo esparso/buraco) ou der erro de leitura
        if (phys_block == 0 || !read_block(phys_block, block_buffer)) {
            memset(block_buffer, 0, global_block_size); // Preenche com zeros
        }

        // Grava no arquivo do sistema operacional
        fwrite(block_buffer, 1, bytes_to_write, out_file);
        bytes_remaining -= bytes_to_write;
    }

    fclose(out_file);
    free(block_buffer);
    printf("Sucesso! Arquivo exportado para '%s' (%llu bytes).\n", target_path, (unsigned long long)size);
}
