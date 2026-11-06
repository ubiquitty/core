// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/owner_presents.cpp — the program's pixel-size and colour replies
// come from the pane's geometry owner, with a live human colour presenter
// while that owner is absent (Docs/sync.md §6.10, §6.11).
//
// Two people's clients, each holding a token minted for a person, show one
// session. A types, so A owns the size, and presents its
// pixels and its theme. B, which has not typed, presents other pixels in its
// `Viewport` of the pane and another theme in its `Facts`. The program asks for
// its text area in pixels (CSI 14 t) and for its background (OSC 11): the
// answers are A's. Once B types and the coalesce window passes, they are B's.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Geometry.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/TermPalette.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// An 80x24 viewport drawn in cells of a size.
///
/// - Parameters:
///   - cellWidth: A cell's width in pixels.
///   - cellHeight: Its height.
/// - Returns: The viewport, its text area and drawable the grid's.
Sync::Viewport viewportOfCells(std::uint16_t cellWidth, std::uint16_t cellHeight)
{
    return { .rows = 24, .columns = 80, .cellPixelWidth = cellWidth, .cellPixelHeight = cellHeight, .textAreaPixelWidth = static_cast<std::uint16_t>(80 * cellWidth), .textAreaPixelHeight = static_cast<std::uint16_t>(24 * cellHeight), .drawablePixelWidth = static_cast<std::uint16_t>(80 * cellWidth), .drawablePixelHeight = static_cast<std::uint16_t>(24 * cellHeight) };
}

/// The key a client types to take the size.
constexpr std::array<std::uint8_t, 1> space { ' ' };

/// The point at which a pending presenter's connection stops serving.
enum class Departure {
    Reaped,
    Superseded,
    Malformed,
};

Ubiquitty::TermPalette paletteWithBackground(std::uint16_t red, std::uint16_t green, std::uint16_t blue)
{
    Ubiquitty::TermPalette palette {};
    palette.seedDefaults();
    palette.defaultBackground = { red, green, blue };
    return palette;
}

} // namespace

