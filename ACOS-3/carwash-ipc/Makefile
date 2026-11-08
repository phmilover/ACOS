# Сборка всех программ ИЗ3. make - обычная сборка в bin/, make asan - с ASan и UBSan в bin-asan/,
# make test - сценарные тесты, make clean - удалить собранное.
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c17 -Wall -Wextra -Wpedantic -Iinclude $(EXTRA)
LDLIBS  :=
BIN     ?= bin
OBJ     := $(BIN)/obj

UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
  CFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
  LDLIBS += -pthread -lrt
endif

COMMON := src/common/config.c src/common/rng.c src/common/util.c src/common/state.c
SHM    := src/shm/ipc_shm.c src/shm/model.c
FIFO   := src/fifo/proto.c

PROGS := cw1 cw2 cw3-coord cw3-post cw3-cars cw4-coord cw4-post cw4-cars
all: $(addprefix $(BIN)/,$(PROGS)) $(BIN)/ipc-left

obj = $(patsubst src/%.c,$(OBJ)/%.o,$(1))

$(OBJ)/%.o: src/%.c $(wildcard include/*.h) | $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ):
	@mkdir -p $@

$(BIN)/cw1: $(call obj,$(COMMON) $(SHM) src/shm/ipc_named.c src/shm/cw12.c)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BIN)/cw2: $(call obj,$(COMMON) $(SHM) src/shm/ipc_unnamed.c src/shm/cw12.c)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BIN)/cw3-%: $(call obj,$(COMMON) $(SHM) src/shm/ipc_named.c) $(OBJ)/shm/cw3_%.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BIN)/ipc-left: tests/ipc_left.c $(call obj,src/fifo/proto.c src/common/util.c)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BIN)/cw4-%: $(call obj,$(COMMON) $(FIFO)) $(OBJ)/fifo/cw4_%.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

asan:
	$(MAKE) BIN=bin-asan EXTRA="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer"

test: all
	tests/run_tests.sh $(BIN)

clean:
	rm -rf bin bin-asan

.PHONY: all asan test clean
.SECONDARY:
