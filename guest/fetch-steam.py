# Downloads Valve's native arm64 Linux Steam client packages listed in the public manifest, verifies each
# sha256 and leaves them in ~/.local/share/Steam/package for unpack-steam.py.
import hashlib, os, re, socket, sys, urllib.request
socket.setdefaulttimeout(60)  # a stalled connection fails instead of hanging the setup
BASE = "https://client-update.steamstatic.com/"
dest = os.path.expanduser("~/.local/share/Steam"); pk = os.path.join(dest, "package"); os.makedirs(pk, exist_ok=True)
man = urllib.request.urlopen(BASE + "steam_client_linuxarm64", timeout=30).read().decode()
open(os.path.join(pk, "steam_client_linuxarm64.manifest"), "w").write(man)
ver = re.search(r'"version"\s+"(\d+)"', man).group(1)
pkgs = re.findall(r'"file"\s+"([^"]+)"\s+"size"\s+"(\d+)"\s+"sha2"\s+"([0-9a-f]+)"', man)
print("version", ver, "packages", len(pkgs), "total MB", sum(int(s) for _, s, _ in pkgs) // 1048576, flush=True)
def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()

for f, size, sha in pkgs:
    if "/" in f or "\\" in f or f.startswith("."):
        sys.exit("unexpected package name " + f)
    p = os.path.join(pk, f)
    if not (os.path.exists(p) and os.path.getsize(p) == int(size) and sha256(p) == sha):
        tmp = p + ".part"
        urllib.request.urlretrieve(BASE + f, tmp)
        if sha256(tmp) != sha:
            os.remove(tmp)
            sys.exit("sha256 mismatch for " + f)
        os.replace(tmp, p)
    print("ok", f, flush=True)
print("done")
