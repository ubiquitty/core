// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Actions.cpp — see Core/Triggers/Actions.h.
//

#include "Core/Triggers/Actions.h"

#include "Core/CommandLine.h"

namespace Ubiquitty::Triggers {

namespace {

// The body is one flat object, so this escapes a JSON string rather than
// calling Codec::JSON. Triggers links into ubiquitty-host, whose link line
// omits the JSON parser TU because it would drag vendored yyjson into a daemon
// that parses no JSON; depending on it here breaks that link.
void appendJSONString(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (char ch : text) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                // The only characters JSON requires be escaped numerically.
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(byte >> 4) & 0xF]);
                out.push_back(hex[byte & 0xF]);
            } else {
                out.push_back(ch); // UTF-8 passes through
            }
            break;
        }
    }
    out.push_back('"');
}

} // namespace

std::vector<std::string> buildCommandArgv(std::string_view commandTemplate, std::string_view matchedText)
{
    // The match is terminal output, so a match that begins with '-' would be
    // read by the spawned program as an option rather than the datum the
    // template meant — argv injection, since there is no shell to quote it
    // away. A template that passes such a match runs nothing rather than hand
    // untrusted bytes to a program's option parser; an author who wants a
    // dash-leading match anchors the rule's pattern so it cannot start with one.
    const bool matchIsOptionLike = !matchedText.empty() && matchedText.front() == '-';
    std::vector<std::string> argv;
    for (const std::string& word : splitCommandWords(commandTemplate)) {
        if (word == "%s") {
            if (matchIsOptionLike)
                return {};
            argv.emplace_back(matchedText); // the whole match, one argument
        } else if (word == "%%")
            argv.emplace_back("%");
        else
            argv.push_back(word);
    }
    return argv;
}

bool webhookSchemeAllowed(std::string_view url)
{
    const std::size_t colon = url.find(':');
    if (colon == std::string_view::npos)
        return false;
    const std::string_view scheme = url.substr(0, colon);
    if (scheme.size() != 4 && scheme.size() != 5)
        return false;

    std::string lowered;
    lowered.reserve(scheme.size());
    for (char ch : scheme) {
        lowered.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    if (lowered != "http" && lowered != "https")
        return false;
    // A scheme alone is not a URL: something must follow the separator.
    return url.size() > colon + 1;
}

std::string buildWebhookBody(std::string_view matchedText)
{
    std::string body = "{\"match\":";
    appendJSONString(body, matchedText);
    body.push_back('}');
    return body;
}

} // namespace Ubiquitty::Triggers
