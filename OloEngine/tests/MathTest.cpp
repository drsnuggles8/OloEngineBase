#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"

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

    // Use tolerant comparison — transcendental functions (asin, atan2, cos)
    // produce slightly different results across math libraries (MSVC vs glibc).
    constexpr float kEps = 1e-4f;
    EXPECT_NEAR(rotation.x, -2.35619450f, kEps);
    EXPECT_NEAR(rotation.y, 0.615479708f, kEps);
    EXPECT_NEAR(rotation.z, -2.35619450f, kEps);
    EXPECT_EQ(translation, glm::vec3(-107374176.f, -107374176.f, -107374176.f));
    EXPECT_EQ(scale, glm::vec3(185977536.f, 185977536.f, 185977536.f));
}
