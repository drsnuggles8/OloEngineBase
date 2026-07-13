#pragma once

// Process-wide name -> shader lookup for EVERY shader the engine creates from a
// file, whether or not it lives in a ShaderLibrary (issue #607).
//
// Why this exists
// ---------------
// Only shaders owned by the Renderer3D / Renderer2D `ShaderLibrary` used to have
// a name -> Ref<Shader> registry. Every *pass-owned* shader — the ones a render
// pass creates with `Shader::Create("assets/shaders/X.glsl")` in its Init() and
// then keeps in a member (VirtualMeshGBuffer, VirtualVisibilityResolve, the
// GTAO / SSAO / SSR / VirtualCluster* compute shaders, the fluid splat/smooth
// shaders, ...) — was invisible to any by-name lookup. That made
// `olo_shader_reload` refuse them even though `olo_shader_list` happily reported
// them (the ShaderDebugger sees every GL program), so iterating on a pass shader
// meant restarting the editor.
//
// Rather than teaching every pass to opt in to a registry (dozens of call sites,
// easy to forget for the next pass), registration is hooked into the ONE
// chokepoint every file-backed shader already goes through: the backend shader
// constructor. `OpenGLShader(filepath)`, `OpenGLShader(PackDataTag, ...)` and
// `OpenGLComputeShader(filepath)` register themselves; both destructors
// unregister. A pass gets hot-reload for free just by creating its shader the
// normal way.
//
// Source-string shaders (`Shader::Create(name, vertexSrc, fragmentSrc)` — the
// boot / fallback / shader-graph shaders) are deliberately NOT registered: they
// have no file on disk, so "reload from disk" is meaningless for them.
//
// Lifetime / safety
// -----------------
// The registry stores RAW pointers, not Ref<>: a strong Ref would keep dead pass
// shaders (and their GL programs) alive past renderer shutdown, and a WeakRef
// alone is not enough because the liveness side-table is keyed by ADDRESS — a
// freed shader's address can be recycled by an unrelated object, and locking
// such a stale WeakRef would hand out a Ref<Shader> to something that is not a
// shader (see docs/agent-rules/intrusive-refcount-weakref-races.md). Pairing
// registration in the constructor with unregistration in the destructor removes
// the entry BEFORE the memory can be recycled, so a pointer in the map always
// denotes a live shader. Lookups still go through WeakRef::Lock(), which
// atomically checks liveness and increments the refcount, so the caller always
// gets a genuinely valid Ref (or nothing).
//
// A name may map to several shaders (two passes can each create the same .glsl,
// and the library copy is registered too), so each entry holds a vector and a
// reload fans out to all of them.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Shader.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class ShaderRegistry
    {
      public:
        static ShaderRegistry& Get()
        {
            static ShaderRegistry s_Instance;
            return s_Instance;
        }

        ShaderRegistry(const ShaderRegistry&) = delete;
        ShaderRegistry& operator=(const ShaderRegistry&) = delete;
        ShaderRegistry(ShaderRegistry&&) = delete;
        ShaderRegistry& operator=(ShaderRegistry&&) = delete;

        // --- Registration (called from the backend shader ctors/dtors) -------

        void RegisterShader(const std::string& name, Shader* shader)
        {
            if (name.empty() || shader == nullptr)
                return;
            const std::scoped_lock lock(m_Mutex);
            auto& entries = m_Shaders[name];
            if (std::ranges::find(entries, shader) == entries.end())
                entries.push_back(shader);
        }

        void UnregisterShader(Shader* shader)
        {
            if (shader == nullptr)
                return;
            const std::scoped_lock lock(m_Mutex);
            EraseFrom(m_Shaders, shader);
        }

        void RegisterComputeShader(const std::string& name, ComputeShader* shader)
        {
            if (name.empty() || shader == nullptr)
                return;
            const std::scoped_lock lock(m_Mutex);
            auto& entries = m_ComputeShaders[name];
            if (std::ranges::find(entries, shader) == entries.end())
                entries.push_back(shader);
        }

        void UnregisterComputeShader(ComputeShader* shader)
        {
            if (shader == nullptr)
                return;
            const std::scoped_lock lock(m_Mutex);
            EraseFrom(m_ComputeShaders, shader);
        }

        // --- Lookup ----------------------------------------------------------

        // Every live graphics shader registered under `name` (usually one).
        [[nodiscard]] std::vector<Ref<Shader>> FindShaders(const std::string& name) const
        {
            const std::scoped_lock lock(m_Mutex);
            return LockAll<Shader>(m_Shaders, name);
        }

        // Every live compute shader registered under `name` (usually one).
        [[nodiscard]] std::vector<Ref<ComputeShader>> FindComputeShaders(const std::string& name) const
        {
            const std::scoped_lock lock(m_Mutex);
            return LockAll<ComputeShader>(m_ComputeShaders, name);
        }

        [[nodiscard]] bool Contains(const std::string& name) const
        {
            const std::scoped_lock lock(m_Mutex);
            return m_Shaders.contains(name) || m_ComputeShaders.contains(name);
        }

        // Sorted, de-duplicated list of every registered (i.e. reloadable-by-name)
        // shader — what olo_shader_reload offers when a name does not resolve.
        [[nodiscard]] std::vector<std::string> GetAllNames() const
        {
            std::vector<std::string> names;
            {
                const std::scoped_lock lock(m_Mutex);
                names.reserve(m_Shaders.size() + m_ComputeShaders.size());
                for (const auto& [name, entries] : m_Shaders)
                    names.push_back(name);
                for (const auto& [name, entries] : m_ComputeShaders)
                    names.push_back(name);
            }
            std::ranges::sort(names);
            names.erase(std::ranges::unique(names).begin(), names.end());
            return names;
        }

        // Test/shutdown hook — drops every entry without touching the shaders.
        void Clear()
        {
            const std::scoped_lock lock(m_Mutex);
            m_Shaders.clear();
            m_ComputeShaders.clear();
        }

      private:
        ShaderRegistry() = default;

        template<typename TShader>
        static void EraseFrom(std::unordered_map<std::string, std::vector<TShader*>>& map, TShader* shader)
        {
            for (auto it = map.begin(); it != map.end();)
            {
                auto& entries = it->second;
                std::erase(entries, shader);
                it = entries.empty() ? map.erase(it) : std::next(it);
            }
        }

        template<typename TShader>
        static std::vector<Ref<TShader>> LockAll(
            const std::unordered_map<std::string, std::vector<TShader*>>& map,
            const std::string& name)
        {
            std::vector<Ref<TShader>> found;
            const auto it = map.find(name);
            if (it == map.end())
                return found;
            found.reserve(it->second.size());
            for (TShader* raw : it->second)
            {
                // Lock() is the race-free "is it still alive? then take a strong
                // ref" step; a shader mid-destruction yields nullptr rather than a
                // resurrected corpse.
                if (Ref<TShader> locked = WeakRef<TShader>(raw).Lock())
                    found.push_back(std::move(locked));
            }
            return found;
        }

        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, std::vector<Shader*>> m_Shaders;
        std::unordered_map<std::string, std::vector<ComputeShader*>> m_ComputeShaders;
    };
} // namespace OloEngine
