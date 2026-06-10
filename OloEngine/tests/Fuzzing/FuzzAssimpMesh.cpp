// =============================================================================
// FuzzAssimpMesh.cpp
//
// libFuzzer harness for Assimp's in-memory importer. Mesh assets (FBX, glTF
// binary + JSON, OBJ, COLLADA, etc.) are routinely downloaded, redistributed,
// and opened by the editor — a malformed file must never crash the engine or
// leak memory. Assimp has a long CVE history (heap overflows in MDL, MD2,
// LWO, IFC, X, BVH parsers among others); we drive `ReadFileFromMemory` with
// arbitrary bytes so ASan / UBSan observe the full import pipeline.
//
// The harness tries the same byte buffer with several extension hints
// ("", "obj", "gltf", "fbx") so a single corpus file drives multiple parser
// code paths. All standard post-processing flags are disabled — we want to
// fuzz the importers, not the triangulator.
// =============================================================================

#include "OloEnginePCH.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cstddef>
#include <cstdint>

namespace
{
    // Format hints passed as the `pHint` parameter. Empty string lets Assimp
    // sniff the content; explicit hints force specific parsers.
    const char* const kHints[] = { "", "obj", "gltf", "fbx", "dae", "blend" };

    constexpr unsigned int kFlags = 0; // No post-processing — fuzz the parser.
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // Keep the buffer small enough that the driver does not OOM under ASan
    // (which inflates working-set ~3x). A fresh Assimp::Importer is created
    // per hint so one hint's parser state cannot contaminate the next.
    if (size == 0 || size > static_cast<size_t>(4 * 1024 * 1024))
        return 0;

    for (const char* hint : kHints)
    {
        Assimp::Importer importer;
        // Return value is owned by the importer; we intentionally ignore it.
        (void)importer.ReadFileFromMemory(data, size, kFlags, hint);
    }
    return 0;
}
