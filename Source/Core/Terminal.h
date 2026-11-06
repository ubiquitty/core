// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/FallibleString.h"

#include "Base/ByteBuffer.h"
#include "Base/CheckedPtr.h"
#include "Base/SwiftInterop.h"
#include "Base/Types.h"
#include "Core/ClusterPool.h"
#include "Core/Cursor/Style.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/DragDrop/Client.h"
#include "Core/EffectGate.h"
#include "Core/Image/Sixel/Decoder.h"
#include "Core/Keys/KeyEvent.h"
#include "Core/Kitty/Graphics.h"
#include "Core/Kitty/KeyboardState.h"
#include "Core/Modes/Scroll.h"
#include "Core/Modes/SyncOutput.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/ClipboardPolicy.h"
#include "Core/OSC/FontRequestQueue.h"
#include "Core/OSC/KittyNotification.h"
#include "Core/OSC/SequenceGate.h"
#include "Core/OSC/TabStatus.h"
#include "Core/Protocol/Tmux/Tmux.h"
#include "Core/Rendering/CursorMotion.h"
#include "Core/RichText/BlockStore.h"
#include "Core/RichText/Layout.h"
#include "Core/RowMeta.h"
#include "Core/RunTextStore.h"
#include "Core/Screen/LineIdentity.h"
#include "Core/Screen/QualifiedArea.h"
#include "Core/Screen/Screen.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Tek/ColorPreference.h"
#include "Core/Tek/Tek4014.h"
#include "Core/TermPalette.h"
#include "Core/TextSizeTable.h"
#include "Core/Unicode/Encodings.h"
#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Core/Unicode/Width.h"
#include "Core/VT/Constants.h"
#include "Core/VT/SoftFont.h"
#include "Core/VT/Sound.h"
#include "Core/VT/UserDefinedKeys.h"
#include "Forward.h"
#include "PAL/BellMode.h"
#include "PAL/TaskQueue.h"
#include "PAL/TerminalClients.h"
#include <array>
#include <bitset>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Parser state enum forward-declared so Terminal carries it by value; defined in VT/Parser.h.
namespace Ubiquitty {

struct Terminal;

/// Whether a terminal owns a live grid/parser or only passive decoded state.
enum class TerminalInitialization : std::uint8_t {
    Complete,
    StateOnly,
};

struct PaneRouter {
    using Lookup = Terminal* (*)(void*, Protocol::Tmux::PaneID);
    using Flush = void (*)(void*);

    Lookup lookup { nullptr };
    Flush flush { nullptr };
    void* context { nullptr };

    [[nodiscard]] Terminal* route(Protocol::Tmux::PaneID paneID) const
    {
        return lookup ? lookup(context, paneID) : nullptr;
    }

    void endDeliveryPass() const
    {
        if (flush)
            flush(context);
    }
};

static_assert(std::is_trivially_copyable_v<PaneRouter>);
namespace VT::Parser {
enum class State : int;
}
}

// The render projection is carried by pointer without the matcher header.

namespace Ubiquitty::Screen::ScrollSearch {
struct ResultProjection;
}

// Forward-declared to break the LineStore.h include cycle (Terminal carries it by pointer).
namespace Ubiquitty {
class LineStore;
}

// Forward-declared to carry by pointer without Core/Triggers/Engine.h. Allocated lazily on first
// trigger rule / characterClass, released by ~Terminal.

namespace Ubiquitty::Triggers {
class Engine;
}

// Carried by pointer; Core/Attributes/Hyperlinks.h owns it.

namespace Ubiquitty::Attributes::Hyperlinks {
struct LinkTable;
}

// Carried by pointer; Core/Rendering/StatusRow.h owns it.

namespace Ubiquitty::Rendering {
struct StatusRowStorage;
}

namespace Ubiquitty {

/// The text alignment selected by an ECMA-48 character tab stop.
enum class CharacterTabAlignment : std::uint8_t {
    Plain,
    Centered,
    LeadingEdge,
    TrailingEdge,
    TargetCharacter,
};

/// Who answers the program's queries for this terminal.
enum class ReplyAuthority : uint8_t {
    /// This terminal writes its replies to its program.
    ThisTerminal,
    /// Another terminal answers for this one — the authoritative host of a
    /// replica that parses the same bytes — so this one writes none.
    Elsewhere,
};

// Preceding data-stream item for REP (CSI Ps b). Repeats the effect of the preceding
// graphic or control function (ECMA-48 defines only the graphic branch; the control
// branch follows the 1979 2nd ed. §7.2.57 data-stream model). One record per Terminal.
// Control-string entries (OSC/DCS/APC/SOS/PM) setNone().
// `multiplier` makes REP-after-REP well-defined: the record's effect is multiplier × item.
// The product saturates at maxRepeatEffects to cap flood-amplification (else up to 65535× per 6 bytes).
struct PrecedingItem {
    enum class Kind : uint8_t {
        None, // no repeatable predecessor (stream start / control string)
        Graphic,
        ControlByte, // C0 / C1 / single shift — replayed via the Ground DFA action
        ESCSequence,
        CSISequence
    };

    static constexpr uint32_t maxRepeatEffects { 65535 };

    Kind kind { Kind::None };
    uint32_t multiplier { 1 }; // effect count carried by a preceding REP

    // Graphic: exactly what emitCharacter consumed — the raw code point (width /
    // cluster classification), the characterSet-resolved cell code point, and the
    // fused variation selector, so a repeat re-enters the same emit path.
    uint32_t codePoint { 0 };
    uint32_t resolvedCodePoint { 0 };
    uint32_t attachedSelector { 0 };

    uint8_t controlByte { 0 }; // ControlByte byte; final byte for ESC/CSI
    uint8_t escIntermediate { 0 }; // ESC: 0, '#', ' ', or a designator '('..'/'
    uint8_t drcsIntermediate { 0 }; // ESC designation: DRCS intermediate (0x20..0x2F)
    uint8_t privatePrefix { 0 }; // CSI ('?', '>', '=', '<')
    uint8_t intermediate { 0 }; // CSI (0x20-0x2F)
    int parameterCount { 0 }; // for CSI
    std::array<std::uint32_t, Ubiquitty::VT::maxParameters> parameters {}; // saturates at VT::maxParameterValue, past 16 bits
    std::array<uint8_t, Ubiquitty::VT::maxParameters> parameterSeparatorWasColon {};

    void setNone()
    {
        kind = Kind::None;
        multiplier = 1;
    }
    void setGraphic(uint32_t rawCodePoint, uint32_t resolved, uint32_t selector)
    {
        kind = Kind::Graphic;
        multiplier = 1;
        codePoint = rawCodePoint;
        resolvedCodePoint = resolved;
        attachedSelector = selector;
    }
    void setControl(uint8_t byte)
    {
        kind = Kind::ControlByte;
        multiplier = 1;
        controlByte = byte;
    }
    void setESC(uint8_t finalByte, uint8_t intermediateByte, uint8_t drcsByte)
    {
        kind = Kind::ESCSequence;
        multiplier = 1;
        controlByte = finalByte;
        escIntermediate = intermediateByte;
        drcsIntermediate = drcsByte;
    }
    /// Records one CSI sequence, replacing whatever this slot held.
    ///
    /// - Parameters:
    ///   - finalByte: The byte that terminated the sequence.
    ///   - priv: The private-marker byte, 0 when the sequence carried none.
    ///   - interm: The intermediate byte, 0 when the sequence carried none.
    ///   - params: The parsed parameters; the first `paramCount` are meaningful.
    ///   - separatorWasColon: Per parameter, whether a colon rather than a
    ///     semicolon preceded it — the SGR 4:3 / 38:2 sub-parameter form.
    ///   - paramCount: How many entries of `params` the sequence supplied.
    void setCSI(uint8_t finalByte, uint8_t priv, uint8_t interm, const std::array<std::uint32_t, Ubiquitty::VT::maxParameters>& params, const std::array<uint8_t, Ubiquitty::VT::maxParameters>& separatorWasColon, int paramCount)
    {
        kind = Kind::CSISequence;
        multiplier = 1;
        controlByte = finalByte;
        privatePrefix = priv;
        intermediate = interm;
        parameterCount = paramCount;
        parameters = params;
        parameterSeparatorWasColon = separatorWasColon;
    }
};

// Nearly every operation on a Terminal is a free function taking one, declared by
// the module that implements it — `resize` in VT, `applySnapshot` in Protocol/Sync,
// `expandWordSelection` in Triggers. The shape is a decision rather than a leftover
// from the C-era `Term`.
//
// It is what lets a module extend this type without this type depending on the
// module: `Core/Protocol/Sync` names a Terminal in its own signatures while
// nothing here names Sync. As members those declarations would land in this
// file, which every subsystem would then edit and which is already included
// nearly everywhere.
//
// So `Terminal` carries state, its lifetime and the invariants that are nobody
// else's business; everything a module knows how to do to one lives with that
// module.
struct Terminal {
    Terminal();
    /// Constructs passive decoded state without a usable grid or parser buffer.
    /// Only state-codec scratch owners use StateOnly; live terminals use Complete.
    explicit Terminal(TerminalInitialization);
    ~Terminal();
    Terminal(const Ubiquitty::Terminal&) = delete;
    Ubiquitty::Terminal& operator=(const Ubiquitty::Terminal&) = delete;
    Terminal(Ubiquitty::Terminal&&) = delete;
    Ubiquitty::Terminal& operator=(Ubiquitty::Terminal&&) = delete;

    struct Screens {
        Screen::State primary;
        Screen::State alternate;
        Screen::State* active { nullptr };
        bool isUsingAlternateScreen {};

        /// Makes one screen buffer the active one.
        ///
        /// - Parameter buffer: The buffer to activate.
        void activate(::Ubiquitty::Modes::ScreenBuffer buffer)
        {
            isUsingAlternateScreen = buffer == ::Ubiquitty::Modes::ScreenBuffer::Alternate;
            active = isUsingAlternateScreen ? &alternate : &primary;
        }
    } screens;

    // current logical terminal dimensions (≤ maxColumns / maxRows)
    int columns {};
    int rows {};

    // Hot parse-loop cluster: fields the per-byte lanes and span-writer eligibility gate read,
    // kept adjacent to the parser block so the guard set shares a few cache lines. Scalars only.

    // Tek 4014 mode: host bytes route into the Tek state machine instead of the VT100 DFA.
    // Entered by DECSET 38; left by in-mode ESC ETX / ESC CAN.
    bool isTekModeActive {};
    // DECSASD redirect gate — emit code reads it per write to route into
    // statusRow (declared with the other status-line state below).
    uint8_t statusLineActive {};
    int singleShift {}; // one-shot SS2/SS3 (2 or 3); 0 when inactive

    // G0..G3 characterSet designations. 0=US-ASCII, 1=DEC special graphics, 2=UK national set.
    std::array<int, 4> characterSetG {};
    int characterSetActive {}; // GL invocation 0..3. SI/SO shift G0/G1; LS2→G2; LS3→G3.
    int characterSetGR {}; // GR invocation 1..3 (0 = default). Set by LS1R/LS2R/LS3R. Only a
                           // single-byte input encoding resolves 0xA0..0xFF through G[characterSetGR];
                           // under UTF-8 those bytes are encoded characters and GR stays inert.

    // Margin bell (DECSET 44): putCharacter rings when cursor X enters [marginBellColumn .. columns-1].
    // marginBellArmed enforces the fire-once-per-pass rule; CR/CUB/NEL/linefeed-wrap re-arm.
    bool marginBell {};
    int marginBellColumn {};
    bool marginBellArmed {};

    // OSC 8 link id that emitCharacter stamps onto each emitted cell.
    uint16_t currentLinkID {};
    // Autowrap eval-guard: screen row the line-commit hook evaluated at fill time, so the
    // deferred wrap-commit linefeed skips re-evaluating it and a full-width line fires once.
    // -1 when none pending. Touched only when triggers are configured.
    int triggerWrapEvaluatedRow {};

    // Hot CSI scalars grouped right after parseState, before the big parameters[] arrays,
    // so the common short-CSI path shares one cache line.
    struct Parser {
        Ubiquitty::VT::Parser::State parseState {};
        int parameterCount {};
        int privatePrefix {}; // 0, '<', '=', '>', or '?'
        int intermediate {}; // 0x20-0x2F intermediate byte, e.g. '!' or ' '
        std::array<std::uint32_t, Ubiquitty::VT::maxParameters> parameters {}; // saturates at VT::maxParameterValue, past 16 bits
        // True when the separator before parameters[i] was a colon not a semicolon; applySGR uses
        // it to tell T.416 colon sub-parameters (38:2::r:g:b) from semicolon params. Position 0 is false.
        std::array<uint8_t, Ubiquitty::VT::maxParameters> parameterSeparatorWasColon {};
        // Bit i set = parameter i carried no digits. An empty parameter selects the
        // control's default, which is not always what an explicit 0 selects, and the
        // two are otherwise indistinguishable once both land in the slot as 0.
        // One bit per slot, so the mask type is what actually bounds the cap.
        static_assert(Ubiquitty::VT::maxParameters <= 32, "parameterWasEmpty carries one bit per parameter slot");
        uint32_t parameterWasEmpty { ~0u };
        // Set when a separator arrives with every slot already taken. The
        // separator is dropped, so the digits after it would otherwise land in
        // the last slot and rewrite a parameter the control does read.
        bool didParametersOverflow {};
        PrecedingItem precedingItem {}; // REP (CSI Ps b) predecessor
        // Growable OSC + DCS payload accumulator, oscBufferInitialCapacity..maxOSCBufferSize.
        // The growth and the shrink-back are OSC policy and live in VT.cpp; the type
        // carries only the three quantities that have to agree.
        Ubiquitty::ByteBuffer osc;
        // True while an OSC that ended with BEL is dispatched, and while a
        // deferred answer to one is framed: its reply ends with BEL too.
        bool isOSCRequestBellTerminated {};
        int characterSetSelect {}; // 0=G0, 1=G1, 2=G2, 3=G3: which is being defined
        bool is96CharacterSet {}; // pending designation is a 96-set (ESC -/./ final) vs 94-set (ESC (/)/*/+);
                                  // steers actCharacterSetByte's registry. G0 is 94-set only.
        uint8_t characterSetIntermediate {}; // DRCS designator intermediate (0x20..0x2F, 0 if none),
                                             // matched against SoftFont::designIntermediate.

        // UTF-8 byte-stream decoder.
        Ubiquitty::Unicode::UTF8Decoder utf8 {};

        // Substitution policy for invalid/truncated UTF-8: FFFD emits U+FFFD, Drop emits nothing,
        // Latin1 reinterprets each undecodable byte as ISO-8859-1. Read only on the error path.
        Ubiquitty::Unicode::InvalidUTF8Policy invalidUTF8Policy {};
        // Lead byte of the in-progress multibyte sequence, kept so the Latin1 policy can reinterpret
        // an abandoned lead after the decoder discards it. Written only when opening a multibyte sequence.
        std::uint8_t utf8LeadByte {};

        // Legacy input characterSet. When != UTF8, processBytes transcodes incoming bytes to UTF-8 through
        // this streaming decoder before the UTF-8 stage. Default UTF8 = pass-through.
        Ubiquitty::Unicode::EncDecoder inputEncoding {};
        bool parserInputIsUTF8 {};

