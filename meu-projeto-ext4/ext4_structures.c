#ifndef EXT4_STRUCTURES_H
#define EXT4_STRUCTURES_H

#include <stdint.h>

// Garante que o compilador não adicione bytes de preenchimento (padding) 
// nas estruturas, mantendo a compatibilidade exata com o disco binário.
#pragma pack(push, 1)

/* =========================================================================
 * 1. SUPERBLOCO (Apenas os campos essenciais para o nosso projeto)
 * O Superbloco real tem 1024 bytes, aqui mapeamos até o offset necessário.
 * ========================================================================= */
struct ext4_super_block {
    uint32_t s_inodes_count;         // 0x00: Total de Inodes
    uint32_t s_blocks_count_lo;      // 0x04: Total de Blocos
    uint32_t s_rsv_blocks_count_lo;  // 0x08: Blocos reservados
    uint32_t s_free_blocks_count_lo; // 0x0C: Blocos livres
    uint32_t s_free_inodes_count_lo; // 0x10: Inodes livres
    uint32_t s_first_data_block;     // 0x14: Primeiro bloco de dados
    uint32_t s_log_block_size;       // 0x18: Tamanho do bloco (1024 << log)
    uint32_t s_log_cluster_size;     // 0x1C
    uint32_t s_blocks_per_group;     // 0x20: Blocos por grupo
    uint32_t s_clusters_per_group;   // 0x24
    uint32_t s_inodes_per_group;     // 0x28: Inodes por grupo
    uint32_t s_mtime;                // 0x2C
    uint32_t s_wtime;                // 0x30
    uint16_t s_mnt_count;            // 0x34
    uint16_t s_max_mnt_count;        // 0x36
    uint16_t s_magic;                // 0x38: Assinatura mágica (0xEF53)
};

/* =========================================================================
 * 2. ESTRUTURA DO INODE (Mapeamento dos 128 bytes clássicos)
 * ========================================================================= */
struct ext4_inode {
    uint16_t i_mode;        // 0x00: Modo de acesso e tipo de arquivo
    uint16_t i_uid;         // 0x02: UID do dono
    uint32_t i_size_lo;     // 0x04: Tamanho do arquivo em bytes
    uint32_t i_atime;       // 0x08: Último acesso
    uint32_t i_ctime;       // 0x0C: Criação/Alteração de inode
    uint32_t i_mtime;       // 0x10: Modificação de dados
    uint32_t i_dtime;       // 0x14: Deleção
    uint16_t i_gid;         // 0x18: GID do grupo
    uint16_t i_links_count; // 0x1A: Contador de hard links
    uint32_t i_blocks_lo;   // 0x1C: Blocos de 512B alocados
    uint32_t i_flags;       // 0x20: Flags (ex: 0x80000 para Extents)
    union {
        struct {
            uint32_t l_i_version;
        } linux1;
        uint32_t h_i_translator;
    } osd1;                 // 0x24: Específico do SO
    uint32_t i_block[15];   // 0x28: Ponteiros diretos ou Árvore de Extents (60 bytes)
    uint32_t i_generation;  // 0x64: Versão do arquivo (NFS)
    uint32_t i_file_acl_lo; // 0x68: ACL
    uint32_t i_size_hi;     // 0x6C: 32 bits altos do tamanho
    uint32_t i_obso_faddr;  // 0x70
    uint16_t i_blocks_hi;   // 0x74
    uint16_t i_file_acl_hi; // 0x76
    uint16_t i_uid_high;    // 0x78
    uint16_t i_gid_high;    // 0x7A
    uint16_t i_checksum_lo; // 0x7C
    uint16_t i_reserved;    // 0x7E
};

/* =========================================================================
 * 3. ENTRADA DE DIRETÓRIO (Formato ext4_dir_entry_2)
 * ========================================================================= */
struct ext4_dir_entry_2 {
    uint32_t inode;     // Número do Inode apontado
    uint16_t rec_len;   // Tamanho total desta entrada no bloco
    uint8_t  name_len;  // Tamanho do nome do arquivo
    uint8_t  file_type; // Tipo (1=Arquivo, 2=Dir, 7=Link, etc)
    char     name[255]; // Vetor com o nome (tamanho dinâmico no disco)
};

/* =========================================================================
 * 4. ÁRVORE DE EXTENTS (Para arquivos modernos no EXT4)
 * ========================================================================= */

// Cabeçalho dos Extents (fica nos primeiros 12 bytes de i_block)
struct ext4_extent_header {
    uint16_t eh_magic;      // Assinatura (0xF30A)
    uint16_t eh_entries;    // Número de entradas válidas no nó
    uint16_t eh_max;        // Capacidade máxima de entradas
    uint16_t eh_depth;      // Profundidade (0 = folhas com dados reais)
    uint32_t eh_generation; // Geração da árvore
};

// Índice intermediário (quando eh_depth > 0)
struct ext4_extent_idx {
    uint32_t ei_block;   // Bloco lógico inicial coberto por este índice
    uint32_t ei_leaf_lo; // 32 bits baixos do bloco físico apontado
    uint16_t ei_leaf_hi; // 16 bits altos do bloco físico
    uint16_t ei_unused;
};

// Folha de dados real (quando eh_depth == 0)
struct ext4_extent {
    uint32_t ee_block;    // Primeiro bloco lógico coberto
    uint16_t ee_len;      // Número de blocos contíguos cobertos
    uint16_t ee_start_hi; // 16 bits altos do bloco físico
    uint32_t ee_start_lo; // 32 bits baixos do bloco físico no disco
};

#pragma pack(pop)

#endif // EXT4_STRUCTURES_H