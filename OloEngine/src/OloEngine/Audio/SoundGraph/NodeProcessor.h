#pragma once

#include "OloEngine/Audio/AudioCallback.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include <choc/containers/choc_Value.h>

#include <any>
#include <complex>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <utility>

#include <glm/gtc/constants.hpp>
#include <glm/glm.hpp>
#include <atomic>

#define LOG_DBG_MESSAGES 0

//==============================================================================
/// Simple flag utilities (from Hazel's Base.h)

namespace OloEngine::Audio::SoundGraph
{
    // Use Flag utilities from OloEngine namespace
    using OloEngine::Flag;
    using OloEngine::AtomicFlag;

} // namespace OloEngine::Audio::SoundGraph

#if LOG_DBG_MESSAGES
#define DBG(...) OLO_CORE_WARN(__VA_ARGS__)
#else
#define DBG(...)
#endif

namespace OloEngine::Audio::SoundGraph
{
    struct StreamWriter;

    //==============================================================================
    /// NodeProcessor - Base class for all sound graph nodes
    struct NodeProcessor
    {
        explicit NodeProcessor(const char* dbgName, UUID id) noexcept
            : m_DebugName(dbgName), m_ID(id)
        {
        }

        virtual ~NodeProcessor() = default;

        // Explicitly delete copy and move operations to prevent dangling pointers
        NodeProcessor(const NodeProcessor&) = delete;
        NodeProcessor& operator=(const NodeProcessor&) = delete;
        NodeProcessor(NodeProcessor&&) = delete;
        NodeProcessor& operator=(NodeProcessor&&) = delete;

        std::string m_DebugName;
        UUID m_ID;

    protected:
        f32 m_SampleRate = 48000.0f;

    public:
        //==============================================================================
        /// Base Input/Output structures

        struct Input
        {
            Input() = delete;
            explicit Input(NodeProcessor& owner) noexcept : m_Node(&owner) {}
            NodeProcessor* m_Node = nullptr;
        };

        struct Output
        {
            Output() = delete;
            explicit Output(NodeProcessor& owner) noexcept : m_Node(&owner) {}
            NodeProcessor* m_Node = nullptr;
        };

        //==============================================================================
        /// Event handling system

        struct InputEvent : public Input
        {
            using EventFunction = std::function<void(f32)>;

            explicit InputEvent(NodeProcessor& owner, EventFunction ev) noexcept
                : Input(owner), m_Event(std::move(ev))
            {
            }

            inline virtual void operator()(f32 value) noexcept
            {
                OLO_PROFILE_FUNCTION();
                
                if (m_Event)
                    m_Event(value);
            }

            // Should be bound to NodeProcessor member method
            EventFunction m_Event;
        };

        struct OutputEvent : public Output
        {
            explicit OutputEvent(NodeProcessor& owner) noexcept
                : Output(owner)
            {
            }

            inline void operator()(f32 value) noexcept
            {
                OLO_PROFILE_FUNCTION();
                
                // Iterate through weak_ptr connections, cleaning up expired ones
                auto it = m_DestinationEvents.begin();
                while (it != m_DestinationEvents.end())
                {
                    if (auto dest = it->lock()) // Check if still valid
                    {
                        (*dest)(value);
                        ++it;
                    }
                    else
                    {
                        it = m_DestinationEvents.erase(it);
                    }
                }
            }

            void AddDestination(std::shared_ptr<InputEvent> dest)
            {
                OLO_PROFILE_FUNCTION();
                
                if (dest)
                    m_DestinationEvents.push_back(std::weak_ptr<InputEvent>(dest));
            }

            // Safe connection management using weak_ptr to prevent dangling pointers
            // InputEvent instances are now owned by shared_ptr in InEvents
            std::vector<std::weak_ptr<InputEvent>> m_DestinationEvents;
        };

        //==============================================================================
        /// Endpoint management

        InputEvent& AddInEvent(Identifier id, InputEvent::EventFunction function = nullptr)
        {
            OLO_PROFILE_FUNCTION();
            
            auto inputEvent = std::make_shared<InputEvent>(*this, function);
            const auto& [element, inserted] = InEvents.try_emplace(id, inputEvent);
            OLO_CORE_ASSERT(inserted, "Input event with this ID already exists");
            return *element->second;
        }