        // Live grapheme-cluster accumulation state between emitCharacter calls. emitCharacter appends
        // extending codePoints to the lead cell's combining buffer instead of advancing the cursor.
        //   clusterActive  — lead cell open for extension.
        //   clusterRow/Column — active-screen coords of the lead cell.
        //   clusterScreen  — owning screen (0 primary, 1 alternate); a screen flip invalidates it.
        //   clusterLast    — running last codePoint, fed to the segmenter as prev.
        //   clusterState   — UAX #29 carried break state (RI parity + GB11).
        //   clusterAtX/Y/clusterAtWrap — cursor snapshot after the base; the next emitCharacter honors
        //                    the open cluster only if the live cursor still matches, so any CR/LF/
        //                    positioning between calls implicitly invalidates the anchor.
        bool clusterActive {};
        int clusterRow {};
        int clusterColumn {};
        uint8_t clusterScreen {};
        uint32_t clusterLast {};
        Ubiquitty::Unicode::GraphemeState clusterState {};
        int clusterAtX {};
        int clusterAtY {};
        bool clusterAtWrap {};
        // Full codePoint sequence of the live cluster; never truncates — once it outgrows the cell's
        // inline buffer it interns into the cluster pool. Capped at clusterPoolMaximumCodePoints (rest dropped).
        std::array<uint32_t, clusterPoolMaximumCodePoints> clusterCodePoints {};
        int clusterCodepointLength {};
    } parser;

    /// Preimages of the last graphic's cell writes and wrap scrolls.
    struct GraphicCancellation {
        /// Names a saved cell span or the row displaced by a scroll.
        struct Operation {
            bool isScroll {};
            bool isStatus {};
            int row {};
            int column {};
            int count {};
            int bottom {};
            std::uint32_t cellOffset {};
            std::uint32_t metaOffset {};
            bool didAppendHistory {};
            bool didEvictHistory {};
            int historyLength {};
            std::uint32_t historyOffset {};
            RowMeta historyMeta {};
            std::uint64_t historyFront {};
            bool doesHistoryContinueLine {};
        };
        bool isValid {};
        bool isCapturing {};
        bool isAlternate {};
        bool isStatus {};
        bool wrapPending {};
        int rows {};
        int columns {};
        int cursorX {};
        int cursorY {};
        int singleShift {};
        int triggerWrapRow {};
        bool didAlignedTabOverflow {};
        bool didEmitAlignedGraphic {};
        bool isClusterActive {};
        int clusterRow {};
        int clusterColumn {};
        uint8_t clusterScreen {};
        uint32_t clusterLast {};
        Unicode::GraphemeState clusterState {};
        int clusterAtX {};
        int clusterAtY {};
        bool isClusterAtWrap {};
        std::array<uint32_t, clusterPoolMaximumCodePoints> clusterCodePoints {};
        int clusterCodepointLength {};
        PrecedingItem predecessor {};
        FallibleVector<Cell> cells;
        FallibleVector<RowMeta> metadata;
        FallibleVector<Operation> operations;
    };
    std::unique_ptr<GraphicCancellation> graphicCancellation;

    // Host-escape-toggled behavioral modes (DECSET/DECRST + ANSI SM/RM) and derived handshake state.
    // Direct field access, no accessors: the parser reads these per escape, print paths per character.
    struct Modes {
        bool isCursorVisible {};
        bool autowrap {};
        bool appCursorKeys {}; // DECCKM
        bool appKeypad {};
        bool bracketedPaste {};
        bool shouldReportPasteEvents {};
        // DECCRTSM. Reset until a program sets it: the VT510 enables its CRT
        // saver by default to prolong the life of a tube the emulator does not
        // have, and a display blanked on its own reads as a hung window.
        bool shouldSaveCRT {};
        bool shouldReportKeyPositions = false;
        bool shouldMoveReadlinePointOnClick {};
        bool shouldMoveReadlinePointBeforePaste {};
        bool shouldDeleteReadlineSelection {};
        bool shouldQuotePasteCharacters {};
        bool shouldPreservePasteNewlines {};
        bool focusReporting {};
        // DECSET 1046 — alternate-screen feature gate. Default on; when reset, modes 47/1047/1049
        // no-op and a reset while on the alternate screen forces back to primary.
        bool allowAlternateScreen {};
        bool scrollOnOutput {}; // DEC 1010 — snap view to live tail on host output while scrolled back
        bool scrollOnKeypress {}; // DEC 1011 — snap view to live tail on keypress while scrolled back (default on)
        bool colorThemeReporting {};
        bool originMode {}; // DECOM
        bool reverseWrapExtended {}; // DEC private 1045 — reverse wrap across the whole margin
                                     // rectangle; 45 alone only re-enters a row that soft-wrapped
        // DEC STD 070 cursor coupling: DECHCCM (?60) is reset by default, DECVCCM (?61) and
        // DECPCCM (?64) set. The page here is never larger than the window, so coupling has
        // nothing to move; the states are held and reported. The two set-by-default modes are
        // stored inverted so that every flag starts false, as a checkpoint without them reads.
        bool isHorizontalCursorCoupled {}; // DECHCCM — set means coupled
        bool isVerticalCursorUncoupled {}; // DECVCCM — set means uncoupled
        bool isPageCursorUncoupled {}; // DECPCCM — set means uncoupled
        bool keyboardLocked {}; // KAM (ANSI mode 2) — set = the frontend discards typed input
        bool lnm {}; // ANSI mode 20 — Line Feed/New Line Mode
        bool srm {}; // ANSI mode 12 — Send/Receive: set = no local echo (default); reset = local echo
        bool insertMode {}; // IRM (ANSI mode 4) — printable chars shift the rest of the line right
        bool reverseVideo {}; // DECSCNM — reverse video screen
        bool syncOutput {}; // DECSET 2026 — bytes still feed the grid but the renderer holds off painting
                            // until the mode clears. Watchdog (syncOutputTouch) prevents a permanent freeze.
        bool vt52Mode {}; // DECRST 2 (DECANM=off) — VT52 grammar until ESC < returns to ANSI
        bool vt52Graphics {}; // VT52 ESC F graphics mode (0x60..0x7E as DEC special graphics); ESC G clears.
                              // Only meaningful while vt52Mode.

        // DECSDM (DEC ?80) — Sixel CellGrid Mode. Set: finished sixel leaves the cursor where it began
        // (no scroll). Reset (default): cursor drops below the image. Consulted in sixelDecodeCompletion.
        bool sixelDisplayMode {};
        bool clearScreenOnColumnChange {}; // DECCOLM clears screen on 80<->132; DECNCSM (?95) turns off
        bool columns132 {}; // DECCOLM tracking flag: 132 cols while CSI ?3h, 80 after ?3l. Pure query state;
                            // the width change is applied via Terminal::resize.
        uint8_t sace {}; // DECSACE — Select Attribute Change Extent. 1 = stream (default), 2 = rectangle.
        bool leftRightMarginMode {}; // DECLRMM (DECSET 69). False (default): margins ignored, motions use
                                     // [0, columns-1]. True: DECSLRM can set margins and motions clip to them.
        uint32_t syncOutputTouchTicks {}; // PAL-owned timestamp; 0 when not inside a synchronized block

        bool mouseX10 {}; // DECSET 9   — X10 compat: press-only
        bool mouseButtonTracking {}; // bool-ok: the DECSET mode family (mouseX10, mouseCellMotion, mouseAnyEvent) shares this noun shape // DECSET 1000 — normal: press + release
        bool mouseSGR {};
        bool mouseCellMotion {};
        bool mouseAnyEvent {};
        // Coordinate encodings, orthogonal to the tracking-mode flags above. Mouse.cpp's encoder
        // consults them in priority SGR-pixels (1016) > SGR (1006) > URXVT (1015) > UTF-8 (1005) > X10.
        bool mouseUTF8Coordinates {}; // bool-ok: the DECSET encoding family (mouseSGR, mouseURXVT) shares this noun shape // DECSET 1005 — UTF-8 coords
        bool mouseURXVT {}; // DECSET 1015 — URXVT decimal
        bool mouseSGRPixels {}; // DECSET 1016 — SGR pixel coords
        bool mouseRelativePointerLock {}; // Ubiquitty-private 7403 — pointer lock + hide + relative-delta mouselook
        // DECSET 1001 — highlight (hilite) mouse tracking. Host replies with the highlight region
        // (XTHIMOUSE); we consume it without blocking the parser, so a silent host cannot wedge it.
        // Latest region cached here for the frontend; hiliteRegionValid gates it.
        bool mouseHilite {};
        bool hiliteRegionValid {};
        int hiliteStartX {};
        int hiliteStartY {};
        int hiliteFirstRow {};
        int hiliteLastRow {};
        // Bounded-wait guard for the XTHIMOUSE handshake: rather than block, arm a
        // wall-clock deadline (PAL::monotonicTicks 60Hz). tickHilite aborts the await past the deadline;
        // an arriving reply clears it.
        bool hiliteAwaiting {};
        uint32_t hiliteDeadlineTick {};

        // DEC 1007. When set and alternate screen active, the frontend turns wheel ticks into
        // cursor-key presses (DECCKM-aware, Modes::alternateScrollKeys()) instead of scrolling scrollback.
        ::Ubiquitty::Modes::AlternateScrollMode alternateScrollMode {};

        // DEC 45. When set, Backspace/CUB at the left margin wraps to the right margin of the prior row.
        bool reverseWrap {}; // DECSET 45

        // Stored-only modes — cosmetic/out-of-scope, but round-trip through DECSET/DECRST + DECRQM.
        bool smoothScroll {}; // DECSCLM (4) — smooth scroll
        bool autoRepeat {}; // DECARM  (8) — keyboard auto-repeat
        bool nrcMode {}; // DECNRCM (42) — 7-bit national replacement on the main display
        bool backspaceKeySendsBackspace {}; // DECBKM (67) — Backspace key sends BS not DEL
        // Meta/Alternate keyboard discipline (xterm 1034/1036/1039), consulted by the key encoder:
        //   metaSendsEscape (1036) → Alternate+key sends ESC then key; else eightBitInput (1034) →
        //   key with bit 7 set; else ESC prefix. metaSendsEscape defaults ON (historical Option-as-Meta).
        bool metaSendsEscape {}; // mode 1036 (default on)
        bool alternateSendsEscape {}; // mode 1039 — Alternate+key sends ESC prefix
        bool eightBitInput {}; // mode 1034 — Meta sets the 8th bit (when not ESC)
        // xterm 1037. Set → Delete key sends DEL (0x7F); reset (default) → VT220 Remove CSI 3 ~.
        bool deleteKeySendsDelete {}; // mode 1037
        bool shouldHonorNumLock { true }; // mode 1035: NumLock overrides the application keypad
        Keys::KeyboardType keyboardType { Keys::KeyboardType::Default };
        Keys::TermcapProfile termcapProfile { Keys::TermcapProfile::Ubiquitty };
        bool graphemeClustering {}; // mode 2027 — Unicode grapheme clustering
        // xterm 35 (rxvt font-shifting). No glyph effect (single font); round-trips through DECRQM.
        bool fontShifting {}; // mode 35
        // xterm 1044. Set ⇒ primary selection reused as clipboard. Frontend reads it; core stores + reports.
        bool keepClipboard {}; // mode 1044
        bool inBandResize {}; // mode 2048 — in-band resize notifications
        bool fullscreenContent {}; // Ubiquitty-private 7402 — edge-to-edge, no padding, letterbox
        bool allow80To132 {}; // DECSET 40 — gate DECCOLM 80↔132 switching. Default off, as xterm's c132 resource: see Docs/conformance.md.
        // DECECM (DEC ?117). Set: ED/EL/ECH, DECERA and the status-line erases blank
        // to the screen's default colours. Reset (default): the blank keeps the pen's
        // colours, the background-colour-erase every xterm-family program expects.
        bool shouldEraseToDefaultColors {};
        // xterm ?1070. Set (default): each sixel starts from the default palette.
        // Reset: the colour registers persist in `sixelColorRegisters`, so an image
        // may use a register the previous one defined.
        bool hasPrivateSixelColorRegisters {};
        // xterm ?8452. Set: a finished sixel leaves the cursor on the image's last
        // row to the right of it. Reset (default): at the image's first column.
        bool shouldLeaveCursorRightOfSixel {};
        // mintty ?7723. Set (default): a width change reflows the primary screen
        // and scrollback. Reset: geometry-only, the rows keep their wrap points.
        bool shouldRewrapOnResize {};
        // mintty ?7700. Set: a change of the ambiguous-width choice reports
        // CSI 1 W (narrow) or CSI 2 W (wide) so a program re-measures its text.
        bool shouldReportAmbiguousWidth {};
        // foot ?77096. Set (default): the frontend's input method composes.
        // Reset: keys reach the encoder directly, as vim's normal mode wants.
        bool allowsInputMethodEditor {};
        // xterm's fastScroll, keepSelection and selectToClipboard resources
        // (modes 1014, 1040, 1041). Kept for DECRQM: the renderer draws whole
        // frames, and the frontend owns the selection and the pasteboard.
        bool shouldScrollFast {};
        bool shouldKeepSelection {};
        bool shouldSelectToClipboard {};
        // DECGEPM (43) and DECKBUM (68). Recorded for DECRQM and for DECSTR's
        // and RIS's defaults: expanded graphics printing belongs to a printer
        // the host does not drive, and the frontend's keyboard layout owns the
        // typewriter/data-processing key behaviour. Both default to reset.
        bool shouldExpandGraphicsPrint {};
        bool shouldUseDataProcessingKeys {};
        // The user-preference supplemental set (DECAUPSS, DECRQUPSS). DECSTR
        // and RIS return it to ISO Latin-1. A user preference, so only the
        // host answers for it.
        VT::SupplementalSet userPreferredSupplementalSet {};

    } modes;

    /// How many cells an East_Asian_Width=Ambiguous code point takes in this
    /// terminal's grid, seeded from `[terminal] ambiguous_width`.
    ///
    /// The width tables every terminal reads are built once and never
    /// rewritten, so this field is the only place the choice lives: two
    /// terminals in one process measure the same text independently, and a
    /// reload that moves one leaves the others' cell arithmetic alone.
    Ubiquitty::Unicode::Width::AmbiguousWidth ambiguousWidth = Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow;

    /// The ambiguous-width choice (1 or 2 cells) last reported under ?7700, so
    /// the next output can tell whether it moved; 0 until the mode is set.
    int ambiguousWidthAnnounced {};

    /// The sixel colour registers shared between images while ?1070 is reset:
    /// 256 entries of RGB, seeded from the DEC default palette at reset.
    std::array<std::uint8_t, Ubiquitty::Image::Sixel::paletteBytes> sixelColorRegisters {};

    bool windowFocused {}; // frontend-pushed key focus (default true); gates unfocused-bell notification.

