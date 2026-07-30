#!/usr/bin/env python3
"""Run a command with its stdout (and optionally stdin) redirected to files.

`add_custom_command` has no portable way to express shell redirection, but
several of the upstream Makefile rules rely on it, e.g.

    $(AIFF_EXTRACT_CODEBOOK) $< >$@
    $(CPP) -I . levels/level_headers.h.in | $(PYTHON) tools/output_level_headers.py > $@

This helper provides that missing piece:

    redirect.py <stdout-file> [--stdin <file>] [--binary] -- <command> [args...]

Captured output is newline-normalised to LF unless --binary is given: every
generator used here writes text, and both the Python interpreter and the MinGW
C runtime translate '\n' to '\r\n' on stdout under Windows, which would make
the generated sources differ from the upstream Makefile's output.

The output file is written atomically-ish (only replaced once the command
succeeded) so that a failed build step never leaves a truncated artifact
behind that a later incremental build would consider up to date.
"""

import os
import subprocess
import sys


def main(argv):
    args = argv[1:]
    if not args:
        sys.stderr.write(
            "usage: redirect.py <stdout-file> [--stdin <file>] -- <command> [args...]\n")
        return 1

    stdout_path = args.pop(0)
    stdin_path = None
    binary = False

    while args and args[0] != "--":
        if args[0] == "--stdin":
            if len(args) < 2:
                sys.stderr.write("redirect.py: --stdin requires an argument\n")
                return 1
            stdin_path = args[1]
            del args[0:2]
        elif args[0] == "--binary":
            binary = True
            del args[0:1]
        else:
            sys.stderr.write("redirect.py: unexpected argument %r\n" % args[0])
            return 1

    if not args or args[0] != "--":
        sys.stderr.write("redirect.py: missing '--' before the command\n")
        return 1
    command = args[1:]
    if not command:
        sys.stderr.write("redirect.py: no command given\n")
        return 1

    out_dir = os.path.dirname(stdout_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    tmp_path = stdout_path + ".tmp"
    fin = open(stdin_path, "rb") if stdin_path else None
    try:
        with open(tmp_path, "wb") as fout:
            rc = subprocess.call(command, stdin=fin, stdout=fout)
    finally:
        if fin is not None:
            fin.close()

    if rc != 0:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        sys.stderr.write("redirect.py: command failed (%d): %s\n"
                         % (rc, " ".join(command)))
        return rc

    if not binary:
        with open(tmp_path, "rb") as f:
            data = f.read()
        normalised = data.replace(b"\r\n", b"\n")
        if normalised != data:
            with open(tmp_path, "wb") as f:
                f.write(normalised)

    if os.path.exists(stdout_path):
        os.remove(stdout_path)
    os.replace(tmp_path, stdout_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
