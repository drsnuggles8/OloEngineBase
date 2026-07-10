#include "OloEnginePCH.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"

// This file is intentionally near-empty (issue #591).
//
// The old comment here claimed the NodeDescription<T> specializations were
// "removed temporarily" and needed re-adding for editor metadata. Archaeology
// on this file's history found no commit that ever actually removed working
// specializations from it — it has only ever held this include + comment.
// The real specializations for every current node type live in
// NodeDescriptions.h/.cpp (note the plural), which DESCRIBE_NODE-specializes
// NodeDescription<T> for all 27 SoundGraph node types registered in
// SoundGraphFactory.cpp; NodeTypes.cpp's INIT_ENDPOINTS_FUNCS macros rely on
// them for reflection-driven endpoint registration (RegisterEndpointInputs /
// RegisterEndpointOutputs in NodeDescriptors.h) and would assert at
// construction if a node's specialization were missing.
//
// Separately, NodeDescription<T> only carries Inputs/Outputs member lists for
// endpoint wiring — it has no notion of display names, tooltips, or parameter
// ranges, so it was never actually usable as "editor metadata" in the first
// place. That metadata is provided by a deliberately separate, hand-maintained
// system: NodeSchema.h/.cpp (NodeParamSchema for the property panel,
// NodePinSchema for pin display), consumed by SoundGraphEditorPanel.cpp. See
// SoundGraphNodeSchemaCoverageTest.cpp for the guard that keeps that schema in
// sync with every node type the factory can construct.
