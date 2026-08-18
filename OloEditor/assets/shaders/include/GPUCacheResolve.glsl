// =============================================================================
// GPUCacheResolve.glsl — shader-side residency lookup for GPUPagedCache (#704).
//
// The CPU side (OloEngine/Renderer/GPUCache/) keeps two GPU-readable
// structures when created with the HostMirrored backing:
//
//   * the object directory — GPUHashMap<u64, ObjectAllocation>: one flat
//     std430 array of { key, value } entries, linear-probed, power-of-two
//     capacity, key sentinels for empty/tombstone;
//   * the page-node table — u32 next-pointers, one per page, kInvalidPage
//     (0xFFFFFFFF) terminating each object's chain.
//
// With those two bound, a compute shader resolves "is object X resident, and
// which pages hold it?" entirely on the GPU — no CPU round trip. That is
// acceptance criterion 2 of issue #704, proven by
// tests/GPUCacheResolve_Probe.comp + GPUCacheShaderResolveTest.cpp.
//
// THE CONTRACT (all of it checked by that test):
//
// 1. The consumer declares the entry struct and both buffers BEFORE including
//    this file (GLSL includes are textual; binding points are the consumer's
//    business). The entry struct must mirror the C++ instantiation EXACTLY —
//    for GPUPagedCache<u64, Atom, LRUPolicy> that is:
//
//        struct OloGpuCacheEntry
//        {
//            uint64_t Key;               // GPUHashMap entry key
//            uint TotalElementCount;     // ObjectAllocation begins here
//            uint StartPage;
//            uint EndPage;
//            uint PolicyHandle;          // LRUPolicy<u64>::Handle (one u32)
//        };                              // std430: size 24, align 8
//
//    A policy with a different Handle size changes the stride — re-derive it.
//
// 2. `#extension GL_ARB_gpu_shader_int64 : require` at the top of the
//    consumer (extensions cannot live in an include). Keys are 64-bit, and
//    the hash below does 64-bit multiplies. Ask the driver for the extension
//    BEFORE creating the shader — a failed compute compile is a modal assert
//    dialog in Debug, not a return value (gpu-scan-compaction.md §6).
//
// 3. The hash and sentinels below must stay bit-identical to the C++ side:
//    the hash is the engine's canonical MurmurHash3 finalizer Hash::Hash64
//    (Core/Hash.h, reached through GPUHashMap::HashKey), the sentinels are
//    GPUHashMap::kEmptyKey / kTombstoneKey. GPUHashMapTest's
//    GlslResolveContractMatchesCppConstants pins this file against them.
//
// 4. Capacity is a power of two; the start slot is hash & (capacity - 1).
//
// Use OLO_GPU_CACHE_DEFINE_FIND to stamp out the probe function against your
// buffer declarations:
//
//     OLO_GPU_CACHE_DEFINE_FIND(OloGpuCacheFind, u_Entries)
//     ...
//     uint count, startPage, endPage;
//     if (OloGpuCacheFind(objectKey, count, startPage, endPage)) { ... }
//
// and walk the chain with the page-node array:
//
//     for (uint page = startPage; page != OLO_GPU_CACHE_INVALID_PAGE;
//          page = u_PageNodes[page]) { ... }
// =============================================================================

const uint64_t OLO_GPU_CACHE_EMPTY_KEY = 0xFFFFFFFFFFFFFFFFUL;
const uint64_t OLO_GPU_CACHE_TOMBSTONE_KEY = 0xFFFFFFFFFFFFFFFEUL;
const uint OLO_GPU_CACHE_INVALID_PAGE = 0xFFFFFFFFu;

// MurmurHash3 finalizer — bit-identical to Hash::Hash64 (Core/Hash.h), the
// hash GPUHashMap::HashKey uses for its start slot.
uint64_t OloGpuCacheHash64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdUL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53UL;
    x ^= x >> 33;
    return x;
}

uint OloGpuCacheStartSlot(uint64_t key, uint capacity)
{
    return uint(OloGpuCacheHash64(key) & uint64_t(capacity - 1u));
}

// Stamps out `bool FuncName(uint64_t key, out uint outTotalElementCount,
// out uint outStartPage, out uint outEndPage)` probing `EntriesArray`
// (an OloGpuCacheEntry[] as specified in the header comment). Linear probe:
// an empty slot ends the search; tombstones are skipped implicitly (their key
// matches nothing).
#define OLO_GPU_CACHE_DEFINE_FIND(FuncName, EntriesArray)                                          \
    bool FuncName(uint64_t key, out uint outTotalElementCount, out uint outStartPage,              \
                  out uint outEndPage)                                                             \
    {                                                                                              \
        uint capacity = uint(EntriesArray.length());                                               \
        uint start = OloGpuCacheStartSlot(key, capacity);                                          \
        for (uint probe = 0u; probe < capacity; ++probe)                                           \
        {                                                                                          \
            uint slot = (start + probe) & (capacity - 1u);                                         \
            if (EntriesArray[slot].Key == OLO_GPU_CACHE_EMPTY_KEY)                                 \
            {                                                                                      \
                return false;                                                                      \
            }                                                                                      \
            if (EntriesArray[slot].Key == key)                                                     \
            {                                                                                      \
                outTotalElementCount = EntriesArray[slot].TotalElementCount;                       \
                outStartPage = EntriesArray[slot].StartPage;                                       \
                outEndPage = EntriesArray[slot].EndPage;                                           \
                return true;                                                                       \
            }                                                                                      \
        }                                                                                          \
        return false;                                                                              \
    }
