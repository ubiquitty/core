// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/Image/TerminalImage.h"
#include "Core/TerminalShared.h"
#include "Forward.h"
#include "PAL/Capability.h"

#include <array>
#include <cstdint>

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
    uint32_t frameBackground; // Y on a=f is a packed RGBA color
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

/// A chunked transfer of raw pixels sent directly, decoded as its blocks
/// arrive.
///
/// Decoded whole at its final block, a transfer's decode and the page faults
/// of the buffer it writes come after the last byte the sender wrote, and the
/// sender waits for them. Decoded as the blocks arrive, both are spent while
/// the rest of the transfer is still on its way.
///
/// The pixels and every refusal are the ones decoding the whole payload at
/// the end gives. Base64 decodes four characters at a time, so the blocks are
/// decoded on the four-character grid of the whole payload: up to three
/// characters at a block's end are carried to the next.
struct ChunkDecode {
    /// What the payload received so far does not decode to.
    enum class Fault : std::uint8_t {
        None,
        /// A byte outside the alphabet, or padding before the end.
        Malformed,
        /// More bytes than the transfer's geometry describes.
        TooLarge,
    };
    /// The pixels, sized to what the transfer's geometry describes.
    Image::OwnedPixels pixels;
    int expectedLength = 0;
    int decodedLength = 0;
    /// The payload's characters received, held to ``maxAPCCapacity`` as an
    /// accumulated payload is.
    int encodedLength = 0;
    std::array<char, 4> carry {};
    int carryLength = 0;
    /// Whether a decoded group was padded, which ends the payload.
    bool hasSeenPadding = false;
    Fault fault = Fault::None;
    /// Whether the open transfer is decoded this way rather than accumulated.
    bool isActive = false;
};

/// Dispatches one accumulated APC graphics block.
///
/// The parser accumulates `ESC _ G … ESC \` into the terminal's APC buffer and
/// calls this at the terminator.
///
/// - Parameter t: The terminal whose APC buffer holds the block.
void dispatch(Ubiquitty::Terminal& t);
/// Drops the open chunked transfer and what it received.
///
/// A transfer is abandoned when a block of another command arrives while it is
/// open, when it passes ``maxAPCCapacity``, and when its blocks are refused.
///
/// - Parameter t: The terminal whose transfer is dropped.
void abandonChunkedTransfer(Ubiquitty::Terminal& t);
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
