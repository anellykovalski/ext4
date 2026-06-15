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
    // Remove a quebra de linha '\n' capturada pelo fgets
    line[strcspn(line, "\n")] = 0;

    if (strlen(line) == 0) return; // Se for linha vazia, apenas ignora

    // Divide a entrada em comando e argumentos usando espaço como delimitador
    char *args[MAX_ARGS];
    int arg_count = 0;
    
    char *token = strtok(line, " ");
    while (token != NULL && arg_count < MAX_ARGS) {
        args[arg_count++] = token;
        token = strtok(NULL, " ");
    }

    char *cmd = args[0];

    // --- Tratamento dos Comandos ---
    
    if (strcmp(cmd, "help") == 0) {
        printf("Comandos Suportados (Sintaxe Simplificada):\n");
        printf("  ls          - Lista arquivos e subdiretórios da pasta atual\n");
        printf("  cd <nome>   - Entra em um diretório (Ex: cd fotos_pasta ou cd ..)\n");
        printf("  cat <nome>  - Mostra o conteúdo de um arquivo (Ex: cat documento.txt)\n");
        printf("  pwd         - Exibe o caminho do diretório atual\n");
        printf("  clear       - Limpa o terminal\n");
        printf("  exit        - Fecha o interpretador\n");
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
            
            // Atualiza a string do caminho na tela de forma simplificada
            if (strcmp(args[1], "..") == 0) {
                strcpy(state->current_path, "/"); // Simplificação ao voltar
            } else if (strcmp(args[1], ".") != 0) {
                if (strcmp(state->current_path, "/") != 0) {
                    strcat(state->current_path, "/");
                }
                strcat(state->current_path, args[1]);
            }
        }
    } 
    else if (strcmp(cmd, "cat") == 0) {
        if (arg_count < 2) {
            printf("Uso: cat <nome_do_arquivo>\n");
            return;
        }
        uint32_t file_inode = ext4_lookup(state->current_inode, args[1]);
        ext4_cat(file_inode);
    } 
    else if (strcmp(cmd, "pwd") == 0) {
        printf("%s\n", state->current_path);
    } 
    else if (strcmp(cmd, "clear") == 0) {
        printf("\033[H\033[J"); // Limpa a tela usando código de escape ANSI (Nativo)
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
    // Validação de argumento inicial
    if (argc < 2) {
        fprintf(stderr, "Erro de Execução!\nUso correto: %s <caminho_da_imagem_ext4>\n", argv[0]);
        return 1;
    }

    // Inicializa o leitor EXT4 apontando para a imagem informada
    if (!ext4_init(argv[1])) {
        fprintf(stderr, "Erro Crítico: Não foi possível abrir o arquivo de imagem '%s'.\n", argv[1]);
        return 1;
    }

    // Estado Inicial do Shell (A raiz de qualquer sistema EXT4 é sempre o Inode 2)
    ShellState state;
    state.current_inode = 2;
    strcpy(state.current_path, "/");

    char input_buffer[MAX_INPUT_SIZE];

    printf("=========================================\n");
    printf("   INTERPRETADOR EXT4 INTERNO (SHELL)    \n");
    printf("=========================================\n");
    printf("Digite 'help' para listar os comandos disponíveis.\n\n");

    // Loop Principal (REPL: Read, Evaluate, Print, Loop)
    while (1) {
        printf("ext4_user@sistema:%s$ ", state.current_path);
        fflush(stdout);

        // Captura o comando do usuário de forma segura sem estourar o buffer
        if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) {
            break; // Sai se pressionado Ctrl+D (EOF)
        }

        execute_command(input_buffer, &state);
    }

    return 0;
}