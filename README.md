# 🐧 EXT4 File System Interpreter

> **Projeto Acadêmico** desenvolvido para a disciplina de **BCC5002 – Sistemas Operacionais** na Universidade Tecnológica Federal do Paraná (UTFPR).

Este projeto é um interpretador interativo (Shell) de baixo nível construído em **C** puro para o sistema de arquivos **EXT4**. Ele permite a leitura, navegação, abstração de *extents* e modificação direta de uma imagem de disco (`.img`), sem depender de chamadas de sistema operacionais hospedeiras (syscalls).

---

## ✨ Funcionalidades Implementadas

O shell suporta comandos robustos divididos nas seguintes categorias de atuação:

📁 **Navegação e Inspeção**
- `ls`, `cd`, `pwd` — Navegação básica de diretórios.
- `cat` — Leitura de arquivos regulares e modo de recuperação (File Carving).
- `info` — Exibição de metadados gerais (Superbloco).
- `attr` — Exibe permissões detalhadas, UUIDs, GIDs e tamanho em bytes de Inodes.

🛠️ **Modificação e Alocação Física**
- `touch`, `mkdir` — Criação de arquivos e pastas reais via *Inode Bitmap* e *Block Bitmap*.
- `rm`, `rmdir` — Remoção segura desvinculando arquivos no disco.
- `rename` — Renomeia itens remanejando a alocação flexível (`rec_len`).

🔍 **Diagnóstico de Baixo Nível**
- `testi`, `testb` — Auditoria dos bits de estado livre/ocupado para Inodes e Blocos Físicos.
- `export` — Extrai um arquivo da imagem EXT4 para o sistema operacional físico.

---

## 🚀 Como Executar no VS Code

Você pode compilar e rodar o projeto rapidamente através do terminal integrado do Visual Studio Code. 

1. Abra o terminal do VS Code (`Ctrl` + `'` ou através do menu superior `Terminal > New Terminal`).
2. Execute o comando de compilação do GCC:
   ```bash
   gcc main.c -o meu_shell
