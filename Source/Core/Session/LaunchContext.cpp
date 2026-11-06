// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/LaunchContext.cpp — see LaunchContext.h.

#include "Core/Session/LaunchContext.h"

#include <utility>

namespace Ubiquitty::Session {

namespace {

void append(std::vector<std::string>& entries, std::string_view name, std::string_view value)
{
    std::string entry(name);
    entry += '=';
    entry += value;
    entries.push_back(std::move(entry));
}

std::optional<Identity::ObjectID> identityOf(const char* (*lookup)(const char*), std::string_view name, Identity::ObjectKind kind)
{
    // The names are literals, so their views are NUL-terminated.
    const char* value = lookup(name.data());
    if (!value)
        return std::nullopt;
    const std::optional<Identity::ObjectID> id = Identity::parseObjectID(value);
    if (!id || id->kind != kind)
        return std::nullopt;
    return id;
}

} // namespace

std::vector<Identity::ObjectID> LaunchContext::identities() const
{
    std::vector<Identity::ObjectID> out;
    for (const auto* part : { &session, &window, &tab, &pane }) {
        if (*part)
            out.push_back(**part);
    }
    return out;
}

std::vector<std::string> contextEnvironment(const LaunchContext& context)
{
    std::vector<std::string> entries;
    if (!context.socketPath.empty())
        append(entries, socketEnvironmentName, context.socketPath);
    const std::pair<std::string_view, const std::optional<Identity::ObjectID>*> parts[] {
        { sessionEnvironmentName, &context.session },
        { windowEnvironmentName, &context.window },
        { tabEnvironmentName, &context.tab },
        { paneEnvironmentName, &context.pane },
    };
    for (const auto& [name, id] : parts) {
        if (*id && (*id)->isValid())
            append(entries, name, Identity::format(**id));
    }
    return entries;
}

LaunchContext contextFromEnvironment(const char* (*lookup)(const char* name))
{
    LaunchContext context;
    if (!lookup)
        return context;
    if (const char* socket = lookup(socketEnvironmentName.data()))
        context.socketPath = socket;
    context.session = identityOf(lookup, sessionEnvironmentName, Identity::ObjectKind::Session);
    context.window = identityOf(lookup, windowEnvironmentName, Identity::ObjectKind::Window);
    context.tab = identityOf(lookup, tabEnvironmentName, Identity::ObjectKind::Tab);
    context.pane = identityOf(lookup, paneEnvironmentName, Identity::ObjectKind::Pane);
    return context;
}

} // namespace Ubiquitty::Session
