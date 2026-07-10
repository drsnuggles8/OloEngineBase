#pragma once

#include "OloEngine/Core/Base.h"

#include <functional>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OloEngine
{
    // Lightweight per-Scene gameplay event dispatcher.
    //
    // Subsystems (QuestSystem, InventorySystem, ...) publish the POD
    // notification payloads declared in Gameplay/Quest/QuestEvents.h and
    // Gameplay/Inventory/InventoryEvents.h; UI panels, audio cues, analytics,
    // and gameplay reactions subscribe by event type. The bus is the
    // "notification mechanism" the quest/inventory systems previously lacked.
    //
    // Dispatch is synchronous: Publish<E> immediately invokes every handler
    // registered for E, in subscription order. Publishers must therefore call
    // Publish *outside* of any entity-view iteration or deferred-destruction
    // window — the engine systems do (see InventorySystem::OnUpdate /
    // QuestSystem::OnUpdate, which accumulate then dispatch after the loop).
    //
    // Lives on Scene (Scene::GetGameplayEvents()); it is runtime-only state —
    // never serialized, never copied with the scene, and Clear()ed on
    // Scene::OnRuntimeStop so a stop/play cycle does not accumulate stale
    // subscribers.
    class GameplayEventBus
    {
      public:
        template<typename E>
        using Handler = std::function<void(const E&)>;

        // Register a handler for events of type E. Handlers are retained until
        // Clear() (typically the end of the owning Scene's runtime session).
        template<typename E>
        void Subscribe(Handler<E> handler)
        {
            auto& handlers = m_Handlers[std::type_index(typeid(E))];
            handlers.emplace_back(
                [fn = std::move(handler)](const void* payload)
                {
                    fn(*static_cast<const E*>(payload));
                });
        }

        // Synchronously dispatch `event` to every handler subscribed to E.
        // No-op when nothing subscribes to E. Safe against a handler calling
        // Subscribe<E>() for the same event type mid-dispatch: indexing
        // (rather than holding range-for iterators) tolerates the vector
        // reallocating underneath us, and snapshotting the count means a
        // handler subscribed during this Publish() is not itself invoked
        // until the next Publish() call.
        template<typename E>
        void Publish(const E& event) const
        {
            auto it = m_Handlers.find(std::type_index(typeid(E)));
            if (it == m_Handlers.end())
            {
                return;
            }
            auto const& handlers = it->second;
            for (sizet i = 0, count = handlers.size(); i < count; ++i)
            {
                handlers[i](&event);
            }
        }

        // Number of handlers registered for E (diagnostics / tests).
        template<typename E>
        [[nodiscard("handler count must be used")]] sizet HandlerCount() const
        {
            auto it = m_Handlers.find(std::type_index(typeid(E)));
            return it == m_Handlers.end() ? 0 : it->second.size();
        }

        // Drop all subscriptions.
        void Clear()
        {
            m_Handlers.clear();
        }

      private:
        using ErasedHandler = std::function<void(const void*)>;
        std::unordered_map<std::type_index, std::vector<ErasedHandler>> m_Handlers;
    };

} // namespace OloEngine
