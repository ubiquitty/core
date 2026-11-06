// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/attachments_listed.cpp — the daemon tells each client who else
// shows its pane (Docs/sync.md §6.10 "Attachments are visible").
//
// One client attaches to a session and is listed itself. A second attaches:
// both are told of two attachments. The second types: both are told it owns
// the size and is the primary. No notify names a principal, and a token
// appears only as the verified grant key a person's presence row carries
// (Docs/sync.md §6.10), never as anything else.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"
#include "NotifyRead.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// A client and the newest attachments list the daemon sent it.
struct Listed {
    UbiquittyTest::ScreenClient client { 24, 80 };
    std::optional<Sync::AttachmentsList> list;
    std::vector<std::string> heard;

    /// Reads what the client was sent since the last call.
    void read()
    {
        for (const Sync::Notify& notify : client.takeNotifies()) {
            if (std::optional<Sync::AttachmentsList> parsed = UbiquittyTest::attachmentsOf(notify))
                list = std::move(parsed);
            heard.emplace_back(notify.record.begin(), notify.record.end());
        }
    }
};

} // namespace

int main()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const Ubiquitty::Protocol::Sync::ScopeSet driving { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive };

    const std::size_t firstIndex = Screen::attach(d, session.id);
    Screen::grantScope(d, firstIndex, driving);
    Screen::setTokenID(d, firstIndex, "first-token");
    Screen::markInteractiveUser(d, firstIndex);
    Listed first;
    Screen::greet(d, firstIndex, first.client);
    Screen::deliver(d, firstIndex, first.client);
    first.read();
    CHECK_MSG(first.list && first.list->attachments.size() == 1, "a lone client was not listed itself");

    const std::size_t secondIndex = Screen::attach(d, session.id);
    Screen::grantScope(d, secondIndex, driving);
    Screen::setTokenID(d, secondIndex, "second-token");
    Screen::markInteractiveUser(d, secondIndex);
    Listed second;
    Screen::greet(d, secondIndex, second.client);
    Screen::deliver(d, secondIndex, second.client);
    Screen::deliver(d, firstIndex, first.client);
    first.read();
    second.read();
    CHECK_MSG(first.list && first.list->attachments.size() == 2, "the first client was not told of the second");
    CHECK_MSG(second.list && second.list->attachments.size() == 2, "the second client was not told of the first");

    // The second types: it owns the size and is the primary, and both hear.
    const std::array<std::uint8_t, 1> key { ' ' };
    CHECK(second.client.sendRaw(key).has_value());
    Screen::deliver(d, secondIndex, second.client);
    Screen::deliver(d, firstIndex, first.client);
    first.read();
    second.read();
    const Sync::AttachmentRecord* typist = Screen::connection(d, secondIndex) ? Screen::connection(d, secondIndex)->attachment() : nullptr;
    CHECK_MSG(typist != nullptr, "premise: the second client has no attachment");
    for (const Listed* listed : { &first, &second }) {
        const bool isNamed = typist && listed->list && std::ranges::any_of(listed->list->attachments, [&](const Sync::AttachmentEntry& entry) {
            return entry.attachmentID == typist->attachmentID && entry.isGeometryOwner && entry.isPrimary;
        });
        CHECK_MSG(isNamed, "the typist was not listed as the size's owner and the primary");
        for (const std::string& text : listed->heard) {
            CHECK_MSG(!text.contains("principal"), "a notify names a principal");
            std::string rest = text;
            // The presence row carries the verified grant key as text inside the record.
            for (const std::string_view key : { "token:first-token", "token:second-token" }) {
                for (std::size_t at = rest.find(key); at != std::string::npos; at = rest.find(key))
                    rest.erase(at, key.size());
            }
            CHECK_MSG(!rest.contains("token"), "a notify names a token other than as a presence row's grant key");
        }
    }

    if (failures == 0)
        std::printf("attachments_listed: each client is told who shows its pane, who owns its size and who is its primary, and no principal\n");
    return failures == 0 ? 0 : 1;
}