    // Escape grammar the byte driver interprets. VT keeps the native DEC/ANSI hot path unchanged;
    // Linux/Sco layer console sequences on it; Wyse/Televideo/Adm/Heath/Hp switch to a legacy grammar.
    // Set once at session start via Modes::setEmulation; never changed by a host escape.
    Ubiquitty::VT::Emulation emulation {};
    // Legacy non-ANSI parser substate (only meaningful for non-ANSI grammars). 0 = ground, 1 = saw ESC,
    // 2 = ESC = saw row byte (awaiting column), 3 = ESC = awaiting row byte.
    uint8_t legacyState {};
    // Row byte captured by ESC = while the column is still pending (legacyState == 2). Biased as on the
    // wire; decoded to a 0-based row when the column lands.
    uint8_t legacyAddrRow {};
    bool transmit8bitC1 {}; // S8C1T/S7C1T (ESC SP G/F). Tracked for DECRQM round-trip; encoders emit 7-bit today.
    // DECSCL operating conformance level, stored as Pl code (61=VT100 .. 65=VT500). Default VT500.
    // Gates VT400+ ops (rectangular areas, DECSLRM) and the DA1/DECID device class. RIS/DECSTR restore default.
    Ubiquitty::VT::ConformanceLevel conformanceLevel {};
    // Protection mechanisms used since RIS; a conservative guard-scan shortcut.
    // Each cell's pen holds the protection that applies to that cell.
    Ubiquitty::VT::ProtectedMode protectedMode {};
    // ERM and GRCM (SM/RM 6 and 21). The host executes the controls they change; a
    // replica never does, so neither leaves the host. DECRQM keeps answering 0 for both.
    Ubiquitty::VT::ErasureMode erasureMode {};
    Ubiquitty::VT::RenditionCombination renditionCombination {};
    // Where DAQ began each qualified area, for EA's extent.
    Ubiquitty::Screen::QualifiedAreaStore qualifiedAreas;
    // XTCHECKSUM's calculation flags. Reset to Core's device-specific formula.
    VT::ChecksumExtensions checksumExtensions { VT::checksumInitialExtensions };
    // Answerback transmitted on ENQ. Settable only via config, never by a host escape, so it can't
    // echo attacker-chosen bytes back to the shell.
    FallibleString answerback;
    bool isAnswerbackConcealed = false;
    std::uint8_t keyPositionReportedModifiers = 0x30;
    std::uint8_t keyPositionDisabledModifiers = 0;

    OSC::ContextState contexts;
    OSC::ProgramStatusStore programStatus;
    uint64_t activityGeneration {};

    // OSC 9;4 — ConEmu progress. state 0=none,1=normal,2=error,3=indeterminate,4=paused; value 0..100
    // in state=1. Generation bumps on change so the PAL refreshes its indicator.
    uint8_t progressState {};
    uint8_t progressValue {};
    uint32_t progressGeneration {};
    /// OSC 666 progress properties; -1 denotes an unset property.
    int termpropProgressHint { -1 };
    int termpropProgressValue { -1 };
    // OSC 21337, iTerm2's session status (Docs/tab-status.md): a colored dot per session state. tabStatusSet=0
    // means none; when 1, tabStatusR/G/B hold the color. Generation bumps on change.
    uint8_t tabStatusSet {};
    uint8_t tabStatusR {};
    uint8_t tabStatusG {};
    uint8_t tabStatusB {};
    // The escape's `status` label ("Waiting", "Working") beside the colour, UTF-8, not NUL-terminated;
    // tabStatusTextLength bytes of it are live. Fixed-size so the runtime snapshot can carry it lock-free.
    std::array<char, OSC::tabStatusTextCapacity> tabStatusText {};
    uint8_t tabStatusTextLength {};
    uint32_t tabStatusGeneration {};
    // OSC 176 App ID: the identifier a program asks the window to be grouped and decorated by, UTF-8;
    // empty means unset. It is held out of line, as the answerback is: few programs set one, and 255
    // inline bytes would cross the ceiling on sizeof(Terminal) below. The App ID specification says
    // "Desktop application names are limited to 255 characters, so it is expected that App IDs stay
    // below this length"; the bound kept here is 255 bytes, and a longer ID is not taken.
    static constexpr std::size_t applicationIdentifierCapacity = 255;
    FallibleString applicationIdentifier;
    static_assert(applicationIdentifierCapacity < 256, "host handover carries the App ID's length in one byte");
    // iTerm2 tab colour (OSC 6;1;bg;<channel>;brightness;N): the colour a program asks its tab to take,
    // sent a channel at a time. tabColorSet=0 means none. A change bumps tabStatusGeneration, as the
    // indicator above does, so one wake covers both.
    uint8_t tabColorSet {};
    uint8_t tabColorRed {};
    uint8_t tabColorGreen {};
    uint8_t tabColorBlue {};
    // iTerm2 badge (OSC 1337 SetBadgeFormat): the text a program asks to be shown over its pane, UTF-8,
    // at most OSC::badgeTextCapacity bytes, kept as sent (its \(…) interpolations are not evaluated).
    // Empty means none. A change bumps tabStatusGeneration.
    FallibleString badgeText;
    // Attention a person has not seen yet: a bell that rang, a notification that was posted. Latched
    // here so a viewer who arrives later still learns of it; cleared only by markAttentionSeen. A
    // change bumps attentionGeneration.
    bool hasUnseenBell {};
    bool hasUnseenNotification {};
    // The newest unseen notification's words, each at most OSC::attentionTextCapacity bytes; empty
    // once seen. Kept so the mark can say what asked for attention.
    FallibleString unseenNotificationTitle;
    FallibleString unseenNotificationBody;
    uint32_t attentionGeneration {};
    // OSC 99 notifications a program has begun and not finished; see KittyNotification.h for the
    // bounds on what it holds.
    OSC::KittyNotificationAssembler kittyNotifications;

    // Linux-console power/display management. Timeouts store the minute counts (0 = never blank, a
    // display-sleep inhibit). screenBlanked = currently darkened (CSI 13 ] forces unblank). A frontend
    // hears the wake through WindowClient::setDisplayPower or reads the state off
    // Runtime::ModeSnapshot::isScreenBlanked. Reset on RIS.
    uint16_t screenBlankTimeout {}; // CSI 9 ; n ] — blank after n minutes (0 = never)
    uint16_t vesaPowerdownTimeout {}; // CSI 14 ; n ] — VESA powerdown after n minutes
    bool screenBlanked {}; // current DPMS-off state
    bool isCRTBlanked {};
    bool hasDisplayActivityTick {};
    uint32_t displayActivityTick {};

    // The parser owner detects password entry from line discipline, using
    // prompt text for raw or unknown modes, including an absent PTY.
    // Private mode 7401 asserts it directly.
    // The host's verdict is replicated; active protection masks local echo and
    // supplies the frontend's lock indicator and secure-input policy.
    bool passwordDetect {}; // config: detect password entry at a settled batch
    bool passwordPromptActive {};
    // Whether mode 7401 asserted the prompt. The application owns that state:
    // the heuristic neither raises nor clears the prompt while it stands.
    bool isPasswordPromptAsserted {};

    std::array<uint16_t, 2> kittyKeyboardFlags {};
    std::array<std::array<uint16_t, Ubiquitty::Kitty::maxKeyStackDepth>, 2> kittyKeyboardStack {};
    std::array<uint8_t, 2> kittyKeyboardStackLength {};
    std::array<uint8_t, Ubiquitty::VT::xtermKeyModifierCategoryCount> xtermKeyModifiers { Ubiquitty::VT::xtermKeyModifierInitialValues };
    std::array<uint8_t, Ubiquitty::VT::xtermKeyModifierCategoryCount> xtermKeyFormats {};

    // tab stops
    std::bitset<maxColumns> tabStops {};
    // Alignment storage is allocated only when a program defines a typed stop.
    FallibleVector<CharacterTabAlignment> tabAlignments;
    FallibleVector<std::uint32_t> tabAlignmentTargets;
    // A single-row staging terminal shares grapheme and width rules with normal output.
    std::unique_ptr<Terminal> alignedTabTerminal;
    FallibleVector<Cell> alignedTabOriginalRow;
    int alignedTabRow {};
    int alignedTabStop {};
    bool isAlignedTabActive {};
    bool didAlignedTabOverflow {};

    std::bitset<maxRows> lineTabStops {};
    /// The zero-based destination of carriage return and implicit line starts.
    int lineHome {};
    /// The one-based character position SLL set, or 0 while none has been set.
    int lineLimit {};
    /// Where CR and NEL move the active position (SIMD).
    VT::ImplicitMovementDirection implicitMovementDirection {};
    /// The zero-based column where the string the next QUAD positions began:
    /// the active position after the last formator function.
    int stringStartColumn {};

    // Per-Terminal allow-* policies, each gating an OSC/CSI family. Installed via setAllow* before
    // serving remote bytes. Default true (permissive), so ordinary apps work unconfigured.
    bool allowTitleOperations {}; // OSC 0/1/2 — set window/icon title
    bool allowFontOperations {}; // OSC 50 — set/query font
    bool allowColorOperations {}; // OSC 4/10/11/12/110/111/112
    bool allowWindowOperations {}; // CSI <Ps> t — XTWINOPS resize/move
    bool allowTerminalCapabilityOperations {}; // DCS + q … ST — XTGETTCAP replies
    bool allowMouseOperations {}; // DECSET 9/1000/1002/1003/1006/1015/1016 — mouse tracking
    bool allowPasteControls {}; // DECSET 2004 — bracketed-paste mode
    // The per-OSC / per-DCS / per-APC sequence gate ([security] osc_*, dcs_*, apc_* allow/deny). Owned by
    // value: two Terminals in one process share no policy, and a re-homed session carries its own.
    OSC::SequenceGate sequenceGate;

    // OSC 50 — current font name. Empty = use host default. Core stores; renderer/PAL reads at layout.
    FallibleString font;

    // Query intent and actual adopted state differ while an operation awaits
    // the frontend. A newer accepted family cannot be overwritten by an older
    // adoption, and size-only operations resolve against acknowledged state.
    OSC::FontRequestQueue fontRequests;

    // Bell window behaviors. bellRaisesUrgency (DECSET 1042) sets the urgency hint; popOnBell (1043)
    // raises/focuses the window. Both default off.
    bool bellRaisesUrgency {};
    bool popOnBell {};

    // MC uses 0 for normal printing and 5 for transparent printer control.
    uint8_t printMode {};
    bool shouldIgnorePrinterNUL { true };
    bool shouldCopyRightToLeft {};
    bool shouldWriteRightToLeft {};
    std::array<char, 4> printerControlPrefix {};
    uint8_t printerControlPrefixLength {};
    // DEC MC media-copy print sink. When non-null, controller-mode and print-screen/line/auto-print
    // captures are appended verbatim. Opaque FILE*. Installed by setPrintSinkPath; closed by dispose.
    void* printSinkFile {};
    bool printFormFeed {}; // DECPFF (CSI ? 18 i) — append FF after each page. Default off.
    bool printExtentFullScreen {}; // DECPEX (CSI ? 19 h/l) — full screen (set) vs scrolling region (reset, the VT510 default).
    bool autoPrint {}; // CSI ? 5 i / ? 4 i — copy each completed line to the print sink.

    // rxvt show/hide modes ?10 (toolbar) / ?30 (scrollbar). Stored + DECRQM; ?30 rides the mode
    // snapshot (Runtime::ModeSnapshot::isScrollbarVisible) that every frontend already reads, so a
    // bar cannot be hidden on one shell and shown on another. Default shown (true).
    // Mode 10, rxvt's toolbar. No frontend has a terminal toolbar, so DECRQM
    // answers it permanently reset and no control writes this; it keeps its
    // place because the checkpoint and pane-record layouts carry it.
    bool toolbarVisible {};
    bool scrollbarVisible { true }; // mode 30 — shown before any reset, stored only when the WindowMutate gate allows it

    // mode ?41 "more(1) fix": suppresses the spurious blank line from the extra char more(1)/old curses
    // emit past the right margin. Suppression lives in putCharacter. Default off.
    bool moreFix {};

    // mode ?46 host-visible logging bit (DECRQM-reported). Frontend session logging has its own intent
    // below so host RIS/DECRST cannot silently truncate a user's audit log.
    bool loggingEnabled {}; // DEC private mode 46
    bool sessionLogCaptureEnabled {}; // frontend File-menu logging intent

    // DEC VT300 locator protocol (CSI ' z/{/|/w):
    //   locatorEnabled/OneShot — DECELR Ps1 reporting on / one-shot.
    //   locatorUseRows — DECELR Ps2: cells (true) vs pixels (false).
    //   locatorReportDown/Up — DECSLE: emit DECLRP on button down/up.
    // Frontend routes events through locatorReportEvent; parser answers DECRQLP.
    bool locatorEnabled {};
    bool locatorOneShot {};
    bool locatorUseRows {}; // false ⇒ pixels-mode (DECELR Ps2 = 1)
    bool locatorReportDown {};
    bool locatorReportUp {};

    // Where the locator last was, as the frontend published it: 1-based cells, or device
    // pixels when locatorUseRows is false. DECLRP's Pr;Pc are the LOCATOR's coordinates,
    // never the text cursor's, and DECEFR resolves an omitted bound to "the current locator
    // position" — neither is answerable without this. 0 = no pointer event seen yet, which
    // DECRQLP answers with the coordinates omitted (locator position undefined).
    uint16_t locatorRow {};
    uint16_t locatorColumn {};
    uint8_t locatorButtons {}; // DECLRP Pb mask of the buttons held at that position

    // DECEFR (CSI ' w) filter rectangle, armed with fully resolved 1-based inclusive bounds
    // (csiH_decefr substitutes the live locator position for an omitted one). A DECLRP
    // (Pe=10) fires the first time the locator leaves the rect, then it auto-disarms.
    bool locatorFilterActive {};
    uint16_t locatorFilterTop {};
    uint16_t locatorFilterLeft {};
    uint16_t locatorFilterBottom {};
    uint16_t locatorFilterRight {};

    // Session log — when non-null, every byte processedBytes sees is also written verbatim (raw wire,
    // pre/post tmux decode). Opaque FILE*. Installed by setSessionLogPath; closed by dispose.
    void* sessionLogFile {};

    // DECUDK — User-Defined Keys. Pc clear (0 all / 1 named), Pl lock (0 lock / 1 unlocked). Each Ky/St
    // maps a DEC function-key code to a hex transmit string; the input encoder consults udk on keypress.
    // Null until the first programmed key; absent means no programmed keys.
    std::unique_ptr<Ubiquitty::VT::UserDefinedKeys> udk;

    // DECDLD downloadable soft character set (DRCS). DCS decodes sixel-style glyph bitmaps here; SCS maps
    // a G-set to it, the renderer draws them. Cleared by RIS / Pe; persists across DECSTR. Fixed caps.
    // Null until the first DECDLD; absent means no valid soft font.
    std::unique_ptr<Ubiquitty::VT::SoftFont> softFont;

    // DCS + s downloadable samples, one slot per DECPS note, for the sample
    // voice; and the DECPS requests a host forwards to attached windows.
    // Null until the first download or request; RIS drops both with the
    // Terminal, as it drops the soft font.
    std::unique_ptr<Ubiquitty::VT::SampleBank> sampleBank;
    std::unique_ptr<Ubiquitty::VT::PendingSound> pendingSound;

