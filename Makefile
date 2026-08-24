# MAD interpreter — MAD Ain't Disciplined

CC        ?= cc
CFLAGS    += -std=c17 -Wall -Wextra -pedantic
OPTFLAGS  ?= -O2
LDFLAGS   ?=

SRC_DIR   := src
BUILD_DIR := build
TARGET    := mad

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OPTFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OPTFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

test: $(TARGET)
	@echo "== nqueens =="
	./$(TARGET) examples/nqueens.mad < tests/nqueens.in | diff -u tests/nqueens.expected -
	@echo "== p1038 =="
	./$(TARGET) examples/p1038.mad < tests/p1038.in | diff -u tests/p1038.expected -
	@echo "== branch =="
	./$(TARGET) tests/branch.mad | diff -u tests/branch.expected -
	@echo "All tests passed."

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)
