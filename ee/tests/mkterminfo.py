#!/usr/bin/env python3
# Build a minimal terminfo entry for tests.  The strings may contain
# % escapes (including malformed ones) to exercise the conditional
# evaluator.  Terminfo binary layout (16-bit, little endian):
#   magic(2) names(2) booleans(2) numbers(2) strings(2) table(2)
#   names NUL separated + terminating NUL
#   booleans (1 byte each)
#   numbers  (2 bytes each, 0xFFFF = absent)
#   string offsets (2 bytes each, 0xFFFF = absent)
#   string table
import struct, sys

CO = 0      # number of columns
LI = 2      # number of lines
CL = 5      # clear screen
CM = 10     # cursor motion
KS = 89     # keypad on
KE = 88     # keypad off
PC = 104    # pad character
NS = 190    # total number of string capabilities (see new_curse.c)

def build(path, clear="\033[H\033[2J", cup="\033[%i%d;%dH", pad="\0"):
    numbers = [0xFFFF] * 40
    numbers[CO] = 80
    numbers[LI] = 24
    soff = [0xFFFF] * NS
    table = bytearray()
    def add(idx, s):
        nonlocal table
        soff[idx] = len(table)
        table += s.encode() + b"\0"
    add(CL, clear)
    add(CM, cup)
    add(PC, pad)
    names = b"openutils-test|x|test terminal for ee tests\0"
    hdr = struct.pack("<HHHHHH", 282, len(names), 0, len(numbers), NS, len(table))
    with open(path, "wb") as f:
        f.write(hdr + names)
        f.write(struct.pack("<%dH" % len(numbers), *numbers))
        f.write(struct.pack("<%dH" % NS, *soff))
        f.write(table)

if __name__ == "__main__":
    build(sys.argv[1], *sys.argv[2:])
