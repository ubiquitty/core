// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Bits/BitField.h"
#include "Base/Bits/OptionSet.h"
#include "Core/CellContent.h"
#include "Core/Color.h"
#include "Core/GridBounds.h"
#include "Core/Screen/MarkValues.h"

#include <array>
#include <cstdint>
#include <utility>

namespace Ubiquitty::VT {

constexpr int dirtyCellBytes = (Ubiquitty::maxColumns + 7) / 8;

// Parameter slots per control. The parser makes a new slot for `:` as well as
// `;`, so this bounds colon-separated PIECES, and the sequence that sizes it is
// an SGR carrying three colours in the ITU colon notation: `38:2::r:g:b` is six
// pieces, so foreground + background + underline is eighteen before a single
// attribute joins them. Thirty leaves room for those attributes and matches
// xterm. The ceiling is 32: Parser::parameterWasEmpty is one bit per slot.
constexpr int maxParameters = 30;
/// Where a CSI parameter saturates while its digits accumulate.
///
/// Above 65535 because the largest private mode any terminal defines is
/// foot's 77096 (input method editor); a ceiling below it turns that DECSET
/// into a different mode. Still bounded, so `CSI n b` and its kind run a
/// loop of a known worst case rather than one the wire chose.
constexpr int maxParameterValue = 99999;
// OSC / DCS accumulator: grows on demand up to maxOSCBufferSize (bounds a
// single unterminated string; covers base64 inline-image payloads).
constexpr int oscBufferInitialCapacity = 256;
constexpr int maxOSCBufferSize = 64 * 1024 * 1024; // one OSC (iTerm2 inline-image base64 fits a 4K image)
/// The most an image or clipboard OSC (iTerm2 1337, OSC 52) keeps of its
/// string: the accumulator's whole capacity less the terminating NUL it always
/// carries. Bytes past it are consumed and dropped.
constexpr int maxOSCImageSize = maxOSCBufferSize - 1;
// Per-command ceiling for non-image OSCs (title 0/1/2, hyperlink 8, ...): a
// runaway unterminated one is clipped here instead of growing to the 64 MiB
// image cap. Only image/clipboard codes (iTerm2 1337, OSC 52) keep
// maxOSCImageSize.
//
// Enforced at four sites in VT.cpp, and the four spell one rule three ways: a
// writer of N bytes admits the write while `length + N <= maxOSCNonImageSize`
// and reserves `length + N + 1`, the extra byte being the terminator the
// accumulator always carries. So `length >= max` guards a one-byte write,
// `length + 2 > max` a two-byte one, and a run clamps its own count first. The
// spelling differs because N differs; reading two of them side by side without
// their writes suggests a disagreement that is not there.
constexpr int maxOSCNonImageSize = 64 * 1024;
// Leading digits of an OSC's Ps that are worth reading. The ceiling is read from
// the string's leading digits on every append, so an unbounded scan makes
// a payload of nothing but digits quadratic — 64 KiB of '0' took 2.7 seconds
// here, because `ps = ps * 10 + 0` stays at zero and never trips the overflow
// early-out. No Ps this project routes on has more than four digits.
constexpr int maxOSCPsDigits = 8;
constexpr int maxDCSTmuxPassthroughDepth = 4;
// Ceiling on an accumulated OSC 52 chunked clipboard write; on overflow the
// transfer aborts (buffer reset, nothing applied).
constexpr int maxOSC52ChunkSize = 1 * 1024 * 1024;

// OSC 5522 extended clipboard (kitty). The write cap bounds the DECODED
// payload a remote can accumulate, one session per terminal; the read cap
// bounds what one query discloses, which is also what sizes the bulk reply
// the framed chunks queue into. Reads are capped tighter than writes because
// a read's cost lands on the reply channel and every byte of it crosses back
// over the transport.
constexpr int maxExtendedClipboardWriteBytes = 8 * 1024 * 1024;
constexpr int maxExtendedClipboardReadBytes = 1 * 1024 * 1024;

// One framed read reply is bounded by the read cap's base64 plus per-chunk
// framing; twice that leaves a second reply's headroom before the transaction
// refuses, so a pass carrying two queries answers both and a loop of them
// degrades to EBUSY rather than to silence.
constexpr int maxBulkOutputBytes = 4 * 1024 * 1024;

// Ceiling on a 5522 write session's declared alias names, as one
// space-separated list. Aliases are MIME names — short by nature — and the
// cap bounds what a remote can park on every terminal it touches.
constexpr int maxExtendedClipboardAliasBytes = 512;

// How many distinct MIME payloads one 5522 write session may carry. Eight
// covers every real clipboard shape — text, markup, an image, a few
// app-private flavours — and bounds what a remote can make a terminal hold.
constexpr int maxExtendedClipboardWriteItems = 8;
// What a Terminal retains when nothing configures it — a bench, a test, the
// snapshot tool. Every configured session gets Preferences::defaultScrollbackLines
// (10000) instead, and the two must not share a name: they differ by a factor
// of twelve, and a bare use resolves by which namespace it is lexically
// inside, which no build and no test reports.
constexpr int fallbackScrollbackLines = 800;

// OSC 8 hyperlink table — bounded ring of (id → URI). A single id must fit
// a Cell's two link-id bytes (uint16_t). Ring full → oldest slot recycled.
constexpr int linkTableCapacity = 64;
// 2048 is the interoperable ceiling, not a guess: RFC 3986 sets no limit, IE's
// historical 2083 is the lowest any shipping client imposed, and every current
// browser and server accepts at least this much (nginx and Apache default to
// 8k). 256 was below a routine OAuth callback -- a 450-character authorize URL
// was cut at byte 255, mid-percent-escape, and the truncated result still
// looked like a URL and still opened.
//
// It is not free: linkURIs is linkTableCapacity slots of this size, so the
// table goes from 16 KB to 128 KB per terminal, paid per pane whether or not
// any link is long. That is the price of a link that opens what it shows.
constexpr int maxLinkURILength = 2048;
constexpr int maxLinkKeyLength = 32;
constexpr int xtermKeyModifierCategoryCount = 8;
// Each xterm key-modifier resource's initial value, the one RIS and DECSTR
// leave behind: modifyCursorKeys (1) and modifyFunctionKeys (2) start at 2,
// the modifier as the second parameter, and the rest at 0 (xterm's resource
// defaults, `mfkParam` and `mfkOriginal`).
constexpr std::array<uint8_t, xtermKeyModifierCategoryCount> xtermKeyModifierInitialValues { 0, 2, 2, 0, 0, 0, 0, 0 };
// 15-bit id space (0 reserved)

// DECSIXEL accumulator: grows (doubling) up to the ceiling so a tiny sixel
// doesn't pre-allocate megabytes while a full-screen one still fits.
constexpr int sixelAccumulatorInitialCapacity = 65536;
constexpr int maxSixelAccumulatorCapacity = 16 * 1024 * 1024;
// How many times a payload may paint over its own canvas before it is refused.
// A `!` run is capped against the canvas, but `$` returns to column zero and `-`
// walks past the canvas bottom, so the canvas bounds neither the work nor time:
// `!2048~$` repeated through a full accumulator ran 13 seconds here against a
// canvas of four million pixels, with the pane's model lock held and the decode
// reporting no truncation.
//
// Both shapes reach the same billions — `$` by repainting one band, `-` by
// running the emit loop below a canvas it has already left — so the count is a
// total rather than a per-band one, which would price only the first.
//
// The bound has to clear what real encoders emit. libsixel — behind img2sixel,
// chafa, lsix and mpv's sixel output — writes one pass per colour present in a
// band, each pass a set of `!` runs separated by `$`, which is the same grammar
// the attack uses. Its palette tops out at 256, so 256 passes is the format's
// worst legitimate case and this is twice that. A budget below it drops the last
// colours of a real image, which renders blotchy rather than failing — the worse
// way to be wrong.
constexpr int maxSixelRepaintsOfItsCanvas = 512;
// The core parser has no cell metrics (those live in the PAL), so cursor
// advance below a stored sixel approximates cell-row height at 12 px.
constexpr int sixelLineHeightInPixels = 12;

// DECSCL conformance levels. The enumerator value is the spec's Pl code
// (61..65), so it round-trips the DECSCL / DA1 wire via std::to_underlying.
// The character SUB draws where it stands: DEC STD 070's reversed question
// mark, which is U+2426, the symbol for substitute form two, at level 2 and above,
// and the VT100's error blotch, U+2592 MEDIUM SHADE, at level 1, as xterm
// draws them. The cell holds the character, so copy and search give it back.
inline constexpr uint32_t substituteCharacter = 0x2426;
inline constexpr uint32_t vt100SubstituteCharacter = 0x2592;

/// The supplemental graphic set a program may assign as the user's preference
/// (DECAUPSS) and ask for (DECRQUPSS). DEC STD 070 defines these two.
enum class SupplementalSet : uint8_t {
    /// ISO Latin-1 supplemental, a 96-character set (Ps 1, Dscs `A`).
    ISOLatin1,
    /// DEC Supplemental, a 94-character set (Ps 0, Dscs `% 5`).
    DECSupplemental,
};

/// The direction in which carriage return and next line move the active position
/// (SIMD, ECMA-48 8.3.120).
enum class ImplicitMovementDirection : uint8_t {
    /// Along the character progression: CR and NEL move to the line home position.
    WithProgression,
    /// Against it: CR and NEL move to the line limit position.
    AgainstProgression,
};

enum class ConformanceLevel : uint8_t {
    VT100 = 61,
    VT200 = 62,
    VT300 = 63,
    VT400 = 64,
    VT500 = 65,
};

// Protection mechanisms used since RIS. This conservative summary lets ordinary
// erasure skip guarded-cell checks until SPA or DAQ has been used. Individual
// cells carry DEC protection and ECMA guarding independently.
enum class ProtectedMode : uint8_t {
    Off = 0,
    DEC = 1,
    ISO = 2,
    Both = 3,
};

/// ERASURE MODE (ECMA-48 7.2.4, SM/RM 6): which areas an erasure control
/// function puts into the erased state.
enum class ErasureMode : uint8_t {
    /// Reset: only the contents of unprotected areas are erased.
    Protect = 0,
    /// Set: protected areas are erased with the rest.
    All = 1,
};

/// GRAPHIC RENDITION COMBINATION MODE (ECMA-48 7.2.8, SM/RM 21): what an SGR
/// does with the rendition already selected.
enum class RenditionCombination : uint8_t {
    /// Set: an SGR changes only the aspects it names.
    Cumulative = 0,
    /// Reset by an RM: an SGR cancels the earlier rendition, so every aspect
    /// to keep is restated.
    Replacing = 1,
};

// Cell attribute flags — a type-safe OptionSet over the single-bit SGR/render
// attributes. The bit VALUES are frozen: Pen::attributes serializes to the sync
// wire as this set's raw uint16_t (OptionSet::toRaw/fromRaw), so an enumerator's
// value is a wire contract, not an implementation detail.
enum class Attribute : uint16_t {
    Bold = 0x0001,
    Dim = 0x0002,
    Italic = 0x0004,
    Underline = 0x0008,
    Blink = 0x0010,
    Reverse = 0x0020,
    Hidden = 0x0040,
    Strike = 0x0080,
    Wide = 0x0100,
    WideContinuation = 0x0200,
    // SGR 76 — force emoji presentation: renderer ignores default presentation
    // and VS-15/VS-16 and uses the Emoji bit as the color-vs-mono switch. Clear
    // (76:0 / SGR 0) → auto-detect from codePoint + variation selector.
    // (Reuses the historic never-set "graphic" line-drawing-hint bit.)
    ForceEmojiPresentation = 0x0400,
    Emoji = 0x0800,
    // DECSCA — selective-erase protected. Cells with this bit set survive
    // DECSED/DECSEL with Ps=1 ("selective erase"); Ps=2 honors the bit by
    // erasing only unprotected cells.
    Protected = 0x1000,
    Overline = 0x2000, // SGR 53 — line above the glyph.
    Framed = 0x4000, // SGR 51 — box drawn around the glyph.
    Encircled = 0x8000, // SGR 52 — circle. SGR 54 clears Framed|Encircled.
};

using AttributeSet = Ubiquitty::Bits::OptionSet<Attribute>;

/// The five documented XTCHECKSUM calculation switches (CSI Ps # y).
enum class ChecksumExtension : uint8_t {
    Positive = 1,
    OmitAttributes = 2,
    IncludeBlanks = 4,
    OmitUninitialized = 8,
    FullCodePoints = 16,
};
using ChecksumExtensions = Ubiquitty::Bits::OptionSet<ChecksumExtension>;
inline constexpr ChecksumExtensions checksumSupportedExtensions { ChecksumExtension::Positive, ChecksumExtension::OmitAttributes, ChecksumExtension::IncludeBlanks, ChecksumExtension::OmitUninitialized, ChecksumExtension::FullCodePoints };
// DEC leaves the checksum formula to the device. Core's default sums Unicode
// text and blank cells without video attributes; XTCHECKSUM replaces this set.
inline constexpr ChecksumExtensions checksumInitialExtensions { ChecksumExtension::OmitAttributes, ChecksumExtension::IncludeBlanks, ChecksumExtension::FullCodePoints };

// Extended attribute bits (Pen::extendedAttributes) — flags that don't fit the
// saturated 16-bit attributes field, kept here so Cell stays 32 bytes.
//
// Bits 1-4 carry the text-size spec id subfield below; a new enumerator has
// no free bit in this byte and needs the next free bit in Pen::font's low
// half instead. The disjointness asserts below the struct turn an overlap
// into a compile error instead of a pen whose subfield reads as a flag.
enum class ExtendedAttribute : uint8_t {
    BlinkFast = 0x01, // SGR 6 — rapid blink (half the SGR 5 period).
};

// Pen::extendedAttributes — one packed byte: the single-bit flags above (bit
// 0), the text-size spec id (bits 1-4, value 0-15) and the SGR 1:n
// variable-weight subfield (bits 5-7, value 0-7). The flags are an OptionSet
// (toRaw/fromRaw preserve the non-flag bits); the subfields ride them.
// Serializes to the sync wire as its raw byte.
struct ExtendedAttributes {
    Ubiquitty::Bits::OptionSet<ExtendedAttribute> flags;

