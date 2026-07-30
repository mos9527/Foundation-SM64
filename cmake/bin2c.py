#!/usr/bin/env python3
"""Portable replacement for the Makefile's `hexdump -v -e '1/1 "0x%X,"'` rule.

The original rule is:

    hexdump -v -e '1/1 "0x%X,"' $< > $@
    echo >> $@

which emits one `0x<UPPERCASE-HEX>,` token per input byte (no zero padding,
trailing comma after the very last byte) followed by a single newline.
`hexdump` is not available in a plain MinGW-w64 environment, so the CMake
build uses this script instead.
"""

import sys


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: bin2c.py <input-binary> <output-inc-c>\n")
        return 1

    with open(argv[1], "rb") as fin:
        data = fin.read()

    out = bytearray()
    for byte in data:
        out += b"0x%X," % byte
    out += b"\n"

    with open(argv[2], "wb") as fout:
        fout.write(bytes(out))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
