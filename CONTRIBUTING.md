# Contributing to MetalSharp Wine

Thanks for helping. MetalSharp Wine is Wine built as a native arm64 macOS app, with a FEX fork
for x86_64 and i386 Windows programs; the [README](README.md) describes what it includes and
[docs/STATUS.md](docs/STATUS.md) the design, the results and the open items.

## Before you start

- **Bugs and program reports**: open an issue with:
  - `wine --version`, and the download you use (`MetalSharp-Wine.app` or the `.tar.xz` folder);
  - the macOS version and Mac model;
  - the program, its architecture (ARM64, x86_64 or 32-bit) and the graphics API it uses;
  - a log of the failing run: `WINEDEBUG=+loaddll,+seh,err` is a good start, with any
    backend switch you set (`GRAPHICS_BACKEND`, `OPENGL_BACKEND`).
- **Security issues**: never in a public issue. See [SECURITY.md](SECURITY.md).
- **Larger changes** (a new backend, a change to the loader policy, the signing or the
  entitlements, a new bundled component): open an issue first, so the approach is agreed
  before the work.
- **Elsewhere:** some parts live in other repositories. Contribute those there:
  - Mullion: [dttdrv/mullion](https://github.com/dttdrv/mullion).
  - WineMetalGL: [metalsharp/WineMetalGL](https://github.com/metalsharp/WineMetalGL).
  - The MetalSharp app (launching, Steam, bottles): [metalsharp/MetalSharp](https://github.com/metalsharp/MetalSharp).

## Where changes go

| Change | Where |
|---|---|
| Wine | a commit on branch `source/wine` (`src/wine`), exported to `patches/wine` with `git format-patch` |
| FEX | a commit on branch `source/fex` (`src/FEX`), exported to `patches/fex`. The FEX fork is never sent upstream: FEX does not accept AI-assisted contributions |
| WineMetalGL's arm64 Wine module | branch `source/winemetalgl`, and upstream as a pull request to WineMetalGL |
| Build, packaging, signing, tests | `scripts/`, `tests/src/` on `main` |

A change to a `source/*` branch also moves its submodule pin on `main` (`git -C src/wine
checkout <commit>`, then commit `src/wine`), so `main` always names the commits a release is
built from.

## Building and testing

[README: Building](README.md#building) has the steps:

```sh
git clone --recurse-submodules https://github.com/metalsharp/MetalSharp-Wine.git
zsh scripts/build-all.sh
```

Before opening a pull request:
- Build it.
- Run the smoke tests for what you changed, from the unpackaged `install/` tree:

  ```sh
  zsh scripts/build-tests.sh
  install/bin/wine tests/bin/<test>-<arch>.exe
  ```

  [docs/TESTING.md](docs/TESTING.md) lists the tests. Run each on every architecture it covers. 32-bit programs need the signed app, built by `scripts/sign-app.sh` with a Developer ID and the provisioning profile.
- Wine's full conformance suites take hours. Run them only for changes to ntdll, the loader, wineserver or sync, and say which units you ran.

## Rules

- **Patches and branches stay in step:** `patches/` and the `source/*` branches describe the same commits.
- **No Rosetta and no hardware TSO:** nothing may depend on either.
- **Pinned downloads:** every dependency the scripts fetch is pinned to a version and checked against a SHA-256.
- **Prefixes stay private:** no Wine prefixes, user registries or other personal data in commits.
- **Licenses:** MetalSharp Wine's own material is AGPL-3.0 ([LICENSE](LICENSE)). Changes to Wine stay LGPL-2.1-or-later and changes to FEX stay MIT, like the code they change.
