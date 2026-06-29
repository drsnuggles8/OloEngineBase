#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Audio/AudioTransform.h"
#include "OloEngine/Audio/AudioSource.h"
#include "VBAP.h"

#include <glm/glm.hpp>

#include <atomic>
#include <unordered_map>

#include <miniaudio.h>

namespace OloEngine::Audio::DSP
{
    struct VBAPData;

    // 3D Sound Spatializer
    //
    // Manages per-source spatializer nodes that handle VBAP panning in the
    // audio callback. The audio callback is started in the "stopped" state
    // until an initial position is set, preventing volume spikes at playback
    // start.
    class Spatializer
    {
      public:
        Spatializer() = default;
        ~Spatializer();

        bool Initialize(ma_engine* engine);
        void Uninitialize();

        [[nodiscard("initialization state must be used")]] bool IsInitialized(u32 sourceID) const;
        [[nodiscard("attenuation value must be used")]] float GetCurrentDistanceAttenuation(u32 sourceID) const;
        [[nodiscard("attenuation value must be used")]] float GetCurrentConeAngleAttenuation(u32 sourceID) const;
        [[nodiscard("distance must be used")]] float GetCurrentDistance(u32 sourceID) const;

        // Spatialized Sources.
        //
        // Two flavours of upstream node can host a per-source spatializer node:
        //   * ma_engine_node — the ma_sound path (AudioSource). Carries a built-in
        //     ma_spatializer whose dopplerPitch we drive for the Doppler effect.
        //   * ma_node_base   — a bare custom node (e.g. SoundGraphSource's node, which
        //     attaches straight to the engine endpoint). There is no engine node to host
        //     Doppler, so Doppler is skipped for this flavour; VBAP panning + distance /
        //     cone attenuation are computed entirely inside the spatializer node and behave
        //     identically for both. See issue #424.
        bool InitSource(u32 sourceID, ma_engine_node* nodeToInsertAfter, const AudioSourceConfig& config);
        bool InitSource(u32 sourceID, ma_node_base* nodeToInsertAfter, const AudioSourceConfig& config);
        bool ReleaseSource(u32 sourceID);
        void UpdateSourcePosition(u32 sourceID, const Audio::Transform& position,
                                  glm::vec3 velocity = { 0.0f, 0.0f, 0.0f });
        void SetSpread(u32 sourceID, float newSpread);
        void SetFocus(u32 sourceID, float newFocus);

        void UpdateListener(const Audio::Transform& transform, glm::vec3 velocity = { 0.0f, 0.0f, 0.0f });

      private:
        struct Source;

        // Shared core for both InitSource overloads. `sourceNode` is the upstream node the
        // spatializer is inserted after (for the engine overload this is &engineNode->baseNode);
        // `sourceChannels` is the source's channel count for the VBAP source channel map;
        // `dopplerSink` is the ma_engine_node whose built-in spatializer receives the Doppler
        // pitch, or nullptr for a bare node (Doppler disabled).
        bool InitSourceInternal(u32 sourceID, ma_node_base* sourceNode, u32 sourceChannels,
                                ma_engine_node* dopplerSink, const AudioSourceConfig& config);

        static float GetSpreadFromSourceSize(float sourceSize, float distance);
        static void UpdatePositionalData(Source& source, const ma_spatializer_listener* listener, glm::vec3 listenerVelocity);
        static void UpdateVBAP(Source& source, bool isInitialPosition = false);
        static void FlagRealtimeForUpdate(Source& source);

      private:
        friend void spatializer_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn,
                                                        ma_uint32* pFrameCountIn, float** ppFramesOut,
                                                        ma_uint32* pFrameCountOut);

        ma_engine* m_Engine = nullptr;

        struct spatializer_node
        {
            ma_node_base base{};
            u32 channelsIn = 0;
            u32 channelsOut = 0;
            // Upstream node the spatializer was inserted after — used to re-route on release.
            // Stored as the generic node base so a bare custom node (SoundGraphSource) works
            // alongside an ma_engine_node.
            ma_node_base* attachedNode = nullptr;
            // ma_engine_node hosting the built-in spatializer we drive for Doppler pitch, or
            // nullptr for a bare node (no Doppler). See InitSource.
            ma_engine_node* targetEngineNode = nullptr;

            Scope<VBAPData> vbap;

            // Accessed from audio callback to set miniaudio's spatializer doppler pitch
            std::atomic<float> DopplerPitch{ 1.0f };

            std::atomic_flag fGainsUpToDate;
            bool IsDirty()
            {
                return !fGainsUpToDate.test_and_set();
            }

            spatializer_node() = default;
            spatializer_node(const spatializer_node&) = delete;
            spatializer_node& operator=(const spatializer_node&) = delete;
            spatializer_node(spatializer_node&&) = delete;
            spatializer_node& operator=(spatializer_node&&) = delete;
        };

        struct Source
        {
            // Static data
            u32 SourceID = 0;
            bool bInitialized = false;
            bool bInitialPositionSet = false;
            u32 InternalChannelCount = 4; // Virtual quad speaker layout

            spatializer_node SpatializerNode;
            ma_channel_converter Converter{};
            ma_channel InternalChannelMap[MA_MAX_CHANNELS]{};
            ma_channel SourceChannelMap[MA_MAX_CHANNELS]{};

            // Config snapshot (copied from AudioSourceConfig)
            float MinGain = 0.0f;
            float MaxGain = 1.0f;
            float MinDistance = 0.3f;
            float MaxDistance = 1000.0f;
            float ConeInnerAngle = glm::radians(360.0f);
            float ConeOuterAngle = glm::radians(360.0f);
            float ConeOuterGain = 0.0f;
            float DopplerFactor = 1.0f;
            float RollOff = 1.0f;
            AttenuationModelType AttenuationModel = AttenuationModelType::Inverse;
            bool bSpreadFromSourceSize = false;
            float SourceSize = 1.0f;

            // Dynamic data
            float Spread = 1.0f;
            float Focus = 1.0f;

            float vbapAzimuth = 0.0f;
            float Distance = 0.0f;
            float DistanceAttenuationFactor = 1.0f;
            float AngleAttenuationFactor = 1.0f;
            glm::vec3 PositionRelative{ 0.0f };
            Audio::Transform TransformData;
            glm::vec3 RelativeDir{ 0.0f, 0.0f, -1.0f };

            glm::vec3 Velocity{ 0.0f };

            // Original downstream input bus index, captured during InitSource
            ma_uint8 DownstreamInputBus = 0;
        };

        std::unordered_map<u32, Source> m_Sources;

        Audio::Transform m_ListenerTransform;
        glm::vec3 m_ListenerVelocity{ 0.0f };
    };

} // namespace OloEngine::Audio::DSP
