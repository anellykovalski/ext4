#include <stdint.h>

// --- Estruturas Simplificadas do EXT4 para o Projeto ---

// Superbloco (Localizado a 1024 bytes do início do disco)
struct ext4_super_block {
    uint32_t s_inodes_count;      // Total de inodes
    uint32_t s_blocks_count_lo;   // Total de blocos
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count_lo;
    uint32_t s_first_data_block;  // Primeiro bloco de dados (0 ou 1)
    uint32_t s_log_block_size;    // Tamanho do bloco: 0 = 1KiB, 1 = 2KiB, 2 = 4KiB
    // ... (o restante dos campos você pode adicionar conforme a necessidade)
};

// Inode do EXT4
struct ext4_inode {
    uint16_t i_mode;        // Tipo de arquivo e permissões
    uint16_t i_uid;         // ID do dono
    uint32_t i_size_lo;     // Tamanho em bytes (32 bits inferiores)
    uint32_t i_atime;       // Tempo de acesso
    uint32_t i_ctime;       // Tempo de criação
    uint32_t i_mtime;       // Tempo de modificação
    uint32_t i_dtime;       // Tempo de deleção
    uint16_t i_gid;         // ID do grupo
    uint16_t i_links_count; // Contador de links
    uint32_t i_blocks_lo;   // Contador de blocos de 512 bytes alocados
    uint32_t i_flags;       // Flags do Inode (indica se usa extents)
    uint32_t i_block[15];   // Ponteiros de dados / Árvore de Extents (60 bytes)
    uint32_t osd1;
    // ...
};

// Cabeçalho do Extent (Fica nos primeiros 12 bytes do i_block se a flag EXTENTS estiver ativa)
struct ext4_extent_header {
    uint16_t eh_magic;      // Magic number (Sempre 0xF30A)
    uint16_t eh_entries;    // Número de entradas válidas
    uint16_t eh_max;        // Capacidade máxima de entradas
    uint16_t eh_depth;      // Profundidade da árvore (0 se for folha direta)
    uint32_t eh_generation;
};

// Extent de folha (Mapeia blocos lógicos diretamente para blocos físicos)
struct ext4_extent {
    uint32_t ee_block;      // Primeiro bloco lógico do arquivo cobertor por este extent
    uint16_t ee_len;        // Quantidade de blocos contíguos
    uint16_t ee_start_hi;   // 16 bits superiores do bloco físico
    uint32_t ee_start_lo;   // 32 bits inferiores do bloco físico
};

// Entrada de Diretório Linear (Sintaxe Básica)
struct ext4_dir_entry_2 {
    uint32_t inode;         // Número do Inode do arquivo/pasta
    uint16_t rec_len;       // Tamanho total desta entrada de diretório
    uint8_t  name_len;      // Tamanho do nome do arquivo
    uint8_t  file_type;     // Tipo do arquivo (1 = arquivo, 2 = diretório, etc.)
    char     name[255];     // Nome do arquivo (tamanho variável)
};