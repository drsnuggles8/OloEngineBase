#include "OloEnginePCH.h"
#include "OloEngine/Audio/DSP/Spatializer/VBAP.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/glm.hpp>

namespace OloEngine::Audio::DSP
{
    // Speaker direction vectors for each miniaudio channel position
    constexpr glm::vec3 g_maChannelDirections[MA_CHANNEL_POSITION_COUNT] = {
        { 0.0f,     0.0f,    -1.0f    },  // MA_CHANNEL_NONE
        { 0.0f,     0.0f,    -1.0f    },  // MA_CHANNEL_MONO
        {-0.7071f,  0.0f,    -0.7071f },  // MA_CHANNEL_FRONT_LEFT
        {+0.7071f,  0.0f,    -0.7071f },  // MA_CHANNEL_FRONT_RIGHT
        { 0.0f,     0.0f,    -1.0f    },  // MA_CHANNEL_FRONT_CENTER
        { 0.0f,     0.0f,    -1.0f    },  // MA_CHANNEL_LFE
        {-0.7071f,  0.0f,    +0.7071f },  // MA_CHANNEL_BACK_LEFT
        {+0.7071f,  0.0f,    +0.7071f },  // MA_CHANNEL_BACK_RIGHT
        {-0.3162f,  0.0f,    -0.9487f },  // MA_CHANNEL_FRONT_LEFT_CENTER
        {+0.3162f,  0.0f,    -0.9487f },  // MA_CHANNEL_FRONT_RIGHT_CENTER
        { 0.0f,     0.0f,    +1.0f    },  // MA_CHANNEL_BACK_CENTER
        {-1.0f,     0.0f,     0.0f    },  // MA_CHANNEL_SIDE_LEFT
        {+1.0f,     0.0f,     0.0f    },  // MA_CHANNEL_SIDE_RIGHT
        { 0.0f,    +1.0f,     0.0f    },  // MA_CHANNEL_TOP_CENTER
        {-0.5774f, +0.5774f, -0.5774f },  // MA_CHANNEL_TOP_FRONT_LEFT
        { 0.0f,    +0.7071f, -0.7071f },  // MA_CHANNEL_TOP_FRONT_CENTER
        {+0.5774f, +0.5774f, -0.5774f },  // MA_CHANNEL_TOP_FRONT_RIGHT
        {-0.5774f, +0.5774f, +0.5774f },  // MA_CHANNEL_TOP_BACK_LEFT
        { 0.0f,    +0.7071f, +0.7071f },  // MA_CHANNEL_TOP_BACK_CENTER
        {+0.5774f, +0.5774f, +0.5774f },  // MA_CHANNEL_TOP_BACK_RIGHT
        { 0.0f,     0.0f,    -1.0f    },  // MA_CHANNEL_AUX_0 .. AUX_31
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
        { 0.0f,     0.0f,    -1.0f    },
    };

    //==============================================================================
    // Constructors

    VBAP::VirtualSource::VirtualSource(u32 numberOfOutputChannels, float angle)
        : Angle(angle)
        , Channel(0)
        , NumOutputChannels(numberOfOutputChannels)
        , Gains{}
    {
    }

    VBAP::ChannelGroup::ChannelGroup() = default;

    VBAP::ChannelGroup::ChannelGroup(u32 numberOfOutputChannels, u32 channel, float angle)
        : Angle(angle)
        , Channel(channel)
    {
        u32 gainSmoothTimeInFrames = 360; // ~7.5ms at 48kHz
        ma_gainer_config gainerConfig = ma_gainer_config_init(numberOfOutputChannels, gainSmoothTimeInFrames);
        ma_result result = ma_gainer_init(&gainerConfig, nullptr, &Gainer);
        OLO_CORE_ASSERT(result == MA_SUCCESS);
        (void)result;

        for (u32 iChannel = 0; iChannel < Gainer.config.channels; ++iChannel)
        {
            Gainer.pOldGains[iChannel] = 0.0f;
            Gainer.pNewGains[iChannel] = 0.0f;
        }
    }

