// oloctl — a command-line frontend over a running OloEngine editor's automation
// registry (issue #1125).
//
// This file is deliberately almost empty. Everything oloctl decides lives in
// OloCtl/CliRunner.cpp over an ICommandSource and two streams, so the whole
// surface — resolution, help, argument binding, the write refusal, the exit
// codes — is testable without a process. All that is left here is turning argv
// into a vector, choosing the source, and returning the code.

#include "OloCtl/CliRunner.h"
#include "OloCtl/McpHttpCommandSource.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);

    const OloCtl::ParsedCommandLine command = OloCtl::ParseCommandLine(args);

    OloCtl::ConnectionRequest request;
    request.Url = command.Options.Url;
    request.Token = command.Options.Token;
    request.DiscoveryFile = command.Options.DiscoveryFile;
    request.Port = command.Options.Port;
    request.TimeoutMs = command.Options.TimeoutMs;

    // Constructed but not connected: the source dials on its first use, so
    // `oloctl help` and `oloctl version` cost nothing on a machine with no editor
    // running.
    OloCtl::McpHttpCommandSource source(request, std::cerr, command.Options.Verbose);
    return OloCtl::RunCli(command, source, std::cout, std::cerr);
}
