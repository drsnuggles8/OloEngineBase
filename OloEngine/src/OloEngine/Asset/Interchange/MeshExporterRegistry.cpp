#include "OloEnginePCH.h"
#include "OloEngine/Asset/Interchange/MeshExporterRegistry.h"

#include "OloEngine/Asset/Interchange/AssimpMeshExporter.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace OloEngine
{
    namespace
    {
        std::string NormalizeExtension(std::string_view extension)
        {
            std::string normalized(extension);
            if (!normalized.empty() && normalized.front() == '.')
                normalized.erase(normalized.begin());
            std::ranges::transform(normalized, normalized.begin(),
                                   [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return normalized;
        }
    } // namespace

    MeshExporterRegistry& MeshExporterRegistry::Get()
    {
        static MeshExporterRegistry s_Instance;
        return s_Instance;
    }

    void MeshExporterRegistry::EnsureBuiltinsRegistered() const
    {
        static std::once_flag s_Flag;
        std::call_once(s_Flag,
                       [this]()
                       {
                           auto* self = const_cast<MeshExporterRegistry*>(this);
                           // Assimp's glTF2 exporter writes both the text (.gltf) and binary
                           // (.glb) containers. Only claim a slot if it is free so an external
                           // Register() wins.
                           constexpr std::string_view kGltfExtensions[] = { "gltf", "glb" };
                           for (std::string_view ext : kGltfExtensions)
                           {
                               if (!self->m_Exporters.contains(std::string(ext)))
                                   self->m_Exporters.emplace(std::string(ext), CreateScope<AssimpMeshExporter>());
                           }
                       });
    }

    void MeshExporterRegistry::Register(std::string_view extension, Scope<MeshExporter> exporter)
    {
        if (!exporter)
        {
            OLO_CORE_ERROR("MeshExporterRegistry::Register - null exporter for extension '{}'", extension);
            return;
        }
        m_Exporters.insert_or_assign(NormalizeExtension(extension), std::move(exporter));
    }

    MeshExporter* MeshExporterRegistry::Find(std::string_view extension) const
    {
        EnsureBuiltinsRegistered();
        if (auto it = m_Exporters.find(NormalizeExtension(extension)); it != m_Exporters.end())
            return it->second.get();
        return nullptr;
    }

    MeshExportResult MeshExporterRegistry::Export(const MeshSource& source,
                                                  const std::filesystem::path& path,
                                                  const MeshExportOptions& options) const
    {
        const std::string extension = path.has_extension() ? path.extension().string() : std::string{};
        MeshExporter* exporter = Find(extension);
        if (!exporter)
        {
            return MeshExportResult::Failure(
                "No mesh exporter registered for extension '" + extension + "' (path: " + path.string() + ")");
        }
        return exporter->Export(source, path, options);
    }

    bool MeshExporterRegistry::IsSupported(std::string_view extension) const
    {
        EnsureBuiltinsRegistered();
        return m_Exporters.contains(NormalizeExtension(extension));
    }

    std::vector<std::string> MeshExporterRegistry::GetRegisteredExtensions() const
    {
        EnsureBuiltinsRegistered();
        std::vector<std::string> extensions;
        extensions.reserve(m_Exporters.size());
        for (const auto& [ext, exporter] : m_Exporters)
            extensions.push_back(ext);
        std::ranges::sort(extensions);
        return extensions;
    }
} // namespace OloEngine
