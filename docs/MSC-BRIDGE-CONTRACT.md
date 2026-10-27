# Metal Shader Converter bridge: the VKD3D-Proton ↔ MoltenVK contract

Version 2, 2026-10-27. Scope: D3D12 compute pipelines, SM 6.6 heap indexing, and graphics pipelines of a vertex and a
pixel shader. Version 1 (compute only, no heaps) blobs still work. Hull, domain, geometry, mesh and amplification
shaders stay on the SPIR-V path.

VKD3D-Proton is a Windows DLL that reaches MoltenVK only through standard Vulkan calls passed through Wine's
`winevulkan` thunks. Custom `pNext` structures are dropped there, so the bridge uses only:

1. a shader module whose `pCode` is a custom blob (Wine passes `pCode` through untouched on 64-bit),
2. `VK_NVX_image_view_handle` (`vkGetImageViewHandle64NVX`), which Wine already thunks, to read Metal
   resource IDs,
3. ordinary push constants, which carry the converter's top-level argument buffer.

Metal Shader Converter (Apple, 4.0 beta 3) converts DXIL to a Metal library. Its headers (Apache 2.0) are in
MoltenVK's patches (`MoltenVK/MoltenVK/MetalShaderConverter/`). The dylib is Apple-signed and shipped unmodified:
VKD3D-Proton-MacOS's `scripts/fetch-metal-shaderconverter.sh` fetches it, with its license and acknowledgements,
from that repository's `deps-metal-shaderconverter-4.0b3` release, and `scripts/package.sh` puts it in the app.

## Switch

The bridge is on by default in VKD3D-Proton. `METALSHARP_MSC=0` in the environment turns it off, and then nothing
changes.
MoltenVK always supports the blob and the NVX query. It needs no switch: it only acts on blobs.

## 1. The shader module blob

Little-endian, every field `uint32_t` unless noted, every section 4-byte aligned.

```
struct MSCBlobHeader {
    uint32_t magic;            // 0x4D534358 ("XCSM" in memory): never SPIR-V's 0x07230203 or MoltenVK's MSL magics
    uint32_t version;          // 2 (version 1 ends after rootSigSize with a reserved 0, and is compute only)
    uint32_t stage;            // 0 = compute, 1 = vertex, 2 = fragment (pixel)
    uint32_t dxilOffset;       // from the start of the blob
    uint32_t dxilSize;         // bytes of the DXIL container (the "DXBC"-magic container holding a DXIL part)
    uint32_t rootSigOffset;    // the flattened root signature below
    uint32_t rootSigSize;
    uint32_t heapsOffset;      // byte offset in the push constants of the heap addresses (section 2), or 0xFFFFFFFF
    uint32_t vertexInputOffset;// vertex blobs: the input table below, else 0
    uint32_t vertexInputCount;
};
```

### Vertex input table (vertex blobs)

One 64-byte entry per vertex shader input that isn't a system value:

```
struct MSCVertexInput {
    uint32_t location;         // the Vulkan vertex attribute location VKD3D-Proton gives it (its signature register)
    char     name[60];         // semantic name and index, NUL-terminated: "POSITION0", "TEXCOORD3"
};
```

The converter names its stage-in inputs by semantic in lower case (`position0`) and numbers them itself. MoltenVK
matches the names without regard to case and moves each Vulkan attribute to stage-in attribute
`11 + the converter's index` (`kIRStageInAttributeStartIndex`).

`codeSize` passed to `vkCreateShaderModule` is the whole blob size, a multiple of 4 (pad with zeros).

### Flattened root signature (version 1.1 semantics; 1.0 is converted to 1.1 by VKD3D-Proton)

