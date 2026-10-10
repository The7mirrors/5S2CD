#!/usr/bin/env python3
"""Make link-time stub libraries (export names + SONAME only) for libSceAgc / libSceAgcDriver
from DNNDHH's genstub listings, the way the SDK's own target/lib/*.so stubs look. Never deployed."""
import re, subprocess, sys, pathlib, tempfile
src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
for lib in ("libSceAgc", "libSceAgcDriver"):
    names = sorted(set(re.findall(r'\.global (\w+)', (src / f"{lib}.c").read_text())))
    asm = "".join(f".globl {n}\n.type {n},@function\n{n}:\n" for n in names) + "ret\n"
    with tempfile.TemporaryDirectory() as t:
        s = pathlib.Path(t) / "stub.S"; s.write_text(asm)
        subprocess.check_call(["clang-18", "--target=x86_64-unknown-freebsd", "-c", str(s), "-o", str(pathlib.Path(t) / "stub.o")])
        subprocess.check_call(["ld.lld-18", "-shared", f"-soname={lib}.sprx", "-o", str(out / f"{lib}.so"), str(pathlib.Path(t) / "stub.o")])
    print(f"{lib}.so: {len(names)} exports, SONAME {lib}.sprx")
