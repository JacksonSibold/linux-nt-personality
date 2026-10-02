# Conformance test build for the NT ABI infrastructure workstream.
#
# Builds:
#   - syscall_stub.S   (assembly stubs generated from syscall_table.def)
#   - ntstatus_to_dos.c
#   - test_nt_types.c
#
# and runs the conformance binary.

CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -std=c11 -m64
INC     := -I../../src/ntdll
NTDLL   := ../../src/ntdll

.PHONY: all clean test

all: test

test: test_nt_types
	./test_nt_types

syscall_stub.o: $(NTDLL)/syscall_stub.S $(NTDLL)/syscall_table.def
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

test_nt_types: test_nt_types.c $(NTDLL)/ntstatus_to_dos.c syscall_stub.o
	$(CC) $(CFLAGS) $(INC) test_nt_types.c $(NTDLL)/ntstatus_to_dos.c syscall_stub.o -o $@

clean:
	rm -f test_nt_types syscall_stub.o
