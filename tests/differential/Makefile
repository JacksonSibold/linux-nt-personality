# Differential checker build
CC := gcc
CFLAGS := -Wall -Wextra -Werror -std=c11 -m64
INC := -I../../src/ntdll
NTDLL := ../../src/ntdll

.PHONY: all clean check

all: check_ntstatus check_process_thread

check_ntstatus: check_ntstatus.c $(NTDLL)/ntstatus_to_dos.c
	$(CC) $(CFLAGS) $(INC) check_ntstatus.c $(NTDLL)/ntstatus_to_dos.c -o $@

check_process_thread: check_process_thread.c
	$(CC) $(CFLAGS) check_process_thread.c -o $@

check: check_ntstatus check_process_thread
	./check_ntstatus golden/latest/probe_ntstatus.json
	@echo "check_ntstatus PASS"
	./check_process_thread golden/latest/probe_process_thread.json
	@echo "check_process_thread PASS"

clean:
	rm -f check_ntstatus check_process_thread
