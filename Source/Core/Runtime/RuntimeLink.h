// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Color.h"
#include "Core/OSC/TabStatus.h"
#include "Core/Rendering/PaletteColors.h"
#include "Core/Runtime/ModeSnapshot.h"
#include "Core/Screen/HistoryFacts.h"
#include "Core/Search/Search.h"
#include "Core/Themes/StatusColors.h"
#include "Core/VT/UserDefinedKeys.h"
#include "PAL/MousePointerShape.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>

// The per-pane cross-thread seam between the off-main ingest/writer (owning
// Ubiquitty::Terminal under the model lock) and the main-thread frontend. TWO
// lock-free primitives, portable (no threading types beyond <atomic>):
//   (R) SnapshotPublisher<T> — writer publishes a versioned snapshot; any reader
//       reads it lock-free + tear-free (model->main).
//   (W) CommandRing<T, N>    — main posts POD commands; the writer drains them
//       under the lock it already holds (main->model).
//
// SCOPE: one Ubiquitty::Terminal == one writer == one RuntimeLink. Carries only this
// pane's parser/frame state (geometry, modes, cursor, scrollback). Window LAYOUT
// (the pane tree) is a separate concern and is never published here.
//
// Safety by absence: there is no synchronous model->main primitive.
// A Core/OSC hook cannot reach a main-thread dispatch from this seam, so it cannot
// deadlock against a main thread blocked on the model lock.

namespace Ubiquitty::Runtime {

// (R) Everything the main thread reads about this pane's parser state, published
// once per drain under the model lock and read lock-free. All-scalar, trivially
// copyable. Title BYTES are not here (they ride the EventSink TITLE pull); only a
// generation bump is, so the snapshot stays fixed-size.
/// A tab status colour, or none: the two answers travel together so a colour
/// is never read apart from whether one is set.
struct TabStatusColor {
    bool isSet = false; // bool-ok: names the one condition it reports
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
};

struct RuntimeSnapshot {
    std::uint64_t version = 0; // dimension generation (0 at init, +1 per resize)
    /// The find search's latest publication, independent of arriving output.
    Search::Status search;

    // geometry (folds DimensionSnapshot)
    int rows = 0;
    int columns = 0;
    int scrollbackCount = 0;
    Screen::HistoryFacts history; // what a viewport policy judges the drain's output by
    bool isTekModeActive = false; // the Tektronix storage tube owns the frame, not the cell grid
    bool isTekGraphicInputActive = false; // Tek GIN armed — the pointer ladder's crosshair

    // mouse tracking (folds the four _cachedMouse* bools)
    bool mouseReportsActive = false; // any mode: X10 / 1000 / 1001 / 1002 / 1003
    bool mouseRelativePointerLockActive = false; // private 7403 — pointer lock + relative-delta mouselook
    bool mouseMotionReportsActive = false; // 1002 cell-motion or 1003 any-event
    bool mouseAnyEventActive = false; // 1003 any-event
    bool mouseHiliteActive = false; // 1001 hilite tracking
    bool hiliteAwaiting = false; // 1001 press sent, XTHIMOUSE reply outstanding
    bool syncHoldActive = false; // DECSET 2026 held -- the frontend books its ask-back timer from this
    // DECCOLM's 132-column mode: while it stands the application owns the
    // column count, so a window resize moves rows alone and the cells stretch
    // to the new width. Recomputing columns from the width there snaps a
    // 132-column screen back to the ~80 the window happens to fit and
    // corrupts the double-width and double-height lines drawn against it.
    bool isColumnCountHeld = false;
    // DECSET 7402 with a Color letterbox, resolved by Core's one spelling
    // (Rendering::isFrameEdgeToEdge): the frame drops its padding, so the
    // pointer maps through zero insets and every cell rect the chrome places
    // follows the grid to the drawable's edge. The mode alone rides in
    // `modes`; a frontend deciding its geometry from that bit would zero its
    // insets under a Passthrough letterbox whose grid Core still draws padded.
    bool isEdgeToEdge = false;
    // The letterbox fill, 0x00RRGGBB, read only under isEdgeToEdge: what the
    // margin outside the last whole cell clears to, for the frame path that
    // sets its clear before it takes the lock the capture reads under.
    std::uint32_t letterboxColor = 0;
    // The low 32 bits of the accepted OSC 50 arrival count. This is diagnostic
    // metadata, not application authority: a Core-issued receipt identifies
    // the leased operation, independently of which snapshot a frontend saw.
    std::uint32_t fontRequestGeneration = 0;
    bool mouseSGRPixelsActive = false; // 1016 SGR-pixel coordinates
    bool alternateScrollActive = false; // 1007 alt-screen wheel->arrows
    bool locatorActive = false; // DECELR armed — the DEC locator pointer grab