    // DECAC (CSI Ps1 ; Pf ; Pb , |) item colours, reported by DECRQSS. The normal text colours
    // are resolved into the cells of both screens when assigned (Screen::applyNormalTextColors)
    // and into the pen (penApplyNormalTextColors) wherever SGR, DECSTR, DECRC or XTPOPSGR returns
    // it to the default; the window frame colours are only held and reported.
    ColorAssignment normalTextColors = unassignedColors;
    ColorAssignment windowFrameColors = unassignedColors;
    // DECATC (CSI Ps1 ; Pf ; Pb , }) colours for each attribute combination, held and reported
    // by DECRQSS. The VT520 uses them only under its alternate colour look-up table (DECSTGLT 1),
    // which Core does not offer: text keeps its ANSI SGR colours, so the table is state alone. It is
    // held out of line, null until a DECATC assigns a combination, as few programs send one; read
    // it through alternateTextColorTable.
    std::unique_ptr<AlternateTextColors> alternateTextColors;

    // DECSSDT (CSI Ps $ ~) status line type: 0=none, 1=indicator (VT220 default), 2=host-writable.
    // DECSASD (CSI Ps $ }) active page: 0=data screen, 1=status line. Renderer reads statusLineType;
    // emit code reads statusLineActive to redirect writes to statusRow.
    uint8_t statusLineType {};
    bool isStatusNationalReplacementEnabled {};
    // statusLineActive lives in the hot parse-loop cluster up top.
    // One-row buffer used while statusLineActive == 1, separate from Screen so the data screen is unperturbed.
    // 32 KB of cells. Null until the first status-row write; absent reads as blanks.
    std::unique_ptr<Rendering::StatusRowStorage> statusRow;
    int statusCursorX {};

    bool titleChanged {};
    // An OSC 50 recorded a font swap this drain; consumed like titleChanged.
    // The generation beside the request identifies the swap; this says a
    // drain carries one, so a frontend that reads its facts on an event rather
    // than polling has something to be woken by. A font swap moves no cell, so
    // without it the drain that parsed the request is the last one to run.
    bool fontRequestChanged {}; // bool-ok: reads with its sibling titleChanged, and is consumed the same way
    bool currentDirectoryChanged {}; // latched by setCurrentDirectory on a value change; consumed like titleChanged
    bool bellPending {};

    // Bell policy. bellMode = frontend preference; lastBellTick = coalescer's last-fired timestamp.
    // Both consumed by resolveBellEvent.
    Ubiquitty::BellMode bellMode {};
    uint32_t lastBellTick {};

    // Coherence facts a publish gate reads between frames: how often the
    // program closed a DEC 2026 block, and how often it hid a shown cursor
    // (DECTCEM). Counters rather than flags, so a reader that samples them at
    // drain-pass boundaries sees a close or a hide that happened inside one
    // read. Written by the mode handlers only; wrapping is harmless, since a
    // reader compares two samples for inequality.
    uint32_t syncOutputCloseCount {};
    uint32_t cursorHideCount {};

    // Text-sizing policy: whether this terminal's renderer draws a sized
    // block's vertical bands. False — the default — makes OSC 66 print its
    // run at the ordinary size, which is what a renderer without the band
    // crop must do: the protocol's detection reads the cursor advance, so a
    // terminal that advances by a block it cannot draw reports a capability
    // it does not have, and the application then trusts a layout the screen
    // contradicts. One Core serves frontends whose renderers differ, so the
    // answer is per Terminal and the frontend that proved its crop against a
    // golden is the one that sets it.
    bool drawsSizedText {};

    // The renderer supports OSC 66 p=1 runs independently of sized blocks.
    // False leaves measurement queries unanswered and prints runs as grid text.
    // The frontend sets this before processing input (Docs/proportional-text.md).
    bool drawsProportionalText {};

    // Window title, UTF-8. Frontends fold to their renderer's encoding at paint.
    FallibleString titleUTF8;
    // XTSAVE / XTRESTORE (CSI ? Ps s / r). Mode numbers reach 7400, so this is an
    // association list rather than a bitmap; a repeat save overwrites its slot and
    // a full table drops the oldest. 0 marks a free slot (no mode 0 exists).
    static constexpr int savedPrivateModeSlots = 32;
    std::array<uint16_t, savedPrivateModeSlots> savedPrivateModeNumbers {};
    std::array<bool, savedPrivateModeSlots> savedPrivateModeStates {};
    // xterm title modes (CSI > Ps t set / CSI > Ps T reset), bit Ps: 0 = set labels in hex,
    // 1 = report labels in hex, 2 = set in UTF-8, 3 = report in UTF-8. Bits 0/1 are the ones with
    // an observable effect; 2/3 are their defaults and only matter for round-tripping the mode.
    uint8_t titleModes {};
    // XTPUSHSGR / XTPOPSGR (CSI # { and CSI # }): each frame is the pen at the push and the mask of
    // XTPUSHSGR parameters it named (bit Ps - 1), so a pop restores only those attributes. Ten
    // frames, xterm's MAX_SAVED_SGR. The count runs past the capacity, as xterm's does: a push past
    // the tenth saves nothing, and the pop that matches it restores nothing.
    static constexpr std::size_t savedRenditionCapacity = 10;
    std::array<uint32_t, savedRenditionCapacity> savedRenditionMasks {};
    std::array<Pen, savedRenditionCapacity> savedRenditionPens {};
    uint16_t savedRenditionCount {};
    // Icon name (OSC 1), UTF-8. Separate from the window title: OSC 0 sets both, OSC 1 and OSC 2 set one
    // each, and CSI 20 t / 21 t report them independently.
    FallibleString iconNameUTF8;

    // Per-Terminal frontend clients (PAL/TerminalClients.h); non-owning, frontend outlives Terminal.
    // Null when the frontend installs none; Core no-ops rather than calling.
    //
    // windowClient is gated (Core/EffectGate.h): its pointer is private, so an
    // effect site reaches it through effectClient() and names the capability it
    // is about to exercise, or through clientForEffectThatEscapesNothing() and
    // says that it exercises none. Neither is optional — there is no accessor
    // that answers without being asked which of the two this is.
    Ubiquitty::GatedClient<Ubiquitty::WindowClient> windowClient {};
    Ubiquitty::CheckedPtr<Ubiquitty::ClipboardClient> clipboardClient {};
    // Set only by a frontend whose Terminal mirrors a remote screen; see MirrorClient.
    Ubiquitty::CheckedPtr<Ubiquitty::MirrorClient> mirrorClient {};
    // This terminal's own effect sink; see EffectRoute. Unset ⇒ an effect is
    // only recorded on the terminal (bellPending), for the drain to pick up.
    Ubiquitty::CheckedPtr<Ubiquitty::EffectRoute> effectRoute {};
    // This terminal's clock; see TerminalClock and terminalNow. Unset ⇒ the
    // process-wide PAL clock.
    Ubiquitty::CheckedPtr<Ubiquitty::TerminalClock> tickClock {};
    // Whether this terminal answers its program's queries; see replyDisposition.
    Ubiquitty::ReplyAuthority replyAuthority {};
    Ubiquitty::CheckedPtr<Ubiquitty::ShellClient> shellClient {};
    Ubiquitty::GatedClient<Ubiquitty::DragDrop::Client> dragDropClient {};
    std::unique_ptr<Ubiquitty::DragDrop::Session> dragDropSession;
    Ubiquitty::CheckedPtr<Ubiquitty::DiagnosticsClient> diagnosticsClient {};
    Ubiquitty::CheckedPtr<Ubiquitty::FileReadClient> fileReadClient {};
    // Measures a proportional run for OSC 66 p=1:q=1. Unset ⇒ Core answers the grid-face width.
    Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient> textMeasureClient {};
    // Runs a matched trigger's action (notify/copy/title/highlight/run). Unset ⇒ Core drops the action.
    Ubiquitty::CheckedPtr<Ubiquitty::TriggerClient> triggerClient {};
    // Default-deny gate for grid-escaping side effects. Unset ⇒ Core answers PAL's defaultDecision for the
    // current origin: Local runs every effect, Agent and Remote are refused host-escaping and exfil ones.
    // The effectGranted file reads (Kitty t=f/t=t/t=s) refuse every origin without a policy.
    Ubiquitty::GatedPolicy capabilityPolicy {};
    // Principal driving the current input. Core passes it to capabilityPolicy->check, so a grant is keyed
    // (Capability × Origin) — a Local grant never extends to Agent/Remote. Default Local.
    Ubiquitty::CapabilityOrigin currentCapabilityOrigin {};

    // XTWINOPS title stack (CSI 22/23 t). One stack of frames, not one per label: each frame holds an
    // optional window label and an optional icon label — Ps 0 records both, Ps 1 the icon, Ps 2 the
    // window. A pop consumes the WHOLE top frame however few labels it restores, and a label the
    // frame never recorded is taken from the nearest frame below that did. The *Valid bitmasks say
    // which labels frame i recorded. Ten frames, xterm's MAX_SAVED_TITLES, which XTTITLEPOS reports
    // as the stack's maximum index; the oldest frame is dropped on overflow. A third XTWINOPS
    // parameter, 1 to 10, stores into or reads from that frame without moving the length.
    static constexpr std::size_t titleStackCapacity = 10;
    std::array<FallibleString, titleStackCapacity> titleUTF8Stack; // raw-UTF-8 mirror so a pop restores the displayed title
    std::array<FallibleString, titleStackCapacity> iconNameUTF8Stack;
    uint8_t titleStackLength {}; // frames in use — indexes both arrays
    uint16_t titleStackValid {}; // bit i: frame i recorded a window label
    uint16_t iconNameStackValid {}; // bit i: frame i recorded an icon label

    // OSC 22 mouse-pointer-shape stack. Top = live shape; empty = Default. Fixed depth 16, the
    // protocol's minimum; a push past the top drops the oldest FIFO.
    std::array<MousePointerShape, 16> mousePointerShapeStack {};
    uint8_t mousePointerShapeStackLength {};
    std::array<MousePointerShape, 16> alternateMousePointerShapeStack {};
    uint8_t alternateMousePointerShapeStackLength {};

    // The drawable's extent in device pixels: the whole surface a frame paints, padding and the
    // sub-cell remainder included. Under DECSET 7402 a fitted image fills it from its corner
    // whatever the letterbox, so CSI 14 t and the SGR-pixel clamp measure it rather than the grid
    // while the mode holds (reportedTextAreaPixels decides). 0 = not published; the grid stands in.
    // Beside the image store it sizes, in the four bytes of padding ahead of it, rather than beside
    // the text-area extent: placed there the pair would grow the struct to a whole number of Cells, and a
    // Terminal that size cannot be told from a grid store by size, which is how
    // Tests/Host/create_session_refusal.cpp names the allocation it refuses.
    uint16_t drawablePixelWidth {};
    uint16_t drawablePixelHeight {};

    // Inline images: kitty/APC graphics tables, shared placement model, and the DECSIXEL accumulator
    // that feeds the same store.
    struct Images {
        // Kitty graphics protocol state (https://sw.kovidgoyal.net/kitty/graphics-protocol/). Parser
        // consumes APC frames without emitting cells; supported
        // direct RGB/RGBA images are retained here.
        std::array<Image::TerminalImage, Image::maxTerminalImageCount> stored {};
        // Placement table: where each stored image is drawn. Every renderer reads image geometry from
        // here alone.
        std::array<Image::TerminalImagePlacement, Image::maxTerminalImagePlacementCount> placements {};
        // The counter behind Image::allocateImageID; the ids it hands out sit in the top half of the
        // range, above anything a kitty client names.
        uint32_t kittyNextImageID {};
        // Used placement slots, maintained O(1) at each used-flag transition. The scroll hot path gates on
        // this. Not kittyNextImageID: that is the auto-id allocator and stays 0 for explicit-id clients
        // (a=T,i=N), which would pin their placements while text scrolls beneath.
        uint32_t livePlacementCount {};
        // Per-Terminal image-pixel budget (bytes) + monotonic LRU stamp source. Budget defaults in init();
        // 0 would evict all images.
        uint64_t kittyImageBytesBudget {};
        uint64_t kittyImageUseSeq {};
        // Monotonic source for TerminalImage::contentGeneration — bumped on every pixel (re)assignment or
        // frame add so a same-id re-transmit invalidates the GPU texture cache. First value is 1.
        uint64_t kittyImageContentSeq {};
        Ubiquitty::ByteBuffer kittyAPC;
        bool kittyAPCOverflow {};
        Ubiquitty::ByteBuffer kittyChunk;
        bool kittyChunkActive {};
        Ubiquitty::Kitty::GraphicsControl kittyChunkControl {};
        // DECSIXEL. Parser accumulates payload bytes between DCS ... q and ST/BEL into
        // sixelAccumulatorBuffer, then decodes to RGBA and places through the unified image model.
        // sixelOriginRow/Column snapshot the cursor at sixel start (the image anchors there).
        uint8_t* sixelAccumulatorBuffer {};
        int sixelAccumulatorLength {};
        int sixelAccumulatorCapacity {};
        bool sixelAccumulatorOverflow {};
        uint16_t sixelOriginRow {};
        uint16_t sixelOriginColumn {};
    } images;

    // Per-Terminal OSC 52 clipboard security policy, seeded from config at init. handleClipboard reads only
    // these fields — no shared mutable global on the ingest thread.
    bool isOSC52Allowed { true }; // master toggle: false = parse+consume, no hook
    Ubiquitty::OSC::ClipboardReadPolicy osc52ClipboardReadPolicy { Ubiquitty::OSC::ClipboardReadPolicy::Prompt };
    Ubiquitty::OSC::ClipboardWritePolicy osc52ClipboardWritePolicy { Ubiquitty::OSC::ClipboardWritePolicy::Allow };
    // One OSC 52 read query may be outstanding with the frontend at a time. A query is answered
    // asynchronously — the frontend allocates a request and, under Prompt, raises a dialog — while the
    // stream that asked keeps running. Without this, a stream emitting the query in a loop makes the
    // frontend allocate and stack one of each per repeat, faster than a user can dismiss them.
    // Set when the frontend accepts a query, cleared by completeClipboardReadQuery. Survives RIS: the
    // stream that can flood the query can also emit RIS, so clearing it there would hand the flood back.
    bool isOSC52ClipboardReadInFlight {};
    // Whether the outstanding read's request ended with BEL, which its deferred
    // answer then ends with too.
    bool isClipboardReadRequestBellTerminated {};

    // OSC 52 chunked write accumulator. A SET whose base64 ends in '!' appends and waits; the first SET
    // to the same selection without the marker appends+decodes+applies. Malloc'd lazily, grown to
    // maxOSC52ChunkSize, freed in dispose. osc52ChunkSet snapshots the transfer's selection so an
    // interleaved SET to a different selection aborts cleanly.
    Ubiquitty::ByteBuffer osc52Chunk;
    bool osc52ChunkActive {};
    // Poison flag: once a transfer overflows maxOSC52ChunkSize, remaining chunks are swallowed and the
    // terminating SET drops without applying, so an oversize stream can't partially apply. Cleared on reset.
    bool osc52ChunkOverflow {};
    std::array<char, 16> osc52ChunkSet {};

