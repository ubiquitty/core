// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Sync/HandoverGround.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/ParseState.h"
#include "Core/VT/Parser.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string_view>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

namespace {

std::unique_ptr<Terminal> terminal()
{
    auto result = std::make_unique<Terminal>();
    resize(*result, 8, 40);
    return result;
}

void increment(void* count) { ++*static_cast<int*>(count); }

} // namespace

TEST(groundAndAnOpenGraphemeDoNotGetResetByInspection)
{
    auto model = terminal();
    CHECK(terminalHandoverBlockers(*model).isEmpty());
    model->modes.graphemeClustering = true;
    processBytes(model.get(), "e\xcc\x81");
    REQUIRE(VT::isParserAtGround(model.get()));
    REQUIRE(model->parser.clusterActive);
    const auto length = model->parser.clusterCodepointLength;
    const auto last = model->parser.clusterLast;
    CHECK(terminalHandoverBlockers(*model).isEmpty());
    CHECK(model->parser.clusterActive);
    CHECK_EQ(model->parser.clusterCodepointLength, length);
    CHECK_EQ(model->parser.clusterLast, last);
    model->parser.parseState = VT::Parser::State::VT52Ground;
    CHECK(terminalHandoverBlockers(*model).isEmpty());
    CHECK(model->parser.parseState == VT::Parser::State::VT52Ground);
}

TEST(partialParserSequencesAreRefusedWithoutSyntheticTerminators)
{
    for (const std::string_view prefix : { "\x1b[31", "\x1b]0;title", "\x1bPq~", "\x1b_Gf=32;AAAA" }) {
        auto model = terminal();
        processBytes(model.get(), prefix);
        const auto state = model->parser.parseState;
        const auto parameters = model->parser.parameterCount;
        const auto oscLength = model->parser.osc.length;
        const auto apcLength = model->images.kittyAPC.length;
        const auto sixelLength = model->images.sixelAccumulatorLength;
        CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::ParserMidSequence));
        CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::ParserMidSequence));
        CHECK(model->parser.parseState == state);
        CHECK_EQ(model->parser.parameterCount, parameters);
        CHECK_EQ(model->parser.osc.length, oscLength);
        CHECK_EQ(model->images.kittyAPC.length, apcLength);
        CHECK_EQ(model->images.sixelAccumulatorLength, sixelLength);
    }
}

TEST(parserGroundIsNotSufficientForIncompleteCharacterOrLegacyGrammar)
{
    auto model = terminal();
    processBytes(model.get(), "\xc3");
    REQUIRE(VT::isParserAtGround(model.get()));
    REQUIRE(model->parser.utf8.need != 0);
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::IncompleteCharacter));
    CHECK_EQ(model->parser.utf8.need, 1u);
    processBytes(model.get(), "\xa9");
    CHECK(!terminalHandoverBlockers(*model).contains(HandoverBlocker::IncompleteCharacter));
    Unicode::encReset(&model->parser.inputEncoding, Unicode::Encoding::ShiftJis);
    std::uint32_t scalar = 0;
    REQUIRE(Unicode::encFeed(&model->parser.inputEncoding, 0x82, &scalar) == Unicode::EncStatus::Need);
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::IncompleteCharacter));
    CHECK(model->parser.inputEncoding.enc == Unicode::Encoding::ShiftJis);
    CHECK_EQ(model->parser.inputEncoding.need, 1u);
    model->legacyState = 2;
    model->legacyAddrRow = 7;
    model->dcsTmuxPassthroughDepth = 1;
    const auto blockers = terminalHandoverBlockers(*model);
    CHECK(blockers.contains(HandoverBlocker::LegacySequence));
    CHECK(blockers.contains(HandoverBlocker::DeviceControlString));
    CHECK_EQ(model->legacyState, 2);
    CHECK_EQ(model->legacyAddrRow, 7);
    CHECK_EQ(model->dcsTmuxPassthroughDepth, 1);
}