int main()
{
    // A pure theme change reaches an idle program before the person types,
    // including while an agent is attached, typing or holding a size override.
    for (const bool isSizeOverridden : { false, true }) {
        Daemon host;
        auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        auto& pty = *stream;
        auto& session = Screen::makeSession(host, 24, 80, std::move(stream));
        Screen::publish(host, session, Ubiquitty::Runtime::FrameClass::Structural);
        const auto humanIndex = Screen::attach(host, session.id);
        Screen::grantScope(host, humanIndex, { Sync::Scope::Observe, Sync::Scope::Drive });
        Screen::setTokenID(host, humanIndex, "human-presenter");
        Screen::markInteractiveUser(host, humanIndex);
        UbiquittyTest::ScreenClient human(24, 80, Sync::ID128 { 110, 1 });
        Screen::greet(host, humanIndex, human);
        const auto dark = paletteWithBackground(0x1111, 0x1111, 0x1111);
        human.setThemePalette(Sync::ThemePalette { .live = dark, .baseline = dark });
        Screen::deliver(host, humanIndex, human);
        CHECK(session.term->palette.defaultBackground == dark.defaultBackground);

        const auto agentIndex = Screen::attach(host, session.id);
        Screen::grantScope(host, agentIndex, { Sync::Scope::Observe, Sync::Scope::Drive });
        Screen::setTokenID(host, agentIndex, "theme-observer");
        Screen::setAgentLabel(host, agentIndex, "theme-observer");
        Screen::grantAgentSession(host, session, agentIndex);
        UbiquittyTest::ScreenClient agent(24, 80, Sync::ID128 { 111, 1 }, Sync::Role::Agent);
        Screen::greet(host, agentIndex, agent);
        CHECK(agent.sendRaw(space).has_value());
        Screen::deliver(host, agentIndex, agent);
        const auto light = paletteWithBackground(0xffff, 0xffff, 0xffff);
        agent.setThemePalette(Sync::ThemePalette { .live = light, .baseline = light });
        Screen::deliver(host, agentIndex, agent);
        CHECK_MSG(session.term->palette.defaultBackground == dark.defaultBackground, "an agent's facts replaced the human's palette");
        if (isSizeOverridden) {
            CHECK(agent.call(R"({"jsonrpc":"2.0","id":1,"method":"pane.overrideGeometry","params":{"pane":1,"rows":12,"columns":50}})"));
            Screen::deliver(host, agentIndex, agent);
            CHECK(session.screenSource->geometry().status().isOverridden);
        }
        const auto owner = session.screenSource->geometry().drivingOwner();
        CHECK(owner == Screen::connection(host, humanIndex)->attachment()->attachmentID);
        pty.pushInbound("\033[?2031h");
        Screen::servicePTY(host, session);
        const auto before = pty.written.size();
        human.setThemePalette(Sync::ThemePalette { .live = light, .baseline = light });
        Screen::deliver(host, humanIndex, human);
        Screen::endReadPass(host);
        CHECK_MSG(pty.written.substr(before).find("\033[?997;2n") != std::string::npos, "a pure human theme change did not notify the idle DEC 2031 subscriber");
        pty.pushInbound("\033]11;?\007");
        Screen::servicePTY(host, session);
        CHECK_MSG(pty.written.substr(before).find("]11;rgb:ffff/ffff/ffff") != std::string::npos, "OSC 11 did not answer with the human's new background");
        CHECK(session.screenSource->geometry().drivingOwner() == owner);
        CHECK(session.screenSource->geometry().status().isOverridden == isSizeOverridden);
    }

    // A lost driver retains geometry for resume, but a live person can
    // still change the program's colours without taking that geometry.
    {
        Daemon host;
        auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        auto& pty = *stream;
        auto& session = Screen::makeSession(host, 24, 80, std::move(stream));
        Screen::publish(host, session, Ubiquitty::Runtime::FrameClass::Structural);
        const auto attachHuman = [&] {
            const auto index = Screen::attach(host, session.id);
            Screen::grantScope(host, index, { Sync::Scope::Observe, Sync::Scope::Drive });
            Screen::setTokenID(host, index, "presenter-" + std::to_string(index));
            Screen::markInteractiveUser(host, index);
            return index;
        };
        const auto oldIndex = attachHuman();
        UbiquittyTest::ScreenClient old(24, 80, Sync::ID128 { 120, 1 });
        Screen::greet(host, oldIndex, old);
        const auto dark = paletteWithBackground(0x1111, 0x1111, 0x1111);
        old.setThemePalette(Sync::ThemePalette { .live = dark, .baseline = dark });
        CHECK(old.sendViewport(viewportOfCells(10, 20)));
        CHECK(old.sendRaw(space).has_value());
        Screen::deliver(host, oldIndex, old);
        const auto liveIndex = attachHuman();
        UbiquittyTest::ScreenClient live(24, 80, Sync::ID128 { 121, 1 });
        Screen::greet(host, liveIndex, live);
        live.setThemePalette(Sync::ThemePalette { .live = dark, .baseline = dark });
        CHECK(live.sendViewport(viewportOfCells(7, 14)));
        Screen::deliver(host, liveIndex, live);
        std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
        Screen::endReadPass(host);
        const auto owner = session.screenSource->geometry().drivingOwner();
        Screen::drop(host, oldIndex);
        CHECK_MSG(session.screenSource->geometry().drivingOwner() == owner, "the lost driver's geometry remains reserved for its resume");
        pty.pushInbound("\033[?2031h");
        Screen::servicePTY(host, session);
        const auto before = pty.written.size();
        const auto light = paletteWithBackground(0xffff, 0xffff, 0xffff);
        live.setThemePalette(Sync::ThemePalette { .live = light, .baseline = light });
        Screen::deliver(host, liveIndex, live);
        Screen::endReadPass(host);
        CHECK_MSG(pty.written.substr(before).find("\033[?997;2n") != std::string::npos, "a lost geometry owner suppressed a live person's colour-scheme notification");
        pty.pushInbound("\033]11;?\007\033[14t");
        Screen::servicePTY(host, session);
        CHECK_MSG(pty.written.substr(before).find("]11;rgb:ffff/ffff/ffff") != std::string::npos, "a lost geometry owner suppressed a live person's background palette");
        CHECK_MSG(pty.written.substr(before).find("\033[4;480;800t") != std::string::npos, "the palette fallback took the lost driver's pixel geometry");
        CHECK(session.screenSource->geometry().drivingOwner() == owner);

        const auto settle = [&] {
            std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
            Screen::endReadPass(host);
        };
        const auto otherIndex = attachHuman();
        UbiquittyTest::ScreenClient other(24, 80, Sync::ID128 { 122, 1 });
        Screen::greet(host, otherIndex, other);
        const auto green = paletteWithBackground(0, 0xffff, 0);
        other.setThemePalette(Sync::ThemePalette { .live = green, .baseline = green });
        Screen::deliver(host, otherIndex, other);
        settle();
        CHECK(session.term->palette.defaultBackground == green.defaultBackground);
        live.setThemePalette(Sync::ThemePalette { .live = dark, .baseline = dark });
        Screen::deliver(host, liveIndex, live);
        settle();
        CHECK_MSG(session.term->palette.defaultBackground == dark.defaultBackground, "the fallback followed connection order rather than the latest human presentation");

        other.setThemePalette(Sync::ThemePalette { .live = light, .baseline = light });
        Screen::deliver(host, otherIndex, other);
        CHECK(session.presentedDueAt.has_value());
        Screen::supersedeScreen(host, otherIndex);
        settle();
        CHECK_MSG(session.term->palette.defaultBackground == dark.defaultBackground, "a superseded physical connection supplied the fallback theme");

        const auto expiredIndex = attachHuman();
        UbiquittyTest::ScreenClient expired(24, 80, Sync::ID128 { 123, 1 });
        Screen::greet(host, expiredIndex, expired);
        expired.setThemePalette(Sync::ThemePalette { .live = light, .baseline = light });
        Screen::deliver(host, expiredIndex, expired);
        Screen::setTokenExpiry(host, expiredIndex, 1);
        settle();
        CHECK_MSG(session.term->palette.defaultBackground == dark.defaultBackground, "an expired token supplied the fallback theme before the expiry sweep");

        for (const bool isAgent : { false, true }) {
            const auto index = Screen::attach(host, session.id);
            Screen::grantScope(host, index, isAgent ? Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Drive } : Sync::ScopeSet { Sync::Scope::Observe });
            Screen::setTokenID(host, index, "non-presenter-" + std::to_string(index));
            if (isAgent) {
                Screen::setAgentLabel(host, index, "non-presenter");
                Screen::grantAgentSession(host, session, index);
            } else
                Screen::markInteractiveUser(host, index);
            UbiquittyTest::ScreenClient observer(24, 80, Sync::ID128 { 124, isAgent ? 2U : 1U }, isAgent ? Sync::Role::Agent : Sync::Role::Viewer);
            Screen::greet(host, index, observer);
            observer.setThemePalette(Sync::ThemePalette { .live = green, .baseline = green });
            Screen::deliver(host, index, observer);
            settle();
            CHECK_MSG(session.term->palette.defaultBackground == dark.defaultBackground, "an agent or Observe-only client supplied the fallback theme");
        }
        CHECK(session.screenSource->geometry().drivingOwner() == owner);
        CHECK(session.term->textAreaPixelWidth == 800 && session.term->textAreaPixelHeight == 480);
        CHECK(!session.presentedDueAt.has_value());
    }

    // A workspace connection carries independent pane viewports. Window
    // facts, a sibling's viewport and unrelated traffic cannot replace them.
    {
        Daemon split;
        auto leftStream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        auto rightStream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        auto& leftPTY = *leftStream;
        auto& rightPTY = *rightStream;
        auto& left = Screen::makeSession(split, 38, 40, std::move(leftStream));
        auto& right = Screen::makeSession(split, 38, 64, std::move(rightStream));
        Screen::nameWorkspace(split, left, "pixel-geometry");
        Screen::splitInto(split, left, right, Ubiquitty::Panes::Orientation::Vertical);
        Screen::publish(split, left, Ubiquitty::Runtime::FrameClass::Structural);
        Screen::publish(split, right, Ubiquitty::Runtime::FrameClass::Structural);
        const auto index = Screen::attach(split, left.id);
        Screen::bindWorkspace(split, index, left);
        Screen::grantScope(split, index, { Sync::Scope::Observe, Sync::Scope::Drive });
        Screen::setTokenID(split, index, "split-owner");
        Screen::markInteractiveUser(split, index);
        UbiquittyTest::ScreenClient client(40, 130);
        Screen::greet(split, index, client);
        Screen::endReadPass(split);
        Screen::deliver(split, index, client);
        const auto* connection = Screen::connection(split, index);
        CHECK(connection && connection->boundPanes().size() == 2);
        if (!connection || connection->boundPanes().size() != 2)
            return UBIQUITTY_TEST_SUMMARY();
        const auto handleFor = [&](const auto& session) {
            for (const auto& [handle, pane] : connection->boundPanes()) {
                if (pane == session.screenSource->paneID())
                    return handle;
            }
            return std::uint32_t { 0 };
        };
        const auto send = [&](Sync::FrameKind kind, std::uint32_t handle, const auto& value) {
            std::vector<std::uint8_t> body;
            Sync::encode(body, value);
            std::vector<std::uint8_t> frame;
            CHECK(Sync::appendFrame(frame, kind, handle, body));
            Screen::receive(split, index, frame);
            Screen::endReadPass(split);
        };
        Sync::Facts window;
        window.viewportRows = 40;
        window.viewportColumns = 130;
        window.cellPixelWidth = 15;
        window.cellPixelHeight = 34;
        window.textAreaPixelWidth = 1944;
        window.textAreaPixelHeight = 1360;
        send(Sync::FrameKind::Facts, 0, window);
        CHECK_MSG(left.term->textAreaPixelWidth != 1944 && right.term->textAreaPixelWidth != 1944,
            "window pixels were applied to panes laid out beside one another");
        Sync::Viewport leftViewport { .rows = 38, .columns = 40, .cellPixelWidth = 15, .cellPixelHeight = 34, .textAreaPixelWidth = 598, .textAreaPixelHeight = 1292, .drawablePixelWidth = 610, .drawablePixelHeight = 1300 };
        Sync::Viewport rightViewport { .rows = 38, .columns = 64, .cellPixelWidth = 15, .cellPixelHeight = 34, .textAreaPixelWidth = 957, .textAreaPixelHeight = 1292, .drawablePixelWidth = 968, .drawablePixelHeight = 1300 };
        const auto checkPixels = [&] {
            CHECK_MSG(left.term->textAreaPixelWidth == leftViewport.textAreaPixelWidth
                    && left.term->drawablePixelWidth == leftViewport.drawablePixelWidth,
                "the left pane lost its own viewport pixels");
            CHECK_MSG(right.term->textAreaPixelWidth == rightViewport.textAreaPixelWidth
                    && right.term->drawablePixelWidth == rightViewport.drawablePixelWidth,
                "the right pane lost its own viewport pixels");
            for (auto* session : { &left, &right }) {
                auto& pty = session == &left ? leftPTY : rightPTY;
                const auto& viewport = session == &left ? leftViewport : rightViewport;
                const auto before = pty.written.size();
                pty.pushInbound("\033[14t\033[16t");
                Screen::servicePTY(split, *session);
                const auto reply = pty.written.substr(before);
                CHECK_MSG(reply.find("\033[4;1292;" + std::to_string(viewport.textAreaPixelWidth) + "t") != std::string::npos,
                    "CSI 14 t did not report this pane's own text extent");
                CHECK_MSG(reply.find("\033[6;34;15t") != std::string::npos,
                    "CSI 16 t did not report this pane's cell pixels");
            }
        };
        send(Sync::FrameKind::Viewport, handleFor(left), leftViewport);
        send(Sync::FrameKind::Viewport, handleFor(right), rightViewport);
        checkPixels();
        send(Sync::FrameKind::Facts, 0, window);
        checkPixels();
        leftViewport.textAreaPixelWidth = 597;
        send(Sync::FrameKind::Viewport, handleFor(left), leftViewport);
        checkPixels();
        Screen::deliver(split, index, client);
        checkPixels();
    }

    // A token's Drive scope does not admit Facts before the screen greeting.
    {
        Daemon premature;
        auto& session = Screen::makeSession(premature, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
        Screen::publish(premature, session, Ubiquitty::Runtime::FrameClass::Structural);
        const auto before = session.term->palette;
        const auto index = Screen::attach(premature, session.id);
        Screen::grantScope(premature, index, { Sync::Scope::Observe, Sync::Scope::Drive });
        Screen::markInteractiveUser(premature, index);
        const auto theme = paletteWithBackground(0xabcd, 0x1234, 0x5678);
        Sync::Facts facts;
        facts.themePalette = Sync::ThemePalette { .live = theme, .baseline = theme };
        std::vector<std::uint8_t> body;
        Sync::encode(body, facts);
        std::vector<std::uint8_t> frame;
        CHECK(Sync::appendFrame(frame, Sync::FrameKind::Facts, 0, body));
        Screen::receive(premature, index, frame);
        CHECK(Screen::connection(premature, index)->state() == Sync::ConnectionState::Closed);
        CHECK(session.term->palette == before);
    }

    // Presentation facts belong to their attachment and grant no Drive
    // authority. Neither an observer nor an agent may stand in for a person.
    for (const bool isHuman : { false, true }) {
        Daemon observerHost;
        auto& observed = Screen::makeSession(observerHost, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
        Screen::publish(observerHost, observed, Ubiquitty::Runtime::FrameClass::Structural);
        const auto before = observed.term->palette;
        const auto index = Screen::attach(observerHost, observed.id);
        Screen::grantScope(observerHost, index, isHuman ? Sync::ScopeSet { Sync::Scope::Observe } : Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Drive });
        Screen::setTokenID(observerHost, index, "non-presenter");
        if (isHuman)
            Screen::markInteractiveUser(observerHost, index);
        else {
            Screen::setAgentLabel(observerHost, index, "palette-agent");
            // No person is attached to approve it, so the agent holds the
            // standing grant a person's allow-for-this-session answer gives.
            Screen::grantAgentSession(observerHost, observed, index);
        }
        UbiquittyTest::ScreenClient client(24, 80);
        Screen::greet(observerHost, index, client);
        const auto theme = paletteWithBackground(0xabcd, 0x1234, 0x5678);
        client.setThemePalette(Sync::ThemePalette { .live = theme, .baseline = theme });
        Screen::deliver(observerHost, index, client);
        CHECK(Screen::connection(observerHost, index)->facts().themePalette.has_value());
        CHECK_MSG(observed.term->palette == before, "a client without human Drive authority presented before a geometry owner");
    }

    // Before anyone types, two people present their themes one right after
    // the other. The second waits out the window the first opened and then
    // applies; and a presentation that waits is never left waiting, since the
    // daemon's loop wakes for it until it is gone.
    for (const Departure departure : { Departure::Reaped, Departure::Superseded, Departure::Malformed }) {
        Daemon early;
        auto& fresh = Screen::makeSession(early, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
        Screen::publish(early, fresh, Ubiquitty::Runtime::FrameClass::Structural);
        const Sync::ScopeSet scope { Sync::Scope::Observe, Sync::Scope::Drive };
        const std::size_t firstIndex = Screen::attach(early, fresh.id);
        Screen::grantScope(early, firstIndex, scope);
        Screen::setTokenID(early, firstIndex, "first");
        Screen::markInteractiveUser(early, firstIndex);
        const std::size_t secondIndex = Screen::attach(early, fresh.id);
        Screen::grantScope(early, secondIndex, scope);
        Screen::setTokenID(early, secondIndex, "second");
        Screen::markInteractiveUser(early, secondIndex);
        // They have Drive authority but do not claim the geometry role;
        // this exercises the stand-in path rather than last-viewport wins.
        UbiquittyTest::ScreenClient first(24, 80, 0, Sync::LinkCost::Unmetered, Sync::Role::Viewer);
        UbiquittyTest::ScreenClient second(24, 80, 0, Sync::LinkCost::Unmetered, Sync::Role::Viewer);
        Screen::greet(early, firstIndex, first);
        Screen::greet(early, secondIndex, second);
        std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
        const auto present = [&](std::size_t index, const Ubiquitty::TermPalette& palette) {
            auto& client = index == firstIndex ? first : second;
            client.setThemePalette(Sync::ThemePalette { .live = palette, .baseline = palette });
            Screen::deliver(early, index, client);
        };
        const Ubiquitty::TermPalette firstTheme = paletteWithBackground(0x1111, 0x2222, 0x3333);
        const Ubiquitty::TermPalette secondTheme = paletteWithBackground(0xaaaa, 0xbbbb, 0xcccc);
        present(firstIndex, firstTheme);
        CHECK_MSG(fresh.term->palette.defaultBackground == firstTheme.defaultBackground, "premise: a lone presenter's theme did not apply at once");
        present(secondIndex, secondTheme);
        std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
        Screen::endReadPass(early);
        CHECK_MSG(fresh.term->palette.defaultBackground == secondTheme.defaultBackground, "a theme presented inside another's window never applied");
        CHECK_MSG(!fresh.presentedDueAt.has_value(), "a presentation that applied was left due");

        // The one whose theme waits goes before the window ends: nothing
        // stays due for a client that is gone.
        present(firstIndex, paletteWithBackground(0x4444, 0x5555, 0x6666));
        CHECK_MSG(fresh.presentedDueAt.has_value(), "premise: a theme presented inside the other's window did not wait");
        switch (departure) {
        case Departure::Reaped:
            Screen::reap(early, firstIndex);
            break;
        case Departure::Superseded:
            Screen::supersedeScreen(early, firstIndex);
            break;
        case Departure::Malformed: {
            std::vector<std::uint8_t> invalid;
            CHECK(Sync::appendFrame(invalid, Sync::FrameKind::Raw, 1, {}));
            Screen::receive(early, firstIndex, invalid);
            CHECK(Screen::connection(early, firstIndex)->state() == Sync::ConnectionState::Closed);
            break;
        }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
        Screen::endReadPass(early);
        CHECK_MSG(!fresh.presentedDueAt.has_value(), "a presentation of a client that is gone was left due");
        CHECK_MSG(fresh.term->palette.defaultBackground == secondTheme.defaultBackground, "a client that is gone presented for the pane");
    }

    Daemon d;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
    auto& session = Screen::makeSession(d, 24, 80, std::move(stream));
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    const std::size_t aIndex = Screen::attach(d, session.id);
    Screen::grantScope(d, aIndex, driving);
    Screen::setTokenID(d, aIndex, "a");
    Screen::markInteractiveUser(d, aIndex);
    const std::size_t bIndex = Screen::attach(d, session.id);
    Screen::grantScope(d, bIndex, driving);
    Screen::setTokenID(d, bIndex, "b");
    Screen::markInteractiveUser(d, bIndex);
    UbiquittyTest::ScreenClient a(24, 80, Sync::ID128 { 3, 103 });
    UbiquittyTest::ScreenClient b(24, 80, Sync::ID128 { 4, 104 });
    Screen::greet(d, aIndex, a);
    Screen::greet(d, bIndex, b);

    // What the program is told when it asks for its text area in pixels and
    // for its background colour.
    const auto ask = [&] {
        Screen::endReadPass(d);
        const std::size_t before = pty.written.size();
        pty.pushInbound("\033[14t\033]11;?\007");
        Screen::servicePTY(d, session);
        return pty.written.substr(before);
    };

    // A presents its pixels and its theme, and types: A owns the size.
    a.setThemePalette(Sync::ThemePalette { .live = paletteWithBackground(0x1010, 0x2020, 0x3030), .baseline = paletteWithBackground(0x1010, 0x2020, 0x3030) });
    Screen::deliver(d, aIndex, a);
    CHECK(a.sendViewport(viewportOfCells(10, 20)));
    CHECK(a.sendRaw(space).has_value());
    Screen::deliver(d, aIndex, a);
    // The theme applied at once; the pixels behind it wait out its window.
    std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
    const std::string answeredForA = ask();
    CHECK_MSG(answeredForA.find("\033[4;480;800t") != std::string::npos, ("premise: the owner's text area was not reported; the program read " + answeredForA).c_str());
    CHECK_MSG(answeredForA.find("]11;rgb:") != std::string::npos, ("premise: the background was not reported; the program read " + answeredForA).c_str());

    // B, which has not typed, presents other pixels and another theme, once
    // A's window is long past: nothing but ownership stands in its way.
    std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
    CHECK(b.sendViewport(viewportOfCells(7, 14)));
    b.setThemePalette(Sync::ThemePalette { .live = paletteWithBackground(0xf0f0, 0xe0e0, 0xd0d0), .baseline = paletteWithBackground(0xf0f0, 0xe0e0, 0xd0d0) });
    Screen::deliver(d, bIndex, b);
    const std::string answeredWithB = ask();
    CHECK_MSG(answeredWithB == answeredForA, ("a client that does not own the size changed the program's replies: " + answeredWithB).c_str());

    // B types; once the window passes, B owns the size and its replies.
    std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
    CHECK(b.sendRaw(space).has_value());
    Screen::deliver(d, bIndex, b);
    std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
    const std::string answeredForB = ask();
    CHECK_MSG(answeredForB.find("\033[4;336;560t") != std::string::npos, ("the new owner's text area was not reported; the program read " + answeredForB).c_str());
    CHECK_MSG(answeredForB != answeredForA, "the new owner's theme did not reach the program's replies");

    // Two owners with different themes typing in turn every 10 ms move the
    // program's palette at most once per window, as they move its size.
    const std::uint64_t paletteChangesBefore = session.term->paletteEpoch;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (int turn = 0; std::chrono::steady_clock::now() < end; ++turn) {
        if (turn % 2 == 0) {
            CHECK(a.sendRaw(space).has_value());
            Screen::deliver(d, aIndex, a);
        } else {
            CHECK(b.sendRaw(space).has_value());
            Screen::deliver(d, bIndex, b);
        }
        Screen::endReadPass(d);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::uint64_t paletteChanges = session.term->paletteEpoch - paletteChangesBefore;
    const std::uint64_t window = Sync::measuredGeometryPolicy().coalesceMicroseconds;
    CHECK_MSG(paletteChanges >= 2, "two owners in turn never moved the program's palette");
    CHECK_MSG(paletteChanges <= 1'000'000 / window + 1, (std::to_string(paletteChanges) + " palette changes in a second of two typists").c_str());

    if (failures == 0)
        std::printf("owner_presents: geometry ownership, live human colour fallback and coalesced presentation pass\n");
    return failures == 0 ? 0 : 1;
}