    // OSC 5522 extended-clipboard state (Core/OSC/ExtendedClipboard.cpp). The
    // protocol shares OSC 52's policy triplet above — one clipboard-escape
    // policy, named for the sequence it first governed — and shares
    // isOSC52ClipboardReadInFlight as the one-outstanding-read invariant, so a
    // stream cannot stack prompts by alternating protocols. What is 5522's own
    // is the metadata a deferred read must echo back, and the write session a
    // chunked wdata stream accumulates into.
    //
    // Which framing an outstanding read completes with. The frontends answer
    // every clipboard read through one completion call; without this the
    // completion would frame a 5522 answer as an OSC 52 reply, which the
    // requester cannot parse.
    bool isPendingClipboardReadExtended {};
    // The deferred 5522 read's echo state: the sanitized id and the location,
    // captured at the query so the completion frames what was asked. 64 id
    // characters, because the spec's own recommendation for a generated id is
    // a UUID4 (36) and a multiplexer routes responses by matching the echo —
    // a truncated echo routes nowhere.
    std::array<char, 65> extendedClipboardReadID {};
    bool isExtendedClipboardReadPrimary {};
    // The 5522 write session: decoded (never base64) payloads accumulated
    // across wdata packets, one buffer per MIME up to
    // maxExtendedClipboardWriteItems, the total capped at
    // maxExtendedClipboardWriteBytes, freed in dispose. MIMEs past the item
    // cap are dropped — the type list then shows what is held, so a probe
    // reads the truth.
    std::array<Ubiquitty::ByteBuffer, 8> extendedClipboardWriteItems;
    std::array<std::array<char, 128>, 8> extendedClipboardWriteItemMimes {};
    int extendedClipboardWriteItemCount {};
    int extendedClipboardWriteTotalBytes {};
    bool isExtendedClipboardWriteActive {};
    bool isExtendedClipboardWritePrimary {};
    bool didExtendedClipboardWriteOverflow {};
    std::array<char, 65> extendedClipboardWriteID {};
    // Consecutive packets for one MIME form a single base64 value.
    std::array<char, 128> extendedClipboardWriteMime {};
    std::array<char, 4> extendedClipboardBase64Tail {};
    std::uint8_t extendedClipboardBase64TailLength {};
    bool isExtendedClipboardBase64Complete {};
    // Alias names, space-separated. The session set accumulates walias
    // declarations targeting the text; a successful commit publishes it as
    // the served set — and a commit that declared none clears it, because the
    // aliases described the payload the commit just replaced. The served set
    // is what a read may match a requested MIME against beyond text/plain.
    // Not carried across RIS: the aliases describe the last committed
    // payload, and a reset severs that provenance where the clipboard's own
    // text survives outside this terminal.
    Ubiquitty::ByteBuffer extendedClipboardSessionAliases;
    Ubiquitty::ByteBuffer extendedClipboardServedAliases;
    // The MIME names the outstanding deferred read requested,
    // space-separated. Which of them the clipboard serves is resolved when
    // the completion runs — a typed client cannot answer before its capture —
    // and like the rest of the outstanding read the carry survives RIS whole.
    std::array<char, 256> extendedClipboardReadMimes {};
    // Whether the outstanding deferred read asked for the type list. A typed
    // asynchronous client lists from its capture, so even the promptless list
    // defers on such a client, and the completion must know which answer to
    // frame.
    bool isExtendedClipboardReadListRequested {};

    bool isSixelDCS {}; // hot-path routing: sixel payloads land in sixelAccumulatorBuffer not the OSC accumulator.
                        // Derived from dcsHandlers at header-final-byte time; cached to keep the parser branch-light.
    bool dcsHeaderDone {};
    // Set once per DCS frame when the OSC accumulator's cap clips payload bytes; surfaces as a diagLog("dcs-truncated")
    // event. Cleared on each DCS-start.
    bool dcsTruncated {};
    // DCS sub-protocol routing. Header: DCS <params> <intermediate?> <final> <payload> ST. (intermediate,
    // final): (0,'q')→DECSIXEL, ('$','q')→DECRQSS, ('+','q')→XTGETTCAP. Else falls to tmux-passthrough.
    uint8_t dcsIntermediate {};
    uint8_t dcsFinal {};
    int dcsTmuxPassthroughDepth {};
    // DCS header parameters (like CSI: semicolon-separated ints). Captured during header phase; sub-protocol
    // handlers read them at terminator.
    std::array<int, Ubiquitty::VT::maxParameters> dcsParameters {};
    int dcsParamCount {};
    // The DCS twin of Parser::didParametersOverflow.
    bool didDCSParametersOverflow {};

    // DECSCUSR cursor style. 0 = user preference; 1/2 block, 3/4 underline, 5/6 bar (odd blink, even steady).
    // The renderer never reads this directly — call Cursor::resolve(), which returns a Cursor::RenderState.
    Ubiquitty::Cursor::Style cursorStyle {};

    // Monotonic tick of the most recent user activity (frontends call noteUserActivity). resolveCursor
    // freezes blink on while typing is recent so the cursor doesn't vanish mid-keystroke. 0 = none yet.
    uint32_t lastActivityTick {};

    // Frames the cursor holds solid after activity before blink resumes. 0 = default (cursorActivityHoldTicks).
    uint32_t cursorHoldFrames {};

    // Cursor blink period (ms, one full cycle). 0 = default (Cursor::Blink::caretPeriodTicks, 1000ms at 60Hz).
    // Milliseconds so it is frame-rate independent; resolveCursor converts ms↔ticks at 60Hz.
    uint32_t cursorBlinkPeriodMilliseconds {};

    // User preference: when false, the cursor never blinks regardless of DECSCUSR. Default true.
    bool cursorBlinkPref {};

    // The program's blink request, as xterm keeps it (charproc.c cursor_blink_esc): DECSET/DECRST 12
    // set and reset it, a blinking DECSCUSR style sets it and a steady one resets it, so DECRQM 12 tells
    // which the program chose last. DECSTR and RIS reset it. It does not move the drawn blink: the
    // reader's [cursor] blink governs the default shape, so a program's cnorm cannot steady it.
    // Default false.
    bool cursorBlinkAtt610 {};

    // Default cursor shape when no DECSCUSR style is active (cursorStyle == 0). 0 block, 1 underline, 2 bar.
    uint8_t cursorShapePref {};

    // The [cursor] motion preferences, seeded beside the blink fields by
    // Preferences::Effective::applyCursorPreferences so every route a terminal
    // reaches a frontend by carries them. The frame capture hands them to
    // whichever animator ticks Rendering::CursorMotion, so no frontend holds a
    // second settings store for the caret. Off never glides; the duration is
    // milliseconds to reach the target cell, 0 = the compiled default.
    Ubiquitty::Rendering::CursorMotionMode cursorMotionModePref {};
    uint32_t cursorMotionDurationMilliseconds {};

    // Cell size in device pixels, published by the frontend for the XTWINOPS pixel-geometry reports.
    // 0 = not reported yet (handler falls back to a nominal cell).
    uint16_t cellPixelWidth {};
    uint16_t cellPixelHeight {};

    // Actual drawn text-area extent in device pixels (round(columns * cellWidthPoints * scale)), not
    // columns * cellPixelWidth — the fractional per-cell advance accumulates a rounding residue that drifts
    // ~tens of px by column 80. CSI 14 t and CSI 15 t's fallback report this; cellPixelWidth stays for
    // CSI 16 t (one cell). 0 = not published yet → fall back to columns * cellPixelWidth.
    uint16_t textAreaPixelWidth {};
    uint16_t textAreaPixelHeight {};

    // CellGrid size in device pixels for CSI 15 t. 0 = unknown (falls back to text-area size).
    uint16_t screenPixelWidth {};
    uint16_t screenPixelHeight {};

    // DECSET 7402 fullscreen-content display state, set by OSC 7402. Reset to defaults by a bare OSC 7402;
    // preserved across DECSTR, cleared by RIS. Consumed only while edge-to-edge is active.
    FullscreenContentDisplay fullscreenContentDisplay {};

    // Window top-left in device pixels (screen coords, y down) for CSI 13 t. Signed: a window can sit
    // left of / above the main display's origin.
    // Frontend-published miniaturised state; XTWINOPS 11 t reports it.
    bool windowIconified {};
    int32_t winPosX {};
    int32_t winPosY {};

    // terminal-to-host outbound buffer (DSR, DA, mouse)
    std::array<char, 256> outputBuffer {};
    int outputLength {};
    // Reply-pressure line for the ingest parse: at or past this many queued
    // bytes, processBytesUpToSyncClear yields at its next dispatch boundary so
    // the caller can drain the buffer to the stream and resume. Without the
    // yield a query burst inside one chunk (a 256-color palette walk, a DA
    // loop) overflows the fixed buffer mid-call and writeOutput's guard drops
    // the surplus replies silently — the host blocks on answers that never
    // come. Half the buffer, so the dispatches that land before the next
    // check still have a full reply's headroom.
    static constexpr int outputReplyPressureBytes = 128;
    static_assert(outputReplyPressureBytes * 2 == sizeof(outputBuffer), "the pressure line tracks the buffer it protects");

    // Terminal-to-host bulk outbound, for a reply the fixed buffer cannot hold:
    // a chunked OSC 5522 clipboard read runs to a megabyte of base64 where the
    // buffer above holds 256 bytes and respond() drops what would not fit.
    // Drained by drainOutput after the fixed buffer, so the one flush loop
    // serves both; freed the moment it empties, because a bulk reply is rare
    // and a megabyte of dead capacity per idle terminal is not. Bounded by
    // maxBulkOutputBytes through respondBulk, whose caller owns what to do
    // about a refusal — a reply is composed of many packets, and only the
    // composer knows how much of a partial composition to take back.
    Ubiquitty::ByteBuffer bulkOutput;

    // OSC selection-color + host-identity state, owned by the OSC dispatcher (Core/OSC/Dispatch.cpp).
    // OSC 17/19 set the selection background/foreground color. 16-bit RGB, the same wire convention as
    // OSC 4/10/11/12; the *Set flags distinguish host-set from the default for ? queries.
    uint16_t selectionBackgroundRed {};
    uint16_t selectionBackgroundGreen {};
    uint16_t selectionBackgroundBlue {};
    uint16_t selectionForegroundRed {};
    uint16_t selectionForegroundGreen {};
    uint16_t selectionForegroundBlue {};
    bool selectionBackgroundSet {};
    bool selectionForegroundSet {};
    // kitty's OSC 21 cursor_text: the color the glyph under a focused block cursor is drawn in. Unset,
    // the renderer gives that glyph its own contrast with the cursor.
    uint16_t cursorTextRed {};
    uint16_t cursorTextGreen {};
    uint16_t cursorTextBlue {};
    bool hasCursorTextColor {};

    // OSC 13/14 (mouse pointer fg/bg), 15/16 (Tek fg/bg), 18 (Tek cursor): remaining xterm dynamic-color
    // siblings of OSC 10/11/12; same wire/*Set semantics. Tek slots feed the Tek 4014 renderer; pointer
    // slots (13/14) are store/query/reset only (macOS NSCursor has no tint API).
    uint16_t mousePointerForegroundRed {};
    uint16_t mousePointerForegroundGreen {};
    uint16_t mousePointerForegroundBlue {};
    uint16_t mousePointerBackgroundRed {};
    uint16_t mousePointerBackgroundGreen {};
    uint16_t mousePointerBackgroundBlue {};
    uint16_t tektronixForegroundRed {};
    uint16_t tektronixForegroundGreen {};
    uint16_t tektronixForegroundBlue {};
    uint16_t tektronixBackgroundRed {};
    uint16_t tektronixBackgroundGreen {};
    uint16_t tektronixBackgroundBlue {};
    uint16_t tektronixCursorRed {};
    uint16_t tektronixCursorGreen {};
    uint16_t tektronixCursorBlue {};
    bool mousePointerForegroundSet {};
    bool mousePointerBackgroundSet {};
    bool tektronixForegroundSet {};
    bool tektronixBackgroundSet {};
    bool tektronixCursorSet {};
    // The user's storage-tube colours, which an unset OSC 15/16/18 colour
    // reports through the resolvers in Tek/Colors.h. Seeded from the
    // configuration by Runtime::seedTerminalConfiguration.
    Tek::ColorPreference tekColorPreference {};

    // Live palette (default fg/bg, 256 indexed, cursor) and the baseline OSC 110/111/112 revert to.
    // Frontend pushes theme colors on apply; OSC color ops mutate palette; renderer reads it.
    Ubiquitty::TermPalette palette {};
    Ubiquitty::TermPalette paletteBaseline {};

    /// Retains an indexed palette and its dynamic OSC colour overrides.
    ///
    /// A slot remains available after a pop for an explicit indexed restore.
    struct SavedPalette {
        TermPalette palette {};
        std::array<uint16_t, 21> dynamicChannels {};
        std::array<bool, 7> dynamicSet {};
        /// kitty's `cursor_text` colour, read only under `hasCursorTextColor`.
        PaletteRGB cursorTextColor {};
        bool hasCursorTextColor {};
    };
    static constexpr size_t paletteStackDepth = 10;
    using PaletteStack = std::array<SavedPalette, paletteStackDepth>;
    std::unique_ptr<PaletteStack> savedPalettes;
    uint16_t savedPaletteValid {};
    uint8_t savedPalettePosition {};
    uint8_t savedPaletteCount {};

    // Bumped once for every non-empty wire batch the parser applies. This is
    // the model epoch an immutable frame records: conservative control-only
    // batches may advance it, but two different applied batches never share an
    // epoch merely because no painter drained the grid between them.
    std::uint64_t modelEpoch = 0;

    // Bumped by every palette mutation that moved a colour — OSC 4/10/11/12,
    // the reset family, a frontend theme apply. A palette write recolours
    // cells without touching them, so damage tracking alone lets a retained
    // frame keep the replaced colours on every clean row; a renderer folds
    // this into its reuse verdict instead. Moves only when a colour did:
    // applications re-assert their palette every frame, and an epoch that
    // ticked on no-op writes would break grid reuse permanently.
    std::uint64_t paletteEpoch = 0;

    // Advances on a whole-screen erase — ED 2, and the alternate-screen entry
    // that clears. A frontend holds its next present briefly after one, so a
    // program that clears and redraws (every full-screen program answering a
    // SIGWINCH) is not shown between the two.
    std::uint32_t eraseDisplayGeneration = 0;

    // OSC 1337 RemoteHost=user@host — remote login identity from shell integration. Empty until set.
    FallibleString remoteHost;

    // OSC 1337 SetUserVar — iTerm2 per-session user variables. Fixed table; `used` flags an occupied slot;
    // an empty value clears the slot.
    static constexpr std::size_t maxAnswerbackLength = 1024;
    static constexpr std::size_t maxFontLength = 256;
    static constexpr std::size_t maxTitleLength = 4096;
    static constexpr std::size_t maxRemoteHostLength = 1024;
    static constexpr std::size_t maxCommandLineLength = 8192;
    static constexpr std::size_t maxCurrentDirectoryLength = 4096;
    static constexpr std::size_t maxCurrentFileURLLength = 4096;
    static constexpr std::size_t maxUserVarNameLength = 128;
    static constexpr std::size_t maxUserVarValueLength = 1024;
    static constexpr int userVariableCapacity = 32;
    /// One iTerm2 OSC 1337 "SetUserVar=" entry, stored inline on the terminal.
    struct UserVariable {
        std::string name;
        std::string value;
        bool used {};
    };
    std::array<UserVariable, userVariableCapacity> userVariables {};