    // The SGR 1:n variable-weight subfield rides the byte's high 3 bits.
    using WeightField = Ubiquitty::Bits::BitField<uint8_t, 5, 3>;

    // The text-size spec id rides bits 1-4: zero for an unsized cell, else a
    // 1-based index into the terminal's spec table. Written only by the OSC 66
    // producer, never by any SGR arm — the sweep test holds that — so a reset
    // that zeroes the pen zeroes the id with it, which is the unsized state.
    using TextSizeSpecField = Ubiquitty::Bits::BitField<uint8_t, 1, 4>;

    // SGR 1:n variable font weight (0 = none/regular, 1–7 → progressively bolder).
    [[nodiscard]] constexpr unsigned weight() const { return WeightField::get(flags.toRaw()); }
    constexpr void setWeight(unsigned weight) { flags = decltype(flags)::fromRaw(WeightField::set(flags.toRaw(), static_cast<uint8_t>(weight))); }

    /// The cell's text-size spec id; zero for an unsized cell.
    [[nodiscard]] constexpr unsigned textSizeSpecID() const { return TextSizeSpecField::get(flags.toRaw()); }

    /// Stamps the text-size spec id.
    ///
    /// - Parameter specID: A 1-based spec table index, or zero for unsized.
    constexpr void setTextSizeSpecID(unsigned specID) { flags = decltype(flags)::fromRaw(TextSizeSpecField::set(flags.toRaw(), static_cast<uint8_t>(specID))); }

