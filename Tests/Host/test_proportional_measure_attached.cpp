// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/test_proportional_measure_attached.cpp — a program on a
// host-attached pane is answered an OSC 66 measure in the faces of the people
// who may drive it (Docs/proportional-text.md, "A pane a host runs").
//
// The host draws nothing, so it answers from the faces each client publishes
// once attached, which the daemon gives the pane's measurer before it reads
// the program's output. Only a person whose scope allows driving the pane
// sets its layout: an observer's faces are not counted. With two drivers the
// answer is the wider face's, so the run fits on both; when the wider leaves,
// the narrower's; and once no driver views the pane it answers in the grid's
// width again.
//
// Friend seam, built with the daemon's sources as test_daemon_client_io is.
// The session is reached through `auto`, never by its private type's name.
//

#include "Harness.h"

#include "Core/OSC/TextSizing.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HandoverTables.h"
#include "Platform/StreamDouble.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <span>
#include <string>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

/// A client's measurer whose letters are `advance` pixels in eight-pixel
/// cells, wider than the cells as a text face beside a monospace grid is.
struct WideFaceMeasurer final : Ubiquitty::TextMeasureClient {
    explicit WideFaceMeasurer(float letter)
        : advance(letter)
    {
    }
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        const std::size_t count = std::min(codePoints.size(), glyphsOut.size());
        for (std::size_t index = 0; index < count; ++index)
            glyphsOut[index] = { .glyph = codePoints[index], .advanceInPixels = advance, .cluster = static_cast<std::uint16_t>(index) };
        return static_cast<int>(count);
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
    float advance;
};

/// The faces a client with `letter`-pixel letters publishes.
Ubiquitty::OSC::TextSizing::ProportionalFaceFacts facesOf(float letter)
{
    WideFaceMeasurer measurer(letter);
    Ubiquitty::OSC::TextSizing::ProportionalFaceFacts faces;
    CHECK_MSG(Ubiquitty::OSC::TextSizing::measureProportionalFaces(measurer, faces), "premise: the client's measurer gave faces");
    return faces;
}

} // namespace

TEST(aHostedPaneIsAnsweredInTheWidestFaceOfThePeopleWhoMayDriveIt)
{
    Daemon d;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
    auto& session = Screen::makeSession(d, 6, 40, std::move(stream));
    session.term->drawsSizedText = true;
    session.term->drawsProportionalText = true;
    Screen::installTerminalClients(session);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);

    // What the program reads back for a measure of `Name`, through the pass
    // that reads the query.
    const auto askName = [&] {
        const std::size_t before = pty.written.size();
        pty.pushInbound("\x1b]66;p=1:q=1:i=1;Name\x1b\\");
        Screen::servicePTY(d, session);
        return pty.written.substr(before);
    };
    // Attaches a person's client with `scope` and has it publish the faces
    // of `letter`-pixel letters.
    const auto attachPublishing = [&](UbiquittyTest::ScreenClient& client, Ubiquitty::Protocol::Sync::ScopeSet scope, float letter) {
        const std::size_t index = Screen::attach(d, session.id);
        Screen::grantScope(d, index, scope);
        Screen::markInteractiveUser(d, index);
        client.publishesFaces();
        Screen::greet(d, index, client);
        CHECK_MSG(client.connection().state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached, "premise: the client attached");
        client.publishProportionalFaces(facesOf(letter));
        Screen::deliver(d, index, client);
        return index;
    };
    const Ubiquitty::Protocol::Sync::ScopeSet driving { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive };
    const Ubiquitty::Protocol::Sync::ScopeSet observing { Ubiquitty::Protocol::Sync::Scope::Observe };

    const std::string unviewed = askName();
    CHECK_MSG(unviewed == "\x1b]66;i=1:c=4\x1b\\", ("premise: a pane no client has viewed answers in the grid's width, " + unviewed.substr(1)).c_str());

    // 1. An observer's faces, however wide, do not set the pane's layout.
    UbiquittyTest::ScreenClient observer(6, 40);
    (void)attachPublishing(observer, observing, 30.0f);
    const std::string observed = askName();
    CHECK_MSG(observed == "\x1b]66;i=1:c=4\x1b\\", ("an observer's faces are not counted, " + observed.substr(1)).c_str());

    // 2. A driver's faces measure the pane: four 10.4-pixel letters take six
    // 8-pixel cells.
    UbiquittyTest::ScreenClient first(6, 40);
    const std::size_t firstIndex = attachPublishing(first, driving, 10.4f);
    const std::string inFirst = askName();
    CHECK_MSG(inFirst == "\x1b]66;i=1:c=6\x1b\\", ("the driver's face answers, " + inFirst.substr(1)).c_str());

    // 3. A second driver with wider letters: the wider face answers, so the
    // run fits on both.
    UbiquittyTest::ScreenClient second(6, 40);
    const std::size_t secondIndex = attachPublishing(second, driving, 13.0f);
    const std::string inBoth = askName();
    CHECK_MSG(inBoth == "\x1b]66;i=1:c=7\x1b\\", ("the wider face answers, " + inBoth.substr(1)).c_str());

    // 4. The wider driver leaves, and the face still driving the pane answers.
    Screen::drop(d, secondIndex);
    const std::string afterWider = askName();
    CHECK_MSG(afterWider == "\x1b]66;i=1:c=6\x1b\\", ("the remaining driver's face answers, " + afterWider.substr(1)).c_str());

    // 5. The last driver leaves: its faces go with it, and with only an
    // observer left the pane answers in the grid's width.
    Screen::drop(d, firstIndex);
    const std::string afterDrivers = askName();
    CHECK_MSG(afterDrivers == "\x1b]66;i=1:c=4\x1b\\", ("no driver's faces outlive it, " + afterDrivers.substr(1)).c_str());
}