    // OSC 133 ; D[;n] — command completion. lastCommandExitCode = most recent exit status (-1 = no code);
    // commandCompletedGeneration bumps on each D. commandExitCodeRing keeps the last 8 completions' codes
    // (indexed by generation) so a daemon processing two D marks in one batch emits a CommandDone per
    // completion with its own code instead of coalescing.
    int32_t lastCommandExitCode {};
    uint32_t commandCompletedGeneration {};
    std::array<int32_t, 8> commandExitCodeRing {};
    // OSC 133 ; C — a command line began running; bumps on each C. commandStartedAtCompletionRing records,
    // by completion generation like commandExitCodeRing, how many C marks preceded that D, so a daemon
    // draining one batch reports each start and each end in the order the shell wrote them.
    uint32_t commandStartedGeneration {};
    std::array<uint32_t, 8> commandStartedAtCompletionRing {};

    // OSC 133 C→D elapsed time (PAL::monotonicTicks 60Hz), ring-indexed by generation like
    // commandExitCodeRing so the duration matches the completion whose exit code was just read.
    bool hasCommandStartTick {};
    uint32_t commandStartTick {};
    std::array<uint32_t, 8> commandDurationTicksRing {};

    // OSC 133 A→C deferral: absolute row of the most recent prompt start, recorded at A, promoted when
    // the command runs (C). -1 = no prompt awaiting a command.
    int32_t pendingPromptRow {};

    // Whether the shell is reading a command line: OSC 133 B seen and neither C, D nor a new A since.
    // The host's echo context reports it, so a client predicts typing only at a prompt.
    bool isReadingCommand {};
    bool shouldEndSemanticRoleAtLineBreak {};

    /// Identifies an open shell command and retains its state while a child runs.
    /// The top frame uses the terminal's live timing and input state.
    struct ShellCommand {
        std::array<char, 256> applicationID {};
        uint16_t applicationIDLength {};
        int32_t promptRow = -1;
        int32_t pendingPromptRow = -1;
        uint32_t rowEpoch {};
        uint64_t historyEpochBase {};
        uint32_t startTick {};
        bool hasStartTick {};
        bool hasNestedCommand {};
        bool isReading {};
        bool shouldEndRoleAtLineBreak {};
        SemanticRole role = SemanticRole::Output;
    };
    static constexpr size_t shellCommandCapacity = 16;
    std::unique_ptr<std::array<ShellCommand, shellCommandCapacity>> shellCommands;
    uint8_t shellCommandCount {};

    // Whether the shell has marked a prompt (OSC 133 A) in this terminal's life. A shell that marks its
    // prompts ends a program's working and blocked records there; one that has marked none ends them when
    // it takes the foreground back, the only sign it gives.
    bool hasMarkedPrompt {};

    // OSC 633 ; E — VS Code's explicit command line at preexec: the exact post-history-expansion text with
    // wire escapes decoded (unlike the screen-scraped resolveCommandRegion). Generation bumps on each E.
    FallibleString commandLine;
    uint32_t commandLineGeneration {};

    // The directory this pane is in: paneDirectory's answer over the two fields below, resolved
    // by resolvePaneDirectory whenever either moves. Every consumer reads this one — the daemon, the
    // title composers — so none of them arbitrates itself. It is not a confinement root: the Kitty
    // file transport ignores it, because a report is program output and the sample under ssh is the
    // local ssh client's directory.
    FallibleString currentDirectory;

    // What the shell reported (OSC 7 / 633 / iTerm2 CurrentDir), and the foreground process group that
    // held the terminal when it did. The group retires the report once that program exits
    // (hasReportOutlivedItsProgram); zero means nothing has been reported.
    FallibleString reportedDirectory;
    // The authority of the same OSC 7 report — the shell that sent it naming
    // the machine it is on. Empty when the report carried none, and empty for
    // `localhost`, which is a spelling of this machine rather than a name.
    // Kept because it is the only thing on the wire that can see past a
    // nested ssh: the process tree here holds one ssh however many the
    // reader has opened, and the innermost shell is what sends this. It stands
    // only while the program that reported it holds the terminal, and is
    // retired by retireReportedHostIfStale once that program leaves.
    FallibleString reportedHost;
    int reportingProcessGroup {};
    /// The foreground process group that held the terminal when the host was
    /// reported, as `setReportedHostString` stamped it. Zero when no host is
    /// reported, or for one reported before any probe observed the foreground,
    /// which the first probe then adopts.
    int reportedHostProcessGroup {};

    // What the PTY's foreground process is in, sampled on the drain's probe budget. Stands in while
    // nothing is reported, which is every shell with no integration configured.
    FallibleString sampledDirectory;

    // The process group that set the window title, as `setTitleUTF8String` stamped it. Zero for a title
    // set before any probe observed the foreground, which the first probe then adopts.
    int titleOwnerProcessGroup {};

    // The process group holding the terminal, as the platform last sampled it; zero until a probe runs.
    // A reported fact rather than a decision: it is what retires a directory report whose program has
    // exited, and what a title composer keys its own staleness on.
    int foregroundProcessGroup {};

    // OSC 6 — focused document/file URL (file analog of OSC 7). Drives the proxy icon and "reveal in Finder".
    // Stored verbatim as the file:// URL; an empty OSC 6 payload clears it.
    FallibleString currentFileURL;

    // OSC 8 hyperlinks: bounded ring of (id → URI). OSC 8 interns the URI (deduped by id= key then exact
    // URI) and sets currentLinkID, which emitCharacter stamps per cell; empty URI closes the link. Full ⇒ the
    // oldest slot recycles (its id retires; referencing cells resolve empty). Ids 1-based, monotonic, so a
    // recycled slot never collides with a live cell's id within the ring's lifetime window.
    uint16_t linkNextID {}; // next sequential id to mint; 0 = that space is spent (reclaim serves linkFreeIDs)
    int linkCount {}; // live entries, ≤ linkTableCapacity
    int linkHead {}; // ring index of the oldest entry
    // Ids the census reclaimed for re-minting: each one referenced by no cell
    // on either screen, no scrollback line, no ring entry and no open link.
    // Reuse under that census keeps the id invariant — an id never resolves
    // differently for a cell that still exists — while lifting the hard stop
    // at 65535 mints, which left every later link in the session drawing as
    // plain text.
    std::vector<uint16_t> linkFreeIDs;
    // Mint attempts to skip after a census reclaimed nothing, so a stream of
    // unmintable links costs one failed walk per batch rather than one per
    // link.
    int linkReclaimBackoffMints {};
    // 132 KB of per-slot storage. Null until the first intern; linkCount above
    // stays the gate, so a zero count needs no dereference.
    std::unique_ptr<Attributes::Hyperlinks::LinkTable> linkTable;

    // Grapheme-cluster overflow pool: full codePoint sequences for clusters that don't fit a Cell's inline
    // buffer. An overflowing cell interns its whole sequence and stamps the id via cell.setClusterID. Grows
    // to hold every distinct cluster on screen + scrollback so a live id is never evicted (eviction would
    // collapse the cell to its inline base — scrollback/reflow corruption).
    Ubiquitty::ClusterPool clusterPool;

    // OSC 66 proportional-run text: one entry a run, bounded by the OSC
    // payload rather than by the cells the run claims. The run's anchor cell
    // carries the entry id in its cluster field, and the anchor's proportional
    // spec is what tells that id from a cluster-pool id (isProportionalRunAnchor).
    // Entries leave only through retainOnly, after a census of both screens
    // and every scrollback line.
    Ubiquitty::RunTextStore runTextStore;
    // Placements until an exhausted store permits another reference census.
    int runTextReclaimBackoffInterns {};

    // OSC 7404 rich text blocks (Docs/rich-text.md): where each block is and
    // its document, per screen. The grid holds the covered cells; a block
    // whose cells something changed is removed by RichText::reconcileBlocks
    // after the parse pass that changed them.
    Ubiquitty::RichText::BlockStore richTextBlocks;
    // The typography table this terminal's blocks are measured and laid out
    // by, as its frontend — or, on a host, the client viewing the pane — last
    // published it; and each block's layout under it, derived on each side
    // and never carried.
    Ubiquitty::RichText::PublishedTypography typography;
    Ubiquitty::FallibleVector<Ubiquitty::RichText::PendingMeasurement> richTextMeasurements;
    std::uint32_t richTextMeasurementToken = 0;
    mutable Ubiquitty::RichText::LayoutCache richTextLayouts;

    // OSC 66 text-size specs: cells carry a 4-bit id (ExtendedAttributes'
    // TextSizeSpecField), this resolves it. Reset rides the Terminal
    // reconstruct like every table here; reclamation is a census of both
    // screens, the status row and the scrollback through retainOnly, run by
    // the clears and by a mint that finds the table full.
    Ubiquitty::TextSizeTable textSizeTable;
    // Refused interns to skip after a census freed nothing, so a stream of
    // distinct specs over a table whose every id is still named costs one walk
    // per batch rather than one per run.
    int textSizeReclaimBackoffInterns {};

    // Scrollback cells live in a content-trimmed LineStore; these mirror its state for external readers
    // and the absolute-row accessors. LineStore presents oldest-first, so no ring start is tracked here.
    std::unique_ptr<LineStore> lineStore;
    // The history address space (Core/Screen/HistoryLines.h). The epoch counts
    // destructive clears and starts at one; only Screen::retireHistory and RIS
    // advance it. The base is the epoch's first line number, which the cleared
    // lines are all below, and the number a lazily created store starts at.
    std::uint64_t historyEpochBase {};
    std::uint32_t historyEpoch { 1 };
    // The names positions give lines (Core/Screen/LineIdentity.h): both
    // buffers' pins and the history's name segments. Null until a name is
    // handed out, since until then no name has to stay true; touched only by
    // the cold paths that move, erase or rewrap rows. Mutable: naming a line
    // records where it is and changes nothing the terminal shows, so a reader
    // holding the terminal as const, a selection resolving its ends, may name
    // one.
    mutable std::unique_ptr<Ubiquitty::Screen::LineIdentity> lineIdentity;
    // Triggers engine. Null until the first trigger rule / characterClass is configured; freed in dispose.
    // Null or zero-rule engine makes the per-line evaluateLine hook a no-op, so the byte-hot path is
    // unpaid when no triggers are configured (the default).
    std::unique_ptr<Triggers::Engine> triggersEngine;
    // Highlight spans matched by a highlight rule: a ring of {absolute row, inclusive byte columns} the
    // renderer maps into the viewport and tints. Only a configured `highlight` trigger writes here.
    static constexpr int triggerHighlightCapacity = 64;
    struct TriggerHighlight {
        int absoluteRow {}; // scrollbackCount + screenRow at commit
        int column0 {}; // first matched byte column (inclusive)
        int column1 {}; // last matched byte column (inclusive)
    };
    std::array<TriggerHighlight, triggerHighlightCapacity> triggerHighlights {};
    int triggerHighlightCount {}; // 0..triggerHighlightCapacity; oldest-evicted ring
    int triggerHighlightHead {}; // ring index of the oldest entry
    // triggerWrapEvaluatedRow lives in the hot parse-loop cluster up top.
    int scrollbackCapacity {};
    int scrollbackCount {}; // == lineStore->count()
    // Deferred-state scroll lane: during an eligible flood the per-scroll dirty/pendingScroll/image
    // bookkeeping accumulates here and commits once per processBytes (Screen::scrollLaneFlush) instead of per
    // scrolled line. Never carries state across processBytes calls. Off by default.
    struct ScrollLaneRun {
        bool active {};
        int scrolledLines {};
        // Run's BCE blank, built once at open: the pen cannot change inside the lane (an SGR is a
        // dispatch, and every dispatch flushes the run).
        Cell blank;
    };
    ScrollLaneRun scrollLaneRun;
    bool isScrollLaneEnabled {};
    // Deferred band scrolls: the margined sibling of the lane above. A DECSLRM
    // band cannot rotate the ring, so an eligible at-bottom band flood defers
    // the MOVE itself: each linefeed swaps the band-bottom row's storage for a
    // spare pool row (the write path lands on the pool row untouched), and the
    // flush performs one rectangle move of the accumulated count plus one
    // band-column copy per deferred line -- height + k row operations in place
    // of height * k. Same lifetime rules: never crosses a processBytes return,
    // and every dispatch flush covers both lanes.
    struct BandScrollRun {
        bool active {};
        int scrolledLines {};
        int top {};
        int bot {};
        int left {};
        int right {};
        Cell blank;
    };
    BandScrollRun bandScrollRun;
    bool isBandScrollLaneEnabled {};
    // Configured history depth: the lineStore's capacity, and the bound the reflow ring + image/cursor
    // floors honor. VT::fallbackScrollbackLines until a frontend overrides it per
    // session (setScrollbackLimit) — every configured session gets Values's larger default instead.
    int scrollbackLimit = Ubiquitty::VT::fallbackScrollbackLines;
    // Non-zero while an Interactive resize deferred the O(scrollback) history rewrap: the target column
    // count to rewrap to when the drag settles. Never survives into a serialized snapshot.
    int pendingHistoryRewrapColumns {};
    // The deferred rewrap retains the viewport intent of its last geometry change.
    Ubiquitty::Modes::AnchorMode pendingHistoryRewrapAnchor { Ubiquitty::Modes::AnchorMode::Top };

    struct Integration {
        // Control-mode filtering and pane routing are one lifetime domain: a cached child is valid only
        // while the router retains its delivery pin.
        Protocol::Tmux::TmuxClient tmux;
        std::array<uint8_t, 32> traceBytes {};
        int traceLength {};
        PaneRouter paneRouter;
        int cachedPaneID {};
        Terminal* cachedPaneTarget { nullptr };
    } integration;

    // Memo in front of paneLookup: the active-pane route resolved once per delivery run instead of per
    // %output byte (paneLookup is an os_unfair_lock + NSMapTable + objc_msgSend round-trip). Only a
    // resolved, distinct child is cached; NULL/self (pane pending spawn) is retried next run.
    // LIFETIME CONTRACT: the cached Terminal is safe only while the frontend holds the pane's deliver
    // pin, so the memo dies at every release — end-of-pass flush, any route change, and any mid-pass
    // frontend release (which calls invalidateTmuxPaneRoute). pin-release ⇒ memo-dead for every path.

    // Owns the cell model and renderer shadow. Row metadata views borrow it and are rebound after
    // construction and reset.
    std::unique_ptr<CellGrid> grid;

    // Generation counter for async-decode lifecycle safety. ~Terminal bumps it; in-flight async-decode
    // jobs snapshot it at submit and the completion compares before touching the Terminal — a mismatch
    // means it was disposed/re-init'd, so drop the result. Single-threaded; mutated only by the main task.
    // Bumped whenever an operation replaces the buffer's absolute-row
    // coordinate space rather than shifting a remappable prefix: a resize
    // repacks rows, and a buffer clear destroys or relocates live content.
    // Anything holding state keyed on an absolute row (a selection, a
    // highlight a frontend cached) compares the epoch it recorded against this
    // one, because the rows it named are not the rows that number now.
    //
    // Core cannot repair that state: the selection lives in the frontends and
    // Core has no pointer to it. What Core can do is say the coordinate space
    // moved, in one place, rather than four frontends each guessing from a
    // resize callback.
    //
    // Starts at one, because zero is the value a selection carries when
    // nothing stamped it. A terminal whose epoch could be zero would make a
    // selection made in its first buffer indistinguishable from one made in no
    // buffer at all, and the drop leaves the second alone.
    uint32_t absoluteRowEpoch { 1 };

