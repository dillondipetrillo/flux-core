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
TEST_CONN_MAP = tests/test_conn_map.c src/conn_map.c
TEST_PROTOCOL = tests/test_protocol.c
TEST_RATE_LIMIT = tests/test_rate_limit.c
TEST_SCOPE_MAP = tests/test_scope_map.c src/scope_map.c
TEST_TTL = tests/test_ttl.c

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

tests/run_conn_map: $(TEST_CONN_MAP)
	$(CC) $(CFLAGS) -o $@ $(TEST_CONN_MAP)

tests/run_protocol: $(TEST_PROTOCOL)
	$(CC) $(CFLAGS) -o $@ $(TEST_PROTOCOL)

tests/run_rate_limit: $(TEST_RATE_LIMIT)
	$(CC) $(CFLAGS) -o $@ $(TEST_RATE_LIMIT)

tests/run_scope_map: $(TEST_SCOPE_MAP)
	$(CC) $(CFLAGS) -o $@ $(TEST_SCOPE_MAP)

tests/run_ttl: $(TEST_TTL)
	$(CC) $(CFLAGS) -o $@ $(TEST_TTL)

test: tests/run_auth tests/run_rate_limit tests/run_scope_map \
		tests/run_conn_map tests/run_protocol tests/run_ttl
	./tests/run_auth
	./tests/run_conn_map
	./tests/run_protocol
	./tests/run_rate_limit
	./tests/run_scope_map
	./tests/run_ttl
	@echo "All unit tests passed."

sanitize: $(ENGINE_SRCS)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o server_san $(ENGINE_SRCS) $(LDFLAGS)

integration: tests/test_integration.c src/logger.c
	$(CC) $(CFLAGS) -o tests/run_integration \
		tests/test_integration.c src/logger.c -lpthread

bench: bench/bench_routing.c src/logger.c
	$(CC) $(CFLAGS) -O2 -o bench/bench_routing bench/bench_routing.c \
		src/logger.c -lpthread

debug: CFLAGS += -DDEBUG_LOG
debug: all

server_opt: $(ENGINE_SRCS)
	$(CC) -Wall -Wextra -O2 -Iinclude -o server_opt $(ENGINE_SRCS) \
		-lpthread -lssl -lcrypto -lcurl

clean:
	rm -f server client server_san server_opt
	rm -rf tests/run_*
	rm -rf bench/bench_routing
	rm -rf *.dSYM

.PHONY: all test integration sanitize bench debug clean
