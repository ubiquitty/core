// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Session/Identity.h"
#include "Core/Session/SessionTree.h"

#include <cstdint>
#include <string>
#include <utility>

namespace Ubiquitty::Agent {

/// Borrows one listed pane and its authoritative window and tab.
struct ListedPane {
    const Session::TreeWindow* window = nullptr;
    const Session::TreeTab* tab = nullptr;
    const Session::TreePane* pane = nullptr;
};

/// Formats one complete host-listed pane without opening a connection.
/// Labels are host values, not inferred from the foreground title or tab name.
///
/// - Parameter listed: Three non-null pointers into a snapshot alive for this call.
/// - Returns: An owned CLI row, including empty labels when none were assigned.
[[nodiscard]] inline Codec::JSON::Value sessionJSON(const ListedPane& listed)
{
    using Codec::JSON::Value;
    namespace Identity = Session::Identity;
    const auto formatID = [](Identity::ObjectKind kind, std::uint64_t number) {
        return number ? Identity::format({ kind, number }) : std::string();
    };
    const Session::TreePane& pane = *listed.pane;
    Value entry = Value::object();
    entry.set("session", Value::string(formatID(Identity::ObjectKind::Session, pane.sessionID)));
    entry.set("label", Value::string(pane.sessionLabel));
    entry.set("window", Value::string(formatID(Identity::ObjectKind::Window, std::to_underlying(listed.window->id))));
    entry.set("windowLabel", Value::string(listed.window->label));
    entry.set("tab", Value::string(formatID(Identity::ObjectKind::Tab, std::to_underlying(listed.tab->id))));
    entry.set("tabLabel", Value::string(listed.tab->label));
    entry.set("pane", Value::string(formatID(Identity::ObjectKind::Pane, std::to_underlying(pane.id))));
    entry.set("paneLabel", Value::string(pane.label));
    entry.set("columns", Value::number(pane.columns()));
    entry.set("rows", Value::number(pane.rows()));
    entry.set("atPrompt", Value::boolean(pane.facts.isAtPrompt));
    entry.set("foreground", Value::string(pane.program));
    entry.set("directory", Value::string(pane.directory()));
    entry.set("title", Value::string(pane.title.programTitle));
    return entry;
}

} // namespace Ubiquitty::Agent
