# Correctness + performance workload for the embeddable CPython builds.
# Every test prints a deterministic checksum; output from different architectures must match.
import bz2, ctypes, decimal, hashlib, json, lzma, platform, random, re, struct, sys, threading, time, zlib


def t_hash():
    data = bytes(range(256)) * (1 << 16)   # 16 MiB
    return "".join(hashlib.new(n, data).hexdigest()[:16] for n in ("md5", "sha1", "sha256", "sha512", "blake2b", "sha3_256"))


def t_compress():
    rnd = random.Random(1234)
    data = bytes(rnd.getrandbits(8) & 0x3f for _ in range(1 << 20)) * 4
    out = []
    for mod in (zlib, bz2, lzma):
        c = mod.compress(data)
        assert mod.decompress(c) == data
        out.append(f"{len(c)}")
    return ",".join(out) + f",{zlib.crc32(data):08x}"


def t_json_re():
    rnd = random.Random(42)
    doc = [{"id": i, "name": f"item{i}", "v": rnd.random(), "tags": [rnd.choice("abcxyz") for _ in range(5)]} for i in range(50000)]
    s = json.dumps(doc, sort_keys=True)
    back = json.loads(s)
    n = len(re.findall(r'"name": "item(\d*7)"', s))
    return f"{hashlib.sha256(s.encode()).hexdigest()[:16]},{len(back)},{n}"


def t_decimal_bigint():
    decimal.getcontext().prec = 2000
    pi = decimal.Decimal(0)
    for k in range(400):  # Bailey-Borwein-Plouffe
        k8 = 8 * k
        pi += (decimal.Decimal(1) / (16 ** k)) * (decimal.Decimal(4) / (k8 + 1) - decimal.Decimal(2) / (k8 + 4) -
                                                  decimal.Decimal(1) / (k8 + 5) - decimal.Decimal(1) / (k8 + 6))
    f = 1
    for i in range(2, 20000):
        f *= i
    return f"{str(pi)[:40]},{hashlib.sha256(str(f).encode()).hexdigest()[:16]}"


def t_sort_float():
    rnd = random.Random(7)
    a = [rnd.random() * 1e6 - 5e5 for _ in range(1_000_000)]
    a.sort()
    s = sum(x * x for x in a[::997])
    packed = struct.pack(f"<{len(a[:1000])}d", *a[:1000])
    return f"{s!r},{zlib.crc32(packed):08x}"


def t_threads():
    lock = threading.Lock()
    total = [0]

    def work(n):
        local = 0
        for i in range(n):
            local += i * i % 7
        with lock:
            total[0] += local

    ts = [threading.Thread(target=work, args=(300000,)) for _ in range(8)]
    for t in ts: t.start()
    for t in ts: t.join()
    return str(total[0])


def t_ctypes():
    k32 = ctypes.windll.kernel32
    buf = ctypes.create_unicode_buffer(260)
    k32.GetSystemDirectoryW(buf, 260)
    tick = k32.GetTickCount64
    tick.restype = ctypes.c_uint64
    return f"{buf.value.lower()},{tick() > 0}"


sys.set_int_max_str_digits(0)
TESTS = [t_hash, t_compress, t_json_re, t_decimal_bigint, t_sort_float, t_threads, t_ctypes]

if __name__ == "__main__":
    print(f"python {platform.python_version()} {platform.machine()} {struct.calcsize('P') * 8}-bit")
    total = 0
    for t in TESTS:
        start = time.perf_counter()
        r = t()
        dt = time.perf_counter() - start
        total += dt
        print(f"{t.__name__:18} {dt * 1000:9.1f} ms  {r}")
    print(f"{'total':18} {total * 1000:9.1f} ms")
