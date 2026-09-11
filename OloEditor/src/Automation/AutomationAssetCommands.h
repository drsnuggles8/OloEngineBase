#pragma once

// Asset automation: query, safe CRUD/move/delete, import and import settings
// (issue #1128, Epic F of the automation control plane).
//
// The read-only pair that predates this -- olo_assets_list and
// olo_assets_problems in McpToolsAssets.cpp -- stays where it is; this file adds
// the per-asset commands, and olo_assets_list gains the name/path filters that
// make it the search half of slice 1.

#include "Automation/AutomationRegistry.h"

namespace OloEngine::Automation
{
    void RegisterAssetAuthoringCommands(AutomationRegistry& registry);
} // namespace OloEngine::Automation
