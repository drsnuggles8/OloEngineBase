#pragma once

// =============================================================================
// GroomBindingCooker.h — the named cook of a groom/body pair. Issue #1249.
//
// One call, one deterministic buffer — the shape GroomCooker::CookToBytes
// establishes for .ologroom, and for the same reason: the editor's "Build
// binding" button, an offline cooker and the determinism test must all go
// through ONE entry point, or the thing the test pins is not the thing that
// ships.
//
// There is deliberately no canonicalisation step here, unlike GroomCooker.
// GroomBindingBuilder's output is already canonical by construction: records
// are emitted in curve order, and the only ordering decision in the whole build
// (which triangle wins a tie) is resolved on the triangle INDEX rather than on
// traversal order. A sort here would have nothing to do.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"

#include <string>
#include <vector>

namespace OloEngine
{
    class GroomAsset;

    namespace GroomBindingCooker
    {
        /// Encodes an already-built binding to the cooked .ologroombinding byte
        /// stream. Returns false with a named reason on failure.
        [[nodiscard]] bool CookToBytes(const GroomBindingAsset& binding, std::vector<u8>& outBytes,
                                       std::string& outReason);

        /**
         * @brief Build a binding from a groom and a bind-pose body, then cook it.
         *
         * `targetSourcePath` is recorded in the binding so a file on disk can
         * name the body it belongs to; it is project-relative or a bare file
         * name, never absolute, because an absolute path would make the cooked
         * bytes differ between two machines cooking the same input and
         * determinism is the contract.
         *
         * Returns false with a named reason and leaves both outputs untouched on
         * any failure.
         */
        [[nodiscard]] bool CookPair(const GroomAsset& groom, const GroomSurfaceView& target,
                                    const std::string& targetSourcePath,
                                    const GroomBindingBuildSettings& settings, std::vector<u8>& outBytes,
                                    Ref<GroomBindingAsset>& outBinding, GroomBindingBuildStats& outStats,
                                    std::string& outReason);
    } // namespace GroomBindingCooker
} // namespace OloEngine
