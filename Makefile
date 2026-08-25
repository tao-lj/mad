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

EXAMPLES := $(sort $(wildcard examples/*/))

test: $(TARGET)
	@set -e; for d in $(EXAMPLES); do \
		echo "== $$(basename $$d) =="; \
		if [ -f "$$d/input" ]; then \
			./$(TARGET) "$$d/main.mad" < "$$d/input" | diff -u "$$d/expected" -; \
		else \
			./$(TARGET) "$$d/main.mad" | diff -u "$$d/expected" -; \
		fi; \
	done
	@echo "All tests passed."

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)
