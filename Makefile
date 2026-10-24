CC ?= clang
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -g -Iinclude -fsanitize=address,undefined -pthread

SRC_VCLOCK = src/vector_clock.c
TEST_SRC_VCLOCK = tests/test_vector_clock.c
BIN_VCLOCK = bin/test_vector_clock

SRC_RING = src/ring.c
TEST_SRC_RING = tests/test_ring.c
BIN_RING = bin/test_ring

SRC_STORAGE = src/storage.c $(SRC_VCLOCK) $(SRC_RING)
TEST_SRC_STORAGE = tests/test_storage.c
BIN_STORAGE = bin/test_storage

SRC_PROTOCOL = src/protocol.c $(SRC_VCLOCK)
TEST_SRC_PROTOCOL = tests/test_protocol.c
BIN_PROTOCOL = bin/test_protocol

SRC_NETWORK = src/network.c $(SRC_PROTOCOL)
TEST_SRC_NETWORK = tests/test_network.c
BIN_NETWORK = bin/test_network

SRC_COORDINATOR = src/coordinator.c src/hint.c src/storage.c src/ring.c src/network.c src/protocol.c src/vector_clock.c
TEST_SRC_COORDINATOR = tests/test_coordinator.c
BIN_COORDINATOR = bin/test_coordinator

SRC_HINT = src/hint.c $(SRC_NETWORK)
TEST_SRC_HINT = tests/test_hint.c
BIN_HINT = bin/test_hint

SRC_MERKLE = src/merkle.c $(SRC_VCLOCK)
TEST_SRC_MERKLE = tests/test_merkle.c
BIN_MERKLE = bin/test_merkle

SRC_GOSSIP = src/gossip.c
TEST_SRC_GOSSIP = tests/test_gossip.c
BIN_GOSSIP = bin/test_gossip

SRC_SERVER = src/server.c src/storage.c src/ring.c src/coordinator.c src/hint.c src/gossip.c src/network.c src/protocol.c src/vector_clock.c
BIN_SERVER = bin/dynamo_server

SRC_CLI = src/cli.c src/network.c src/protocol.c src/vector_clock.c
BIN_CLI = bin/dynamo_cli

BIN_SHELL = bin/dynamo_shell

all: test $(BIN_SERVER) $(BIN_CLI) $(BIN_SHELL)

$(BIN_VCLOCK): $(SRC_VCLOCK) $(TEST_SRC_VCLOCK)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_VCLOCK) $(SRC_VCLOCK) $(TEST_SRC_VCLOCK)

$(BIN_RING): $(SRC_RING) $(TEST_SRC_RING)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_RING) $(SRC_RING) $(TEST_SRC_RING)

$(BIN_STORAGE): $(SRC_STORAGE) $(TEST_SRC_STORAGE)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_STORAGE) $(SRC_STORAGE) $(TEST_SRC_STORAGE)

$(BIN_PROTOCOL): $(SRC_PROTOCOL) $(TEST_SRC_PROTOCOL)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_PROTOCOL) $(SRC_PROTOCOL) $(TEST_SRC_PROTOCOL)

$(BIN_NETWORK): $(SRC_NETWORK) $(TEST_SRC_NETWORK)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_NETWORK) $(SRC_NETWORK) $(TEST_SRC_NETWORK)

$(BIN_COORDINATOR): $(SRC_COORDINATOR) $(TEST_SRC_COORDINATOR)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_COORDINATOR) $(SRC_COORDINATOR) $(TEST_SRC_COORDINATOR)

$(BIN_HINT): $(SRC_HINT) $(TEST_SRC_HINT)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_HINT) $(SRC_HINT) $(TEST_SRC_HINT)

$(BIN_MERKLE): $(SRC_MERKLE) $(TEST_SRC_MERKLE)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_MERKLE) $(SRC_MERKLE) $(TEST_SRC_MERKLE)

$(BIN_GOSSIP): $(SRC_GOSSIP) $(TEST_SRC_GOSSIP)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_GOSSIP) $(SRC_GOSSIP) $(TEST_SRC_GOSSIP)

$(BIN_SERVER): $(SRC_SERVER)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_SERVER) $(SRC_SERVER)

$(BIN_CLI): $(SRC_CLI)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN_CLI) $(SRC_CLI)

.PHONY: $(BIN_SHELL)
$(BIN_SHELL):
	@mkdir -p bin
	@$(MAKE) -C "POSIX Shell"
	@cp "POSIX Shell/shell" $(BIN_SHELL)

test: $(BIN_VCLOCK) $(BIN_RING) $(BIN_STORAGE) $(BIN_PROTOCOL) $(BIN_NETWORK) $(BIN_COORDINATOR) $(BIN_HINT) $(BIN_MERKLE) $(BIN_GOSSIP)
	@./$(BIN_VCLOCK)
	@echo ""
	@./$(BIN_RING)
	@echo ""
	@./$(BIN_STORAGE)
	@echo ""
	@./$(BIN_PROTOCOL)
	@echo ""
	@./$(BIN_NETWORK)
	@echo ""
	@./$(BIN_COORDINATOR)
	@echo ""
	@./$(BIN_HINT)
	@echo ""
	@./$(BIN_MERKLE)
	@echo ""
	@./$(BIN_GOSSIP)

clean:
	rm -rf bin *.dSYM
	@$(MAKE) -C "POSIX Shell" clean

.PHONY: all test clean $(BIN_SHELL)
