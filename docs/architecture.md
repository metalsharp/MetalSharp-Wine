# Developer Guide

## Purpose

WFDXCompat is a PE-level compatibility layer placed between a Windows
application and the graphics backend selected by WineForge. It implements
small, verified pieces of missing public DirectX behavior and forwards
everything else to the backend.

```text
application
  -> WFDXCompat frontend DLL
      -> compatibility path for a matched API gap
      -> selected backend for all other calls
```

WFDXCompat does not choose applications or graphics backends. That policy
belongs to the WineForge loader.

## Source map

```text
exports/
  d3d12.exports              Public export list
  d3d12core.def              Agility layered-core bridge exports
include/wfdxcompat/          Internal interfaces
src/core/
  notifier_registry.c       Thread-safe callback lifetime registry
src/frontend/
  module.c                   DLL entry point
  proxy.c                    D3D12 entry points and device interception
  agility_shim.c             D3D12 Agility SDK activation bridge
  nv12_resource.c            Two-plane NV12 facade
  command_list_proxy.c       Copy and barrier translation
  resource_notifier.c        Narrow resource-lifetime compatibility
tests/                       Fake backend and focused API probes
tools/
  generate_forwarders.sh     Generates the frontend DEF file
```

`build.sh` produces:

```text
lib/wfdxcompat/x86_64-windows/
  d3d12.dll
  d3d12core.dll
  wfdxbackend-d3d12.dll
```

`d3d12core.dll` forwards the Agility layered-core exports to the selected
backend. `wfdxbackend-d3d12.dll` is a Wine loader marker, not a graphics
backend.

## Loader contract

The frontend imports the private name `wfdxbackend-d3d12.dll`. WineForge maps
that name to the D3D12 module of the already-selected backend. The mapping is
process-stable, must never point back to WFDXCompat, and must preserve the
normal backend path when WFDXCompat is absent. Backend binaries are not part of
this package.

## Current D3D12 module

The current frontend provides an NV12 facade backed by `R8` and `RG8`
textures, translates the required views, copies, and barriers, and supplies a
narrowly matched resource-lifetime contract. Exact matching rules live beside
their implementation in `src/frontend/` and require a new probe before they
are generalized.

It also recognizes the standard `D3D12SDKVersion` and `D3D12SDKPath`
application exports and implements `ID3D12SDKConfiguration`. These entry paths
activate the packaged `d3d12core.dll` bridge without application-name routing
or a behavior environment variable.

## Build

```sh
TOOLCHAIN=/path/to/llvm-mingw \
WINEBUILD=/path/to/winebuild \
./build.sh
```
