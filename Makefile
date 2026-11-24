CC = cc
CPPFLAGS ?=
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS = -pthread
PYTHON ?= python3
DATA_DIR ?= .
FRAME_COUNT ?= 128
USE_TLB ?= 1
THREADED_TLB ?= 1

.PHONY: all clean test etapas

all: vm

# FORCE evita reutilizar um binario compilado com outra etapa da rubrica.
vm: vm.c FORCE
	$(CC) $(CPPFLAGS) -DFRAME_COUNT=$(FRAME_COUNT) -DUSE_TLB=$(USE_TLB) -DTHREADED_TLB=$(THREADED_TLB) $(CFLAGS) vm.c $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: FORCE
FORCE:

test: vm
	$(PYTHON) tests/test_vm.py --data "$(DATA_DIR)"

etapas:
	$(PYTHON) tests/test_vm.py --stages --data "$(DATA_DIR)"

clean:
	rm -f vm vm.exe *.o correct.txt correct.txt.tmp
