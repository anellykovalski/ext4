# Nome do executável final
TARGET = meu_shell

# Compilador e flags
CC = gcc
CFLAGS = -Wall -Wextra -O2

# Arquivos fonte
SRCS = main.c ext4_core.c ext4_structures.c
OBJS = main.o

# Regra padrão executada ao rodar apenas 'make'
all: $(TARGET)

# Como compilar o executável
$(TARGET): $(SRCS)
	@echo "🐧 Compilando o interpretador EXT4..."
	$(CC) $(CFLAGS) main.c -o $(TARGET)
	@echo "✅ Compilação concluída! Executável '$(TARGET)' gerado."

# Regra para limpar os arquivos gerados (make clean)
clean:
	@echo "🧹 Limpando os arquivos gerados..."
	rm -f $(TARGET) *.o
	@echo "✨ Tudo limpo!"

# Regra para rodar rapidamente com a imagem de 1K (make run)
run: $(TARGET)
	@echo "🚀 Iniciando o Shell..."
	./$(TARGET) images_ext4/myext4image1k.img