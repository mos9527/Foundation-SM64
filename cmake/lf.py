#!/usr/bin/env python3
"""Rewrite the given files in place with LF line endings.

Some of the host tools (textconv in particular) open their output with the C
runtime's text mode, so on Windows every '\n' becomes '\r\n'. That would make
the generated sources differ byte-for-byte from what the upstream Makefile
produces, which is the property the CMake port is validated against.
"""

import sys


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: lf.py <file> [file...]\n")
        return 1

    for path in argv[1:]:
        with open(path, "rb") as f:
            data = f.read()
        normalised = data.replace(b"\r\n", b"\n")
        if normalised != data:
            with open(path, "wb") as f:
                f.write(normalised)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
