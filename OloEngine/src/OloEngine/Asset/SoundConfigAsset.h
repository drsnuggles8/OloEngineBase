#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Audio/AudioSource.h" // AudioSourceConfig + AttenuationModelType

namespace OloEngine
{
    /**
     * @brief Reusable, serializable preset of per-sound playback parameters.
     *
     * A SoundConfigAsset captures an @ref AudioSourceConfig (volume / pitch
     * multipliers, play-on-awake, looping, spatialization, attenuation model +
     * rolloff, gain & distance bounds, cone, doppler, VBAP spread/focus and the
     * DSP filter sends) so the same playback tuning can be authored once and
     * shared across many AudioSource entities.
     *
     * Named `SoundConfigAsset` (not `SoundConfig`) on purpose: `OloEngine::SoundConfig`
     * is already taken by the unrelated runtime playback struct in
     * Audio/SoundGraph/SoundGraphSound.h. This follows the engine's `XxxAsset`
     * naming convention (MeshColliderAsset, MaterialAsset, ...). The serializer,
     * AssetType enum value and `.olosoundc` extension all keep the plain
     * `SoundConfig` name.
     *
     * The preset is stored verbatim as an @ref AudioSourceConfig. An
     * AudioSourceComponent references one via its SoundConfigHandle; when set,
     * Scene::InitAudioRuntime stamps the preset into the component's Config at
     * playback (effectively AudioSource::SetConfig(asset->m_Config)).
     */
    class SoundConfigAsset : public Asset
    {
      public:
        AudioSourceConfig m_Config{};

        SoundConfigAsset() = default;
        explicit SoundConfigAsset(const AudioSourceConfig& config) noexcept
            : m_Config(config)
        {
        }

        static constexpr AssetType GetStaticType() noexcept
        {
            return AssetType::SoundConfig;
        }
        AssetType GetAssetType() const noexcept override
        {
            return GetStaticType();
        }
    };
} // namespace OloEngine
