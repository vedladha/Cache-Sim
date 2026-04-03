CC       = gcc
CFLAGS   = -Wall -Wextra -Wpedantic -O2 -std=c11 -pthread -I include
LDFLAGS  = -pthread

SRC_DIR  = src
INC_DIR  = include
BUILD    = build

SRCS     = $(wildcard $(SRC_DIR)/*.c)
OBJS     = $(patsubst $(SRC_DIR)/%.c, $(BUILD)/%.o, $(SRCS))
TARGET   = cache-sim

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

run: $(TARGET)
	./$(TARGET)

run-fifo: $(TARGET)
	./$(TARGET) --policy fifo

run-comparison: $(TARGET)
	./$(TARGET) --policy lru --seed 42
	./$(TARGET) --policy fifo --seed 42

clean:
	rm -rf $(BUILD) $(TARGET)
