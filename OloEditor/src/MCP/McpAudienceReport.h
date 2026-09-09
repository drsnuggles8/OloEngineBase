#pragma once

// Forwarding header. The dual-audience Markdown renderer moved to the
// transport-independent automation layer in issue #1123 — it shapes a RESULT
// PAYLOAD and never touched the MCP transport, so it belongs beside
// AutomationResult rather than under MCP/.
//
// Kept so the call sites (and McpAudienceBlocksTest) that spell the old path
// and the old namespace keep compiling unchanged.

#include "Automation/AutomationAudienceReport.h"

namespace OloEngine::MCP
{
    namespace AudienceReport = Automation::AudienceReport;
} // namespace OloEngine::MCP
