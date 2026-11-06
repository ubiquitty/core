// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/rich_text_fit_attached.cpp — a program on a host-attached pane
// is answered an OSC 7404 fit query in the attached client's fonts
// (Docs/rich-text.md, "The measurement model").
//
// The client publishes its typography table after it has attached, as a
// frontend does once its renderer has measured its faces. The table travels
// in a TypographyFacts frame, the daemon gives it to the session the client
// views, and the session's fit query answers from it (x=1) instead of in
// cells (x=0). A table the client changes later replaces it. A table that
// shapes natively sends the query to the client, whose shaper measures it,
// and its answer reaches the program through the pane's PTY.
//
// Friend seam, the QUIC-off host build, as attention_seen_on_focus. Exit 0
// on success.
//

#include "Assertions.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/RichText/Blocks.h"
#include "Core/RichText/Shaping.h"
#include "Core/RichText/TypographyFacts.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

/// One face whose every letter advances 1100/2048 em in a 7x15 point cell.
Ubiquitty::RichText::TypographyFacts clientFacts(int cellWidthPoints)
{
    Ubiquitty::RichText::TypographyFacts facts;
    facts.cellWidth = cellWidthPoints * 64;
    facts.cellHeight = 15 * 64;
    facts.faceCount = 1;
    facts.rendersBlocks = 1;
    Ubiquitty::RichText::FaceMetrics& face = facts.faces[0];
    face.lineHeight = 2400;
    face.ascent = 1900;
    face.descent = 500;
    face.averageAdvance = 1100;
    face.advances.fill(1100);
    return facts;
}

/// The session's answer to a fit query for four letters at ten points.
std::string askFit(Ubiquitty::Terminal& terminal)
{
    UbiquittyTest::feed(terminal, "\x1b]7404;a=q:w=10:h=1;{\\rtf1\\fs20 aaaa}\x1b\\");
    std::array<char, 4096> reply {};
    const int length = UbiquittyTest::drain(terminal, reply);
    return std::string(reply.data(), static_cast<std::size_t>(std::max(length, 0)));
}

struct NativeMetrics {
    unsigned calls = 0;
    std::uint32_t width = 7 * 4096;
};

struct NativeFonts {
    unsigned calls = 0;
    std::array<std::string, 2> families;
};

void installFontCatalog(UbiquittyTest::ScreenClient& client, NativeFonts& fonts)
{
    auto* replica = client.replica();
    CHECK(replica);
    if (!replica)
        return;
    replica->typography.fontCatalog = { [](void* context, const Ubiquitty::RichText::FontCatalogQuery* query,
                                            const Ubiquitty::RichText::TypographyFacts*, Ubiquitty::RichText::FontCatalogPage* page) {
                                           auto& fonts = *static_cast<NativeFonts*>(context);
                                           ++fonts.calls;
                                           page->offset = query->offset;
                                           page->rendererRevision = query->rendererRevision;
                                           page->scope = Ubiquitty::RichText::FontCatalogScope::System;
                                           if (query->offset > fonts.families.size()) {
                                               page->status = Ubiquitty::RichText::FontCatalogStatus::Invalid;
                                               return true;
                                           }
                                           page->status = Ubiquitty::RichText::FontCatalogStatus::Ready;
                                           page->total = static_cast<std::uint32_t>(fonts.families.size());
                                           const auto end = std::min<std::size_t>(fonts.families.size(), query->offset + query->count);
                                           for (std::size_t index = query->offset; index < end; ++index)
                                               CHECK(page->appendFamily(fonts.families[index]));
                                           return true;
                                       },
        &fonts };
}

void installShaper(UbiquittyTest::ScreenClient& client, const Ubiquitty::RichText::TypographyFacts& facts, NativeMetrics& metrics)
{
    auto* replica = client.replica();
    CHECK(replica);
    if (!replica)
        return;
    CHECK(Ubiquitty::RichText::adoptTypography(*replica, facts));
    replica->typography.shaper = { [](void* context, const Ubiquitty::RichText::Document* document, const Ubiquitty::RichText::ShapeRequest*,
                                       const Ubiquitty::RichText::TypographyFacts* typography, Ubiquitty::RichText::ShapedText* output) {
                                      auto& metrics = *static_cast<NativeMetrics*>(context);
                                      ++metrics.calls;
                                      const auto end = static_cast<std::uint32_t>(document->text.size());
                                      output->appendLine({ 0, end, 0, static_cast<std::uint32_t>(typography->cellHeight) * 64, static_cast<std::uint32_t>(typography->baseline) * 64, 0, metrics.width });
                                      output->appendSpan({ 0, end, 0, 0, static_cast<std::int32_t>(metrics.width) });
                                      return true;
                                  },
        &metrics };
}

