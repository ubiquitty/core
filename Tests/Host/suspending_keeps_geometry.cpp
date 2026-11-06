// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/suspending_keeps_geometry.cpp — a phone put in a pocket never
// resizes the program (Docs/sync.md §6.9 "Suspending"), as the program reads
// its size with TIOCGWINSZ.
//
// A client types into a session on a real PTY and so owns its size. It says
// `Bye{Suspending}`: the daemon holds its attachment Lost in the pass that
// reads the Bye, and keeps it when the connection is dropped. A second client
// with another window then attaches and types nothing: the program's size is
// still the suspended owner's.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include <sys/ioctl.h>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// The size the program reads.
struct WindowSize {
    int rows = 0;
    int columns = 0;

    friend bool operator==(const WindowSize&, const WindowSize&) = default;
};

WindowSize programSize(int fileDescriptor)
{
    winsize size {};
    if (::ioctl(fileDescriptor, TIOCGWINSZ, &size) != 0)
        return {};
    return { .rows = size.ws_row, .columns = size.ws_col };
}

std::string describe(WindowSize size)
{
    return std::to_string(size.columns) + "x" + std::to_string(size.rows);
}

} // namespace

int main()
{
    Daemon d;
    const std::array<std::string, 3> command { "/bin/sh", "-c", "sleep 60" };
    auto* session = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(session != nullptr, "premise: the session was not spawned");
    if (!session)
        return UBIQUITTY_TEST_SUMMARY();
    const int fileDescriptor = session->stream->fileDescriptor();
    const Ubiquitty::Protocol::Sync::ScopeSet driving { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive };
    const WindowSize owned { .rows = 30, .columns = 100 };
    const WindowSize other { .rows = 10, .columns = 40 };

    // Runs the loop's steps for a client until the program reads a size, or
    // 300 ms pass.
    const auto settle = [&](std::size_t index, UbiquittyTest::ScreenClient& client, WindowSize wanted) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < end) {
            Screen::endReadPass(d);
            Screen::deliver(d, index, client);
            if (programSize(fileDescriptor) == wanted)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    // The owner types, and the program is its size.
    const std::size_t ownerIndex = Screen::attach(d, session->id);
    Screen::grantScope(d, ownerIndex, driving);
    Screen::setTokenID(d, ownerIndex, "phone");
    Screen::markInteractiveUser(d, ownerIndex);
    UbiquittyTest::ScreenClient owner(owned.rows, owned.columns);
    Screen::greet(d, ownerIndex, owner);
    const std::array<std::uint8_t, 1> key { ' ' };
    CHECK(owner.sendRaw(key).has_value());
    Screen::deliver(d, ownerIndex, owner);
    CHECK_MSG(settle(ownerIndex, owner, owned), ("premise: the typist's window did not apply; the program reads " + describe(programSize(fileDescriptor))).c_str());
    const Sync::AttachmentRecord* attachment = Screen::connection(d, ownerIndex) ? Screen::connection(d, ownerIndex)->attachment() : nullptr;
    CHECK_MSG(attachment != nullptr, "premise: the owner has no attachment");
    if (!attachment)
        return UBIQUITTY_TEST_SUMMARY();
    const Sync::ID128 attachmentID = attachment->attachmentID;

    // It suspends: Lost in the pass that reads the Bye, and still held once
    // its connection is dropped.
    owner.suspend();
    Screen::receive(d, ownerIndex, owner.takeStream(Screen::now()));
    const Sync::AttachmentRecord* suspended = Screen::attachments(d).find(attachmentID);
    CHECK_MSG(suspended && suspended->state == Sync::AttachmentState::Lost, "the suspending client's attachment was not Lost at once");
    Screen::drop(d, ownerIndex);
    suspended = Screen::attachments(d).find(attachmentID);
    CHECK_MSG(suspended && suspended->state == Sync::AttachmentState::Lost, "dropping the suspended client's connection freed its attachment");

    // Another window attaches and types nothing.
    const std::size_t otherIndex = Screen::attach(d, session->id);
    Screen::grantScope(d, otherIndex, driving);
    Screen::setTokenID(d, otherIndex, "desk");
    Screen::markInteractiveUser(d, otherIndex);
    UbiquittyTest::ScreenClient desk(other.rows, other.columns);
    Screen::greet(d, otherIndex, desk);
    CHECK_MSG(!settle(otherIndex, desk, other), "a window that typed nothing took the size from the suspended owner");
    CHECK_MSG(programSize(fileDescriptor) == owned, ("the suspended owner's size was not kept; the program reads " + describe(programSize(fileDescriptor))).c_str());

    if (failures == 0)
        std::printf("suspending_keeps_geometry: a suspended owner's attachment is Lost at once and its size stays the program's\n");
    return failures == 0 ? 0 : 1;
}
