#include "OloEnginePCH.h"
#include "Spatializer.h"

#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Audio/SampleBufferOperations.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine::Audio::DSP
{
    using ChannelGains = std::array<float, MA_MAX_CHANNELS>;

    //==============================================================================
    // Distance / cone / doppler attenuation helpers

    static float ProcessDistanceAttenuation(AttenuationModelType model, float distance, float minDistance,
                                            float maxDistance, float rolloff)
    {
        switch (model)
        {
            case AttenuationModelType::Inverse:
            {
                if (minDistance >= maxDistance)
                {
                    return 1.0f;
                }
                return minDistance / (minDistance + rolloff * (std::clamp(distance, minDistance, maxDistance) - minDistance));
            }
            case AttenuationModelType::Linear:
            {
                if (minDistance >= maxDistance)
                {
                    return 1.0f;
                }
                return 1.0f - rolloff * (std::clamp(distance, minDistance, maxDistance) - minDistance) / (maxDistance - minDistance);
            }
            case AttenuationModelType::Exponential:
            {
                if (minDistance >= maxDistance)
                {
                    return 1.0f;
                }
                return std::pow(std::clamp(distance, minDistance, maxDistance) / minDistance, -rolloff);
            }
            case AttenuationModelType::None:
            default:
                return 1.0f;
        }
    }

    static float ProcessAngularAttenuation(glm::vec3 dirSourceToListener, glm::vec3 dirSource,
                                           float coneInnerAngleInRadians, float coneOuterAngleInRadians,
                                           float coneOuterGain)
    {
        if (coneInnerAngleInRadians < 6.283185f)
        {
            float cutoffInner = cosf(coneInnerAngleInRadians * 0.5f);
            float cutoffOuter = cosf(coneOuterAngleInRadians * 0.5f);
            float d = glm::dot(dirSourceToListener, dirSource);

            if (d > cutoffInner)
            {
                return 1.0f;
            }
            if (d > cutoffOuter)
            {
                return std::lerp(coneOuterGain, 1.0f, (d - cutoffOuter) / (cutoffInner - cutoffOuter));
            }
            return coneOuterGain;
        }
        return 1.0f;
    }

    static float ProcessDopplerPitch(glm::vec3 relativePosition, glm::vec3 sourceVelocity, glm::vec3 listenVelocity,
                                     float speedOfSound, float dopplerFactor)
    {
        float len = glm::length(relativePosition);
        if (len == 0.0f)
        {
            return 1.0f;
        }

        float vls = glm::dot(relativePosition, listenVelocity) / len;
        float vss = glm::dot(relativePosition, sourceVelocity) / len;

        vls = std::min(vls, speedOfSound / dopplerFactor);
        vss = std::min(vss, speedOfSound / dopplerFactor);

        return (speedOfSound - dopplerFactor * vls) / (speedOfSound - dopplerFactor * vss);
    }

    //==============================================================================
    // Audio callback

    void spatializer_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
                                             float** ppFramesOut, ma_uint32* pFrameCountOut)
    {
        const float* pFramesIn_0 = ppFramesIn[0];
        float* pFramesOut_0 = ppFramesOut[0];
        u32 frameCount = *pFrameCountIn;

        auto* node = static_cast<Spatializer::spatializer_node*>(pNode);
        VBAPData& vbap = *node->vbap;
        auto* pEngineNode = node->targetEngineNode;

        const u32 channelsIn = node->channelsIn;
        const u32 channelsOut = node->channelsOut;

        // Retrieve updated panning gain values
        if (node->IsDirty())
        {
            for (auto& chg : vbap.ChannelGroups)
            {
                const auto& gains = chg.Gains.Read();

                if (!Audio::SampleBufferOperations::ContentMatches(chg.Gainer.pNewGains, gains.data(),
                                                                   chg.Gainer.config.channels, 1))
                {
                    std::memcpy(chg.Gainer.pNewGains, gains.data(), sizeof(float) * chg.Gainer.config.channels);
                }
            }

            pEngineNode->spatializer.dopplerPitch = node->DopplerPitch;
        }

        // Apply panning to output
        ma_silence_pcm_frames(pFramesOut_0, frameCount, ma_format_f32, channelsOut);

        const u32 numSourceInputs = static_cast<u32>(vbap.ChannelGroups.size());
        for (u32 iChannelIn = 0; iChannelIn < numSourceInputs; ++iChannelIn)
        {
            auto& gainer = vbap.ChannelGroups[iChannelIn].Gainer;

            for (u32 iChannelOut = 0; iChannelOut < channelsOut; ++iChannelOut)
            {
                const float startGain = gainer.pOldGains[iChannelOut];
                const float endGain = gainer.pNewGains[iChannelOut];

                Audio::SampleBufferOperations::AddAndApplyGainRamp(pFramesOut_0, pFramesIn_0, iChannelOut, iChannelIn,
                                                                   channelsOut, channelsIn, frameCount, startGain,
                                                                   endGain);
                gainer.pOldGains[iChannelOut] = endGain;
            }
        }
    }

    //==============================================================================
    // Spatializer node vtable + helpers

    static const ma_node_vtable s_SpatializerNodeVtable = { spatializer_node_process_pcm_frames, nullptr, 1, 1, 0 };

    // Copy constructor deleted — spatializer_node owns a Scope<VBAPData> and
    // contains miniaudio node state that cannot be trivially deep-copied.

    Spatializer::~Spatializer()
    {
        Uninitialize();
    }

    bool Spatializer::Initialize(ma_engine* engine)
    {
        m_Engine = engine;
        return true;
    }

    void Spatializer::Uninitialize()
    {
        while (!m_Sources.empty())
        {
            auto it = m_Sources.begin();
            if (!ReleaseSource(it->first))
            {
                OLO_CORE_ASSERT(false, "Failed to uninitialize Spatializer Source! ID: {}", it->first);
                break;
            }
        }
        m_Engine = nullptr;
    }

    bool Spatializer::IsInitialized(u32 sourceID) const
    {
        auto it = m_Sources.find(sourceID);
        return it != m_Sources.end() && it->second.bInitialized;
    }

    float Spatializer::GetCurrentDistanceAttenuation(u32 sourceID) const
    {
        OLO_CORE_ASSERT(IsInitialized(sourceID));
        return m_Sources.at(sourceID).DistanceAttenuationFactor;
    }

    float Spatializer::GetCurrentConeAngleAttenuation(u32 sourceID) const
    {
        OLO_CORE_ASSERT(IsInitialized(sourceID));
        return m_Sources.at(sourceID).AngleAttenuationFactor;
    }

    float Spatializer::GetCurrentDistance(u32 sourceID) const
    {
        OLO_CORE_ASSERT(IsInitialized(sourceID));
        return m_Sources.at(sourceID).Distance;
    }

    //==============================================================================
    // Spread / Focus

    void Spatializer::SetSpread(u32 sourceID, float newSpread)
    {
        if (!IsInitialized(sourceID))
        {
            OLO_CORE_ERROR("[Spatializer] SetSpread({}, {}) - invalid sourceID.", sourceID, newSpread);
            return;
        }

        Source& source = m_Sources.at(sourceID);
        if (source.bSpreadFromSourceSize)
        {
            OLO_CORE_WARN("[Spatializer] Trying to set Spread while using SpreadFromSourceSize.");
            return;
        }

        source.Spread = std::clamp(newSpread, 0.0f, 1.0f);
        UpdateVBAP(source);
        FlagRealtimeForUpdate(source);
    }

    void Spatializer::SetFocus(u32 sourceID, float newFocus)
    {
        if (!IsInitialized(sourceID))
        {
            OLO_CORE_ERROR("[Spatializer] SetFocus({}, {}) - invalid sourceID.", sourceID, newFocus);
            return;
        }

        Source& source = m_Sources.at(sourceID);
        source.Focus = std::clamp(newFocus, 0.0f, 1.0f);
        UpdateVBAP(source);
        FlagRealtimeForUpdate(source);
    }

    //==============================================================================
    // Position update helpers

    float Spatializer::GetSpreadFromSourceSize(float sourceSize, float distance)
    {
        if (distance <= 0.0f)
        {
            return 1.0f;
        }
        float degreeSpread = glm::degrees(atanf((0.5f * sourceSize) / distance)) * 2.0f;
        return degreeSpread / 180.0f;
    }

    void Spatializer::UpdatePositionalData(Source& source, const ma_spatializer_listener* listener, glm::vec3 listenerVelocity)
    {
        auto* pEngineNode = source.SpatializerNode.targetEngineNode;
        const float distance = source.Distance;
        const glm::vec3 positionRelative = source.PositionRelative;
        const glm::vec3 position = source.TransformData.Position;
        const glm::vec3 relativeDir = source.RelativeDir;
        const glm::vec3 velocity = source.Velocity;

        // Distance attenuation
        float gainAttenuation = ProcessDistanceAttenuation(source.AttenuationModel, source.Distance, source.MinDistance,
                                                           source.MaxDistance, source.RollOff);
        source.DistanceAttenuationFactor = gainAttenuation;

        // Cone angle attenuation
        if (distance > 0.0f)
        {
            float angleAttenuation = ProcessAngularAttenuation(glm::vec3(0.0f, 0.0f, -1.0f), relativeDir, source.ConeInnerAngle,
                                                               source.ConeOuterAngle, source.ConeOuterGain);

            if (listener != nullptr && listener->config.coneInnerAngleInRadians < 6.283185f)
            {
                glm::vec3 listenerDirection = (listener->config.handedness == ma_handedness_right)
                                                  ? glm::vec3(0.0f, 0.0f, -1.0f)
                                                  : glm::vec3(0.0f, 0.0f, +1.0f);

                angleAttenuation *= ProcessAngularAttenuation(
                    listenerDirection, glm::normalize(positionRelative), listener->config.coneInnerAngleInRadians,
                    listener->config.coneOuterAngleInRadians, listener->config.coneOuterGain);
            }

            gainAttenuation *= angleAttenuation;
            source.AngleAttenuationFactor = angleAttenuation;
        }

        gainAttenuation = std::clamp(gainAttenuation, source.MinGain, source.MaxGain);

        // Store clamped combined attenuation for VBAP
        source.DistanceAttenuationFactor = gainAttenuation;
        source.AngleAttenuationFactor = 1.0f; // Already folded into clamped gainAttenuation

        glm::vec3 listenerVel = listenerVelocity;

        // Doppler effect
        if (source.DopplerFactor > 0.0f && listener != nullptr)
        {
            ma_vec3f lpos = ma_spatializer_listener_get_position(listener);
            glm::vec3 lp(lpos.x, lpos.y, lpos.z);
            source.SpatializerNode.DopplerPitch = ProcessDopplerPitch(lp - position, velocity, listenerVel, SPEED_OF_SOUND, source.DopplerFactor);
            pEngineNode->spatializer.dopplerPitch = source.SpatializerNode.DopplerPitch;
        }
        else
        {
            source.SpatializerNode.DopplerPitch = 1.0f;
        }
    }

    void Spatializer::UpdateVBAP(Source& source, bool isInitialPosition)
    {
        VBAP::PositionUpdateData updateData{ source.vbapAzimuth, source.Spread, source.Focus,
                                             source.DistanceAttenuationFactor * source.AngleAttenuationFactor };

        VBAP::UpdateVBAP(source.SpatializerNode.vbap.get(), updateData, source.Converter, isInitialPosition);
    }

    void Spatializer::FlagRealtimeForUpdate(Source& source)
    {
        source.SpatializerNode.fGainsUpToDate.clear();
    }

    //==============================================================================
    // Source management

    bool Spatializer::InitSource(u32 sourceID, ma_engine_node* nodeToInsertAfter, const AudioSourceConfig& config)
    {
        OLO_CORE_ASSERT(m_Sources.find(sourceID) == m_Sources.end());

        auto [it, success] = m_Sources.try_emplace(sourceID);
        auto& source = it->second;

        auto abortIfFailed = [&](ma_result result, const char* errorMessage)
        {
            if (result != MA_SUCCESS)
            {
                OLO_CORE_ASSERT(false, errorMessage);
                ReleaseSource(sourceID);
                return true;
            }
            return false;
        };

        auto* allocationCallbacks = &m_Engine->pResourceManager->config.allocationCallbacks;

        // Base node setup
        source.InternalChannelCount = 4; // Quad virtual speaker layout

        const u32 sourceChannels = nodeToInsertAfter->resampler.channels;
        const u32 sourceNodeChannels = ma_node_get_output_channels(nodeToInsertAfter, 0);

        const u32 numInputChannels[1]{ sourceNodeChannels };
        const u32 numOutputChannels[1]{ sourceNodeChannels };

        ma_node_config nodeConfig = ma_node_config_init();
        nodeConfig.vtable = &s_SpatializerNodeVtable;
        nodeConfig.pInputChannels = numInputChannels;
        nodeConfig.pOutputChannels = numOutputChannels;
        nodeConfig.initialState = ma_node_state_stopped;

        ma_result result = ma_node_init(&m_Engine->nodeGraph, &nodeConfig, allocationCallbacks,
                                        &source.SpatializerNode);
        if (abortIfFailed(result, "SpatializerNode init failed"))
        {
            return false;
        }

        source.SpatializerNode.channelsIn = numInputChannels[0];
        source.SpatializerNode.channelsOut = numOutputChannels[0];
        source.SpatializerNode.targetEngineNode = nodeToInsertAfter;

        // VBAP channel converter
        ma_channel_map_init_standard(ma_standard_channel_map_default, source.InternalChannelMap,
                                     sizeof(source.InternalChannelMap) / sizeof(source.InternalChannelMap[0]),
                                     source.InternalChannelCount);

        ma_channel_converter_config channelMapConfig = ma_channel_converter_config_init(
            ma_format_f32, source.InternalChannelCount, source.InternalChannelMap, numOutputChannels[0], nullptr,
            ma_channel_mix_mode_rectangular);

        result = ma_channel_converter_init(&channelMapConfig, nullptr, &source.Converter);
        if (abortIfFailed(result, "Channel converter init failed"))
        {
            return false;
        }

        ma_channel_map_init_standard(ma_standard_channel_map_default, source.SourceChannelMap,
                                     sizeof(source.SourceChannelMap) / sizeof(source.SourceChannelMap[0]),
                                     sourceChannels);

        // Config snapshot
        source.MinGain = config.MinGain;
        source.MaxGain = config.MaxGain;
        source.MinDistance = config.MinDistance;
        source.MaxDistance = config.MaxDistance;
        source.ConeInnerAngle = config.ConeInnerAngle;
        source.ConeOuterAngle = config.ConeOuterAngle;
        source.ConeOuterGain = config.ConeOuterGain;
        source.DopplerFactor = config.DopplerFactor;
        source.RollOff = config.RollOff;
        source.AttenuationModel = config.AttenuationModel;
        source.Spread = config.Spread;
        source.Focus = config.Focus;

        // Initialize VBAP
        source.SpatializerNode.vbap = CreateScope<VBAPData>();
        bool res = VBAP::InitVBAP(source.SpatializerNode.vbap.get(), sourceChannels, source.InternalChannelCount,
                                  source.SourceChannelMap, source.InternalChannelMap);
        if (abortIfFailed(res ? MA_SUCCESS : MA_INVALID_ARGS, "VBAP init failed"))
        {
            return false;
        }

        // Routing: insert spatializer between nodeToInsertAfter and its current output
        auto* output = nodeToInsertAfter->baseNode.pOutputBuses->pInputNode;
        ma_uint8 downstreamInputBus = nodeToInsertAfter->baseNode.pOutputBuses->inputNodeInputBusIndex;

        result = ma_node_attach_output_bus(&source.SpatializerNode, 0, output, downstreamInputBus);
        if (abortIfFailed(result, "Spatializer output attach failed"))
        {
            return false;
        }

        result = ma_node_attach_output_bus(nodeToInsertAfter, 0, &source.SpatializerNode, 0);
        if (abortIfFailed(result, "Spatializer input attach failed"))
        {
            return false;
        }

        source.SourceID = sourceID;
        source.bInitialized = true;
        source.DownstreamInputBus = downstreamInputBus;

        return true;
    }

    bool Spatializer::ReleaseSource(u32 sourceID)
    {
        auto sIt = m_Sources.find(sourceID);
        if (sIt == m_Sources.end())
        {
            return false;
        }

        Source& source = sIt->second;

        // Reattach input → output, bypassing spatializer
        if (source.bInitialized)
        {
            auto* output = source.SpatializerNode.base.pOutputBuses->pInputNode;
            auto* input = source.SpatializerNode.targetEngineNode;

            ma_result result = ma_node_attach_output_bus(input, 0, output, source.DownstreamInputBus);
            if (result != MA_SUCCESS)
            {
                OLO_CORE_ASSERT(false, "Node reattach failed during ReleaseSource");
            }
        }

        // Always clean up the node if it was initialized (vtable set)
        if (((ma_node_base*)&source.SpatializerNode)->vtable != nullptr)
        {
            ma_node_set_state(&source.SpatializerNode, ma_node_state_stopped);

            auto* allocationCallbacks = &m_Engine->pResourceManager->config.allocationCallbacks;
            if (!allocationCallbacks->onFree)
            {
                allocationCallbacks = nullptr;
            }
            ma_node_uninit(&source.SpatializerNode, allocationCallbacks);
            source.SpatializerNode.targetEngineNode = nullptr;
        }

        VBAP::ClearVBAP(source.SpatializerNode.vbap.get());

        source.bInitialPositionSet = false;
        source.bInitialized = false;

        m_Sources.erase(sourceID);
        return true;
    }

    void Spatializer::UpdateSourcePosition(u32 sourceID, const Audio::Transform& transform, glm::vec3 velocity)
    {
        auto sIt = m_Sources.find(sourceID);
        if (sIt == m_Sources.end())
        {
            return;
        }

        Source& source = sIt->second;
        if (!source.bInitialized)
        {
            OLO_CORE_ASSERT(false);
            return;
        }

        const auto& position = transform.Position;
        const auto& orientation = transform.Orientation;
        const auto& sourceUp = transform.Up;

        const auto& lp = m_ListenerTransform.Position;
        const auto& lr = m_ListenerTransform.Orientation;
        const auto& lup = m_ListenerTransform.Up;

        // Position of the source relative to listener
        const glm::mat4 lookatM = glm::lookAt(lp, lp + lr, lup);
        glm::vec3 relativePos = lookatM * glm::vec4(position, 1.0f);

        // Direction from source to listener in source's space
        const glm::mat4 lookatMR = glm::lookAt(position, position + orientation, sourceUp);
        glm::vec3 relativeDir = glm::normalize(lookatMR * glm::vec4(lp, 1.0f));

        const float distance = glm::length(relativePos);

        if (distance < 1e-6f)
        {
            // Source is essentially at the listener — use forward direction and minimal distance
            relativePos = glm::vec3(0.0f, 0.0f, -0.001f);
        }

        float azimuth = VectorAngle(glm::normalize(relativePos));

        source.Distance = distance;
        source.vbapAzimuth = azimuth;
        source.PositionRelative = relativePos;
        source.TransformData.Position = position;
        source.TransformData.Orientation = orientation;
        source.TransformData.Up = sourceUp;
        source.RelativeDir = relativeDir;
        source.Velocity = velocity;

        if (source.bSpreadFromSourceSize)
        {
            source.Spread = GetSpreadFromSourceSize(source.SourceSize, distance);
        }

        UpdatePositionalData(source, &m_Engine->listeners[0], m_ListenerVelocity);
        UpdateVBAP(source, !source.bInitialPositionSet);

        // Start audio callback after first position update (prevents volume spike)
        if (!source.bInitialPositionSet)
        {
            ma_node_set_state(&source.SpatializerNode, ma_node_state_started);
            source.bInitialPositionSet = true;
        }

        FlagRealtimeForUpdate(source);
    }

    void Spatializer::UpdateListener(const Audio::Transform& transform, glm::vec3 velocity)
    {
        m_ListenerTransform = transform;
        m_ListenerVelocity = velocity;

        const auto& lp = m_ListenerTransform.Position;
        const auto& lr = m_ListenerTransform.Orientation;
        const auto& lup = m_ListenerTransform.Up;

        // Recompute all source relative positions
        for (auto& [sourceID, source] : m_Sources)
        {
            const auto& sp = source.TransformData.Position;
            const auto& sr = source.TransformData.Orientation;
            const auto& sup = source.TransformData.Up;

            const glm::mat4 lookatM = glm::lookAt(lp, lp + lr, lup);
            glm::vec3 relativePos = lookatM * glm::vec4(sp, 1.0f);

            const glm::mat4 lookatMR = glm::lookAt(sp, sp + sr, sup);
            glm::vec3 relativeDir = glm::normalize(lookatMR * glm::vec4(lp, 1.0f));

            const float distance = glm::length(relativePos);

            if (distance < 1e-6f)
            {
                relativePos = glm::vec3(0.0f, 0.0f, -0.001f);
            }

            float azimuth = VectorAngle(glm::normalize(relativePos));

            source.Distance = distance;
            source.vbapAzimuth = azimuth;
            source.PositionRelative = relativePos;
            source.RelativeDir = relativeDir;

            if (source.bSpreadFromSourceSize)
            {
                source.Spread = GetSpreadFromSourceSize(source.SourceSize, distance);
            }
        }

        // Update attenuation + VBAP for all sources
        std::for_each(m_Sources.begin(), m_Sources.end(),
                      [&](std::pair<const u32, Source>& pair)
                      {
                          auto& source = pair.second;
                          UpdatePositionalData(source, &m_Engine->listeners[0], m_ListenerVelocity);
                          UpdateVBAP(source);
                          FlagRealtimeForUpdate(source);
                      });
    }

} // namespace OloEngine::Audio::DSP
