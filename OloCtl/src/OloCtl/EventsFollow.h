#pragma once

// `oloctl events follow`: the CLI consumer of the automation event bus
// (issue #1131).
//
// It is a LOOP over one registry command, `olo_events_wait`, through the same
// ICommandSource::Invoke every other verb uses. There is no second transport:
// the editor's SSE push is for MCP clients, and a CLI that can long-poll needs
// nothing more than the request/response path it already has. Each poll returns
// the events recorded since a cursor, or times out empty; the verb prints what
// came back, advances the cursor to the `lastId` the editor reported, and polls
// again.
//
// OUTPUT DISCIPLINE, same as the rest of oloctl but stricter: stdout carries
// one compact JSON object per line, one line per event, the editor's event
// record verbatim - and NOTHING else, ever. No banner, no summary, no cursor.
// `oloctl events follow | jq -c 'select(.category=="play")'` is the intended
// shape. Warnings (dropped records), the verbose cursor report and every error
// go to stderr.
//
// The verb runs BEFORE the catalogue is fetched: it needs no command tree, only
// the one registry name it hard-codes, so an editor whose olo_tool_search is
// unreachable can still be followed. An editor that predates olo_events_wait
// answers the first poll with an isError result saying "Unknown tool"; that is
// reported as such (exit 1) with a hint, not retried.

#include "OloCtl/CliRunner.h"
#include "OloCtl/CommandSource.h"

#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace OloCtl
{
    // The registry name the verb loops over.
    inline constexpr const char* kEventsWaitCommand = "olo_events_wait";

    // The category tokens olo_events_wait accepts, in the editor's order. The
    // ONE piece of registry knowledge oloctl carries: `--category` and `--until`
    // are validated against it locally so a typo is a usage error (exit 2, the
    // valid tokens named) rather than an error result from the first poll. The
    // editor validates too, so a token missing here surfaces as its message.
    [[nodiscard]] std::span<const char* const> EventCategoryTokens();

    // `oloctl events ...`. `rest` is the positional tail starting AT "events";
    // the verb parses its own flags from rest[2..] (they are not reserved
    // options, so ParseCommandLine leaves them in place). Returns the process
    // exit code.
    [[nodiscard]] int RunEventsVerb(const CliOptions& options, const std::vector<std::string>& rest,
                                    ICommandSource& source, std::ostream& out, std::ostream& err);
} // namespace OloCtl
