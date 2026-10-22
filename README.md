<div align="center">

# MetalSharp Wine

**Cross-architecture ARM64 Wine with FEX for Apple Silicon macOS.**

Developer Note: This project is under active development. It should be noted that you can install / run steam but the current i386 / WoW64 mapping is not 100% completed so you will experience issues. This project is now in development again as of 10/22/2026 so I expect it to be finished in not too long. 

<a href="https://github.com/metalsharp/VKMT-Wine/releases/tag/VKMT-1.0"><img src="https://img.shields.io/github/v/release/metalsharp/VKMT-Wine?filter=VKMT-1.0&style=for-the-badge" alt="Release"></a>
<a href="https://github.com/metalsharp/VKMT-Wine/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/metalsharp/VKMT-Wine/ci.yml?branch=main&style=for-the-badge&label=CI" alt="CI"></a>
<a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg?style=for-the-badge" alt="License: MIT for VKMT-authored material"></a>

</div>

---

MetalSharp Wine is an Apple-Silicon-native, cross-architecture Wine runtime for
macOS. The Wine host and native Unix boundary are ARM64; FEX supplies the
execution path for x86_64 and i386/WoW64 Windows applications.

## Architecture

~~~text
Apple Silicon macOS
└── ARM64 Wine host and wineserver
    ├── ARM64/AArch64 Windows
    ├── ARM64EC Windows
    ├── x86_64 Windows  ── FEX xtajit64
    └── i386/WoW64      ── FEX xtajit
~~~

xtajit64 and xtajit are required runtime providers. Both are retained in the
runtime, along with their architecture-specific build and compatibility
variants.

## Quick start

See the [Quick Start](docs/quick-start.md) for verified installation and
launch commands.

## Documentation

- [Complete runtime inventory](docs/runtime-inventory.md)
- [Quick Start](docs/quick-start.md)
- [Project overview](docs/project-overview.md)
- [Architecture](docs/architecture.md)
- [Graphics](docs/graphics.md)
- [D3DMetal integration audit and roadmap](https://github.com/metalsharp/VKMT-Wine/blob/main/D3DMetal.md)
- [Performance](docs/performance.md)
- [Packaging and validation](docs/package-and-validation.md)
- [MetalSharp integration](docs/metalsharp-integration.md)
- [Third-party license index](docs/third-party-licenses.md)
- [Security policy](SECURITY.md)

The complete inventory is the component-level description of the shipped
runtime. The release's file hashes and asset manifest remain authoritative for
the exact bytes in a particular build.

## Continuous integration

GitHub Actions runs portable checks for shell syntax and ShellCheck, Python
compilation and Ruff, Rust formatting/Clippy/tests, a CMake configure/build/test
smoke project, and Java compilation/execution. CodeQL security analysis covers
C/C++, Java, Rust, and Python; ShellCheck is the shell security analyzer. The
CI-only fixtures are kept out of redistributable runtime source snapshots.

## License

Metalsharp-authored repository material is MIT licensed; see [LICENSE](LICENSE).
Wine, FEX, DXMT, DXVK, VKD3D-Proton, MoltenVK, Mono, and other bundled
components retain their own licenses. See the [third-party license index](docs/third-party-licenses.md)
before redistributing the runtime.