    VBAP::ChannelGroup::ChannelGroup(const ChannelGroup& other)
        : Angle(other.Angle)
        , Channel(other.Channel)
        , VirtualSourceIDs(other.VirtualSourceIDs)
        , Gainer(other.Gainer)
    {
    }

    VBAP::ChannelGroup& VBAP::ChannelGroup::operator=(const ChannelGroup& other)
    {
        if (this != &other)
        {
            Angle = other.Angle;
            Channel = other.Channel;
            VirtualSourceIDs = other.VirtualSourceIDs;
            Gainer = other.Gainer;
        }
        return *this;
    }

    //==============================================================================
    // VBAP Algorithm

    bool VBAP::InitVBAP(VBAPData* vbap, u32 numOfInputs, u32 numOfOutputs, const ma_channel* sourceChannelMap,
                         const ma_channel* outputChannelMap)
    {
        OLO_CORE_ASSERT(numOfInputs > 0 && numOfOutputs > 0);

        ClearVBAP(vbap);

        vbap->spPos.resize(numOfOutputs);

        // Store speaker vectors from channel map
        for (u32 i = 0; i < numOfOutputs; i++)
        {
            const auto vector = g_maChannelDirections[outputChannelMap[i]];
            vbap->spPos[i] = glm::vec2{ vector.x, vector.z };
        }

        // Sort speakers by angle
        SortChannelLayout(vbap->spPos, vbap->spPosSorted);

        // Pre-compute inverse matrices for each sorted speaker pair
        for (sizet i = 0; i < vbap->spPosSorted.size(); i++)
        {
            auto& [p, idx] = vbap->spPosSorted.at(i);
            auto& [p2, idx2] = vbap->spPosSorted.at((i + 1) % vbap->spPosSorted.size());

            const glm::mat2 L(glm::vec2(p.x, p.y), glm::vec2(p2.x, p2.y));
            vbap->InverseMats.push_back(glm::inverse(L));
        }

        // Virtual source distribution
        const u32 numOfVirtualSources = numOfOutputs * 2u;
        const u32 vsPerChannel = numOfVirtualSources / numOfInputs;
        const float vsAngle = 360.0f / static_cast<float>(numOfVirtualSources);
        const float inputPlaneSection = 360.0f / static_cast<float>(numOfInputs);

        vbap->ChannelGroups.resize(numOfInputs);
        vbap->VirtualSources.reserve(numOfVirtualSources);
        i32 vsID = 0;

        // Sort input channels by their rotation vectors
        std::vector<glm::vec2> inputChannelsUnsorted;
        inputChannelsUnsorted.reserve(numOfInputs);
        for (u32 i = 0; i < numOfInputs; i++)
        {
            const auto vec = g_maChannelDirections[sourceChannelMap[i]];
            inputChannelsUnsorted.push_back(glm::vec2{ vec.x, vec.z });
        }

        std::vector<std::pair<glm::vec2, u32>> inputChannelsSorted;
        inputChannelsSorted.reserve(numOfInputs);
        SortChannelLayout(inputChannelsUnsorted, inputChannelsSorted);

        const bool evenChannelCount = numOfInputs % 2 == 0;
        float sourceAngle = -0.5f * vsAngle;

        for (u32 i = 0; i < numOfInputs; i++)
        {
            auto& [pos, id] = inputChannelsSorted[i];

            // Assign centre angle of the equal section of the input plane
            float channelAngle = inputPlaneSection * static_cast<float>(i);
            if (evenChannelCount)
            {
                channelAngle += 0.5f * inputPlaneSection;
            }

            if (channelAngle > 180.0f)
            {
                channelAngle -= 360.0f;
            }

            ChannelGroup channelGroup(numOfOutputs, id, channelAngle);
            channelGroup.VirtualSourceIDs.reserve(vsPerChannel);

            for (u32 vi = 0; vi < vsPerChannel; vi++)
            {
                sourceAngle += vsAngle;
                float sa = sourceAngle;

                if (sa > 180.0f)
                {
                    sa = -(sa - 180.0f);
                }

                VirtualSource virtualSource(numOfOutputs, sa);
                virtualSource.Channel = id;

                vbap->VirtualSources.push_back(virtualSource);
                channelGroup.VirtualSourceIDs.push_back(vsID);
                vsID++;
            }

            vbap->ChannelGroups[id] = channelGroup;
        }

        return true;
    }

