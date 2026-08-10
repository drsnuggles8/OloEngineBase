#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"

#include "OloEngine/Renderer/RendererAPI.h"

namespace OloEngine::RHI
{
    namespace
    {
        // clip' = F * clip with row1' = -row1, row2' = (row2 + row3) / 2.
        // glm is column-major: m[col][row], so F's row 2 lives in
        // F[2][2] (= 0.5) and F[3][2] (= 0.5).
        [[nodiscard]] constexpr glm::mat4 FullFlipMatrix()
        {
            glm::mat4 f(1.0f);
            f[1][1] = -1.0f;
            f[2][2] = 0.5f;
            f[3][2] = 0.5f;
            return f;
        }

        // F without the y row: z' = (z + w) / 2 only, for direction-addressed
        // captures (see AdjustCaptureProjectionForBackend).
        [[nodiscard]] constexpr glm::mat4 DepthOnlyFlipMatrix()
        {
            glm::mat4 f(1.0f);
            f[2][2] = 0.5f;
            f[3][2] = 0.5f;
            return f;
        }

        [[nodiscard]] constexpr glm::mat4 RowFlipMatrix()
        {
            glm::mat4 y(1.0f);
            y[1][1] = -1.0f;
            return y;
        }

        [[nodiscard]] bool BackendFlips()
        {
            return RendererAPI::GetAPI() == RendererAPI::API::Vulkan;
        }
    } // namespace

    glm::mat4 AdjustProjectionForBackend(const glm::mat4& projection)
    {
        return BackendFlips() ? FullFlipMatrix() * projection : projection;
    }

    glm::mat4 AdjustProjectionForShaderReconstruction(const glm::mat4& projection)
    {
        return BackendFlips() ? RowFlipMatrix() * projection : projection;
    }

    glm::mat4 AdjustedInverseForShaderReconstruction(const glm::mat4& forward)
    {
        // Recompute-from-flipped, never invert-then-flip: the inverse of the
        // adjusted matrix is what shader-side ndc math must see.
        return glm::inverse(AdjustProjectionForShaderReconstruction(forward));
    }

    glm::mat4 AdjustCaptureProjectionForBackend(const glm::mat4& projection)
    {
        // See the header: the z remap WITHOUT the y flip, so a
        // direction-addressed face bake stores GL-identical rows.
        return BackendFlips() ? DepthOnlyFlipMatrix() * projection : projection;
    }
} // namespace OloEngine::RHI
