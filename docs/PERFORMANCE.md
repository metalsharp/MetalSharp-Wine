# Performance

Measured 2026-10-22 on Apple M4, macOS 27.2, with `scripts/run-bench.sh`. x86 code runs through FEX with software x86 memory-ordering emulation: no Rosetta, no hardware TSO. x86_64 uses ARM64EC; i386 uses WoW64 and needs the signed MetalSharp-Wine.app (4K pages, low 4G). Ratios are emulated time / native ARM64 time (lower is better).

## Startup

|  | native ARM64 | x86_64 (FEX) | i386 (FEX) |
| --- | --- | --- | --- |
| cold (wineserver + services start) | 482 ms | 495 ms | 525 ms |
| warm (server running) | 25 ms | 31 ms | 64 ms |

## C workloads (best of 3)

| test | native ARM64 | x86_64 (FEX) | i386 (FEX) | ratio x86_64 | ratio i686 | checksums match |
| --- | --- | --- | --- | --- | --- | --- |
| jit | 0.01 ms | 4.56 ms | 4.20 ms | 380.25 | 350.25 | yes |
| int | 106.88 ms | 108.11 ms | 243.70 ms | 1.01 | 2.28 | yes |
| fp | 6.51 ms | 6.29 ms | 9.48 ms | 0.97 | 1.46 | yes |
| mem | 55.53 ms | 55.48 ms | 57.03 ms | 1.00 | 1.03 | yes |
| atomic | 370.57 ms | 364.44 ms | 2963.61 ms | 0.98 | 8.00 | yes |
| order | 149.82 ms | 140.38 ms | 140.12 ms | 0.94 | 0.94 | yes |

## System behaviour

| test | native ARM64 | x86_64 (FEX) | i386 (FEX) |
| --- | --- | --- | --- |
| direct KUSER_SHARED_DATA read | 0.2 ns | 0.3 ns | 0.4 ns |
| thread create + join | 104.3 us | 357.2 us | 330.8 us |
| 2000-function JIT stress, 1 thread (cold / warm) | 0.14 / 0.14 ms | 18.19 / 1.02 ms | 20.05 / 1.07 ms |
| 2000-function JIT stress, 8 threads (cold / warm) | 0.29 / 0.29 ms | 42.26 / 7.20 ms | 43.05 / 8.05 ms |

## CPython 3.14.9 (official python.org embeddable builds)

| test | native ARM64 | x86_64 (FEX) | i386 (FEX) | ratio x86_64 | ratio i686 | results match |
| --- | --- | --- | --- | --- | --- | --- |
| t_hash | 257.8 ms | 169.5 ms | 649.9 ms | 0.66 | 2.52 | yes |
| t_compress | 730.4 ms | 1219.8 ms | 1485.0 ms | 1.67 | 2.03 | yes |
| t_json_re | 136.6 ms | 384.4 ms | 393.4 ms | 2.81 | 2.88 | yes |
| t_decimal_bigint | 79.2 ms | 131.6 ms | 316.9 ms | 1.66 | 4.00 | yes |
| t_sort_float | 183.9 ms | 332.6 ms | 367.0 ms | 1.81 | 2.00 | yes |
| t_threads | 80.8 ms | 215.3 ms | 345.5 ms | 2.66 | 4.28 | yes |
| t_ctypes | 0.1 ms | 2.1 ms | 2.2 ms | 21.00 | 22.00 | yes |
| total | 1468.8 ms | 2455.3 ms | 3560.0 ms | 1.67 | 2.42 | - |