    // OSC 22 (kitty pointer shapes). Published rather than reported, because a
    // frontend that installs no WindowClient has no other channel to it, and
    // the pointer is drawn on the main thread between drains where the model
    // lock is the wrong thing to take. Two fields: an empty stack and a stack
    // holding Default are different asks, and a frontend that cannot tell them
    // apart shows the arrow where the terminal's own I-beam belongs.
    bool isAppPointerShapeSet = false;
    MousePointerShape appRequestedPointerShape = MousePointerShape::Default;

    // OSC 9;4 (ConEmu progress), published for the same reason OSC 22 is: a
    // report moves no cell, so a frontend watching the screen learns nothing
    // from it, and one that installs no WindowClient has no other channel.
    // The pair travels together — a state read apart from its value shows a
    // determinate fill at whatever percent an error left behind.
    std::uint8_t progressState = 0; // OSC::ProgressState
    std::uint8_t progressValue = 0; // 0..100, determinate and paused only

    // OSC 21337 (the session's own tab status colour — an agent marking itself
    // waiting, busy or idle), published for the reason the two fields above and
    // OSC 22 above them are: it moves no cell, so a frontend watching the
    // screen never learns of it, and this one is the sharpest of the three —
    // the escape exists to speak for a session nobody is looking at, which is
    // exactly when nothing else would say so. The four travel together for the
    // reason the progress pair does —
    // a colour read apart from whether one is set is whatever the last program
    // to set one chose, still painted after it cleared it.
    bool isTabStatusColorSet = false;
    std::uint8_t tabStatusRed = 0;
    std::uint8_t tabStatusGreen = 0;
    std::uint8_t tabStatusBlue = 0;
    // Which theme status the colour above stands for, as a
    // Themes::StatusColorSlot, or Themes::noStatusColorSlot when the program
    // chose the colour itself (OSC 21337) or none is set. The colour above is
    // the slot's as the terminal's own palette derives it; a reader that
    // knows the pane's theme asks `tabStatusColor` instead, which honours the
    // colours that theme names.
    std::uint8_t tabStatusSlot = Themes::noStatusColorSlot;
    /// The primary program-status record's App::StatusKind; zero for no record.
    /// Kept separately from colour so a semantic status can have an accessible glyph.
    std::uint8_t programStatusKind = 0;
    /// What a blocked primary record waits for, as an OSC::ProgramBlockKind;
    /// zero for a record that is not blocked, and for none.
    std::uint8_t programBlockKind = 0;
    /// A working primary record's percent, 0 to 100; -1 for a record that
    /// reports none, is not working, or for none.
    std::int8_t programProgress = -1;
    /// The program the primary record's app names, as a
    /// `Title::processIconOrdinal`; zero for none, which leaves the program to
    /// the foreground process.
    std::uint8_t reportedProcessIcon = 0;
    // The label for that colour ("Waiting"), shown when the reader hovers the
    // indicator; UTF-8, `tabStatusTextLength` bytes live, empty whenever no
    // colour is set. A fixed array because this struct is copied lock-free.
    std::uint8_t tabStatusTextLength = 0;
    std::array<char, Ubiquitty::OSC::tabStatusTextCapacity> tabStatusText {};
    // Moves when the whole of what the status says does, which the label
    // above is only the first words of; zero when it says nothing. A reader
    // showing the whole text asks the session for it when this moves.
    std::uint32_t statusHelpDigest = 0;

