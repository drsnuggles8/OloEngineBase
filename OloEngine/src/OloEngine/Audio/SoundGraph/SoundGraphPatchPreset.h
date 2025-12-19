#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
    class SoundGraphSound; // Forward declaration
}

namespace OloEngine::Audio::SoundGraph
{
    /// Parameter value types that can be stored in presets
    using ParameterValue = std::variant<f32, i32, bool>;

    /// Parameter descriptor with metadata
    struct ParameterDescriptor
    {
        u32 ID = 0;
        std::string Name;
        std::string DisplayName;
        std::string Description;
        ParameterValue DefaultValue;
        ParameterValue MinValue;
        ParameterValue MaxValue;
        std::string Units;
        bool IsAutomatable = true;
    };

    /// Collection of parameter changes that can be applied as a group
    struct ParameterPatch
    {
        std::unordered_map<u32, ParameterValue> Parameters;
        std::string Name;
        std::string Description;
        f64 Timestamp = 0.0; // When this patch was created/modified

        /// Add a parameter change to this patch
        void SetParameter(u32 parameterID, const ParameterValue& value);

        /// Remove a parameter from this patch
        void RemoveParameter(u32 parameterID);

        /// Check if this patch contains a specific parameter
        bool HasParameter(u32 parameterID) const;

        /// Get a parameter value, returns default if not found
        template<typename T>
        T GetParameter(u32 parameterID, const T& defaultValue = T{}) const;

        /// Get all parameter IDs in this patch
        std::vector<u32> GetParameterIDs() const;

        /// Clear all parameters
        void Clear();

        /// Get number of parameters in this patch
        sizet GetParameterCount() const
        {
            return Parameters.size();
        }
    };

    /// Manages presets and parameter patches for SoundGraph instances
    class SoundGraphPatchPreset : public RefCounted
    {
      public:
        SoundGraphPatchPreset() = default;
        ~SoundGraphPatchPreset() = default;

        /// Parameter Descriptors
        void RegisterParameter(const ParameterDescriptor& descriptor);
        void UnregisterParameter(u32 parameterID);
        bool IsParameterRegistered(u32 parameterID) const;
        const ParameterDescriptor* GetParameterDescriptor(u32 parameterID) const;
        std::vector<ParameterDescriptor> GetAllParameterDescriptors() const;

        /// Patch Management
        bool CreatePatch(const std::string& name, const std::string& description = "");
        void DeletePatch(const std::string& name);
        bool HasPatch(const std::string& name) const;
        ParameterPatch* GetPatch(const std::string& name);
        const ParameterPatch* GetPatch(const std::string& name) const;
        std::vector<std::string> GetPatchNames() const;

        /// Apply patches to targets
        void ApplyPatch(const std::string& patchName, SoundGraphSound* target);
        void ApplyPatch(const ParameterPatch& patch, SoundGraphSound* target);

        /// Capture current state as a patch
        void CaptureStateToPatch(const std::string& patchName, SoundGraphSound* source);

        /// Preset File I/O
        bool SaveToFile(const std::string& filePath) const;
        bool LoadFromFile(const std::string& filePath);

        /// JSON Serialization
        std::string SerializeToJSON() const;
        bool DeserializeFromJSON(const std::string& jsonData);

        /// Preset Metadata
        void SetName(const std::string& name)
        {
            m_PresetName = name;
        }
        void SetDescription(const std::string& description)
        {
            m_PresetDescription = description;
        }
        void SetVersion(const std::string& version)
        {
            m_Version = version;
        }
        void SetAuthor(const std::string& author)
        {
            m_Author = author;
        }

        const std::string& GetName() const
        {
            return m_PresetName;
        }
        const std::string& GetDescription() const
        {
            return m_PresetDescription;
        }
        const std::string& GetVersion() const
        {
            return m_Version;
        }
        const std::string& GetAuthor() const
        {
            return m_Author;
        }

        /// Utility
        void Clear();
        bool IsEmpty() const;
        sizet GetPatchCount() const
        {
            return m_Patches.size();
        }
        sizet GetParameterCount() const
        {
            return m_ParameterDescriptors.size();
        }

        /// Create a merged patch from multiple patches
        ParameterPatch MergePatches(const std::vector<std::string>& patchNames, const std::string& mergedPatchName) const;

        /// Interpolate between two patches
        ParameterPatch InterpolatePatches(const std::string& patchA, const std::string& patchB, f32 t, const std::string& resultPatchName) const;

      private:
        std::string m_PresetName;
        std::string m_PresetDescription;
        std::string m_Version = "1.0";
        std::string m_Author;

        std::unordered_map<u32, ParameterDescriptor> m_ParameterDescriptors;
        std::unordered_map<std::string, ParameterPatch> m_Patches;

        /// Helper functions for JSON serialization
        std::string SerializeParameterValue(const ParameterValue& value) const;
        ParameterValue DeserializeParameterValue(const std::string& valueStr, const ParameterValue& defaultValue) const;
    };

    //==============================================================================
    /// Template Implementations

    template<typename T>
    T ParameterPatch::GetParameter(u32 parameterID, const T& defaultValue) const
    {
        auto it = Parameters.find(parameterID);
        if (it == Parameters.end())
            return defaultValue;

        try
        {
            return std::get<T>(it->second);
        }
        catch (const std::bad_variant_access&)
        {
            return defaultValue;
        }
    }

    //==============================================================================
    /// Factory Functions

    /// Create a preset with common parameter descriptors for basic sound controls
    Ref<SoundGraphPatchPreset> CreateBasicSoundPreset();

    /// Create a preset for 3D spatial audio parameters
    Ref<SoundGraphPatchPreset> CreateSpatialAudioPreset();

    /// Create a preset for filter and effects parameters
    Ref<SoundGraphPatchPreset> CreateFilterEffectsPreset();

} // namespace OloEngine::Audio::SoundGraph
