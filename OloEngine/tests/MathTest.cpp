#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

TEST(MathTest, DecomposeTransformTest)
{
    // Build a well-defined TRS transform with unambiguous decomposition
    const glm::vec3 expectedTranslation(10.0f, -5.0f, 3.0f);
    const glm::vec3 expectedRotation(0.3f, 0.7f, -0.4f);
    const glm::vec3 expectedScale(2.0f, 3.0f, 1.5f);

    const glm::mat4 transform = glm::translate(glm::mat4(1.0f), expectedTranslation) * glm::toMat4(glm::quat(expectedRotation)) * glm::scale(glm::mat4(1.0f), expectedScale);

    glm::vec3 translation;
    glm::vec3 rotation;
    glm::vec3 scale;

    ASSERT_TRUE(OloEngine::Math::DecomposeTransform(transform, translation, rotation, scale));

    constexpr float kEps = 1e-4f;

    for (int i = 0; i < 3; ++i)
    {
        EXPECT_NEAR(translation[i], expectedTranslation[i], kEps) << "Translation mismatch at [" << i << "]";
    }

    for (int i = 0; i < 3; ++i)
    {
        EXPECT_NEAR(scale[i], expectedScale[i], kEps) << "Scale mismatch at [" << i << "]";
    }

    // Euler angle decomposition is not unique — different platforms may return
    // equivalent but numerically different angles. Compare rotation matrices.
    const glm::mat4 expectedRot = glm::toMat4(glm::quat(expectedRotation));
    const glm::mat4 actualRot = glm::toMat4(glm::quat(rotation));
    for (int col = 0; col < 3; ++col)
    {
        for (int row = 0; row < 3; ++row)
        {
            EXPECT_NEAR(actualRot[col][row], expectedRot[col][row], kEps)
                << "Rotation matrix mismatch at [" << col << "][" << row << "]";
        }
    }
}