    /// The tab status colour under a theme's status colours.
    ///
    /// - Parameter themed: The pane's theme's status colours.
    /// - Returns: The colour to draw, with `isSet` false for none.
    [[nodiscard]] TabStatusColor tabStatusColor(const Themes::StatusColors& themed) const
    {
        if (!isTabStatusColorSet)
            return {};
        if (tabStatusSlot >= Themes::statusColorSlotCount)
            return { true, tabStatusRed, tabStatusGreen, tabStatusBlue };
        const ColorRGBA color = themed.color(static_cast<Themes::StatusColorSlot>(tabStatusSlot));
        return { true, color.red, color.green, color.blue };
    }

    /// The status label as text, empty when the program gave none.
    ///
    /// By value: the bytes live in this snapshot, and neither a reference to
    /// them nor a view into them crosses the Swift seam — the view dangles once
    /// the snapshot it borrowed from is gone, and Swift keeps no copy alive.
    ///
    /// - Returns: The label, UTF-8.
    [[nodiscard]] std::string tabStatusLabel() const
    {
        return std::string(tabStatusText.data(), tabStatusTextLength);
    }

    // keyboard / terminal modes (folds the A4 _cachedModes)
    ModeSnapshot modes;

    // animation inputs isAnythingAnimating reads (Terminal-side only)
    bool isCursorVisible = false;
    int cursorColumn = 0;
    int cursorRow = 0;
    std::uint8_t cursorStyle = 0; // DECSCUSR style
    bool cursorBlinkEnabled = false; // resolved blink pref/mode
    bool hasBlinkHint = false; // any Attribute::Blink cell on screen, EVER — the grid's monotone flag
    // Whether the live screen carries a blinking cell at the moment of this
    // capture. The hint above is monotone, so on its own it holds every clock
    // open for the life of the grid; this is the reading that retracts it,
    // taken under the same lock as the writes so it cannot be stale against
    // them. It describes the live screen alone: a viewer scrolled back displays
    // scrollback rows outside it, and keeps the hint.
    bool hasBlinkCellOnScreen = false;
    bool imagesAnimating = false; // any kitty/inline image mid-animation

    // The EFFECTIVE default background, sRGB 0-1 per channel: the palette's,
    // with DECSCNM's whole-frame swap already applied, so a reader paints the
    // ground the mode asks for. Published here because a renderer needs it off
    // the model lock: it paints the letterbox and every cell carrying no
    // background of its own, and a renderer holding its own copy paints a
    // colour the theme has already moved away from.
    float defaultBackgroundRed = 0;
    float defaultBackgroundGreen = 0;
    float defaultBackgroundBlue = 0;

    /// The theme's link colour, red channel, sRGB 0-1.
    ///
    /// Resolved by ``resolveLinkColor`` — whichever of ANSI blue and
    /// bright-blue reads better against this background, lifted toward the
    /// foreground while it falls under the legibility floor. Published beside
    /// the default background for the same reason and one sharper: a renderer
    /// keeping its own copy has no value at all on its first frame, and a link
    /// cell drawn from an unset copy is black, which is legible as text and
    /// wrong as a link.
    float linkColorRed = 0;
    /// The link colour's green channel, sRGB 0-1.
    float linkColorGreen = 0;
    /// The link colour's blue channel, sRGB 0-1.
    float linkColorBlue = 0;

    /// The model's palette epoch at this capture.
    ///
    /// Every write to the palette or its reset baseline moves it, so a reader
    /// holding an answer derived from the palette at this epoch knows the
    /// answer still stands without taking the lock the parser holds.
    std::uint64_t paletteEpoch = 0;

