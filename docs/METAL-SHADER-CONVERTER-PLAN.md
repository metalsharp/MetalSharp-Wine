# Plan: Metal Shader Converter to cut shader compile time

Status: Phases 2 (compute), 2b (SM 6.6 heap indexing) and 3 (vertex and pixel shaders) implemented 2026-10-27, on by default
(METALSHARP_MSC=0 turns it off); see MSC-BRIDGE-CONTRACT.md. Shaders that reach typed buffers stay on SPIR-V (the
converter can't express typed views that don't start 16-byte aligned). Checked natively and with VKD3D-Proton's D3D12
tests; not yet measured in a game.

## Why

On the VKD3D-Proton lane, God of War Ragnarök stalls whenever a new scene needs new pipelines. The game creates
pipelines on demand on one thread and waits for each one. In the 13:00 traced run:

| Measure | Value |
|---|---|
| Pipeline states created | 27,471, almost all on one thread |
| Total pipeline creation time | 977 s |
| Spent in Metal compiles | 828 s (85%) |
| Metal source (MSL) library compiles | 5,184, totalling 492 s |
| Render pipeline (back-end) compiles | 321 s |
| SPIR-V and MSL generation, cache lookups | about 150 s |
| Worst freeze | 28.4 s, during 1,034 compiles |

The VKD3D-Proton lane converts each shader DXIL → SPIR-V (dxil-spirv) → MSL text (SPIRV-Cross), and then Metal
compiles the MSL text. That last front-end compile is the largest single cost.

## What Metal Shader Converter offers

Metal Shader Converter 4.0 beta 3 ships in the Game Porting Toolkit 4.0 beta 3 disk image as a command-line tool,
`libmetalirconverter.dylib`, and public headers (`metal_irconverter/*.h`). Its C API takes DXIL and a D3D12 root
signature and produces a Metal library (`IRCompilerAllocCompileAndLink`, `IRObjectGetMetalLibBinary`). It also
covers geometry and tessellation emulation, ray tracing, dual-source blending and stage-in vertex fetch.

**License:** section 2B grants a license "to install, use, test, and distribute the Dynamic Libraries for the
sole purpose of shader conversion". Calling the library from our code and shipping the dylib inside
MetalSharp-Wine.app is allowed. Section 2E forbids reverse engineering it, so we only use the documented API.
D3DMetal (the evaluation environment) is a separate license that forbids reverse engineering; it is out of scope.

## Measurements (God of War shaders, M4, one at a time)

| Path | Largest shader (150 KB DXIL) | Mid-size shader (95 KB DXIL) |
|---|---|---|
| Metal Shader Converter CLI, DXIL → metallib, incl. process start | 0.05 s | 0.03 s |
| Mullion airconv CLI, DXIL → metallib, incl. process start | 0.09 s | 0.04 s |
| VKD3D lane today: MSL compile of MoltenVK's dumped source (`xcrun metal -c`) | 1.72 s (900 KB MSL) | 0.40–0.52 s |

So a direct converter is 10–35 times faster than the MSL front-end on large shaders. Both direct converters still
leave Metal's back-end compile at pipeline creation (God of War: 321 s of render pipeline compiles in the run).

## Where it fits

### Mullion (D3D11/D3D12 straight to Metal)

Mullion already converts DXBC and DXIL straight to Metal libraries with its own converter (src/airconv,
`dxil_converter.cpp`, about 8,800 lines), at about the same speed as Apple's. Its binding model follows D3D12
root signatures, which is what Metal Shader Converter expects. So on Mullion, Apple's converter is mainly a
**correctness backstop**: shaders our converter can't handle or gets wrong could fall back to it.

### VKD3D-Proton + MoltenVK

This is where the speed-up is, and the hard part. MoltenVK only sees SPIR-V and binds resources through Vulkan
descriptor sets and argument buffers, while Metal Shader Converter's output expects its own top-level argument
buffer built from the D3D12 root signature. Using it means building that layout in MoltenVK from what
VKD3D-Proton binds, a large new binding path.

## Plan

### Phase 0: quick checks, no new code (1 session)

1. **Run God of War on the Mullion lane** with tracing, and compare its hitching with the VKD3D run. Mullion
   already skips the MSL step, so this measures the gain available without new work.
2. **Rerun God of War on the VKD3D lane with shader dumps off.** Dumps force VKD3D-Proton to retranslate every
   shader on each launch. The run gives the baseline every later change is measured against.
3. **Count constant-specialized libraries,** which MoltenVK always compiles from MSL and never caches compiled.
   Add their count to the shader trace.