```
struct MSCRootSig {
    uint32_t numParameters;
    uint32_t numRanges;          // total descriptor ranges across all tables
    uint32_t numStaticSamplers;
    uint32_t flags;              // D3D12_ROOT_SIGNATURE_FLAGS
    MSCRootParam params[numParameters];
    MSCRange     ranges[numRanges];
    MSCStaticSampler samplers[numStaticSamplers];
};
struct MSCRootParam {            // 7 x uint32
    uint32_t type;               // D3D12_ROOT_PARAMETER_TYPE: 0 table, 1 32-bit constants, 2 CBV, 3 SRV, 4 UAV
    uint32_t visibility;         // D3D12_SHADER_VISIBILITY
    uint32_t a;                  // constants/descriptor: ShaderRegister; table: index of its first range in ranges[]
    uint32_t b;                  // constants/descriptor: RegisterSpace; table: number of ranges
    uint32_t c;                  // constants: Num32BitValues; descriptor: D3D12_ROOT_DESCRIPTOR_FLAGS; table: 0
    uint32_t reserved0, reserved1;
};
struct MSCRange {                // 6 x uint32 (D3D12_DESCRIPTOR_RANGE1)
    uint32_t rangeType;          // D3D12_DESCRIPTOR_RANGE_TYPE: 0 SRV, 1 UAV, 2 CBV, 3 SAMPLER
    uint32_t numDescriptors;     // UINT32_MAX for unbounded
    uint32_t baseShaderRegister;
    uint32_t registerSpace;
    uint32_t flags;              // D3D12_DESCRIPTOR_RANGE_FLAGS
    uint32_t offsetInDescriptorsFromTableStart; // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND = UINT32_MAX allowed
};
struct MSCStaticSampler {        // 13 x 4 bytes (D3D12_STATIC_SAMPLER_DESC)
    uint32_t filter, addressU, addressV, addressW;
    float    mipLODBias;
    uint32_t maxAnisotropy, comparisonFunc, borderColor;
    float    minLOD, maxLOD;
    uint32_t shaderRegister, registerSpace, shaderVisibility;
};
```

## 2. The top-level argument buffer (built by VKD3D-Proton, pushed as push constants)

Layout, per the converter's manual ("Explicit layout via Root Signatures"): walk the root parameters in order;
32-bit constants are 4-byte aligned, `4 * Num32BitValues` bytes; root CBV/SRV/UAV and descriptor tables are
8-byte aligned, 8 bytes. The total is rounded up to 8.

Values:
- **Constants:** the current root constants.
- **Root CBV/SRV/UAV:** the D3D12 GPU virtual address (on MoltenVK a buffer device address is
  `MTLBuffer.gpuAddress + offset`, so it is directly usable).
- **Descriptor table:** `shadowHeapVA + tableStartIndex * 24`, where `tableStartIndex` is the heap index of
  the table's GPU handle, and `shadowHeapVA` is the shadow heap (section 3) of the bound CBV_SRV_UAV heap, or of
  the bound sampler heap for sampler tables.

Before every `vkCmdDispatch` / `vkCmdDispatchIndirect` with a bridge pipeline bound, VKD3D-Proton calls
`vkCmdPushConstants(cmd, mscLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, bytes)` with the whole buffer.
`mscLayout` is a `VkPipelineLayout` with no descriptor sets and one compute push-constant range `[0, size)`;
the bridge pipeline is created with it. `size <= 4096`; larger root signatures stay on SPIR-V.

**Heaps (SM 6.6).** When the root signature has `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` or
`SAMPLER_HEAP_DIRECTLY_INDEXED`, two `uint64_t` follow the top-level buffer at `heapsOffset` (its size, already a
multiple of 8): the shadow heap address (section 3) of the bound CBV_SRV_UAV heap, then of the bound sampler heap,
0 for none. They count toward the 4096 bytes.

**Graphics.** Before every draw with a bridge pipeline bound, VKD3D-Proton pushes the same bytes, built from the
graphics bindings, with `mscGraphicsLayout`: one push-constant range `[0, size)` for the vertex and fragment stages,
which share it.

