#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ext4_core.c" // Puxa todas as funções e estruturas automaticamente

#define MAX_INPUT_SIZE 1024
#define MAX_ARGS 5

// Estado atual do Shell (Diretório que o usuário está navegando)
typedef struct {
    uint32_t current_inode;
    char current_path[512];
} ShellState;

// Processador de Comandos do Interpretador
void execute_command(char *line, ShellState *state) {
    // 1. Remove tanto o \n quanto o \r
    line[strcspn(line, "\r\n")] = 0;
    if (strlen(line) == 0) return; // Se for linha vazia, apenas ignora

    // 2. Usa \r e \n no delimitador do strtok por segurança
    char *args[MAX_ARGS];
    int arg_count = 0;

    char *token = strtok(line, " \r\n");
    while (token != NULL && arg_count < MAX_ARGS) {
        args[arg_count++] = token;
        token = strtok(NULL, " \r\n");
    }

    char *cmd = args[0];

    // --- Tratamento dos Comandos ---
    if (strcmp(cmd, "help") == 0) {
        printf("Comandos Suportados:\n");
        printf("  ls          - Lista arquivos do diretorio atual ou da pasta informada\n");
        printf("  cd <nome>   - Entra em um diretorio\n");
        printf("  cat <nome>  - Mostra o conteudo de um arquivo\n");
        printf("  pwd         - Exibe o caminho do diretorio atual\n");
        printf("  clear       - Limpa o terminal\n");
        printf("  exit        - Fecha o interpretador\n");
        printf("  info        - Informacoes do sistema de arquivos\n");
        printf("  attr <nome> - Exibe atributos de arquivo ou diretorio\n");
        printf("  testi <num> - Verifica se um inode esta livre ou ocupado\n");
        printf("  testb <num> - Verifica se um bloco esta livre ou ocupado\n");
        printf("  export <src> <tgt> - Copia um arquivo da imagem EXT4 para a maquina real\n");
        printf("  touch <nome>- Cria um arquivo regular vazio no diretorio atual\n");
        printf("  mkdir <nome>- Cria um novo diretorio vazio no diretorio atual\n");
        printf("  rm <nome>   - Remove um arquivo regular do diretorio atual\n");
        printf("  rename <file> <newfilename> - Renomeia um arquivo file para newfilename\n");
        printf("  rmdir <dir> - Remove um diretório, se vazio\n");
    } 
    else if (strcmp(cmd, "ls") == 0) {
        ext4_readdir(state->current_inode);
    }
    else if (strcmp(cmd, "cd") == 0) {
        if (arg_count < 2) {
            printf("Uso: cd <nome_do_diretorio>\n");
            return;
        }

        uint32_t target_inode = ext4_lookup(state->current_inode, args[1]);
        if (target_inode == 0) {
            printf("Erro: O diretório '%s' não existe.\n", args[1]);
        } else {
            state->current_inode = target_inode;
            if (strcmp(args[1], "..") == 0) {
                // Lógica simples para voltar (em um shell real atualizaríamos a string com precisão)
                strcpy(state->current_path, "/");
            } else if (strcmp(args[1], ".") != 0) {
                if (strcmp(state->current_path, "/") != 0) {
                    strcat(state->current_path, "/");
                }
                strcat(state->current_path, args[1]);
            }
        }
    }
    else if (strcmp(cmd, "touch") == 0) {
        if (arg_count < 2) {
            printf("Uso: touch <nome_do_arquivo>\n");
            return;
        }
        ext4_touch(state->current_inode, args[1]);
    }
    else if (strcmp(cmd, "mkdir") == 0) {
        if (arg_count < 2) {
            printf("Uso: mkdir <nome_do_diretorio>\n");
            return;
        }
        ext4_mkdir(state->current_inode, args[1]);
    }
    else if (strcmp(cmd, "rm") == 0) {
        if (arg_count < 2) {
            printf("Uso: rm <nome_do_arquivo>\n");
            return;
        }
        ext4_rm(state->current_inode, args[1]);
    }
    else if (strcmp(cmd, "attr") == 0) {
        if (arg_count < 2) {
            printf("Uso: attr <nome_do_arquivo_ou_diretorio>\n");
            return;
        }

        // Reconstrói o nome completo unindo args[1], args[2], etc. com espaços
        char full_name[256] = "";
        for (int i = 1; i < arg_count; i++) {
            strncat(full_name, args[i], sizeof(full_name) - strlen(full_name) - 1);
            if (i < arg_count - 1) {
                strncat(full_name, " ", sizeof(full_name) - strlen(full_name) - 1);
            }
        }

        uint32_t target_inode = ext4_lookup(state->current_inode, full_name);
        if (target_inode == 0) {
            printf("Erro: Arquivo ou diretório '%s' não encontrado.\n", full_name);
        } else {
            ext4_attr(target_inode);
        }
    }
    else if (strcmp(cmd, "rename") == 0){
        if (arg_count < 3) {
            printf("Uso: rename <nome_antigo> <nome_novo>\n");
            return;
        }
        ext4_rename(state->current_inode, args[1], args[2]);
    }
    else if (strcmp(cmd, "rmdir") == 0) {
        if (arg_count < 2) {
            printf("Uso: rmdir <nome_do_diretorio>\n");
            return;
        }
        ext4_rmdir(state->current_inode, args[1]);
    }
    else if (strcmp(cmd, "cat") == 0) {
        if (arg_count < 2) {
            printf("Uso: cat <nome_do_arquivo>\n");
            return;
        }
        uint32_t file_inode = ext4_lookup(state->current_inode, args[1]);
        if (file_inode == 0) {
            printf("Erro: Arquivo '%s' não encontrado.\n", args[1]);
        } else {
            ext4_cat(file_inode);
        }
    } 
    else if (strcmp(cmd, "export") == 0) {
        if (arg_count < 3) {
            printf("Uso: export <arquivo_origem_na_imagem> <caminho_destino_no_seu_pc>\n");
            return;
        }

        uint32_t source_inode = ext4_lookup(state->current_inode, args[1]);

        if (source_inode == 0) {
            printf("Erro: Arquivo '%s' nao encontrado no diretorio atual da imagem.\n", args[1]);
        } else {
            ext4_export(source_inode, args[2]);
        }
    }
    else if (strcmp(cmd, "info") == 0) {
        ext4_show_info();
    }
    else if (strcmp(cmd, "pwd") == 0) {
        printf("%s\n", state->current_path);
    } 
    else if (strcmp(cmd, "clear") == 0) {
        printf("\033[H\033[J");
    } 
    else if (strcmp(cmd, "testi") == 0) {
        if (arg_count < 2) {
            printf("Uso: testi <numero_do_inode>\n");
            return;
        }
        uint32_t inode_num = (uint32_t)atoi(args[1]);
        ext4_testi(inode_num);
    }
    else if (strcmp(cmd, "testb") == 0) {
        if (arg_count < 2) {
            printf("Uso: testb <numero_do_bloco>\n");
            return;
        }
        uint32_t block_num = (uint32_t)atoi(args[1]);
        ext4_testb(block_num);
    }
    else if (strcmp(cmd, "exit") == 0) {
        printf("Fechando interpretador EXT4. Até mais!\n");
        exit(0);
    } 
    else {
        printf("Comando desconhecido: '%s'. Digite 'help' para comandos válidos.\n", cmd);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Erro de Execução!\nUso correto: %s <caminho_da_imagem_ext4>\n", argv[0]);
        return 1;
    }

    if (!ext4_init(argv[1])) {
        fprintf(stderr, "Erro Crítico: Não foi possível abrir o arquivo de imagem '%s'.\n", argv[1]);
        return 1;
    }

    ShellState state;
    state.current_inode = 2;
    strcpy(state.current_path, "/");

    char input_buffer[MAX_INPUT_SIZE];

    printf("=========================================\n");
    printf("   INTERPRETADOR EXT4 INTERNO (SHELL)    \n");
    printf("=========================================\n");
    printf("Digite 'help' para listar os comandos disponíveis.\n\n");

    while (1) {
        printf("ext4_user@sistema:%s$ ", state.current_path);
        fflush(stdout);

        if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) {
            break;
        }

        execute_command(input_buffer, &state);
    }

    return 0;
}