        void AddOutEvent(Identifier id, OutputEvent& out)
        {
            OLO_PROFILE_FUNCTION();
            
            const auto& [element, inserted] = OutEvents.insert({ id, std::ref(out) });
            OLO_CORE_ASSERT(inserted, "Output event with this ID already exists");
        }

        choc::value::ValueView& AddInStream(Identifier id, choc::value::ValueView* source = nullptr)
        {
            OLO_PROFILE_FUNCTION();
            
            const auto& [element, inserted] = InputStreams.try_emplace(id);
            OLO_CORE_ASSERT(inserted, "Input stream with this ID already exists");
            
            if (source)
                element->second = *source;

            return element->second;
        }

        template<typename T>
        choc::value::ValueView& AddOutStream(Identifier id, T& memberVariable)
        {
            OLO_PROFILE_FUNCTION();
            
            const auto& [element, inserted] = OutputStreams.try_emplace(id,
                choc::value::ValueView(choc::value::Type::createPrimitive<T>(),
                    &memberVariable,
                    nullptr));
            
            OLO_CORE_ASSERT(inserted, "Output stream with this ID already exists");
            return element->second;
        }

        //==============================================================================
        /// Virtual interface - must be implemented by derived nodes

        virtual void SetSampleRate(f32 sampleRate) { m_SampleRate = sampleRate; }
        virtual void Init() {}
        virtual void Process() {}

        //==============================================================================
        /// Endpoint access

        std::unordered_map<Identifier, std::shared_ptr<InputEvent>> InEvents;
        std::unordered_map<Identifier, std::reference_wrapper<OutputEvent>> OutEvents;

        std::unordered_map<Identifier, choc::value::ValueView> InputStreams;
        std::unordered_map<Identifier, choc::value::ValueView> OutputStreams;

        /// Temporary storage for default value plugs when nothing is connected to an input
        std::vector<std::shared_ptr<StreamWriter>> DefaultValuePlugs;

        //==============================================================================
        /// Convenience accessors

        inline choc::value::ValueView& InValue(const Identifier& id) { return InputStreams.at(id); }
        inline choc::value::ValueView& OutValue(const Identifier& id) { return OutputStreams.at(id); }
        inline InputEvent& InEvent(const Identifier& id) { return *InEvents.at(id); }
        inline OutputEvent& OutEvent(const Identifier& id) { return OutEvents.at(id).get(); }

        // Additional helper methods for SoundGraph integration
        inline std::shared_ptr<InputEvent> GetInputEvent(const Identifier& id) 
        { 
            auto it = InEvents.find(id);
            return it != InEvents.end() ? it->second : nullptr;
        }
        
        inline OutputEvent* GetOutputEvent(const Identifier& id) 
        { 
            auto it = OutEvents.find(id);
            return it != OutEvents.end() ? &(it->second.get()) : nullptr;
        }

        inline const char* GetDisplayName() const { return m_DebugName.c_str(); }
        inline bool HasParameter(const Identifier& id) const { return ParameterInfos.find(id) != ParameterInfos.end(); }

        //==============================================================================
        /// Parameter access wrapper for InitializeInputs functionality
        
        template<typename T>
        struct ParameterWrapper
        {
            T m_Value;
            Identifier m_ID;
            
            ParameterWrapper(const T& value, Identifier id) : m_Value(value), m_ID(id) {}
        };
        
        // Storage for parameter wrappers (must persist for pointer stability)
        std::unordered_map<Identifier, std::shared_ptr<void>> m_ParameterWrappers;
        
        // Mutex for thread-safe parameter access
        mutable std::shared_mutex m_ParameterMutex;
        
