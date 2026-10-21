#!/usr/bin/env python3
"""runtime/sha256.c on its own: a small driver compiled with it hashes files in chunks of a given size and prints the
digest, which is checked against FIPS 180-4's examples and against Python's hashlib.

    python3 tests/sha256_test.py [--sanitize] [--cc CC]

What it checks: the NIST vectors ("abc", the 56-byte two-block message, one million 'a'), the empty message, every
length from 0 to 200 (each padding case around the 56- and 64-byte boundaries) fed in chunks of 1, 7, 63, 64, 65
and the whole, and the one-shot helper. --sanitize adds AddressSanitizer and UBSan. Exit 0 pass, 1 fail, 2 no
compiler.
"""
import argparse
import hashlib
import random
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
/* sha256_test FILE CHUNK: the file's digest fed CHUNK bytes at a time (0: one port_sha256 call over all of it) */
int main(int argc, char **argv) {
    FILE *f;
    unsigned char *data;
    long size, chunk, at;
    unsigned char digest[32];
    char hex[65];
    PortSha256 c;
    if (argc != 3 || (f = fopen(argv[1], "rb")) == NULL) {
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(size > 0 ? (size_t)size : 1);
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        return 2;
    }
    fclose(f);
    chunk = atol(argv[2]);
    if (chunk == 0) {
        port_sha256(data, (size_t)size, digest);
    } else {
        port_sha256_init(&c);
        for (at = 0; at < size; at += chunk) {
            port_sha256_update(&c, data + at, (size_t)(size - at < chunk ? size - at : chunk));
        }
        port_sha256_final(&c, digest);
    }
    port_sha256_hex(digest, hex);
    puts(hex);
    free(data);
    return 0;
}
"""

# FIPS 180-4's examples (the NIST "Examples with Intermediate Values" for SHA-256).
VECTORS = [
    (b"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
    (b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
    (b"a" * 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
    (b"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sanitize", action="store_true")
    ap.add_argument("--cc", default="gcc")
    args = ap.parse_args()
    if not shutil.which(args.cc):
        print(f"sha256 test: no {args.cc}")
        return 2
    out = ROOT / "build" / ("sha256_test" + ("-asan" if args.sanitize else ""))
    out.mkdir(parents=True, exist_ok=True)
    (out / "driver.c").write_text(DRIVER)
    flags = ["-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'include/psxstack'}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined"]
    exe = out / "sha256_test"
    r = subprocess.run([args.cc, *flags, str(out / "driver.c"), str(ROOT / "runtime/sha256.c"), "-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("sha256 test: the build failed:\n" + r.stderr)
        return 1
    failures = 0

    def digest(data, chunk):
        f = out / "input.bin"
        f.write_bytes(data)
        r = subprocess.run([str(exe), str(f), str(chunk)], capture_output=True, text=True, timeout=60)
        if r.returncode != 0 or r.stderr.strip():
            return f"(exit {r.returncode}: {r.stderr.strip()})"
        return r.stdout.strip()

    def check(ok, what):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + what)
        failures += not ok

    for data, want in VECTORS:
        name = repr(data) if len(data) < 60 else f"{len(data)} x {data[:1]!r}"
        got = {chunk: digest(data, chunk) for chunk in (0, 1 if len(data) < 1000 else 4096, 64, 65)}
        check(all(g == want for g in got.values()), f"FIPS 180-4: {name} (one shot, and in chunks)")
    rng = random.Random(256)
    bad = []
    for n in range(201):
        data = bytes(rng.randrange(256) for _ in range(n))
        want = hashlib.sha256(data).hexdigest()
        for chunk in (0, 1, 7, 63, 64, 65, max(n, 1)):
            if digest(data, chunk) != want:
                bad.append((n, chunk))
    check(not bad, "every length from 0 to 200 bytes, in chunks of 1, 7, 63, 64, 65 and whole, equals hashlib" +
          (f": wrong at {bad[:5]}" if bad else ""))
    print(f"sha256 test: {'FAIL' if failures else 'pass'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