void differentRenderersKeepOneMeasurementAuthority()
{
    Daemon daemon;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    const auto& pty = *stream;
    auto& session = Screen::makeSession(daemon, 6, 30, std::move(stream));
    session.term->tickClock = &session.clock;
    session.clock.tick(Ubiquitty::Ticks::fromMilliseconds(Screen::now() / 1000));
    Screen::publish(daemon, session, Ubiquitty::Runtime::FrameClass::Structural);
    auto facts = clientFacts(7);
    facts.shaping = 1;
    facts.fontCatalogScope = Ubiquitty::RichText::FontCatalogScope::System;
    facts.baseline = 12 * 64;
    facts.proportionalSize = 12 * 64;
    const auto attach = [&](UbiquittyTest::ScreenClient& client, Ubiquitty::Protocol::Sync::ScopeSet scope) {
        const auto index = Screen::attach(daemon, session.id);
        Screen::grantScope(daemon, index, scope);
        Screen::markInteractiveUser(daemon, index);
        Screen::greet(daemon, index, client);
        return index;
    };
    using Ubiquitty::Protocol::Sync::Scope;
    UbiquittyTest::ScreenClient other(6, 30);
    const auto otherIndex = attach(other, { Scope::Observe, Scope::Drive });
    UbiquittyTest::ScreenClient observer(6, 30);
    const auto observerIndex = attach(observer, { Scope::Observe });
    observer.publishTypography(facts);
    Screen::deliver(daemon, observerIndex, observer);
    CHECK_MSG(!session.term->typography.facts, "an observer's publication cannot select application geometry");

    UbiquittyTest::ScreenClient owner(6, 30);
    const auto ownerIndex = attach(owner, { Scope::Observe, Scope::Drive });
    owner.publishTypography(facts);
    Screen::deliver(daemon, ownerIndex, owner);
    NativeMetrics ownerMetrics;
    NativeMetrics otherMetrics { .width = 14 * 4096 };
    NativeFonts ownerFonts { .families = { "Owner A", "Owner B" } };
    NativeFonts otherFonts { .families = { "Other A", "Other B" } };
    installShaper(owner, facts, ownerMetrics);
    installFontCatalog(owner, ownerFonts);
    const auto epoch = session.term->typography.epoch;

    // Equal Latin advance tables do not imply equal native shaping engines.
    other.publishTypography(facts);
    Screen::deliver(daemon, otherIndex, other);
    installShaper(other, facts, otherMetrics);
    installFontCatalog(other, otherFonts);
    CHECK_EQ(session.term->typography.epoch, epoch);
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=q:i=91:e=1:w=10:h=1;{\\rtf1 authority}\x1b\\");
    Screen::deliver(daemon, otherIndex, other);
    CHECK_EQ(otherMetrics.calls, 0U);
    const auto before = pty.written.size();
    Screen::deliver(daemon, ownerIndex, owner);
    CHECK_EQ(ownerMetrics.calls, 1U);
    CHECK_MSG(pty.written.substr(before).find("a=r:i=91:f=1:w=1:") != std::string::npos, "only the selected renderer answers the fit query");

    UbiquittyTest::feed(*session.term, "\x1b]7404;a=f:i=101:n=1:e=1;\x1b\\");
    Screen::deliver(daemon, otherIndex, other);
    CHECK_EQ(otherFonts.calls, 0U);
    const auto beforeFonts = pty.written.size();
    Screen::deliver(daemon, ownerIndex, owner);
    CHECK_EQ(ownerFonts.calls, 1U);
    CHECK_MSG(pty.written.substr(beforeFonts).find(":F=2:b=0:n=1:t=2:s=ok;4f776e65722041") != std::string::npos,
        "font discovery uses the selected client's actual catalog");
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=f:i=102:E=" + std::to_string(epoch) + ":b=1:n=1:e=1;\x1b\\");
    Screen::deliver(daemon, ownerIndex, owner);
    CHECK_EQ(ownerFonts.calls, 2U);
    CHECK(pty.written.find(":F=2:b=1:n=1:t=2:s=ok;4f776e65722042") != std::string::npos);

    auto different = facts;
    different.cellWidth = 11 * 64;
    other.publishTypography(different);
    Screen::deliver(daemon, otherIndex, other);
    installShaper(other, different, otherMetrics);
    CHECK_EQ(session.term->typography.epoch, epoch);
    CHECK(*session.term->typography.facts == facts);
    CHECK(*other.replica()->typography.facts == different);
    CHECK(*owner.replica()->typography.facts == facts);
    other.publishTypography(facts);
    Screen::deliver(daemon, otherIndex, other);
    installShaper(other, facts, otherMetrics);

    UbiquittyTest::feed(*session.term, "\x1b]7404;a=s:i=90:N=1\x1b\\");
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=q:i=92:e=1:w=10:h=1;{\\rtf1 pending}\x1b\\");
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=f:i=103:n=1:e=1;\x1b\\");
    Screen::service(daemon);
    // The former owner may already have shaped an answer in flight.
    owner.read(Screen::takeOutput(daemon, ownerIndex), Screen::now());
    const auto delayed = owner.takeStream(Screen::now());
    CHECK(!session.term->richTextMeasurements.empty());
    Screen::drop(daemon, ownerIndex);
    CHECK_EQ(session.term->typography.epoch, epoch + 1);
    CHECK(session.term->richTextMeasurements.empty());
    CHECK(*session.term->typography.facts == facts);
    Screen::handleFrames(daemon, ownerIndex, delayed);
    Screen::deliver(daemon, otherIndex, other);
    CHECK_MSG(pty.written.find("a=e:i=92:v=3:s=stale") != std::string::npos, "departure invalidates outstanding queries");
    CHECK_MSG(pty.written.find("a=r:i=92:") == std::string::npos, "a delayed answer cannot acquire the new authority's epoch");
    CHECK(pty.written.find("a=e:i=103:v=3:s=stale") != std::string::npos);
    CHECK_MSG(pty.written.find("a=f:i=103:") == std::string::npos, "a departing font catalog cannot answer after ownership changed");
    CHECK_MSG(pty.written.find("a=n:i=90:v=3:E=" + std::to_string(epoch + 1)) != std::string::npos, "the application learns that the renderer changed");

    UbiquittyTest::feed(*session.term, "\x1b]7404;a=q:i=93:e=1:w=10:h=1;{\\rtf1 successor}\x1b\\");
    Screen::deliver(daemon, otherIndex, other);
    CHECK_EQ(otherMetrics.calls, 1U);
    CHECK_MSG(pty.written.find("a=r:i=93:f=1:w=2:") != std::string::npos, "the successor's distinct native metrics now answer");
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=f:i=104:E=" + std::to_string(epoch) + ":b=1:n=1:e=1;\x1b\\");
    Screen::deliver(daemon, otherIndex, other);
    CHECK_EQ(otherFonts.calls, 0U);
    CHECK(pty.written.find("a=e:i=104:v=3:s=stale") != std::string::npos);
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=f:i=105:n=1:e=1;\x1b\\");
    Screen::deliver(daemon, otherIndex, other);
    CHECK_EQ(otherFonts.calls, 1U);
    CHECK_MSG(pty.written.find(":F=2:b=0:n=1:t=2:s=ok;4f746865722041") != std::string::npos,
        "a new query uses the successor's different OS catalog");
    Screen::grantScope(daemon, otherIndex, { Scope::Observe });
    Screen::service(daemon);
    CHECK(!session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, epoch + 2);
    CHECK_MSG(askFit(*session.term).find(":x=0:") != std::string::npos, "without an eligible driver the pane returns estimates");
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=f:i=106:n=1:e=1;\x1b\\");
    Screen::service(daemon);
    CHECK(pty.written.find(":F=0:b=0:n=0:t=0:s=unavailable;") != std::string::npos);
    CHECK_EQ(otherFonts.calls, 1U);
    owner.replica()->typography.shaper = {};
    other.replica()->typography.shaper = {};
    owner.replica()->typography.fontCatalog = {};
    other.replica()->typography.fontCatalog = {};
}

