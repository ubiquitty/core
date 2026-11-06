// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Exercises hostile consent Calls, human questions, own-status, resolutions and
// the canonical attachment roster through production decoders. It proves parser
// and framing invariants, not permission to act: authority is checked by the
// live host adapter and is not supplied by a well-formed decision.
//
// The local adopt record and the whole-terminal body it carries are covered
// independently of consent. No host, descriptor transfer, process, or
// transport is started.
// ASAN+UBSAN: -fsanitize=fuzzer,address,undefined.
//
// Selector byte (data[0] % 9):
//   0 — one local adopt Prepare record, and the terminal body it borrows.
//   1 — a bare canonical Call's JSON-RPC text.
//   2 — bare consent decision/revocation parameters.
//   3 — a local share's whole-terminal body.
//   4 — an independent checkpoint body behind the fixed seed probe offset.
//   5 — a bare Notify body, a kind and its record.
//   6 — a split inner-envelope stream through bounded production ingress.
//   7 — bounded mutations of valid canonical consent/presence seeds.
//   8 — bare current Call and Notify bodies.

#include "ConsentFuzz.h"

#include "Core/CellGrid.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/Profile.h"
#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/ShareTerminalLimits.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>
#include <stddef.h>
#include <string_view>
#include <utility>
#include <vector>

namespace Local = Ubiquitty::Protocol::Local;

// Passive Terminal: constructed once, reused every input to avoid per-call heap churn.
static Ubiquitty::Terminal& term()
{
    static Ubiquitty::Terminal shared;
    return shared;
}

// An applied checkpoint must leave the Ubiquitty::Terminal structurally valid.
// Same invariants fuzz_sync/fuzz_vt100 pin.
static void checkTermSanity(const Ubiquitty::Terminal& t);

// Applies a checkpoint body as the daemon applies an adopted session's, and
// checks what an applied one leaves.
static void applyCheckpointBody(std::span<const uint8_t> body)
{
    Ubiquitty::Protocol::Sync::ScreenReplica replica;
    if (Ubiquitty::Protocol::Sync::applyCheckpoint(term(), body, replica) == Ubiquitty::Protocol::Sync::ApplyOutcome::Applied)
        checkTermSanity(term());
}

// Reads a local share's whole-terminal body as the daemon does, under the
// daemon's own limits, and checks the terminal it admits.
static void shareBody(std::span<const uint8_t> body)
{
    const auto restored = Ubiquitty::Protocol::Sync::decodeHandoverTerminal(body, Ubiquitty::Session::localShareTerminalLimits(Local::maxAdoptBodyBytes));
    if (restored && restored->terminal)
        checkTermSanity(*restored->terminal);
}

static void checkTermSanity(const Ubiquitty::Terminal& t)
{
    assert(t.rows > 0 && t.rows <= Ubiquitty::maxRows);
    assert(t.columns > 0 && t.columns <= Ubiquitty::maxColumns);
    assert(t.screens.active == &t.screens.primary || t.screens.active == &t.screens.alternate);
    assert(t.screens.active->cursorX >= 0 && t.screens.active->cursorX < t.columns);
    assert(t.screens.active->cursorY >= 0 && t.screens.active->cursorY < t.rows);
    assert(t.linkCount >= 0 && t.linkCount <= Ubiquitty::VT::linkTableCapacity);
    assert(t.clusterPool.size() >= 0 && t.clusterPool.size() <= Ubiquitty::ClusterPool::maxEntries);
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < t.columns; ++c)
            (void)Ubiquitty::Screen::visibleCell(&t, 0, r, c);
}

// One local adopt Prepare record: its bounded prefix and digest, then the body
// it borrows. No remote frame can grant descriptor authority.
static void prepareRecord(std::span<const uint8_t> bytes)
{
    constexpr std::size_t maxRecordBytes = Local::maxAdoptBodyBytes + Local::adoptWirePrepareHeaderBytes;
    const auto prepare = Local::decodeAdoptWirePrepare(bytes, maxRecordBytes);
    if (!prepare)
        return;
    assert(prepare->terminalBody.size() <= Local::maxAdoptBodyBytes);
    shareBody(prepare->terminalBody);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
        return 0;

    const uint8_t selector = data[0];
    const uint8_t* body = data + 1;
    const size_t bodyLength = size - 1;

    const std::span<const uint8_t> bytes { body, bodyLength };
    const std::string_view text { reinterpret_cast<const char*>(body), bodyLength };
    switch (selector % 9) {
    case 0:
        prepareRecord(bytes);
        break;

    case 1: {
        UbiquittyTest::ConsentFuzz::callText(text);
        break;
    }

    case 2: {
        UbiquittyTest::ConsentFuzz::parameters(text);
        break;
    }

    case 3: {
        shareBody(bytes);
        break;
    }

    case 4: {
        // Raw checkpoint body at a fixed offset: lets the fuzzer explore the
        // checkpoint grammar with no header in front of it. The eight is this
        // case's own number rather than the protocol's — nothing here decodes a
        // header — held so seeds recorded against that offset stay valid.
        constexpr std::size_t rawBodyProbeOffset = 8;
        if (size > rawBodyProbeOffset) {
            applyCheckpointBody({ data + rawBodyProbeOffset, size - rawBodyProbeOffset });
        }
        break;
    }

    case 5: {
        UbiquittyTest::ConsentFuzz::notifyBody(bytes);
        break;
    }
    case 6:
        UbiquittyTest::ConsentFuzz::stream(bytes);
        break;
    case 7:
        UbiquittyTest::ConsentFuzz::seeded(bytes);
        break;
    case 8: {
        UbiquittyTest::ConsentFuzz::frameBody(Ubiquitty::Protocol::Sync::FrameKind::Call, bytes);
        UbiquittyTest::ConsentFuzz::frameBody(Ubiquitty::Protocol::Sync::FrameKind::Notify, bytes);
        break;
    }
    }
    return 0;
}
