CC ?= gcc
CFLAGS ?= -O3 -march=native -Wall -Wextra -pthread -D_GNU_SOURCE
BIN_DIR = bin
SRC_DIR = src

TARGETS = $(BIN_DIR)/generator $(BIN_DIR)/baseline_rx

all: $(TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/generator: $(SRC_DIR)/generator/generator.c $(SRC_DIR)/common/market_data.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/generator/generator.c

$(BIN_DIR)/baseline_rx: $(SRC_DIR)/baseline_rx/baseline_rx.c $(SRC_DIR)/common/market_data.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/baseline_rx/baseline_rx.c

clean:
	rm -rf $(BIN_DIR) *.csv *.pcap

test: all
	@echo "=== Ejecutando test de latencia en loopback (10,000 paquetes) ==="
	@./$(BIN_DIR)/baseline_rx -c 10000 -o loopback_test.csv & \
	RX_PID=$$!; \
	sleep 0.5; \
	./$(BIN_DIR)/generator -c 10000 -r 50000; \
	wait $$RX_PID

.PHONY: all clean test
