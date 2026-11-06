// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Trigger action payloads. A trigger matches attacker-controllable terminal
// output, so everything a matched string reaches is a security boundary: these
// build what run_command spawns and what a webhook sends, and they are the
// reason a match cannot become a command.
//
// The gates ([triggers] allow_run_command / allow_webhook) and the spawn/POST
// themselves stay with the frontend.

#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Triggers {

// Expand a run_command template into an explicit argv.
//
// The *template* is tokenised on whitespace; the *match* never is. `%s` expands to
// the whole match as exactly one argument, so a match containing spaces, `;`,
// `|`, `$(…)`, backticks or newlines stays a single argv element — there is no
// shell, and execve passes elements verbatim. `%%` is a literal `%`.
//
// Empty when the template is empty or holds no words. argv[0] is the program.
[[nodiscard]] std::vector<std::string> buildCommandArgv(std::string_view commandTemplate,
    std::string_view matchedText);

// Whether a webhook URL's scheme is permitted: http and https only, compared
// case-insensitively, so a webhook cannot double as a local opener. This is a
// scheme gate, not a URL validator — the caller still parses the URL and must
// refuse one that does not.
[[nodiscard]] bool webhookSchemeAllowed(std::string_view url);

// The webhook request body: {"match":"<text>"} with the match as a JSON VALUE,
// escaped, so attacker-controlled output cannot break out of the object, forge
// a field, or reach a header. Never interpolated into the URL.
[[nodiscard]] std::string buildWebhookBody(std::string_view matchedText);

} // namespace Ubiquitty::Triggers