    [[nodiscard]] constexpr uint8_t toRaw() const { return flags.toRaw(); }
    static constexpr ExtendedAttributes fromRaw(uint8_t raw) { return { decltype(flags)::fromRaw(raw) }; }
    friend constexpr bool operator==(ExtendedAttributes, ExtendedAttributes) = default;
};

// Every ExtendedAttribute enumerator, OR'd — the mask a flag may occupy. Update
// this when adding one; the assert below is what fails if you do not.
constexpr uint8_t extendedAttributeFlagMask = std::to_underlying(ExtendedAttribute::BlinkFast);

// The flags and the weight subfield share one byte and are declared apart, with
// nothing relating a `0x..` literal to a shift/width pair. An enumerator at 0x20
// would land on weight bit 0: every weighted cell would read as carrying the new
// flag and every flagged cell as weighted, and nothing would say so — toRaw and
// fromRaw preserve the bit, memcmp compares it, and Pen's size assert only fires
// when a field WIDENS. This is the check that fails instead.
static_assert((extendedAttributeFlagMask & ExtendedAttributes::WeightField::mask) == 0,
    "an ExtendedAttribute enumerator overlaps the SGR 1:n weight subfield — this byte has no free bit; take one in Pen::font's low half");
static_assert((extendedAttributeFlagMask & ExtendedAttributes::TextSizeSpecField::mask) == 0,
    "an ExtendedAttribute enumerator overlaps the text-size spec id — this byte has no free bit; take one in Pen::font's low half");
static_assert((ExtendedAttributes::TextSizeSpecField::mask & ExtendedAttributes::WeightField::mask) == 0,
    "the text-size spec id and the weight subfield overlap — one of the two shift/width pairs is wrong");

// Underline style codes for Pen::underlineStyle (SGR 4 / 4:n / 24).
// Attribute::Underline mirrors whether the style is non-zero, so paint paths
// that only test the bit keep working alongside painters that read the shape.
using PenUnderlineShapeField = Ubiquitty::Bits::BitField<uint8_t, 0, 3>;
constexpr uint8_t underlineNone = 0;
constexpr uint8_t underlineSingle = 1;
constexpr uint8_t underlineDouble = 2;
constexpr uint8_t underlineCurly = 3;
constexpr uint8_t underlineDotted = 4;
constexpr uint8_t underlineDashed = 5;

// Superscript / subscript state (SGR 73 / 74 / 75) — mutually exclusive, so
// one 2-bit field packed into bits 3-4 of Pen::underlineStyle rather than
// growing Pen (which would push Cell past its 32-byte memcmp-comparable size).
using PenScriptField = Ubiquitty::Bits::BitField<uint8_t, 3, 2>; // in Pen::underlineStyle
constexpr uint8_t scriptNone = 0; // SGR 75 / reset — baseline, full size
constexpr uint8_t scriptSuper = 1; // SGR 73 — raised, smaller
constexpr uint8_t scriptSub = 2; // SGR 74 — lowered, smaller

// Ideogram attributes (SGR 60–64, reset 65 / SGR 0) — mutually exclusive marks
// for CJK ideographs (ECMA-48 §8.3.117), packed into bits 5-7 of
// Pen::underlineStyle alongside the shape (low 3) and script (bits 3-4) fields.
using PenIdeogramField = Ubiquitty::Bits::BitField<uint8_t, 5, 3>; // in Pen::underlineStyle
constexpr uint8_t ideogramNone = 0; // SGR 65 / reset
constexpr uint8_t ideogramUnderline = 1; // SGR 60 — underline / right-side line
constexpr uint8_t ideogramDoubleUnderline = 2; // SGR 61 — double underline / double right-side line
constexpr uint8_t ideogramOverline = 3; // SGR 62 — overline / left-side line
constexpr uint8_t ideogramDoubleOverline = 4; // SGR 63 — double overline / double left-side line
constexpr uint8_t ideogramStress = 5; // SGR 64 — stress (emphasis) marking

// Alternate font (SGR 10–19), Fraktur (SGR 20) and proportional spacing
// (SGR 26, reset 50) — the two subfields of Pen::font. The exact alt slot
// (11..19) is not stored — no consumer needs it — so a 2-bit enum suffices
// (only Fraktur → italic has a renderer).
//
// Bit 4 carries ECMA guarded-area membership independently of DEC protection.
// The vertical band of a multi-row sized block, in Pen::font's low bits:
// zero for a single-row cell or a block's top row, else which row of its
// block this cell draws (0-6, scale at most 7). Stamped by the block writer
// beside the spec id, never by any SGR arm.
using PenTextBandField = Ubiquitty::Bits::BitField<uint8_t, 0, 3>; // in Pen::font

using PenFontStyleField = Ubiquitty::Bits::BitField<uint8_t, 5, 2>; // in Pen::font
constexpr uint8_t fontPrimary = 0; // SGR 10 / reset — the default font
constexpr uint8_t fontAlternate = 1; // SGR 11–19 — an alternate font slot (🟡 falls back to primary)
constexpr uint8_t fontFraktur = 2; // SGR 20 — Fraktur / blackletter (rendered as italic)
using PenProportionalField = Ubiquitty::Bits::BitField<uint8_t, 7, 1>; // in Pen::font — SGR 26 (reset 50)

/// A cell a rich text block covers (Docs/rich-text.md). Stamped by the block
/// writer on every cell of a block's rectangle and by no SGR arm, so the pen a
/// program prints or erases with never carries it: a covered cell that loses
/// it was changed, and the block it belonged to is gone.
using PenRichTextCoveredField = Ubiquitty::Bits::BitField<uint8_t, 3, 1>; // in Pen::font
using PenGuardedField = Ubiquitty::Bits::BitField<uint8_t, 4, 1>; // SPA/EPA and DAQ

// Pen::font's two subfields must not overlap, for the reason the extended
// attributes' assert gives: they are declared as independent shift/width pairs,
// and a wrong shift is invisible to every other check in the tree.
static_assert((PenTextBandField::mask & PenFontStyleField::mask) == 0
        && (PenTextBandField::mask & PenProportionalField::mask) == 0,
    "the text band overlaps a font subfield — one of the shift/width pairs is wrong");
static_assert((PenFontStyleField::mask & PenProportionalField::mask) == 0,
    "Pen::font's style and proportional subfields overlap");
static_assert((PenRichTextCoveredField::mask & (PenTextBandField::mask | PenFontStyleField::mask | PenProportionalField::mask)) == 0,
    "the rich text cover bit overlaps a font subfield");
static_assert((PenGuardedField::mask & (PenTextBandField::mask | PenFontStyleField::mask | PenProportionalField::mask | PenRichTextCoveredField::mask)) == 0,
    "the guarded-area bit overlaps another pen field");

} // namespace Ubiquitty::VT