    void VBAP::ClearVBAP(VBAPData* vbap)
    {
        OLO_CORE_ASSERT(vbap != nullptr);
        *vbap = VBAPData();
    }

    void VBAP::UpdateVBAP(VBAPData* vbap, const PositionUpdateData& positionData,
                           const ma_channel_converter& converter, bool isInitialPosition)
    {
        OLO_CORE_ASSERT(!vbap->VirtualSources.empty());

        const float panAngle = positionData.PanAngle;
        const float spread = positionData.Spread;
        const float focus = positionData.Focus;
        const float gainAttenuation = positionData.GainAttenuation;

        // Calculate channel panning gains
        for (auto& vs : vbap->VirtualSources)
        {
            float channelGroupAngle = vbap->ChannelGroups[vs.Channel].Angle;
            const float vsAngle = glm::radians(std::lerp(vs.Angle, channelGroupAngle, focus) * spread);
            ProcessVBAP(vbap, panAngle + vsAngle, vs.Gains);
        }

        // Normalize and apply gains
        for (auto& chg : vbap->ChannelGroups)
        {
            ChannelGains gainsLocal{};
            ma_silence_pcm_frames(gainsLocal.data(), MA_MAX_CHANNELS, ma_format_f32, 1);

            for (auto& vsID : chg.VirtualSourceIDs)
            {
                auto& vsGains = vbap->VirtualSources[static_cast<sizet>(vsID)].Gains;
                for (sizet i = 0; i < gainsLocal.size(); ++i)
                {
                    gainsLocal[i] += vsGains[i] * vsGains[i];
                }
            }

            // RMS normalization with distance attenuation
            const u32 numVirtualSources = static_cast<u32>(chg.VirtualSourceIDs.size());
            std::for_each(gainsLocal.begin(), gainsLocal.end(),
                          [numVirtualSources, gainAttenuation](float& g)
                          { g = std::sqrt(g / static_cast<float>(numVirtualSources)) * gainAttenuation; });

            // Convert intermediate surround channel gains to output format
            ChannelGains gainsOut = ConvertChannelGains(gainsLocal, converter);
            chg.Gains.Write(gainsOut);

            // For initial position, skip interpolation
            if (isInitialPosition)
            {
                std::memcpy(chg.Gainer.pOldGains, gainsOut.data(), sizeof(float) * chg.Gainer.config.channels);
            }
        }
    }

