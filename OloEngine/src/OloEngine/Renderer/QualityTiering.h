#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <string_view>

namespace OloEngine
{
	enum class QualityPreset : u8
	{
		Low,
		Medium,
		High,
		Ultra,
		Custom
	};

	struct QualityTieringSettings
	{
		QualityPreset Preset = QualityPreset::High;

		// Shadow
		u32 ShadowResolution = 4096;
		f32 ShadowSoftness = 1.0f;
		bool ShadowEnabled = true;

		// AO
		AOTechnique AO = AOTechnique::GTAO;
		i32 SSAOSamples = 32;
		i32 GTAODenoisePasses = 4;
		f32 GTAORadius = 0.5f;

		// Post-process toggles
		bool BloomEnabled = true;
		i32 BloomIterations = 5;
		bool FXAAEnabled = true;
		bool DOFEnabled = false;
		bool MotionBlurEnabled = false;
		bool VignetteEnabled = true;
		bool ChromaticAberrationEnabled = false;
	};

	// Return the canonical preset values for a named tier (Custom returns High defaults).
	[[nodiscard]] QualityTieringSettings GetPresetSettings(QualityPreset preset);

	// Write tiering values into the runtime structs.
	void ApplyTieringToSettings(const QualityTieringSettings& tiering, PostProcessSettings& pp, ShadowSettings& shadow);

	// String conversions
	[[nodiscard]] std::string_view QualityPresetToString(QualityPreset preset);
	[[nodiscard]] QualityPreset QualityPresetFromString(std::string_view str);

} // namespace OloEngine