    // title changed-generation (bytes pulled separately via EventSink TITLE)
    std::uint32_t titleGeneration = 0;
};

/// The colour every piece of a window's chrome is painted with, and what it
/// came from.
///
/// One answer for the window's backing, its title row, the tab band and every
/// pane bar, so they cannot disagree about what colour the chrome is.
struct ChromeTint {
    /// The red channel, sRGB 0-1.
    float red = 0;
    /// The green channel, sRGB 0-1.
    float green = 0;
    /// The blue channel, sRGB 0-1.
    float blue = 0;
    /// Whether it is a DECSET 7402 letterbox fill rather than the theme's
    /// ground. A letterbox fill paints the chrome opaque whatever the window's
    /// opacity says: the bars it matches are opaque, and a see-through title
    /// row over them reads as a seam.
    bool isLetterbox = false;
    /// Whether the system's dark polarity is the legible one on it, which is
    /// what the system-drawn title and the traffic lights follow.
    bool isDark = false;
};

/// The chrome's tint for a pane, off its published snapshot.
///
/// The letterbox fill while a fullscreen-content application holds one, and the
/// effective default background otherwise — effective because DECSCNM's
/// whole-frame swap is already applied where the snapshot is filled, so a
/// reversed screen paints its chrome the colour the frame clears to.
///
/// The channels are truncated to bytes for the polarity rather than rounded,
/// so a theme sitting exactly on the boundary falls on the same side in every
/// shell.
///
/// - Parameter snapshot: The pane's published state.
/// - Returns: The tint.
[[nodiscard]] constexpr ChromeTint chromeTint(const RuntimeSnapshot& snapshot)
{
    const auto byte = [](float channel) {
        const int scaled = static_cast<int>(channel * 255.0F);
        return static_cast<std::uint8_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
    };
    ChromeTint tint;
    tint.isLetterbox = snapshot.isEdgeToEdge;
    if (tint.isLetterbox) {
        tint.red = static_cast<float>((snapshot.letterboxColor >> 16) & 0xFF) / 255.0F;
        tint.green = static_cast<float>((snapshot.letterboxColor >> 8) & 0xFF) / 255.0F;
        tint.blue = static_cast<float>(snapshot.letterboxColor & 0xFF) / 255.0F;
    } else {
        tint.red = snapshot.defaultBackgroundRed;
        tint.green = snapshot.defaultBackgroundGreen;
        tint.blue = snapshot.defaultBackgroundBlue;
    }
    tint.isDark = Ubiquitty::perceivedLightness(
                      Ubiquitty::ColorRGBA { byte(tint.red), byte(tint.green), byte(tint.blue), 0xFF })
        < 128.0;
    return tint;
}

// Copied per key press and per mouse query, so its size is the price of
// reading one mode. Bulk a session may never use — the DECUDK table — belongs
// beside it on the link, and this refuses it a way back in.
static_assert(sizeof(RuntimeSnapshot) <= 320, "RuntimeSnapshot is copied per key press; publish bulk beside it on the link, not inside it");

/// A pane's colours as its frame shows them: the palette as of the newest
/// moment no synchronized update (DEC 2026) held the frame.
///
/// A colour a program sets inside an update is not here until the update ends,
/// and one it sets and puts back inside an update never is: the grid draws
/// neither, and a host painting its chrome from these matches the grid.
struct CoherentColors {
    /// The palette epoch the colours were read at.
    std::uint64_t paletteEpoch = 0;
    /// The colours.
    Rendering::PaletteColors colors;
};

// A single writer publishes to any number of readers. Atomic words permit a
// reader to overlap reuse of its slot; the sequence check rejects that copy.
template<typename T> class SnapshotPublisher {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static constexpr std::size_t wordCount = (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
    using Words = std::array<std::uint64_t, wordCount>;

    struct Slot {
        std::array<std::atomic<std::uint64_t>, wordCount> words {};
    };

public:
    SnapshotPublisher()
    {
        const T initial {};
        const auto words = encode(initial);
        store(m_slots[0], words);
        store(m_slots[1], words);
    }

    // Writer only (the thread holding the model lock). Publishes `next`.
    void publish(const T& next)
    {
        const std::uint64_t control = m_control.load(std::memory_order_relaxed);
        const unsigned idle = 1u - static_cast<unsigned>(control & 1u);
        const auto words = encode(next);
        // A reader observing any overwritten word must also observe the
        // preceding publication when it validates the control word.
        std::atomic_thread_fence(std::memory_order_release);
        store(m_slots[idle], words);
        const std::uint64_t published = (control + 2) & ~std::uint64_t { 1 };
        m_control.store(published | idle, std::memory_order_release);
    }

    // any reader, lock-free. Returns a coherent (untorn) copy of the latest publish.
    T read() const
    {
        for (;;) {
            const std::uint64_t before = m_control.load(std::memory_order_acquire);
            Words words {};
            for (std::size_t index = 0; index < wordCount; ++index) {
                words[index] = m_slots[before & 1u].words[index].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            const std::uint64_t after = m_control.load(std::memory_order_acquire);
            if (before == after) {
                // Only a coherent representation may be materialized as T:
                // torn bytes could contain invalid bool or pointer values.
                T copy;
                std::memcpy(static_cast<void*>(&copy), words.data(), sizeof(T));
                return copy;
            }
        }
    }

    // Monotonic publish count — a free "frame dirty" gate (e.g. for the sync streamer).
    [[nodiscard]] std::uint64_t version() const { return m_control.load(std::memory_order_acquire) >> 1; }

private:
    static Words encode(const T& value)
    {
        Words words {};
        std::memcpy(words.data(), &value, sizeof(T));
        return words;
    }

    static void store(Slot& slot, const Words& words)
    {
        for (std::size_t index = 0; index < wordCount; ++index) {
            slot.words[index].store(words[index], std::memory_order_relaxed);
        }
    }

    Slot m_slots[2];
    // bit 0 = active slot index; bits 1.. = monotonic sequence. Starts at 0 (slot 0,
    // sequence 0) — a default-constructed T until the first publish.
    std::atomic<std::uint64_t> m_control { 0 };
};

// (W) CommandRing — bounded lock-free SPSC ring (main produces, the writer thread
// consumes). Elements are POD. `post` returns false when full so the caller can
// fall back rather than drop — never a silent drop.
template<typename T, std::size_t Capacity> class CommandRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // PRODUCER (main). Returns false if full (caller falls back; never drops silently).
    bool post(const T& item)
    {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= Capacity)
            return false; // full
        m_slots[head & mask] = item;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // CONSUMER (writer thread, under the model lock). Pops one item FIFO into `out`.
    bool pop(T& outputBuffer)
    {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire))
            return false; // empty
        outputBuffer = m_slots[tail & mask];
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const { return m_tail.load(std::memory_order_acquire) == m_head.load(std::memory_order_acquire); }

private:
    static constexpr std::size_t mask = Capacity - 1;
    T m_slots[Capacity] {};
    std::atomic<std::size_t> m_head { 0 }; // producer index (monotonic)
    std::atomic<std::size_t> m_tail { 0 }; // consumer index (monotonic)
};

// (W) A main->writer command. POD, ring-copyable. `kind` selects which scalar
// arguments apply.
enum class CommandKind : std::uint8_t {
    SetFocus, // flag = focused
    ResolveClipboardRead, // flag = approved, set = the selection set
    SetPresentationFocus, // flag = focused; passive mirror, no program report
};
struct Command {
    CommandKind kind {};
    bool flag = false; // bool-ok: the ring's one generic slot — each kind names it (SetFocus/SetPresentationFocus: focused; ResolveClipboardRead: approved)
    int arg0 = 0;
    int arg1 = 0;
    int arg2 = 0;
    // ResolveClipboardRead: the selection set the request named, NUL-ended.
    // Carried by value — the ring outlives any caller's view.
    std::array<char, 16> set {};
};

// The per-pane seam instance the runtime owns and a frontend drives: one per
// Ubiquitty::Terminal / pane. The writer publishes `reads` + drains `commands` under
// the model lock; main reads `reads` + posts `commands`, both lock-free.
struct RuntimeLink {
    SnapshotPublisher<RuntimeSnapshot> reads; // (R) model -> main
    // (R) The DECUDK table, published beside `reads` rather than inside it, so
    // a snapshot read copies the modes and the geometry and not a table that
    // dwarfs them. Published before `reads`, so a reader seeing a nonzero
    // userDefinedKeyCount finds a table at least that new.
    SnapshotPublisher<VT::UserDefinedKeys> userDefinedKeys; // (R) model -> main
    // (R) The colours the frame shows, beside `reads` rather than inside it:
    // the snapshot is copied per key press, and its size is what that costs.
    // Published by every snapshot publish that finds no update open, and at
    // each update's close, which the next update can follow inside one drain.
    // An expiry publishes nothing; its colours ride the next snapshot publish.
    SnapshotPublisher<CoherentColors> coherentColors; // (R) model -> main
    CommandRing<Command, 64> commands; // (W) main -> model
};

} // namespace Ubiquitty::Runtime
