# Metal Shader Converter bridge: the VKD3D-Proton ↔ MoltenVK contract

Version 1, 2026-10-27. Scope: **D3D12 compute pipelines only**. Graphics pipelines stay on the SPIR-V path.

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
    uint32_t version;          // 1
    uint32_t stage;            // 0 = compute (the only one in version 1)
    uint32_t dxilOffset;       // from the start of the blob
    uint32_t dxilSize;         // bytes of the DXIL container (the "DXBC"-magic container holding a DXIL part)
    uint32_t rootSigOffset;    // the flattened root signature below
    uint32_t rootSigSize;
    uint32_t reserved;         // 0
};
```

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

**MoltenVK** binds, for a bridge pipeline, exactly the layout's push-constant bytes at Metal buffer index 2
(`kIRArgumentBufferBindPoint`) with the encoder state's `bindBytes` wrapper, and binds nothing else.

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
| Buffer UAV **with a counter** (append/consume) | exact VA of the first byte | handle of an `R32_UINT` storage texel `VkBufferView` over the counter resource, starting at `CounterOffsetInBytes` aligned down to 16 | `range` (low 32 bits) \| `((CounterOffsetInBytes % 16) / 4) << 32`; typed bit 0 (as Apple's `IRRuntimeCreateAppendBufferView` does) |
| CBV | exact VA | 0 | `SizeInBytes` (low 32 bits) |
| Sampler | handle of its `VkSampler` (**in gpuVA**, as Apple's `IRDescriptorTableSetSampler` does) | 0 | 0 (MoltenVK already applies the LOD bias in the sampler) |
| Null | 0 | 0 | 0 |

`element_offset` is the padding, in elements, between the texel view's aligned start and the real start
(VKD3D-Proton's `offset_buffer[i].element_offset`). If it is over 255 the bridge can't express it: write the
entry anyway with 255 and log once.

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
- Compute PSO whose shader is DXIL (not DXBC), with no local root signature and no
  `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` / `SAMPLER_HEAP_DIRECTLY_INDEXED` root signature flags (SM 6.6 heap
  indexing is out of scope for version 1).
- The top-level buffer is at most 4096 bytes.
- `vkCreateComputePipelines` with the blob succeeds. MoltenVK fails it (any error) when the converter can't be
  loaded, conversion fails, or the device has no residency set.

ExecuteIndirect (`EXT_dgc`) writes push constants on the GPU in VKD3D-Proton's own layout, so it can't feed a
bridge pipeline: for indirect command signatures with a bridge PSO, VKD3D-Proton uses a SPIR-V pipeline for the
same PSO, created lazily on first need from a copy of the DXIL kept with the PSO.

## 6. MoltenVK side

- `MVKShaderModule`: a `case` for the magic copies the blob, hashes it into `_key`, and marks the module.
- `MVKComputePipeline`: for such a module, `dlopen` `libmetalirconverter.dylib` (next to `libMoltenVK.dylib`
  first, then `/usr/local/lib`), build the `IRRootSignature` from the flattened one, convert
  (`IRCompilerAllocCompileAndLink`), create the `MTLLibrary` with `newLibraryWithData`, take the entry point name
  and threadgroup size from the converter's compute reflection, leave the stage's bind script empty, and compile
  the pipeline state with the existing compiler (binary archive included). Never send such a module to
  SPIRV-Cross or the pipeline cache.
- Dispatch: in `prepareComputeDispatch`, a bridge pipeline binds the push-constant bytes at index 2 instead of
  running `bindVulkanComputeToMetalCompute`.
- `METALSHARP_TRACE=shader` logs each conversion with its time; failures are errors with the converter's message.
