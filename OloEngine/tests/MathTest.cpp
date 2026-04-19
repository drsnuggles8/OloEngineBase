#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"

#include <glm/gtx/quaternion.hpp>

TEST(MathTest, DecomposeTransformTest)
{
    glm::vec3 translation;
    glm::vec3 rotation;
    glm::vec3 scale;

    const glm::mat4 transform = { glm::vec4(-107374176.f, -107374176.f, -107374176.f, -107374176.f),
                                  glm::vec4(-107374176.f, -107374176.f, -107374176.f, -107374176.f),
                                  glm::vec4(-107374176.f, -107374176.f, -107374176.f, -107374176.f),
                                  glm::vec4(-107374176.f, -107374176.f, -107374176.f, -107374176.f) };

    ASSERT_TRUE(OloEngine::Math::DecomposeTransform(transform, translation, rotation, scale));

    // Euler angle decomposition is not unique — different platforms may return
    // equivalent but numerically different angles (differing by multiples of pi).
    // Verify the angles reconstruct the same rotation matrix instead.
    constexpr float kEps = 1e-4f;
    const glm::mat4 expected = glm::toMat4(glm::quat(glm::vec3(-2.35619450f, 0.615479708f, -2.35619450f)));
    const glm::mat4 actual = glm::toMat4(glm::quat(rotation));
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(actual[col][row], expected[col][row], kEps)
                << "Mismatch at [" << col << "][" << row << "]";
        }
    }
    EXPECT_EQ(translation, glm::vec3(-107374176.f, -107374176.f, -107374176.f));
    EXPECT_EQ(scale, glm::vec3(185977536.f, 185977536.f, 185977536.f));
}