    void VBAP::SortChannelLayout(const std::vector<glm::vec2>& speakerVectors,
                                  std::vector<std::pair<glm::vec2, u32>>& sorted)
    {
        for (sizet i = 0; i < speakerVectors.size(); i++)
        {
            sorted.push_back({ speakerVectors.at(i), static_cast<u32>(i) });
        }

        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& p1, const auto& p2)
                  {
                      float ang1 = VectorAngle({ p1.first.x, p1.first.y });
                      float ang2 = VectorAngle({ p2.first.x, p2.first.y });
                      constexpr float circle = glm::radians(360.0f);
                      if (ang1 < 0.0f)
                          ang1 = circle + ang1;
                      if (ang2 < 0.0f)
                          ang2 = circle + ang2;
                      return ang1 < ang2;
                  });
    }

    ChannelGains VBAP::ConvertChannelGains(const ChannelGains& channelGainsIn, const ma_channel_converter& converter)
    {
        ChannelGains channelGainsOut{};

        float* pFramesOutF32 = channelGainsOut.data();
        const float* pFramesInF32 = channelGainsIn.data();

        switch (converter.conversionPath)
        {
            case ma_channel_conversion_path_passthrough:
                std::memcpy(pFramesOutF32, pFramesInF32, sizeof(float) * channelGainsIn.size());
                break;

            case ma_channel_conversion_path_shuffle:
                for (ma_uint32 iChannelIn = 0; iChannelIn < converter.channelsIn; ++iChannelIn)
                {
                    pFramesOutF32[converter.pShuffleTable[iChannelIn]] = pFramesInF32[iChannelIn];
                }
                break;

            case ma_channel_conversion_path_mono_in:
                for (ma_uint32 iChannel = 0; iChannel < converter.channelsOut; ++iChannel)
                {
                    pFramesOutF32[iChannel] = pFramesInF32[0];
                }
                break;

            case ma_channel_conversion_path_mono_out:
                pFramesOutF32[0] = (pFramesInF32[0] + pFramesInF32[1]) * 0.5f;
                break;

            case ma_channel_conversion_path_weights:
            default:
                for (ma_uint32 iChannelIn = 0; iChannelIn < converter.channelsIn; ++iChannelIn)
                {
                    for (ma_uint32 iChannelOut = 0; iChannelOut < converter.channelsOut; ++iChannelOut)
                    {
                        const auto w = converter.weights.f32[iChannelIn][iChannelOut];
                        pFramesOutF32[iChannelOut] += pFramesInF32[iChannelIn] * w;
                    }
                }
                break;
        }

        return channelGainsOut;
    }

    void VBAP::ProcessVBAP(const VBAPData* vbap, float azimuthRadians, ChannelGains& gains)
    {
        std::pair<i32, i32> speakers;
        std::pair<float, float> speakerGains;
        if (FindActiveArch(vbap, azimuthRadians, speakers, speakerGains))
        {
            std::fill(gains.begin(), gains.end(), 0.0f);
            gains[static_cast<sizet>(speakers.first)] = speakerGains.first;
            gains[static_cast<sizet>(speakers.second)] = speakerGains.second;
        }
    }

    bool VBAP::FindActiveArch(const VBAPData* vbap, float azimuthRadians, std::pair<i32, i32>& outSpeakerIndexes,
                               std::pair<float, float>& outGains)
    {
        float powr;
        float ref = 0.0f;
        float g1 = 0.0f;
        float g2 = 0.0f;
        i32 idx1 = -1;
        i32 idx2 = -1;

        const float x = sinf(azimuthRadians);
        const float y = -cosf(azimuthRadians);

        const auto& sortedPositions = vbap->spPosSorted;
        const auto& inverseMats = vbap->InverseMats;
        const auto size = sortedPositions.size();

        for (sizet i = 0; i < size; i++)
        {
            const auto& [pos, ind] = sortedPositions[i];
            const auto& [pos2, ind2] = sortedPositions[(i + 1) % size];

            const glm::mat2& Li = inverseMats[i];
            float gi1 = Li[0][0] * x + Li[1][0] * y;
            float gi2 = Li[0][1] * x + Li[1][1] * y;

            // Both gains positive → this is the active speaker pair
            if (gi1 > -1e-6f && gi2 > -1e-6f)
            {
                powr = sqrtf(gi1 * gi1 + gi2 * gi2);
                if (powr > ref)
                {
                    ref = powr;
                    idx1 = static_cast<i32>(ind);
                    idx2 = static_cast<i32>(ind2);

                    gi1 = std::max(gi1, 0.0f);
                    gi2 = std::max(gi2, 0.0f);

                    // Normalize gains to RMS
                    g1 = gi1 / powr;
                    g2 = gi2 / powr;
                }
            }
        }

        outSpeakerIndexes = { idx1, idx2 };
        outGains = { g1, g2 };

        return ref != 0.0f;
    }

} // namespace OloEngine::Audio::DSP
