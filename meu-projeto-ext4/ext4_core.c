#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ext4_structures.c" // Inclui diretamente o arquivo de structs

#define EXT4_EXTENTS_FL 0x00080000

// Variável global para manter o arquivo de imagem aberto (simplificação)
FILE *disk_image = NULL;
uint32_t global_block_size = 1024; // Padrão inicial, será atualizado no init


// Função real e dinâmica para buscar e ler um Inode no EXT4
int read_inode(uint32_t inode_num, struct ext4_inode *inode) {
    if (!disk_image) return 0;

    // 1. Buscando valores críticos diretamente dos offsets do Superbloco
    // O Superbloco começa no byte 1024.
    uint32_t inodes_per_group;
    uint16_t inode_size;
    uint16_t desc_size;

    // Offset 40 do SB: s_inodes_per_group
    fseek(disk_image, 1024 + 40, SEEK_SET);
    fread(&inodes_per_group, sizeof(uint32_t), 1, disk_image);

    // Offset 88 do SB: s_inode_size
    fseek(disk_image, 1024 + 88, SEEK_SET);
    fread(&inode_size, sizeof(uint16_t), 1, disk_image);

    // Offset 254 do SB: s_desc_size (Tamanho do Group Descriptor, o Linux novo usa 64)
    fseek(disk_image, 1024 + 254, SEEK_SET);
    fread(&desc_size, sizeof(uint16_t), 1, disk_image);
    
    // Se a feature 64bit não estiver ativa, o desc_size lido pode vir zerado/lixo. O padrão clássico é 32 bytes.
    if (desc_size < 32 || desc_size > 1024) {
        desc_size = 32; 
    }

    // 2. Calcular o grupo e o índice do inode
    uint32_t group = (inode_num - 1) / inodes_per_group;
    uint32_t index = (inode_num - 1) % inodes_per_group;

    // 3. Localizar a tabela de Inodes do Grupo (Block Group Descriptor Table)
    uint64_t bgdt_offset = (global_block_size == 1024) ? 2048 : global_block_size;
    
    // 4. Ler o ponteiro da Tabela de Inodes (fica nos bytes 8 a 11 do Group Descriptor)
    uint32_t inode_table_block = 0;
    uint64_t descriptor_offset = bgdt_offset + (group * desc_size); 
    
    fseek(disk_image, descriptor_offset + 8, SEEK_SET);
    if (fread(&inode_table_block, sizeof(uint32_t), 1, disk_image) != 1) {
        return 0;
    }

    // 5. Calcular o byte exato do Inode e ler para a struct
    uint64_t inode_offset = ((uint64_t)inode_table_block * global_block_size) + (index * inode_size);

    fseek(disk_image, inode_offset, SEEK_SET);
    if (fread(inode, sizeof(struct ext4_inode), 1, disk_image) != 1) {
        return 0;
    }

    return 1;
}

// Função que converte um bloco lógico do arquivo em um bloco físico no disco
uint64_t map_logical_to_physical_block(struct ext4_inode *inode, uint64_t logical_block) {
    // 1. O cabeçalho de extents fica nos primeiros 12 bytes do i_block
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;

    // Se o magic number não for 0xF30A, o inode não está usando extents de forma correta
    if (eh->eh_magic != 0xF30A) {
        return 0; 
    }

    // Como o enunciado garante diretórios/arquivos pequenos, a profundidade (eh_depth) será 0.
    // Isso significa que os extents logo após o cabeçalho já são folhas diretas (dados).
    if (eh->eh_depth == 0) {
        // Os extents de folha começam logo após o cabeçalho (offset de 12 bytes dentro do i_block)
        // O i_block mapeia a memória continuamente. Podemos usar um ponteiro para varrer as entradas.
        struct ext4_extent *ext = (struct ext4_extent *)((char *)inode->i_block + sizeof(struct ext4_extent_header));

        // Percorre todos os extents válidos deste inode
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            // Verifica se o bloco lógico que queremos está dentro da faixa coberta por este extent
            if (logical_block >= ext[i].ee_block && logical_block < (ext[i].ee_block + ext[i].ee_len)) {
                
                // Calcula o deslocamento dentro deste extent
                uint32_t offset_inside_extent = logical_block - ext[i].ee_block;
                
                // Monta o número do bloco físico (combinando os bits superiores e inferiores)
                uint64_t physical_block = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
                
                return physical_block + offset_inside_extent;
            }
        }
    } else {
        printf("[Erro] Árvore de extents com profundidade > 0 não implementada (simplificação).\n");
    }

    return 0; // Bloco não encontrado (fim do arquivo ou buraco)
}
// Inicializa e detecta o tamanho do bloco (1KiB, 2KiB ou 4KiB)
int ext4_init(const char *image_path) {
    disk_image = fopen(image_path, "rb");
    if (!disk_image) {
        return 0; // Falha ao abrir
    }

    struct ext4_super_block sb;
    // O Superbloco fica sempre no offset de 1024 bytes do disco
    fseek(disk_image, 1024, SEEK_SET);
    fread(&sb, sizeof(struct ext4_super_block), 1, disk_image);

    // Calcula o tamanho do bloco baseado no s_log_block_size
    // 0 = 1024 (1KiB), 1 = 2048 (2KiB), 2 = 4096 (4KiB)
    global_block_size = 1024 << sb.s_log_block_size;

    printf("[EXT4 CORE] Sistema carregado com sucesso!\n");
    printf("[EXT4 CORE] Tamanho de Bloco Detectado: %u bytes (%u KiB)\n\n", 
           global_block_size, global_block_size / 1024);

    return 1;
}

