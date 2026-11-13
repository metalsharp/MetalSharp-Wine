#!/usr/bin/env python3
"""Unpack the downloaded arm64 Steam client packages into ~/.local/share/Steam.

Valve's zips use backslash path separators and carry no Unix permissions, so paths are converted and ELF
and script files are marked executable by their first bytes. The old tree is only removed once every package is present and
opens, and no member is allowed to write outside the destination (prefix match on a real separator, and
no writing through a symlink an earlier member created)."""
import hashlib
import os
import re
import shutil
import stat
import sys
import zipfile

DEST = os.path.realpath(os.path.expanduser("~/.local/share/Steam"))
PK = os.path.join(DEST, "package")
MANIFEST = os.path.join(PK, "steam_client_linuxarm64.manifest")


def manifest_files():
    man = open(MANIFEST).read()
    pkgs = re.findall(r'"file"\s+"([^"]+)"\s+"size"\s+"(\d+)"\s+"sha2"\s+"([0-9a-f]+)"', man)
    if not pkgs:
        sys.exit("manifest lists no packages")
    return pkgs


def check_present(pkgs):
    for f, size, sha in pkgs:
        p = os.path.join(PK, f)
        if not os.path.exists(p) or os.path.getsize(p) != int(size):
            sys.exit("package missing or wrong size: " + f)
        if hashlib.sha256(open(p, "rb").read()).hexdigest() != sha:
            sys.exit("package sha256 mismatch: " + f)
        try:
            zipfile.ZipFile(p).close()
        except zipfile.BadZipFile:
            sys.exit("package is not a valid zip: " + f)


def wipe():
    for n in os.listdir(DEST):
        if n == "package":
            continue
        p = os.path.join(DEST, n)
        if os.path.isdir(p) and not os.path.islink(p):
            shutil.rmtree(p)
        else:
            os.remove(p)


def safe_out(name):
    out = os.path.normpath(os.path.join(DEST, name))
    if out != DEST and not out.startswith(DEST + os.sep):
        sys.exit("package member escapes the destination: " + name)
    # refuse to follow a symlink any path component has become (an earlier member cannot redirect a write)
    parent = os.path.dirname(out)
    if os.path.realpath(parent) != parent and not os.path.realpath(parent).startswith(DEST + os.sep):
        sys.exit("package member writes through a symlink: " + name)
    return out


def is_program(path):
    """Valve's zips carry no Unix permissions: an ELF file or a script is a program by its first bytes."""
    with open(path, "rb") as f:
        head = f.read(4)
    return head == b"\x7fELF" or head[:2] == b"#!"


def main():
    pkgs = manifest_files()
    check_present(pkgs)
    wipe()
    n = 0
    for f, _, _ in pkgs:
        z = zipfile.ZipFile(os.path.join(PK, f))
        for i in z.infolist():
            name = i.filename.replace("\\", "/")
            out = safe_out(name)
            mode = (i.external_attr >> 16) & 0xFFFF
            if name.endswith("/"):
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if stat.S_ISLNK(mode):
                if os.path.lexists(out):
                    os.remove(out)
                os.symlink(z.read(i).decode(), out)
                continue
            if os.path.islink(out):
                os.remove(out)  # never write through a link an earlier member left at this name
            with z.open(i) as s, open(out, "wb") as o:
                shutil.copyfileobj(s, o)
            if mode & 0o111 or is_program(out):
                os.chmod(out, 0o755)
            n += 1
    # the last step: setup takes the client as installed only when this is here
    open(os.path.join(DEST, ".mtsp-unpacked"), "w").close()
    print("files", n)


if __name__ == "__main__":
    main()