### Phase 1: Mullion backstop (small)

1. Load `libmetalirconverter.dylib` at runtime with `dlopen`, so Mullion builds and runs without it.
2. When airconv fails on a DXIL shader, or for shaders on a deny list, convert with Metal Shader Converter
   using the pipeline's root signature, and log which converter produced each shader.
3. Run the existing Mullion shader corpus (`build/traces/rdr2-shaders` and the GoW dumps) through both
   converters. Compare failures, output size and conversion time.

Done when Mullion falls back cleanly, and the corpus comparison shows which converter wins where.

### Phase 2: VKD3D lane prototype, compute shaders only (medium)

Compute pipelines dominate God of War's worst freezes (the 20 s freeze was 34 compute pipelines), and their
bindings are the simplest.

1. VKD3D-Proton passes the original DXIL and its root signature mapping to MoltenVK through a private
   `pNext` structure on `VkComputePipelineCreateInfo`.
2. MoltenVK converts it with Metal Shader Converter, and binds a top-level argument buffer built from the
   descriptor sets and push constants VKD3D-Proton already provides.
3. Anything unsupported falls back to today's SPIR-V path, so nothing breaks.
4. Measure God of War's compute pipeline creation time and worst freezes against the Phase 0 baseline.

Done when God of War runs correctly with converted compute shaders and the compute share of its freezes drops.

### Phase 2b: SM 6.6 heap indexing for compute (medium)

In God of War, 530 of its 557 compute pipelines use `ResourceDescriptorHeap[]` / `SamplerDescriptorHeap[]`
(root signature flags `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` / `SAMPLER_HEAP_DIRECTLY_INDEXED`), so version 1 of the
bridge sends them to SPIR-V. The converter reads those heaps from Metal buffers 0 and 1. VKD3D-Proton already
keeps both as 24-byte shadow heaps, so:

1. VKD3D-Proton appends the two shadow heaps' device addresses after the top-level buffer in the push constants.
2. MoltenVK looks up the `MTLBuffer` for each address and binds it at index 0 and 1 (offset included).
3. Contract version 2 records the extra 16 bytes; the flags check in VKD3D-Proton is dropped.

### Phase 3: graphics pipelines (large)

Extend the bridge to vertex and pixel shaders first: God of War has 3,915 pixel and 513 vertex shaders, the bulk
of its 5,184 MSL compiles. Input layouts become stage-in attributes matching MoltenVK's vertex descriptor, the
top-level buffer is bound for both stages, and render target formats and blending stay on MoltenVK's existing
pipeline descriptor. Pipelines with hull, domain or geometry shaders stay on SPIR-V (the converter emulates those
with mesh shaders, a separate project). Dual-source blending comes after.

### Not on the converter

- **DXBC shaders** (Shader Model 5.x, 16 in God of War): the converter takes only DXIL. Microsoft's open-source
  `dxilconv` (in DirectXShaderCompiler) translates DXBC to DXIL and could feed it later.
- **Ray tracing and local root signatures:** the converter supports DXR, but VKD3D-Proton's ray tracing runs
  through MoltenVK's Vulkan ray tracing; out of scope.
- **The back-end compile:** the converter removes the MSL front-end compile only. Metal still compiles each
  pipeline state (God of War: 321 s of render pipeline compiles); the binary archive is what helps there.

### Independent of the converter

These cut the remaining back-end cost on either lane:

- Keep caches warm: avoid cache-key changes, and save the pipeline cache outside the pipeline-creation lock
  (next time `MVKPipeline.mm` changes anyway).
- Make the binary archive useful: it is now filtered, deferred while the game compiles, and saved at exit.
  Measure whether it saves back-end time on a second launch.

## Risks

- **Binding model.** Metal Shader Converter's argument buffer layout is fixed by its documentation, and
  MoltenVK's is its own. Phase 2 is mostly this bridging work.
- **Beta software.** 4.0 is a beta; pin the version in the build, and keep the existing paths as fallback.
- **Headers.** The headers (`metal_irconverter/*.h`, `metal_irconverter_runtime.h`) carry Apple's Apache
  License 2.0 notice, so they may be committed with their license file and notices kept.
- **Shipping the dylib.** Ship `libmetalirconverter.dylib` unmodified, with Apple's own signature
  ("Software Signing"): exclude it from our re-signing, since the license forbids modifying it. Ship its
  Acknowledgements file alongside. Only the dylib may be distributed, not the command-line tool or docs.
- **Platform.** The license limits use to Apple-branded devices, which matches MetalSharp's target.