// Lê um bloco lógico qualquer do disco
int read_block(uint64_t block_num, void *buffer) {
    if (!disk_image) return 0;
    
    // Calcula a posição real em bytes baseado no tamanho do bloco
    uint64_t offset = block_num * global_block_size;
    fseek(disk_image, offset, SEEK_SET);
    fread(buffer, global_block_size, 1, disk_image);
    return 1;
}


void ext4_readdir(uint32_t dir_inode_num) {
    struct ext4_inode inode;
    if (!read_inode(dir_inode_num, &inode)) {
        printf("Erro ao ler o inode do diretório.\n");
        return;
    }

    // Mapeia o primeiro bloco (bloco 0) do diretório
    uint64_t phys_block = map_logical_to_physical_block(&inode, 0);
    if (phys_block == 0) {
        printf("Diretório vazio ou inválido.\n");
        return;
    }

    char *block_buffer = malloc(global_block_size);
    read_block(phys_block, block_buffer);

    printf("\n%-10s %-8s %s\n", "Inode", "Tipo", "Nome");
    printf("----------------------------------------\n");

    uint32_t offset = 0;
    // Varre o bloco lendo as entradas de diretório consecutivas
    while (offset < global_block_size) {
        struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)(block_buffer + offset);

        // Se o número do inode for 0, essa entrada foi deletada ou está vazia
        if (entry->inode != 0) {
            // Garante o fechamento da string do nome para exibição segura
            char name_buffer[256];
            strncpy(name_buffer, entry->name, entry->name_len);
            name_buffer[entry->name_len] = '\0';

            char *type_str = (entry->file_type == 2) ? "DIR" : "FILE";

            printf("%-10u %-8s %s\n", entry->inode, type_str, name_buffer);
        }

        // Importante: rec_len diz quantos bytes pular para chegar na PRÓXIMA entrada
        if (entry->rec_len == 0) break; // Evita loop infinito se o bloco estiver corrompido
        offset += entry->rec_len;
    }
    printf("\n");

    free(block_buffer);
}

uint32_t ext4_lookup(uint32_t parent_inode_num, const char *name) {
    struct ext4_inode parent_inode;
    if (!read_inode(parent_inode_num, &parent_inode)) return 0;

    // Calcula quantos blocos o diretório ocupa
    uint32_t total_blocks = (parent_inode.i_size_lo + global_block_size - 1) / global_block_size;
    char *block_buffer = malloc(global_block_size);
    if (!block_buffer) return 0;

    uint32_t found_inode = 0;

    // Varre TODOS os blocos do diretório
    for (uint32_t blk = 0; blk < total_blocks; blk++) {
        uint64_t phys_block = map_logical_to_physical_block(&parent_inode, blk);
        if (phys_block == 0) continue; // Pula buracos/erros no diretório

        read_block(phys_block, block_buffer);
        uint32_t offset = 0;

        while (offset < global_block_size) {
            struct ext4_dir_entry_2 *entry = (struct ext4_dir_entry_2 *)(block_buffer + offset);
            
            if (entry->rec_len == 0) break; // Previne loop infinito

            if (entry->inode != 0) {
                char name_buffer[256];
                strncpy(name_buffer, entry->name, entry->name_len);
                name_buffer[entry->name_len] = '\0';

                if (strcmp(name_buffer, name) == 0) {
                    found_inode = entry->inode;
                    break; // Achou neste bloco!
                }
            }
            offset += entry->rec_len;
        }
        if (found_inode != 0) break; // Sai do loop de blocos se já achou
    }

    free(block_buffer);
    return found_inode;
}

void ext4_cat(uint32_t file_inode_num) {
    if (file_inode_num == 0) {
        printf("Arquivo não encontrado.\n");
        return;
    }

    struct ext4_inode inode;
    if (!read_inode(file_inode_num, &inode)) {
        printf("Erro ao ler o inode do arquivo.\n");
        return;
    }

    // Calcula quantos blocos o arquivo ocupa no total
    // i_size_lo guarda o tamanho em bytes. Dividindo pelo tamanho do bloco, temos a qtde de blocos.
    uint32_t total_blocks = (inode.i_size_lo + global_block_size - 1) / global_block_size;

    // Buffer dinâmico para ler um bloco de cada vez
    char *block_buffer = malloc(global_block_size);
    
    printf("\n--- CONTEÚDO DO ARQUIVO ---\n");
    
    uint32_t bytes_remaining = inode.i_size_lo;

    for (uint32_t i = 0; i < total_blocks; i++) {
        uint64_t phys_block = map_logical_to_physical_block(&inode, i);
        
       if (phys_block == 0) {
            // É um buraco no arquivo (sparse file)! Preenche com zeros.
            memset(block_buffer, 0, global_block_size);
        } else {
            read_block(phys_block, block_buffer);
        }

        // Determina quantos bytes printar deste bloco (o último bloco pode não estar cheio)
        uint32_t bytes_to_print = (bytes_remaining > global_block_size) ? global_block_size : bytes_remaining;
        
        // Imprime os caracteres na tela
        for (uint32_t j = 0; j < bytes_to_print; j++) {
            putchar(block_buffer[j]);
        }

        bytes_remaining -= bytes_to_print;
    }
    
    printf("\n---------------------------\n\n");
    free(block_buffer);
}

