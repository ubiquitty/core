// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tests/Core/Protocol/Tmux/fuzz_tmux.cpp — libFuzzer harness for the tmux
// Control Mode (`tmux -CC`) parser (Source/Core/Protocol/Tmux/Tmux.{h,cpp} +
// Source/Core/Protocol/Tmux/Commands.cpp).
//
// Threat model: every byte the parser sees originates from a `tmux -CC`
// server reached over SSH — attacker-controlled if the peer is hostile or
// compromised. feedInput is a hand-rolled byte-at-a-time state machine; the
// layout decoder (parseLayout → parseLayoutString → parseLayoutCell) walks a
// RECURSIVE cell tree off a `%layout-change` field; parseDimension / parseNonNegativeInteger
// / parseWindowID turn wire digits into pane geometry; the `%output` path
// length-decodes an octal-escaped payload byte-by-byte. All of it runs on
// untrusted bytes with no length prefix — the classic reassembly + integer +
// recursion surface.
//
// This harness links the REAL Tmux.cpp / Commands.cpp (no parser copy — it
// fuzzes exactly what ships) and drives the PUBLIC entry points:
//   - feedInput(tc, byte, ctx, deliver), the byte machine. The fuzzer picks a
//     chunk stride from byte 0, then feeds the rest in those chunks so a frame
//     split mid-header / mid-%output-payload / mid-layout reassembles exactly
//     as it would across socket reads. A no-op deliver sink and recording
//     event sinks stand in for the host callbacks.
//   - parseLayout(view, span), the layout decoder the frontend calls on the
//     `%layout-change` field — invoked both from inside the event sink (the
//     real consumption path) and directly on a fuzzer slice.
//   - decodePaneModeReport(body, len, &paneID), the display-message reply
//     decoder (Commands.cpp).
//
// After every chunk, and on every decoded layout, the pane-geometry / pane-id /
// window invariants are asserted: dimensions within [0, tmuxMaxLayoutDimension],
// pane ids within [0, maxTmuxPaneID], pane count within the span, the active
// pane id never negative / never above the ceiling. ASan catches OOB on the
// fixed line/geometry buffers, UBSan catches the integer overflow in the
// dimension / octal / pane-id accumulators, and the asserts pin a blown bound
// that happens to stay in-bounds of allocated memory.
//
// Build via Scripts/run-tests.sh --with-fuzz; same shape as fuzz_tiff /
// fuzz_sync. The seed corpus under Tests/Protocol/Tmux/fuzz-corpus/ (real
// control-mode streams) replays first, then libFuzzer explores.

#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/Tmux.h"

#include <cassert>
#include <cstdint>
#include <span>
#include <stddef.h>
#include <string>
#include <string_view>

using namespace Ubiquitty::Protocol::Tmux;

// No-op delivery sink. The decoded %output bytes flow here; we only count
// them so the call can't be optimized away and so a runaway length decode is
// observable. deliver must stay trivial — it is on the hottest path.
struct DeliverContext {
    unsigned long long bytes = 0;
};

static void deliverByte(void* context, uint8_t /*byte*/) { static_cast<DeliverContext*>(context)->bytes++; }

// Every TmuxPaneGeometry surfaced — whether via the in-band %layout-change
// sink or a direct parseLayout call — must satisfy the same bounds the
// frontend relies on to place panes without OOB / nonsense geometry.
static void checkGeometry(const Ubiquitty::Protocol::Tmux::TmuxPaneGeometry* panes, int count, int cap)
{
    assert(count >= 0 && count <= cap);
    for (int i = 0; i < count; ++i) {
        const Ubiquitty::Protocol::Tmux::TmuxPaneGeometry& p = panes[i];
        assert(p.paneID >= 0 && p.paneID <= Ubiquitty::Protocol::Tmux::maxTmuxPaneID);
        assert(p.width >= 0 && p.width <= Ubiquitty::Protocol::Tmux::tmuxMaxLayoutDimension);
        assert(p.height >= 0 && p.height <= Ubiquitty::Protocol::Tmux::tmuxMaxLayoutDimension);
        assert(p.x >= 0 && p.x <= Ubiquitty::Protocol::Tmux::tmuxMaxLayoutDimension);
        assert(p.y >= 0 && p.y <= Ubiquitty::Protocol::Tmux::tmuxMaxLayoutDimension);
    }
}

