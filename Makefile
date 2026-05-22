CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude
LDFLAGS = -lpthread -lssl -lcrypto -lcurl

ENGINE_SRCS = src/main.c \
	src/auth_hook.c \
	src/config.c \
	src/conn_map.c \
	src/engine.c \
	src/logger.c \
	src/scope_map.c \
	src/utils.c

TEST_AUTH = tests/test_auth_hook.c src/auth_hook.c src/logger.c

all: server client

server: $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -o server $(ENGINE_SRCS) $(LDFLAGS)

client: src/client.c src/config.c src/utils.c src/logger.c
	$(CC) $(CFLAGS) -o client \
		src/client.c \
		src/config.c \
		src/utils.c \
		src/logger.c

tests/run_auth: $(TEST_AUTH)
	$(CC) $(CFLAGS) -o $@ $(TEST_AUTH) $(LDFLAGS)

sanitize: $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o server_san \
		$(ENGINE_SRCS) $(LDFLAGS)

clean:
	rm -f server client server_san
	rm -rf tests/run_*
	rm -rf *.dSYM

.PHONY: all clean
