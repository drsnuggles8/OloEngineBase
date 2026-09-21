#pragma once

#include "OloEngine/Particle/GPUParticleSystem.h"
#include "OloEngine/Templates/UnrealTypeTraits.h"

#include <utility>

namespace OloEngine
{
    // Concrete exclusive owner with a pointer-only representation. ParticleSystem
    // may relocate while the GPU resource and its address remain unchanged.
    class GPUParticleSystemOwner
    {
      public:
        GPUParticleSystemOwner() = default;
        ~GPUParticleSystemOwner()
        {
            delete m_Instance;
        }

        GPUParticleSystemOwner(const GPUParticleSystemOwner&) = delete;
        GPUParticleSystemOwner& operator=(const GPUParticleSystemOwner&) = delete;

        GPUParticleSystemOwner(GPUParticleSystemOwner&& other) noexcept
            : m_Instance(std::exchange(other.m_Instance, nullptr))
        {
        }

        GPUParticleSystemOwner& operator=(GPUParticleSystemOwner&& other) noexcept
        {
            if (this != &other)
            {
                Reset(std::exchange(other.m_Instance, nullptr));
            }
            return *this;
        }

        void Reset(GPUParticleSystem* instance = nullptr)
        {
            if (m_Instance != instance)
            {
                delete std::exchange(m_Instance, instance);
            }
        }

        [[nodiscard]] GPUParticleSystem* Get() const
        {
            return m_Instance;
        }

        [[nodiscard]] GPUParticleSystem* operator->() const
        {
            return m_Instance;
        }

        explicit operator bool() const
        {
            return m_Instance != nullptr;
        }

      private:
        friend struct TIsTriviallyRelocatable<GPUParticleSystemOwner>;
        GPUParticleSystem* m_Instance = nullptr;
    };

    template<>
    struct TIsTriviallyRelocatable<GPUParticleSystemOwner>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(GPUParticleSystemOwner::m_Instance)>::Value;
    };
} // namespace OloEngine
