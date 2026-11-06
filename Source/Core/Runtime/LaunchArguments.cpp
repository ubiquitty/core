// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/LaunchArguments.h"

#include <string_view>

namespace Ubiquitty::Runtime {

namespace {

// The whole tail after the marker, so a token starting with '-' reaches the
// child instead of being read as one of ours.
void takeTail(LaunchRequest& request, int argc, const char* const* argv, int markerIndex)
{
    for (int i = markerIndex + 1; i < argc; ++i)
        request.command.emplace_back(argv[i] ? argv[i] : "");
}

} // namespace

LaunchRequest parseLaunchArguments(int argc, const char* const* argv)
{
    LaunchRequest request;
    request.terminalArgumentCount = argc > 0 ? argc : 0;
    if (argc <= 1 || !argv)
        return request;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i] ? argv[i] : "");

        // --hold is the terminal's own and keeps its place in the terminal's
        // argv, so a frontend that does not implement it still sees it.
        if (argument == "--hold") {
            request.shouldHold = true;
            continue;
        }

        if (argument != "-e" && argument != "--")
            continue;

        request.terminalArgumentCount = i;
        takeTail(request, argc, argv, i);
        if (request.command.empty()) {
            request.status = LaunchRequest::Status::Error;
            request.error = std::string(argument) + " needs a program to run";
            return request;
        }
        // An empty program name reaches execvp as "" and fails there, by which
        // time there is a window to fail in. Naming it here keeps the failure
        // on the command line where the caller can read it.
        if (request.command.front().empty()) {
            request.command.clear();
            request.status = LaunchRequest::Status::Error;
            request.error = std::string(argument) + " was given an empty program name";
            return request;
        }
        request.status = LaunchRequest::Status::Command;
        return request;
    }

    return request;
}

bool launchEndsWithChild(const LaunchRequest& request)
{
    return request.status == LaunchRequest::Status::Command && !request.shouldHold;
}

std::string launchDefaultTitle(const LaunchRequest& request)
{
    if (request.command.empty())
        return {};
    const std::string& program = request.command.front();
    const std::string::size_type slash = program.find_last_of('/');
    // A trailing slash leaves nothing after it; the whole spelling is a better
    // answer than an empty title.
    if (slash == std::string::npos || slash + 1 >= program.size())
        return program;
    return program.substr(slash + 1);
}

} // namespace Ubiquitty::Runtime
