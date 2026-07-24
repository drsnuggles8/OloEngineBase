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
                                   self->m_Exporters.emplace(std::string(ext), std::make_shared<AssimpMeshExporter>());
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
        std::lock_guard<std::mutex> lock(m_Mutex);
        // unique_ptr -> shared_ptr; replacing an existing slot drops the registry's reference but
        // any Find()-held handle keeps that exporter alive until its caller finishes.
        m_Exporters.insert_or_assign(NormalizeExtension(extension),
                                     std::shared_ptr<MeshExporter>(std::move(exporter)));
    }

    std::shared_ptr<MeshExporter> MeshExporterRegistry::Find(std::string_view extension) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        EnsureBuiltinsRegistered();
        if (auto it = m_Exporters.find(NormalizeExtension(extension)); it != m_Exporters.end())
            return it->second;
        return nullptr;
    }

    MeshExportResult MeshExporterRegistry::Export(const MeshSource& source,
                                                  const std::filesystem::path& path,
                                                  const MeshExportOptions& options) const
    {
        const std::string extension = path.has_extension() ? path.extension().string() : std::string{};
        // Find() locks/unlocks internally and returns an ownership-retaining handle.
        std::shared_ptr<MeshExporter> exporter = Find(extension);
        if (!exporter)
        {
            return MeshExportResult::Failure(
                "No mesh exporter registered for extension '" + extension + "' (path: " + path.string() + ")");
        }
        // Invoke OUTSIDE the registry lock; the retained handle keeps the exporter alive even if a
        // concurrent Register() replaces its slot mid-export.
        return exporter->Export(source, path, options);
    }

    bool MeshExporterRegistry::IsSupported(std::string_view extension) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        EnsureBuiltinsRegistered();
        return m_Exporters.contains(NormalizeExtension(extension));
    }

    std::vector<std::string> MeshExporterRegistry::GetRegisteredExtensions() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        EnsureBuiltinsRegistered();
        std::vector<std::string> extensions;
        extensions.reserve(m_Exporters.size());
        for (const auto& [ext, exporter] : m_Exporters)
            extensions.push_back(ext);
        std::ranges::sort(extensions);
        return extensions;
    }
} // namespace OloEngine
