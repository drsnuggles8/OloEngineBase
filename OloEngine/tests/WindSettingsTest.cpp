#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

using namespace OloEngine;

TEST(WindSettings, DefaultsAreReasonable)
{
    WindSettings wind;

    EXPECT_FALSE(wind.Enabled);
    EXPECT_GT(glm::length(wind.Direction), 0.9f);
    EXPECT_LT(glm::length(wind.Direction), 1.1f);
    EXPECT_GT(wind.Speed, 0.0f);
    EXPECT_GE(wind.GustStrength, 0.0f);
    EXPECT_LE(wind.GustStrength, 1.0f);
    EXPECT_GT(wind.GustFrequency, 0.0f);
    EXPECT_GE(wind.TurbulenceIntensity, 0.0f);
    EXPECT_GT(wind.TurbulenceScale, 0.0f);
    EXPECT_GT(wind.GridWorldSize, 0.0f);
    EXPECT_GE(wind.GridResolution, 32u);
    EXPECT_LE(wind.GridResolution, 256u);
}

TEST(WindUBOData, SizeIs64Bytes)
{
    // 4 vec4s = 4 * 16 = 64 bytes
    EXPECT_EQ(WindUBOData::GetSize(), 64u);
    EXPECT_EQ(sizeof(WindUBOData), 64u);
}

TEST(WindUBOData, FieldOffsets_Std140Compatible)
{
    EXPECT_EQ(offsetof(WindUBOData, DirectionAndSpeed), 0u);
    EXPECT_EQ(offsetof(WindUBOData, GustAndTurbulence), 16u);
    EXPECT_EQ(offsetof(WindUBOData, GridMinAndSize), 32u);
    EXPECT_EQ(offsetof(WindUBOData, TimeAndFlags), 48u);
}

TEST(WindUBOData, DefaultsMatchSettings)
{
    WindSettings settings;
    WindUBOData gpu;

    EXPECT_FLOAT_EQ(gpu.DirectionAndSpeed.x, settings.Direction.x);
    EXPECT_FLOAT_EQ(gpu.DirectionAndSpeed.y, settings.Direction.y);
    EXPECT_FLOAT_EQ(gpu.DirectionAndSpeed.z, settings.Direction.z);
    EXPECT_FLOAT_EQ(gpu.DirectionAndSpeed.w, settings.Speed);

    EXPECT_FLOAT_EQ(gpu.GustAndTurbulence.x, settings.GustStrength);
    EXPECT_FLOAT_EQ(gpu.GustAndTurbulence.y, settings.GustFrequency);
    EXPECT_FLOAT_EQ(gpu.GustAndTurbulence.z, settings.TurbulenceIntensity);
    EXPECT_FLOAT_EQ(gpu.GustAndTurbulence.w, settings.TurbulenceScale);

    EXPECT_FLOAT_EQ(gpu.TimeAndFlags.y, 0.0f);
}

TEST(ShaderBindingLayout, WindBindingsExist)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_WIND, 15u);
    EXPECT_EQ(ShaderBindingLayout::TEX_WIND_FIELD, 29u);

    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_CAMERA);
    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_SNOW);
    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_SSS);
    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_SHADOW);
    EXPECT_NE(ShaderBindingLayout::UBO_WIND, ShaderBindingLayout::UBO_USER_0);
}

// =============================================================================
// SnowSettings — WindDriftFactor
// =============================================================================

TEST(SnowSettings, WindDriftFactorDefaultIsZero)
{
    SnowSettings snow;
    EXPECT_FLOAT_EQ(snow.WindDriftFactor, 0.0f);
}

TEST(SnowSettings, WindDriftFactorInUBOFlags)
{
    // WindDriftFactor should be packed into Flags.y of SnowUBOData
    SnowUBOData gpu;
    // Default: Flags = vec4(0) → Flags.y = 0 (no drift)
    EXPECT_FLOAT_EQ(gpu.Flags.y, 0.0f);
}
