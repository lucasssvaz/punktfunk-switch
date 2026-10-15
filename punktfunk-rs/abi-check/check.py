#!/usr/bin/env python3
# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
"""Compare the forked libc's view of the Switch ABI with devkitA64 newlib + libnx headers.

Nothing here runs on the console. Both sides are compiled for aarch64 and their numbers are read
back out of the object files:

* constants: rustdoc JSON carries libc's evaluated value; the C side evaluates the macro.
* type aliases and structs: size and alignment, plus every public field's offset and size.
  Rust gets them from a generated `pf-abi-check` table, C from `sizeof`/`offsetof`.
* pthread static initializers: raw bytes of the Rust constants vs the C macros.

Items the C headers do not declare are reported as "rust-only" (informational), since libc
carries plenty of names a given libc lacks. Run from punktfunk-rs/ inside the dev container:

    python3 abi-check/check.py            # report
    python3 abi-check/check.py --strict   # exit 1 on any mismatch
"""
import concurrent.futures
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = "aarch64-nintendo-switch-libnx"
DOC_JSON = os.path.join(ROOT, "target", TARGET, "doc", "libc.json")
CC = "aarch64-none-elf-gcc"
DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")
CFLAGS = [
    "-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE",
    "-D__SWITCH__", "-D_GNU_SOURCE", "-D_BSD_SOURCE", "-D_DEFAULT_SOURCE",
    f"-I{DKP}/libnx/include", f"-I{DKP}/portlibs/switch/include",
    f"-I{os.path.join(ROOT, 'shims')}",
]
HEADERS = """
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include "horizon_shims.h"
"""

# Rust struct names whose C spelling is a typedef rather than `struct NAME`.
# Everything else tries `struct NAME` first, then `NAME`.
SKIP_CONSTS = {"RTLD_DEFAULT", "_PATH_BSHELL", "SIG_DFL", "SIG_IGN", "SIG_ERR", "INT_MIN"}
# libc models these as opaque enums that only ever sit behind a pointer.
OPAQUE = {"DIR", "FILE", "fpos_t"}
# Static initializers whose bytes must match the C macros exactly.
INITIALIZERS = [
    ("pthread_mutex_t", "PTHREAD_MUTEX_INITIALIZER"),
    ("pthread_cond_t", "PTHREAD_COND_INITIALIZER"),
    ("pthread_rwlock_t", "PTHREAD_RWLOCK_INITIALIZER"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def load_items():
    d = json.load(open(DOC_JSON))
    idx = d["index"]
    consts, aliases, structs = {}, set(), {}
    for it in idx.values():
        inner = it.get("inner")
        if not isinstance(inner, dict) or it.get("name") is None:
            continue
        name = it["name"]
        if "constant" in inner:
            v = (inner["constant"]["const"].get("value") or "").replace("_", "")
            m = re.fullmatch(r"(-?\d+)(?:[iu](?:8|16|32|64|size|128))?", v)
            if m and name not in SKIP_CONSTS:
                consts[name] = int(m.group(1))
        elif "type_alias" in inner:
            aliases.add(name)
        elif "struct" in inner:
            kind = inner["struct"]["kind"]
            fields = []
            if "plain" in kind:
                for fid in kind["plain"]["fields"]:
                    f = idx[str(fid)]
                    if f.get("name"):
                        fields.append(f["name"])
            structs[name] = fields
    for name in OPAQUE:
        aliases.discard(name)
        structs.pop(name, None)
    return consts, sorted(aliases), structs


def c_compiles(snippet):
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as fh:
        fh.write(HEADERS + snippet + "\n")
        path = fh.name
    try:
        r = run([CC, *CFLAGS, "-fsyntax-only", "-w", path])
        return r.returncode == 0
    finally:
        os.unlink(path)


def resolve_c_types(aliases, structs):
    """Find which spelling (if any) each Rust type has in C, and which fields exist."""
    jobs = {}
    for a in aliases:
        jobs[("alias", a, "")] = f"static const unsigned long x = sizeof({a});"
    for s in structs:
        jobs[("struct", s, "struct")] = f"static const unsigned long x = sizeof(struct {s});"
        jobs[("struct", s, "typedef")] = f"static const unsigned long x = sizeof({s});"
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 8) as ex:
        ok = dict(zip(jobs, ex.map(c_compiles, jobs.values())))

    c_alias = [a for a in aliases if ok[("alias", a, "")]]
    c_struct = {}
    for s in structs:
        if ok[("struct", s, "struct")]:
            c_struct[s] = f"struct {s}"
        elif ok[("struct", s, "typedef")]:
            c_struct[s] = s
    fjobs = {}
    for s, cty in c_struct.items():
        for f in structs[s]:
            fjobs[(s, f)] = f"static const unsigned long x = offsetof({cty}, {f});"
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 8) as ex:
        fok = dict(zip(fjobs, ex.map(c_compiles, fjobs.values())))
    return c_alias, c_struct, fok