TEST(clipboardTransferAndReadWorkAreNeverDiscarded)
{
    auto model = terminal();
    REQUIRE(model->osc52Chunk.append("kept", 64));
    model->osc52ChunkActive = true;
    model->osc52ChunkOverflow = true;
    model->isOSC52ClipboardReadInFlight = true;
    model->isPendingClipboardReadExtended = true;
    model->isExtendedClipboardWriteActive = true;
    model->extendedClipboardWriteItemCount = 1;
    model->extendedClipboardWriteTotalBytes = 3;
    REQUIRE(model->extendedClipboardWriteItems[0].append("abc", 64));
    const auto blockers = terminalHandoverBlockers(*model);
    CHECK(blockers.contains(HandoverBlocker::ClipboardChunkOpen));
    CHECK(blockers.contains(HandoverBlocker::ClipboardReadPending));
    CHECK(blockers.contains(HandoverBlocker::ExtendedClipboardOpen));
    CHECK(model->osc52ChunkActive);
    CHECK(model->osc52ChunkOverflow);
    CHECK_EQ(model->osc52Chunk.length, 4);
    CHECK(model->isOSC52ClipboardReadInFlight);
    CHECK(model->isExtendedClipboardWriteActive);
    CHECK_EQ(model->extendedClipboardWriteItemCount, 1);
    CHECK_EQ(model->extendedClipboardWriteTotalBytes, 3u);
    CHECK_EQ(model->extendedClipboardWriteItems[0].length, 3);
}

TEST(clipboardBase64BetweenPacketsBlocksHandoverUntilCommit)
{
    auto model = terminal();
    processBytes(model.get(), "\033]5522;type=write\033\\\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;YQ\033\\");
    CHECK_EQ(model->extendedClipboardBase64TailLength, 2);
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::ExtendedClipboardOpen));
    processBytes(model.get(), "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;==\033\\");
    CHECK_EQ(model->extendedClipboardBase64TailLength, 0);
    CHECK(model->isExtendedClipboardBase64Complete);
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::ExtendedClipboardOpen));
    processBytes(model.get(), "\033]5522;type=wdata\033\\");
    CHECK(!model->isExtendedClipboardBase64Complete);
    CHECK(!terminalHandoverBlockers(*model).contains(HandoverBlocker::ExtendedClipboardOpen));
}

TEST(imageChunkBetweenAPCsBlocksButCompletedBufferStorageDoesNot)
{
    auto model = terminal();
    REQUIRE(model->images.kittyAPC.append("Gf=32", 64));
    REQUIRE(VT::isParserAtGround(model.get()));
    CHECK(terminalHandoverBlockers(*model).isEmpty());
    REQUIRE(model->images.kittyChunk.append("AAAA", 64));
    model->images.kittyChunkActive = true;
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::ImageTransferOpen));
    CHECK_EQ(model->images.kittyChunk.length, 4);
    CHECK(model->images.kittyChunkActive);
}

TEST(idleDecodeQueuesDoNotBlockAndInspectionNeverRunsCompletions)
{
    auto model = terminal();
    model->asyncDecodes.kittyDecode = std::make_unique<TaskQueue>();
    CHECK(terminalHandoverBlockers(*model).isEmpty());
    int calls = 0;
    REQUIRE(model->asyncDecodes.kittyDecode->submit(increment, &calls));
    CHECK_EQ(calls, 1);
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::ImageDecodePending));
    CHECK_EQ(calls, 1);
    CHECK_EQ(model->asyncDecodes.kittyDecode->pendingCount(), 1);
    CHECK_EQ(model->asyncDecodes.kittyDecode->drain(increment), 1);
    CHECK_EQ(calls, 2);
    CHECK(terminalHandoverBlockers(*model).isEmpty());
}

