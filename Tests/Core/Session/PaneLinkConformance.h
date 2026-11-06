// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/Core/Session/PaneLinkConformance.h — the contract every `PaneLink`
// keeps (Source/Core/Session/PaneLink.h), checked the same way for each: the
// in-process link, the remote link and the loopback link.
//
// A fixture hands the checks a link already attached to a program that writes
// back what it is written, and drives whatever carries the link:
//
//     Ubiquitty::Session::PaneLink& link();
//     bool runUntil(const std::function<bool()>& isDone); // bounded
//     void settle();                                      // until quiet, bounded
//     void readView(const std::function<void(const Ubiquitty::Session::PaneView&)>&);
//
// `readView` holds whatever lock the model needs while the reader runs, as
// the view's contract asks of its caller. The checks:
//
//   1. An attached link is Live and not stalled, on a clock that does not run
//      backwards.
//   2. Its view lends one terminal, the same on every call while nothing
//      mutates the link.
//   3. An observer is told the link's state and its newest frame once, and
//      neither again while neither changes.
//   4. Raw input with no bytes is refused. Input handed to `send` reaches
//      the program, the view shows its echo, and the observer is told of a
//      newer frame.
//   5. While the view is Dormant the observer is told of no frame; once it is
//      Live again the view shows what the program wrote meanwhile, and the
//      observer is told of it.
//   6. While the view is RecordsOnly the observer is told of no frame, not
//      even one the view's terminal already holds; once it is Live again
//      the view shows what the program wrote meanwhile, and the observer is
//      told of it.
//   7. `wantHistory` admits a valid range and refuses an empty range,
//      unsorted ranges, a priority past the last, and more ranges than one
//      want carries.
//

#include "Assertions.h"

#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/PaneLink.h"
#include "Core/Session/PaneLinkEvents.h"
#include "Core/Terminal.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UbiquittyTest {

/// What the conformance checks need of a fixture holding one attached link.
template<typename Fixture>
concept PaneLinkFixture = requires(Fixture& fixture, const std::function<bool()>& isDone,
    const std::function<void(const Ubiquitty::Session::PaneView&)>& reader) {
    { fixture.link() } -> std::same_as<Ubiquitty::Session::PaneLink&>;
    { fixture.runUntil(isDone) } -> std::same_as<bool>;
    { fixture.settle() } -> std::same_as<void>;
    { fixture.readView(reader) } -> std::same_as<void>;
};

/// Keeps what a link tells its observer.
struct PaneLinkRecorder final : Ubiquitty::Session::PaneLinkObserver {
    /// Keeps a frame the link reports ready.
    ///
    /// - Parameter frame: The frame.
    void frameReady(const Ubiquitty::Session::FrameReady& frame) override { frames.push_back(frame); }

    /// Counts a change of the pane's facts.
    void factsChanged() override { ++factsChanges; }

    /// Keeps a state the link reports.
    ///
    /// - Parameter state: The state.
    void linkStateChanged(const Ubiquitty::Session::LinkState& state) override { states.push_back(state); }

    std::vector<Ubiquitty::Session::FrameReady> frames;
    std::vector<Ubiquitty::Session::LinkState> states;
    int factsChanges = 0;
};

/// The visible grid a view lends, as text, one line per row.
///
/// - Parameter view: The view.
/// - Returns: The text, empty when the view lends no terminal.
inline std::string paneViewText(const Ubiquitty::Session::PaneView& view)
{
    std::string text;
    if (!view.terminal)
        return text;
    const Ubiquitty::Terminal& terminal = *view.terminal;
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
            const std::uint32_t codePoint = cell && cell->codePoint ? cell->codePoint : ' ';
            text.push_back(codePoint >= 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : '?');
        }
        text.push_back('\n');
    }
    return text;
}