// Terminal emulation grammar selector ([terminal] emulation) — which escape
// grammar the byte driver interprets:
//
//   VT        DEC VT100..VT520 + xterm/ECMA-48 (the default, hot path).
//   Linux     Linux console: ANSI base + console privates layered on VT.
//   Sco       SCO ANSI console: ANSI base + SCO sequences on VT.
//   Wyse      Wyse 50/60 — non-ANSI (ESC = row col, ESC G attr regions).
//   Televideo Televideo 925/950 — non-ANSI (ESC = row col, ESC G attrs).
//   Adm       Lear-Siegler ADM-3A/5 — non-ANSI (ESC = row col, control-only).
//   Heath     Heath/Zenith H19/H29 — non-ANSI (Heath ESC grammar).  [partial]
//   Hp        HP 2392A / 700 — non-ANSI (HP ESC& grammar).            [partial]
//
// Wyse/Televideo/Adm share an ESC= cursor-addressing core; the driver routes
// to the legacy grammar for the non-ANSI values, else stays on the VT DFA.

namespace Ubiquitty::VT {
enum class Emulation : uint8_t {
    VT = 0,
    Linux,
    Sco,
    Wyse,
    Televideo,
    Adm,
    Heath,
    Hp,
};
} // namespace Ubiquitty::VT