def build_layout_tables(c_alias, c_struct, structs, fok):
    """Rows: (label, c_expr, rust_expr). Both sides emit them in this order."""
    rows = []
    for a in c_alias:
        rows.append((f"sizeof({a})", f"sizeof({a})", f"size_of::<libc::{a}>()"))
        rows.append((f"alignof({a})", f"_Alignof({a})", f"align_of::<libc::{a}>()"))
    for s, cty in sorted(c_struct.items()):
        rows.append((f"sizeof({s})", f"sizeof({cty})", f"size_of::<libc::{s}>()"))
        rows.append((f"alignof({s})", f"_Alignof({cty})", f"align_of::<libc::{s}>()"))
        for f in structs[s]:
            if fok.get((s, f)):
                rows.append((f"offsetof({s}.{f})", f"offsetof({cty}, {f})",
                             f"offset_of!(libc::{s}, {f})"))
                rows.append((f"sizeof({s}.{f})", f"sizeof((({cty}*)0)->{f})",
                             f"size_of_field(|v: &libc::{s}| &v.{f})"))
    return rows


def write_c(rows, consts_c, path):
    lines = [HEADERS, "#include <stdint.h>", ""]
    lines.append('__attribute__((used, section(".rodata.pf_abi_c")))')
    lines.append("const int64_t pf_abi_c[] = {")
    for _, cexpr, _ in rows:
        lines.append(f"    (int64_t)({cexpr}),")
    for name in consts_c:
        lines.append(f"    (int64_t)({name}),")
    lines.append("    0x7066616269656e64,")
    lines.append("};")
    lines.append('__attribute__((used, section(".rodata.pf_init_c")))')
    lines.append("const struct {")
    lines += [f"    {ty} f{i};" for i, (ty, _) in enumerate(INITIALIZERS)]
    lines.append("} pf_init_c = {")
    lines += [f"    {init}," for _, init in INITIALIZERS]
    lines.append("};")
    open(path, "w").write("\n".join(lines) + "\n")


def write_rust(rows, path):
    out = [
        "// @generated by abi-check/check.py — do not edit.",
        "#![allow(deprecated, non_camel_case_types)]",
        "use core::mem::{align_of, offset_of, size_of};",
        "",
        "#[allow(dead_code)]",
        "const fn size_of_field<S, F>(_: fn(&S) -> &F) -> usize { size_of::<F>() }",
        "",
        f"pub const N: usize = {len(rows) + 1};",
        "#[no_mangle]",
        '#[link_section = ".rodata.pf_abi_rust"]',
        "#[used]",
        "pub static PF_ABI_RUST: [i64; N] = [",
    ]
    for _, _, rexpr in rows:
        out.append(f"    ({rexpr}) as i64,")
    out.append("    0x7066616269656e64,")
    out.append("];")
    out += ["", "#[repr(C)]", "pub struct Inits("]
    out += [f"    pub libc::{ty}," for ty, _ in INITIALIZERS]
    out += [");", "#[no_mangle]", '#[link_section = ".rodata.pf_init_rust"]', "#[used]",
            "pub static PF_INIT_RUST: Inits = Inits("]
    out += [f"    libc::{init}," for _, init in INITIALIZERS]
    out.append(");")
    open(path, "w").write("\n".join(out) + "\n")


def dump_section_raw(obj, section):
    with tempfile.NamedTemporaryFile(delete=False) as fh:
        out = fh.name
    r = run(["aarch64-none-elf-objcopy", "-O", "binary", f"--only-section={section}", obj, out])
    if r.returncode != 0:
        raise SystemExit(r.stderr)
    data = open(out, "rb").read()
    os.unlink(out)
    return data


def dump_section(obj, section):
    data = dump_section_raw(obj, section)
    return list(struct.unpack(f"<{len(data) // 8}q", data))


def rust_table():
    r = run(["cargo", "build", "--release", "-p", "pf-abi-check"], cwd=ROOT)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise SystemExit("pf-abi-check failed to build: a libc item the C side has is missing "
                         "or private in the fork")
    lib = os.path.join(ROOT, "target", TARGET, "release", "libpf_abi_check.a")
    with tempfile.TemporaryDirectory() as tmp:
        run(["aarch64-none-elf-ar", "x", lib], cwd=tmp)
        for obj in sorted(os.listdir(tmp)):
            objp = os.path.join(tmp, obj)
            r = run(["aarch64-none-elf-objdump", "-h", objp])
            if ".rodata.pf_abi_rust" in r.stdout:
                return (dump_section(objp, ".rodata.pf_abi_rust"),
                        dump_section_raw(objp, ".rodata.pf_init_rust"))
    raise SystemExit("PF_ABI_RUST not found in libpf_abi_check.a")


def main():
    strict = "--strict" in sys.argv
    r = run(["cargo", "rustdoc", "-p", "libc", "--release", "--",
             "-Zunstable-options", "--output-format", "json"], cwd=ROOT)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise SystemExit("rustdoc failed")
    consts, aliases, structs = load_items()

    # Constants: which ones does C define? `#ifdef` covers macros; enum constants fall back to
    # a compile probe.
    probe = HEADERS + "\n".join(
        f'#ifdef {n}\n__attribute__((used)) const char pf_has_{n}[] = "1";\n#endif' for n in consts)
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "p.c")
        open(src, "w").write(probe)
        r = run([CC, *CFLAGS, "-w", "-E", "-dM", src])
        defined = set(re.findall(r"#define (\w+)", r.stdout))
    consts_c = sorted(n for n in consts if n in defined)
    rust_only_consts = sorted(n for n in consts if n not in defined)

    c_alias, c_struct, fok = resolve_c_types(aliases, structs)
    rows = build_layout_tables(c_alias, c_struct, structs, fok)

    gen = os.path.join(ROOT, "pf-abi-check", "src", "generated.rs")
    write_rust(rows, gen)
    rvals, rinit = rust_table()

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "abi.c")
        obj = os.path.join(tmp, "abi.o")
        write_c(rows, consts_c, src)
        r = run([CC, *CFLAGS, "-w", "-O0", "-c", src, "-o", obj])
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            raise SystemExit("C table failed to compile")
        cvals = dump_section(obj, ".rodata.pf_abi_c")
        cinit = dump_section_raw(obj, ".rodata.pf_init_c")

    assert rvals[-1] == cvals[-1] == 0x7066616269656e64, "table sentinel mismatch"
    mismatches = []
    if rinit != cinit:
        mismatches.append(("pthread initializers (bytes)", rinit.hex(), cinit.hex()))
    for i, (label, _, _) in enumerate(rows):
        if rvals[i] != cvals[i]:
            mismatches.append((label, rvals[i], cvals[i]))
    base = len(rows)
    for j, name in enumerate(consts_c):
        cv = cvals[base + j]
        rv = consts[name]
        # Compare as the C value's type would: libc spells some unsigned flags as negative i32.
        if rv != cv and (rv & 0xFFFFFFFF) != (cv & 0xFFFFFFFF):
            mismatches.append((name, rv, cv))

    missing_structs = sorted(s for s in structs if s not in c_struct)
    print(f"checked: {len(consts_c)} constants, {len(c_alias)} type aliases, "
          f"{len(c_struct)} structs ({sum(1 for v in fok.values() if v)} fields)")
    print(f"rust-only (not in C headers): {len(rust_only_consts)} constants, "
          f"{len(set(aliases) - set(c_alias))} aliases, {len(missing_structs)} structs")
    if "-v" in sys.argv:
        print("  rust-only structs:", " ".join(missing_structs))
        print("  rust-only aliases:", " ".join(sorted(set(aliases) - set(c_alias))))
        print("  rust-only consts:", " ".join(rust_only_consts))
    print(f"MISMATCHES: {len(mismatches)}")
    for label, rv, cv in mismatches:
        print(f"  {label:48} rust={rv:<14} c={cv}")
    if strict and mismatches:
        sys.exit(1)


if __name__ == "__main__":
    main()
