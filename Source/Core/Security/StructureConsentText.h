// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Session/SessionOrigin.h"

#include <string>
#include <string_view>

namespace Ubiquitty::Security {

/// The words a frontend presents for one exact tree change. Denial is the
/// default; no button promises a reusable permission.
struct StructureConsentText {
    std::string question;
    std::string detail;
    std::string allow = "Allow Once";
    std::string deny = "Deny";
};

/// Describes an operation and its requester without letting program-chosen
/// names forge prompt text. Controls and bidi overrides are removed, quoted
/// values are bounded, and an ellipsis discloses any shortened display.
///
/// - Parameter request: A validated question received from the host.
/// - Returns: The shared prompt; empty wording for unreadable parameters.
[[nodiscard]] inline StructureConsentText structureConsentText(const Protocol::Sync::StructureConsent& request)
{
    namespace JSON = Codec::JSON;
    bool isRead = false;
    const JSON::Value parameters = JSON::parse(request.parameters, isRead);
    if (!isRead || !parameters.isObject() || !Protocol::Sync::isStructureMethod(request.method))
        return {};
    const auto quoted = [](std::string_view value) {
        return "“" + Session::sanitizedOriginName(Title::sanitizedBannerText(value, Title::LevelMarks::Remove)) + "”";
    };
    const auto numbered = [&](const char* field) {
        return std::string(field) + ":" + std::to_string(static_cast<std::uint32_t>(parameters.numberOr(field)));
    };
    std::string change;
    if (request.method == "label.set") {
        const std::string label = parameters.stringOr("label");
        change = (label.empty() ? "Clear the label of " : "Set the label of ") + quoted(parameters.stringOr("object"));
        if (!label.empty())
            change += " to " + quoted(label);
    } else if (request.method == "tags.set") {
        change = "Replace the tags of " + quoted(parameters.stringOr("object")) + " with ";
        const JSON::Value* tags = parameters.find("tags");
        if (!tags || !tags->isArray())
            return {};
        if (tags->items().empty())
            change += "no tags";
        for (std::size_t index = 0; index < tags->items().size(); ++index) {
            if (index != 0)
                change += ", ";
            change += quoted(tags->items()[index].asString());
        }
    } else if (request.method == "workspace.rename" || request.method == "tab.rename") {
        change = "Rename " + numbered(request.method == "workspace.rename" ? "workspace" : "tab") + " to " + quoted(parameters.stringOr("name"));
    } else if (request.method == "tab.move") {
        change = "Move " + numbered("tab") + " to " + numbered("window") + " at position " + std::to_string(static_cast<std::uint32_t>(parameters.numberOr("index")) + 1);
    } else if (request.method == "workspace.addTab") {
        change = "Add a tab with a new session to " + numbered("window");
    } else if (request.method == "workspace.split") {
        const JSON::Value* ratio = parameters.find("ratio");
        if (!ratio || !ratio->isNumber())
            return {};
        change = "Split " + numbered("pane") + " with a " + parameters.stringOr("orientation") + " divider at ratio " + JSON::serialize(*ratio) + " and start a new session";
    } else if (request.method == "workspace.closeTab") {
        change = "Close " + numbered("tab") + " and end every session in that tab";
    } else if (request.method == "workspace.closePane") {
        change = "Close " + numbered("pane") + " and end its session";
    } else {
        change = "Close " + numbered("window") + " and end every session in that window";
    }
    StructureConsentText text;
    text.question = request.method == "window.close" ? "Let an agent close this window and end its sessions?"
        : request.method == "workspace.closeTab"     ? "Let an agent close this tab and end its sessions?"
        : request.method == "workspace.closePane"    ? "Let an agent close this pane and end its session?"
                                                     : "Let an agent change this workspace?";
    text.detail = change + ".\nWorkspace: " + quoted(request.workspaceName) + " (workspace:" + std::to_string(request.workspaceID) + ").";
    text.detail += request.peerProcess.empty() ? "\nThe requester’s process identity is not available." : "\nVerified requester: " + quoted(request.peerProcess) + ".";
    if (!request.agentLabel.empty())
        text.detail += "\nThe requester calls itself " + quoted(request.agentLabel) + "; that name is not verified.";
    text.detail += "\n\nAllow Once approves only this change. It grants no permission for later changes.";
    return text;
}

} // namespace Ubiquitty::Security