void unansweredQueriesWakeTheHostAndReleaseTheirCredits()
{
    Daemon daemon;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    const auto& pty = *stream;
    auto& session = Screen::makeSession(daemon, 6, 30, std::move(stream));
    session.term->tickClock = &session.clock;
    session.clock.tick(Ubiquitty::Ticks::fromMilliseconds(Screen::now() / 1000));
    Screen::publish(daemon, session, Ubiquitty::Runtime::FrameClass::Structural);
    const auto index = Screen::attach(daemon, session.id);
    using Ubiquitty::Protocol::Sync::Scope;
    Screen::grantScope(daemon, index, { Scope::Observe, Scope::Drive });
    Screen::markInteractiveUser(daemon, index);
    UbiquittyTest::ScreenClient client(6, 30);
    Screen::greet(daemon, index, client);
    auto facts = clientFacts(7);
    facts.shaping = 1;
    client.publishTypography(facts);
    Screen::deliver(daemon, index, client);
    const auto admittedAt = Screen::now();
    session.clock.tick(Ubiquitty::Ticks::fromMilliseconds(admittedAt / 1000));
    for (unsigned id = 1; id <= Ubiquitty::RichText::maxPendingMeasurements; ++id) {
        const auto query = "\x1b]7404;a=q:i=" + std::to_string(id) + ":e=1:w=10:h=1;{\\rtf1 waiting}\x1b\\";
        UbiquittyTest::feed(*session.term, query);
    }
    CHECK_EQ(session.term->richTextMeasurements.size(), Ubiquitty::RichText::maxPendingMeasurements);
    const auto due = Screen::nextService(daemon, admittedAt);
    CHECK_MSG(due && *due <= admittedAt + 2000000, "even unsent queries schedule a bounded host wakeup");
    Screen::service(daemon, admittedAt);
    CHECK_EQ(session.term->richTextMeasurements.size(), Ubiquitty::RichText::maxPendingMeasurements);
    CHECK(pty.written.find("s=timeout") == std::string::npos);
    // No renderer bytes or program output arrive. The timer alone must retire
    // the whole admitted queue, publish every outcome, and release its credits.
    Screen::service(daemon, admittedAt + 2000000);
    CHECK(session.term->richTextMeasurements.empty());
    for (unsigned id = 1; id <= Ubiquitty::RichText::maxPendingMeasurements; ++id) {
        const auto expected = "a=e:i=" + std::to_string(id) + ":v=3:s=timeout";
        const auto first = pty.written.find(expected);
        CHECK(first != std::string::npos);
        CHECK(first == std::string::npos || pty.written.find(expected, first + 1) == std::string::npos);
    }
    // Retiring the final viewer can publish a terminal status and remove the
    // last pending record. Those already queued bytes must still wake and
    // drain without either a connection or new program output.
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=q:i=91:e=1:w=10:h=1;{\\rtf1 departure}\x1b\\");
    CHECK_EQ(session.term->richTextMeasurements.size(), std::size_t { 1 });
    Screen::drop(daemon, index);
    CHECK(session.term->richTextMeasurements.empty());
    CHECK(session.term->bulkOutput.length > 0);
    const auto departedAt = admittedAt + 2000000;
    const auto departureDue = Screen::nextService(daemon, departedAt);
    CHECK(departureDue && *departureDue == departedAt);
    Screen::service(daemon, departedAt);
    CHECK(!session.term->bulkOutput.length);
    CHECK(pty.written.find("a=e:i=91:v=3:s=stale") != std::string::npos);
}

