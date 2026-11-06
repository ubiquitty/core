// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The in-process MCP server for agent mode. `ubq mcp` runs a JSON-RPC
// 2.0 loop over stdin/stdout exposing the terminal verbs as MCP tools (terminal_new
// / screen / run / send / interrupt / resize / wait). terminal_run / terminal_send
// are screened by the same AgentPolicy + audit log as the CLI.

#include "Core/Security/InjectionGate.h"

#include <cstdint>
#include <string>

namespace Ubiquitty {
namespace Agent {

// Run the MCP server loop until stdin EOF. Returns a process exit code
// (0 on clean EOF). `policy` gates the command/input tools.
int runMCP(Security::AgentPolicy&, const std::string& remoteHost);

} // namespace Agent
} // namespace Ubiquitty