/// Checks one link against the contract every `PaneLink` keeps.
///
/// - Parameters:
///   - fixture: Holds the link, attached to a program that writes back what
///     it is written.
///   - name: The link's name in each failure.
template<PaneLinkFixture Fixture>
void checkPaneLinkConformance(Fixture& fixture, std::string_view name)
{
    namespace Session = Ubiquitty::Session;
    namespace Sync = Ubiquitty::Protocol::Sync;
    const auto failure = [name](std::string_view what) { return std::string(name) + ": " + std::string(what); };
    const auto shows = [&fixture](std::string_view text) {
        bool isShown = false;
        fixture.readView([&](const Session::PaneView& view) { isShown = paneViewText(view).find(text) != std::string::npos; });
        return isShown;
    };
    const auto send = [](Session::PaneLink& link, std::string_view text) {
        return link.send(Session::PaneInput { .intent = Session::PaneInput::Raw {}, .localEncoding = std::span<const char>(text.data(), text.size()) });
    };
    Session::PaneLink& link = fixture.link();

    // 1. Live, not stalled, on a clock that does not run backwards.
    const std::uint64_t firstClock = link.clockMicroseconds();
    const Session::LinkState attached = link.linkState(firstClock);
    CHECK_MSG(attached.phase == Session::LinkPhase::Live, failure("an attached link is not Live").c_str());
    CHECK_MSG(!attached.isStalled(), failure("an attached link reads as stalled").c_str());
    fixture.settle();
    CHECK_MSG(link.clockMicroseconds() >= firstClock, failure("the link's clock ran backwards").c_str());

    // 2. One terminal, lent the same on every call.
    Ubiquitty::Terminal* firstTerminal = nullptr;
    Ubiquitty::Terminal* secondTerminal = nullptr;
    fixture.readView([&](const Session::PaneView& view) { firstTerminal = view.terminal; });
    fixture.readView([&](const Session::PaneView& view) { secondTerminal = view.terminal; });
    CHECK_MSG(firstTerminal, failure("an attached link lends no terminal").c_str());
    CHECK_MSG(firstTerminal == secondTerminal, failure("two views of an unchanged link lent different terminals").c_str());

    // 3. An observer is told the state and the newest frame once. A local
    // pane publishes when its coherence rule admits a frame, so the first is
    // waited for.
    PaneLinkRecorder recorder;
    link.observe(recorder);
    const auto isToldNewer = [&](std::size_t framesBefore) {
        return [&link, &recorder, framesBefore] {
            link.pollEvents(link.clockMicroseconds());
            return recorder.frames.size() > framesBefore;
        };
    };
    CHECK_MSG(fixture.runUntil(isToldNewer(0)), failure("an observer was never told a frame").c_str());
    CHECK_MSG(recorder.states.size() == 1 && recorder.states.back().phase == Session::LinkPhase::Live,
        failure("an observer was not told the link's Live state once").c_str());
    CHECK_MSG(recorder.frames.size() == 1, failure("an observer was not told the newest frame once").c_str());
    link.pollEvents(link.clockMicroseconds());
    CHECK_MSG(recorder.states.size() == 1, failure("an unchanged state was told again").c_str());
    CHECK_MSG(recorder.frames.size() == 1, failure("an unchanged frame was told again").c_str());
    if (recorder.frames.empty())
        return;

    // 4. Raw input that writes nothing is refused; input reaches the program,
    // and its echo comes back as a newer frame.
    CHECK_MSG(!send(link, {}), failure("the link admitted raw input with no bytes").c_str());
    constexpr std::string_view typed = "pane-link-conformance";
    CHECK_MSG(send(link, typed), failure("the link refused input").c_str());
    CHECK_MSG(fixture.runUntil([&] { return shows(typed); }), failure("the program's echo of the input never reached the view").c_str());
    CHECK_MSG(fixture.runUntil(isToldNewer(1)) && recorder.frames.back().generation > recorder.frames.front().generation,
        failure("the observer was not told of the echo's newer frame").c_str());

    // 5. A Dormant view is told of no frame; shown again, it catches up.
    link.setView(Sync::ViewState::Dormant);
    const std::size_t framesWhileShown = recorder.frames.size();
    constexpr std::string_view hidden = "while-dormant";
    CHECK_MSG(send(link, hidden), failure("the link refused input while its view was Dormant").c_str());
    fixture.settle();
    link.pollEvents(link.clockMicroseconds());
    CHECK_MSG(recorder.frames.size() == framesWhileShown, failure("an observer was told of a frame while the view was Dormant").c_str());
    link.setView(Sync::ViewState::Live);
    CHECK_MSG(fixture.runUntil([&] { return shows(hidden); }), failure("shown again, the view did not catch up with what the program wrote while it was Dormant").c_str());
    CHECK_MSG(fixture.runUntil(isToldNewer(framesWhileShown)), failure("shown again, the observer was not told of the newest frame").c_str());

    // 6. A RecordsOnly view is told of no frame, not even one its terminal
    // already holds: the echo of `untold` reaches the view while it is Live,
    // and no poll tells of it before the view leaves Live.
    constexpr std::string_view untold = "untold";
    CHECK_MSG(send(link, untold), failure("the link refused input").c_str());
    CHECK_MSG(fixture.runUntil([&] { return shows(untold); }), failure("the program's echo of the input never reached the view").c_str());
    link.setView(Sync::ViewState::RecordsOnly);
    const std::size_t framesBeforeRecords = recorder.frames.size();
    link.pollEvents(link.clockMicroseconds());
    CHECK_MSG(recorder.frames.size() == framesBeforeRecords, failure("an observer was told of a frame its terminal held as the view became RecordsOnly").c_str());
    constexpr std::string_view unseen = "unseen";
    CHECK_MSG(send(link, unseen), failure("the link refused input while its view was RecordsOnly").c_str());
    fixture.settle();
    link.pollEvents(link.clockMicroseconds());
    CHECK_MSG(recorder.frames.size() == framesBeforeRecords, failure("an observer was told of a frame while the view was RecordsOnly").c_str());
    link.setView(Sync::ViewState::Live);
    CHECK_MSG(fixture.runUntil([&] { return shows(unseen); }), failure("shown again, the view did not catch up with what the program wrote while it was RecordsOnly").c_str());
    CHECK_MSG(fixture.runUntil(isToldNewer(framesBeforeRecords)), failure("shown again after RecordsOnly, the observer was not told of the newest frame").c_str());

    // 7. History wants: one valid range is admitted, and malformed wants are not.
    const std::array<Sync::LineRange, 1> valid { Sync::LineRange { 0, 10 } };
    const std::array<Sync::LineRange, 1> empty { Sync::LineRange { 7, 7 } };
    const std::array<Sync::LineRange, 2> unsorted { Sync::LineRange { 20, 30 }, Sync::LineRange { 0, 10 } };
    std::vector<Sync::LineRange> tooMany;
    for (std::uint64_t index = 0; index <= Sync::maxHistoryWantRanges; ++index)
        tooMany.push_back({ 2 * index, 2 * index + 1 });
    CHECK_MSG(link.wantHistory(Sync::HistoryPriority::Viewport, valid), failure("a valid history want was refused").c_str());
    CHECK_MSG(!link.wantHistory(Sync::HistoryPriority::Viewport, empty), failure("an empty range was admitted").c_str());
    CHECK_MSG(!link.wantHistory(Sync::HistoryPriority::Viewport, unsorted), failure("unsorted ranges were admitted").c_str());
    CHECK_MSG(!link.wantHistory(static_cast<Sync::HistoryPriority>(255), valid), failure("a priority past the last was admitted").c_str());
    CHECK_MSG(!link.wantHistory(Sync::HistoryPriority::Viewport, tooMany), failure("more ranges than one want carries were admitted").c_str());
    fixture.settle();
}

} // namespace UbiquittyTest