        template<typename T>
        std::shared_ptr<ParameterWrapper<T>> GetParameter(const Identifier& id)
        {
            OLO_PROFILE_FUNCTION();
            
            // First, try to find existing wrapper with shared lock (read-only)
            {
                std::shared_lock<std::shared_mutex> lock(m_ParameterMutex);
                auto wrapperIt = m_ParameterWrappers.find(id);
                if (wrapperIt != m_ParameterWrappers.end())
                {
                    return std::static_pointer_cast<ParameterWrapper<T>>(wrapperIt->second);
                }
            }
            
            // Not found in cache, need to create it - acquire exclusive lock
            std::unique_lock<std::shared_mutex> lock(m_ParameterMutex);
            
            // Double-check: another thread might have created it while we were waiting for the lock
            auto wrapperIt = m_ParameterWrappers.find(id);
            if (wrapperIt != m_ParameterWrappers.end())
            {
                return std::static_pointer_cast<ParameterWrapper<T>>(wrapperIt->second);
            }
            
            // Find the parameter in the storage
            auto it = m_ParameterStorage.find(id);
            if (it == m_ParameterStorage.end())
                return nullptr;
            
            // Try to get the typed value from the any storage
            try
            {
                T value = std::any_cast<T>(it->second);
                auto wrapper = std::make_shared<ParameterWrapper<T>>(value, id);
                m_ParameterWrappers[id] = wrapper;
                return wrapper;
            }
            catch (const std::bad_any_cast&)
            {
                return nullptr;
            }
        }

        //==============================================================================
        /// Parameter system	
        struct ParameterInfo
        {
            Identifier m_ID;
            std::string m_DebugName;
            std::string m_TypeName;
        };
        std::unordered_map<Identifier, ParameterInfo> ParameterInfos;
        
        /// Storage for parameter values (for GetParameter access)
        std::unordered_map<Identifier, std::any> m_ParameterStorage;

        template<typename T>
        void AddParameter(Identifier id, std::string_view debugName, const T& defaultValue)
        {
            OLO_PROFILE_FUNCTION();
            
            // Add input stream for this parameter
            auto& stream = AddInStream(id);
            
            // Create default value plug
            auto defaultPlug = std::make_shared<StreamWriter>(stream, T(defaultValue), id);
            DefaultValuePlugs.push_back(defaultPlug);
            
            // Store parameter value for GetParameter access (thread-safe)
            {
                std::unique_lock<std::shared_mutex> lock(m_ParameterMutex);
                m_ParameterStorage[id] = defaultValue;
            }
            
            // Store parameter info for debugging
            ParameterInfo info;
            info.m_ID = id;
            info.m_DebugName = std::string(debugName);
            info.m_TypeName = typeid(T).name();
            ParameterInfos[id] = std::move(info);
        };
    };

    //==============================================================================
    /// StreamWriter - Utility for writing values to streams
    struct StreamWriter : public NodeProcessor
    {
        template<typename T>
        explicit StreamWriter(const choc::value::ValueView& destination, T&& externalObjectOrDefaultValue, Identifier destinationID, UUID id = UUID()) noexcept
            : NodeProcessor("Stream Writer", id)
            , DestinationID(destinationID)
            , OutputValue(std::forward<T>(externalObjectOrDefaultValue))
            , DestinationView(destination)
        {
            // Write the default value into the destination immediately
            DestinationView = OutputValue;
        }

        // Explicitly delete copy and move operations to prevent dangling references
        StreamWriter(const StreamWriter&) = delete;
        StreamWriter& operator=(const StreamWriter&) = delete;
        StreamWriter(StreamWriter&&) = delete;
        StreamWriter& operator=(StreamWriter&&) = delete;

        inline void operator<<(f32 value) noexcept
        {
            OLO_PROFILE_FUNCTION();
            
            m_OutputValue = choc::value::Value(value);
            m_DestinationView = m_OutputValue;
        }

        template<typename T>
        inline void operator<<(T value) noexcept
        {
            OLO_PROFILE_FUNCTION();

            m_OutputValue = choc::value::Value(value);
            m_DestinationView = m_OutputValue;
        }

        Identifier m_DestinationID;
        choc::value::Value m_OutputValue;
        choc::value::ValueView m_DestinationView;
    };

    //==============================================================================
    /// Template specializations for StreamWriter

    template<>
    inline void StreamWriter::operator<<(const choc::value::ValueView& value) noexcept
    {
        OLO_PROFILE_FUNCTION();
        
        m_OutputValue = value;
        m_DestinationView = m_OutputValue;
    }

    template<>
    inline void StreamWriter::operator<<(choc::value::ValueView value) noexcept
    {
        OLO_PROFILE_FUNCTION();
        
        m_OutputValue = value;
        m_DestinationView = m_OutputValue;
    }

} // namespace OloEngine::Audio::SoundGraph

#undef LOG_DBG_MESSAGES
#undef DBG