void unchangedOffersDoNotRepeatCandidateElections()
{
    Daemon daemon;
    auto firstStream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    auto& firstPTY = *firstStream;
    auto& first = Screen::makeSession(daemon, 6, 30, std::move(firstStream));
    auto& second = Screen::makeSession(daemon, 6, 30);
    const std::array sessions { &first, &second };
    std::array<std::unique_ptr<UbiquittyTest::ScreenClient>, 4> clients;
    std::array<std::size_t, 4> indices {};
    const auto facts = clientFacts(7);
    using Ubiquitty::Protocol::Sync::Scope;
    for (std::size_t pane = 0; pane < sessions.size(); ++pane) {
        auto& session = *sessions[pane];
        Screen::publish(daemon, session, Ubiquitty::Runtime::FrameClass::Structural);
        for (std::size_t viewer = 0; viewer < 2; ++viewer) {
            const auto slot = pane * 2 + viewer;
            indices[slot] = Screen::attach(daemon, session.id);
            Screen::grantScope(daemon, indices[slot], { Scope::Observe, Scope::Drive });
            Screen::markInteractiveUser(daemon, indices[slot]);
            clients[slot] = std::make_unique<UbiquittyTest::ScreenClient>(6, 30);
            Screen::greet(daemon, indices[slot], *clients[slot]);
            clients[slot]->publishTypography(facts);
            Screen::deliver(daemon, indices[slot], *clients[slot]);
        }
    }
    Screen::service(daemon);
    const std::array elections { first.richTextMeasurerElections, second.richTextMeasurerElections };
    const auto epoch = first.term->typography.epoch;
    for (int pass = 0; pass < 8; ++pass)
        Screen::service(daemon);
    firstPTY.pushInbound("ordinary output\r\n");
    Screen::servicePTY(daemon, first);
    CHECK_EQ(firstPTY.bytesRemaining(), 0U);
    for (std::size_t pane = 0; pane < sessions.size(); ++pane)
        CHECK_EQ(sessions[pane]->richTextMeasurerElections, elections[pane]);

    // Send a real duplicate frame, bypassing the frontend's own publication
    // deduplication. The host preserves the immutable table's identity too.
    std::vector<std::uint8_t> body;
    Ubiquitty::Protocol::Sync::encode(body, facts);
    std::vector<std::uint8_t> duplicate;
    REQUIRE(Ubiquitty::Protocol::Sync::appendFrame(duplicate, Ubiquitty::Protocol::Sync::FrameKind::TypographyFacts, 1, body));
    Screen::handleFrames(daemon, indices[0], duplicate);
    Screen::service(daemon);
    CHECK_EQ(first.richTextMeasurerElections, elections[0]);
    CHECK_EQ(second.richTextMeasurerElections, elections[1]);

    // A changed offer may cause one election, but the current eligible owner
    // remains selected and an unrelated pane is not elected again.
    auto changed = facts;
    ++changed.rendererRevision;
    clients[1]->publishTypography(changed);
    Screen::deliver(daemon, indices[1], *clients[1]);
    CHECK_EQ(first.richTextMeasurerElections, elections[0] + 1);
    CHECK_EQ(first.term->typography.epoch, epoch);
    CHECK_EQ(second.richTextMeasurerElections, elections[1]);
    for (int pass = 0; pass < 8; ++pass)
        Screen::service(daemon);
    CHECK_EQ(first.richTextMeasurerElections, elections[0] + 1);
    CHECK_EQ(second.richTextMeasurerElections, elections[1]);
}

void changedOffersAndBindingsRefreshAuthority()
{
    Daemon daemon;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    const auto& pty = *stream;
    auto& session = Screen::makeSession(daemon, 6, 30, std::move(stream));
    session.term->tickClock = &session.clock;
    session.clock.tick(Ubiquitty::Ticks::fromMilliseconds(Screen::now() / 1000));
    Screen::publish(daemon, session, Ubiquitty::Runtime::FrameClass::Structural);
    const auto index = Screen::attach(daemon, session.id);
    using Ubiquitty::Protocol::Sync::Scope;
    using Ubiquitty::Protocol::Sync::ViewState;
    Screen::grantScope(daemon, index, { Scope::Observe, Scope::Drive });
    Screen::markInteractiveUser(daemon, index);
    UbiquittyTest::ScreenClient client(6, 30);
    Screen::greet(daemon, index, client);
    auto facts = clientFacts(7);
    facts.shaping = 1;
    client.publishTypography(facts);
    Screen::deliver(daemon, index, client);
    REQUIRE(session.term->typography.facts);
    auto epoch = session.term->typography.epoch;
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=q:i=81:e=1:w=10:h=1;{\\rtf1 waiting}\x1b\\");
    REQUIRE(session.term->richTextMeasurements.size() == 1);
    ++facts.rendererRevision;
    client.publishTypography(facts);
    Screen::handleFrames(daemon, index, client.takeStream(Screen::now()));
    CHECK_EQ(session.term->typography.epoch, ++epoch);
    CHECK(session.term->richTextMeasurements.empty());
    REQUIRE(session.term->typography.facts);
    CHECK_EQ(session.term->typography.facts->rendererRevision, facts.rendererRevision);
    Screen::deliver(daemon, index, client);
    CHECK(pty.written.find("a=e:i=81:v=3:s=stale") != std::string::npos);

    client.setView(ViewState::Dormant);
    Screen::deliver(daemon, index, client);
    CHECK(!session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, ++epoch);
    client.setView(ViewState::Live);
    Screen::deliver(daemon, index, client);
    CHECK(session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, ++epoch);

    facts.rendersBlocks = 0;
    client.publishTypography(facts);
    Screen::deliver(daemon, index, client);
    CHECK(!session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, ++epoch);
    facts.rendersBlocks = 1;
    client.publishTypography(facts);
    Screen::deliver(daemon, index, client);
    CHECK(session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, ++epoch);

    Screen::grantScope(daemon, index, { Scope::Observe });
    Screen::service(daemon);
    CHECK(!session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, ++epoch);
    Screen::grantScope(daemon, index, { Scope::Observe, Scope::Drive });
    Screen::service(daemon);
    CHECK(session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, ++epoch);

    // An idle subscriber must learn that its final renderer withdrew even
    // when the subscription is erased before any later client or PTY service.
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=s:i=82:N=1\x1b\\");
    Screen::service(daemon);
    const auto beforeUnbind = pty.written.size();
    REQUIRE(Screen::unbindPane(daemon, index, session));
    CHECK(!session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, ++epoch);
    CHECK(session.term->bulkOutput.length > 0);
    Screen::service(daemon);
    CHECK(pty.written.substr(beforeUnbind).find("a=n:i=82:v=3:E=" + std::to_string(epoch)) != std::string::npos);
    // Bind before the client acknowledges the unbind, so its old handle is
    // still fenced and this offer necessarily uses another binding identity.
    const auto handle = Screen::rebindPane(daemon, index, session);
    REQUIRE(handle > 1);
    Screen::deliver(daemon, index, client);
    client.publishTypography(facts, handle);
    Screen::deliver(daemon, index, client);
    CHECK(session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, ++epoch);

    // Expiry withdraws attachment eligibility while the ordered refusal is
    // still draining. The cached offer must not survive that live-slot change.
    Screen::expireToken(daemon, index);
    CHECK(Screen::isLeaving(daemon, index));
    Screen::service(daemon);
    CHECK(!session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, ++epoch);
    Screen::service(daemon);
    const auto elections = session.richTextMeasurerElections;
    for (int pass = 0; pass < 8; ++pass)
        Screen::service(daemon);
    CHECK_EQ(session.richTextMeasurerElections, elections);
}

void anInternalCheckpointUnbindWithdrawsTheCachedOffer()
{
    Daemon daemon;
    auto& session = Screen::makeSession(daemon, 6, 30);
    Screen::publish(daemon, session, Ubiquitty::Runtime::FrameClass::Structural);
    const auto index = Screen::attach(daemon, session.id);
    using Ubiquitty::Protocol::Sync::Scope;
    Screen::grantScope(daemon, index, { Scope::Observe, Scope::Drive });
    Screen::markInteractiveUser(daemon, index);
    UbiquittyTest::ScreenClient client(6, 30);
    Screen::greet(daemon, index, client);
    client.publishTypography(clientFacts(7));
    Screen::deliver(daemon, index, client);
    REQUIRE(session.term->typography.facts);
    const auto epoch = session.term->typography.epoch;
    const auto elections = session.richTextMeasurerElections;

    // test_sync_memory_owner drives actual checkpoint admission past its cap.
    // Here the same connection-level unbind bypasses the daemon's explicit
    // unbind helper, preserving the subscription with its now-unbound handle.
    REQUIRE(Screen::unbindCheckpointPane(daemon, index, session));
    CHECK(session.term->typography.facts);
    Screen::service(daemon);
    CHECK(!session.term->typography.facts);
    CHECK_EQ(session.term->typography.epoch, epoch + 1);
    CHECK_EQ(session.richTextMeasurerElections, elections + 1);
    for (int pass = 0; pass < 8; ++pass)
        Screen::service(daemon);
    CHECK_EQ(session.richTextMeasurerElections, elections + 1);
}

} // namespace