    // Last authoritative absolute-row epoch applied through the Sync protocol.
    // A passive terminal keeps this separate from its local epoch: ordinary
    // keyframes and deltas preserve coordinates while the source identity is
    // stable, while a changed source epoch invalidates them.
    uint32_t sourceAbsoluteRowEpoch {};
    bool hasSourceAbsoluteRowEpoch = false;

    uint32_t generation {};

    // Owned per-terminal render projection of logical search hits and their focus.
    // Null until a search publishes; freed by ~Terminal or Screen::ScrollSearch::clear. Per-Terminal (not a
    // process-global) so independent Terms keep concurrent results without clobbering.
    std::unique_ptr<Screen::ScrollSearch::ResultProjection> scrollSearchState;

    // Async-decode queues, one set per Terminal. A completion writes deep into
    // the Terminal that submitted it — cursor, image slots, scroll region — and
    // frees the pixel buffer an evicted image held, so it may only run on the
    // thread that owns that Terminal. While these queues were process-global,
    // `drain` popped every Terminal's jobs, so whichever pane happened to parse
    // next ran another pane's completion holding only its own model lock.
    // Each stays null until that Terminal's first decode: most sessions print no
    // image and allocate none of the three.
    struct AsyncDecodeQueues {
        std::unique_ptr<TaskQueue> sixel;
        std::unique_ptr<TaskQueue> kittyDecode;
        std::unique_ptr<TaskQueue> kittyInflate;
    } asyncDecodes;

    // isTekModeActive lives in the hot parse-loop cluster up top.

    // Tek 4014 bright-pen runtime override (DECSET 7400, Ubiquitty-private): 0 = config default, 1 = forced
    // on, 2 = forced off.
    uint8_t tekBrightPenOverride {};

    // Tek 4014 storage-tube + parser state. tekInit applies the non-zero power-on defaults after the
    // generic Terminal fields are established.
    Ubiquitty::Tek::State tek {};

