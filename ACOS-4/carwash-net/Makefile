# Сборка ИЗ4. make - сервер и клиенты в bin/, make asan - с ASan и UBSan в bin-asan/,
# make test - сценарные тесты, make clean - удалить собранное.
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c17 -Wall -Wextra -Wpedantic -Iinclude $(EXTRA)
BIN     ?= bin
OBJ     := $(BIN)/obj

ifeq ($(shell uname -s),Linux)
  CFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
endif

COMMON := $(patsubst src/%.c,$(OBJ)/%.o,$(wildcard src/common/*.c))
PROGS  := cw-server cw-post cw-cars cw-view

all: $(addprefix $(BIN)/,$(PROGS))

$(OBJ)/%.o: src/%.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN)/cw-%: $(OBJ)/%.o $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@

asan:
	$(MAKE) BIN=bin-asan EXTRA="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer"

test: all
	tests/run_tests.sh $(BIN)

clean:
	rm -rf bin bin-asan

.PHONY: all asan test clean
.SECONDARY:
