#!/usr/bin/env python3
"""Describe what a 64-bit Linux shared library needs from a host.

Usage: elfinfo.py library.so [--imports]

Prints the architecture, the load segments and their alignment, the libraries it needs, thread-local
storage, its imports grouped by the library that should provide them, and how many direct system-call
instructions sit in executable segments. With --imports it prints one "library<TAB>symbol" line per
import, for building the list of functions the Linux module host has to provide.
"""
import collections
import struct
import sys

PT_LOAD, PT_DYNAMIC, PT_TLS, PT_GNU_RELRO = 1, 2, 7, 0x6474E552
DT = dict(NEEDED=1, STRTAB=5, SYMTAB=6, STRSZ=10, SONAME=14, RUNPATH=29, INIT_ARRAYSZ=27,
          VERSYM=0x6FFFFFF0, VERNEED=0x6FFFFFFE, VERNEEDNUM=0x6FFFFFFF, GNU_HASH=0x6FFFFEF5, HASH=4)
MACHINES = {62: "x86_64", 183: "aarch64"}


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            sys.exit("not a 64-bit little-endian ELF file")
        (self.type, self.machine) = struct.unpack_from("<HH", d, 16)
        (phoff,) = struct.unpack_from("<Q", d, 32)
        (phentsize, phnum) = struct.unpack_from("<HH", d, 54)
        self.segments = []
        for i in range(phnum):
            p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, p_align = struct.unpack_from(
                "<IIQQQQQQ", d, phoff + i * phentsize)
            self.segments.append(dict(type=p_type, flags=p_flags, offset=p_offset, vaddr=p_vaddr,
                                      filesz=p_filesz, memsz=p_memsz, align=p_align))
        self.dynamic = collections.defaultdict(list)
        for seg in self.segments:
            if seg["type"] == PT_DYNAMIC:
                for off in range(seg["offset"], seg["offset"] + seg["filesz"], 16):
                    tag, val = struct.unpack_from("<qQ", d, off)
                    if tag == 0:
                        break
                    self.dynamic[tag].append(val)

    def offset(self, vaddr):
        for seg in self.segments:
            if seg["type"] == PT_LOAD and seg["vaddr"] <= vaddr < seg["vaddr"] + seg["filesz"]:
                return vaddr - seg["vaddr"] + seg["offset"]
        raise ValueError(hex(vaddr))

    def string(self, index):
        start = self.offset(self.dynamic[DT["STRTAB"]][0]) + index
        return self.data[start:self.data.index(b"\0", start)].decode("utf-8", "replace")

    def names(self, tag):
        return [self.string(v) for v in self.dynamic.get(DT[tag], [])]

    def symbol_count(self):
        d = self.data
        if DT["HASH"] in self.dynamic:
            return struct.unpack_from("<I", d, self.offset(self.dynamic[DT["HASH"]][0]) + 4)[0]
        base = self.offset(self.dynamic[DT["GNU_HASH"]][0])
        nbuckets, symoffset, bloom_size, _ = struct.unpack_from("<IIII", d, base)
        buckets = base + 16 + bloom_size * 8
        chains = buckets + nbuckets * 4
        last = max(struct.unpack_from("<%dI" % nbuckets, d, buckets), default=0)
        if last < symoffset:
            return symoffset
        while not struct.unpack_from("<I", d, chains + (last - symoffset) * 4)[0] & 1:
            last += 1
        return last + 1

    def version_files(self):
        """Version index -> (library, version name)."""
        out = {}
        if DT["VERNEED"] not in self.dynamic:
            return out
        d, off = self.data, self.offset(self.dynamic[DT["VERNEED"]][0])
        for _ in range(self.dynamic[DT["VERNEEDNUM"]][0]):
            _, cnt, file_idx, aux, nxt = struct.unpack_from("<HHIII", d, off)
            a = off + aux
            for _ in range(cnt):
                _, _, other, name, anext = struct.unpack_from("<IHHII", d, a)
                out[other] = (self.string(file_idx), self.string(name))
                a += anext
            off += nxt
        return out

    def symbols(self):
        d = self.data
        symtab = self.offset(self.dynamic[DT["SYMTAB"]][0])
        versym = self.offset(self.dynamic[DT["VERSYM"]][0]) if DT["VERSYM"] in self.dynamic else None
        versions = self.version_files()
        for i in range(1, self.symbol_count()):
            name, info, _, shndx, _, _ = struct.unpack_from("<IBBHQQ", d, symtab + i * 24)
            ver = struct.unpack_from("<H", d, versym + i * 2)[0] & 0x7FFF if versym is not None else 0
            yield dict(name=self.string(name), bind=info >> 4, type=info & 15, undefined=shndx == 0,
                       version=versions.get(ver))

    def syscall_instructions(self):
        """Direct system-call instructions in executable segments: svc #0 on aarch64, syscall on x86_64."""
        count = 0
        for seg in self.segments:
            if seg["type"] != PT_LOAD or not seg["flags"] & 1:
                continue
            code = self.data[seg["offset"]:seg["offset"] + seg["filesz"]]
            if self.machine == 183:
                needle, step = b"\x01\x00\x00\xd4", 4
                pos = code.find(needle)
                while pos != -1:
                    if pos % step == 0:
                        count += 1
                    pos = code.find(needle, pos + 1)
            else:
                count += code.count(b"\x0f\x05")  # upper bound: the bytes can occur inside other instructions
        return count


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    elf = Elf(sys.argv[1])
    imports = collections.defaultdict(list)
    exported = 0
    for s in elf.symbols():
        if s["undefined"] and s["name"]:
            lib = s["version"][0] if s["version"] else ("(weak, unversioned)" if s["bind"] == 2 else "(unversioned)")
            imports[lib].append(s["name"])
        elif not s["undefined"] and s["bind"] in (1, 2):
            exported += 1

    if "--imports" in sys.argv:
        for lib in sorted(imports):
            for name in sorted(set(imports[lib])):
                print(f"{lib}\t{name}")
        return

    print("machine:  ", MACHINES.get(elf.machine, elf.machine))
    print("soname:   ", ", ".join(elf.names("SONAME")) or "-")
    print("needs:    ", ", ".join(elf.names("NEEDED")))
    print("runpath:  ", ", ".join(elf.names("RUNPATH")) or "-")
    for seg in elf.segments:
        if seg["type"] == PT_LOAD:
            perms = "".join(c if seg["flags"] & b else "-" for c, b in (("r", 4), ("w", 2), ("x", 1)))
            print(f"load:      {perms} vaddr {seg['vaddr']:#x} size {seg['memsz']:#x} align {seg['align']:#x}")
        elif seg["type"] == PT_TLS:
            print(f"tls:       size {seg['memsz']:#x} align {seg['align']:#x}")
    print("exports:  ", exported)
    print("imports:  ", sum(len(set(v)) for v in imports.values()))
    for lib in sorted(imports):
        print(f"  {lib}: {len(set(imports[lib]))}")
    versions = sorted({v[1] for v in elf.version_files().values()},
                      key=lambda s: [int(x) if x.isdigit() else x for x in s.replace("_", ".").split(".")])
    print("versions: ", ", ".join(versions))
    kind = "svc #0" if elf.machine == 183 else "syscall (upper bound)"
    print(f"direct system calls in code: {elf.syscall_instructions()} x {kind}")


if __name__ == "__main__":
    main()
