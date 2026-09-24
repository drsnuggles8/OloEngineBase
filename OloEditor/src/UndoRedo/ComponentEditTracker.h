#pragma once

#include <concepts>
#include <cstring>
#include <type_traits>

namespace OloEngine
{
    // How ComponentEditTracker decides that a component changed.
    //   Bytes  memcmp. For trivially copyable components with no padding traps.
    //   Value  operator==. For everything else that has one; the operator decides
    //          what counts as authored state (see ParticleSystemComponent).
    enum class ComponentEditDetection
    {
        Bytes,
        Value
    };

    // The per-(entity, component) undo state behind SceneHierarchyPanel's
    // DrawComponent<T>. It turns "the inspector's widgets changed this component"
    // into exactly one undo entry whose before-state is the value the widgets
    // started from.
    //
    // Per frame:
    //     tracker.BeginFrame(component);        // before the widgets draw
    //     uiFunction(component);
    //     if (tracker.EndFrame(component, ImGui's ActiveId != 0))
    //         push ComponentChangeCommand(tracker.Snapshot(), component);
    //
    // BeginFrame re-bases the snapshot whenever the component changed while no
    // inspector edit was in progress: undo, redo, a gizmo drag, an MCP write. That
    // change already has its own history entry, or is not an edit at all. Without
    // the re-base, the Value path saw an undo as a fresh edit and pushed it, which
    // cleared the redo stack and made every further Ctrl+Z flip between the two
    // values. The Bytes path did not push, but kept a stale snapshot, so the next
    // real edit recorded the wrong before-state.
    template<typename T, ComponentEditDetection Detection>
    class ComponentEditTracker
    {
        static_assert(Detection != ComponentEditDetection::Bytes || std::is_trivially_copyable_v<T>,
                      "Byte detection needs a trivially copyable component");
        static_assert(Detection != ComponentEditDetection::Value || std::equality_comparable<T>,
                      "Value detection needs operator==");

      public:
        void BeginFrame(const T& component)
        {
            if (!m_IsEditing && (!m_SnapshotValid || Differs(component)))
            {
                m_Snapshot = component;
                if constexpr (Detection == ComponentEditDetection::Bytes)
                {
                    // Byte copy, so padding bytes match the live component's.
                    std::memcpy(m_SnapshotBytes, &component, sizeof(T));
                }
                m_SnapshotValid = true;
            }

            if constexpr (Detection == ComponentEditDetection::Bytes)
            {
                std::memcpy(m_FrameStartBytes, &component, sizeof(T));
            }
        }

        // Returns true when an inspector edit has just finished and left the
        // component different from Snapshot(). The caller pushes the undo entry
        // then; Snapshot() stays valid until the next BeginFrame.
        [[nodiscard]] bool EndFrame(const T& component, bool widgetActive)
        {
            if constexpr (Detection == ComponentEditDetection::Bytes)
            {
                const bool changedThisFrame = std::memcmp(m_FrameStartBytes, &component, sizeof(T)) != 0;
                if (changedThisFrame)
                {
                    m_IsEditing = true;
                }
                // A drag keeps changing the value; it has ended once a frame passes
                // with no change and no active widget.
                if (m_IsEditing && !changedThisFrame && !widgetActive)
                {
                    return FinishEdit(component);
                }
            }
            else
            {
                if (Differs(component))
                {
                    m_IsEditing = true;
                }
                if (m_IsEditing && !widgetActive)
                {
                    return FinishEdit(component);
                }
            }
            return false;
        }

        [[nodiscard]] const T& Snapshot() const
        {
            return m_Snapshot;
        }

      private:
        [[nodiscard]] bool Differs(const T& component) const
        {
            if constexpr (Detection == ComponentEditDetection::Bytes)
            {
                return std::memcmp(m_SnapshotBytes, &component, sizeof(T)) != 0;
            }
            else
            {
                return !(m_Snapshot == component);
            }
        }

        [[nodiscard]] bool FinishEdit(const T& component)
        {
            const bool changed = Differs(component);
            m_IsEditing = false;
            m_SnapshotValid = false;
            return changed;
        }

        bool m_IsEditing = false;
        bool m_SnapshotValid = false;
        T m_Snapshot{};
        alignas(alignof(T)) unsigned char m_SnapshotBytes[sizeof(T)]{};
        alignas(alignof(T)) unsigned char m_FrameStartBytes[sizeof(T)]{};
    };
} // namespace OloEngine
