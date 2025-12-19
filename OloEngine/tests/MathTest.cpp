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

    ASSERT_EQ(OloEngine::Math::DecomposeTransform(transform, translation, rotation, scale), true);
    ASSERT_EQ(rotation, glm::vec3(-2.35619450, 0.615479708, -2.35619450));
    ASSERT_EQ(translation, glm::vec3(-107374176.f, -107374176.f, -107374176.f));
    ASSERT_EQ(scale, glm::vec3(185977536., 185977536., 185977536.));
}
