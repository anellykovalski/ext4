#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ext4_structures.c" // Inclui diretamente o arquivo de structs

// Variável global para manter o arquivo de imagem aberto (simplificação)
FILE *disk_image = NULL;
uint32_t global_block_size = 1024; // Padrão inicial, será atualizado no init

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

// Busca o Inode na tabela de Inodes (Você implementará a matemática de achar o BG do Inode aqui)
int read_inode(uint32_t inode_num, struct ext4_inode *inode) {
    // TODO: Implementar busca do inode com base nos Block Groups
    return 1;
}

// Simulação de busca/leitura do conteúdo de um diretório
void ext4_readdir(uint32_t dir_inode_num) {
    printf("[Simulação] Listando arquivos no Inode %u\n", dir_inode_num);
    // TODO: Ler o Inode -> Mapear Extents -> Ler blocos de dados -> Printar struct ext4_dir_entry_2
    
    // Mock de exemplo para testar o Shell visualmente:
    printf("  .             (inode %u)\n", dir_inode_num);
    printf("  ..            (inode 2)\n");
    printf("  documento.txt (inode 12)\n");
    printf("  fotos_pasta   (inode 15)\n");
}

// Simulação para buscar um arquivo por nome dentro de um diretório pai
uint32_t ext4_lookup(uint32_t parent_inode_num, const char *name) {
    // TODO: Varre as entradas do diretório pai procurando pelo 'name'
    
    // Mock para testar a navegação do Shell:
    if (strcmp(name, ".") == 0) return parent_inode_num;
    if (strcmp(name, "..") == 0) return 2; // Volta para a raiz fictícia
    if (strcmp(name, "fotos_pasta") == 0) return 15;
    if (strcmp(name, "documento.txt") == 0) return 12;
    
    return 0; // 0 significa não encontrado
}

// Simulação de leitura de arquivo (cat)
void ext4_cat(uint32_t file_inode_num) {
    if (file_inode_num == 0) {
        printf("Arquivo não encontrado.\n");
        return;
    }
    printf("[Simulação Cat] Lendo blocos via árvore de Extents do Inode %u:\n", file_inode_num);
    printf("------------------------------------------------------------\n");
    printf("Conteudo ficticio do arquivo lido com sucesso usando Extents!\n");
    printf("------------------------------------------------------------\n");
}