    // Batched-run translations, one per designated set, prepared from the same
    // resolveGLByte the per-character path uses so the two can never disagree
    // about a byte. Held per SET rather than for the current one: a stream that
    // alternates designations would otherwise rebuild on every switch, which
    // costs more than the batch returns.
    //
    // The whole set of them is keyed by the ambiguous-width choice, because an
    // East Asian Ambiguous mapping stands in one cell under one choice and two
    // under the other, and the batched writer advances exactly one column per
    // byte. Lives at the struct TAIL for the reason the block above gives.
    std::array<std::array<std::uint16_t, Ubiquitty::runTranslationByteCount>, Ubiquitty::runTranslationSetCount> glRunTranslations {};
    std::array<std::uint8_t, Ubiquitty::runTranslationSetCount> glRunTranslationState {}; // 0 undecided, 1 usable, 2 refused
    int glRunTranslationAmbiguousWidth = 0;
} UBIQUITTY_UNSAFE_REFERENCE;

// Non-polymorphic state owner: independent-lifetime resources via RAII; raw pointers are non-owning seams.
static_assert(!std::is_polymorphic_v<Ubiquitty::Terminal>);

inline bool isUTF8ContinuationByte(char byte) { return (static_cast<unsigned char>(byte) & 0xC0) == 0x80; }

/// The number of bytes in the UTF-8 sequence `byte` leads.
///
/// - Parameter byte: The candidate lead byte.
/// - Returns: 1 to 4 for a lead byte, or 0 when the byte leads nothing — a
///   continuation, or one of the values no lead may take.
inline int utf8SequenceLength(char byte)
{
    const unsigned char value = static_cast<unsigned char>(byte);
    if ((value & 0x80) == 0)
        return 1;
    if ((value & 0xE0) == 0xC0)
        return 2;
    if ((value & 0xF0) == 0xE0)
        return 3;
    if ((value & 0xF8) == 0xF0)
        return 4;
    return 0;
}

/// Returns `source` cut to at most `maxBytes`, never through a character.
///
/// A view rather than a copy, so a setter can assign into storage it already
/// owns instead of adopting a fresh allocation and freeing the old one on every
/// call; ``boundedUTF8String`` is the owning form. The view borrows `source`.
///
/// Two conditions have to hold at the cut, and only the first is about the byte
/// the bound lands on. A continuation there means the cut falls inside a
/// sequence, and the walk moves back to that sequence's start. Anything else
/// there means only that the cut falls between sequences, which leaves open
/// whether the sequence before it is whole — a lead whose continuations were
/// never sent ends the copy mid-character, the state this function exists to
/// prevent. So the retained tail is walked back to its lead as well, and the
/// sequence dropped when the bound cut it short.
///
/// It is not a validator: a run of continuation bytes with no lead, or an
/// over-long encoding, is returned as given when it fits. The invariant is
/// narrower — the cut this function makes never separates a lead from its
/// continuations.
///
/// - Parameters:
///   - source: The text to bound. Arrives from the wire, so every byte is the
///     sender's choice and no encoding may be assumed.
///   - maxBytes: The most bytes the result may hold.
/// - Returns: A prefix of `source` no longer than `maxBytes`, ending on a
///   character boundary.
inline std::string_view boundedUTF8View(std::string_view source, std::size_t maxBytes)
{
    if (source.size() <= maxBytes)
        return source;
    std::size_t length = maxBytes;
    while (length > 0 && length < source.size() && isUTF8ContinuationByte(source[length]))
        --length;
    // At most three continuations, because four bytes is the longest sequence
    // and a longer run of them has no lead to be incomplete against.
    std::size_t lead = length;
    while (lead > 0 && isUTF8ContinuationByte(source[lead - 1]) && length - lead < 3)
        --lead;
    if (lead > 0) {
        const int sequenceLength = utf8SequenceLength(source[lead - 1]);
        if (sequenceLength > 0 && lead - 1 + static_cast<std::size_t>(sequenceLength) > length)
            length = lead - 1;
    }
    return source.substr(0, length);
}

/// An owned copy of `source` bounded to `maxBytes`, ending on a character
/// boundary.
///
/// For callers that need storage outliving the source bytes; a setter writing
/// into a string it already owns wants ``boundedUTF8View`` instead.
///
/// - Parameters:
///   - source: The bytes to bound.
///   - maxBytes: The ceiling the result may not exceed.
/// - Returns: The bounded prefix, copied.
inline std::string boundedUTF8String(std::string_view source, std::size_t maxBytes)
{
    return std::string(boundedUTF8View(source, maxBytes));
}

/// Returns `label` with every control byte dropped, for reflection back to the host.
///
/// A reply reaches the program on the other side of the pseudoterminal as though
/// it were typed, so a control byte carried out of a label that same program set
/// becomes input it acts on: a newline inside a font family name submits whatever
/// follows as a command line. Every site that reflects stored text into a reply
/// passes it through here. The setters keep the bytes they were given, because
/// what a human sees is not the injection channel — the reply is.
///
/// - Parameter label: The stored text about to be reflected.
/// - Returns: The label without C0 controls, DEL, or the UTF-8 form of C1.
inline std::string sanitizedReplyLabel(std::string_view label)
{
    std::string out;
    out.reserve(label.size());
    for (std::size_t index = 0; index < label.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(label[index]);
        if (byte < 0x20 || byte == 0x7F)
            continue;
        // C1 reaches a UTF-8 reply as 0xC2 followed by 0x80..0x9F; dropping the
        // pair together keeps the survivors well-formed, which a bare test for
        // 0x80..0x9F would not — those bytes are also UTF-8 continuations.
        if (byte == 0xC2 && index + 1 < label.size()) {
            const unsigned char trail = static_cast<unsigned char>(label[index + 1]);
            if (trail >= 0x80 && trail <= 0x9F) {
                ++index;
                continue;
            }
        }
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

/// Sets the ENQ answerback, bounded to the answerback ceiling.
///
/// - Parameters:
///   - term: The terminal to write.
///   - value: The bytes to store; may not alias the terminal's own answerback.
inline void setAnswerbackString(Ubiquitty::Terminal& term, std::string_view value)
{
    term.answerback.assign(boundedUTF8View(value, Ubiquitty::Terminal::maxAnswerbackLength));
    term.isAnswerbackConcealed = false;
}

/// The answerback text a settings or inspector view may display.
/// - Parameter term: The authoritative terminal, held under its model lock.
/// - Returns: The message or the DEC concealment label; valid until the message changes.
[[nodiscard]] inline std::string_view answerbackForDisplay(const Ubiquitty::Terminal& term)
{
    return term.isAnswerbackConcealed ? std::string_view("concealed") : term.answerback.view();
}

// A Terminal is per pane and per host session, so its size is per-session
// memory. The ceiling makes re-inlining bulk storage a compile error; the
// headers named below carry the reasoning for their own share.
static_assert(sizeof(Ubiquitty::Terminal) <= 176 * 1024,
    "sizeof(Terminal) crossed its ceiling — see Core/Attributes/Hyperlinks.h, "
    "Core/Rendering/StatusRow.h, Core/VT/SoftFont.h and Core/VT/UserDefinedKeys.h "
    "for why bulk storage is held by pointer");

/// DECATC's table: the one held, or the unassigned table while no DECATC has assigned a combination.
[[nodiscard]] inline const Ubiquitty::AlternateTextColors& alternateTextColorTable(const Ubiquitty::Terminal& term)
{
    return term.alternateTextColors ? *term.alternateTextColors : Ubiquitty::unassignedAlternateTextColors;
}

/// The DECDLD store, allocating it on first use. Only the DECDLD target needs this.
[[nodiscard]] inline Ubiquitty::VT::SoftFont& ensureSoftFont(Ubiquitty::Terminal& term)
{
    if (!term.softFont) [[unlikely]]
        term.softFont = std::make_unique<Ubiquitty::VT::SoftFont>();
    return *term.softFont;
}

/// The DECDLD store, or nullptr when nothing was downloaded.
[[nodiscard]] inline const Ubiquitty::VT::SoftFont* softFontOrNull(const Ubiquitty::Terminal& term) { return term.softFont.get(); }

/// Whether a designated soft font is usable. Absent and invalid answer alike.
[[nodiscard]] inline bool hasValidSoftFont(const Ubiquitty::Terminal& term) { return term.softFont && term.softFont->valid; }

/// The sample bank, allocating it on first use. Only a download needs this.
[[nodiscard]] inline Ubiquitty::VT::SampleBank& ensureSampleBank(Ubiquitty::Terminal& term)
{
    if (!term.sampleBank) [[unlikely]]
        term.sampleBank = std::make_unique<Ubiquitty::VT::SampleBank>();
    return *term.sampleBank;
}

/// The pending sound a host forwards, allocating it on first use.
[[nodiscard]] inline Ubiquitty::VT::PendingSound& ensurePendingSound(Ubiquitty::Terminal& term)
{
    if (!term.pendingSound) [[unlikely]]
        term.pendingSound = std::make_unique<Ubiquitty::VT::PendingSound>();
    return *term.pendingSound;
}

/// The DECUDK table, allocating it on first use.
[[nodiscard]] inline Ubiquitty::VT::UserDefinedKeys& ensureUserDefinedKeys(Ubiquitty::Terminal& term)
{
    if (!term.udk) [[unlikely]]
        term.udk = std::make_unique<Ubiquitty::VT::UserDefinedKeys>();
    return *term.udk;
}

/// The DECUDK table, or nullptr when no key was programmed.
[[nodiscard]] inline const Ubiquitty::VT::UserDefinedKeys* userDefinedKeysOrNull(const Ubiquitty::Terminal& term) { return term.udk.get(); }

inline void setFontString(Ubiquitty::Terminal& term, std::string_view value) { term.font.assign(boundedUTF8View(value, Ubiquitty::Terminal::maxFontLength)); }

/// Records an authorized font request for a frontend that applies it off the parser thread.
///
/// The caller holds the model lock. A refused request changes neither the
/// queue, its generation, nor its drain flag.
///
/// - Parameters:
///   - term: The terminal receiving the request.
///   - family: The requested family, or empty to preserve or reset the live family.
///   - sizePoints: The absolute point size, or zero to preserve the live size.
///   - sizeDelta: The change relative to the live point size, or zero.
/// - Returns: Whether the request was admitted within both request bounds.
[[nodiscard]] inline bool recordFontRequest(Ubiquitty::Terminal& term, std::string_view family, double sizePoints, double sizeDelta)
{
    if (family.size() > Terminal::maxFontLength || !term.fontRequests.append(family, sizePoints, sizeDelta)) {
        return false;
    }
    term.fontRequestChanged = true;
    return true;
}

/// Records mintty's return to the configured font size, the family kept, for
/// the frontend's next delivery.
///
/// The caller holds the model lock. A refused request changes neither the
/// queue, its generation, nor its drain flag.
///
/// - Parameter term: The terminal receiving the request.
/// - Returns: Whether the request was admitted within the request bound.
[[nodiscard]] inline bool recordFontSizeReset(Ubiquitty::Terminal& term)
{
    if (!term.fontRequests.appendConfiguredSize())
        return false;
    term.fontRequestChanged = true;
    return true;
}

/// Sets the window title, bounded to the title ceiling, and leaves it waiting
/// for the program that set it to be named.
///
/// Which program that was is read after the bytes, never before: the last
/// probe can predate them. A shell that titles its pane the moment a command
/// exits is parsed while that probe still names the command, and a title
/// stamped with the command's group was retired by the next probe, which found
/// the shell that set it. The probe that follows the bytes adopts the title
/// instead (`retireTitleIfStale`).
///
/// The reset rides the setter rather than each caller because there are
/// several — the OSC dispatch, the title-stack pop, a host's snapshot — and a
/// title set through the one that forgot would keep the previous one's owner.
///
/// - Parameters:
///   - term: The terminal to write.
///   - value: The title, already filtered; bytes past the ceiling are dropped
///     at a code-point boundary. May not alias the terminal's own title, which
///     is overwritten while it is read.
inline void setTitleUTF8String(Ubiquitty::Terminal& term, std::string_view value)
{
    term.titleOwnerProcessGroup = 0;
    term.titleUTF8.assign(boundedUTF8View(value, Ubiquitty::Terminal::maxTitleLength));
}

/// Sets the icon name, bounded to the title ceiling.
///
/// - Parameters:
///   - term: The terminal to write.
///   - value: The bytes to store; may not alias the terminal's own icon name.
inline void setIconNameUTF8String(Ubiquitty::Terminal& term, std::string_view value) { term.iconNameUTF8.assign(boundedUTF8View(value, Ubiquitty::Terminal::maxTitleLength)); }

inline void setRemoteHostString(Ubiquitty::Terminal& term, std::string_view value) { term.remoteHost = boundedUTF8String(value, Ubiquitty::Terminal::maxRemoteHostLength); }

inline void setCommandLineString(Ubiquitty::Terminal& term, std::string_view value) { term.commandLine = boundedUTF8String(value, Ubiquitty::Terminal::maxCommandLineLength); }

/// The two directories a pane can answer with.
struct PaneDirectoryFacts {
    /// What the shell reported, empty when it has not or its report went stale.
    std::string_view reportedDirectory;
    /// What the PTY's foreground process is in, empty when unsampled.
    std::string_view sampledDirectory;
};

/// The directory a pane is in.
///
/// Two sources answer and they disagree. A shell reports where it is with
/// OSC 7 (or 633, or iTerm2's CurrentDir) at each prompt; the PTY's foreground
/// process can be sampled for the directory that process is in, which for an
/// ssh or a tmux client is the client's own directory and not the session's.
/// So the report wins whenever there is one, and the sample stands in only
/// while nothing was reported, which is what makes a shell with no integration
/// configured still carry a directory in its title.
///
/// Arbitrating between them was written once per frontend, each spelling a
/// different ladder, so a pane's directory depended on which shell drew it.
///
/// - Parameter facts: What the pane's two sources say.
/// - Returns: The directory, or empty when neither source has an answer.
[[nodiscard]] constexpr std::string_view paneDirectory(const PaneDirectoryFacts& facts)
{
    return !facts.reportedDirectory.empty() ? facts.reportedDirectory : facts.sampledDirectory;
}

/// Whether something a program reported still describes what is running now.
///
/// A report belongs to the program that made it, whether it is a directory or
/// a window title. An ssh session reports a remote directory and exits;
/// without this the window keeps naming a directory on another machine until
/// something else reports one. An editor sets a title and quits, and the title
/// sticks to a window running something else. When the foreground moves, the
/// report is retired and whatever stands behind it answers again.
///
/// Neither group being zero is the unknown case, and an unknown foreground
/// retires nothing: a probe that has not run yet must not drop a report the
/// shell just made.
///
/// - Parameters:
///   - reportingProcessGroup: The foreground group when the report landed.
///   - foregroundProcessGroup: The group holding the terminal now.
/// - Returns: Whether the report has outlived the program that made it.
[[nodiscard]] constexpr bool hasReportOutlivedItsProgram(int reportingProcessGroup, int foregroundProcessGroup)
{
    return reportingProcessGroup != 0 && foregroundProcessGroup != 0 && reportingProcessGroup != foregroundProcessGroup;
}

/// Re-resolves ``Terminal::currentDirectory`` from the two sources beside it.
///
/// Called by whichever of them just moved, so the resolved field is never a
/// race between two writers: it was one string both a parser and a process
/// probe assigned, and whichever ran last won.
///
/// - Parameter term: The terminal to resolve.
/// - Returns: Whether the resolved directory changed.
inline bool resolvePaneDirectory(Ubiquitty::Terminal& term)
{
    const std::string_view resolved = paneDirectory(
        { .reportedDirectory = term.reportedDirectory, .sampledDirectory = term.sampledDirectory });
    if (term.currentDirectory == resolved)
        return false;
    term.currentDirectory = boundedUTF8String(resolved, Ubiquitty::Terminal::maxCurrentDirectoryLength);
    return true;
}

/// Records what the shell reported, unstamped, and re-resolves.
///
/// The report is left for the probe after its bytes to stamp, as a title is
/// (`setTitleUTF8String`): the last probe can predate the bytes. A remote
/// shell's first prompt is parsed while that probe still names the local shell
/// that ran ssh, and a report stamped with it was retired by the next probe as
/// soon as it found ssh holding the terminal.
///
/// - Parameters:
///   - term: The terminal to write.
///   - value: The reported path, already filtered of control bytes.
/// - Returns: Whether the resolved directory changed.
inline bool setReportedDirectoryString(Ubiquitty::Terminal& term, std::string_view value)
{
    term.reportedDirectory = boundedUTF8String(value, Ubiquitty::Terminal::maxCurrentDirectoryLength);
    term.reportingProcessGroup = 0;
    return resolvePaneDirectory(term);
}

/// Records the host the same report named, if it named one, unstamped.
///
/// Separate from the path because the two have different readers: the path
/// resolves a directory, the host answers which machine a session is on.
///
/// The stamp is cleared by the setter rather than the caller, as a title's is,
/// and the probe after the bytes adopts the host for whoever then holds the
/// terminal (`retireReportedHostIfStale`) — never the last probe's reading,
/// which can name the local shell a remote prompt arrived through. It is
/// cleared before the comparison, so a program repeating a host an earlier
/// one reported comes to own it. A caller gates the report before calling: a
/// refused host must neither be written nor re-stamped.
///
/// - Parameters:
///   - term: The terminal to write.
///   - value: The URL authority, already filtered of control bytes.
/// - Returns: Whether the recorded host changed.
inline bool setReportedHostString(Ubiquitty::Terminal& term, std::string_view value)
{
    std::string next = boundedUTF8String(value, Ubiquitty::Terminal::maxRemoteHostLength);
    term.reportedHostProcessGroup = 0;
    if (term.reportedHost == next)
        return false;
    term.reportedHost = std::move(next);
    return true;
}

/// Records what the foreground process was sampled in, and re-resolves.
///
/// - Parameters:
///   - term: The terminal to write.
///   - value: The sampled path.
/// - Returns: Whether the resolved directory changed.
inline bool setSampledDirectoryString(Ubiquitty::Terminal& term, std::string_view value)
{
    term.sampledDirectory = boundedUTF8String(value, Ubiquitty::Terminal::maxCurrentDirectoryLength);
    return resolvePaneDirectory(term);
}

/// Retires a window title whose program has exited.
///
/// An editor sets a title and quits; without this the title sticks to a window
/// running something else, and no later prompt clears it because a shell that
/// sets no title writes nothing. Dropping it lets the composed fallback — the
/// directory and the foreground process — answer again.
///
/// Every title arrives unstamped and is adopted exactly as an unstamped
/// directory report is: the first program observed holding the terminal after
/// the bytes that set it is the one that set it.
///
/// - Parameters:
///   - term: The terminal to write.
///   - foregroundProcessGroup: The group holding the terminal now.
/// - Returns: Whether the title was dropped.
inline bool retireTitleIfStale(Ubiquitty::Terminal& term, int foregroundProcessGroup)
{
    if (term.titleUTF8.empty()) {
        term.titleOwnerProcessGroup = 0;
        return false;
    }
    if (term.titleOwnerProcessGroup == 0) {
        term.titleOwnerProcessGroup = foregroundProcessGroup;
        return false;
    }
    if (!hasReportOutlivedItsProgram(term.titleOwnerProcessGroup, foregroundProcessGroup))
        return false;
    term.titleUTF8.clear();
    term.titleOwnerProcessGroup = 0;
    return true;
}

/// Retires a report whose program has exited, and re-resolves.
///
/// Every report arrives unstamped, since the last probe can predate its bytes,
/// and a report with no reporter can never be found to have outlived one. So
/// the first program observed holding the terminal after the bytes ADOPTS it:
/// it is the one that made it. Without the adoption the very first directory a
/// shell reports outlives every program after it.
///
/// - Parameters:
///   - term: The terminal to write.
///   - foregroundProcessGroup: The group holding the terminal now.
/// - Returns: Whether the resolved directory changed.
inline bool retireReportedDirectoryIfStale(Ubiquitty::Terminal& term, int foregroundProcessGroup)
{
    if (term.reportedDirectory.empty())
        return false;
    if (term.reportingProcessGroup == 0) {
        term.reportingProcessGroup = foregroundProcessGroup;
        return false;
    }
    if (!hasReportOutlivedItsProgram(term.reportingProcessGroup, foregroundProcessGroup))
        return false;
    term.reportedDirectory.clear();
    term.reportingProcessGroup = 0;
    return resolvePaneDirectory(term);
}

/// Retires a reported host whose program has left the foreground.
///
/// A host names where the shell that reported it is, and says nothing once
/// that shell no longer holds the terminal. It outranks the walk when a
/// session's location is resolved, so a host left standing after an ssh to a
/// machine with no integration names the machine the reader left, and one left
/// by a remote shell that exited names a machine the session is no longer on.
///
/// Every host arrives unstamped and is adopted exactly as a directory report
/// is: the first program observed holding the terminal after the bytes that
/// reported it is the one that reported it.
///
/// - Parameters:
///   - term: The terminal to write.
///   - foregroundProcessGroup: The group holding the terminal now.
/// - Returns: Whether the reported host was dropped.
inline bool retireReportedHostIfStale(Ubiquitty::Terminal& term, int foregroundProcessGroup)
{
    if (term.reportedHost.empty()) {
        term.reportedHostProcessGroup = 0;
        return false;
    }
    if (term.reportedHostProcessGroup == 0) {
        term.reportedHostProcessGroup = foregroundProcessGroup;
        return false;
    }
    if (!hasReportOutlivedItsProgram(term.reportedHostProcessGroup, foregroundProcessGroup))
        return false;
    term.reportedHost.clear();
    term.reportedHostProcessGroup = 0;
    return true;
}

/// Whether this drain reported something that still waits for its program.
///
/// Read off the drain's own change latches, so a drain that reported nothing
/// costs no foreground read: a report parsed on it has no owner until the
/// probe after its bytes names one.
///
/// - Parameter term: The terminal the drain wrote.
/// - Returns: Whether a title, directory or host set on this drain is unstamped.
[[nodiscard]] inline bool didDrainLeaveAReportUnstamped(const Ubiquitty::Terminal& term)
{
    const bool isTitleUnstamped = term.titleChanged && !term.titleUTF8.empty() && term.titleOwnerProcessGroup == 0;
    const bool isDirectoryUnstamped = term.currentDirectoryChanged && !term.reportedDirectory.empty() && term.reportingProcessGroup == 0;
    const bool isHostUnstamped = term.currentDirectoryChanged && !term.reportedHost.empty() && term.reportedHostProcessGroup == 0;
    return isTitleUnstamped || isDirectoryUnstamped || isHostUnstamped;
}

/// Names the program holding the terminal as the owner of every unstamped
/// report, retiring none: a stamped report is judged by the full probe.
///
/// - Parameters:
///   - term: The terminal to write.
///   - foregroundProcessGroup: The group holding the terminal now; zero, an
///     unknown foreground, leaves every report unstamped.
inline void stampUnstampedReports(Ubiquitty::Terminal& term, int foregroundProcessGroup)
{
    if (!term.titleUTF8.empty() && term.titleOwnerProcessGroup == 0)
        term.titleOwnerProcessGroup = foregroundProcessGroup;
    if (!term.reportedDirectory.empty() && term.reportingProcessGroup == 0)
        term.reportingProcessGroup = foregroundProcessGroup;
    if (!term.reportedHost.empty() && term.reportedHostProcessGroup == 0)
        term.reportedHostProcessGroup = foregroundProcessGroup;
}

inline void setCurrentFileURLString(Ubiquitty::Terminal& term, std::string_view value) { term.currentFileURL = boundedUTF8String(value, Ubiquitty::Terminal::maxCurrentFileURLLength); }

// The gated-client accessors declared in EffectGate.h. They live here, below
// Terminal, because they read its policy and origin and so need the complete
// type — while Terminal holds GatedClient members and so needs that class
// first. The declarations carry the documentation; these are the bodies.
/// Returns the client only when `capability` is allowed for the current origin.
///
/// - Parameters:
///   - terminal: The terminal whose policy and origin decide.
///   - gated: The client to unlock.
///   - capability: What the caller is about to do.
///   - detail: The request payload a policy may key on, empty when there is none.
/// - Returns: The client, or null when the effect must not happen.
template<typename Client>
Client* effectClient(Terminal& terminal, GatedClient<Client>& gated, Capability capability, std::string_view detail)
{
    Client* client = gated.m_client.get();
    if (!client)
        return nullptr;
    // An unset policy is answered by effectAllowed from PAL's defaults, so a
    // client wired onto a terminal with no policy still refuses an Agent or
    // Remote host escape; the capabilityPolicy field above documents it.
    return effectAllowed(terminal, capability, detail) ? client : nullptr;
}

/// Returns the client without consulting the policy, for an effect that escapes
/// nothing.
///
/// - Parameters:
///   - gated: The client to read.
/// - Returns: The client, or null when none is installed.
template<typename Client>
Client* clientForEffectThatEscapesNothing(GatedClient<Client>& gated)
{
    return gated.m_client.get();
}

/// Reports whether an effect that reaches no client may happen.
///
/// With no policy installed the answer is PAL's `defaultDecision` for the
/// current origin, and only its Allow passes.
///
/// - Parameters:
///   - terminal: The terminal whose policy and origin decide.
///   - capability: What the caller is about to do.
///   - detail: The request payload a policy may key on, empty when there is none.
/// - Returns: Whether the effect may happen.
inline bool effectAllowed(Terminal& terminal, Capability capability, std::string_view detail)
{
    auto* policy = terminal.capabilityPolicy.m_policy.get();
    if (!policy)
        return defaultDecision(capability, terminal.currentCapabilityOrigin) == CapabilityDecision::Allow;
    return policy->check(capability, terminal.currentCapabilityOrigin, detail) == CapabilityDecision::Allow;
}

/// Reports whether an effect may happen, refusing when no policy is installed.
///
/// See `EffectGate.h` for why this is a second function rather than an argument
/// to the first: which way an unwired terminal falls belongs to the path.
///
/// - Parameters:
///   - terminal: The terminal whose policy and origin decide.
///   - capability: What the caller is about to do.
///   - detail: The request payload a policy may key on, empty when there is none.
/// - Returns: Whether a policy is installed and allows the effect.
inline bool effectGranted(Terminal& terminal, Capability capability, std::string_view detail)
{
    auto* policy = terminal.capabilityPolicy.m_policy.get();
    if (!policy)
        return false;
    return policy->check(capability, terminal.currentCapabilityOrigin, detail) == CapabilityDecision::Allow;
}

/// Delivers an allowed OSC 52 write or retains its exact payload for consent.
///
/// - Parameters:
///   - terminal: The terminal whose clipboard client and policy decide delivery.
///   - buffers: The clipboard destinations requested by the program.
///   - bytes: The decoded payload delivered only after approval.
inline void requestClipboardWrite(Terminal& terminal, std::span<const ClipboardBuffer> buffers, std::span<const char> bytes)
{
    if (!terminal.isOSC52Allowed || terminal.osc52ClipboardWritePolicy == OSC::ClipboardWritePolicy::Deny || !terminal.clipboardClient)
        return;
    auto* policy = terminal.capabilityPolicy.m_policy.get();
    const CapabilityDecision decision = policy
        ? policy->check(Capability::ClipboardWrite, terminal.currentCapabilityOrigin, {})
        : defaultDecision(Capability::ClipboardWrite, terminal.currentCapabilityOrigin);
    // PAL's default prompts a Remote or Agent write, and with no policy there is
    // nothing to hold the payload for consent, so that Prompt is a refusal.
    if (decision == CapabilityDecision::Prompt && policy) {
        policy->holdClipboardWrite(terminal.currentCapabilityOrigin, buffers, bytes);
        return;
    }
    if (decision != CapabilityDecision::Allow)
        return;
    for (ClipboardBuffer buffer : buffers)
        terminal.clipboardClient->writeClipboard(buffer, bytes);
}

} // namespace Ubiquitty