int main()
{
    differentRenderersKeepOneMeasurementAuthority();
    unansweredQueriesWakeTheHostAndReleaseTheirCredits();
    unchangedOffersDoNotRepeatCandidateElections();
    changedOffersAndBindingsRefreshAuthority();
    anInternalCheckpointUnbindWithdrawsTheCachedOffer();
    Daemon d;
    auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
    const Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
    auto& session = Screen::makeSession(d, 6, 30, std::move(stream));
    session.term->tickClock = &session.clock;
    session.clock.tick(Ubiquitty::Ticks::fromMilliseconds(Screen::now() / 1000));
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    CHECK_MSG(askFit(*session.term).find(":x=0:") != std::string::npos, "premise: with no table the query is answered in cells");

    const auto index = Screen::attach(d, session.id);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(6, 30);
    Screen::greet(d, index, client);
    CHECK_MSG(client.connection().state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached, "premise: the client attached");

    // 1. The table the client publishes once attached measures the session.
    client.publishTypography(clientFacts(7));
    Screen::deliver(d, index, client);
    CHECK_MSG(session.term->typography.facts && *session.term->typography.facts == clientFacts(7), "the session took the client's table");
    const std::string answered = askFit(*session.term);
    // Four letters of 1100/2048 em at ten points: 21.5 points, four 7-point cells.
    CHECK_MSG(answered.find(":x=1:") != std::string::npos && answered.find(":w=4:") != std::string::npos, answered.c_str());

    // 2. A table the client changes replaces the one the session holds.
    client.publishTypography(clientFacts(11));
    Screen::deliver(d, index, client);
    CHECK_MSG(session.term->typography.facts && *session.term->typography.facts == clientFacts(11), "the session took the changed table");
    const std::string wider = askFit(*session.term);
    CHECK_MSG(wider.find(":x=1:") != std::string::npos && wider.find(":w=2:") != std::string::npos, wider.c_str()); // two 11-point cells

    // 3. A natively shaped table: the session asks the client, which shapes
    // in its own fonts, and the program reads that answer.
    Ubiquitty::RichText::TypographyFacts shaped = clientFacts(7);
    shaped.shaping = 1;
    shaped.baseline = 12 * 64;
    shaped.proportionalSize = 12 * 64;
    client.publishTypography(shaped);
    Screen::deliver(d, index, client);
    CHECK_MSG(session.term->typography.facts && *session.term->typography.facts == shaped, "the session took the shaped table");
    Ubiquitty::Terminal* replica = client.replica();
    CHECK_MSG(replica != nullptr, "premise: the client holds the pane's replica");
    if (!replica)
        return UBIQUITTY_TEST_SUMMARY();
    // The frontend's measurer, installed where the client measures.
    CHECK(Ubiquitty::RichText::adoptTypography(*replica, shaped));
    unsigned shapes = 0;
    replica->typography.shaper = { [](void* context, const Ubiquitty::RichText::Document* document, const Ubiquitty::RichText::ShapeRequest*,
                                       const Ubiquitty::RichText::TypographyFacts* facts, Ubiquitty::RichText::ShapedText* output) {
                                      ++*static_cast<unsigned*>(context);
                                      const auto end = static_cast<std::uint32_t>(document->text.size());
                                      output->appendLine({ 0, end, 0, static_cast<std::uint32_t>(facts->cellHeight) * 64, static_cast<std::uint32_t>(facts->baseline) * 64, 0, 4096 });
                                      output->appendSpan({ 0, end, 0, 0, 4096 });
                                      return true;
                                  },
        &shapes };
    const std::size_t writtenBefore = pty.written.size();
    UbiquittyTest::feed(*session.term, "\x1b]7404;a=q:i=5:w=10:h=1;{\\rtf1 shaped}\x1b\\");
    Screen::deliver(d, index, client);
    const std::string reachedProgram = pty.written.substr(writtenBefore);
    CHECK_MSG(shapes == 1, "the client's shaper measured the query once");
    CHECK_MSG(reachedProgram.find("\x1b]7404;a=r:i=5:") != std::string::npos && reachedProgram.find(":x=1:") != std::string::npos, reachedProgram.c_str());
    replica->typography.shaper = {};
    return UBIQUITTY_TEST_SUMMARY();
}