TEST(notificationAndFontOperationsMustFinishNaturally)
{
    auto model = terminal();
    REQUIRE(!model->kittyNotifications.accept("i=1:d=0;kept", std::nullopt));
    REQUIRE(model->fontRequests.append("mono", 12.0, 0.0));
    const auto blockers = terminalHandoverBlockers(*model);
    CHECK(blockers.contains(HandoverBlocker::NotificationOpen));
    CHECK(blockers.contains(HandoverBlocker::FontOperationPending));
    CHECK_EQ(model->kittyNotifications.openCount(), 1u);
    CHECK_EQ(model->fontRequests.pendingCount(), 1u);
    REQUIRE(model->kittyNotifications.accept("i=1;p", std::nullopt));
    const auto lease = model->fontRequests.begin();
    REQUIRE(model->fontRequests.acknowledge(lease.receipt, OSC::FontAdoptionOutcome::Applied, "mono", 12.0));
    CHECK(terminalHandoverBlockers(*model).isEmpty());
    CHECK(model->fontRequests.actualFamily() == "mono");
}

TEST(synchronizedOutputAndDeferredScreenChangesAreNotFlushed)
{
    auto model = terminal();
    model->modes.syncOutput = true;
    model->scrollLaneRun.active = true;
    model->bandScrollRun.active = true;
    model->pendingHistoryRewrapColumns = 60;
    const auto blockers = terminalHandoverBlockers(*model);
    CHECK(blockers.contains(HandoverBlocker::SynchronizedOutput));
    CHECK(blockers.contains(HandoverBlocker::DeferredScreenMutation));
    CHECK(model->modes.syncOutput);
    CHECK(model->scrollLaneRun.active);
    CHECK(model->bandScrollRun.active);
    CHECK_EQ(model->pendingHistoryRewrapColumns, 60);
    // The fixture installed no actual run; restore its synthetic active flags.
    model->scrollLaneRun.active = false;
    model->bandScrollRun.active = false;
}

TEST(opaqueLogSinksAreNeitherClosedNorReopened)
{
    auto model = terminal();
    std::FILE* sink = std::tmpfile();
    REQUIRE(sink);
    model->sessionLogFile = sink;
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::ExternalSinkOpen));
    CHECK(model->sessionLogFile == sink);
    CHECK(std::fputs("still open", sink) >= 0);
    model->sessionLogFile = nullptr;
    CHECK_EQ(std::fclose(sink), 0);
    CHECK(terminalHandoverBlockers(*model).isEmpty());
}

TEST(tmuxPartialPrefixesAndActiveControlStateAreNotReinitialized)
{
    auto model = terminal();
    auto& tmux = model->integration.tmux;
    tmux.enabled = true;
    tmux.autoDetect = true;
    CHECK(terminalHandoverBlockers(*model).isEmpty());
    tmux.beginPrefixPosition = 3;
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::IntegrationActive));
    CHECK_EQ(tmux.beginPrefixPosition, 3u);
    tmux.beginPrefixPosition = 0;
    tmux.state = Protocol::Tmux::TmuxState::Line;
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::IntegrationActive));
    CHECK(tmux.state == Protocol::Tmux::TmuxState::Line);
    CHECK(tmux.enabled);
    CHECK(tmux.autoDetect);
}

TEST(anAlignedTabStringKeepsHandoverPendingUntilItsTerminator)
{
    auto model = terminal();
    processBytes(model.get(), "\033[3g\033[9 a\tAB");
    CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::DeferredScreenMutation));
    processBytes(model.get(), "C\r");
    CHECK(terminalHandoverBlockers(*model).isEmpty());
}

UBIQUITTY_TEST_MAIN()

TEST(erasureAndRenditionModesAndQualifiedAreasKeepHandoverPendingUntilTheyEnd)
{
    for (const std::string_view start : { "\x1b[6h", "\x1b[21l", "\x1b[0o" }) {
        auto model = terminal();
        CHECK(terminalHandoverBlockers(*model).isEmpty());
        processBytes(model.get(), start);
        CHECK(terminalHandoverBlockers(*model).contains(HandoverBlocker::ErasureQualification));
        processBytes(model.get(), "\x1b[!p");
        // A soft reset returns both modes; a qualified area stays until the page is cleared.
        CHECK_EQ(terminalHandoverBlockers(*model).contains(HandoverBlocker::ErasureQualification), start == "\x1b[0o");
        processBytes(model.get(), "\x1b[2J");
        CHECK(!terminalHandoverBlockers(*model).contains(HandoverBlocker::ErasureQualification));
    }
}
