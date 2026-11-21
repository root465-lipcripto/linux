# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O2 -g
WARNINGS := -std=c11 -Wall -Wextra -Werror -pedantic
TARGET := ram-adaptive

.PHONY: all clean test

all: $(TARGET)

$(TARGET): ram-adaptive.c
	$(CC) $(CFLAGS) $(WARNINGS) -o $@ $< -lm

test: $(TARGET)
	python3 test_ram_adaptive.py ./$(TARGET)

clean:
	rm -f $(TARGET)
