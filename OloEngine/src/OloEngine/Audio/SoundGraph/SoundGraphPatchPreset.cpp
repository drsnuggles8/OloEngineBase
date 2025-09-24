#include "OloEnginePCH.h"
#include "SoundGraphPatchPreset.h"
#include "SoundGraphSound.h"

#include <algorithm>
#include <fstream>
#include <chrono>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// ParameterPatch Implementation
    
    void ParameterPatch::SetParameter(u32 parameterID, const ParameterValue& value)
    {
        Parameters[parameterID] = value;
        Timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
    }

    void ParameterPatch::RemoveParameter(u32 parameterID)
    {
        Parameters.erase(parameterID);
    }

    bool ParameterPatch::HasParameter(u32 parameterID) const
    {
        return Parameters.find(parameterID) != Parameters.end();
    }

    std::vector<u32> ParameterPatch::GetParameterIDs() const
    {
        std::vector<u32> ids;
        ids.reserve(Parameters.size());
        
        for (const auto& [id, value] : Parameters)
        {
            ids.push_back(id);
        }
        
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    void ParameterPatch::Clear()
    {
        Parameters.clear();
        Name.clear();
        Description.clear();
        Timestamp = 0.0;
    }

    //==============================================================================
    /// SoundGraphPatchPreset Implementation

    void SoundGraphPatchPreset::RegisterParameter(const ParameterDescriptor& descriptor)
    {
        m_ParameterDescriptors[descriptor.ID] = descriptor;
    }

    void SoundGraphPatchPreset::UnregisterParameter(u32 parameterID)
    {
        m_ParameterDescriptors.erase(parameterID);
        
        // Remove from all patches
        for (auto& [name, patch] : m_Patches)
        {
            patch.RemoveParameter(parameterID);
        }
    }

    bool SoundGraphPatchPreset::IsParameterRegistered(u32 parameterID) const
    {
        return m_ParameterDescriptors.find(parameterID) != m_ParameterDescriptors.end();
    }

    const ParameterDescriptor* SoundGraphPatchPreset::GetParameterDescriptor(u32 parameterID) const
    {
        auto it = m_ParameterDescriptors.find(parameterID);
        return it != m_ParameterDescriptors.end() ? &it->second : nullptr;
    }

    std::vector<ParameterDescriptor> SoundGraphPatchPreset::GetAllParameterDescriptors() const
    {
        std::vector<ParameterDescriptor> descriptors;
        descriptors.reserve(m_ParameterDescriptors.size());
        
        for (const auto& [id, descriptor] : m_ParameterDescriptors)
        {
            descriptors.push_back(descriptor);
        }
        
        // Sort by ID for consistent ordering
        std::sort(descriptors.begin(), descriptors.end(), 
                  [](const ParameterDescriptor& a, const ParameterDescriptor& b) {
                      return a.ID < b.ID;
                  });
        
        return descriptors;
    }

    void SoundGraphPatchPreset::CreatePatch(const std::string& name, const std::string& description)
    {
        ParameterPatch patch;
        patch.Name = name;
        patch.Description = description;
        patch.Timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
        
        m_Patches[name] = patch;
    }

    void SoundGraphPatchPreset::DeletePatch(const std::string& name)
    {
        m_Patches.erase(name);
    }

    bool SoundGraphPatchPreset::HasPatch(const std::string& name) const
    {
        return m_Patches.find(name) != m_Patches.end();
    }

    ParameterPatch* SoundGraphPatchPreset::GetPatch(const std::string& name)
    {
        auto it = m_Patches.find(name);
        return it != m_Patches.end() ? &it->second : nullptr;
    }

    const ParameterPatch* SoundGraphPatchPreset::GetPatch(const std::string& name) const
    {
        auto it = m_Patches.find(name);
        return it != m_Patches.end() ? &it->second : nullptr;
    }

    std::vector<std::string> SoundGraphPatchPreset::GetPatchNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_Patches.size());
        
        for (const auto& [name, patch] : m_Patches)
        {
            names.push_back(name);
        }
        
        std::sort(names.begin(), names.end());
        return names;
    }

    void SoundGraphPatchPreset::ApplyPatch(const std::string& patchName, SoundGraphSound* target)
    {
        if (!target)
        {
            OLO_CORE_WARN("SoundGraphPatchPreset::ApplyPatch - target is null");
            return;
        }

        const ParameterPatch* patch = GetPatch(patchName);
        if (!patch)
        {
            OLO_CORE_WARN("SoundGraphPatchPreset::ApplyPatch - patch '{}' not found", patchName);
            return;
        }

        ApplyPatch(*patch, target);
    }

    void SoundGraphPatchPreset::ApplyPatch(const ParameterPatch& patch, SoundGraphSound* target)
    {
        if (!target)
        {
            OLO_CORE_WARN("SoundGraphPatchPreset::ApplyPatch - target is null");
            return;
        }

        for (const auto& [parameterID, value] : patch.Parameters)
        {
            // Apply the parameter value based on its type
            std::visit([target, parameterID](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, f32>)
                {
                    target->SetParameter(parameterID, v);
                }
                else if constexpr (std::is_same_v<T, i32>)
                {
                    target->SetParameter(parameterID, v);
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    target->SetParameter(parameterID, v);
                }
            }, value);
        }
    }

    void SoundGraphPatchPreset::CaptureStateToPatch(const std::string& patchName, SoundGraphSound* source)
    {
        if (!source)
        {
            OLO_CORE_WARN("SoundGraphPatchPreset::CaptureStateToPatch - source is null");
            return;
        }

        // Create or update patch
        ParameterPatch& patch = m_Patches[patchName];
        patch.Name = patchName;
        patch.Description = "Captured state from SoundGraphSound";
        patch.Clear();

        // Capture basic sound properties
        // These would be mapped to specific parameter IDs in a real implementation
        constexpr u32 VOLUME_PARAM_ID = 1;
        constexpr u32 PITCH_PARAM_ID = 2;
        constexpr u32 LOWPASS_PARAM_ID = 10;
        constexpr u32 HIGHPASS_PARAM_ID = 11;

        patch.SetParameter(VOLUME_PARAM_ID, source->GetVolume());
        patch.SetParameter(PITCH_PARAM_ID, source->GetPitch());
        // Additional parameters would be captured here based on the sound graph's parameter interface
    }

    bool SoundGraphPatchPreset::SaveToFile(const std::string& filePath) const
    {
        std::string jsonData = SerializeToJSON();
        
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("Failed to open file for writing: {}", filePath);
            return false;
        }

        file << jsonData;
        file.close();
        
        return true;
    }

    bool SoundGraphPatchPreset::LoadFromFile(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("Failed to open file for reading: {}", filePath);
            return false;
        }

        std::string jsonData((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();

        return DeserializeFromJSON(jsonData);
    }

    std::string SoundGraphPatchPreset::SerializeToJSON() const
    {
        // Simple JSON serialization (in a real implementation, use a JSON library like nlohmann::json)
        std::string json = "{\n";
        json += "  \"name\": \"" + m_PresetName + "\",\n";
        json += "  \"description\": \"" + m_PresetDescription + "\",\n";
        json += "  \"version\": \"" + m_Version + "\",\n";
        json += "  \"author\": \"" + m_Author + "\",\n";
        json += "  \"parameters\": [\n";

        bool firstParam = true;
        for (const auto& [id, descriptor] : m_ParameterDescriptors)
        {
            if (!firstParam) json += ",\n";
            firstParam = false;
            
            json += "    {\n";
            json += "      \"id\": " + std::to_string(descriptor.ID) + ",\n";
            json += "      \"name\": \"" + descriptor.Name + "\",\n";
            json += "      \"displayName\": \"" + descriptor.DisplayName + "\",\n";
            json += "      \"description\": \"" + descriptor.Description + "\",\n";
            json += "      \"defaultValue\": \"" + SerializeParameterValue(descriptor.DefaultValue) + "\",\n";
            json += "      \"units\": \"" + descriptor.Units + "\",\n";
            json += "      \"automatable\": " + (descriptor.IsAutomatable ? "true" : "false") + "\n";
            json += "    }";
        }

        json += "\n  ],\n";
        json += "  \"patches\": [\n";

        bool firstPatch = true;
        for (const auto& [name, patch] : m_Patches)
        {
            if (!firstPatch) json += ",\n";
            firstPatch = false;
            
            json += "    {\n";
            json += "      \"name\": \"" + patch.Name + "\",\n";
            json += "      \"description\": \"" + patch.Description + "\",\n";
            json += "      \"timestamp\": " + std::to_string(patch.Timestamp) + ",\n";
            json += "      \"parameters\": {\n";
            
            bool firstPatchParam = true;
            for (const auto& [paramId, value] : patch.Parameters)
            {
                if (!firstPatchParam) json += ",\n";
                firstPatchParam = false;
                
                json += "        \"" + std::to_string(paramId) + "\": \"" + SerializeParameterValue(value) + "\"";
            }
            
            json += "\n      }\n";
            json += "    }";
        }

        json += "\n  ]\n";
        json += "}\n";

        return json;
    }

    bool SoundGraphPatchPreset::DeserializeFromJSON(const std::string& jsonData)
    {
        // Simple JSON deserialization (in a real implementation, use a proper JSON parser)
        // This is a placeholder implementation
        OLO_CORE_WARN("SoundGraphPatchPreset::DeserializeFromJSON - Not fully implemented");
        return false;
    }

    void SoundGraphPatchPreset::Clear()
    {
        m_PresetName.clear();
        m_PresetDescription.clear();
        m_Version = "1.0";
        m_Author.clear();
        m_ParameterDescriptors.clear();
        m_Patches.clear();
    }

    bool SoundGraphPatchPreset::IsEmpty() const
    {
        return m_ParameterDescriptors.empty() && m_Patches.empty();
    }

    ParameterPatch SoundGraphPatchPreset::MergePatches(const std::vector<std::string>& patchNames, const std::string& mergedPatchName) const
    {
        ParameterPatch merged;
        merged.Name = mergedPatchName;
        merged.Description = "Merged from multiple patches";

        for (const std::string& patchName : patchNames)
        {
            const ParameterPatch* patch = GetPatch(patchName);
            if (patch)
            {
                // Later patches override earlier ones
                for (const auto& [paramId, value] : patch->Parameters)
                {
                    merged.SetParameter(paramId, value);
                }
            }
        }

        return merged;
    }

    ParameterPatch SoundGraphPatchPreset::InterpolatePatches(const std::string& patchA, const std::string& patchB, f32 t, const std::string& resultPatchName) const
    {
        const ParameterPatch* a = GetPatch(patchA);
        const ParameterPatch* b = GetPatch(patchB);
        
        ParameterPatch result;
        result.Name = resultPatchName;
        result.Description = "Interpolated between " + patchA + " and " + patchB;

        if (!a || !b)
        {
            OLO_CORE_WARN("SoundGraphPatchPreset::InterpolatePatches - one or both patches not found");
            return result;
        }

        t = std::clamp(t, 0.0f, 1.0f);

        // Find common parameters
        for (const auto& [paramId, valueA] : a->Parameters)
        {
            auto itB = b->Parameters.find(paramId);
            if (itB != b->Parameters.end())
            {
                const ParameterValue& valueB = itB->second;
                
                // Interpolate based on type
                std::visit([&result, paramId, t](const auto& vA, const auto& vB) {
                    using T = std::decay_t<decltype(vA)>;
                    if constexpr (std::is_same_v<T, std::decay_t<decltype(vB)>>)
                    {
                        if constexpr (std::is_same_v<T, f32>)
                        {
                            f32 interpolated = vA + (vB - vA) * t;
                            result.SetParameter(paramId, interpolated);
                        }
                        else if constexpr (std::is_same_v<T, i32>)
                        {
                            i32 interpolated = static_cast<i32>(vA + (vB - vA) * t);
                            result.SetParameter(paramId, interpolated);
                        }
                        else if constexpr (std::is_same_v<T, bool>)
                        {
                            bool interpolated = t < 0.5f ? vA : vB;
                            result.SetParameter(paramId, interpolated);
                        }
                    }
                }, valueA, valueB);
            }
        }

        return result;
    }

    std::string SoundGraphPatchPreset::SerializeParameterValue(const ParameterValue& value) const
    {
        return std::visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, f32>)
            {
                return std::to_string(v);
            }
            else if constexpr (std::is_same_v<T, i32>)
            {
                return std::to_string(v);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return v ? "true" : "false";
            }
            return "";
        }, value);
    }

    ParameterValue SoundGraphPatchPreset::DeserializeParameterValue(const std::string& valueStr, const ParameterValue& defaultValue) const
    {
        // Simple deserialization based on the default value type
        return std::visit([&valueStr](const auto& defaultVal) -> ParameterValue {
            using T = std::decay_t<decltype(defaultVal)>;
            if constexpr (std::is_same_v<T, f32>)
            {
                try { return std::stof(valueStr); }
                catch (...) { return defaultVal; }
            }
            else if constexpr (std::is_same_v<T, i32>)
            {
                try { return std::stoi(valueStr); }
                catch (...) { return defaultVal; }
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return valueStr == "true" || valueStr == "1";
            }
            return defaultVal;
        }, defaultValue);
    }

    //==============================================================================
    /// Factory Functions

    Ref<SoundGraphPatchPreset> CreateBasicSoundPreset()
    {
        auto preset = CreateRef<SoundGraphPatchPreset>();
        preset->SetName("Basic Sound Controls");
        preset->SetDescription("Common parameters for sound control");

        // Register basic parameters
        ParameterDescriptor volume;
        volume.ID = 1;
        volume.Name = "Volume";
        volume.DisplayName = "Volume";
        volume.Description = "Overall sound volume";
        volume.DefaultValue = 1.0f;
        volume.MinValue = 0.0f;
        volume.MaxValue = 2.0f;
        volume.Units = "linear";
        preset->RegisterParameter(volume);

        ParameterDescriptor pitch;
        pitch.ID = 2;
        pitch.Name = "Pitch";
        pitch.DisplayName = "Pitch";
        pitch.Description = "Playback pitch/speed";
        pitch.DefaultValue = 1.0f;
        pitch.MinValue = 0.1f;
        pitch.MaxValue = 4.0f;
        pitch.Units = "multiplier";
        preset->RegisterParameter(pitch);

        return preset;
    }

    Ref<SoundGraphPatchPreset> CreateSpatialAudioPreset()
    {
        auto preset = CreateBasicSoundPreset();
        preset->SetName("Spatial Audio");
        preset->SetDescription("3D spatial audio parameters");

        // Register spatial parameters
        ParameterDescriptor doppler;
        doppler.ID = 20;
        doppler.Name = "Doppler";
        doppler.DisplayName = "Doppler Effect";
        doppler.Description = "Doppler effect strength";
        doppler.DefaultValue = 1.0f;
        doppler.MinValue = 0.0f;
        doppler.MaxValue = 2.0f;
        doppler.Units = "factor";
        preset->RegisterParameter(doppler);

        return preset;
    }

    Ref<SoundGraphPatchPreset> CreateFilterEffectsPreset()
    {
        auto preset = CreateBasicSoundPreset();
        preset->SetName("Filter & Effects");
        preset->SetDescription("Audio filters and effects parameters");

        // Register filter parameters
        ParameterDescriptor lowpass;
        lowpass.ID = 10;
        lowpass.Name = "LowPass";
        lowpass.DisplayName = "Low Pass Filter";
        lowpass.Description = "Low pass filter cutoff";
        lowpass.DefaultValue = 1.0f;
        lowpass.MinValue = 0.0f;
        lowpass.MaxValue = 1.0f;
        lowpass.Units = "normalized";
        preset->RegisterParameter(lowpass);

        ParameterDescriptor highpass;
        highpass.ID = 11;
        highpass.Name = "HighPass";
        highpass.DisplayName = "High Pass Filter";
        highpass.Description = "High pass filter cutoff";
        highpass.DefaultValue = 0.0f;
        highpass.MinValue = 0.0f;
        highpass.MaxValue = 1.0f;
        highpass.Units = "normalized";
        preset->RegisterParameter(highpass);

        return preset;
    }

} // namespace OloEngine::Audio::SoundGraph