// The active pane id is the keystroke target; the parser must never leave it
// negative or above the ceiling regardless of what %output / %window-pane-changed
// fed it.
static void checkClientInvariants(const Ubiquitty::Protocol::Tmux::TmuxClient& tc)
{
    assert(tc.activePaneID >= 0 && tc.activePaneID <= Ubiquitty::Protocol::Tmux::maxTmuxPaneID);
    // The line buffer caret stays inside the fixed buffer (off-by-one here
    // would be the OOB the byte machine guards against).
    assert(tc.lineLength >= 0 && tc.lineLength < Ubiquitty::Protocol::Tmux::maxTmuxLineLength);
    assert(tc.responseDepth >= 0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
        return 0;

    const uint8_t selector = data[0];
    const uint8_t* body = data + 1;
    const size_t bodyLength = size - 1;

    // Path A — the byte machine. Drive feedInput in fuzzer-chosen chunks so a
    // frame split mid-%output / mid-layout / mid-%begin reassembles. The
    // layout sink calls the production parseLayout, so the recursive cell
    // decoder is exercised in-band exactly as the frontend exercises it.
    {
        // Fixed geometry sink shared with the layout event handler.
        static Ubiquitty::Protocol::Tmux::TmuxPaneGeometry geom[Ubiquitty::Protocol::Tmux::tmuxMaxLayoutPanes];

        Ubiquitty::Protocol::Tmux::TmuxClient tc;
        init(&tc);
        // Control-mode autodetect on, so the `%begin ` / DCS introducers can
        // flip the machine into Line/Response exactly as on a live attach.
        configure(&tc, IntegrationMode::Enabled, AutoDetectMode::Automatic);

        Ubiquitty::Protocol::Tmux::TmuxEventClient events;
        // geom has static storage duration, so the sink reaches it without a
        // capture and without the client's context.
        events.layoutChange = [](void*, int windowID, std::string_view layout) {
            assert(windowID >= 0);
            int n = parseLayout(layout, std::span<Ubiquitty::Protocol::Tmux::TmuxPaneGeometry>(geom, Ubiquitty::Protocol::Tmux::tmuxMaxLayoutPanes));
            assert(n >= -1 && n <= Ubiquitty::Protocol::Tmux::tmuxMaxLayoutPanes);
            if (n > 0)
                checkGeometry(geom, n, Ubiquitty::Protocol::Tmux::tmuxMaxLayoutPanes);
        };
        // The remaining sinks just touch their borrowed views so ASan flags a
        // bad span; they must never copy past the parser's bounds.
        events.windowAdd = [](void*, int windowID) { assert(windowID >= 0); };
        events.windowClose = [](void*, int windowID) { assert(windowID >= 0); };
        events.windowRenamed = [](void*, int windowID, std::string_view name) {
            assert(windowID >= 0);
            (void)name.size();
        };
        events.sessionRenamed = [](void*, std::string_view name) { (void)name.size(); };
        events.commandResponseLine = [](void*, std::string_view line) { (void)line.size(); };
        setEventClient(&tc, events);

        DeliverContext ctx;

        // Chunk stride from byte 0: 1..256, so the fuzzer steers the split
        // points. Feeding byte-by-byte within each chunk matches feedInput's
        // one-byte-at-a-time contract; the chunk boundary is the reassembly
        // seam under test (state is held across the boundary in `tc`).
        const size_t stride = static_cast<size_t>(selector) + 1;
        size_t off = 0;
        while (off < bodyLength) {
            size_t chunk = stride;
            if (chunk > bodyLength - off)
                chunk = bodyLength - off;
            for (size_t i = 0; i < chunk; ++i)
                feedInput(&tc, body[off + i], &ctx, deliverByte);
            off += chunk;
            // Invariants must hold at every reassembly boundary, not just at
            // the end — a transient bad state is still a bug.
            checkClientInvariants(tc);
        }
    }

    // Path B — parseLayout directly on the bare slice. The %layout-change sink
    // only reaches the decoder with a space-delimited token; feeding the whole
    // slice lets the fuzzer drive the recursive cell grammar (depth cap, the
    // `{`/`[` containers, the dimension overflow guards) without first
    // satisfying the line framing.
    {
        static Ubiquitty::Protocol::Tmux::TmuxPaneGeometry geom[Ubiquitty::Protocol::Tmux::tmuxMaxLayoutPanes];
        int n = parseLayout(std::string_view(reinterpret_cast<const char*>(body), bodyLength), std::span<Ubiquitty::Protocol::Tmux::TmuxPaneGeometry>(geom, Ubiquitty::Protocol::Tmux::tmuxMaxLayoutPanes));
        assert(n >= -1 && n <= Ubiquitty::Protocol::Tmux::tmuxMaxLayoutPanes);
        if (n > 0)
            checkGeometry(geom, n, Ubiquitty::Protocol::Tmux::tmuxMaxLayoutPanes);
    }

    // Path C — decodePaneModeReport on the bare slice (the display-message
    // reply decoder; pane id scan + fixed-width bit string).
    {
        PaneID paneID = -1;
        std::string seq = decodePaneModeReport({ reinterpret_cast<const char*>(body), bodyLength }, paneID);
        assert(paneID >= 0);
        (void)seq.size();
    }

    return 0;
}
