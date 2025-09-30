#pragma once

#include "NodeDescriptors.h"
#include "NodeTypes.h"

namespace OloEngine::Audio::SoundGraph 
{
    //==============================================================================
    // Template implementations for nodes - following Hazel's NodeTypeImpls.h pattern
    //==============================================================================

    // Math Nodes Template Implementations
    template<typename T>
    Add<T>::Add(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void Add<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

    template<typename T>
    Subtract<T>::Subtract(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void Subtract<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

    template<typename T>
    Multiply<T>::Multiply(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void Multiply<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

    template<typename T>
    Divide<T>::Divide(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void Divide<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

    template<typename T>
    Min<T>::Min(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void Min<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

    template<typename T>
    Max<T>::Max(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void Max<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

    template<typename T>
    Clamp<T>::Clamp(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void Clamp<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

    template<typename T>
    MapRange<T>::MapRange(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void MapRange<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

    template<typename T>
    Power<T>::Power(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void Power<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

    template<typename T>
    Abs<T>::Abs(const char* dbgName, UUID id) : NodeProcessor(dbgName, id)
    {
        EndpointUtilities::RegisterEndpoints(this);
    }

    template<typename T>
    void Abs<T>::Init()
    {
        EndpointUtilities::InitializeInputs(this);
    }

} // namespace OloEngine::Audio::SoundGraph