TEST(twoDriversKeepTheSameProportionalAnswerBeforeAndAfterHostHandover)
{
    Daemon predecessor;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    auto& pty = *stream;
    auto& session = Screen::makeSession(predecessor, 6, 40, std::move(stream));
    session.term->drawsSizedText = true;
    session.term->drawsProportionalText = true;
    Screen::installTerminalClients(session);
    Screen::publish(predecessor, session, Ubiquitty::Runtime::FrameClass::Structural);
    const auto ask = [](Daemon& daemon, auto& pane, Ubiquitty::TestDoubles::StreamDouble& stream) {
        const auto before = stream.written.size();
        stream.pushInbound("\x1b]66;p=1:q=1:i=1;Name\x1b\\");
        Screen::servicePTY(daemon, pane);
        return stream.written.substr(before);
    };
    const auto attach = [&](UbiquittyTest::ScreenClient& client, float letter) {
        const auto index = Screen::attach(predecessor, session.id);
        Screen::grantScope(predecessor, index, Ubiquitty::Protocol::Sync::Scope::Observe | Ubiquitty::Protocol::Sync::Scope::Drive);
        Screen::markInteractiveUser(predecessor, index);
        client.publishesFaces();
        Screen::greet(predecessor, index, client);
        CHECK(client.connection().state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached);
        client.publishProportionalFaces(facesOf(letter));
        Screen::deliver(predecessor, index, client);
    };
    UbiquittyTest::ScreenClient first(6, 40);
    UbiquittyTest::ScreenClient second(6, 40);
    attach(first, 10.4f);
    attach(second, 13.0f);
    const std::string expected = ask(predecessor, session, pty);
    REQUIRE(expected == "\x1b]66;i=1:c=7\x1b\\");

    constexpr Ubiquitty::Host::HandoverAttachmentLimits limits { 2 * 1024 * 1024, 64, 128, 256, 4096, 4096, 65536 };
    auto carried = Screen::exportAttachmentAuthority(predecessor, Screen::now());
    auto encoded = Ubiquitty::Host::encodeHandoverAttachments(carried, limits);
    REQUIRE(encoded);
    auto decoded = Ubiquitty::Host::decodeHandoverAttachments(encoded->view(), limits);
    REQUIRE(decoded);
    const auto issuer = decoded->attachments.hostInstanceID;

    Daemon successor;
    auto successorStream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    auto& successorPTY = *successorStream;
    auto& resumed = Screen::makeSession(successor, 6, 40, std::move(successorStream));
    resumed.term->drawsSizedText = true;
    resumed.term->drawsProportionalText = true;
    Screen::installTerminalClients(resumed);
    REQUIRE(Screen::inheritAttachmentAuthority(successor, std::move(*decoded), issuer, Screen::now(), static_cast<std::uint64_t>(::time(nullptr))));
    CHECK(ask(successor, resumed, successorPTY) == expected);
}

UBIQUITTY_TEST_MAIN()
