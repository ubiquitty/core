// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/Image/TerminalImage.h"
#include "Core/TerminalShared.h"
#include "Forward.h"
#include "PAL/Capability.h"

// Kitty graphics wire protocol — a decoder backend for the TerminalImage model.
// The transfer parser (APC ESC _ G ... ESC \) decodes payloads into TerminalImage
// / TerminalImagePlacement slots. Terminal owns the underlying state (APC
// accumulator, chunk buffer); these entry points reach into it.

namespace Ubiquitty::Kitty {

// Accumulated APC payload ceiling (base64 across chunks); caps a malicious /
// never-terminated transfer. The DECODED size is bounded separately, and by a
// limit shared with the other image protocols: Image::maxDecodedImageBytes.
constexpr int maxAPCCapacity = 64 * 1024 * 1024;
// The largest APC frame or chunk allocation kept once its payload has been
// dispatched or abandoned; a larger one is released, so one large transfer
// does not hold up to `maxAPCCapacity` for the life of the terminal.
constexpr int maxRetainedAPCCapacity = 64 * 1024;

/// One parsed `ESC _ G <keys>;<payload> ESC \` control block.
///
/// A plain aggregate because `Terminal` embeds it as the chunked-transfer
/// carry-over slot: a transfer split across APC blocks resumes from this, so it
/// has to survive being copied with the terminal rather than owning anything.
struct GraphicsControl {
    char action;
    char transport;
    char deleteMode;
    char compression; // 'z' = zlib, 0 = uncompressed
    int format;
    int width;
    int height;
    int payloadSize; // S — exact payload byte length; bounds the t=s read (the
                     // kernel page-rounds shared-memory object sizes, so fstat
                     // alone cannot recover the true length). 0 = unspecified.
    uint32_t imageID;
    uint32_t imageNumber;
    uint32_t placementID;
    int quiet;
    int more;
    // Placement geometry. All optional; 0 means "unspecified".
    int cellColumns; // c
    int cellRows; // r
    int pixelOffsetX; // X
    int pixelOffsetY; // Y
    int sourceX; // x
    int sourceY; // y
    int sourceWidth; // w
    int sourceHeight; // h
    int32_t zIndex; // z (signed)
    bool hasZIndex;
    int cursorPolicy; // C
    int unicodePlacement; // U — 1 = create a VIRTUAL placement (no grid placement;
                          // the image is positioned later via U+10EEEE placeholder cells)
    bool hasImageID;
    bool hasImageNumber;
    bool hasPlacementID;
    // Whether the block stated `a`, and whether it stated a key that describes
    // a transfer's data (t, f, s, v, S, o). A continuation block carries neither
    // a different action nor any of those, so either one marks a block as a
    // command of its own while a chunked transfer is open.
    bool hasAction;
    bool hasTransferKey;
};

/// Dispatches one accumulated APC graphics block.
///
/// The parser accumulates `ESC _ G … ESC \` into the terminal's APC buffer and
/// calls this at the terminator.
///
/// - Parameter t: The terminal whose APC buffer holds the block.
void dispatch(Ubiquitty::Terminal& t);
/// Drains completed decode jobs and installs their pixels.
///
/// Takes finished PNG-decode and direct-pixel-inflate results off the per-domain
/// task queues, writes them into their `TerminalImage` slots, and sends the
/// kitty `OK` or error response. Safe and cheap on every main-loop tick, so a
/// frontend calls it unconditionally rather than tracking whether one is due.
///
/// - Parameter t: The terminal owning the queues and image slots.
void drainAsyncDecodes(Ubiquitty::Terminal* t);

/// Destroys this terminal's undrained decodes without integrating them.
///
/// Teardown calls it: a completion writes into the terminal being destroyed, so
/// the pending results are dropped rather than applied. Each job owns its
/// buffers, so nothing leaks.
///
/// - Parameter t: The terminal whose pending decodes are discarded.
void discardPendingDecodes(Ubiquitty::Terminal* t);

} // namespace Ubiquitty::Kitty