**MoltenVK** binds, for a bridge pipeline, exactly the layout's push-constant bytes at Metal buffer index 2
(`kIRArgumentBufferBindPoint`) of each stage, the heaps' buffers at 0 (`kIRDescriptorHeapBindPoint`) and 1
(`kIRSamplerHeapBindPoint`) when `heapsOffset` is set, vertex buffers as usual (they must be above index 5), and
nothing else. Its SPIR-V vertex shaders flip Y (`MVK_CONFIG_SHADER_CONVERSION_FLIP_VERTEX_Y`); converted ones
don't, so for them MoltenVK flips the Metal viewport instead (origin y + height, negative height), which puts
every vertex in the same place with the same winding.

## 3. Shadow descriptor heaps (owned by VKD3D-Proton)

For each shader-visible descriptor heap, and in CPU memory for non-shader-visible heaps (so copies work), an array
of 24-byte entries, one per heap index:

```
struct MSCDescriptor { uint64_t gpuVA; uint64_t textureViewID; uint64_t metadata; };
```

Shader-visible heaps keep it in a host-visible, coherent `VkBuffer` with
`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, persistently mapped; `shadowHeapVA` is its device address. Every
descriptor write path, the null template and every copy update it:

| Descriptor | gpuVA | textureViewID | metadata |
|---|---|---|---|
| Texture SRV | 0 | handle of its `VkImageView` | min LOD clamp, as float bits in the low 32 bits |
| Texture UAV | 0 | handle of its `VkImageView` | 0 |
| Buffer SRV/UAV (raw, structured, typed) | exact VA of the first byte | handle of its texel `VkBufferView`, or 0 if none | `range` (low 32 bits) \| `(element_offset & 0xff) << 32` \| `1 << 63` if typed |
| Raw or structured buffer UAV without a counter | exact VA of the first byte | 0 (`IncrementCounter()` must not reach the data) | `range` (low 32 bits) |
| Buffer UAV **with a counter** (append/consume) | exact VA of the first byte | handle of an `R32_UINT` storage texel `VkBufferView` over the counter resource, starting at `CounterOffsetInBytes` aligned down to 16 | `range` (low 32 bits) \| `((CounterOffsetInBytes % 16) / 4) << 32`; typed bit 0 (as Apple's `IRRuntimeCreateAppendBufferView` does) |
| CBV | exact VA | 0 | `SizeInBytes` (low 32 bits) |
| Sampler | handle of its `VkSampler` (**in gpuVA**, as Apple's `IRDescriptorTableSetSampler` does) | 0 | 0 (MoltenVK already applies the LOD bias in the sampler) |
| Null | 0 | 0 | 0 |

**Typed buffers can't go through the bridge.** Measured with converter 4.0 beta 3 (MetalSharp-Wine's native
bridge test): a typed buffer (`Buffer<T>`, `RWBuffer<T>`, SRV or UAV) is read and written only through its texel view,
from element 0, and `GetDimensions` returns the view's width. The metadata's texel offset and size are ignored,
whatever the encoding, with or without bounds checks. Metal texel views must start 16-byte aligned, so a D3D12 typed
view whose first element is not 16-byte aligned can't be expressed, and a descriptor can change after a command list
is recorded. So VKD3D-Proton keeps every PSO whose shaders reach a typed buffer (from a table or the heap) on SPIR-V
(section 5); typed entries in the shadow heaps are written as the table says but no bridge shader reads them.

## 4. Handles: `vkGetImageViewHandle64NVX` (MoltenVK implements `VK_NVX_image_view_handle`)

`VkImageViewHandleInfoNVX { sType, pNext, imageView, descriptorType, sampler }` returns the Metal resource ID
(`gpuResourceID._impl`):

- `descriptorType` SAMPLED_IMAGE / STORAGE_IMAGE / COMBINED_IMAGE_SAMPLER / INPUT_ATTACHMENT: `imageView` is a
  `VkImageView`; returns its `MTLTexture`'s resource ID (plane 0).
- `descriptorType` SAMPLER: returns `sampler`'s `MTLSamplerState` resource ID.
- `descriptorType` UNIFORM_TEXEL_BUFFER / STORAGE_TEXEL_BUFFER: **private to this bridge**, `imageView` carries a
  `VkBufferView` handle; returns the resource ID of that buffer view's `MTLTexture`.
- Anything else, or a null handle: 0.

`vkGetImageViewHandleNVX` returns the low 32 bits. `vkGetImageViewAddressNVX` returns
`VK_ERROR_OUT_OF_HOST_MEMORY` (not used).

## 5. When the bridge is used (VKD3D-Proton decides; any "no" falls back to SPIR-V)

- `METALSHARP_MSC` is not `0`, the device enabled `VK_NVX_image_view_handle`, and buffer device address is available.
- The root signature isn't a local one, and its top-level buffer plus heap addresses fit in 4096 bytes.
- No shader of the PSO reaches a typed buffer (`VKD3D_SHADER_META_FLAG_USES_TYPED_BUFFER`, set by the DXIL to
  SPIR-V compile from the resource kinds dxil-spirv reports, heap-indexed ones included). Compute PSOs therefore
  compile their SPIR-V first, cached like any other.
- Compute: the shader is DXIL (not DXBC), and `vkCreateComputePipelines` with the blob succeeds.
- Graphics: exactly a vertex and a pixel shader, both DXIL; no stream output, dual-source blending or view
  instancing; every vertex input's semantic fits the table; the PSO's pipeline is created up front (not a pipeline
  library, known topology), and creating it with the blobs succeeds. Otherwise the PSO is created with SPIR-V as
  before.
- MoltenVK fails a bridge pipeline (any error) when the converter can't be loaded, conversion fails, the device
  has no residency set, or (graphics) the pipeline needs what only SPIR-V shaders can emulate: multiview, the
  emulated depth bounds test, emulated reversed depth, draw parameters, or vertex buffers at Metal indexes 0 to 5.

ExecuteIndirect (`EXT_dgc`) writes push constants on the GPU in VKD3D-Proton's own layout, so it can't feed a
bridge pipeline. For indirect command signatures with a bridge PSO, VKD3D-Proton uses a SPIR-V pipeline for the
same PSO: compute ones are created lazily on first need from a copy of the DXIL kept with the PSO; graphics ones
are pipeline variants keyed `msc_spirv`, and a graphics bridge PSO is never "fully dynamic", so it keeps what a
variant needs.

## 6. MoltenVK side

- `MVKShaderModule`: a `case` for the magic copies the blob, hashes it into `_key`, and marks the module; it keeps
  the stage, the heaps offset and a vertex blob's input table.
- `MVKComputePipeline`: for such a module, `dlopen` `libmetalirconverter.dylib` (next to `libMoltenVK.dylib`
  first, then `/usr/local/lib`), build the `IRRootSignature` from the flattened one, convert
  (`IRCompilerAllocCompileAndLink`), create the `MTLLibrary` with `newLibraryWithData`, take the entry point name
  and threadgroup size from the converter's compute reflection, leave the stage's bind script empty, and compile
  the pipeline state with the existing compiler (binary archive included). Never send such a module to
  SPIRV-Cross or the pipeline cache.
- `MVKGraphicsPipeline`: a converted vertex module and an optional converted fragment module make a render pipeline
  (`newMSCRenderPipelineDescriptor`) from the converter's libraries, with the vertex descriptor built as for SPIR-V
  and then moved to the stage-in attributes, and the usual fragment outputs.
- Dispatch: in `prepareComputeDispatch`, a bridge pipeline binds the push-constant bytes at index 2, and the heaps
  at 0 and 1, instead of running `bindVulkanComputeToMetalCompute`. Draws do the same for both stages
  (`bindMSCArguments`), and bind vertex buffers.
- `METALSHARP_TRACE=shader` logs each conversion with its time; failures are errors with the converter's message.