// Função que lê os dados do Superbloco e exibe informações do sistema de arquivos
void ext4_show_info() {
    if (!disk_image) {
        printf("Erro: Nenhuma imagem de disco aberta.\n");
        return;
    }

    struct ext4_super_block sb;
    
    // O Superbloco está sempre no offset 1024
    fseek(disk_image, 1024, SEEK_SET);
    if (fread(&sb, sizeof(struct ext4_super_block), 1, disk_image) != 1) {
        printf("Erro ao ler o Superbloco do disco.\n");
        return;
    }

    printf("\n--- METADADOS DO SISTEMA DE ARQUIVOS (EXT4) ---\n");
    printf("Volume Status:          Montado via interpretador\n");
    printf("Tamanho do Bloco:       %u bytes (%u KiB)\n", global_block_size, global_block_size / 1024);
    printf("Total de Inodes:        %u\n", sb.s_inodes_count);
    printf("Inodes Livres:          %u\n", sb.s_free_inodes_count_lo);
    printf("Total de Blocos:        %u\n", sb.s_blocks_count_lo);
    printf("Blocos Livres:          %u\n", sb.s_free_blocks_count_lo);
    printf("Primeiro Bloco de Dados:%u\n", sb.s_first_data_block);
    printf("-----------------------------------------------\n\n");
}

// Função que exibe os atributos detalhados de um Inode específico
void ext4_attr(uint32_t inode_num) {
    if (!disk_image) {
        printf("Erro: Nenhuma imagem de disco aberta.\n");
        return;
    }

    if (inode_num == 0) {
        printf("Erro: Inode inválido ou arquivo não encontrado.\n");
        return;
    }

    struct ext4_inode inode;
    
    if (!read_inode(inode_num, &inode)) {
        printf("Erro ao ler o Inode %u do disco.\n", inode_num);
        return;
    }
    
    printf("\n--- ATRIBUTOS DO INODE: %u ---\n", inode_num);
    
    // 1. Tipo de Arquivo
    printf("Tipo de Arquivo:        ");
    if ((inode.i_mode & 0xF000) == 0x8000) printf("Arquivo Regular\n");
    else if ((inode.i_mode & 0xF000) == 0x4000) printf("Diretório\n");
    else printf("Outro/Desconhecido (Modo: 0x%X)\n", inode.i_mode);

    // 2. Permissões (Formato rwxrwxrwx)
    printf("Permissões:             ");
    printf((inode.i_mode & 0x0100) ? "r" : "-");
    printf((inode.i_mode & 0x0080) ? "w" : "-");
    printf((inode.i_mode & 0x0040) ? "x" : "-");
    printf((inode.i_mode & 0x0020) ? "r" : "-");
    printf((inode.i_mode & 0x0010) ? "w" : "-");
    printf((inode.i_mode & 0x0008) ? "x" : "-");
    printf((inode.i_mode & 0x0004) ? "r" : "-");
    printf((inode.i_mode & 0x0002) ? "w" : "-");
    printf((inode.i_mode & 0x0001) ? "x" : "-");
    printf(" (0o%o)\n", inode.i_mode & 0x0FFF);

    // 3. Proprietários e Links
    printf("UID do Dono:            %u\n", inode.i_uid);
    printf("GID do Grupo:           %u\n", inode.i_gid);
    printf("Contador de Links:      %u\n", inode.i_links_count);

    // 4. Tamanho e Blocos
    printf("Tamanho do Arquivo:     %u bytes\n", inode.i_size_lo);
    // i_blocks_lo conta em blocos de 512 bytes fixos no Linux
    printf("Blocos de 512B Alocados:%u\n", inode.i_blocks_lo); 

    // 5. Verificação de Extents
    printf("Usa Extents?            ");
    if (inode.i_flags & EXT4_EXTENTS_FL) {
        printf("Sim (Obrigatório no projeto)\n");
        
        // Mapeia o cabeçalho de extents que fica guardado nos primeiros bytes do i_block
        struct ext4_extent_header *eh = (struct ext4_extent_header *)inode.i_block;
        printf("  -> Extents Válidos:   %u\n", eh->eh_entries);
        printf("  -> Profundidade Árvore:%u\n", eh->eh_depth);
    } else {
        printf("Não (Atenção: Seu sistema deveria usar extents!)\n");
    }
    
    // 6. Timestamps Básicos
    printf("Última Modificação (mtime): %u\n", inode.i_mtime);
    printf("----------------------------------------\n\n");
}