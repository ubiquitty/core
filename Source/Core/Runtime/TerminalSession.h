// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"
#include "Base/SwiftInterop.h"
#include "Base/WeakPtr.h"

#include "Core/App/ProgramActivity.h"
#include "Core/CellGrid.h"
#include "Core/Cursor/RenderState.h"
#include "Core/DragDrop/Queue.h"
#include "Core/GridBounds.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Input/ComposeEngine.h"
#include "Core/Input/Keybindings.h"
#include "Core/Input/PasteNormalize.h"
#include "Core/Input/PointerInput.h"
#include "Core/Input/PointerLadder.h"
#include "Core/Keys/KeyTranslation.h"
#include "Core/Keys/Keymap.h"
#include "Core/Keys/KittyKeyboard.h"
#include "Core/Modes/DisplayIdle.h"
#include "Core/Modes/Focus.h"
#include "Core/Modes/Scroll.h"
#include "Core/Mouse/Hilite.h"
#include "Core/Mouse/Locator.h"
#include "Core/Mouse/Mouse.h"
#include "Core/OSC/Notification.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/OSC/TextSizing.h"
#include "Core/Preferences/CursorPreferences.h"
#include "Core/Preferences/FontSwap.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/IDText.h"
#include "Core/Protocol/Sync/QUICBootstrap.h"
#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/ContextActions.h"
#include "Core/Protocol/Tmux/PaneReplies.h"
#include "Core/Protocol/Tmux/Tmux.h"
#include "Core/Rendering/AnimationSchedule.h"
#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/FrameCapture.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/RichText/Blocks.h"
#include "Core/Runtime/DrainHostPacing.h"
#include "Core/Runtime/FrameDriver.h"
#include "Core/Runtime/HostDirectory.h"
#include "Core/Runtime/HostDirectoryClient.h"
#include "Core/Runtime/HostOutline.h"
#include "Core/Runtime/HostedWindows.h"
#include "Core/Runtime/IngestLoop.h"
#include "Core/Runtime/KeyEncode.h"
#include "Core/Runtime/KeyPress.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/Runtime/LocalAdoptionEscrow.h"
#include "Core/Runtime/PaneFactsCapture.h"
#include "Core/Runtime/PostedDispatcher.h"
#include "Core/Runtime/RemoteInput.h"
#include "Core/Runtime/TerminalEvents.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Runtime/TerminalRuntime.h"
#include "Core/Runtime/TerminalSnapshots.h"
#include "Core/Runtime/TriggerConfig.h"
#include "Core/Runtime/TriggerEffect.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/ClearBuffer.h"
#include "Core/Screen/LinkResolver.h"
#include "Core/Screen/PromptNavigation.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/ScrollTuning.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/ViewPlacement.h"
#include "Core/Search/SearchController.h"
#include "Core/Search/SearchExecutor.h"
#include "Core/Security/CapabilityConsentText.h"
#include "Core/Security/StructureConsentText.h"
#include "Core/Selection/CopyClipboard.h"
#include "Core/Selection/Selection.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Session/DeployPolicy.h"
#include "Core/Session/FileLink.h"
#include "Core/Session/HostInstall.h"
#include "Core/Session/InputInDoubtNotice.h"
#include "Core/Session/LinkIndicator.h"
#include "Core/Tek/Project.h"
#include "Core/Tek/Tek4014.h"
#include "Core/Themes/RethemeDefaults.h"
#include "Core/Themes/TermPaletteFromTheme.h"
#if defined(UBIQUITTY_QUIC)
#include "Core/Transport/QUICChannel.h"
#endif
#include "Core/Runtime/StructureCalls.h"
#include "Core/Session/ClipboardServe.h"
#include "Core/Session/ClosePrompt.h"
#include "Core/Session/QUICBootstrapRun.h"
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Session/SSHReach.h"
#include "Core/Session/ScreenAccessHistory.h"
#include "Core/Session/SessionDirectoryCore.h"
#include "Core/Session/SessionInfo.h"
#include "Core/Session/SessionLocation.h"
#include "Core/Session/SessionPolicy.h"
#include "Core/Session/SessionShare.h"
#include "Core/Session/SyncTraceReport.h"
#include "Core/Session/WorkspaceConnection.h"
#include "Core/Session/WorkspaceConnectionRegistry.h"
#include "Core/TerminalOperations.h"
#include "Core/Title/Directory.h"
#include "Core/Title/ProcessIcons.h"
#include "Core/Transport/SSHHost.h"
#include "Core/Transport/SubprocessChannel.h"
#include "PAL/NetworkPath.h"
#include "PAL/PAL.h"
#include "PAL/Poll.h"
#include "PAL/Stream.h"
#include "PAL/TerminalClients.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <expected>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "Core/Runtime/TerminalSessionReference.h"

namespace Ubiquitty::Runtime {

// The named byte view the Swift importer generates a safe overload for. The
// safe-interop wrapper keys on a NAMED span specialization: a parameter
// spelled std::span<const char> directly imports only as the raw
// standard-library generic, which no Swift caller can construct, while this
// alias plus the noescape annotation imports as a Swift Span overload.
using ByteSpan = std::span<const char>;

// The named alpha-table view, on the same recipe as ByteSpan: the alias plus
// the noescape annotation is what imports as a Swift Span overload.
using AlphaSpan = std::span<const std::uint8_t>;

// A payload crossing to the host, on the same recipe. Named apart from
// AlphaSpan because the two describe different things at the same width, and a
// signature reading AlphaSpan would say this carries coverage.
using PayloadSpan = std::span<const std::uint8_t>;

/// The frame-clock facts a renderer's capture options carry.
///
/// Core's capture samples no clock — the same source must produce
/// the same frame — so the tick and the cursor's blink weight arrive from a
/// caller that owns a clock. This is that clock for a session-driven surface.
struct FrameClockFacts {
    /// The monotonic 60 Hz tick, or the harness's pinned one.
    std::uint32_t blinkTicks = 0;
    /// The cursor's blink weight at that tick, 1 when solid.
    float cursorMixAlpha = 1.0f;
};

/// The animation schedule's verdict for a session-driven surface.
///
/// Split from the delay because the schedule always returns one — half a
/// second when idle — while arming a repaint timer is a decision about
/// whether anything animates at all: a timer armed on the idle delay costs
/// two frames a second on every quiet terminal forever.
struct AnimationScheduleVerdict {
    /// Whether anything on screen animates — the timer's arm predicate.
    bool hasAnimatingContent = false;
    /// The delay to the next visible change while something does.
    double delaySeconds = 0.5;
};

/// The durable half of a boundary capture's steering, registered ahead.
///
/// ``Rendering::TerminalSnapshotOptions`` cannot be stored: it is nonescapable
/// because it carries borrowed spans owned by the frame being built, and a
/// registered copy's pointers die before the boundary they would steer. This
/// holds only what survives frames — the scalar knobs and geometry — and the
/// capture assembles full options from it at the boundary, span tiers absent.
struct SyncFrameCaptureGeometry {
    /// The frame's scroll position; refresh on every scroll.
    int viewOffset = 0;
    /// Whether the frame draws a cursor at all.
    bool drawCursor = true;
    /// Whether the surface holds keyboard focus.
    bool isFocused = true;
    /// The cursor's cross-fade weight at registration time.
    float cursorMixAlpha = 1.0f;
    /// Whether the glyph under a block cursor takes the contrasting grey.
    bool shouldCursorInvertText = true;
    /// Where the grid's top-left sits within the drawable, in device pixels.
    FloatPoint originInPixels {};
    /// The rasterizer's metrics, so decorations draw at the glyphs' geometry.
    Rendering::CellMetrics metrics {};
    /// The atlas slot layout.
    Rendering::GlyphSlotGeometry glyphSlot {};
    /// The whole surface, in device pixels; zero reads as exactly the grid.
    FloatSize viewportSizeInPixels {};
    /// The cell height in points, the cursor bar's own domain; zero keeps
    /// one pixel per point.
    double cellHeightPoints = 0.0;
    /// The line spacing in points, beside ``cellHeightPoints``.
    double lineSpacingPoints = 0.0;
    /// The prompt-mark face's point size; zero leaves the mark pass off.
    double promptMarkFontSize = 0.0;
    /// The mark face's ascent, in pixels.
    float promptMarkFontAscent = 0.0f;
    /// The mark face's descent, in pixels.
    float promptMarkFontDescent = 0.0f;
    /// The mark row's top line-gap inset, in pixels.
    float promptMarkLineGapTopInPixels = 0.0f;
    /// The mark face's `[` ink above the baseline, in pixels; zero unmeasured.
    float promptMarkBracketInkTopInPixels = 0.0f;
    /// The mark face's `[` ink below the baseline, in pixels; zero unmeasured.
    float promptMarkBracketInkBottomInPixels = 0.0f;
    /// The face's x-height, in pixels; zero unmeasured.
    float promptMarkFontXHeight = 0.0f;
    /// The face's cap height, in pixels; zero unmeasured.
    float promptMarkFontCapHeight = 0.0f;
    /// The atlas oversample the mark geometry scales by; zero means one.
    float promptMarkAtlasScale = 0.0f;
    /// The frame styling, copyable; the alpha borrow lives elsewhere.
    Rendering::TerminalSnapshotStyle style {};
    /// The atlas's phase grid and raster keying, applied at the boundary
    /// through ``Rendering::applyFrameRaster`` — the same entry the live route
    /// calls. Registered rather than defaulted because a glyph placed on the
    /// portable grid while the live frames place it on CoreText's shifts by a
    /// phase bucket on every frame this route produced, and during typing the
    /// two routes alternate.
    Rendering::FrameRasterFacts raster {};
    /// OSC 8 / regex link styling — the durable half only. Hover is absent by
    /// design: the registration refreshes on placement edges, so a registered
    /// hover would outlive the pointer position it recorded, and a boundary
    /// frame drawing an unhovered link is the lesser drift.
    bool shouldUnderlineLinks = false;
    /// The VT underline code link rules draw with, when they draw.
    std::uint8_t linkUnderlineStyle = Rendering::defaultLinkUnderlineStyle;
    /// Link foreground, straight RGBA.
    Rendering::Vec4 linkColor { 0.0f, 0.0f, 0.0f, 1.0f };
    /// Hovered-link foreground, straight RGBA.
    Rendering::Vec4 linkHoverColor { 0.0f, 0.0f, 0.0f, 1.0f };
};

/// The lock-free triple-buffer exchange behind the session's boundary captures.
///
/// The ingest thread is the single producer and the Swift frame drive is the
/// single consumer. Three slots let the producer fill a free capture while one
/// is being read and another is pending. Publishing a completed capture
/// retires any older pending capture, preserving newest-wins without writing
/// storage the consumer can see.
class SyncFrameSlot {
public:
    static constexpr int slotCount = 3;

    /// The buffer the next boundary capture writes.
    ///
    /// Claims free storage before the capture starts. Slots named by
    /// `unavailableMask` remain untouched even when their exchange state is
    /// free, allowing an externally retained frame to outlive a drain.
    ///
    /// - Returns: The claimed slot, or -1 when no reusable slot exists.
    [[nodiscard]] int captureTarget(std::uint8_t unavailableMask = 0)
    {
        assert(m_writingIndex < 0);
        for (int index = 0; index < slotCount; ++index) {
            if (unavailableMask & (1u << static_cast<unsigned>(index))) {
                continue;
            }
            State expected = State::Free;
            if (m_states[index].compare_exchange_strong(
                    expected, State::Writing, std::memory_order_acquire, std::memory_order_relaxed)) {
                m_writingIndex = index;
                return index;
            }
        }
        return -1;
    }

    /// Records a completed capture into `target`.
    ///
    /// - Parameter target: The buffer written, from ``captureTarget``.
    void captured(int target)
    {
        assert(target == m_writingIndex);
        assert(m_states[target].load(std::memory_order_relaxed) == State::Writing);

        for (int index = 0; index < slotCount; ++index) {
            if (index == target)
                continue;
            State expected = State::Ready;
            if (m_states[index].compare_exchange_strong(
                    expected, State::Free, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                m_overwriteCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        m_writingIndex = -1;
        m_states[target].store(State::Ready, std::memory_order_release);
    }

    /// Whether a published capture is still waiting for the consumer.
    ///
    /// Asked by the producer on its own thread, before it decides whether a
    /// publish needs to copy the grid: a capture nobody has taken describes a
    /// moment the grid has already left, and a consumer that prefers the slot
    /// to the model would present it and then owe nothing.
    ///
    /// - Returns: Whether any slot holds a capture that has not been drained.
    [[nodiscard]] bool hasReadyCapture() const
    {
        for (int index = 0; index < slotCount; ++index) {
            if (m_states[index].load(std::memory_order_acquire) == State::Ready)
                return true;
        }
        return false;
    }

    /// Releases a claimed target after a capture fails.
    ///
    /// An older ready frame remains publishable because capture always writes
    /// a distinct free slot.
    ///
    /// - Parameter target: The buffer claimed by ``captureTarget``.
    void cancelCapture(int target)
    {
        assert(target == m_writingIndex);
        assert(m_states[target].load(std::memory_order_relaxed) == State::Writing);
        m_writingIndex = -1;
        m_states[target].store(State::Free, std::memory_order_release);
    }

    /// Hands the pending frame to the drive.
    ///
    /// - Returns: The drained buffer's index, or -1 when none waits.
    [[nodiscard]] int drain()
    {
        const int previous = m_readingIndex;
        m_readingIndex = -1;
        if (previous >= 0)
            m_states[previous].store(State::Free, std::memory_order_release);

        for (int index = 0; index < slotCount; ++index) {
            State expected = State::Ready;
            if (!m_states[index].compare_exchange_strong(
                    expected, State::Reading, std::memory_order_acquire, std::memory_order_relaxed)) {
                continue;
            }
            m_readingIndex = index;
            m_promoteCount.fetch_add(1, std::memory_order_relaxed);
            return index;
        }
        return -1;
    }

    /// Boundary frames retired before the drive promoted them.
    [[nodiscard]] std::uint64_t overwriteCount() const { return m_overwriteCount.load(std::memory_order_relaxed); }

    /// Boundary frames promoted toward the screen.
    [[nodiscard]] std::uint64_t promoteCount() const { return m_promoteCount.load(std::memory_order_relaxed); }

private:
    /// Names each slot's atomic ownership state.
    enum class State : std::uint8_t {
        Free,
        Writing,
        Ready,
        Reading,
    };

    static_assert(std::atomic<State>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    std::array<std::atomic<State>, slotCount> m_states { State::Free, State::Free, State::Free };
    // Each side alone owns its index; the atomics above carry cross-thread
    // ownership and make these bookkeeping values unobservable to the peer.
    int m_writingIndex = -1;
    int m_readingIndex = -1;
    std::atomic<std::uint64_t> m_overwriteCount { 0 };
    std::atomic<std::uint64_t> m_promoteCount { 0 };
};

/// A terminal session Swift can own: the runtime, reached without a bridge.
///
/// `TerminalRuntime` is the session — the Terminal, the stream, the drain and
/// the reply ordering — and it is non-copyable, so a language that manages
/// lifetimes by reference counting needs an owner it can hold. This is that
/// owner, on the same recipe as `Rendering::FrameCapture`: a shared reference
/// type with one factory, forwarding the calls a Swift drive makes rather than
/// handing out the runtime itself.
///
/// The forwarding surface is the drive's, not the runtime's: what is absent
/// has no Swift caller, not a reason it cannot cross. The Metal arm's bridge
/// keeps its own route until its owner is rewritten.
///
/// The session also owns the drain's host: the model lock, the paced unlock,
/// the drain pass and the sync-frame flag live here in C++, so the file
/// descriptor and the lock never cross the seam. Swift's whole ingest role is
/// scheduling — wake on readable, call ``drain()``, read the flags — which is
/// the split that keeps syscalls and locking where they already are.
class TerminalSession : public Ubiquitty::CanMakeWeakPtr {
    friend struct HostDeployTestAccess;
    // The pane-link conformance suite holds the in-process link itself.
    friend struct PaneLinkTestAccess;
#if UBIQUITTY_ENABLE_TEST_HOOKS
    friend struct LocalAdoptionTestAccess;
#endif

public:
    /// Identifies this live session for frontend client-to-view correlation.
    /// The address is process-local and must not be persisted or dereferenced.
    [[nodiscard]] std::uintptr_t identityAddress() const { return reinterpret_cast<std::uintptr_t>(this); }

    /// Initialises the terminal model at the given geometry.
    ///
    /// - Parameters:
    ///   - rows: The grid's rows.
    ///   - columns: The grid's columns.
    void start(int rows, int columns) { start(rows, columns, DrawnText::GridOnly); }

    /// Initialises the terminal model, declaring what this frontend draws.
    ///
    /// Two overloads rather than a default argument: this crosses into Swift,
    /// and an overload imports the same either way where a default argument's
    /// fate depends on the importer's version.
    ///
    /// - Parameters:
    ///   - rows: The grid's rows.
    ///   - columns: The grid's columns.
    ///   - drawnText: Which OSC 66 text this frontend's renderer draws.
    ///     `GridOnly` is the safe answer — the terminal then emits sized and
    ///     proportional runs as ordinary text rather than handing a renderer a
    ///     block it cannot draw.
    void start(int rows, int columns, DrawnText drawnText)
    {
        RuntimeInitParameters parameters;
        parameters.rows = rows;
        parameters.columns = columns;
        parameters.drawsSizedText = drawsSizedText(drawnText);
        parameters.drawsProportionalText = drawsProportionalText(drawnText);
        start(parameters);
    }

    /// Initialises the terminal model from fully-resolved parameters.
    ///
    /// The scalar overloads cover the common geometry-only case; a frontend
    /// that resolved more — scrollback depth, reflow-on-init — hands the
    /// whole set over.
    ///
    /// - Parameter parameters: The resolved init parameters.
    void start(const RuntimeInitParameters& parameters)
    {
        m_runtime->init(parameters);
        // The queue becomes the terminal's window client as soon as there is a
        // terminal, because a frontend that answers these off the drain has no
        // earlier moment to ask for it — its session was built by
        // `WorkspaceSession`, which starts the model itself. A frontend with a
        // client of its own installs it afterwards and replaces this by plain
        // assignment; the requests that
        // queued in between are dropped at the bound and none of them is a
        // request that window has yet been asked to answer.
        installWindowRequestQueue();
        // The notification sink goes in beside it, on the same reasoning and
        // at the same moment: OSC 9, 99 and 777;notify reach a frontend only
        // through `Terminal::shellClient`, and a session-driven frontend has
        // no earlier moment to ask for one.
        installNotificationQueue();
        (void)installDragDropQueue();
        // And the trigger sink, for the third time the same reason: a matched
        // trigger reaches a frontend only through `Terminal::triggerClient`.
        installTriggerQueue();
        // The measurer is the session's: a surface installs it when its
        // renderer arrives, which can be before the model exists, and a model
        // that never learns it lays every proportional run out with the grid
        // face — a drag inside the run then lands on whole cells.
        {
            const std::scoped_lock hold(*m_activeLock);
            if (Ubiquitty::Terminal* term = modelTerm())
                installTextMeasurer(*term);
        }
    }

    /// Pumps host bytes into the model, honouring sync boundaries.
    ///
    /// Loops the runtime's chunk feed until the bytes are consumed, so a caller
    /// can hand over any amount without knowing where the DECSET-2026 clears
    /// fall. Replies drain to the stream in writer order; with no stream they
    /// are dropped, which is the still-frame case.
    ///
    /// The parameter is a borrowed view for exactly this call, and the
    /// annotation says so: it is what lets Swift call this with a `Span` and
    /// no pointer ceremony.
    ///
    /// The read-snapshot is republished when the bytes are in, as the drain
    /// republishes after its pass: the lock-free readers — the input path's
    /// modes, the frontend's caches — describe the model after this call,
    /// not after the last drain. A mode the bytes set would otherwise be
    /// read as unset until the host happened to write.
    ///
    /// Parsing and publication share the active model lock with ingest and
    /// rendering. The caller must not already hold that lock. The drain signal
    /// runs after unlocking so observers can read the published model.
    ///
    /// - Parameter bytes: The bytes to feed.
    void feedBytes(ByteSpan bytes UBIQUITTY_NOESCAPE)
    {
        {
            const std::scoped_lock hold(*m_activeLock);
            const auto previousStatusGeneration = modelTerm() ? modelTerm()->tabStatusGeneration : 0;
            const std::uint32_t eraseGenerationBefore = m_runtime->eraseGeneration();
            bool didFeed = false;
            while (!bytes.empty()) {
                const FeedResult result = m_runtime->feedChunkUpToSyncClear(bytes);
                if (result.consumed <= 0) {
                    break;
                }
                didFeed = true;
                bytes = bytes.subspan(static_cast<std::size_t>(result.consumed));
            }
            if (!didFeed) {
                return;
            }
            // Fed bytes arrive whole, so a redraw they carry behind their own
            // erase has been parsed with it rather than still being on its way.
            m_runtime->noteFeedParsed(eraseGenerationBefore);
            // One feed is one write followed by a pause, so the prompt it may have
            // printed is read once for the whole feed, never per chunk.
            m_runtime->settlePasswordPromptAtPause();
            // An embedder feeding a streamless session owns the way back too.
            m_runtime->flushRepliesToSink();
            std::uint32_t eventBits = 0;
            if (auto* term = modelTerm()) {
                if (term->tabStatusGeneration != previousStatusGeneration) {
                    eventBits |= 1u << static_cast<unsigned>(EventKind::TabStatusChanged);
                }
                const auto bell = resolveBellEvent(term, PAL::monotonicTicks());
                m_events.bellsRung.fetch_add(m_runtime->takeBellsRung(), std::memory_order_relaxed);
                if (bell != BellMode::Off) {
                    m_events.bellStyle.store(static_cast<std::uint32_t>(bell), std::memory_order_relaxed);
                    m_events.bellPopsWindow.store(term->popOnBell, std::memory_order_relaxed);
                    m_events.bellRaisesUrgency.store(term->bellRaisesUrgency, std::memory_order_relaxed);
                    m_events.bellWantsNotification.store(Modes::bellWantsNotification(term), std::memory_order_relaxed);
                    eventBits |= 1u << static_cast<unsigned>(EventKind::Bell);
                }
            }
            republishModel();
            m_events.pending.fetch_or(eventBits, std::memory_order_release);
            // The event's generation is read lock-free, so the snapshot must be
            // published before a frame-clock poll can consume the notification.
            if (Ubiquitty::Terminal* term = modelTerm(); term && term->fontRequestChanged) {
                term->fontRequestChanged = false;
                m_events.pending.fetch_or(
                    1u << static_cast<unsigned>(EventKind::FontSwapRequested), std::memory_order_release);
            }
        }
        // Bytes fed here reach the same parser the host's do and raise the same
        // flags, but they miss the drain loop's deliver hook — which is what
        // signals. Without this a frontend that injects bytes hears nothing
        // about what they asked for: the flags sit in the pending word until
        // some later host byte happens to wake the pump. A window operation is
        // the case that made it visible, since it moves no cell and so has no
        // repaint behind it to arrive on.
        if (m_events.pending.load(std::memory_order_acquire) != 0)
            signalDrain();
    }

    /// Encodes one key press in this terminal's current modes.
    ///
    /// Reads modes and local-echo policy from the locked model. A published
    /// snapshot can precede a parsed protocol reset and must not determine
    /// which bytes the next key sends.
    ///
    /// - Parameters:
    ///   - press: What the frontend knows; see ``KeyPress``.
    ///   - output: Where the bytes land.
    /// - Returns: How many bytes, whether to echo, and the resolved transition.
    [[nodiscard]] KeyEncoding encodeKey(const KeyPress& press, Keys::Keymap::EncodedKeyBytes output)
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return {};
        return Runtime::encodeKey(*term, press, output);
    }

    /// Sends one key press to the host and answers what it encodes to here.
    ///
    /// A local session, and a pane of a tmux control session, are written
    /// the bytes. A remote session is sent the press itself: its host
    /// encodes it under the modes live when it writes it, which a mode the
    /// program set a moment ago may have changed since this mirror last
    /// heard (Docs/sync.md §6.7). The bytes this terminal's own modes give
    /// are still answered, for the echo SRM leaves to the frontend and for
    /// an observer of what was typed.
    ///
    /// An event with no key identity or text is ignored. A remote key is
    /// not discarded merely because this mirror suppresses its release or
    /// repeat: only the program's host can make that mode-dependent choice.
    ///
    /// - Parameters:
    ///   - press: What the frontend knows; see ``KeyPress``.
    ///   - output: Where this terminal's encoding lands.
    /// - Returns: Local bytes, echo policy, transition and endpoint admission.
    KeyEncoding sendKey(const KeyPress& press, Keys::Keymap::EncodedKeyBytes output)
    {
        KeyEncoding encoding = encodeKey(press, output);
        if (encoding.length < 0)
            return encoding;
        if (press.virtualKey == Keys::VirtualKey::None && press.unshiftedCodePoint == 0
            && press.shiftedCodePoint == 0 && press.baseLayoutCodePoint == 0 && press.textLength <= 0)
            return encoding;
        noteUserActivity();
        encoding.wasAccepted = paneLink().send(Session::PaneInput { .intent = Session::PaneInput::Key { press },
            .localEncoding = output.first(static_cast<std::size_t>(encoding.length)) });
        return encoding;
    }

    /// Sends clipboard MIME names when the application enabled paste events.
    ///
    /// - Parameters:
    ///   - mimeTypes: Space-separated names captured by the frontend's paste gesture.
    ///   - buffer: The clipboard location selected by the user.
    /// - Returns: Whether mode 5522 consumed the gesture.
    bool sendPasteNotification(const std::string& mimeTypes, ClipboardBuffer buffer = ClipboardBuffer::Clipboard)
    {
        bool mayRead = false;
        {
            const std::scoped_lock hold(*m_activeLock);
            const auto* terminal = modelTerm();
            if (!terminal || !terminal->modes.shouldReportPasteEvents)
                return false;
            mayRead = terminal->isOSC52Allowed && terminal->osc52ClipboardReadPolicy != OSC::ClipboardReadPolicy::Deny;
        }
        const auto notification = mayRead ? OSC::clipboardPasteNotification(mimeTypes, buffer)
                                          : std::string("\033]5522;type=read:status=EPERM\033\\");
        (void)paneLink().send(Session::PaneInput { .intent = Session::PaneInput::Raw {}, .localEncoding = notification });
        return true;
    }

    /// Sends pasted text to the host and answers the bytes it comes to here.
    ///
    /// A local session is written the text fenced and normalised by
    /// ``Input::wrapPaste``. A remote session is sent the text itself, as one
    /// paste: its host fences and normalises it under the modes live when
    /// it writes it, sends no more than the program has read a window of,
    /// and an interrupt typed behind it ends it.
    ///
    /// - Parameter text: The clipboard's text, UTF-8.
    /// - Returns: The bytes this terminal's modes give, for the echo and for
    ///   an observer of what was typed; empty when nothing was sent.
    std::string sendPaste(const std::string& text)
    {
        if (sendPasteNotification("text/plain"))
            return {};
        Input::BracketedPaste bracketedPaste;
        Input::PasteOptions options;
        {
            const std::scoped_lock hold(*m_activeLock);
            const auto* terminal = modelTerm();
            if (!terminal)
                return {};
            bracketedPaste = terminal->modes.bracketedPaste ? Input::BracketedPaste::Enabled : Input::BracketedPaste::Disabled;
            options = { .shouldQuoteCharacters = terminal->modes.shouldQuotePasteCharacters, .shouldPreserveNewlines = terminal->modes.shouldPreservePasteNewlines };
        }
        std::string wrapped = Input::wrapPaste(text, bracketedPaste, options);
        if (wrapped.empty())
            return wrapped;
        if (!paneLink().send(Session::PaneInput { .intent = Session::PaneInput::Paste { text }, .localEncoding = wrapped }))
            wrapped.clear();
        return wrapped;
    }

    /// Whether bytes the user sends other than keys — a paste, an IME
    /// commit, a macro — are also shown locally: the key encoder's rule
    /// (``Runtime::shouldEchoInputLocally``), with the password prompt read
    /// off the model as the encoder's caller reads it.
    [[nodiscard]] bool shouldEchoInputLocally()
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        return Runtime::shouldEchoInputLocally(Runtime::sampleModes(*term)) && !term->passwordPromptActive;
    }

    /// Writes input bytes to the host, bypassing the model.
    ///
    /// A remainder the transport refused rides the runtime's pending-write
    /// queue, and the parked ingest loop is woken to drain it: the loop's
    /// watch set added Writable interest before it parked, so a queue filled
    /// from this thread afterwards is invisible to the wait until a wake
    /// re-runs the top of the loop.
    ///
    /// - Parameter bytes: The bytes to send.
    void sendInput(ByteSpan bytes UBIQUITTY_NOESCAPE)
    {
        (void)paneLink().send(Session::PaneInput { .intent = Session::PaneInput::Raw {}, .localEncoding = bytes });
    }

    /// Whether input sent here reaches a host.
    ///
    /// The routes ``sendInput`` takes, and nothing else: a tmux -CC primary's
    /// control channel, a remote core, the runtime's own host stream, or the
    /// sink an embedder feeding a streamless session installed. A session
    /// with none of them -- a passive mirror, a preview fed by hand -- drops
    /// what it is sent, so a fan-out that counts it promises a pane the
    /// keystrokes never reach. The one answer to "can this pane take input",
    /// for a broadcast choosing its targets and for the count its
    /// confirmation names.
    ///
    /// - Returns: True when sent input has somewhere to go.
    [[nodiscard]] bool hasHostInputSink() const
    {
        return m_tmuxPrimaryStream || (m_remote && m_remote->core) || (m_runtime && m_runtime->isLocalStreamEnabled() && (m_runtime->hasHostStream() || m_runtime->hasReplySink()));
    }

    /// Whether sent input is still queued behind a full PTY.
    ///
    /// A frontend that owns the poll loop watches this after a send: while it
    /// holds, the PTY wants a writability watch, and the queued bytes go
    /// nowhere until ``drainPendingWrite`` runs. The tmux and remote routes
    /// carry their input elsewhere and never queue here.
    ///
    /// - Returns: True while queued input bytes remain undelivered.
    [[nodiscard]] bool hasPendingWrite() const { return !m_tmuxPrimaryStream && !m_remote && m_runtime && m_runtime->hasPendingWrite(); }

    /// Writes queued input bytes as far as the PTY accepts them.
    ///
    /// Called from the owner's poll loop when the PTY reports writable; safe
    /// to call with nothing queued.
    ///
    /// - Returns: True while queued bytes still remain after the write.
    bool drainPendingWrite() { return !m_tmuxPrimaryStream && !m_remote && m_runtime && m_runtime->drainPendingWrite(); }

    /// Routes this session's outbound input through a tmux -CC primary.
    ///
    /// Every send wraps in send-keys for the pane and rides the primary's
    /// stream instead of a local PTY — the render-only pane's input half,
    /// beside the adoption seam that is its output half. Cleared with nulls.
    ///
    /// - Parameters:
    ///   - client: The primary's tmux client, for the wrap's option facts.
    ///   - primaryStream: The -CC control channel's stream; null clears.
    ///   - paneID: The tmux pane the wrapped keys target.
    void routeInputViaTmux(const Ubiquitty::Protocol::Tmux::TmuxClient* client, Ubiquitty::PAL::Stream* primaryStream, int paneID)
    {
        if (hasUnresolvedLocalAdoption())
            return;
        m_tmuxRoutingClient = client;
        m_tmuxPrimaryStream = primaryStream;
        m_tmuxRoutingPaneID = paneID;
    }

    /// What a pane verb asks of the session's backend.
    enum class PaneVerb : std::uint8_t {
        /// Split beside — a column split.
        SplitBeside UBIQUITTY_SWIFT_NAME(splitBeside),
        /// Split below — a row split.
        SplitBelow UBIQUITTY_SWIFT_NAME(splitBelow),
        /// Close this pane, ending its process.
        Close UBIQUITTY_SWIFT_NAME(close),
        /// Toggle the pane filling its window.
        Zoom UBIQUITTY_SWIFT_NAME(zoom),
        /// Take the backend's focus — the press landed here.
        Focus UBIQUITTY_SWIFT_NAME(focus)
    };

    /// How the backend answered a pane verb.
    enum class PaneVerbOutcome : std::uint8_t {
        /// The backend performed it; its own echo updates the views, and
        /// the frontend does nothing further.
        Handled UBIQUITTY_SWIFT_NAME(handled),
        /// The verb is the frontend's to perform locally — the local-PTY
        /// case, where a split means a view and a session only the
        /// frontend can create.
        LocalActionRequired UBIQUITTY_SWIFT_NAME(localActionRequired),
        /// The backend cannot perform it — a remote daemon with no split
        /// model. The frontend stands the affordance down.
        Unsupported UBIQUITTY_SWIFT_NAME(unsupported)
    };

    /// Asks the session's backend to perform a pane verb, so a frontend
    /// never learns what the backend is: a tmux route ships the wire
    /// command and the server's echo re-mirrors the views; a local PTY
    /// answers that the verb is the frontend's; a thin client answers
    /// unsupported until the daemon models panes.
    ///
    /// - Parameter verb: What the user asked of the pane.
    /// - Returns: How the backend answered.
    PaneVerbOutcome requestPaneVerb(PaneVerb verb)
    {
        if (m_tmuxPrimaryStream) {
            switch (verb) {
            case PaneVerb::SplitBeside:
                sendTmuxCommandLine(Ubiquitty::Protocol::Tmux::splitWindow(m_tmuxRoutingPaneID, Ubiquitty::Protocol::Tmux::SplitDirection::SideBySide));
                break;
            case PaneVerb::SplitBelow:
                sendTmuxCommandLine(Ubiquitty::Protocol::Tmux::splitWindow(m_tmuxRoutingPaneID, Ubiquitty::Protocol::Tmux::SplitDirection::Stacked));
                break;
            case PaneVerb::Close:
                sendTmuxCommandLine(Ubiquitty::Protocol::Tmux::killPane(m_tmuxRoutingPaneID));
                break;
            case PaneVerb::Zoom:
                sendTmuxCommandLine(Ubiquitty::Protocol::Tmux::zoomPane(m_tmuxRoutingPaneID));
                break;
            case PaneVerb::Focus:
                sendTmuxCommandLine(Ubiquitty::Protocol::Tmux::selectPane(m_tmuxRoutingPaneID));
                break;
            }
            return PaneVerbOutcome::Handled;
        }
        // A shared session is a local pane the daemon drives: closing or
        // focusing it is the frontend's layout, and a split or a zoom would ask
        // the daemon's workspace for panes this window does not show.
        if (m_sharing)
            return verb == PaneVerb::Close || verb == PaneVerb::Focus ? PaneVerbOutcome::LocalActionRequired : PaneVerbOutcome::Unsupported;
        if (m_remote)
            return requestRemoteWorkspaceVerb(verb);
        return PaneVerbOutcome::LocalActionRequired;
    }

    /// Selects how much of a source layout transfers together.
    enum class TransferScope : std::uint8_t {
        Pane,
        Tab
    };

    /// Selects how a transfer joins an existing destination.
    enum class TransferPlacement : std::uint8_t {
        Split,
        Tab
    };

    /// Selects the container created for an extracted layout.
    enum class ExtractionPlacement : std::uint8_t {
        Tab,
        Window
    };

    /// Reports whether two panes can share one split layout.
    ///
    /// - Parameter target: The session at the destination pane.
    /// - Returns: Whether both sessions share a compatible layout authority.
    [[nodiscard]] bool canTransferTo(const TerminalSession& target) const
    {
        if (m_tmuxPrimaryStream || target.m_tmuxPrimaryStream) {
            return m_tmuxPrimaryStream && m_tmuxPrimaryStream == target.m_tmuxPrimaryStream;
        }
        if ((m_remote && !m_sharing) || (target.m_remote && !target.m_sharing)) {
            return m_remote && target.m_remote && !m_sharing && !target.m_sharing && sharesRemoteWorkspaceConnection(target)
                && remoteWorkspaceID() && remoteWorkspaceID() == target.remoteWorkspaceID();
        }
        return !m_externalTerminal && !target.m_externalTerminal && !m_hostedWindows && !target.m_hostedWindows;
    }

    /// Routes a pane or tab transfer to the authority that owns both endpoints.
    ///
    /// - Parameters:
    ///   - target: The session at the destination pane.
    ///   - sourceTree: The complete source layout for a whole-tab transfer.
    ///   - orientation: The direction of the destination split.
    ///   - side: The side of the target occupied by the transferred layout.
    ///   - scope: The portion of the source layout to transfer.
    ///   - placement: The container receiving the transferred layout.
    ///   - insertTarget: Whether to split one destination pane or its complete layout.
    /// - Returns: Whether the backend accepted the request, local layout work
    ///   is required, or the transfer is unsupported.
    PaneVerbOutcome requestTransferTo(TerminalSession& target, const Panes::Tree& sourceTree,
        Panes::Orientation orientation, Panes::InsertSide side, TransferScope scope, TransferPlacement placement,
        Panes::InsertTarget insertTarget = Panes::InsertTarget::Pane)
    {
        if (!canTransferTo(target)) {
            return PaneVerbOutcome::Unsupported;
        }
        if (m_tmuxPrimaryStream) {
            if (placement == TransferPlacement::Tab) {
                return scope == TransferScope::Tab ? PaneVerbOutcome::LocalActionRequired : requestExtraction(TransferScope::Pane, ExtractionPlacement::Tab);
            }
            const auto command = scope == TransferScope::Tab
                ? Protocol::Tmux::moveLayout(sourceTree, target.m_tmuxRoutingPaneID, orientation, side, insertTarget)
                : Protocol::Tmux::movePane(m_tmuxRoutingPaneID, target.m_tmuxRoutingPaneID, orientation, side, 50, insertTarget);
            if (command.empty()) {
                return PaneVerbOutcome::Unsupported;
            }
            sendTmuxCommandLine(command);
            if (scope == TransferScope::Pane) {
                sendTmuxCommandLine(Protocol::Tmux::selectPane(m_tmuxRoutingPaneID));
            }
            return PaneVerbOutcome::Handled;
        }
        if (m_remote && !m_sharing) {
            Ubiquitty::Session::WorkspaceIntent verb;
            verb.verb = placement == TransferPlacement::Tab ? (scope == TransferScope::Tab ? Ubiquitty::Session::WorkspaceIntentKind::MoveTabToSurface : Ubiquitty::Session::WorkspaceIntentKind::MovePaneToNewTab)
                : scope == TransferScope::Tab               ? Ubiquitty::Session::WorkspaceIntentKind::MoveTabToPane
                                                            : Ubiquitty::Session::WorkspaceIntentKind::MovePaneToTab;
            verb.tabID = std::to_underlying(remoteWorkspaceTab());
            verb.paneID = std::to_underlying(remoteWorkspacePane());
            verb.targetPaneID = std::to_underlying(target.remoteWorkspacePane());
            verb.targetTabID = std::to_underlying(target.remoteWorkspaceTab());
            verb.orientation = std::to_underlying(orientation);
            verb.side = std::to_underlying(side);
            verb.insertTarget = std::to_underlying(insertTarget);
            return sendRemoteWorkspaceVerb(verb) ? PaneVerbOutcome::Handled : PaneVerbOutcome::Unsupported;
        }
        return PaneVerbOutcome::LocalActionRequired;
    }

    /// Extracts a pane or tab through its backend; native panes stay local.
    ///
    /// - Parameters:
    ///   - scope: The portion of the layout to extract; a tab creates a window.
    ///   - placement: The container created for an extracted pane.
    /// - Returns: Whether the backend accepted the request, local layout work
    ///   is required, or extraction is unsupported.
    PaneVerbOutcome requestExtraction(TransferScope scope, ExtractionPlacement placement)
    {
        if (m_tmuxPrimaryStream) {
            if (scope == TransferScope::Tab) {
                return PaneVerbOutcome::LocalActionRequired;
            }
            sendTmuxCommandLine(Protocol::Tmux::breakPane(m_tmuxRoutingPaneID));
            return PaneVerbOutcome::Handled;
        }
        if (m_remote && !m_sharing) {
            const auto verb = scope == TransferScope::Tab  ? Ubiquitty::Session::WorkspaceIntentKind::ExtractTabToSurface
                : placement == ExtractionPlacement::Window ? Ubiquitty::Session::WorkspaceIntentKind::ExtractPaneToSurface
                                                           : Ubiquitty::Session::WorkspaceIntentKind::ExtractPaneToTab;
            return sendRemoteWorkspaceVerb(verb, remoteWorkspaceTab(), remoteWorkspacePane())
                ? PaneVerbOutcome::Handled
                : PaneVerbOutcome::Unsupported;
        }
        return m_externalTerminal || m_hostedWindows ? PaneVerbOutcome::Unsupported : PaneVerbOutcome::LocalActionRequired;
    }

    /// The routing above, spelled for the language that cannot name the
    /// pointer types: addresses cross the seam and the casts stay here, the
    /// same arrangement the adoption seam's lock address rides.
    ///
    /// - Parameters:
    ///   - clientAddress: The primary's tmux client's address; zero clears.
    ///   - streamAddress: The control channel stream's address; zero clears.
    ///   - paneID: The tmux pane the wrapped keys target.
    void routeInputViaTmuxAddresses(std::uintptr_t clientAddress, std::uintptr_t streamAddress, int paneID)
    {
        routeInputViaTmux(reinterpret_cast<const Ubiquitty::Protocol::Tmux::TmuxClient*>(clientAddress),
            reinterpret_cast<Ubiquitty::PAL::Stream*>(streamAddress), paneID);
    }

    /// Where a tmux intent aimed at this session lands: the -CC control
    /// channel for a routed pane, prefix chords into the PTY for a
    /// full-screen `tmux attach`, nowhere for anything else.
    ///
    /// The process facts are read from the live table at the call, not from
    /// the title walk's last sample: a reader who has just typed `tmux attach`
    /// asks before any walk has run.
    ///
    /// - Returns: The context ``Protocol::Tmux::classifyPaneContext`` gives
    ///   this session's facts.
    [[nodiscard]] Ubiquitty::Protocol::Tmux::PaneContext tmuxPaneContext()
    {
        Ubiquitty::Protocol::Tmux::PaneContextFacts facts;
        facts.isControlModePane = m_tmuxPrimaryStream != nullptr;
        std::string foregroundName;
        if (!facts.isControlModePane && !m_externalTerminal && !m_remote) {
            if (auto* stream = m_runtime->stream()) {
                foregroundName = stream->process().foregroundName();
                if (foregroundName == "tmux") {
                    constexpr std::size_t walkCapacity = 128;
                    std::vector<PAL::Process::Record> records(walkCapacity);
                    const int found = stream->process().gatherProcesses({ records.data(), records.size() });
                    facts.isForegroundControlModeTmux
                        = PAL::Process::isForegroundControlModeTmux({ records.data(), static_cast<std::size_t>(found > 0 ? found : 0) });
                }
            }
        }
        facts.foregroundName = foregroundName;
        return Ubiquitty::Protocol::Tmux::classifyPaneContext(facts);
    }

    /// Performs a tmux intent over whichever transport this session's input
    /// takes, aimed at this session's own pane.
    ///
    /// The pane is the session's, not its window's first: an intent raised
    /// from the focused pane of a split window splits or kills that pane.
    ///
    /// The keystroke transport here writes to the host and tells nobody,
    /// which is all a frontend without an input route of its own can do. A
    /// frontend that has one asks ``tmuxIntentKeystrokes`` for the chord and
    /// types it, because a chord is the reader's own typing and everything
    /// watching typed input must see it.
    ///
    /// - Parameters:
    ///   - intent: What the reader asked for.
    ///   - value: The new name for a rename; ignored otherwise.
    /// - Returns: Whether bytes were sent. False for a native pane, whose
    ///   frontend's own action applies instead.
    bool performTmuxIntent(Ubiquitty::Protocol::Tmux::ActionIntent intent, std::string_view value)
    {
        using Transport = Ubiquitty::Protocol::Tmux::ContextAction::Transport;
        const Ubiquitty::Protocol::Tmux::ContextAction action
            = Ubiquitty::Protocol::Tmux::synthesizeAction(tmuxPaneContext(), intent, m_tmuxRoutingPaneID, value);
        switch (action.transport) {
        case Transport::ControlCommand:
            sendTmuxCommandLine(action.bytes);
            return true;
        case Transport::Keystrokes:
            sendInput({ action.bytes.data(), action.bytes.size() });
            return true;
        case Transport::None:
            return false;
        }
        return false;
    }

    /// The prefix chord a tmux intent takes into a full-screen `tmux attach`,
    /// for a frontend that types it on the reader's behalf.
    ///
    /// Empty for every other context: a -CC pane's intent is a control
    /// command, which ``performTmuxIntent`` sends, and a native pane's is the
    /// frontend's own action. A chord is what the reader would have typed, so
    /// a frontend with a user-input route sends these bytes through it — the
    /// route that drops the selection, echoes under a reset SRM and reaches a
    /// broadcast fan-out — rather than having the session write them to the
    /// host with nobody told.
    ///
    /// - Parameters:
    ///   - intent: What the reader asked for.
    ///   - value: The new name for a rename; ignored otherwise.
    /// - Returns: The chord's bytes, empty for a pane that takes no chord.
    [[nodiscard]] std::string tmuxIntentKeystrokes(Ubiquitty::Protocol::Tmux::ActionIntent intent, std::string_view value)
    {
        const Ubiquitty::Protocol::Tmux::ContextAction action
            = Ubiquitty::Protocol::Tmux::synthesizeAction(tmuxPaneContext(), intent, m_tmuxRoutingPaneID, value);
        if (action.transport != Ubiquitty::Protocol::Tmux::ContextAction::Transport::Keystrokes)
            return {};
        return action.bytes;
    }

    /// Closes one tmux window over this pane's control connection.
    /// The server's window-close event retires its views.
    ///
    /// - Parameter windowID: The window captured by the close action.
    /// - Returns: Whether the command was sent.
    bool requestTmuxWindowClose(int windowID)
    {
        if (!m_tmuxPrimaryStream || windowID < 0)
            return false;
        sendTmuxCommandLine(Ubiquitty::Protocol::Tmux::killWindow(windowID));
        return true;
    }

    /// Selects a session-wide action, independent of closing one window.
    enum class TmuxSessionClose : std::uint8_t {
        /// Detach this client; the session keeps running on the server and
        /// can be attached again.
        Detach UBIQUITTY_SWIFT_NAME(detach),
        /// Kill the session and every window in it.
        Kill UBIQUITTY_SWIFT_NAME(kill),
    };

    /// Ends this pane's tmux session over its control channel. The server
    /// answers with `%exit`, which drops every window of the session at once.
    ///
    /// - Parameter close: Whether the session survives.
    /// - Returns: Whether the command was sent; false for a session that is
    ///   not a tmux pane.
    bool requestTmuxSessionClose(TmuxSessionClose close)
    {
        if (!m_tmuxPrimaryStream)
            return false;
        sendTmuxCommandLine(close == TmuxSessionClose::Detach ? Ubiquitty::Protocol::Tmux::detachClient() : Ubiquitty::Protocol::Tmux::killSession());
        return true;
    }

    /// Tells tmux the control client's size: the grid of the whole window
    /// the session's panes tile, which tmux divides among them itself.
    ///
    /// A pane's own grid is the wrong number here. The session sizes to its
    /// latest client, so reporting one tile of a split would shrink every
    /// window of the session to that tile.
    ///
    /// - Parameters:
    ///   - columns: The window content's columns.
    ///   - rows: The window content's rows.
    /// - Returns: Whether the size was sent; false for a session that is not
    ///   a tmux pane, or a grid with no cells.
    bool reportTmuxClientSize(int columns, int rows)
    {
        if (!m_tmuxPrimaryStream || columns < 1 || rows < 1)
            return false;
        // A pane of a source this process hosts: the source sends the report
        // and sizes the session's other windows to it ahead of the server.
        if (m_hostedPaneSource)
            return m_hostedPaneSource->reportClientSize(m_hostedPaneID, columns, rows);
        sendTmuxCommandLine(Ubiquitty::Protocol::Tmux::refreshClientSize(columns, rows));
        return true;
    }

    /// Lets every session made from here on host the windows of a foreground
    /// program that asks to, through the factory named.
    ///
    /// Process-wide and set once, at launch: a frontend that links no source
    /// never calls it, and its sessions never host. The factory's address
    /// crosses as a number for the reason the adoption seam's lock address
    /// does.
    ///
    /// - Parameters:
    ///   - factoryAddress: A ``HostedWindowSourceFactory``, as an address.
    ///   - loop: The frontend's loop, where every hosting source is made and
    ///     posts its work.
    static void enableHostedWindows(std::uintptr_t factoryAddress, PostedWorkLoop loop)
    {
        s_hostedWindowLoop = loop;
        s_hostedWindowSourceFactory.store(reinterpret_cast<HostedWindowSourceFactory>(factoryAddress), std::memory_order_release);
    }

    /// Whether this session's program hosts windows the frontend shows.
    [[nodiscard]] bool isHostingWindows() const { return m_hostedWindows != nullptr; }

    /// Captures a hosted identity and verifies whether its client runs locally.
    ///
    /// - Returns: The backend reference; unknown carriers cannot reconnect locally.
    [[nodiscard]] HostedSessionReference hostedSessionReference()
    {
        if (!m_hostedWindows) {
            return {};
        }
        auto reference = m_hostedWindows->sessionReference();
        if (reference.isConfirmed && !m_tmuxSSHHost.empty()) {
            reference.sshHost = m_tmuxSSHHost;
            return reference;
        }
        if (reference.isConfirmed && !m_remote && !m_externalTerminal) {
            if (auto* stream = m_runtime->stream()) {
                std::vector<PAL::Process::Record> records(128);
                const int count = stream->process().gatherProcesses({ records.data(), records.size() });
                if (count > 0 && static_cast<std::size_t>(count) <= records.size()) {
                    reference.canReconnectLocally = PAL::Process::isForegroundControlModeTmux({ records.data(), static_cast<std::size_t>(count) });
                }
            }
        }
        return reference;
    }

    /// The oldest hosted-window change the frontend has not shown.
    ///
    /// The frontend drains until the kind is `None`, whenever
    /// ``EventKind::HostedWindowsChanged`` is raised.
    ///
    /// - Returns: The change; `None` when there is none or nothing is hosted.
    [[nodiscard]] HostedWindowEvent takeHostedWindowEvent() { return m_hostedWindows ? m_hostedWindows->takeEvent() : HostedWindowEvent {}; }

    /// The hosted panes whose models changed since the last call.
    ///
    /// - Returns: Their ids, each once; empty when nothing is hosted.
    [[nodiscard]] std::vector<int> takeRepaintedHostedPanes() { return m_hostedWindows ? m_hostedWindows->takeRepaintedPanes() : std::vector<int> {}; }

    /// What a view binds a hosted pane to.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: Its facts; zero addresses when it is not live.
    [[nodiscard]] HostedPaneFacts hostedPaneFacts(int paneID) { return m_hostedWindows ? m_hostedWindows->paneFacts(paneID) : HostedPaneFacts {}; }

    /// Tells the source a hosted pane's view unbound, so a pane the program
    /// retired can be freed.
    ///
    /// - Parameter paneID: The pane.
    void releaseHostedPane(int paneID)
    {
        if (m_hostedWindows)
            m_hostedWindows->releasePane(paneID);
    }

    /// Answers a closed hosted window, once its pane views are unbound.
    ///
    /// - Parameter windowID: The window.
    void releaseHostedWindow(int windowID)
    {
        if (m_hostedWindows)
            m_hostedWindows->releaseWindow(windowID);
    }

    /// Stops hosting: the source leaves the terminal, which returns to
    /// passthrough, and is freed. Called once the frontend has unbound every
    /// hosted pane view, because the source owns the models those views read.
    void endHostedWindows()
    {
        if (!m_hostedWindows)
            return;
        m_hostedWindows->detach();
        m_hostedWindows.reset();
    }

    /// The hosting source, as the address a hosted pane's session routes its
    /// resize through; zero when nothing is hosted.
    [[nodiscard]] std::uintptr_t hostedWindowSourceAddress() const { return reinterpret_cast<std::uintptr_t>(m_hostedWindows.get()); }

    /// Routes this adopted pane's resize and client-size report through the
    /// source that hosts it.
    ///
    /// The pane's model is the source's: a resize goes to the source, which
    /// resizes it now, ahead of the program's re-layout confirming it.
    /// Cleared with the adoption.
    ///
    /// - Parameters:
    ///   - sourceAddress: ``hostedWindowSourceAddress`` of the hosting session.
    ///   - paneID: The pane this session renders.
    void routeResizeThroughHostedSource(std::uintptr_t sourceAddress, int paneID)
    {
        if (hasUnresolvedLocalAdoption())
            return;
        m_hostedPaneSource = reinterpret_cast<HostedWindowSource*>(sourceAddress);
        m_hostedPaneID = paneID;
    }

    /// Attaches this session as a thin client: the model becomes the
    /// passive mirror the host's screen frames rewrite, and every
    /// keystroke, paste and mouse report ships as Sync input. The
    /// transport, the protocol and the event surface are Core's — the
    /// remote events ride the same flag-and-signal path a local drain
    /// raises, so a frontend calls this where it would have launched a
    /// shell and changes nothing else.
    ///
    /// The loop a platform owns crosses as a ``PostedWorkLoop``: its two
    /// entry points are each handed a ``PostedWorkToken``, which the platform
    /// redeems through ``runPostedSessionWork`` on its loop.
    ///
    /// - Parameters:
    ///   - shellCommand: The transport command (`ssh … --attach`).
    ///   - quicHost: The host a QUIC carrier dials; empty offers no QUIC and
    ///     the session takes the transport command alone.
    ///   - quicBootstrapCommand: Prints the daemon's QUIC port, one-time
    ///     token and certificate pin; empty likewise offers no QUIC.
    ///   - reattachSessionID: The daemon session to re-home to. Zero asks for
    ///     a fresh shell, which is what a first connect wants; a restored tab
    ///     names the session it was captured holding, and a name the daemon no
    ///     longer knows degrades to a fresh shell rather than failing the
    ///     attach.
    ///   - loop: The platform's loop, where the session's work runs.
    /// - Returns: Whether the attach started.
    bool attachRemote(const std::string& shellCommand, const std::string& quicHost, const std::string& quicBootstrapCommand, std::uint32_t reattachSessionID, PostedWorkLoop loop) // bool-ok: mirrors the single HELLO flag bit helloSessionEndWhenDetached; an enum here would be a second spelling of one wire bit
    {
        return attachRemote(shellCommand, quicHost, quicBootstrapCommand, reattachSessionID, std::string(), false, loop);
    }

    /// Attaches over a spawned carrier, naming the workspace to attach to or
    /// create.
    ///
    /// ``attachRemote`` in every respect but the name, which the spawning door
    /// carries for the reason the embedder's does: a saved host that names a
    /// workspace reaches the session bearing it if the daemon holds one and
    /// creates one under it otherwise, in one connect. An empty name is the
    /// unnamed attach, which is what the shorter door sends.
    ///
    /// An overload rather than defaulted parameters, because the Swift seam
    /// imports no default arguments: a defaulted one would be a door no
    /// frontend on that side could open with fewer arguments than it has.
    ///
    /// - Parameters:
    ///   - shellCommand: As ``attachRemote``.
    ///   - quicHost: As ``attachRemote``.
    ///   - quicBootstrapCommand: As ``attachRemote``.
    ///   - reattachSessionID: As ``attachRemote``. A reattach carries the name
    ///     too and the daemon ignores it, because a reattach observes what its
    ///     session already is.
    ///   - workspaceName: The workspace to attach under; empty attaches unnamed.
    ///   - endsWhenDetached: Whether a session created under the name ends when
    ///     its last human detaches cleanly.
    ///   - loop: As ``attachRemote``.
    /// - Returns: Whether the attach started.
    bool attachRemote(const std::string& shellCommand, const std::string& quicHost, const std::string& quicBootstrapCommand, std::uint32_t reattachSessionID, const std::string& workspaceName, bool endsWhenDetached, PostedWorkLoop loop) // bool-ok: mirrors the single HELLO flag bit helloSessionEndWhenDetached; an enum here would be a second spelling of one wire bit
    {
        if (!beginRemote(loop, 0))
            return false;
        m_remote->factory->shellCommand = shellCommand;
        m_remote->factory->quicHost = quicHost;
        m_remote->factory->quicBootstrapCommand = quicBootstrapCommand;
        m_remote->workspaceName = workspaceName;
        m_remote->endsWhenDetached = endsWhenDetached;
        return startRemote(reattachSessionID);
    }

    /// Describes whether an attach created a projection or found one already open.
    enum class RemoteWorkspaceAttachOutcome : std::uint8_t {
        Started,
        AlreadyAttached,
        Rejected
    };

    /// Attaches using an authority and stable workspace identity before creating a carrier.
    RemoteWorkspaceAttachOutcome attachRemote(const std::string& shellCommand, const std::string& quicHost,
        const std::string& quicBootstrapCommand, const Session::WorkspaceAttachRequest& request,
        const std::string& workspaceName, bool endsWhenDetached, // bool-ok: mirrors the HELLO lifetime flag
        PostedWorkLoop loop)
    {
        if (hasUnresolvedLocalAdoption() || !modelTerm() || m_remote || !loop.isComplete()) {
            return RemoteWorkspaceAttachOutcome::Rejected;
        }
        PostedDispatcher dispatcher(loop);
        auto attachRequest = request;
        attachRequest.workspaceName = workspaceName;
        auto acquired = remoteWorkspaceRegistry().acquire(attachRequest, [&] {
            auto factory = std::make_unique<RemoteParts::PlainChannelFactory>();
            factory->dispatcher = dispatcher;
            factory->shellCommand = Session::substituteReattachSocket(shellCommand, request.authority.daemonSocket);
            factory->quicHost = quicHost;
            factory->quicBootstrapCommand = Session::substituteReattachSocket(quicBootstrapCommand, request.authority.daemonSocket);
            if (!factory->pinDaemonSocket(request.authority.daemonSocket)) {
                return std::shared_ptr<Session::WorkspaceConnection> {};
            }
            if (m_pendingResumeReach)
                (void)factory->seedResumeReach(*m_pendingResumeReach);
            auto owner = std::make_shared<Session::WorkspaceConnection>(std::make_unique<PostedDispatcher>(dispatcher), std::move(factory));
            owner->connection().setAttachWorkspaceID(request.workspaceID);
            owner->connection().setExpectedDaemonInstanceID(request.authority.daemonInstanceID);
            owner->connection().setWorkspace(workspaceName, endsWhenDetached);
            owner->connection().setReattachPolicy(request.reattachPolicy);
            owner->connection().setAuthToken(m_remoteAuthToken);
            // A prepared protected owner joins the connection it was prepared
            // for before its first carrier; a reach a previous process kept
            // makes that carrier a resume proof.
            if (m_preparedRemote && !owner->connection().adoptPreparedResume(std::move(m_preparedRemote)))
                return std::shared_ptr<Session::WorkspaceConnection> {};
            if (m_remoteClientInstance)
                owner->connection().setClientInstance(*m_remoteClientInstance);
            owner->connection().setClipboardServing(Preferences::Effective::clipboardServing(m_configuration));
            owner->connection().setHostLocation(hostLocationOfPlan(shellCommand, quicHost, quicBootstrapCommand));
            return owner;
        });
        // The seed belongs to this attach alone, adopted or not.
        m_pendingResumeReach.reset();
        if (!acquired) {
            m_preparedRemote.reset();
            m_restoredPreview.reset();
            return RemoteWorkspaceAttachOutcome::Rejected;
        }
        if (acquired->wasReused && m_preparedRemote) {
            // A workspace this process already carries has its own attachment;
            // the prepared one, and the picture it drew, are let go of.
            m_preparedRemote.reset();
            m_restoredPreview.reset();
        }
        auto owner = acquired->connection;
        uint32_t paneSessionID = request.sessionID;
        if (acquired->wasReused) {
            if (!paneSessionID) {
                paneSessionID = owner->connection().sessionID();
                if (!owner->containsSession(paneSessionID) && !owner->connection().workspacePaneSessions().empty()) {
                    paneSessionID = owner->connection().workspacePaneSessions().front().second;
                }
            }
            if (!owner->containsSession(paneSessionID)) {
                return owner->connection().acknowledged() ? RemoteWorkspaceAttachOutcome::Rejected : RemoteWorkspaceAttachOutcome::AlreadyAttached;
            }
            if (!owner->canAttachSession(paneSessionID)) {
                return RemoteWorkspaceAttachOutcome::AlreadyAttached;
            }
        }
        auto remote = std::make_unique<RemoteParts>();
        remote->workspaceConnection = std::move(owner);
        remote->authority = request.authority;
        remote->requestedWorkspaceID = request.workspaceID;
        remote->requestedSessionID = request.sessionID;
        remote->workspaceName = workspaceName;
        remote->dispatcher = dispatcher;
        remote->observer.session = this;
        remote->core = std::make_unique<Session::RemoteSessionCore>(modelTerm(), *remote->workspaceConnection, &remote->observer);
        readHostEffectDecisions(*remote->core);
        if (m_pixelGeometry)
            remote->core->setPixelGeometry(*m_pixelGeometry);
        if (m_networkCost)
            remote->core->setNetworkCost(*m_networkCost);
        m_remote = std::move(remote);
        armHostUpdateBeforeAttach();
        const auto dimensions = m_runtime->dimensions();
        m_remote->core->start(paneSessionID, dimensions.columns, dimensions.rows);
        m_measuredFacesKey = {};
        publishProportionalFaces();
        return RemoteWorkspaceAttachOutcome::Started;
    }

    /// Matches an already projected workspace without exposing backend transport logic.
    [[nodiscard]] bool matchesRemoteWorkspace(const Session::WorkspaceAttachRequest& request) const
    {
        if (!m_remote || !m_remote->workspaceConnection
            || m_remote->workspaceConnection->connection().state() == Session::State::Failed) {
            return false;
        }
        auto authority = m_remote->authority;
        authority.daemonInstanceID = request.authority.daemonInstanceID ? m_remote->workspaceConnection->connection().daemonInstanceID() : 0;
        if (!request.authority.daemonSocket.empty() && authority.daemonSocket.empty()) {
            authority.daemonSocket = m_remote->workspaceConnection->connection().socketName();
        }
        if (authority != request.authority) {
            return false;
        }
        if (request.workspaceID) {
            const auto actual = m_remote->workspaceConnection->connection().workspaceID();
            return (actual ? actual : m_remote->requestedWorkspaceID) == request.workspaceID;
        }
        if (request.sessionID) {
            return m_remote->workspaceConnection->containsSession(request.sessionID)
                || (!m_remote->workspaceConnection->connection().workspace() && request.sessionID == m_remote->requestedSessionID);
        }
        const auto& connection = m_remote->workspaceConnection->connection();
        return !request.workspaceName.empty()
            && request.workspaceName == (connection.workspace() ? connection.workspaceName() : m_remote->workspaceName);
    }

    /// Attaches another pane of the source session's authoritative workspace.
    ///
    /// Shares the physical carrier, topology mirror, and reconnect policy across windows.
    /// Foreign or unknown pane IDs leave this session unchanged.
    ///
    /// - Parameters:
    ///   - source: Attached session that owns the shared workspace connection.
    ///   - sessionID: Authoritative pane session identity to project.
    /// - Returns: Whether the pane joined the shared connection.
    bool attachRemoteSibling(TerminalSession& source, std::uint32_t sessionID)
    {
        if (hasUnresolvedLocalAdoption() || !modelTerm() || m_remote || !source.m_remote || !source.m_remote->workspaceConnection
            || !source.m_remote->workspaceConnection->canAttachSession(sessionID)) {
            return false;
        }
        // The pane opens dressed as the one it opens from, so the frames and
        // chrome before its first checkpoint are already the session's theme.
        (void)seedPaletteFrom(source);
        auto remote = std::make_unique<RemoteParts>();
        remote->workspaceConnection = source.m_remote->workspaceConnection;
        remote->authority = source.m_remote->authority;
        remote->requestedWorkspaceID = source.m_remote->requestedWorkspaceID;
        remote->requestedSessionID = source.m_remote->requestedSessionID;
        remote->workspaceName = source.m_remote->workspaceName;
        remote->dispatcher = source.m_remote->dispatcher;
        remote->observer.session = this;
        remote->core = std::make_unique<Ubiquitty::Session::RemoteSessionCore>(
            modelTerm(), *remote->workspaceConnection, &remote->observer);
        readHostEffectDecisions(*remote->core);
        if (m_pixelGeometry)
            remote->core->setPixelGeometry(*m_pixelGeometry);
        if (m_networkCost)
            remote->core->setNetworkCost(*m_networkCost);
        m_remote = std::move(remote);
        const DimensionSnapshot dimensions = m_runtime->dimensions();
        m_remote->core->start(sessionID, dimensions.columns, dimensions.rows);
        m_measuredFacesKey = {};
        publishProportionalFaces();
        return true;
    }

    /// Dresses this pane in another session's theme before it is attached.
    ///
    /// A hosted pane has nothing of its own to show until its host's first
    /// checkpoint, and the theme it will then carry is the one the session
    /// it opens beside already shows. The reset baseline is copied rather
    /// than the live palette: a program's OSC override belongs to that pane,
    /// not to a fresh shell. The checkpoint carries the host's palette and
    /// replaces this one whole.
    ///
    /// - Parameter source: The session the pane opens from.
    /// - Returns: Whether both models existed and this one is not yet attached.
    [[nodiscard]] bool seedPaletteFrom(TerminalSession& source)
    {
        if (m_remote || &source == this) {
            return false;
        }
        TermPalette baseline;
        std::string themeName;
        {
            const std::scoped_lock hold(*source.m_activeLock);
            const Ubiquitty::Terminal* term = source.modelTerm();
            if (!term) {
                return false;
            }
            baseline = term->paletteBaseline;
        }
        {
            const std::scoped_lock hold(source.m_themeIdentityLock);
            themeName = source.m_chosenThemeName;
        }
        {
            const std::scoped_lock hold(*m_activeLock);
            Ubiquitty::Terminal* term = modelTerm();
            if (!term) {
                return false;
            }
            term->palette = baseline;
            term->paletteBaseline = baseline;
            Themes::rethemeDefaults(term);
            republishModel();
        }
        const std::scoped_lock identityHold(m_themeIdentityLock);
        m_chosenThemeName = std::move(themeName);
        m_themeIdentity.reset();
        return true;
    }

    /// Reports whether two sessions project the same physical workspace connection.
    [[nodiscard]] bool sharesRemoteWorkspaceConnection(const TerminalSession& other) const
    {
        return m_remote && other.m_remote && m_remote->workspaceConnection
            && m_remote->workspaceConnection == other.m_remote->workspaceConnection;
    }

    /// Attaches as a thin client over a carrier the EMBEDDER builds.
    ///
    /// The same session as ``attachRemote`` in every respect that reaches the wire — the same
    /// `RemoteSessionCore`, the same Sync client, the same reattach id, the same clipboard-serving
    /// latch — differing only in where the carrier comes from.
    ///
    /// ``attachRemote``'s factory obtains one by spawning: its QUIC arm runs the bootstrap command
    /// as a subprocess to learn the daemon's port, token and certificate pin, and its fallback
    /// spawns the transport command itself. A frontend that cannot spawn a process can use neither,
    /// while the carriers themselves are portable — a `QUICChannel` dials directly and forks
    /// nothing. This is the door for that frontend: it supplies the factory, and everything above
    /// the carrier is unchanged.
    ///
    /// - Parameters:
    ///   - factoryAddress: The embedder's `Ubiquitty::Session::ChannelFactory`, as an address
    ///     because the languages this seam serves cannot name the type. Borrowed, and it must
    ///     outlive the session: the core calls it again on every reconnect attempt, not once.
    ///   - reattachSessionID: As ``attachRemote``.
    ///   - loop: As ``attachRemote``.
    /// - Returns: Whether the attach started.
    bool attachRemoteOverCarrier(std::uintptr_t factoryAddress, std::uint32_t reattachSessionID, PostedWorkLoop loop) // bool-ok: mirrors the single HELLO flag bit helloSessionEndWhenDetached; an enum here would be a second spelling of one wire bit
    {
        return attachRemoteOverCarrier(factoryAddress, reattachSessionID, std::string(), false, std::string(), 0, loop);
    }

    /// Attaches over an embedder's carrier, naming the workspace to attach to
    /// or create.
    ///
    /// ``attachRemoteOverCarrier`` in every respect but the name: a saved host
    /// that carries one reaches the session bearing it if the daemon holds one,
    /// and creates one under it otherwise — one connect, either outcome. An
    /// empty name is the unnamed attach, which is what the shorter door sends.
    ///
    /// - Parameters:
    ///   - factoryAddress: As ``attachRemoteOverCarrier``.
    ///   - reattachSessionID: As ``attachRemoteOverCarrier``. A reattach carries
    ///     the name too and the daemon ignores it, because a reattach observes
    ///     what its session already is.
    ///   - workspaceName: The workspace to attach under; empty attaches unnamed.
    ///   - endsWhenDetached: Whether a session created under the name ends when
    ///     its last human detaches cleanly.
    ///   - daemonSocketName: The directory's daemon basename, or empty for an unbound attach.
    ///   - daemonInstanceID: The directory's daemon process identity, or zero for an unbound attach.
    ///   - loop: As ``attachRemoteOverCarrier``.
    /// - Returns: Whether the attach started.
    bool attachRemoteOverCarrier(std::uintptr_t factoryAddress, std::uint32_t reattachSessionID, const std::string& workspaceName, bool endsWhenDetached, const std::string& daemonSocketName, std::uint64_t daemonInstanceID, PostedWorkLoop loop) // bool-ok: mirrors the single HELLO flag bit helloSessionEndWhenDetached; an enum here would be a second spelling of one wire bit
    {
        if (!factoryAddress || daemonSocketName.size() > std::numeric_limits<std::uint16_t>::max()
            || daemonSocketName.empty() != (daemonInstanceID == 0))
            return false;
        const bool isSocketNameValid = std::ranges::all_of(daemonSocketName, [](unsigned char character) {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
        });
        if (!isSocketNameValid)
            return false;
        if (!beginRemote(loop, factoryAddress))
            return false;
        if (!daemonSocketName.empty() && !m_remote->activeFactory->pinDaemonSocket(daemonSocketName)) {
            m_remote.reset();
            return false;
        }
        m_remote->authority.daemonSocket = daemonSocketName;
        m_remote->authority.daemonInstanceID = daemonInstanceID;
        m_remote->workspaceName = workspaceName;
        m_remote->endsWhenDetached = endsWhenDetached;
        return startRemote(reattachSessionID);
    }

    /// Whether this session permits serving its machine's clipboard to the host.
    [[nodiscard]] bool isServingClipboardToHost() const
    {
        return m_remote && m_remote->core && m_remote->core->didClaimClipboardServe();
    }

    [[nodiscard]] bool canChangeClipboardServing() const
    {
        return m_remote && m_remote->core && m_remote->core->canChangeClipboardServing();
    }

    [[nodiscard]] bool isClipboardRoutingPending() const
    {
        return m_remote && m_remote->core && m_remote->core->isClipboardRoutingPending();
    }

    [[nodiscard]] bool setClipboardServing(bool enabled)
    {
        if (!m_remote || !m_remote->core) {
            return false;
        }
        if (enabled) {
            const std::scoped_lock hold(*m_activeLock);
            if (modelTerm() && modelTerm()->osc52ClipboardReadPolicy == OSC::ClipboardReadPolicy::Deny) {
                return false;
            }
        }
        return m_remote->core->setClipboardServing(enabled ? Clipboard::Serving::Enabled : Clipboard::Serving::Disabled);
    }

    /// Whether this session is a thin client rather than a local PTY.
    ///
    /// - Returns: Whether ``attachRemote`` holds the model.
    [[nodiscard]] bool isRemoteAttached() const { return m_remote != nullptr; }

    /// Whether a current authenticated attachment other than this one owns
    /// the remote pane's geometry. Local and unacknowledged sessions return false.
    [[nodiscard]] bool isRemoteGeometryOwnedElsewhere() const { return m_remote && m_remote->core && m_remote->core->isGeometryOwnedElsewhere(); }

    // ---- sharing into the local daemon ----
    //
    // "Share This Window": the local ubiquitty-host daemon adopts this
    // session's PTY — its duplicate of the master becomes the one reader — and
    // this session becomes a thin client of the adopted session, so an agent
    // the human consents to can attach to the terminal in use. The shell stays
    // this process's child and the stream that owns the master stays open,
    // unread: a reclaim hands the session back and the same stream resumes,
    // with no reconnect. The master is never closed here, because the stream
    // owns it and a second close would close whatever reused the number.

    /// Whether this session can be shared into the local daemon.
    ///
    /// A PTY this process launched, whose child is still running, and which is
    /// none of the things that have no master of their own to hand over: a thin
    /// client already, a hosted tmux pane, a pane routed through a tmux control
    /// channel, or a session hosting a program's windows.
    ///
    /// - Returns: Whether ``shareIntoLocalDaemon`` would try.
    [[nodiscard]] bool isShareableLocalSession()
    {
        if (m_remote || m_sharing || m_externalTerminal || m_hostedPaneSource || m_tmuxPrimaryStream || m_hostedWindows)
            return false;
        PAL::Stream* stream = m_runtime->stream();
        return stream != nullptr && stream->fileDescriptor() >= 0 && stream->process().hasRunningChild();
    }

    /// Hands this session's PTY to the local daemon a ubiquitty-host binary
    /// owns, and attaches nothing to it.
    ///
    /// Every decision is here, so a frontend only words the outcome: the
    /// daemon's socket is the binary's own (``Session::localDaemonSocketPath``)
    /// or there is none, the daemon is started when nothing answers there, and
    /// the local reader stops before the master crosses — so every byte this
    /// side read is in the snapshot the daemon is seeded from and every byte
    /// after it is the daemon's. A proven refusal restores the local reader;
    /// an unresolved Commit retains it inactive until explicit resolution.
    ///
    /// A committed pane has no thin client yet, and takes no input until it
    /// has one: ``attachSharedLocalSession`` gives it a connection of its own,
    /// ``attachSharedLocalWindow`` makes it the one its window's other panes
    /// ride, and ``joinSharedLocalWindow`` puts it on such a connection.
    ///
    /// - Parameters:
    ///   - hostBinaryPath: The ubiquitty-host to reach the daemon through.
    ///   - handoffAddress: A ``Session::AdoptHandoff``, as an address.
    /// - Returns: How the handoff ended.
    Session::ShareOutcome commitShareIntoLocalDaemon(const std::string& hostBinaryPath, std::uintptr_t handoffAddress)
    {
        if (!isShareableLocalSession() || handoffAddress == 0)
            return Session::ShareOutcome::NotShareable;
        const std::string socketPath = Session::localDaemonSocketPath(hostBinaryPath);
        if (socketPath.empty())
            return Session::ShareOutcome::NoDaemonSocket;
        if (!Session::ensureLocalDaemon(hostBinaryPath, socketPath))
            return Session::ShareOutcome::DaemonDidNotStart;
        const Handoff handoff = handOffToLocalDaemon(socketPath, handoffAddress);
        if (handoff.ownership == Session::ShareOwnership::Unresolved) {
            m_localAdoption->bridgeCommand = Session::localBridgeCommand(Transport::shellSingleQuote(hostBinaryPath));
            return Session::ShareOutcome::Unresolved;
        }
        if (handoff.encoding == Session::ShareEncoding::TooLong)
            return Session::ShareOutcome::TooLongToShare;
        if (handoff.sessionID == 0)
            return handoff.rejection == Session::AdoptRejection::HostFull ? Session::ShareOutcome::HostFull : Session::ShareOutcome::HandoffRefused;
        m_sharing->bridgeCommand = Session::localBridgeCommand(Transport::shellSingleQuote(hostBinaryPath));
        return Session::ShareOutcome::Shared;
    }

    /// Shares this session into the local daemon a ubiquitty-host binary owns,
    /// and becomes the adopted session's thin client over a connection of its
    /// own.
    ///
    /// ``commitShareIntoLocalDaemon``, then ``attachSharedLocalSession``
    /// through the bridge the same binary serves.
    ///
    /// - Parameters:
    ///   - hostBinaryPath: As ``commitShareIntoLocalDaemon``.
    ///   - handoffAddress: As ``commitShareIntoLocalDaemon``.
    ///   - loop: As ``attachRemote``.
    /// - Returns: How the share ended.
    Session::ShareOutcome shareIntoLocalDaemon(const std::string& hostBinaryPath, std::uintptr_t handoffAddress,
        PostedWorkLoop loop)
    {
        if (!loop.isComplete())
            return Session::ShareOutcome::NotShareable;
        const Session::ShareOutcome outcome = commitShareIntoLocalDaemon(hostBinaryPath, handoffAddress);
        if (outcome != Session::ShareOutcome::Shared)
            return outcome;
        (void)attachSharedLocalSession(0, loop);
        noteRemoteEvent(EventKind::RemoteStateChanged, static_cast<std::uint32_t>(remoteState()));
        return Session::ShareOutcome::Shared;
    }

    /// Shares this session into the local daemon over a carrier the embedder
    /// builds.
    ///
    /// ``shareIntoLocalDaemon`` from the handoff on, differing in where the
    /// thin client's carrier comes from, for the reason
    /// ``attachRemoteOverCarrier`` gives: the embedder names the daemon's
    /// socket and has already brought that daemon up.
    ///
    /// - Parameters:
    ///   - socketPath: The daemon's control socket.
    ///   - handoffAddress: As ``shareIntoLocalDaemon``.
    ///   - factoryAddress: As ``attachRemoteOverCarrier``.
    ///   - loop: As ``attachRemote``.
    /// - Returns: The adopted session's id, or zero. A zero with
    ///   ``hasUnresolvedLocalAdoption`` retains the inactive local owner.
    std::uint32_t shareIntoLocalDaemonOverCarrier(const std::string& socketPath, std::uintptr_t handoffAddress, std::uintptr_t factoryAddress,
        PostedWorkLoop loop)
    {
        if (factoryAddress == 0 || !loop.isComplete())
            return 0;
        const std::uint32_t sessionID = handOffToLocalDaemon(socketPath, handoffAddress).sessionID;
        if (sessionID == 0)
            return 0;
        (void)beginRemote(loop, factoryAddress);
        (void)startRemote(sessionID);
        noteRemoteEvent(EventKind::RemoteStateChanged, static_cast<std::uint32_t>(remoteState()));
        return sessionID;
    }

    /// Hands this session's PTY to the local daemon and attaches nothing to
    /// it: the re-home a quit makes.
    ///
    /// The application is ending, so no window is left to be the adopted
    /// session's client. The daemon's duplicate of the master keeps the shell
    /// running once this process is gone, and the id returned is what the next
    /// launch rejoins it by. The reader stops as it does for a share, so the
    /// daemon is the one reader from the handoff on.
    ///
    /// - Parameters:
    ///   - socketPath: The daemon's control socket, already answering.
    ///   - handoffAddress: A ``Session::AdoptHandoff``, as an address.
    /// - Returns: The adopted session's id, or zero. An unresolved zero requires
    ///   cancelling quit while the original owner awaits an exact outcome.
    std::uint32_t handOverToLocalDaemon(const std::string& socketPath, std::uintptr_t handoffAddress)
    {
        return handOffToLocalDaemon(socketPath, handoffAddress).sessionID;
    }

    /// Whether close or normal application exit must await an exact outcome.
    [[nodiscard]] bool hasUnresolvedLocalAdoption() const { return m_runtime->hasUnresolvedLocalAdoption(); }

    /// Whether teardown can dispose this session's stream and terminal.
    [[nodiscard]] bool canClose() const { return !hasUnresolvedLocalAdoption(); }

    /// Counts process-owned attempts that forbid normal application termination.
    /// Every frontend must consult this before allowing ordinary process exit.
    [[nodiscard]] static std::uint32_t unresolvedLocalAdoptionCount()
    {
        return static_cast<std::uint32_t>(LocalAdoptionEscrow::singleton().unresolvedCount());
    }

    /// Identifies the retained session for explicit resolution or reopening.
    [[nodiscard]] std::uint64_t localAdoptionEscrowID() const { return m_localAdoption ? m_localAdoption->escrowID : 0; }

    /// Resolves the same operation once, under the platform's finite work bounds.
    /// No transport error, Unknown, Prepared, or changed daemon resumes local I/O.
    /// - Parameter resolveAddress: A ``Session::AdoptResolve``, as an address.
    /// - Returns: Proven ownership, or Unresolved with the original owner retained.
    [[nodiscard]] Session::ShareOwnership resolveLocalAdoption(std::uintptr_t resolveAddress)
    {
        return resolveLocalAdoptionWithResume(resolveAddress, LocalAdoptionResume::LiveView);
    }

    /// Records an owned bridge command without retaining a frontend context.
    void setLocalAdoptionBridgeCommand(const std::string& command)
    {
        if (m_localAdoption)
            m_localAdoption->bridgeCommand = command;
        if (m_sharing)
            m_sharing->bridgeCommand = command;
    }

    /// Reports the prior ingest intent for an explicitly reopened local owner.
    /// Install fresh frontend callbacks before restarting its ingest loop.
    [[nodiscard]] bool shouldResumeLocalAdoptionIngest() const
    {
        return m_localAdoption && m_localAdoption->shouldResumeIngest;
    }

    /// Attaches a confirmed adoption using a currently live frontend dispatcher.
    /// Resolution retains no borrowed dispatcher or view context across failure.
    /// - Parameters:
    ///   - factoryAddress: A current carrier factory, or zero for the owned bridge.
    ///   - loop: As ``attachRemote``.
    /// - Returns: Whether the adopted session's thin client started.
    [[nodiscard]] bool attachSharedLocalSession(std::uintptr_t factoryAddress, PostedWorkLoop loop)
    {
        if (!m_sharing || hasUnresolvedLocalAdoption() || !loop.isComplete()
            || (!factoryAddress && m_sharing->bridgeCommand.empty())
            || !beginRemote(loop, factoryAddress))
            return false;
        if (!factoryAddress)
            m_remote->factory->shellCommand = m_sharing->bridgeCommand;
        // What a later attach of the same workspace is matched against: the
        // bridge that reaches the daemon, and the daemon process itself.
        m_remote->authority = Session::WorkspaceAuthority { .endpointIdentity = m_sharing->bridgeCommand,
            .principal = {},
            .daemonSocket = {},
            .daemonInstanceID = m_sharing->daemonInstanceID };
        m_remote->requestedSessionID = m_sharing->sessionID;
        return startRemote(m_sharing->sessionID);
    }

    /// Attaches a committed pane as the one connection every other
    /// committed pane of its shared window rides.
    ///
    /// ``attachSharedLocalSession``, and this session keeps the window's
    /// account: each pane ``joinSharedLocalWindow`` names it for becomes an
    /// endpoint of this connection once the host places that pane's session
    /// in this connection's workspace, which publishing the window through
    /// this session does (``publishSharedWorkspace``). Once the host has
    /// confirmed the workspace and this connection follows it, the connection
    /// keeps to that workspace across reconnects, and an attach of the same
    /// workspace by the same authority in this process reuses it.
    ///
    /// The connection is shared, not this session's: closing or reclaiming
    /// this pane leaves it serving the panes that joined it.
    ///
    /// - Parameters:
    ///   - factoryAddress: As ``attachSharedLocalSession``. An embedder's
    ///     factory must outlive every pane that joins.
    ///   - loop: As ``attachSharedLocalSession``.
    /// - Returns: Whether this pane's thin client started.
    [[nodiscard]] bool attachSharedLocalWindow(std::uintptr_t factoryAddress, PostedWorkLoop loop)
    {
        if (!attachSharedLocalSession(factoryAddress, loop) || !m_remote
            || !m_remote->workspaceConnection)
            return false;
        return anchorSharedLocalWindow(0);
    }

    /// Makes an attached pane of a shared window the one whose connection the
    /// window's other panes ride.
    ///
    /// The pane ``attachSharedLocalWindow`` attached is one already. A pane on
    /// a connection of its own becomes one when the window's panes rode
    /// several connections before their workspace was confirmed: a share
    /// retried after some panes attached on their own, a quit cancelled after
    /// its hand-over, an unresolved handoff resolved later. The other panes
    /// then move onto its connection (``joinSharedLocalWindow``), and the
    /// connection keeps to the workspace and is indexed for it as an anchor's
    /// is.
    ///
    /// - Parameter workspaceID: The workspace the host confirmed for the
    ///   window, or zero to take it from the host's answer to a publication
    ///   through this pane.
    /// - Returns: Whether this pane anchors the window.
    [[nodiscard]] bool anchorSharedLocalWindow(std::uint32_t workspaceID)
    {
        if (!m_sharing || hasUnresolvedLocalAdoption() || !m_remote || !m_remote->core || !m_remote->workspaceConnection)
            return false;
        if (!m_remote->sharedWindow) {
            m_remote->sharedWindow = std::make_unique<RemoteParts::SharedWindow>();
            subscribeSharedWindow();
        }
        if (workspaceID && !m_remote->sharedWindow->confirmedWorkspaceID)
            m_remote->sharedWindow->confirmedWorkspaceID = workspaceID;
        serviceSharedWindow();
        return m_remote && m_remote->sharedWindow;
    }

    /// Reports whether this pane rides the connection this process indexes
    /// for its workspace: the one an attach of that workspace by the same
    /// authority reuses, and the one other connections of it retire into.
    [[nodiscard]] bool isOnIndexedWorkspaceConnection() const
    {
        if (!m_remote || !m_remote->workspaceConnection)
            return false;
        return remoteWorkspaceRegistry().indexed(m_remote->authority, *m_remote->workspaceConnection) == m_remote->workspaceConnection;
    }

    /// How a committed pane of a shared window stands toward its window's
    /// connection.
    enum class SharedWindowJoin : std::uint8_t {
        /// Its endpoint rides the anchor's connection.
        Joined,
        /// The host has not yet placed its session in the anchor's
        /// workspace, or the pane is moving there from a connection of its
        /// own. It joins with no further call: inside the topology update
        /// that places it, or once it can move.
        Awaiting,
        /// It may not ride that connection, and needs one of its own.
        Refused,
    };

    /// Joins a committed pane to the connection its window's anchor
    /// attached (``attachSharedLocalWindow``), as one more endpoint of it.
    ///
    /// The pane keeps its own share and reclaim, its model and its
    /// presentation; what it shares is the carrier, the attachment and the
    /// reconnect. It joins only a connection to the daemon process its own PTY
    /// went to, reached through the same bridge
    /// (``Session::isSameLocalShareAuthority``), and only one posting to the
    /// same event loop: that connection outlives the anchor and posts every
    /// pane's work to the anchor's loop. An unresolved handoff joins nothing.
    ///
    /// A pane already on a connection of its own, to the same daemon by the
    /// same bridge, moves onto the anchor's once both carry the window's
    /// workspace: its connection retires into the anchor's
    /// (`Session::WorkspaceConnection::retireInto`), and the pane moves when
    /// its input has settled and its questions have been answered there, and
    /// the anchor's connection holds its picture. Its old attachment detaches
    /// after its last pane has gone.
    ///
    /// - Parameters:
    ///   - anchor: The pane whose connection the window's panes ride.
    ///   - loop: As ``attachRemote``; the anchor's own.
    /// - Returns: Whether the pane rides the anchor's connection now, will once
    ///   the host places it, or may not.
    [[nodiscard]] SharedWindowJoin joinSharedLocalWindow(TerminalSession& anchor, PostedWorkLoop loop)
    {
        if (sharesRemoteWorkspaceConnection(anchor))
            return SharedWindowJoin::Joined;
        if (m_remote) {
            if (!mayMoveIntoSharedWindow(anchor, loop))
                return SharedWindowJoin::Refused;
            (void)retireIntoSharedWindow(anchor);
        } else {
            if (!mayRideSharedWindow(anchor, loop))
                return SharedWindowJoin::Refused;
            if (rideSharedWindow(anchor, loop))
                return SharedWindowJoin::Joined;
        }
        auto& awaiting = anchor.m_remote->sharedWindow->awaiting;
        if (std::ranges::none_of(awaiting, [this](const RemoteParts::SharedWindow::Member& member) { return member.session.get() == this; })) {
            if (awaiting.size() >= RemoteParts::SharedWindow::maxAwaiting)
                return SharedWindowJoin::Refused;
            awaiting.push_back({ .session = weakPtr(), .loop = loop });
        }
        anchor.scheduleSharedWindowRetry();
        return SharedWindowJoin::Awaiting;
    }

    /// Resolves a retained session even after its view released its last owner.
    /// The caller supplies the resolver; the registry starts no background work.
    [[nodiscard]] static Session::ShareOwnership resolveLocalAdoptionEscrow(std::uint64_t id, std::uintptr_t resolveAddress)
    {
        auto owner = LocalAdoptionEscrow::singleton().retain(id);
        return owner ? (*owner)->resolveLocalAdoptionWithResume(resolveAddress, LocalAdoptionResume::RetainedWithoutView)
                     : Session::ShareOwnership::Unresolved;
    }

    /// Transfers one proven-local escrow owner through the Swift factory seam.
    /// The returned reference must be reopened or explicitly closed and released.
    /// An unresolved owner is never transferred or discarded by this operation.
    [[nodiscard]] static TerminalSession* takeResolvedLocalAdoption(std::uint64_t id) UBIQUITTY_RETURNS_RETAINED
    {
        auto owner = LocalAdoptionEscrow::singleton().takeLocal(id);
        if (!owner)
            return nullptr;
        if ((*owner)->m_localAdoption)
            (*owner)->m_localAdoption->escrowID = 0;
        return owner->release();
    }

    /// Whether the local daemon drives this session's PTY on its behalf.
    ///
    /// True from a share until the reclaim that hands it back, whatever the
    /// connection to the daemon is doing in between.
    [[nodiscard]] bool isSharedIntoLocalDaemon() const { return m_sharing.has_value(); }

    /// The id the local daemon adopted this session's PTY under.
    ///
    /// The daemon's answer to the handoff, so it is known before the thin
    /// client's handshake names the session, and after a quit's hand-over,
    /// which attaches no client at all.
    ///
    /// - Returns: The id, or zero while the session is not shared.
    [[nodiscard]] std::uint32_t sharedSessionID() const { return m_sharing ? m_sharing->sessionID : 0; }

    /// Publishes adopted panes as one window, on the daemon this session is
    /// attached to.
    ///
    /// A shared window publishes once every pane's handoff has committed. A
    /// window restored after a quit that handed a split over publishes the
    /// panes that quit handed over, which the daemon holds in a workspace
    /// each until then. The daemon decides whether this connection may: it
    /// admits only a trusted local person attached to one of the sessions.
    ///
    /// - Parameter workspace: The arrangement and its sessions in leaf order.
    /// - Returns: Whether the request was sent; the answer arrives as
    ///   ``sharedWorkspaceResult``.
    [[nodiscard]] bool publishSharedWorkspace(const Session::SharedWorkspace& workspace)
    {
        return m_remote && m_remote->core && m_remote->core->publishSharedWorkspace(workspace);
    }

    /// Reports publication independently of per-pane ownership transfer.
    [[nodiscard]] Session::SharedWorkspaceResult sharedWorkspaceResult() const
    {
        return m_remote && m_remote->core ? m_remote->core->sharedWorkspaceResult() : Session::SharedWorkspaceResult {};
    }

    /// The command that reaches the daemon this session was shared into —
    /// what a saved session stores to rejoin it.
    ///
    /// - Returns: The bridge the ubiquitty-host binary serves, or empty while
    ///   the session is not shared and for a share over an embedder's carrier.
    [[nodiscard]] std::string sharedBridgeCommand() const { return m_sharing ? m_sharing->bridgeCommand : std::string(); }

    /// Whether ``requestReclaim`` would ask the daemon for this session back.
    ///
    /// A session this one shared, attached to the adopted session and
    /// connected. A thin client that attached any other way has no PTY of its
    /// own to resume.
    [[nodiscard]] bool isReclaimableSharedSession() const
    {
        return m_sharing && m_remote && m_remote->core && m_remote->core->sessionID() != 0
            && m_remote->core->state() == Ubiquitty::Session::State::Connected;
    }

    /// Asks the daemon to hand the shared session back.
    ///
    /// The daemon's answer stops it driving the PTY; the thin client ends,
    /// the retained stream resumes where the daemon left it, and
    /// ``EventKind::RemoteStateChanged`` reports the connection gone to Idle,
    /// with ``isSharedIntoLocalDaemon`` false beside it. Asking
    /// twice is harmless: the daemon answers for a session it still holds, and
    /// the first answer takes it away. A pane of a shared window waits behind
    /// another pane's request on the same connection, since the daemon
    /// settles one per attachment at a time.
    ///
    /// - Returns: Whether the request was sent or is waiting its turn.
    bool requestReclaim()
    {
        if (!isReclaimableSharedSession())
            return false;
        return m_remote->core->requestReclaim();
    }

    /// The largest payload the remote file-paste channel accepts.
    [[nodiscard]] static std::size_t pastePushByteLimit() { return Protocol::Control::maxPastePushBytes; }

    /// Sends a pasted payload the host can only receive as a file.
    ///
    /// An image on the clipboard has no spelling as input bytes. The host
    /// lands this one as a file and types its path, which is what a program
    /// taking a path wants. A local session has nothing to send it to: the
    /// clipboard is already on the same machine as the program.
    ///
    /// - Parameters:
    ///   - mime: The payload's MIME name.
    ///   - bytes: The payload.
    /// - Returns: The push's id, or zero when nothing was sent.
    std::uint32_t pushPaste(const std::string& mime, PayloadSpan bytes UBIQUITTY_NOESCAPE)
    {
        return m_remote && m_remote->core ? m_remote->core->pushPaste(mime, bytes) : 0;
    }

    /// Tells a remote session the device's network path moved.
    ///
    /// The frontend's job, because only it can see the event: Core has no
    /// reachability API and a portable one would be three platform backends
    /// behind this call. What the core does with it is its own — a QUIC carrier
    /// migrates and is left alone, anything else is torn down and redialled
    /// without waiting out the backoff a dead path would otherwise sit through.
    ///
    /// A no-op on a local session and on a session that has not attached, so a
    /// frontend may call it on every path change without asking what it holds.
    void networkPathChanged()
    {
        if (m_remote && m_remote->core)
            m_remote->core->networkPathChanged();
    }

    /// Tells a remote session what the device's network path costs.
    ///
    /// The frontend's report, as a path move is: only the platform can see
    /// the path. Core states it to the host as `Facts.link`, which keeps a
    /// metered link from being pushed history it did not ask for. Kept for a
    /// remote session this one starts later; a frontend with no source never
    /// calls it, and the link stays unmetered.
    ///
    /// - Parameter cost: The path's cost.
    void setNetworkCost(PAL::NetworkCost cost)
    {
        m_networkCost = cost;
        if (m_remote && m_remote->core)
            m_remote->core->setNetworkCost(cost);
    }

    /// Reports that this pane is on screen. For a remote pane this decides
    /// whether the host sends it a screen at all: a pane in a background tab,
    /// a miniaturized window or a backgrounded app is `Dormant`, sent only a
    /// `PaneDirty` the first time it changes, and waking it costs the rows that did
    /// (Docs/sync.md §6.8).
    ///
    /// `showRemotePane` makes it `Live`; `hideRemotePane` makes it `Dormant`.
    /// Local panes keep parsing while dormant and coalesce link notifications
    /// until they are shown; no transport traffic is generated.
    void showRemotePane()
    {
        paneLink().setView(Protocol::Sync::ViewState::Live);
    }

    /// Reports that this pane left the screen, which makes it
    /// `Dormant`; see `showRemotePane`.
    void hideRemotePane()
    {
        paneLink().setView(Protocol::Sync::ViewState::Dormant);
    }

    /// The stream bytes the host sent a remote session's pane, 0 for a local
    /// one: what a test reads to see a hidden pane cost nothing.
    ///
    /// - Returns: The bytes.
    [[nodiscard]] std::uint64_t remotePaneBytesReceived() const { return m_remote && m_remote->core ? m_remote->core->paneBytesReceived() : 0; }

    /// Dials a reconnecting remote session at once instead of waiting out its
    /// backoff.
    ///
    /// - Returns: Whether a dial began; false for a local session and for any
    ///   state but Reconnecting.
    bool reconnectRemoteNow()
    {
        return m_remote && m_remote->core && m_remote->core->reconnectNow();
    }

    /// What the person is told of a remote pane's input: events a connection
    /// took whose fate the host could not report after a reconnect, events
    /// waiting behind them or for a connection, and events discarded. Read
    /// on the session thread when ``EventKind::InputInDoubtChanged`` drains.
    ///
    /// - Returns: The status; all zero for a local session.
    [[nodiscard]] Protocol::Sync::PaneInputStatus remoteInputStatus() const
    {
        return m_remote && m_remote->core ? m_remote->core->inputStatus() : Protocol::Sync::PaneInputStatus {};
    }

    /// Words the notice a remote pane shows while its input awaits a
    /// decision, the same on every frontend.
    ///
    /// - Returns: The notice; every field empty when no decision is awaited.
    [[nodiscard]] Session::InputInDoubtNotice remoteInputInDoubtNotice() const
    {
        return Session::inputInDoubtNotice(remoteInputStatus()).value_or(Session::InputInDoubtNotice {});
    }

    /// Sends a remote pane's events in doubt again, at the person's word: the
    /// host writes each it had not written, once, and what waited behind them
    /// follows. Nothing calls this unasked, since sending again can run a
    /// command twice.
    void resendRemoteInputInDoubt()
    {
        if (m_remote && m_remote->core)
            m_remote->core->resendInDoubt();
    }

    /// Discards a remote pane's events in doubt, at the person's word, and
    /// lets what waited behind them go.
    void discardRemoteInputInDoubt()
    {
        if (m_remote && m_remote->core)
            m_remote->core->discardInDoubt();
    }

    /// Tells a remote session's host that this client is about to be
    /// suspended, and lets its carrier go: an app at its background deadline,
    /// a machine about to sleep. The host holds the attachment at once and
    /// stops sending to a client that cannot read; `resumeAfterSuspension`
    /// continues it. A no-op on a local session and on one not connected.
    void suspendRemote()
    {
        if (m_remote && m_remote->core)
            m_remote->core->suspend();
    }

    /// Sets the instance the next remote attach greets its host as. A
    /// frontend that keeps a window's identity across launches keeps this
    /// with it, so the relaunched window is the same client to the host.
    /// Held by the session for the reason the access token is: the
    /// connection is built inside the attach door.
    ///
    /// - Parameters:
    ///   - low: The instance's low half.
    ///   - high: Its high half.
    void setRemoteClientInstance(std::uint64_t low, std::uint64_t high) { m_remoteClientInstance = Protocol::Sync::ID128 { .low = low, .high = high }; }

    /// Gives the next remote attach the QUIC reach a previous process kept,
    /// so a restored attachment's first carrier is a resume proof and runs no
    /// login. Taken by the factory that attach builds, and by no other.
    ///
    /// - Parameters:
    ///   - address: The numeric address the bootstrap reached.
    ///   - port: The daemon's QUIC port.
    ///   - pin: The daemon certificate's SHA-256, its 32 bytes as a string.
    /// - Returns: Whether the reach is well formed and was kept.
    bool seedRemoteResumeReach(const std::string& address, std::uint16_t port, const std::string& pin)
    {
        if (pin.size() != 32 || port == 0 || !Session::isNumericAddress(address) || m_remote)
            return false;
        Session::ResumeReach reach { .address = address, .port = port, .certificatePin = {} };
        std::ranges::transform(pin, reach.certificatePin.begin(), [](char byte) { return static_cast<std::uint8_t>(byte); });
        if (std::ranges::all_of(reach.certificatePin, [](std::uint8_t byte) { return byte == 0; }))
            return false;
        m_pendingResumeReach = std::move(reach);
        return true;
    }

    /// The QUIC reach this session's remote connection would resume by.
    ///
    /// - Returns: The reach, or `nullopt` for a local session or one whose
    ///   carrier cannot be resumed.
    [[nodiscard]] std::optional<Session::ResumeReach> remoteResumeReach() const
    {
        return m_remote && m_remote->workspaceConnection ? m_remote->workspaceConnection->connection().resumeReach() : std::nullopt;
    }

    /// How many panes the remote connection's last Welcome continued with a
    /// diff, the outcome a restored attachment expects; 0 for a local session.
    [[nodiscard]] std::size_t remoteResumedPanesDiffed() const
    {
        return m_remote && m_remote->workspaceConnection
            ? m_remote->workspaceConnection->connection().resumedPaneCount(Protocol::Sync::ResumeOutcome::Diffed)
            : 0;
    }

    /// The QUIC reach this session's remote connection would resume by, for
    /// a frontend to keep beside its resume record; empty when it has none.
    [[nodiscard]] std::string remoteResumeReachAddress() const
    {
        const auto reach = remoteResumeReach();
        return reach ? reach->address : std::string {};
    }

    /// The port of ``remoteResumeReachAddress``; 0 when there is none.
    [[nodiscard]] std::uint16_t remoteResumeReachPort() const
    {
        const auto reach = remoteResumeReach();
        return reach ? reach->port : 0;
    }

    /// The certificate pin of ``remoteResumeReachAddress``, its 32 bytes as a
    /// string; empty when there is no reach.
    [[nodiscard]] std::string remoteResumeReachPin() const
    {
        const auto reach = remoteResumeReach();
        return reach ? std::string(reinterpret_cast<const char*>(reach->certificatePin.data()), reach->certificatePin.size()) : std::string {};
    }

    /// The low half of the instance this session's remote connection greets
    /// its host as; 0 for a local session.
    [[nodiscard]] std::uint64_t remoteClientInstanceLow() const { return m_remote && m_remote->workspaceConnection ? m_remote->workspaceConnection->connection().clientInstance().low : 0; }

    /// The high half; see `remoteClientInstanceLow`.
    [[nodiscard]] std::uint64_t remoteClientInstanceHigh() const { return m_remote && m_remote->workspaceConnection ? m_remote->workspaceConnection->connection().clientInstance().high : 0; }

    /// Reattaches the remote workspace after transport processing was suspended.
    ///
    /// - Parameter secondsSuspended: How long the platform held the process;
    ///   negative when the frontend cannot say.
    void resumeAfterSuspension(double secondsSuspended)
    {
        if (m_remote && m_remote->core) {
            m_remote->core->resumeAfterSuspension(secondsSuspended);
        }
    }

    /// How much of the crossing paste has left, 0 to 1.
    ///
    /// - Returns: The fraction, or a negative answer when nothing is crossing
    ///   or the channel cannot say — never a position nothing stands behind.
    [[nodiscard]] double pastePushProgress() const
    {
        return m_remote && m_remote->core ? m_remote->core->pastePushProgress() : -1;
    }

    /// Where the host landed the last pushed paste.
    ///
    /// Empty until a push settles, and empty for a push that did not land —
    /// the status rides the ``EventKind::PastePushSettled`` event, and a path
    /// is only a path once the file exists.
    ///
    /// - Returns: The host path, or an empty string.
    /// Returned by value: a reference to a std::string does not cross the
    /// Swift seam, and the caller reads this once per settled push.
    [[nodiscard]] std::string lastPastePushPath() const { return m_lastPastePushPath; }

    /// How many clients are attached to this session.
    ///
    /// Read when ``EventKind::PresenceChanged`` drains. More than one means
    /// somebody else can see this screen, which is the fact a frontend exists
    /// to show.
    [[nodiscard]] std::size_t presenceAttendeeCount() const { return m_presence.attendees.size(); }

    /// The bounded history remains after a reader detaches; opening it reviews only its displayed cursor.
    [[nodiscard]] std::size_t screenAccessCount() const { return m_screenAccessHistory.records().size(); }
    [[nodiscard]] bool hasUnreadScreenAccess() const { return m_screenAccessHistory.hasUnread(); }
    [[nodiscard]] std::uint64_t screenAccessCursor(std::size_t index) const { return index < screenAccessCount() ? m_screenAccessHistory.records()[index].cursor : 0; }
    [[nodiscard]] std::uint64_t screenAccessUnixMilliseconds(std::size_t index) const { return index < screenAccessCount() ? m_screenAccessHistory.records()[index].unixMilliseconds : 0; }
    [[nodiscard]] std::string screenAccessLabel(std::size_t index) const { return index < screenAccessCount() ? m_screenAccessHistory.records()[index].claimedLabel : std::string {}; }
    [[nodiscard]] std::string screenAccessProcess(std::size_t index) const { return index < screenAccessCount() ? m_screenAccessHistory.records()[index].verifiedProcess : std::string {}; }
    /// Reviews only the activity list's displayed position; newer reads remain unread.
    /// - Parameter cursor: The last event the opened list displayed.
    void acknowledgeScreenAccessThrough(std::uint64_t cursor)
    {
        m_screenAccessHistory.acknowledgeThrough(cursor);
        noteRemoteEvent(EventKind::PresenceChanged);
    }

    /// How many clients other than this one are attached.
    ///
    /// The fold a frontend actually wants, done once here rather than in each
    /// of them: a session somebody else can see is the fact worth showing, and
    /// this client's own row is never it. The host marks that row per
    /// recipient, so nothing here has to guess which one it is.
    ///
    /// - Returns: The count, 0 when this client is alone or not remote.
    [[nodiscard]] std::size_t otherAttendeeCount() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(m_presence.attendees,
            [](const Ubiquitty::Session::PaneAttendee& attendee) {
                return (attendee.flags & Ubiquitty::Session::presenceSelf) == 0;
            }));
    }

    /// Whether anyone other than this client is attached.
    ///
    /// - Returns: Whether a second party is on the session.
    [[nodiscard]] bool isSharedWithOthers() const { return otherAttendeeCount() > 0; }

    /// One attendee's display-only physical client number; never a selector.
    ///
    /// Indexed accessors rather than the vector, because a std::vector of
    /// structs holding std::string does not cross the Swift seam usefully.
    /// Every one answers a resting value out of range rather than trapping: a
    /// frontend redrawing while the roster shrinks under it must not crash on
    /// the row it was already painting.
    ///
    /// - Parameter index: The attendee.
    /// - Returns: The client id, or 0.
    [[nodiscard]] std::uint32_t presenceClientID(std::size_t index) const
    {
        return index < m_presence.attendees.size() ? m_presence.attendees[index].clientID : 0;
    }

    /// The stable attachment a disconnect names, or empty out of range.
    [[nodiscard]] std::string presenceAttachmentID(std::size_t index) const
    {
        return index < m_presence.attendees.size() ? Protocol::Sync::formatID128(m_presence.attendees[index].attachmentID) : std::string {};
    }

    /// The separately verified daemon grant selector, never display process/label.
    [[nodiscard]] std::string presenceGrantIdentity(std::size_t index) const
    {
        return index < m_presence.attendees.size() ? m_presence.attendees[index].grantIdentity : std::string {};
    }

    /// Whether this attendee is a person rather than an agent.
    ///
    /// - Parameter index: The attendee.
    /// - Returns: Whether the human bit is set.
    [[nodiscard]] bool isPresenceAttendeeHuman(std::size_t index) const
    {
        return index < m_presence.attendees.size()
            && (m_presence.attendees[index].flags & Ubiquitty::Session::presenceHuman) != 0;
    }

    /// Whether this attendee is attached over QUIC rather than the local
    /// socket.
    ///
    /// The carrier is what tells a locally-socketed peer from a network one,
    /// which is the fact a reader deciding whether to revoke is reading the
    /// roster for.
    ///
    /// - Parameter index: The attendee.
    /// - Returns: Whether the QUIC bit is set.
    [[nodiscard]] bool isPresenceAttendeeQUIC(std::size_t index) const
    {
        return index < m_presence.attendees.size()
            && (m_presence.attendees[index].flags & Ubiquitty::Session::presenceQUIC) != 0;
    }

    /// Whether this attendee is the reader itself.
    ///
    /// The host builds the roster per recipient and marks the recipient's own
    /// row, so a frontend can leave itself out of "someone else is watching"
    /// without guessing which row it is.
    ///
    /// - Parameter index: The attendee.
    /// - Returns: Whether this row is this client.
    [[nodiscard]] bool isPresenceAttendeeSelf(std::size_t index) const
    {
        return index < m_presence.attendees.size()
            && (m_presence.attendees[index].flags & Ubiquitty::Session::presenceSelf) != 0;
    }

    /// How this attendee is authorised: 0 once, 1 for the session, 2 always.
    ///
    /// - Parameter index: The attendee.
    /// - Returns: The grant scope, or 0.
    [[nodiscard]] std::uint8_t presenceGrantScope(std::size_t index) const
    {
        return index < m_presence.attendees.size() ? m_presence.attendees[index].grantScope : 0;
    }

    /// The attendee's verified display process path, not a grant selector.
    ///
    /// Empty for a remote agent, which the daemon cannot verify.
    ///
    /// - Parameter index: The attendee.
    /// - Returns: The path, or an empty string. By value: a reference to a
    ///   std::string does not cross the Swift seam.
    [[nodiscard]] std::string presenceProcess(std::size_t index) const
    {
        return index < m_presence.attendees.size() ? m_presence.attendees[index].process : std::string();
    }

    /// What the attendee calls itself.
    ///
    /// Spoofable, like every agent-chosen label: shown as a claim, never used
    /// as a key. ``presenceProcess`` is the verified half.
    ///
    /// - Parameter index: The attendee.
    /// - Returns: The label, or an empty string.
    [[nodiscard]] std::string presenceLabel(std::size_t index) const
    {
        return index < m_presence.attendees.size() ? m_presence.attendees[index].label : std::string();
    }

    /// Acts on an attendee: disconnect it, or forget a grant it holds.
    ///
    /// - Parameters:
    ///   - action: A ``Protocol::Sync::ConsentRevocationKind``: 0 disconnect now,
    ///     1 stop always-allowing, 2 clear this session's grant,
    ///     3 disconnect every agent.
    ///   - attachmentID: The stable attachment's canonical text, for action 0.
    ///   - identity: The separate verified grant key, for actions 1 and 2.
    void revokeConsent(std::uint8_t action, const std::string& attachmentID, const std::string& identity)
    {
        if (m_remote && m_remote->core)
            m_remote->core->revokeConsent(action, attachmentID, identity);
    }

    /// The id of the consent question waiting on the human, or 0.
    ///
    /// Diagnostic host identity only; answers use the local presentation token.
    [[nodiscard]] std::uint32_t pendingConsentRequestID() const { return m_isStructureConsentShown ? m_structureConsents.front().request.requestID : m_pendingConsent.requestID; }

    /// The exact ordinary question's local presentation token, or zero. It is
    /// never reused after a question retires or a connection restarts.
    [[nodiscard]] std::uint64_t pendingConsentToken() const { return m_isStructureConsentShown ? 0 : m_pendingConsentToken; }

    /// What the agent is asking for: 0 attach, 1 terminate, 2 grant a
    /// capability.
    ///
    /// The prompt must say which. Approving a kill as though it were an
    /// attach, or a capability grant as an attach, defeats the gate — which is
    /// why this is a separate reading and not folded into "an agent wants in".
    [[nodiscard]] std::uint8_t pendingConsentAction() const { return m_isStructureConsentShown ? 3 : m_pendingConsent.action; }

    /// The local token of the exact structure question shown, or zero. Unlike
    /// a host's request number, this token is not reused after reconnect.
    [[nodiscard]] std::uint64_t pendingStructureConsentToken() const { return m_isStructureConsentShown ? m_structureConsents.front().token : 0; }
    /// Core's sanitized question for the structure operation.
    [[nodiscard]] std::string pendingStructureConsentQuestion() const { return m_isStructureConsentShown ? m_structureConsents.front().words.question : std::string(); }
    /// Core's sanitized effect, target and requester description.
    [[nodiscard]] std::string pendingStructureConsentDetail() const { return m_isStructureConsentShown ? m_structureConsents.front().words.detail : std::string(); }
    /// The allowing button, promising only one operation.
    [[nodiscard]] std::string pendingStructureConsentAllowTitle() const { return m_isStructureConsentShown ? m_structureConsents.front().words.allow : std::string(); }
    /// The default refusing button.
    [[nodiscard]] std::string pendingStructureConsentDenyTitle() const { return m_isStructureConsentShown ? m_structureConsents.front().words.deny : std::string(); }

    /// Answers the exact question the UI displayed. A stale token, including
    /// one from before reconnect, answers nothing.
    ///
    /// - Parameters:
    ///   - token: The local presentation token captured before showing the UI.
    ///   - allow: Whether the person approved this one operation.
    void respondToStructureConsent(std::uint64_t token, bool allow) // bool-ok: Swift supplies the person's Boolean answer, which maps directly to the wire allow field
    {
        if (!m_isStructureConsentShown || m_structureConsents.front().token != token)
            return;
        const std::uint32_t requestID = m_structureConsents.front().request.requestID;
        m_structureConsents.pop_front();
        m_isStructureConsentShown = false;
        const auto weakThis = weakPtr();
        if (m_remote && m_remote->core)
            (void)m_remote->core->respondToStructureConsent(requestID, allow);
        if (!weakThis)
            return;
        showNextConsent();
    }

    /// Declines the structure question the UI could not show the person,
    /// which is not an answer: it leaves the question to the others asked.
    ///
    /// - Parameter token: The local presentation token of the question.
    void declineStructureConsent(std::uint64_t token)
    {
        if (!m_isStructureConsentShown || m_structureConsents.front().token != token)
            return;
        const std::uint32_t requestID = m_structureConsents.front().request.requestID;
        m_structureConsents.pop_front();
        m_isStructureConsentShown = false;
        const auto weakThis = weakPtr();
        declineUnshownQuestion(requestID, Protocol::Sync::ConsentKind::Structure);
        if (!weakThis)
            return;
        showNextConsent();
    }

    /// The capability an action-2 request wants, as the raw enum value. 0 for
    /// attach and terminate.
    [[nodiscard]] std::uint16_t pendingConsentCapabilityCode() const
    {
        return m_pendingConsent.capabilityCode;
    }

    /// Whether the pending request can be granted for longer than one use.
    ///
    /// A frontend asks rather than deciding: the tier that forces a capability
    /// to a single use is security's classification, an unmapped code falls
    /// through to the strictest tier, and a prompt that offered a Remember
    /// button for one of those would be promising a lifetime the daemon does
    /// not honour. Attach and terminate are not capabilities and are not
    /// per-use — an agent may be trusted for a session or for good.
    [[nodiscard]] bool pendingConsentGrantIsPerUse() const // bool-ok: the tier's own answer, not a call-site constant
    {
        if (m_isStructureConsentShown)
            return true;
        // Action 2 is a capability grant, per ConsentRequestMessage::action.
        constexpr std::uint8_t grantCapabilityAction = 2;
        if (m_pendingConsent.action != grantCapabilityAction)
            return false;
        return Ubiquitty::capabilityGrantIsPerUse(static_cast<Capability>(m_pendingConsent.capabilityCode),
            Security::capabilityOriginOf(m_pendingConsent.capabilityOrigin));
    }

    /// What an application asked the window to do.
    ///
    /// One record for the whole ``WindowClient`` surface, because a frontend
    /// answers them all from one place. `kind` says which verb and therefore
    /// how to read the rest; `None` is what an empty queue answers, so a
    /// caller drains until it sees one rather than asking a second question
    /// first.
    struct WindowRequest {
        /// Which verb the application used.
        enum class Kind : std::uint8_t {
            /// The queue is empty. Not a request.
            None UBIQUITTY_SWIFT_NAME(none),
            /// CSI t: `first` is the operation, `second` and `third` its
            /// arguments.
            WindowOp UBIQUITTY_SWIFT_NAME(windowOp),
            /// DECCOLM: `first` is non-zero for 132 columns.
            ColumnMode UBIQUITTY_SWIFT_NAME(columnMode),
            /// OSC 1337 StealFocus. No arguments.
            StealFocus UBIQUITTY_SWIFT_NAME(stealFocus),
            /// `first` is the ``PAL::AttentionRequest``.
            Attention UBIQUITTY_SWIFT_NAME(attention),
            /// OSC 1337 SetColors=preset: `text` names the theme.
            ColorPreset UBIQUITTY_SWIFT_NAME(colorPreset),
        };
        Kind kind { Kind::None };
        int first {};
        int second {};
        int third {};
        std::string text;
    };

    /// Queues the window verbs for a frontend that installs no client.
    ///
    /// Called by a frontend that will drain ``takeWindowRequest`` instead of
    /// answering the parser inline. Refuses when a client is already
    /// installed — a frontend that installs its own client and hops each call to
    /// its main queue would silently stop responding to CSI t if the session
    /// replaced it.
    ///
    /// Asked for rather than automatic, because "nobody has installed one yet"
    /// and "this frontend wants the queue" are different claims, and a session
    /// that guessed would install the queue for a frontend still wiring itself
    /// up.
    ///
    /// - Returns: Whether the queue is now the terminal's window client.
    bool installWindowRequestQueue() // bool-ok: reports what happened, not a mode chosen at the call site
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        if (term->windowClient.installed())
            return term->windowClient.isInstalled(&m_windowClient);
        m_windowClient.session = this;
        term->windowClient = &m_windowClient;
        return true;
    }

    /// The most window requests the queue holds before it drops the overrun.
    ///
    /// As many as the image store keeps, for the same reason: a bound a
    /// well-behaved program never reaches and a runaway cannot grow past.
    /// Public because a frontend draining in a loop and a case checking the
    /// ceiling are asking the same question.
    static constexpr std::size_t windowRequestQueueLimit = 8;

    /// Whether an application asked the window for anything not yet taken.
    [[nodiscard]] bool hasPendingWindowRequest() const // bool-ok: the queue's own state, not a call-site constant
    {
        const std::scoped_lock hold(*m_activeLock);
        return !m_windowRequests.empty();
    }

    /// The oldest window request, removed from the queue.
    ///
    /// A frontend drains until this answers ``WindowRequest::Kind::None``. The
    /// queue holds every request because a client that hops each call to the
    /// main queue leaves two in flight when a program iconifies and then
    /// moves, and a single slot would drop the first.
    ///
    /// - Returns: The request, or one whose kind is `None` when there is none.
    [[nodiscard]] WindowRequest takeWindowRequest()
    {
        const std::scoped_lock hold(*m_activeLock);
        if (m_windowRequests.empty())
            return WindowRequest {};
        WindowRequest request = std::move(m_windowRequests.front());
        m_windowRequests.erase(m_windowRequests.begin());
        return request;
    }

    /// Installs the model-lock-protected native drag command queue.
    [[nodiscard]] bool installDragDropQueue() // bool-ok: reports installation
    {
        const std::scoped_lock hold(*m_activeLock);
        auto* term = modelTerm();
        if (!term)
            return false;
        if (term->dragDropClient.installed())
            return term->dragDropClient.isInstalled(&m_dragDropQueue);
        m_dragDropQueue.setWake([](void* context) {
            auto& session = *static_cast<TerminalSession*>(context);
            session.m_events.pending.fetch_or(1u << static_cast<unsigned>(EventKind::DragDropPending), std::memory_order_release);
        },
            this);
        term->dragDropClient = &m_dragDropQueue;
        return true;
    }

    /// Transfers one native command out of the model lock.
    [[nodiscard]] DragDrop::CommandRecord takeDragDropCommand()
    {
        const std::scoped_lock hold(*m_activeLock);
        return m_dragDropQueue.take();
    }

    /// Enables the frontend's native drag capability before its connection handshake.
    void setNativeDragSupported() { m_supportsNativeDrag = true; }

    /// Allows another data chunk while the transport has bounded reply headroom.
    [[nodiscard]] bool canSendNativeDragData()
    {
        const std::scoped_lock hold(*m_activeLock);
        if (m_remote && m_remote->core)
            return m_remote->core->pendingOutboundBytes() < (1 << 20);
        const auto* term = modelTerm();
        const auto* stream = m_tmuxPrimaryStream ? m_tmuxPrimaryStream : m_runtime->stream();
        return (!term || term->bulkOutput.length < (64 << 10))
            && (!stream || stream->pendingWriteBytes() < (1u << 20));
    }

    /// Receives input from the pane's native drag owner, never from terminal bytes.
    [[nodiscard]] bool sendNativeDragEvent(const DragDrop::NativeEvent& native, ByteSpan bytes UBIQUITTY_NOESCAPE)
    {
        if (m_remote && m_remote->core) {
            return m_remote->core->sendNativeDragEvent(native, bytes);
        }
        DragDrop::Metadata metadata;
        metadata.x = native.x;
        metadata.y = native.y;
        metadata.pixelX = native.pixelX;
        metadata.pixelY = native.pixelY;
        metadata.operation = native.operation;
        sendDragDropEvent({ .kind = native.kind, .sessionID = native.sessionID, .requestID = native.requestID, .metadata = metadata, .payload = { bytes.data(), bytes.size() }, .error = native.error, .isFinal = native.isFinal, .isSameWindow = native.isSameWindow });
        return true;
    }

    /// Allocates a native lifetime identity without reusing one after RIS or adoption.
    [[nodiscard]] std::uint64_t nextNativeDragSession()
    {
        const std::scoped_lock hold(*m_activeLock);
        if (auto* term = modelTerm(); term && term->dragDropSession)
            m_nativeDragSession = std::max(m_nativeDragSession, term->dragDropSession->registrationState().lastNativeSession);
        if (m_nativeDragSession == std::numeric_limits<std::uint64_t>::max())
            return 0;
        return ++m_nativeDragSession;
    }

    /// Applies native input and flushes its replies under the model lock.
    /// - Parameter event: Input from the pane's native drag adapter.
    void sendDragDropEvent(const DragDrop::Event& event)
    {
        {
            const std::scoped_lock hold(*m_activeLock);
            if (auto* term = modelTerm()) {
                DragDrop::receive(*term, event);
                if (m_externalTerminal && m_tmuxPrimaryStream)
                    Ubiquitty::Protocol::Tmux::flushPaneReplies(*term, m_tmuxRoutingClient, m_tmuxRoutingPaneID, *m_tmuxPrimaryStream);
                else
                    m_runtime->flushReplies();
            }
        }
        if (m_events.pending.load(std::memory_order_acquire) || m_runtime->hasPendingWrite())
            signalDrain();
    }

    /// Revokes grants before a native surface changes ownership or disappears.
    void cancelDragDrop()
    {
        if (m_remote && m_remote->core)
            (void)m_remote->core->sendNativeDragEvent({ .kind = DragDrop::EventKind::Cancel }, {});
        const std::scoped_lock hold(*m_activeLock);
        if (auto* term = modelTerm())
            DragDrop::reset(*term);
    }

    /// One desktop notification an application asked for.
    ///
    /// The pair is already resolved by Core's display policy
    /// (``OSC::resolveNotificationDisplay``): a body-only dialect — OSC 9 and
    /// kitty's OSC 99 carry no title — has its body promoted into `title`, so
    /// a backend posts these two fields verbatim and no frontend re-derives
    /// the promotion.
    struct Notification {
        /// The banner's heading. Non-empty whenever `isPresent`.
        std::string title;
        /// The banner's subtitle, empty for a body-only dialect.
        std::string body;
        /// The program's own name for the notification (OSC 99 `i=`), reduced
        /// to letters, digits and `_-+.`; empty for a dialect without one.
        std::string identifier;
        /// When the program wants it shown (OSC 99 `o=`).
        NotificationOccasion occasion = NotificationOccasion::Always;
        /// Whether this record came off the queue. False is what an empty
        /// queue answers, so a caller drains until it sees one rather than
        /// asking a second question first.
        bool isPresent = false;
    };

    /// Queues the desktop notifications for a frontend that installs no
    /// ``ShellClient``.
    ///
    /// The shape ``installWindowRequestQueue`` has, for the same reason: a
    /// Swift frontend cannot subclass a C++ class with virtual methods, so
    /// without a Core-side sink `Terminal::shellClient` stays null and OSC 9,
    /// OSC 99 and OSC 777;notify are parsed and dropped.
    ///
    /// Refuses when a client is already installed — a frontend that
    /// installs its own and hops each call to its main queue would silently
    /// stop reporting OSC 7 if the session replaced it.
    ///
    /// - Returns: Whether the queue is now the terminal's shell client.
    bool installNotificationQueue() // bool-ok: reports what happened, not a mode chosen at the call site
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        if (term->shellClient.get() != nullptr)
            return term->shellClient.get() == &m_shellClient;
        m_shellClient.session = this;
        term->shellClient = &m_shellClient;
        return true;
    }

    /// Gives the terminal a clipboard that writes the system clipboard's text
    /// through the platform backend, for a shell that installs none of its
    /// own — the route a copy-on-select takes. A terminal that already has a
    /// clipboard client keeps it.
    ///
    /// - Returns: Whether the terminal's clipboard client is now one that
    ///   reaches the system clipboard.
    bool installSystemClipboardWriter() // bool-ok: reports what happened, not a mode chosen at the call site
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        if (term->clipboardClient)
            return true;
        auto writer = std::make_unique<SystemClipboardWriter>();
        term->clipboardClient = writer.get();
        m_ownedClients.push_back({ std::move(writer), &SystemClipboardWriter::detach });
        return true;
    }

    /// The clipboard ``installSystemClipboardWriter`` installs. Writes only:
    /// the system clipboard's text is what the platform backend takes.
    struct SystemClipboardWriter final : Ubiquitty::ClipboardClient {
        /// Writes the system clipboard's text; the other buffers have no
        /// system counterpart and are dropped.
        ///
        /// - Parameters:
        ///   - buffer: The OSC 52 buffer written.
        ///   - bytes: The text, UTF-8.
        void writeClipboard(Ubiquitty::ClipboardBuffer buffer, std::span<const char> bytes) override
        {
            if (buffer == Ubiquitty::ClipboardBuffer::Clipboard)
                Ubiquitty::PAL::clipboardSetText(std::string_view(bytes.data(), bytes.size()));
        }

        /// Clears the terminal's clipboard when it is still this writer.
        ///
        /// - Parameters:
        ///   - terminal: The terminal being released.
        ///   - client: The writer installed on it.
        static void detach(Ubiquitty::Terminal& terminal, Ubiquitty::ClipboardClient& client)
        {
            if (terminal.clipboardClient.get() == &client)
                terminal.clipboardClient = nullptr;
        }
    };

    /// The most notifications the queue holds before it drops the overrun.
    ///
    /// As many as the window queue keeps, for the same reason: a bound a
    /// well-behaved program never reaches and a runaway cannot grow past.
    /// Public because a frontend draining in a loop and a case checking the
    /// ceiling are asking the same question.
    static constexpr std::size_t notificationQueueLimit = 8;

    /// Whether an application asked for a banner not yet taken.
    [[nodiscard]] bool hasPendingNotification() const // bool-ok: the queue's own state, not a call-site constant
    {
        const std::scoped_lock hold(*m_activeLock);
        return !m_notifications.empty();
    }

    /// The oldest notification, removed from the queue.
    ///
    /// A frontend drains until this answers one whose `isPresent` is false.
    ///
    /// - Returns: The notification, or an absent one when there is none.
    [[nodiscard]] Notification takeNotification()
    {
        const std::scoped_lock hold(*m_activeLock);
        if (m_notifications.empty())
            return Notification {};
        Notification notification = std::move(m_notifications.front());
        m_notifications.erase(m_notifications.begin());
        return notification;
    }

    /// Queues the trigger reports for a frontend that installs no
    /// ``TriggerClient``.
    ///
    /// The shape ``installNotificationQueue`` has, for the same reason: a Swift
    /// frontend cannot subclass a C++ class with virtual methods, so without a
    /// Core-side sink `Terminal::triggerClient` stays null and every matched
    /// `notify`, `copy`, `set_title`, `run_command` and `webhook` rule is
    /// evaluated and dropped. Highlight rules paint either way.
    ///
    /// Refuses when a client is already installed — a frontend that
    /// installs its own would have its triggers moved onto a queue nobody
    /// drains if the session replaced it.
    ///
    /// - Returns: Whether the queue is now the terminal's trigger client.
    bool installTriggerQueue() // bool-ok: reports what happened, not a mode chosen at the call site
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        if (term->triggerClient.get() != nullptr)
            return term->triggerClient.get() == &m_triggerClient;
        m_triggerClient.session = this;
        term->triggerClient = &m_triggerClient;
        return true;
    }

    /// The most trigger reports the queue holds before it drops the overrun.
    ///
    /// As many as the engine reports for one line, so a line that matches
    /// every rule lands whole, and a flood of matching lines between two
    /// drains cannot grow the queue past it.
    static constexpr std::size_t triggerQueueLimit = 64;

    /// Whether a trigger matched and its report is not yet taken.
    [[nodiscard]] bool hasPendingTriggerMatch() const // bool-ok: the queue's own state, not a call-site constant
    {
        const std::scoped_lock hold(*m_activeLock);
        return !m_triggerMatches.empty();
    }

    /// The oldest trigger report that resolves to an act, removed from the
    /// queue with every report before it.
    ///
    /// Resolved here, on the caller's thread, rather than when the engine
    /// reported it: the two `[triggers]` gates are configuration, which is read
    /// on the frontend's own loop, and a report queued before the reader
    /// closed a gate must not spawn after they did. A report the gates refuse
    /// is consumed and skipped, so a frontend drains until this answers
    /// ``TriggerEffectKind::None`` and never sees one.
    ///
    /// - Parameter configuration: The live values the gates are read from.
    /// - Returns: The act, whose kind is `None` when the queue holds none.
    [[nodiscard]] TriggerEffect takeTriggerEffect(const Preferences::Values& configuration)
    {
        for (;;) {
            TriggerMatch match;
            {
                const std::scoped_lock hold(*m_activeLock);
                if (m_triggerMatches.empty())
                    return TriggerEffect {};
                match = std::move(m_triggerMatches.front());
                m_triggerMatches.erase(m_triggerMatches.begin());
            }
            TriggerEffect effect = resolveTriggerEffect(match.action, match.argument, match.matchedText, configuration);
            if (effect.kind != TriggerEffectKind::None)
                return effect;
        }
    }

    /// Names the terminal as an OSC 2 from its program would, for a trigger's
    /// `set_title`.
    ///
    /// The program's title is the one field every surface weaves a name from —
    /// the window's, the tab's, a pane header's — so a rule that names the
    /// window names all of them, and the program's next title replaces it as it
    /// replaces its own. Raises nothing: the caller is the frontend's own loop,
    /// which reads the title back when this returns.
    ///
    /// - Parameter title: The name, already resolved by ``resolveTriggerEffect``.
    void applyTriggerTitle(const std::string& title)
    {
        const std::scoped_lock hold(*m_activeLock);
        if (Ubiquitty::Terminal* term = modelTerm())
            term->titleUTF8 = title;
    }

    /// Compiles the configuration's `[triggers]` and `[characterClass]` rules
    /// into this session's terminal, under the model lock.
    ///
    /// A launch already carries them — the seed compiles them — so this is the
    /// reload: a reader who adds a rule expects the windows already open to
    /// obey it, not only the next one. Replaces rather than appends, and leaves
    /// the highlights already painted where they are.
    ///
    /// - Parameter configuration: The live values the rules are read from.
    /// - Returns: What was refused, in words, or empty when nothing was.
    [[nodiscard]] std::string applyTriggerConfiguration(const Preferences::Values& configuration)
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return {};
        return describeTriggerConfig(applyTriggerConfig(*term, configuration));
    }

    /// The peer's process, as the daemon verified it.
    ///
    /// This is the name a prompt may trust. Returned by value: a reference to
    /// a std::string does not cross the Swift seam.
    [[nodiscard]] std::string pendingConsentPeerProcess() const { return m_isStructureConsentShown ? m_structureConsents.front().request.peerProcess : m_pendingConsent.peerProcess; }

    /// What the agent calls itself.
    ///
    /// Spoofable, and named so at every layer it crosses: an agent chooses
    /// this string. A prompt may show it, and may not decide anything on it —
    /// ``pendingConsentPeerProcess`` is the verified half.
    [[nodiscard]] std::string pendingConsentAgentLabel() const { return m_isStructureConsentShown ? m_structureConsents.front().request.agentLabel : m_pendingConsent.agentLabel; }

    /// The origin a capability request was checked against, as the raw
    /// ``CapabilityOrigin`` value the wire carries; 0 for attach and terminate.
    [[nodiscard]] std::uint8_t pendingConsentCapabilityOrigin() const { return m_pendingConsent.capabilityOrigin; }

    /// Whether the pending capability request is this client's own: a program
    /// this device drives asks about this device's clipboard or banners. The
    /// prompt then names the device, which has no verified process to show and
    /// claims no agent name.
    [[nodiscard]] bool pendingConsentIsOwnRequest() const // bool-ok: the request's own field, not a call-site constant
    {
        return m_pendingConsent.isRecipientsOwnRequest;
    }

    /// Whether the pending question is this client's own, about an effect a
    /// host on another machine delivered.
    ///
    /// Its answer never reaches the host, which keeps no deadline for it, and
    /// an allow lasts at most as long as the connection. The prompt words it
    /// with ``Security::hostEffectConsentTitle`` and offers
    /// ``Security::hostEffectConnectionScopeTitle()`` in place of "Always".
    ///
    /// - Returns: Whether the question is the client's own.
    [[nodiscard]] bool pendingConsentIsClientHeld() const // bool-ok: the question's own fact, not a call-site constant
    {
        return !m_isStructureConsentShown && m_pendingHostEffectToken != 0;
    }

    /// Answers the consent question with the person's choice.
    ///
    /// Only a person's answer goes through here: any other value is a
    /// refusal, never a decline.
    ///
    /// - Parameters:
    ///   - token: The local ``pendingConsentToken`` captured by the presentation.
    ///   - decision: A ``Protocol::Sync::ConsentChoice`` value: 0 deny,
    ///     1 allow once, 2 allow for this session, 3 allow always.
    void respondToConsent(std::uint64_t token, std::uint8_t decision)
    {
        const auto choice = decision <= std::to_underlying(Protocol::Sync::ConsentChoice::AllowAlways)
            ? static_cast<Protocol::Sync::ConsentChoice>(decision)
            : Protocol::Sync::ConsentChoice::Deny;
        closePendingConsent(token, choice, choice);
    }

    /// Declines the question the UI could not show the person: one it cannot
    /// present while another is up, or one it cannot word.
    ///
    /// A host's question is declined as unshown, which leaves it to the others
    /// asked; an effect this client holds is discarded, since nobody here can
    /// be asked about it.
    ///
    /// - Parameter token: The local ``pendingConsentToken`` of the question.
    void declineConsent(std::uint64_t token) { closePendingConsent(token, Protocol::Sync::ConsentChoice::Unshown, Protocol::Sync::ConsentChoice::Deny); }

private:
    /// Closes the pending question and shows the next.
    ///
    /// - Parameters:
    ///   - token: The local ``pendingConsentToken`` of the question.
    ///   - hostAnswer: What the host is told for its own question.
    ///   - heldAnswer: What a held effect is answered with.
    void closePendingConsent(std::uint64_t token, Protocol::Sync::ConsentChoice hostAnswer, Protocol::Sync::ConsentChoice heldAnswer)
    {
        if (m_isStructureConsentShown || token == 0 || token != m_pendingConsentToken)
            return;
        // Cleared before the send, and whether or not there is anything to
        // send to. A connection that drops between the question and the answer
        // would otherwise leave the question standing, and the next drain
        // would raise a second prompt for a session that is already gone.
        const auto requestID = m_pendingConsent.requestID;
        const auto kind = static_cast<Protocol::Sync::ConsentKind>(m_pendingConsent.action);
        const std::uint64_t heldEffect = m_pendingHostEffectToken;
        m_pendingConsent = {};
        m_pendingConsentToken = 0;
        m_pendingHostEffectToken = 0;
        const auto weakThis = weakPtr();
        if (m_remote && m_remote->core) {
            // A question this client asked of its own policy is answered here
            // and never reaches the host.
            if (heldEffect)
                m_remote->core->answerHeldHostEffect(heldEffect, heldAnswer);
            else
                m_remote->core->respondToConsent(requestID, kind, std::to_underlying(hostAnswer));
        }
        if (!weakThis)
            return;
        showNextConsent();
    }

public:
#if defined(UBIQUITTY_ENABLE_TEST_HOOKS) || defined(UBIQUITTY_UI_TESTS) || defined(UBIQUITTY_IOS_UI_TESTS)
    /// Drives an exact-operation question through the production queue.
    ///
    /// - Parameter request: The question to report.
    void noteStructureConsentForTest(const Protocol::Sync::StructureConsent& request) { noteStructureConsent(request); }

    /// Sends the complete typed retirement through the production observer door.
    /// - Parameter resolution: The exact question kind, request and session.
    void noteConsentResolvedForTest(const Protocol::Sync::ConsentResolution& resolution) { noteConsentResolved(resolution); }

    /// Retires a question through the production observer path.
    ///
    /// - Parameter requestID: The host's question identifier.
    void noteStructureConsentResolvedForTest(std::uint32_t requestID)
    {
        const auto found = std::ranges::find_if(m_structureConsents, [&](const auto& held) { return held.request.requestID == requestID; });
        if (found != m_structureConsents.end())
            noteConsentResolved({ Protocol::Sync::ConsentKind::Structure, requestID, found->request.sessionID });
    }

    /// Holds a tree as though the listed host had answered with it, for a
    /// scenario that draws a host's outline without dialling one.
    ///
    /// - Parameter tree: The tree, as a host states it.
    void holdHostSessionTreeForUITest(const Ubiquitty::Session::SessionTree& tree)
    {
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        noteHostSessionTree(tree);
        noteHostListingAnswered();
    }

    /// Holds a tree as though a connected watch had reported it, for a
    /// scenario that draws a followed host without dialling one.
    ///
    /// - Parameter tree: The tree, as a host states it.
    void holdWatchedHostSessionTreeForUITest(const Ubiquitty::Session::SessionTree& tree)
    {
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        noteHostWatchedTree(tree);
    }
#endif

#ifdef UBIQUITTY_ENABLE_TEST_HOOKS
    /// Publishes a remote replica's change through the production observer door.
    void noteRemoteScreenChangedForTest() { noteRemoteScreenChanged(); }

    /// Drives the consent report without standing up a transport.
    ///
    /// The observer forwards to the same method, so a case here exercises what
    /// runs. Behind the hook because a frontend inventing a consent question
    /// would be asking the user to authorise something nobody requested.
    ///
    /// - Parameter request: The question to report.
    void noteConsentRequestForTest(const Ubiquitty::Protocol::Sync::ConsentQuestion& request)
    {
        noteConsentRequest(request);
    }

    /// Drives the roster report without standing up a transport.
    ///
    /// - Parameter presence: The roster to report.
    void notePresenceForTest(const Ubiquitty::Session::PanePresenceSnapshot& presence)
    {
        notePresence(presence);
    }

    /// Drives the end of the directory's answer without dialling a host.
    ///
    /// Stands the directory up if nothing has asked a host yet, because a
    /// case driving it has not called ``listHostSessions``. Behind the hook
    /// because a frontend inventing an answer would offer sessions to attach
    /// to that no host holds.
    void noteHostListingAnsweredForTest()
    {
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        noteHostListingAnswered();
    }

    /// Drives the directory's tree without dialling a host.
    ///
    /// - Parameter tree: The tree to hold, as a listing would report it
    ///   before its rows.
    void noteHostSessionTreeForTest(const Ubiquitty::Session::SessionTree& tree)
    {
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        noteHostSessionTree(tree);
    }

    /// Drives a connected watch's report without dialling a host.
    ///
    /// - Parameter tree: The tree the watch reports.
    void noteWatchedHostSessionTreeForTest(const Ubiquitty::Session::SessionTree& tree)
    {
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        m_directory->watchObserver.session = this;
        m_directory->watchObserver.onSessionTree(tree);
    }

    /// Drives a watch losing its host without a carrier to drop.
    ///
    /// - Parameter report: Why it was lost.
    void noteHostWatchInterruptedForTest(const Ubiquitty::Session::DirectoryFailureReport& report)
    {
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        m_directory->watchObserver.session = this;
        m_directory->watchObserver.onWatchInterrupted(report);
    }

    /// Admits a structure change as ``requestHostStructureChange`` does and
    /// holds its call instead of sending it, for a test that then plays the
    /// host.
    ///
    /// - Parameter request: The change.
    /// - Returns: The call the host would be sent; empty when the request
    ///   was refused here, which settled it.
    std::string holdHostStructureCallForTest(const StructureRequest& request)
    {
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        StructureCalls::Admission admission = m_directory->structure.admit(request, m_directory->tree.get());
        if (admission.refusal) {
            noteHostStructureSettled(std::move(*admission.refusal));
            return {};
        }
        m_directory->structureRequestInFlight = admission.requestID;
        return admission.call;
    }

    /// Plays the host's answer to a held call, or the carrier dropping
    /// under it.
    ///
    /// - Parameter response: The JSON-RPC response; empty for a call that
    ///   was never answered.
    void noteHostStructureAnswerForTest(const std::string& response)
    {
        if (response.empty())
            (void)noteHostStructureUnanswered("The host could not be reached.");
        else
            noteHostStructureAnswer(response);
    }

    /// Drives a settled terminate without asking a host to end anything.
    ///
    /// - Parameters:
    ///   - sessionID: The session the request named.
    ///   - outcome: What the daemon did.
    void noteHostSessionTerminateSettledForTest(std::uint32_t sessionID, Ubiquitty::Session::TerminateOutcome outcome)
    {
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        noteHostSessionTerminateSettled(sessionID, outcome);
    }

    /// Drives the title composition without standing up a transport.
    ///
    /// - Parameter parts: The host's record.
    void applyRemoteTitlePartsForTest(const Ubiquitty::Protocol::Sync::PaneFacts& parts)
    {
        applyRemoteTitleParts(parts);
    }

    /// Drives the host's report that the session ended without a transport.
    void noteHostedSessionEndedForTest() { noteHostedSessionEnded(); }

    /// The model's title, for a case asserting what a compose produced.
    ///
    /// - Returns: The title, or empty without a model.
    [[nodiscard]] std::string titleForTest()
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        return term != nullptr ? term->titleUTF8.copyToString() : std::string();
    }
#endif

#ifdef UBIQUITTY_ENABLE_TEST_HOOKS
    /// Drives the connection-state report without standing up a transport.
    ///
    /// The observer forwards to the same method, so a case here exercises what
    /// runs rather than a copy of it. Behind the hook because a frontend
    /// announcing a state the connection is not in would be a lie the status
    /// pill repeats.
    ///
    /// - Parameters:
    ///   - state: The state to report.
    ///   - detail: The failure reason, for Failed.
    void noteRemoteStateChangedForTest(Ubiquitty::Session::State state, const std::string& detail)
    {
        noteRemoteStateChanged(state, detail);
    }

    /// The attached pane's endpoint, for a case that drives what its
    /// workspace does to it.
    ///
    /// - Returns: The endpoint, or null when no pane is attached.
    [[nodiscard]] Session::RemoteSessionCore* remoteCoreForTest() { return m_remote ? m_remote->core.get() : nullptr; }

    /// Records an ask for a host file without a transport to mint the id.
    ///
    /// The ask and the answer are separate hooks because the gate between them
    /// is the assertion: a case has to be able to deliver an answer to a
    /// request that was never made.
    ///
    /// - Parameters:
    ///   - requestID: The id the answer will carry.
    ///   - path: The path the ask named.
    ///   - line: The line the clicked link named.
    void noteAskedRemoteFileForTest(std::uint32_t requestID, const std::string& path, Session::FileLinkLine line = {})
    {
        if (!m_remote)
            m_remote = std::make_unique<RemoteParts>();
        noteAskedRemoteFile(requestID, path, line);
    }

    /// Delivers a host's file answer without standing up a transport.
    ///
    /// - Parameters:
    ///   - requestID: The request the host says it answers.
    ///   - status: How the host answered.
    ///   - path: The path the host echoed.
    ///   - data: The file's bytes; empty unless the status is OK.
    void noteRemoteFileForTest(std::uint32_t requestID, Protocol::Control::FileReadStatus status, const std::string& path, std::span<const std::uint8_t> data)
    {
        if (!m_remote)
            m_remote = std::make_unique<RemoteParts>();
        Protocol::Control::FileReadReply reply;
        reply.requestID = requestID;
        reply.path = path;
        reply.status = status;
        if (!data.empty()) {
            reply.storage.reset(new (std::nothrow) std::uint8_t[data.size()]);
            if (!reply.storage)
                return;
            std::ranges::copy(data, reply.storage.get());
            reply.length = data.size();
        }
        noteRemoteFile(std::move(reply));
    }
#endif

    /// Where the connection is: connecting, connected, dropped, or gone.
    ///
    /// Read when ``EventKind::RemoteStateChanged`` drains. The event carries
    /// the same value, so a frontend that acts on the event needs neither —
    /// this is for one that repaints on its own schedule and wants the
    /// current answer rather than the last one it happened to see.
    ///
    /// - Returns: The state; Idle for a session that is not remote at all.
    [[nodiscard]] Ubiquitty::Session::State remoteState() const
    {
        // Asked of the core rather than mirrored here. A second copy of a state
        // machine's current state is a second thing to forget to update, and
        // the core is where the transitions happen.
        return m_remote && m_remote->core ? m_remote->core->state() : Ubiquitty::Session::State::Idle;
    }

    /// Whether this pane's host offers the Command key on its native keyboard.
    /// A local pane or a remote pane without a host Welcome offers none.
    [[nodiscard]] bool shouldShowCommandAccessory() const
    {
        return m_remote && m_remote->core && m_remote->core->hasMacOSHost();
    }

    /// Reports whether the active remote transport uses QUIC.
    ///
    /// - Returns: Whether the remote Core selected QUIC.
    [[nodiscard]] bool isRemoteUsingQUIC() const
    {
        return m_remote && m_remote->core && m_remote->core->isUsingQUIC();
    }

    /// The physical carrier currently owned by the Sync connection.
    [[nodiscard]] Ubiquitty::Session::CarrierFact remoteCarrierFact() const
    {
        return m_remote && m_remote->core ? m_remote->core->carrierFact() : Ubiquitty::Session::CarrierFact {};
    }

    /// Builds the Sync Debug report for this session's remote connection.
    ///
    /// Read on the thread the remote session runs on — the main thread, the
    /// same as ``remoteState`` — because the trace and the carrier's
    /// statistics are touched there and nowhere else.
    ///
    /// - Returns: The report; an empty one, in state Idle, for a session that
    ///   is not remote.
    [[nodiscard]] Ubiquitty::Session::SyncTraceReport remoteSyncTraceReport() const
    {
        if (!m_remote || !m_remote->core)
            return {};
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        return Ubiquitty::Session::makeSyncTraceReport(*m_remote->core, now.count());
    }

    /// Clears the remote connection's trace counters and rings, so a Sync
    /// Debug window starts counting from now.
    void resetRemoteSyncTrace()
    {
        if (m_remote && m_remote->core)
            m_remote->core->resetTrace();
    }

    /// The daemon session id this connection is attached to.
    ///
    /// Zero until the host pins one, which is when
    /// ``EventKind::RemoteSessionEstablished`` fires. Persist it to reattach
    /// this shell after a restart rather than spawning a fresh one.
    ///
    /// - Returns: The id, or 0.
    [[nodiscard]] std::uint32_t remoteSessionID() const
    {
        return m_remote && m_remote->core ? m_remote->core->sessionID() : 0;
    }

    /// The daemon socket this connection's session lives on, as the host named
    /// it in its handshake.
    ///
    /// Persist it beside ``remoteSessionID`` so a restore pins the reattach to
    /// that daemon even after a newer binary's daemon binds another socket.
    ///
    /// - Returns: The socket's basename, or empty for a local session and
    ///   before the handshake.
    [[nodiscard]] std::string remoteSocketName() const
    {
        return m_remote && m_remote->core ? m_remote->core->socketName() : std::string();
    }

    // ---- the host's workspace, mirrored ----
    //
    // Read beside ``EventKind::WorkspaceChanged``. Every id is the host's, so a
    // verb sent back names what the host holds; every answer is the empty
    // case before the first layout, or on a session that is not remote.

    /// The host's id for the workspace this session belongs to; 0 before the first layout.
    [[nodiscard]] std::uint32_t remoteWorkspaceID() const { return m_remote && m_remote->core ? m_remote->core->workspaceID() : 0; }
    /// Borrows the physical owner's admitted structure on the session thread.
    /// Valid until its next structure or connection revision, never a cold preview.
    [[nodiscard]] const Session::TreeWorkspace* remoteWorkspaceStructureSnapshot() const
    {
        return m_remote && m_remote->core ? m_remote->core->workspaceStructureSnapshot() : nullptr;
    }
    /// Returns the daemon process identity required to restore this remote session.
    [[nodiscard]] std::uint64_t remoteDaemonInstanceID() const
    {
        if (m_remote && m_remote->workspaceConnection) {
            return m_remote->workspaceConnection->connection().daemonInstanceID();
        }
        if (m_sharing) {
            return m_sharing->daemonInstanceID;
        }
        return m_remote && m_remote->core ? m_remote->core->daemonInstanceID() : 0;
    }
    /// The predecessor named by an admitted remote Workspace restore, or zero otherwise.
    [[nodiscard]] std::uint64_t remoteRestoredFromDaemonInstanceID() const
    {
        return m_remote && m_remote->core ? m_remote->core->restoredFromDaemonInstanceID() : 0;
    }
    /// How many surfaces the mirror holds.
    [[nodiscard]] std::ptrdiff_t remoteWorkspaceSurfaceCount() const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        return mirror != nullptr ? mirror->surfaceCount() : 0;
    }
    /// The surface at `index` of the mirror, in creation order.
    ///
    /// - Parameter index: Below ``remoteWorkspaceSurfaceCount``.
    /// - Returns: The surface, or `invalidSurface` past the end.
    [[nodiscard]] Session::Layout::SurfaceID remoteWorkspaceSurfaceAt(std::ptrdiff_t index) const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        return mirror != nullptr ? mirror->surfaceIDAt(static_cast<int>(index)) : Session::Layout::invalidSurface;
    }
    /// The mirror's active surface.
    [[nodiscard]] Session::Layout::SurfaceID remoteWorkspaceActiveSurface() const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        return mirror != nullptr ? mirror->activeSurface() : Session::Layout::invalidSurface;
    }
    /// How many tabs a surface of the mirror holds.
    ///
    /// - Parameter surface: A surface of the mirror.
    /// - Returns: Its tab count, 0 for an unknown surface.
    [[nodiscard]] std::ptrdiff_t remoteWorkspaceTabCount(Session::Layout::SurfaceID surface) const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        return mirror != nullptr ? mirror->tabCount(surface) : 0;
    }
    /// The tab at `index` of a surface of the mirror, in the order they are held.
    ///
    /// - Parameters:
    ///   - surface: A surface of the mirror.
    ///   - index: Below ``remoteWorkspaceTabCount``.
    /// - Returns: The tab, or `invalidTab` past the end.
    [[nodiscard]] Session::Layout::TabID remoteWorkspaceTabAt(Session::Layout::SurfaceID surface, std::ptrdiff_t index) const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        return mirror != nullptr ? mirror->tabAt(surface, static_cast<int>(index)) : Session::Layout::invalidTab;
    }
    /// Returns the host's workspace name.
    ///
    /// - Returns: The authoritative name, empty before attachment or for an unnamed workspace.
    [[nodiscard]] std::string remoteWorkspaceName() const
    {
        return m_remote && m_remote->core ? m_remote->core->workspaceName() : std::string {};
    }
    /// Reports whether the host recreated the attached workspace from its
    /// record after it restarted.
    ///
    /// - Returns: Whether every pane's shell is fresh and its earlier screen
    ///   gone, which a frontend tells the person once; false before attachment.
    [[nodiscard]] bool isRemoteWorkspaceRestoredAfterHostRestart() const
    {
        return m_remote && m_remote->core && m_remote->core->isWorkspaceRestoredAfterHostRestart();
    }
    /// Returns the host's custom title for a tab.
    ///
    /// - Parameters:
    ///   - surface: The surface holding the tab.
    ///   - tab: The tab to name.
    /// - Returns: The custom title, empty for an automatic title or an unknown tab.
    [[nodiscard]] std::string remoteWorkspaceTabTitle(Session::Layout::SurfaceID surface, Session::Layout::TabID tab) const
    {
        const auto* mirror = remoteWorkspace();
        return mirror ? std::string(mirror->tabTitle(surface, tab)) : std::string {};
    }
    /// Returns the host's custom title for a tab, wherever the tab stands.
    ///
    /// A hosted window knows the tab it projects, not which of the host's
    /// windows holds it: a tab moved between the host's windows keeps its id
    /// and its title.
    ///
    /// - Parameter tab: The tab.
    /// - Returns: The custom title, empty for an automatic title or an unknown tab.
    [[nodiscard]] std::string remoteWorkspaceCustomTabTitle(Session::Layout::TabID tab) const
    {
        const auto* mirror = remoteWorkspace();
        if (!mirror)
            return {};
        for (int index = 0; index < mirror->surfaceCount(); ++index) {
            const auto surface = mirror->surfaceIDAt(index);
            if (mirror->paneTree(surface, tab))
                return std::string(mirror->tabTitle(surface, tab));
        }
        return {};
    }
    /// Requests a tab title change from the authoritative host.
    ///
    /// - Parameters:
    ///   - tab: The tab to rename.
    ///   - title: The custom title, empty to restore automatic naming.
    /// - Returns: Whether the request was sent; the host confirms it through the workspace's structure.
    [[nodiscard]] bool remoteWorkspaceRenameTab(Session::Layout::TabID tab, const std::string& title)
    {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = Ubiquitty::Session::WorkspaceIntentKind::RenameTab;
        verb.tabID = std::to_underlying(tab);
        verb.title = Session::normalizedCustomTitle(title);
        return sendRemoteWorkspaceVerb(std::move(verb));
    }
    /// Requests a workspace name change from the authoritative host.
    ///
    /// - Parameter name: The requested workspace name, empty for an unnamed workspace.
    /// - Returns: Whether the request was sent; the host confirms it through the workspace's structure.
    [[nodiscard]] bool remoteWorkspaceRename(const std::string& name)
    {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace;
        verb.title = name;
        return sendRemoteWorkspaceVerb(std::move(verb));
    }
    /// The active tab of a surface of the mirror.
    ///
    /// - Parameter surface: A surface of the mirror.
    /// - Returns: The tab, or `invalidTab` for an unknown surface.
    [[nodiscard]] Session::Layout::TabID remoteWorkspaceActiveTab(Session::Layout::SurfaceID surface) const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        return mirror != nullptr ? mirror->activeTab(surface) : Session::Layout::invalidTab;
    }
    /// The tab a window of the mirror shows in front: the one a person here
    /// asked for while the host has yet to answer, else the host's active tab.
    ///
    /// - Parameter surface: A surface of the mirror.
    /// - Returns: The tab, or `invalidTab` for an unknown surface.
    [[nodiscard]] Session::Layout::TabID remoteWorkspaceFrontTab(Session::Layout::SurfaceID surface) const
    {
        return m_remote && m_remote->core ? m_remote->core->frontTab(surface) : Session::Layout::invalidTab;
    }
    /// The pane zoomed on a surface of the mirror.
    ///
    /// - Parameter surface: A surface of the mirror.
    /// - Returns: The pane, or `invalidPane` when none is zoomed.
    [[nodiscard]] Session::Layout::PaneID remoteWorkspaceZoomedPane(Session::Layout::SurfaceID surface) const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        return mirror != nullptr ? mirror->zoomedPane(surface) : Session::Layout::invalidPane;
    }
    /// The pane tree of one tab of the mirror, copied.
    ///
    /// - Parameters:
    ///   - surface: A surface of the mirror.
    ///   - tab: A tab of that surface.
    /// - Returns: The tree, or a one-pane tree for an unknown tab.
    [[nodiscard]] Ubiquitty::Panes::Tree remoteWorkspacePaneTreeSnapshot(Session::Layout::SurfaceID surface, Session::Layout::TabID tab) const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        const Ubiquitty::Panes::Tree* tree = mirror != nullptr ? mirror->paneTree(surface, tab) : nullptr;
        return tree != nullptr ? *tree : Ubiquitty::Panes::Tree(1);
    }
    /// The host session drawing into a pane of the mirror.
    ///
    /// - Parameter pane: A pane of the mirror.
    /// - Returns: Its session's id, or 0 for a pane the mirror does not hold.
    [[nodiscard]] std::uint32_t remoteWorkspaceSessionForPane(Session::Layout::PaneID pane) const
    {
        return m_remote && m_remote->core ? m_remote->core->sessionForPane(pane) : 0;
    }
    /// This session's own pane in the mirror.
    ///
    /// - Returns: The pane, or `invalidPane` before the first layout.
    [[nodiscard]] Session::Layout::PaneID remoteWorkspacePane() const
    {
        return m_remote && m_remote->core ? m_remote->core->paneForSession(m_remote->core->sessionID()) : Session::Layout::invalidPane;
    }
    /// The surface of the mirror holding this session's pane.
    [[nodiscard]] Session::Layout::SurfaceID remoteWorkspaceSurface() const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        return mirror != nullptr ? mirror->surfaceHolding(remoteWorkspacePane()) : Session::Layout::invalidSurface;
    }
    /// The tab of the mirror holding this session's pane.
    [[nodiscard]] Session::Layout::TabID remoteWorkspaceTab() const
    {
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        return mirror != nullptr ? mirror->tabHolding(remoteWorkspaceSurface(), remoteWorkspacePane()) : Session::Layout::invalidTab;
    }

    /// Asks the host to add a tab to the surface holding this session's pane.
    ///
    /// The host answers with a new layout, through ``EventKind::WorkspaceChanged``.
    /// The verb names this session's pane as the one the tab opens from, and
    /// the host dresses the new session in that pane's theme.
    ///
    /// - Returns: Whether the verb was sent.
    bool remoteWorkspaceAddTab() { return sendRemoteWorkspaceVerb(Ubiquitty::Session::WorkspaceIntentKind::AddTab, Session::Layout::invalidTab, remoteWorkspacePane()); }

    /// Describes the host's answer to the latest window/tab creation, split, or pane close.
    struct RemoteWorkspacePaneChange {
        /// Distinguishes host confirmation from an interrupted request with an unknown outcome.
        enum class UBIQUITTY_CLOSED_ENUM Outcome : std::uint8_t {
            Idle,
            Pending,
            Accepted,
            Refused,
            Interrupted
        };
        Outcome outcome { Outcome::Idle };
        std::uint32_t createdPaneID { 0 };
    };

    /// Consumes a settled pane change while preserving an outstanding request.
    ///
    /// The pane identifier comes from the correlated host reply, so a layout
    /// change caused by another client cannot be mistaken for this request.
    ///
    /// - Returns: The latest outcome and the minted pane, if any.
    [[nodiscard]] RemoteWorkspacePaneChange takeRemoteWorkspacePaneChange()
    {
        if (!m_remote) {
            return {};
        }
        const auto change = m_remote->paneChange;
        if (change.outcome != RemoteWorkspacePaneChange::Outcome::Pending) {
            m_remote->paneChange = {};
        }
        return change;
    }
    /// Asks the host to add a window to this session's shared workspace,
    /// opened from this session's pane as ``remoteWorkspaceAddTab`` opens a tab.
    ///
    /// - Returns: Whether the workspace request was queued.
    bool remoteWorkspaceAddSurface() { return sendRemoteWorkspaceVerb(Ubiquitty::Session::WorkspaceIntentKind::AddSurface, Session::Layout::invalidTab, remoteWorkspacePane()); }

    /// Reports that a person brought this session's tab to the front of its
    /// window, which asks the host to make it the window's active tab.
    ///
    /// The fact is the frontend's and the decision the host's: the tab is in
    /// front for every client of the workspace once the host's structure says
    /// so, and ``remoteWorkspaceFrontTab`` answers it meanwhile.
    ///
    /// - Returns: Whether an ask went out; false when the tab is already in
    ///   front or asked for, and before the first structure.
    bool remoteWorkspaceBringTabToFront()
    {
        return m_remote && m_remote->core && m_remote->core->bringTabToFront(remoteWorkspaceSurface(), remoteWorkspaceTab());
    }
    /// Asks the host to close a tab of this session's surface, ending the sessions behind its panes.
    ///
    /// - Parameter tab: A tab of that surface.
    /// - Returns: Whether the verb was sent.
    bool remoteWorkspaceCloseTab(Session::Layout::TabID tab)
    {
        if (!m_remote || m_remote->closeRequestID)
            return false;
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = Ubiquitty::Session::WorkspaceIntentKind::CloseTab;
        verb.surfaceID = std::to_underlying(remoteWorkspaceSurface());
        verb.tabID = std::to_underlying(tab);
        m_remote->closeRequestID = verb.surfaceID && m_remote->core ? m_remote->core->sendWorkspaceVerb(verb) : 0;
        m_remote->closeOutcome = m_remote->closeRequestID ? RemoteWorkspaceCloseOutcome::Pending : RemoteWorkspaceCloseOutcome::Refused;
        if (!m_remote->closeRequestID)
            noteRemoteEvent(EventKind::WorkspaceChanged);
        return m_remote->closeRequestID != 0;
    }

    /// Captures this pane's host window and every session under its tabs.
    /// Returns zero until the host's complete layout is available.
    [[nodiscard]] std::uint64_t prepareRemoteWindowClose()
    {
        if (!m_remote || !m_remote->core || m_remote->closeRequestID
            || m_nextWindowCloseToken == std::numeric_limits<std::uint64_t>::max())
            return 0;
        const auto token = ++m_nextWindowCloseToken;
        return m_remote->core->prepareWindowClose(token) ? token : 0;
    }

    /// Returns zero after reconnect, retargeting, or an observed topology change.
    [[nodiscard]] std::uint64_t pendingRemoteWindowCloseToken() const
    {
        return m_remote && m_remote->core && !m_remote->closeRequestID ? m_remote->core->pendingWindowCloseToken() : 0;
    }
    /// Returns the captured window's full session count, including hidden tabs.
    [[nodiscard]] std::uint32_t pendingRemoteWindowCloseSessionCount() const
    {
        return m_remote && m_remote->core && !m_remote->closeRequestID ? m_remote->core->pendingWindowCloseSessionCount() : 0;
    }
    /// Consumes one answer; only a current approval requests a destructive host close.
    [[nodiscard]] bool answerRemoteWindowClose(std::uint64_t token, Session::RemoteWindowCloseDecision decision)
    {
        if (!m_remote || !m_remote->core || (m_remote->closeRequestID && decision != Session::RemoteWindowCloseDecision::Cancel))
            return false;
        const bool wasCurrent = token && pendingRemoteWindowCloseToken() == token;
        const auto session = weakPtr();
        const auto core = m_remote->core->weakPtr();
        const auto requestID = m_remote->core->answerWindowClose(token, decision);
        if (!session || !core || !session->m_remote || session->m_remote->core.get() != core.get())
            return false;
        if (decision == Session::RemoteWindowCloseDecision::Cancel || !wasCurrent)
            return false;
        session->m_remote->closeRequestID = requestID;
        session->m_remote->closeOutcome = requestID ? RemoteWorkspaceCloseOutcome::Pending : RemoteWorkspaceCloseOutcome::Refused;
        if (!requestID)
            session->noteRemoteEvent(EventKind::WorkspaceChanged);
        return requestID != 0;
    }

    /// Describes the authority's answer to a requested tab or window close.
    enum class RemoteWorkspaceCloseOutcome : std::uint8_t {
        Idle,
        Pending,
        Accepted,
        Refused
    };

    /// Returns the current close outcome without consuming its event.
    [[nodiscard]] RemoteWorkspaceCloseOutcome remoteWorkspaceCloseOutcome() const
    {
        return m_remote ? m_remote->closeOutcome : RemoteWorkspaceCloseOutcome::Idle;
    }

    /// Consumes a settled answer, leaving an in-flight request pending.
    [[nodiscard]] RemoteWorkspaceCloseOutcome takeRemoteWorkspaceCloseOutcome()
    {
        if (!m_remote)
            return RemoteWorkspaceCloseOutcome::Idle;
        const auto outcome = m_remote->closeOutcome;
        if (outcome != RemoteWorkspaceCloseOutcome::Pending)
            m_remote->closeOutcome = RemoteWorkspaceCloseOutcome::Idle;
        return outcome;
    }
    /// Asks the host to move focus one pane over in this session's tab.
    ///
    /// - Parameter direction: Which way.
    /// - Returns: Whether the verb was sent.
    bool remoteWorkspaceMoveFocus(Ubiquitty::Panes::Direction direction)
    {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = Ubiquitty::Session::WorkspaceIntentKind::MoveFocus;
        verb.direction = static_cast<std::uint8_t>(direction);
        return sendRemoteWorkspaceVerb(verb);
    }
    /// Asks the host to set a divider of this session's tab.
    ///
    /// - Parameters:
    ///   - splitIndex: The split node's index in the tab's tree.
    ///   - ratio: The first child's fraction.
    /// - Returns: Whether the verb was sent.
    bool remoteWorkspaceSetDividerRatio(std::int32_t splitIndex, float ratio)
    {
        if (splitIndex < 0 || splitIndex >= Ubiquitty::Panes::maxNodes)
            return false;
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = Ubiquitty::Session::WorkspaceIntentKind::SetDividerRatio;
        verb.splitIndex = static_cast<std::uint16_t>(splitIndex);
        verb.ratio = ratio;
        return sendRemoteWorkspaceVerb(verb);
    }

    /// Identifies the currently adopted remote topology for deferred UI input.
    /// - Returns: Its generation, or zero without a current remote mirror.
    [[nodiscard]] std::uint64_t remoteWorkspaceMirrorGeneration() const
    {
        return m_remote ? m_remote->core->workspaceMirrorGeneration() : 0;
    }

    /// Takes the pairing of the windows, tabs and panes a frontend keeps by
    /// the mirror's ids with the ones a host restored in their places after
    /// the daemon that served them stopped.
    ///
    /// The workspace's connection is shared by every pane of it, and so is
    /// the pairing: the first frontend to take it is the one that moves its
    /// keys, once. See ``Session/RemoteSessionCore/takeRestoration``.
    ///
    /// - Returns: The restoration since the last take; not restored when
    ///   there was none.
    [[nodiscard]] Session::Layout::Restoration takeRemoteWorkspaceRestoration()
    {
        return m_remote && m_remote->core ? m_remote->core->takeRestoration() : Session::Layout::Restoration {};
    }

    /// Why the connection failed, in the host's own words.
    ///
    /// Empty unless ``remoteState`` is Failed. The detail is a string and an
    /// event is scalars, so it is read beside the flag rather than carried in
    /// it — the same split ``lastPastePushPath`` takes.
    ///
    /// - Returns: The reason, or an empty string. By value: a reference to a
    ///   std::string does not cross the Swift seam.
    [[nodiscard]] std::string lastRemoteFailure() const { return m_lastRemoteFailure; }

    /// Why the connection last changed state, whatever the state.
    ///
    /// A reconnect carries its cause too — a handshake that timed out, no
    /// transport, a moved path — and a log that names it tells a slow network
    /// from a refusing host. Not for the status line: the words are Core's
    /// own, not a sentence for a person.
    ///
    /// - Returns: The detail, or an empty string when the transition gave none.
    [[nodiscard]] std::string lastRemoteTransitionDetail() const { return m_lastRemoteTransitionDetail; }

    /// Whether the connection failed because the host would not hand back the
    /// session it was asked to reattach.
    ///
    /// The two Failed states a frontend has to word differently: a host that
    /// died is one to retry, and a session the host will not give back is one
    /// to replace — by listing what it does hold, or by attaching fresh.
    /// ``lastRemoteFailure`` names the session in words; this is the same fact
    /// as a flag, so a frontend branches on it rather than parsing a sentence.
    ///
    /// Cleared by any state that is not Failed, as the reason is.
    ///
    /// - Returns: Whether the last failure was a refused reattach.
    [[nodiscard]] bool wasReattachRefused() const { return m_wasReattachRefused; }

    /// Which session the host would not hand back.
    ///
    /// - Returns: The id, or 0 when no reattach was refused.
    [[nodiscard]] std::uint32_t refusedReattachSessionID() const { return m_refusedReattachSessionID; }

    /// The last refusal the host answered this session's connection with.
    ///
    /// Read off the physical connection a hosted workspace's panes share,
    /// because that is the connection the host refused.
    ///
    /// - Returns: The refusal, with ``Session::HostRefusal::hasRefusal`` clear
    ///   when the host refused nothing.
    [[nodiscard]] Ubiquitty::Session::HostRefusal lastHostRefusal() const
    {
        if (!m_remote)
            return {};
        if (m_remote->workspaceConnection)
            return m_remote->workspaceConnection->connection().lastRefusal();
        return m_remote->core ? m_remote->core->lastRefusal() : Ubiquitty::Session::HostRefusal {};
    }

    /// Makes the next attach update the host before it dials anything.
    ///
    /// Called before the attach: the connection that attach starts installs
    /// this app's host first and dials only once the install has succeeded;
    /// see ``Session::RemoteSessionCore::updateHostBeforeConnect``. A window
    /// refused for its version is replaced by one that does this, rather than
    /// updating in place, because the refused one is pinned to the daemon of
    /// the host being replaced.
    ///
    /// - Parameter platformArch: The host's `uname -sm`, empty when the
    ///   refusal did not name it.
    void requestHostUpdateBeforeAttach(const std::string& platformArch) { m_hostUpdateBeforeAttachPlatform = platformArch; }

    /// Sets the access token the next attach presents, and every reconnect
    /// after it.
    ///
    /// Held by the session rather than handed to an attach door, because the
    /// physical connection is built inside the door and a token set after it
    /// starts would miss its first HELLO.
    ///
    /// - Parameter token: The token, empty for none.
    void setRemoteAuthToken(const std::string& token)
    {
        m_remoteAuthToken = token;
        if (m_remote && m_remote->workspaceConnection)
            m_remote->workspaceConnection->connection().setAuthToken(token);
    }

    /// Asks a host what sessions it is holding, over a carrier the EMBEDDER
    /// builds.
    ///
    /// Separate from an attach and callable before one: a frontend that offers
    /// the human a choice has to know what there is to choose from before it
    /// binds this surface to any of it. The answer arrives as
    /// ``EventKind::HostSessionListChanged`` and is read as the host's tree,
    /// through ``hostOutline`` and ``decideHostAttach``.
    ///
    /// The factory and the loop are ``attachRemoteOverCarrier``'s, for the
    /// same reasons: the directory dials the host itself, and every answer it
    /// produces runs on the platform's loop.
    ///
    /// - Parameters:
    ///   - factoryAddress: The embedder's `Ubiquitty::Session::ChannelFactory`,
    ///     as an address. Borrowed, and it must outlive the session.
    ///   - loop: The platform's loop, where the directory's work runs.
    /// - Returns: Whether the question was asked.
    bool listHostSessions(std::uintptr_t factoryAddress, PostedWorkLoop loop)
    {
        if (factoryAddress == 0 || !loop.isComplete())
            return false;
        prepareDirectory(loop);
        // Rebuilt only for a factory this directory has not dialled. A second
        // ask over the same factory is a refresh, and tearing the core down to
        // build an identical one would drop an answer already in flight.
        if (!m_directory->core || m_directory->factoryAddress != factoryAddress) {
            m_directory->factoryAddress = factoryAddress;
            m_directory->core = std::make_unique<Ubiquitty::Session::SessionDirectoryCore>(
                &m_directory->dispatcher,
                reinterpret_cast<Ubiquitty::Session::ChannelFactory*>(factoryAddress),
                &m_directory->observer);
        }
        m_directory->didListingReportTree = false;
        return m_directory->core->refresh();
    }

    /// Asks a host what sessions it is holding, over a carrier this session
    /// SPAWNS.
    ///
    /// ``listHostSessions`` in every respect but where the carrier comes from,
    /// the same split the two attach doors take: a desktop frontend reaches a
    /// host by spawning `ssh … --attach` and has no `ChannelFactory` of its own
    /// to hand over, and building one outside Core would put a second spelling
    /// of the transport beside the one ``attachRemote`` already owns.
    ///
    /// The QUIC arguments are accepted and reach the carrier the attach path
    /// uses, but this spawned factory prefers TCP for a directory errand: one
    /// channel carries one HELLO and request, and a QUIC bootstrap would pay
    /// a spawn to save none. `SessionDirectoryCore` uses the supplied factory's
    /// control-carrier preference; native iOS factories may prefer QUIC.
    ///
    /// An overload rather than defaulted parameters, because the Swift seam
    /// imports no default arguments.
    ///
    /// - Parameters:
    ///   - shellCommand: The transport command (`ssh … --attach`).
    ///   - quicHost: As ``attachRemote``.
    ///   - quicBootstrapCommand: As ``attachRemote``.
    ///   - loop: The platform's loop, where the directory's work runs.
    /// - Returns: Whether the question was asked.
    bool listHostSessions(const std::string& shellCommand, const std::string& quicHost, const std::string& quicBootstrapCommand, PostedWorkLoop loop)
    {
        if (shellCommand.empty())
            return false;
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        m_directory->factory.shellCommand = shellCommand;
        m_directory->factory.quicHost = quicHost;
        m_directory->factory.quicBootstrapCommand = quicBootstrapCommand;
        // Through the borrowed door over the owned factory, so a refresh over
        // the same command finds the same address and keeps the core it built.
        // The address is the directory's own and lives exactly as long as it.
        return listHostSessions(reinterpret_cast<std::uintptr_t>(
                                    static_cast<Ubiquitty::Session::ChannelFactory*>(&m_directory->factory)),
            loop);
    }

    /// Follows a host's tree over a carrier the EMBEDDER builds: the tree
    /// arrives as ``EventKind::HostSessionListChanged`` once it is whole, and
    /// again whenever a workspace's structure or a pane's facts change, until
    /// ``stopWatchingHostSessions``.
    ///
    /// A connection of its own, beside the one the listings and the changes
    /// take, so a watch never makes a terminate or a rename wait, and a
    /// carrier that drops is dialled again without a listing being asked.
    /// While the watch is connected ``hostSessionTreeFreshness`` is
    /// ``TreeFreshness::Live``; once it loses its host the tree it left is
    /// ``TreeFreshness::Stale``, never drawn as the host's word.
    ///
    /// - Parameters:
    ///   - factoryAddress: As ``listHostSessions``. Borrowed: it must outlive
    ///     the watch, so the embedder stops the watch before freeing it.
    ///   - loop: As ``listHostSessions``.
    /// - Returns: Whether a watch is running: this one or one already out
    ///   over the same factory.
    bool watchHostSessions(std::uintptr_t factoryAddress, PostedWorkLoop loop)
    {
        if (factoryAddress == 0 || !loop.isComplete())
            return false;
        prepareDirectory(loop);
        if (m_directory->watcher && m_directory->watcherFactoryAddress == factoryAddress && m_directory->watcher->isWatching())
            return true;
        stopWatchingHostSessions();
        m_directory->watcherFactoryAddress = factoryAddress;
        m_directory->watcher = std::make_unique<Ubiquitty::Session::SessionDirectoryCore>(
            &m_directory->dispatcher,
            reinterpret_cast<Ubiquitty::Session::ChannelFactory*>(factoryAddress),
            &m_directory->watchObserver);
        return m_directory->watcher->watch();
    }

    /// Stops following the host, and lets go of the factory the watch
    /// borrowed. The tree it followed stays, as one nothing follows.
    void stopWatchingHostSessions()
    {
        if (!m_directory)
            return;
        if (m_directory->watcher) {
            m_directory->watcher->stopWatching();
            m_directory->watcher.reset();
            m_directory->watcherFactoryAddress = 0;
        }
        if (m_directory->freshness == HostDirectoryClient::TreeFreshness::Live) {
            m_directory->freshness = HostDirectoryClient::TreeFreshness::Stale;
            noteRemoteEvent(EventKind::HostSessionListChanged);
        }
    }

    /// How current the tree ``hostOutline`` reads is: none, what a listing
    /// answered, followed by a connected watch, or left by a watch that lost
    /// its host.
    [[nodiscard]] HostDirectoryClient::TreeFreshness hostSessionTreeFreshness() const
    {
        if (!m_directory || !m_directory->tree)
            return HostDirectoryClient::TreeFreshness::None;
        return m_directory->freshness;
    }

    /// A number that moves whenever the tree ``hostOutline`` reads is
    /// replaced: by a listing's answer, or by one that had none.
    ///
    /// Read when ``EventKind::HostSessionListChanged`` drains.
    ///
    /// - Returns: The count; zero before any tree.
    [[nodiscard]] std::uint64_t hostSessionTreeRevision() const
    {
        return m_directory ? m_directory->treeRevision : 0;
    }

    /// Live sibling daemons reported by the authenticated host listing.
    [[nodiscard]] Session::HostInstanceList hostInstances() const
    {
        return { m_directory && m_directory->tree ? m_directory->tree->hostInstances : std::vector<Session::HostInstanceInfo> {} };
    }

    /// Returns the daemon socket retained with the current directory answer.
    [[nodiscard]] std::string hostSessionDaemonSocketName() const
    {
        return m_directory ? m_directory->listedDaemonSocketName : std::string();
    }

    /// Identifies the daemon process that supplied the current directory answer.
    [[nodiscard]] std::uint64_t hostSessionDaemonInstanceID() const
    {
        return m_directory ? m_directory->listedDaemonInstanceID : 0;
    }

    /// The rows a frontend draws for what the host last said it holds: its
    /// workspaces, their windows, tabs and panes.
    ///
    /// Core decides the rows. The title compositions and the tag rules are
    /// the live preferences', and ages are measured against the host's
    /// clock, kept running by this machine's since the host stated it.
    ///
    /// - Parameter request: Who is looking, which rows are closed and what
    ///   is searched for. A negative `themeIndex` draws in this session's
    ///   own theme.
    /// - Returns: The rows; none before a host answered with a tree.
    [[nodiscard]] App::SessionOutline hostOutline(const HostOutlineRequest& request)
    {
        if (!m_directory || !m_directory->tree)
            return {};
        const Ubiquitty::Session::SessionTree& tree = *m_directory->tree;
        const std::int64_t readerNow = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::int64_t hostNow = std::max(readerNow + m_directory->hostClockLeadMilliseconds, static_cast<std::int64_t>(tree.hostNowUnixMilliseconds));
        return buildHostOutline(tree, Preferences::values(), request.themeIndex < 0 ? statusColors() : readerStatusColors(Preferences::values(), request.themeIndex), request, static_cast<std::uint64_t>(hostNow));
    }

    /// ``hostOutline`` for a caller across a language seam: the same rows,
    /// read one field of one row a call.
    ///
    /// - Parameter request: As ``hostOutline``.
    /// - Returns: The rows.
    [[nodiscard]] HostOutlineRows hostOutlineRows(const HostOutlineRequest& request)
    {
        return HostOutlineRows(hostOutline(request));
    }

    /// A request with a row and every row under it opened, on the tree this
    /// session's listed host last reported: what "expand all under this
    /// row" leaves a reader with.
    ///
    /// - Parameters:
    ///   - request: The reader's request.
    ///   - kind: The row's identity's kind.
    ///   - number: The identity's number.
    /// - Returns: The request, changed when the tree holds the row.
    [[nodiscard]] HostOutlineRequest hostOutlineRequestOpeningAllUnder(HostOutlineRequest request, std::uint8_t kind, std::uint64_t number)
    {
        if (m_directory && m_directory->tree)
            (void)request.openAllUnder(*m_directory->tree, kind, number);
        return request;
    }

    /// ``hostOutlineRequestOpeningAllUnder`` for a row that has no identity.
    ///
    /// - Parameters:
    ///   - request: The reader's request.
    ///   - workspaceID: The restorable workspace.
    ///   - level: 1 for a window's row, 2 for a tab's.
    ///   - window: The window's index in the record.
    ///   - tab: The tab's index in that window; zero on a window's row.
    /// - Returns: The request, changed when the tree holds the row.
    [[nodiscard]] HostOutlineRequest hostOutlineRequestOpeningAllUnderRecorded(HostOutlineRequest request, std::uint32_t workspaceID, std::uint8_t level, std::size_t window, std::size_t tab)
    {
        if (m_directory && m_directory->tree)
            (void)request.openAllUnderRecorded(*m_directory->tree, workspaceID, level, window, tab);
        return request;
    }

    /// Whether the host this session listed answered with what it holds.
    ///
    /// A listing that has not come back, or one that failed, is not an
    /// empty host: ``decideHostAttach`` reads neither as a host holding
    /// nothing.
    ///
    /// - Returns: Whether a tree from the host's answer is held.
    [[nodiscard]] bool hasHostAnsweredListing() const { return m_directory && m_directory->tree; }

    /// The connect to make to the host this session listed: Core's rule
    /// over the tree the host last answered with and this host's remembered
    /// pairing.
    ///
    /// - Parameters:
    ///   - hostKey: The embedder's name for the host, whose remembered
    ///     pairing is recalled.
    ///   - predefinedWorkspaceName: The name the saved host carries, empty
    ///     when it names none.
    ///   - chosenSessionID: The listed session the person selected, or 0.
    /// - Returns: The connect to make; ``Session::AttachPlan::AwaitListing``
    ///   or the remembered session while the host has not answered.
    [[nodiscard]] Ubiquitty::Session::AttachDecision decideHostAttach(const std::string& hostKey, const std::string& predefinedWorkspaceName, std::uint32_t chosenSessionID) const
    {
        return HostDirectory::decide(hostKey, predefinedWorkspaceName, m_directory ? m_directory->tree.get() : nullptr, chosenSessionID);
    }

    /// Words the question a request that ends sessions on the listed host
    /// is asked with, and gives it the token the request is sent with.
    ///
    /// The question is read through the accessors below until the next one
    /// is asked. A frontend shows the title, the body and the two buttons as
    /// they are and makes the safe button the default.
    ///
    /// - Parameters:
    ///   - request: A `Runtime::DestructiveRequest`.
    ///   - targetKind: The target's `Session::Identity::ObjectKind`: a window
    ///     to close; a session, the pane it draws into, or a workspace to
    ///     terminate.
    ///   - targetNumber: The target's number.
    ///   - hostName: What the host is called in the question.
    /// - Returns: The token; zero when there is nothing to ask, and
    ///   ``hostDestructiveConfirmationRefusal`` says why.
    std::uint64_t askHostDestructiveConfirmation(std::uint8_t request, std::uint8_t targetKind, std::uint64_t targetNumber, const std::string& hostName)
    {
        if (!m_directory)
            return 0;
        DestructiveConfirmation& asked = m_directory->lastConfirmation;
        asked = {};
        if (request < std::to_underlying(DestructiveRequest::CloseWindow) || request > std::to_underlying(DestructiveRequest::TerminateWorkspace) || targetKind >= Ubiquitty::Session::Identity::objectKindCount) {
            asked.whyNot = "This is not a request this build asks about.";
            return 0;
        }
        asked = m_directory->structure.confirmations().ask(m_directory->tree.get(), static_cast<DestructiveRequest>(request), { static_cast<Ubiquitty::Session::Identity::ObjectKind>(targetKind), targetNumber }, hostName);
        return asked.token;
    }

    /// The question ``askHostDestructiveConfirmation`` last worded.
    [[nodiscard]] std::string hostDestructiveConfirmationTitle() const { return m_directory ? m_directory->lastConfirmation.title : std::string(); }
    /// What it says ends.
    [[nodiscard]] std::string hostDestructiveConfirmationBody() const { return m_directory ? m_directory->lastConfirmation.body : std::string(); }
    /// Its button that ends nothing.
    [[nodiscard]] std::string hostDestructiveConfirmationSafeButton() const { return m_directory ? m_directory->lastConfirmation.safeButton : std::string(); }
    /// Its button that goes through with it.
    [[nodiscard]] std::string hostDestructiveConfirmationDestructiveButton() const { return m_directory ? m_directory->lastConfirmation.destructiveButton : std::string(); }
    /// How many sessions it says end.
    [[nodiscard]] std::uint32_t hostDestructiveConfirmationSessionCount() const { return m_directory ? m_directory->lastConfirmation.sessionCount() : 0; }
    /// Why there was nothing to ask, as a sentence; empty when there was.
    [[nodiscard]] std::string hostDestructiveConfirmationRefusal() const { return m_directory ? m_directory->lastConfirmation.whyNot : std::string("This session has listed no host."); }

    /// Asks the host to end a session, or every session of a workspace.
    ///
    /// Sent only with the token ``askHostDestructiveConfirmation`` returned
    /// for terminating this target, and only while it would end the sessions
    /// the question named. Each session is one terminate, sent as the one
    /// before it settles, and each arrives as its own
    /// ``EventKind::HostSessionTerminateSettled``.
    ///
    /// - Parameters:
    ///   - request: `Runtime::DestructiveRequest::TerminateSession` or
    ///     `TerminateWorkspace`.
    ///   - targetKind: The target's `Session::Identity::ObjectKind`.
    ///   - targetNumber: The target's number.
    ///   - confirmationToken: The token of the question that was answered.
    /// - Returns: How many sessions the host is asked to end; zero when none
    ///   is, and ``lastHostTerminateRefusal`` says why.
    std::uint32_t terminateHostSessions(std::uint8_t request, std::uint8_t targetKind, std::uint64_t targetNumber, std::uint64_t confirmationToken)
    {
        if (!m_directory)
            return 0;
        std::string& refusal = m_directory->lastTerminateRefusal;
        refusal.clear();
        const bool isTerminate = request == std::to_underlying(DestructiveRequest::TerminateSession) || request == std::to_underlying(DestructiveRequest::TerminateWorkspace); // bool-ok: a local verdict
        if (!isTerminate || targetKind >= Ubiquitty::Session::Identity::objectKindCount) {
            refusal = "This is not a request this build sends.";
            return 0;
        }
        if (!m_directory->owedTerminations.empty()) {
            refusal = "This host is still ending what was asked before. Try again in a moment.";
            return 0;
        }
        // Before the token is spent, so that a busy moment costs no second
        // question.
        if (!m_directory->core || m_directory->core->isBusy()) {
            refusal = "This host is answering another request. Try again in a moment.";
            return 0;
        }
        DestructiveConfirmations::Redemption redemption = m_directory->structure.confirmations().redeem(confirmationToken, static_cast<DestructiveRequest>(request), { static_cast<Ubiquitty::Session::Identity::ObjectKind>(targetKind), targetNumber }, m_directory->tree.get());
        if (!redemption.refusal.empty()) {
            refusal = std::move(redemption.refusal);
            return 0;
        }
        m_directory->owedTerminations = redemption.sessionIDs;
        sendOwedHostTerminations();
        return static_cast<std::uint32_t>(redemption.sessionIDs.size());
    }

    /// Why ``terminateHostSessions`` last sent nothing, as a sentence; empty
    /// when it sent.
    [[nodiscard]] std::string lastHostTerminateRefusal() const { return m_directory ? m_directory->lastTerminateRefusal : std::string("This session has listed no host."); }

    /// How the last terminate settled, as a ``Session::TerminateOutcome``.
    ///
    /// Read when ``EventKind::HostSessionTerminateSettled`` drains.
    ///
    /// - Returns: The outcome; Terminated (0) before anything has settled,
    ///   which a frontend never reads because the event has not fired.
    [[nodiscard]] std::uint8_t lastHostSessionTerminateOutcome() const
    {
        return static_cast<std::uint8_t>(m_events.hostSessionTerminateOutcome.load(std::memory_order_relaxed));
    }

    /// Which session the last settled terminate was about.
    ///
    /// Read beside ``lastHostSessionTerminateOutcome``: a frontend that showed
    /// the outcome without it would be reporting an answer with no question,
    /// and a picker holding several rows cannot tell which one it applies to.
    ///
    /// - Returns: The id, or 0 before anything has settled.
    [[nodiscard]] std::uint32_t lastHostSessionTerminateSessionID() const
    {
        return m_directory ? m_directory->lastTerminateSessionID : 0;
    }

    /// Asks the host this session listed for one change to its structure.
    ///
    /// The request is checked against the tree the last listing reported
    /// (`Runtime::StructureCalls`): with no tree, a target the tree does not
    /// hold, a change to the same target still out, or the directory busy
    /// with another errand, it is refused here in Core's words. Otherwise it
    /// is one call to the host. Either way it ends as
    /// ``EventKind::HostStructureSettled`` with ``lastHostStructureResult``
    /// holding the number returned here, and a served change is followed by
    /// a listing, so the outline shows it.
    ///
    /// - Parameter request: The change, typed.
    /// - Returns: The request's number; zero when this session has listed no
    ///   host, which is the one case with no result.
    std::uint64_t requestHostStructureChange(StructureRequest request)
    {
        if (!m_directory)
            return 0;
        StructureCalls::Admission admission = m_directory->structure.admit(request, m_directory->tree.get());
        if (!admission.refusal && !(m_directory->core && m_directory->core->call(admission.call)))
            admission.refusal = m_directory->structure.withdraw(admission.requestID, "This host is answering another request. Try again in a moment.");
        if (admission.refusal)
            noteHostStructureSettled(std::move(*admission.refusal));
        else
            m_directory->structureRequestInFlight = admission.requestID;
        return admission.requestID;
    }

    /// Sets or clears the label of a workspace, a window, a tab, a pane or a
    /// session of the listed host; see ``requestHostStructureChange``.
    ///
    /// - Parameters:
    ///   - target: The row's identity, or the identity of what the row
    ///     folded into itself when that is what is meant.
    ///   - label: The label; empty clears it.
    /// - Returns: The request's number.
    std::uint64_t setHostLabel(Ubiquitty::Session::Identity::ObjectID target, const std::string& label)
    {
        StructureRequest request;
        request.operation = StructureOperation::SetLabel;
        request.target = target;
        request.text = label;
        return requestHostStructureChange(std::move(request));
    }

    /// Replaces the tags of one of those; none clears them.
    ///
    /// - Parameters:
    ///   - target: What is tagged.
    ///   - tags: Its tags, in the order they are drawn.
    /// - Returns: The request's number.
    std::uint64_t setHostTags(Ubiquitty::Session::Identity::ObjectID target, const StructureTags& tags)
    {
        StructureRequest request;
        request.operation = StructureOperation::SetTags;
        request.target = target;
        request.tags = tags.names;
        return requestHostStructureChange(std::move(request));
    }

    /// Gives a workspace of the listed host its name, or takes it away.
    ///
    /// - Parameters:
    ///   - workspace: The workspace.
    ///   - name: The name; empty takes it away.
    /// - Returns: The request's number.
    std::uint64_t renameHostWorkspace(Ubiquitty::Session::Identity::ObjectID workspace, const std::string& name)
    {
        StructureRequest request;
        request.operation = StructureOperation::RenameWorkspace;
        request.target = workspace;
        request.text = name;
        return requestHostStructureChange(std::move(request));
    }

    /// Gives a tab of the listed host its name, or takes it away.
    ///
    /// - Parameters:
    ///   - tab: The tab.
    ///   - name: The name; empty takes it away.
    /// - Returns: The request's number.
    std::uint64_t renameHostTab(Ubiquitty::Session::Identity::ObjectID tab, const std::string& name)
    {
        StructureRequest request;
        request.operation = StructureOperation::RenameTab;
        request.target = tab;
        request.text = name;
        return requestHostStructureChange(std::move(request));
    }

    /// Puts a tab at a place among the tabs of a window of its workspace.
    ///
    /// - Parameters:
    ///   - tab: The tab.
    ///   - window: The window it is to stand in.
    ///   - index: The place it takes there, from zero.
    /// - Returns: The request's number.
    std::uint64_t moveHostTab(Ubiquitty::Session::Identity::ObjectID tab, Ubiquitty::Session::Identity::ObjectID window, std::uint32_t index)
    {
        StructureRequest request;
        request.operation = StructureOperation::MoveTab;
        request.target = tab;
        request.window = window;
        request.index = index;
        return requestHostStructureChange(std::move(request));
    }

    /// Closes a window of the listed host and ends the sessions in its panes.
    ///
    /// Refused here unless the token is the one
    /// ``askHostDestructiveConfirmation`` returned for closing this window
    /// and the window still holds the sessions the question named.
    ///
    /// - Parameters:
    ///   - window: The window.
    ///   - confirmationToken: The token of the question that was answered.
    /// - Returns: The request's number.
    std::uint64_t closeHostWindow(Ubiquitty::Session::Identity::ObjectID window, std::uint64_t confirmationToken)
    {
        StructureRequest request;
        request.operation = StructureOperation::CloseWindow;
        request.target = window;
        request.confirmationToken = confirmationToken;
        return requestHostStructureChange(std::move(request));
    }

    /// ``requestHostStructureChange`` for a caller across a language seam,
    /// which hands scalars: the typed wrappers on that side are the only
    /// callers, so no frontend writes an operation's number.
    ///
    /// - Parameters:
    ///   - operation: A `Runtime::StructureOperation`.
    ///   - targetKind: The target's `Session::Identity::ObjectKind`.
    ///   - targetNumber: The target's number.
    ///   - text: The label or the name, where the operation takes one.
    ///   - tags: The tags, for `SetTags`.
    ///   - windowNumber: The window a moved tab is to stand in.
    ///   - index: The place it takes there.
    ///   - confirmationToken: For `CloseWindow`, the token of the question
    ///     that was answered; zero otherwise.
    /// - Returns: The request's number; zero when this session has listed no
    ///   host or the operation or kind is none this build knows.
    std::uint64_t requestHostStructureChange(std::uint8_t operation, std::uint8_t targetKind, std::uint64_t targetNumber, const std::string& text, const StructureTags& tags, std::uint64_t windowNumber, std::uint32_t index, std::uint64_t confirmationToken)
    {
        if (operation < std::to_underlying(StructureOperation::SetLabel) || operation > std::to_underlying(StructureOperation::CloseWindow) || targetKind >= Ubiquitty::Session::Identity::objectKindCount)
            return 0;
        StructureRequest request;
        request.operation = static_cast<StructureOperation>(operation);
        request.target = { static_cast<Ubiquitty::Session::Identity::ObjectKind>(targetKind), targetNumber };
        request.text = text;
        request.tags = tags.names;
        request.window = { Ubiquitty::Session::Identity::ObjectKind::Window, windowNumber };
        request.index = index;
        request.confirmationToken = confirmationToken;
        return requestHostStructureChange(std::move(request));
    }

    /// The number of the request ``lastHostStructureResult`` is about.
    [[nodiscard]] std::uint64_t lastHostStructureRequestID() const { return lastHostStructureResult().requestID; }
    /// Its `Runtime::StructureOutcome`.
    [[nodiscard]] std::uint8_t lastHostStructureOutcome() const { return std::to_underlying(lastHostStructureResult().outcome); }
    /// Its `Runtime::StructureOperation`.
    [[nodiscard]] std::uint8_t lastHostStructureOperation() const { return std::to_underlying(lastHostStructureResult().operation); }
    /// Its target's `Session::Identity::ObjectKind` and number.
    [[nodiscard]] std::uint8_t lastHostStructureTargetKind() const { return std::to_underlying(lastHostStructureResult().target.kind); }
    [[nodiscard]] std::uint64_t lastHostStructureTargetNumber() const { return lastHostStructureResult().target.number; }
    /// Why it was not served, as a sentence; empty when it was.
    [[nodiscard]] std::string lastHostStructureReason() const { return lastHostStructureResult().reason; }

    /// How the change that settled last ended.
    ///
    /// Read when ``EventKind::HostStructureSettled`` drains.
    ///
    /// - Returns: The result, with the number its request was given;
    ///   `requestID` zero before any has settled.
    [[nodiscard]] StructureResult lastHostStructureResult() const
    {
        return m_directory ? m_directory->lastStructureResult : StructureResult {};
    }

    /// Offers to install `ubiquitty-host` on the host this session attaches
    /// to, when the host turns out not to have one.
    ///
    /// Every field is what the decision needs and nothing more: the policy
    /// is Core's, the push is a shell command, and only the first-time
    /// approval is the frontend's to answer — which it hears as the
    /// ``EventKind::DeployConfirmationPending`` event, on the same flag
    /// surface every other session event rides, rather than through a
    /// callback carrying a frontend pointer.
    ///
    /// - Parameters:
    ///   - sshHost: The host a push targets; empty offers no deploy.
    void offerHostDeploy(const std::string& sshHost)
    {
        if (!m_remote)
            return;
        m_remote->deploy.sshHost = sshHost;
        // Whether an automatic install is allowed at all is configuration,
        // and configuration is Core's to read. A frontend passing it would
        // be answering a question it had to ask Core anyway.
        m_remote->deploy.isAutoInstallAllowed = Preferences::Effective::autoDeployHostOnRemoteConnect();
    }

    /// Registers one bundled host binary, keyed by the remote `uname -sm`
    /// it serves.
    ///
    /// - Parameters:
    ///   - platformArch: The key, e.g. `Linux x86_64`.
    ///   - binaryPath: The bundled binary's path.
    void addDeployBinary(const std::string& platformArch, const std::string& binaryPath)
    {
        if (m_remote)
            m_remote->deploy.binaryForPlatform[platformArch] = binaryPath;
    }

    /// Approves the install this session asked about, and remembers the
    /// host so it is not asked again.
    ///
    void approveHostDeploy()
    {
        if (!m_remote || !m_remote->core)
            return;
        // The consent is the person's and outlives this session: every later
        // push to the host, from any window, reads it from the one store.
        Runtime::HostDirectory::trustDeploy(m_remote->deploy.sshHost);
        performDeployPush();
    }

    /// Declines the install this session asked about.
    ///
    /// The connect carries on without a host binary, which is what the
    /// remote's own bootstrap will report on the next attempt.
    void declineHostDeploy()
    {
        if (!m_remote || !m_remote->core)
            return;
        const bool wasUpdateQuestion = m_remote->deploy.pendingLiveSessionCount != 0;
        m_remote->deploy.pendingLiveSessionCount = 0;
        m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed,
            wasUpdateQuestion ? "host update cancelled; nothing on the host changed" : "host install declined");
    }

    /// Asks the host for a file on its own filesystem.
    ///
    /// A path a program printed on a host names a file there, not here, so it
    /// is fetched rather than opened: the daemon confines the read to the
    /// session's own directory subtree and answers with
    /// ``EventKind::RemoteFileArrived``, whatever the answer is.
    ///
    /// The id is remembered until its answer arrives, because only an answer
    /// to a question this session asked is taken: a host sending a file nobody
    /// asked for is a host writing to this disk, and the frontend above opens
    /// what it lands.
    ///
    /// - Parameter path: The path on the host, absolute.
    /// - Returns: The request id, or zero when the session cannot ask — a
    ///   local session, or a host that does not serve files.
    [[nodiscard]] std::uint32_t requestRemoteFile(const std::string& path)
    {
        if (!m_remote || !m_remote->core)
            return 0;
        const auto session = weakPtr();
        const auto core = m_remote->core->weakPtr();
        const std::uint32_t requestID = m_remote->core->requestFile(path);
        if (!requestID || !session || !core || !session->m_remote || session->m_remote->core.get() != core.get())
            return 0;
        session->noteAskedRemoteFile(requestID, path);
        return requestID;
    }

    /// Captures a clicked file target without requesting bytes from the host.
    ///
    /// The target is read by `Session::resolveHostFileLink`, the reading the
    /// plate shows, so a URI whose fragment names a line asks for the file and
    /// keeps the line for the landing.
    [[nodiscard]] std::uint64_t prepareRemoteFileDownload(const std::string& candidate, Session::FileLinkSyntax syntax,
        const std::string& host, const std::string& hostLabel)
    {
        if (!m_remote || !m_remote->core || host.empty() || m_nextFileDownloadToken == std::numeric_limits<std::uint64_t>::max())
            return 0;
        std::string directory;
        {
            const std::scoped_lock hold(*m_activeLock);
            directory = currentDirectory();
        }
        auto target = Session::resolveHostFileLink(candidate, syntax, directory, host);
        if (!target)
            return 0;
        const auto token = ++m_nextFileDownloadToken;
        const std::string label = hostLabel.empty() || hostLabel == host ? host : hostLabel + " (" + host + ")";
        return m_remote->core->prepareFileDownload(token, label, target->path, target->line) ? token : 0;
    }

    /// Returns zero when the question's attachment has ended or changed.
    [[nodiscard]] std::uint64_t pendingRemoteFileDownloadToken() const
    {
        const auto* pending = m_remote && m_remote->core ? m_remote->core->pendingFileDownload() : nullptr;
        return pending ? pending->token : 0;
    }
    /// Returns the captured connected host, never a hostname taken from terminal output.
    [[nodiscard]] std::string pendingRemoteFileDownloadHost() const
    {
        const auto* pending = m_remote && m_remote->core ? m_remote->core->pendingFileDownload() : nullptr;
        return pending ? pending->host : std::string();
    }
    /// Returns the exact path the pending question will request.
    [[nodiscard]] std::string pendingRemoteFileDownloadPath() const
    {
        const auto* pending = m_remote && m_remote->core ? m_remote->core->pendingFileDownload() : nullptr;
        return pending ? pending->path : std::string();
    }
    /// Consumes one answer and correlates only an approved, current request with its eventual reply.
    [[nodiscard]] std::uint32_t answerRemoteFileDownload(std::uint64_t token, Session::FileDownloadDecision decision)
    {
        if (!m_remote || !m_remote->core)
            return 0;
        const auto* pending = m_remote->core->pendingFileDownload();
        const auto path = pending ? pending->path : std::string();
        const auto line = pending ? pending->line : Session::FileLinkLine {};
        const auto session = weakPtr();
        const auto core = m_remote->core->weakPtr();
        const auto requestID = m_remote->core->answerFileDownload(token, decision);
        if (!requestID || !session || !core || !session->m_remote || session->m_remote->core.get() != core.get())
            return 0;
        session->noteAskedRemoteFile(requestID, path, line);
        return requestID;
    }

    /// Whether a host's answer is waiting to be read.
    ///
    /// - Returns: Whether ``remoteFileRequestID`` and the getters beside it
    ///   describe anything.
    [[nodiscard]] bool hasRemoteFile() const { return m_remote && !m_remote->arrivedFiles.empty(); }

    /// Which request the waiting answer answers.
    ///
    /// - Returns: The id, or zero when nothing is waiting.
    [[nodiscard]] std::uint32_t remoteFileRequestID() const
    {
        return hasRemoteFile() ? m_remote->arrivedFiles.front().message.requestID : 0;
    }

    /// How the host answered: a ``Protocol::Sync::FileDataStatus``.
    ///
    /// - Returns: The status, or OK's zero when nothing is waiting — which a
    ///   caller reaches only through ``hasRemoteFile``.
    [[nodiscard]] std::uint8_t remoteFileStatus() const
    {
        return hasRemoteFile() ? static_cast<std::uint8_t>(m_remote->arrivedFiles.front().message.status) : 0;
    }

    /// The path the host echoed back.
    ///
    /// The host's own string, and therefore untrusted: it is what an answer
    /// says it is about, never what a landing is named from — see
    /// ``remoteFileRequestedPath`` for that.
    ///
    /// - Returns: The path, empty when nothing is waiting.
    [[nodiscard]] std::string remoteFilePath() const
    {
        return hasRemoteFile() ? m_remote->arrivedFiles.front().message.path : std::string();
    }

    /// The path this session asked the host for, for the waiting answer.
    ///
    /// This side's own string, kept from the ask and matched to the answer by
    /// request id. A landing is named from this: the name carries the
    /// extension a frontend reads to decide whether the bytes go to the
    /// default handler or are only revealed, and a host that chose the name
    /// would be choosing that instead of the reader who clicked.
    ///
    /// - Returns: The path, empty when nothing is waiting.
    [[nodiscard]] std::string remoteFileRequestedPath() const
    {
        return hasRemoteFile() ? m_remote->arrivedFiles.front().requestedPath : std::string();
    }

    /// The line the clicked link named, for the waiting answer.
    ///
    /// Carried from the ask like the path, so the landed copy opens at the
    /// line the reader clicked, in the reader's `[links] line_handler`.
    ///
    /// - Returns: The line and column, both 0 when the link named none or
    ///   nothing is waiting.
    [[nodiscard]] Session::FileLinkLine remoteFileRequestedLine() const
    {
        return hasRemoteFile() ? m_remote->arrivedFiles.front().requestedLine : Session::FileLinkLine {};
    }

    /// How many bytes the host sent.
    ///
    /// - Returns: The count, zero when nothing is waiting or the read failed.
    [[nodiscard]] std::size_t remoteFileByteCount() const
    {
        return hasRemoteFile() ? m_remote->arrivedFiles.front().message.data().size() : 0;
    }

    /// Copies the bytes the host sent into a caller's buffer.
    ///
    /// A copy into memory the caller owns rather than a pointer into the
    /// queue: a borrowed one is good only until the next answer arrives, and
    /// the Swift seam has no way to say that — it refuses an interior pointer
    /// outright, which is the same judgement stated once by the toolchain.
    ///
    /// - Parameters:
    ///   - out: Where to write, holding at least `capacity` bytes.
    ///   - capacity: How much `out` holds.
    /// - Returns: How many bytes were written: the whole answer, or nothing at
    ///   all when the buffer is too short for it.
    [[nodiscard]] std::size_t copyRemoteFileBytes(std::uint8_t* out, std::size_t capacity) const
    {
        const std::size_t count = remoteFileByteCount();
        if (out == nullptr || count == 0 || capacity < count)
            return 0;
        const auto data = m_remote->arrivedFiles.front().message.data();
        std::copy(data.begin(), data.end(), out);
        return count;
    }

    /// Discards the answer at the front, so the next one can be read.
    void dropRemoteFile()
    {
        if (hasRemoteFile())
            m_remote->arrivedFiles.pop_front();
    }

    /// What the last failed push printed, for the person to act on.
    ///
    /// Read when ``EventKind::DeployFailed`` drains: the tail of ssh's and the
    /// remote shell's output, or a fixed sentence when the push printed
    /// nothing or never started.
    ///
    /// - Returns: The failure, empty before any push failed. By value: a
    ///   reference to a std::string does not cross the Swift seam.
    [[nodiscard]] std::string lastDeployFailure() const { return m_remote ? m_remote->deploy.lastFailure : std::string(); }

    /// Whether the last install waits for sessions to end or a refused
    /// handover to become possible. The old daemon keeps those sessions.
    ///
    /// Read beside ``lastDeployFailure`` when ``EventKind::DeployFailed``
    /// drains: the connection is Failed either way, since this Mac cannot
    /// attach until the new host takes over.
    ///
    /// - Returns: Whether it was deferred.
    [[nodiscard]] bool lastDeployWasDeferred() const { return m_remote && m_remote->deploy.wasDeferred; }

    /// How many sessions the update waiting on an answer would hand over.
    ///
    /// Non-zero while ``EventKind::DeployConfirmationPending`` asks about
    /// them: the host's running daemon holds sessions, and the frontend asks
    /// whether to update now (``updateHostNow``), when they end
    /// (``updateHostWhenSessionsEnd``), or not at all (``declineHostDeploy``).
    /// Zero when the pending question is the first-install consent instead.
    ///
    /// - Returns: The count.
    [[nodiscard]] std::size_t hostUpdateLiveSessionCount() const { return m_remote ? m_remote->deploy.pendingLiveSessionCount : 0; }

    /// The question the update with live sessions asks, verbatim.
    ///
    /// - Parameter host: What the person calls the host.
    /// - Returns: The sentence.
    [[nodiscard]] std::string hostUpdateSessionsQuestion(const std::string& host) const
    {
        return Ubiquitty::Session::hostUpdateSessionsQuestion(hostUpdateLiveSessionCount(), host);
    }

    /// Updates now by handing live sessions over, preserving any that refuse.
    void updateHostNow() { answerHostUpdateSessions(Ubiquitty::Session::HostInstallMode::Now); }

    /// Installs the update beside the running host, which keeps its sessions;
    /// the new host takes over on the first connect after they have ended.
    void updateHostWhenSessionsEnd() { answerHostUpdateSessions(Ubiquitty::Session::HostInstallMode::WhenSessionsEnd); }

    /// The remote's `uname -sm` for the last request no automatic install
    /// could answer.
    ///
    /// Read when ``EventKind::DeployNeedsManualInstall`` drains, beside
    /// ``manualHostInstallCommands``.
    ///
    /// - Returns: The platform, empty when the host did not say.
    [[nodiscard]] std::string manualInstallPlatform() const { return m_remote ? m_remote->deploy.manualInstallPlatform : std::string(); }

    /// Whether the last manual install is needed because automatic install is
    /// turned off, rather than because no bundled binary serves the platform.
    ///
    /// The two call for different sentences: one is a setting the person can
    /// change, the other a platform no install ships for.
    ///
    /// - Returns: Whether the setting is what stood in the way.
    [[nodiscard]] bool isManualInstallForSetting() const { return m_remote && m_remote->deploy.isManualInstallForSetting; }

    /// The commands a person runs on the host to install `ubiquitty-host`
    /// by hand, for the platform ``manualInstallPlatform`` names, at this
    /// build's version.
    ///
    /// Core's (``Session::manualHostInstallCommands``), so every frontend
    /// offers the same steps for the same host.
    ///
    /// - Returns: The commands, newline-separated, ready to paste, or the
    ///   sentence saying no published host serves the platform.
    [[nodiscard]] std::string manualHostInstallCommands() const
    {
        return Ubiquitty::Session::manualHostInstallCommands(manualInstallPlatform(), Ubiquitty::Protocol::Sync::appVersion());
    }

    /// Posts a focus change to the thread that owns the model.
    ///
    /// - Parameter focus: The new state.
    void setDesiredFocus(Ubiquitty::Modes::FocusState focus)
    {
        // A thin client's own cursor and attention presentation follows its
        // surface immediately; the host remains the input and frame owner.
        // The two links paneLink() routes between, named here so the call
        // binds to the final class rather than through PaneLink.
        if (m_remote && m_remote->core && !m_tmuxPrimaryStream) {
            Command command;
            command.kind = CommandKind::SetPresentationFocus;
            command.flag = focus == Modes::FocusState::Focused;
            postCommand(command);
            m_remote->core->setFocus(focus);
            return;
        }
        m_localPaneLink.setFocus(focus);
    }

    /// Borrows the state this pane presents. The active model lock must be
    /// held while reading the terminal, as for every render/model accessor.
    [[nodiscard]] Session::PaneView paneView() { return paneLink().view(); }

    /// Requests logical history from the pane's owner, at one priority.
    [[nodiscard]] bool wantPaneHistory(Protocol::Sync::HistoryPriority priority, std::span<const Protocol::Sync::LineRange> ranges)
    {
        return paneLink().wantHistory(priority, ranges);
    }

    /// Reads phase and both independent stall indicators on the owner's clock.
    [[nodiscard]] Session::LinkState paneLinkState(std::uint64_t now) { return paneLink().linkState(now); }

    /// Describes a transport stall without replacing a native failure's reason.
    /// Lag warnings remain disabled until a measured floor is supplied.
    [[nodiscard]] std::string paneLinkIndicatorText()
    {
        auto& link = paneLink();
        const auto now = link.clockMicroseconds();
        return Session::linkIndicatorText(Session::linkIndicatorFor(link.linkState(now),
            m_lastPaneLinkFrame ? m_lastPaneLinkFrame->lagMicroseconds : 0, now, {}));
    }

    /// Delivers the pane's notifications on this session's presentation owner.
    /// The observer can read the model; no callback runs under the model lock.
    void observePaneLink(Session::PaneLinkObserver& observer) { m_paneLinkObserver = observer.weakPtr(); }

    /// Posts one command for the ingest loop to apply under the model lock.
    ///
    /// Lock-free on the live path, so a caller never blocks on the model
    /// lock the drain holds hot under a flood; the wake bounds pickup by one
    /// loop iteration rather than an idle tick. A full ring and a stopped
    /// loop both apply inline under the lock instead — a posted command may
    /// not be dropped, and with no loop running nothing else would ever
    /// drain the ring.
    ///
    /// - Parameter command: The command to apply.
    void postCommand(const Command& command)
    {
        if (m_ingestThread.joinable() && m_runtime->link().commands.post(command)) {
            m_loop.raiseWake();
            return;
        }
        m_host.lockModel();
        m_runtime->applyCommand(command);
        m_host.unlockModel();
    }

    /// Resizes the grid and the host window in the proven order.
    ///
    /// The grid mutation runs under the model lock — the ingest loop parses
    /// under the same lock on its own thread, and an unlocked reallocation
    /// moves the grid out from under a live drain. The ioctl follows outside
    /// the lock: the kernel need not wait on the model, and SIGWINCH's
    /// redraw arrives through the drain against a grid already sized. The
    /// pixel extents ride with it, so a step of a drag reaches the child as
    /// one winsize: a grid change sent with no pixels and the pixels sent
    /// after it are two SIGWINCHs, and a program that erases its last frame
    /// by cursor arithmetic on the second finds the first's repaint under it.
    ///
    /// Which gesture a resize belongs to.
    enum class ResizeGesture : std::uint8_t {
        /// One resize, standing on its own: it pays for its history rewrap now.
        Settled UBIQUITTY_SWIFT_NAME(settled),
        /// One step of a live drag. A width step's rewrap is deferred to
        /// ``settleResize``, because paying it twenty times a second is what
        /// makes a drag stutter, and every step but the last is thrown away.
        Interactive UBIQUITTY_SWIFT_NAME(interactive),
    };

    /// - Parameters:
    ///   - rows: The new row count.
    ///   - columns: The new column count.
    ///   - gesture: Whether this is one step of a live drag.
    ///   - anchor: Which edge of the grid stays put. A drag whose bottom
    ///     edge is stationary anchors there, so the prompt does not ride
    ///     the edge the reader is moving.
    ///   - widthInPixels: The text area's width in pixels, or zero when the
    ///     caller reports no extent.
    ///   - heightInPixels: The text area's height in pixels, likewise.
    void resize(int rows, int columns, ResizeGesture gesture, Ubiquitty::Modes::AnchorMode anchor, int widthInPixels = 0, int heightInPixels = 0)
    {
        paneLink().setViewport({ .rows = rows, .columns = columns, .widthInPixels = widthInPixels, .heightInPixels = heightInPixels, .urgency = gesture == ResizeGesture::Interactive ? Modes::ResizeUrgency::Interactive : Modes::ResizeUrgency::Settled, .anchor = anchor });
    }

    // ---- presentation pacing ----
    //
    // `Runtime::FrameDriver` is the idle/active policy every frontend shares.
    // It lives here rather than in the frontend because its members are
    // atomics, which a Swift arm cannot hold; the frontend keeps the
    // mechanism — its link, its timer — and asks these three questions.

    /// Stamps host-output activity for the pacing policy.
    ///
    /// The drains stamp themselves; this is for the frontend's own reasons to
    /// keep painting — a resize whose reflow lands milliseconds later, a pane
    /// that just became visible — where output is not what changed.
    void notePresentationActivity() { m_runtime->notePresentationActivity(); }

    /// Whether frames are being driven on a clock right now.
    ///
    /// The question a delivery asks: a frontend seeing output while this is
    /// false takes the idle-to-active edge and resumes. It is not asked as an
    /// edge return, because the stamp that takes the edge happens on the
    /// ingest thread where no frontend is listening.
    [[nodiscard]] bool isPresentationContinuous() const { return m_runtime->isPresentationContinuous(); }

    /// Whether host output has been idle past the driver's timeout.
    ///
    /// - Returns: Whether the frontend may drop back to on-demand drive.
    [[nodiscard]] bool isPresentationIdleExpired() const { return m_runtime->isPresentationIdleExpired(); }

    /// Publishes how the frontend is driving frames.
    ///
    /// Ordered by the caller: continuous is published before the clock starts
    /// and cleared after it stops, so an edge test never observes a stopped
    /// clock that still reads as driving.
    ///
    /// - Parameter mode: What the frontend is doing now.
    void setPresentationMode(Ubiquitty::Runtime::FrameMode mode) { m_runtime->setPresentationMode(mode); }

    /// Enables the screen-change-rate recommendation. Off,
    /// isPresentationRecommendedContinuous is always true.
    ///
    /// - Parameter drive: On to recommend a rate, Off to hold Continuous.
    void setAdaptivePresentation(Ubiquitty::Runtime::AdaptiveDrive drive) { m_runtime->setAdaptivePresentation(drive); }

    /// Whether adaptive presentation is enabled.
    [[nodiscard]] bool isAdaptivePresentation() const { return m_runtime->isAdaptivePresentation(); }

    /// Feeds the presentation driver the resolved panel frame interval.
    ///
    /// - Parameter seconds: The interval (1/rate); a non-positive value is ignored.
    void setPresentationFrameIntervalSeconds(double seconds) { m_runtime->setPresentationFrameIntervalSeconds(seconds); }

    /// Whether the screen-change rate warrants continuous drive as of now.
    [[nodiscard]] bool isPresentationRecommendedContinuous() const { return m_runtime->isPresentationRecommendedContinuous(); }

    /// The adaptive drain streak, for a debug readout.
    [[nodiscard]] int presentationFastDrainStreak() const { return m_runtime->presentationFastDrainStreak(); }

    /// The frame interval the adaptive estimate measures against; 0 until sourced.
    [[nodiscard]] double presentationFrameIntervalSeconds() const { return m_runtime->presentationFrameIntervalSeconds(); }

    /// Stamps a rendered frame that changed pixels, for the adaptive rate estimate.
    void notePresentationNetChange() { m_runtime->notePresentationNetChange(); }

    /// Whether the coherence rule holds the pane's frame: the live grid may be
    /// a screen the program has not finished drawing. Any thread.
    [[nodiscard]] bool isFrameHeld() const { return m_runtime->isFrameHeld(); }

    /// Whether a frontend should withhold this present: a synchronized-output
    /// block is open, the coherence rule holds the frame (a cursor-hidden
    /// redraw, a read not yet quiet), or a whole-screen erase was parsed and the
    /// program's redraw is still arriving. A frame the rule releases wakes
    /// the next present: through the delivery of the pass that released it,
    /// or through the loop's release hook when the rule's clock did. A
    /// saturated drain is not withheld for the rule: its frames are the
    /// captures each full pass publishes, and the present skips the live read
    /// between them on its own. Takes the model lock for the block's stamp.
    [[nodiscard]] bool shouldWithholdFrame(Ticks nowTicks)
    {
        if (m_runtime->isFrameHeld() && !m_runtime->isDrainSaturated())
            return true;
        // The decision stamps the block on first sight and expires it, so it
        // writes the model and must hold the lock to do it. Whether there is a
        // block at all is published: the writer raises `syncHoldActive` on the
        // drain that opened one, and the frame that would show it runs after
        // that delivery. A frame drawn with no block open therefore owes the
        // parser no handoff to be told so -- and this is asked once per frame,
        // on the thread the parser is contending with.
        if (!readSnapshot().syncHoldActive)
            return m_runtime->shouldWithholdPresentation(nullptr, nowTicks, PAL::monotonicMicros());
        const std::scoped_lock hold(*m_activeLock);
        return m_runtime->shouldWithholdPresentation(modelTerm(), nowTicks, PAL::monotonicMicros());
    }

    /// The capture's recheck of ``shouldWithholdFrame``, under the model lock
    /// the caller holds for the capture.
    ///
    /// The published check runs before the lock is taken, and the parser can
    /// open a block or erase the screen between the two: an erase whose drain
    /// has not ended yet is armed here, so the capture never takes the erased
    /// screen the hold exists to keep off the glass.
    ///
    /// - Parameters:
    ///   - terminal: The model, borrowed under the caller's lock.
    ///   - nowTicks: Now, on the 60 Hz monotonic clock.
    /// - Returns: True while the capture should wait.
    [[nodiscard]] bool shouldWithholdCapture(Ubiquitty::Terminal& terminal, Ticks nowTicks)
    {
        return m_runtime->shouldWithholdPresentation(&terminal, nowTicks, PAL::monotonicMicros());
    }

    /// The live-resize capture's check, under the model lock the caller
    /// holds: whether the redraw behind the newest whole-screen erase is
    /// still arriving. See ``TerminalRuntime/isEraseRedrawArriving(_:_:)``.
    ///
    /// - Parameter terminal: The model, borrowed under the caller's lock.
    /// - Returns: True while the capture would show the erased screen.
    [[nodiscard]] bool isEraseRedrawArriving(Ubiquitty::Terminal& terminal)
    {
        return m_runtime->isEraseRedrawArriving(terminal, PAL::monotonicMicros());
    }

    /// Sets whether the frontend is in a live resize. See
    /// ``TerminalRuntime/setLiveResize(_:)``.
    ///
    /// - Parameter resize: Whether a live resize is in progress.
    void setLiveResize(LiveResize resize) { m_runtime->setLiveResize(resize); }

    /// Returns what the frontend last said of its live resize. See
    /// ``TerminalRuntime/liveResize()``.
    ///
    /// - Returns: The state `setLiveResize` last stored.
    [[nodiscard]] LiveResize liveResize() const { return m_runtime->liveResize(); }

    /// Ticks until a withheld present may release on its own — the block's
    /// budget, or the erase hold's settle or watchdog — at least 1 while
    /// anything is held, 0 otherwise.
    [[nodiscard]] std::uint32_t presentationHoldTicksRemaining(Ticks nowTicks)
    {
        const std::scoped_lock hold(*m_activeLock);
        return m_runtime->presentationHoldTicksRemaining(modelTerm(), nowTicks, PAL::monotonicMicros());
    }

    /// Plans a live frame against the damage baseline the boundary captures
    /// share. One retained texture holds what was presented, so one lineage
    /// describes it: a live frame and a boundary capture planned against
    /// separate baselines each read the other's drain as a peer's and fell
    /// back to Full. Caller holds the model lock.
    ///
    /// A boundary capture still waiting in the slot drained the grid and
    /// advanced that baseline, and the target never received it: the drive
    /// takes the live route over a waiting capture whenever a selection is on
    /// screen. Its rows, and any scroll it acknowledged, are in that capture
    /// and nowhere else, and the capture is older than the frame about to be
    /// drawn, so presenting it later would put older rows over newer ones.
    /// The capture is retired here and this frame plans whole.
    [[nodiscard]] Rendering::RetainedFramePlan planRetainedFrame(Ubiquitty::Terminal& terminal,
        const Rendering::RetainedFrameRequest& request, Rendering::RowDirtySpan rowDirty)
    {
        if (m_syncSlot.drain() >= 0)
            m_retainedBaseline = {};
        return Rendering::planRetainedFrame(terminal, m_retainedBaseline, request, rowDirty);
    }

    /// Plans a skipped grid frame against that same shared baseline -- the
    /// frame-skip gate's path, which reads the baseline without a live build
    /// so its lineage stays one with the boundary captures. Caller holds the
    /// model lock.
    [[nodiscard]] Rendering::RetainedFramePlan planSkippedGridFrame(Ubiquitty::Terminal& terminal,
        const Rendering::RetainedFrameRequest& request, Rendering::RowDirtySpan rowDirty)
    {
        return Rendering::planSkippedGridFrame(terminal, m_retainedBaseline, request, rowDirty);
    }

    /// Invalidates the shared baseline; the next frame on either route is Full.
    ///
    /// - Parameter terminal: The model whose baseline is dropped.
    void invalidateRetainedFrame(Ubiquitty::Terminal& terminal)
    {
        Rendering::invalidateRetainedFrame(terminal, m_retainedBaseline);
    }

    /// Replaces fallback rich-text pixels once the frontend's font inventory is ready.
    /// The generation is a frontend fact; Core owns the layout and frame damage.
    [[nodiscard]] bool fontCatalogBecameReady(std::uint64_t generation, std::uint64_t rendererRevision)
    {
        std::optional<RichText::TypographyFacts> facts;
        bool hasBlocks = false;
        {
            const std::scoped_lock hold(*m_activeLock);
            if (!generation || generation <= m_fontCatalogGeneration || !rendererRevision)
                return false;
            m_fontCatalogGeneration = generation;
            if (!m_typography)
                return false;
            facts = *m_typography;
            facts->rendererRevision = rendererRevision;
            if (const auto* terminal = modelTerm())
                hasBlocks = !terminal->richTextBlocks.isEmpty();
        }
        if (!publishTypography(*facts))
            return false;
        if (!hasBlocks)
            return false;
        const std::scoped_lock hold(*m_activeLock);
        if (auto* terminal = modelTerm())
            Rendering::invalidateRetainedFrame(*terminal, m_retainedBaseline);
        return true;
    }

    /// Retunes the sync-publish throttle to the live presentation rate: a
    /// boundary inside the throttle window publishes no capture, so the window
    /// must be shorter than the link's frame.
    ///
    /// - Parameter seconds: The presentation refresh interval; non-positive clears the throttle.
    void setSyncPublishRefreshSeconds(double seconds) { m_host.setPublishMinInterval(seconds > 0.0 ? seconds * 0.5 : 0.0); }

    /// Whether a history rewrap is owed at the current width.
    ///
    /// True between an `Interactive` resize that changed the width and the
    /// settle that pays for it. Asked without paying: reading the reachable
    /// row count instead would force the rewrap, because that count is one
    /// of the accesses the deferral resolves on.
    ///
    /// - Returns: Whether a rewrap is pending.
    [[nodiscard]] bool hasDeferredHistoryRewrap()
    {
        const std::scoped_lock hold(*m_activeLock);
        const Ubiquitty::Terminal* term = modelTerm();
        return term != nullptr && term->pendingHistoryRewrapColumns != 0;
    }

    /// Pays for the history rewrap a live drag deferred.
    ///
    /// Called once when the drag settles, not per step: the rewrap is
    /// proportional to the scrollback, and the steps in between are
    /// intermediate widths nobody asked to read at.
    void settleResize()
    {
        if (m_remote)
            return;
        const std::scoped_lock hold(*m_activeLock);
        if (Ubiquitty::Terminal* term = modelTerm())
            Ubiquitty::settleResize(term);
    }

    /// Resizes the grid alone, under the model lock, with the reflow choice.
    ///
    /// ``resize`` is the steady-state pair; this is for the frontend that
    /// owns the window-size ioctl separately because it reports the pixel
    /// extent with it, and for the reflow-on-resize preference the pair
    /// hardwires to Fixed.
    ///
    /// - Parameters:
    ///   - rows: The new row count.
    ///   - columns: The new column count.
    ///   - reflow: Whether soft-wrapped history re-wraps to the new width.
    void resizeGrid(int rows, int columns, Ubiquitty::Modes::ReflowMode reflow)
    {
        const std::scoped_lock hold(*m_activeLock);
        m_runtime->resizeGrid(rows, columns, reflow);
    }

    /// The session's Terminal, for the renderer's capture.
    ///
    /// Safety is the caller's lock bracket, not constness: the render writes
    /// the Terminal (palette write-back). A single-threaded caller — the
    /// snapshot driver — has a trivial bracket; a live drive brackets with the
    /// same lock its feed thread holds.
    ///
    /// - Returns: The Terminal, or null before ``start``.
    [[nodiscard]] Ubiquitty::Terminal* terminal() { return modelTerm(); }

    /// The grid geometry, as a cheap field copy.
    [[nodiscard]] DimensionSnapshot dimensions() const { return m_runtime->dimensions(); }

    /// The latest parser-state snapshot, read lock-free from any thread.
    ///
    /// The writer publishes once per drain under the model lock; the seqlock
    /// hands back an untorn copy without touching that lock, which is what
    /// lets a frontend poll geometry, modes and animation inputs while a
    /// flood holds the lock hot. All-scalar and trivially copyable, so the
    /// copy crosses the Swift seam by value.
    ///
    /// - Returns: The latest published snapshot; default-valued (rows 0)
    ///   before the first drain publishes one.
    [[nodiscard]] RuntimeSnapshot readSnapshot() { return m_runtime->link().reads.read(); }

    /// The colours the pane's frame shows, read lock-free from any thread.
    ///
    /// The palette as of the newest moment no synchronized update held the
    /// frame, with the epoch it was read at; see ``CoherentColors``.
    ///
    /// - Returns: The latest published colours.
    [[nodiscard]] CoherentColors readCoherentColors() { return m_runtime->link().coherentColors.read(); }

    /// The grid a title names: the laid-out grid during remote bootstrap, then the model's grid.
    [[nodiscard]] TitleGrid titleGrid(TitleGrid laidOut)
    {
        const RuntimeSnapshot snapshot = readSnapshot();
        const bool isRemote = m_remote && m_remote->core;
        const bool hasHostScreen = isRemote && m_remote->core->hasHostScreen();
        return titleGridForScreen(isRemote, hasHostScreen, laidOut, { snapshot.rows, snapshot.columns });
    }

    /// This session's link, for a frontend that caches its own ModeSnapshot and
    /// needs the DECUDK side channel to go with it.
    [[nodiscard]] const RuntimeLink& link() const { return m_runtime->link(); }

    /// The close-confirmation facts, sampled fresh off the live process —
    /// reading reaps a dead child, so nothing here is cached.
    ///
    /// A remote session answers "no stream": the host owns the process, and
    /// detaching this thin client kills nothing.
    ///
    /// - Returns: The facts ``Session::needsCloseConfirmation`` decides over.
    [[nodiscard]] Session::ProcessFacts processFacts()
    {
        Session::ProcessFacts facts { .streamPresent = false, .hasRunningChild = false, .foregroundIsShell = true };
        if (m_remote)
            return facts;
        if (PAL::Stream* stream = m_runtime->stream()) {
            facts.streamPresent = true;
            facts.hasRunningChild = stream->process().hasRunningChild();
            facts.foregroundIsShell = stream->process().foregroundIsShell();
        }
        return facts;
    }

    /// Whether closing this session requires the configured confirmation.
    [[nodiscard]] bool needsCloseConfirmation()
    {
        const auto facts = processFacts();
        const auto records = m_runtime->processRecords();
        const auto& configuration = Preferences::values();
        return Session::needsCloseConfirmation(facts, Preferences::Effective::closeConfirmation(configuration),
            Preferences::Effective::closeIgnoredProcesses(configuration), records, m_runtime->stream() ? m_runtime->stream()->process().foregroundName() : std::string());
    }

    /// Whether a standing indicator should say that closing kills a job.
    ///
    /// The same rule as ``needsCloseConfirmation`` over the last process
    /// sample instead of a live read. An indicator is redrawn when the sample
    /// moves, so it has to answer for the sample's instant: answered live, it
    /// can catch a job in the millisecond before it ends, and the next sample,
    /// which finds the shell again, moves nothing that would redraw it.
    /// A close itself still asks ``needsCloseConfirmation``, which answers for
    /// the moment of the close.
    ///
    /// - Returns: Whether the last sample found a job a close would kill.
    [[nodiscard]] bool needsRunningJobMarker()
    {
        auto facts = processFacts();
        facts.foregroundIsShell = m_runtime->isSampledForegroundShell();
        const auto records = m_runtime->processRecords();
        const auto& configuration = Preferences::values();
        return Session::needsCloseConfirmation(facts, Preferences::Effective::closeConfirmation(configuration),
            Preferences::Effective::closeIgnoredProcesses(configuration), records, m_runtime->foregroundProcessName());
    }

    /// How many times this session's sampled process records have moved.
    ///
    /// A lock-free load, for a view that asks on every screen update whether
    /// the table it is showing is still the one the walk found.
    ///
    /// - Returns: The generation, zero before the first walk.
    [[nodiscard]] std::uint64_t processRecordsGeneration() const { return m_runtime->processRecordsGeneration(); }

    /// Captures inspector facts using the process sample shared with session titles.
    ///
    /// - Returns: Geometry, palette, locations, and cached process records.
    [[nodiscard]] Session::SessionInfo inspectorInfo()
    {
        auto records = m_runtime->processRecords();
        const std::scoped_lock hold(*m_activeLock);
        auto* term = modelTerm();
        auto* stream = !m_externalTerminal && !m_remote ? m_runtime->stream() : nullptr;
        auto info = Session::captureSessionInfo(term, stream, records);
        if (m_remote) {
            info.canConfigureAnswerback = false;
            info.answerbackDisplay.clear();
        }
        if (term && info.directory.empty() && !m_paneFacts.currentDirectory.empty()) {
            info.directory = m_paneFacts.currentDirectory;
        }
        return info;
    }

    /// Reads the answerback label under the model lock.
    /// - Returns: A concealed or visible message, or empty for a host-owned session.
    [[nodiscard]] std::string inspectorAnswerbackDisplay()
    {
        const std::scoped_lock hold(*m_activeLock);
        const auto* term = modelTerm();
        return !m_remote && term && !term->mirrorClient ? std::string(answerbackForDisplay(*term)) : std::string();
    }

    /// Replaces the local session's answerback and clears its concealment latch.
    /// - Parameter value: User-entered UTF-8, bounded by the answerback limit.
    /// - Returns: False for a host-owned session or an absent terminal.
    [[nodiscard]] bool setInspectorAnswerback(const std::string& value)
    {
        const std::scoped_lock hold(*m_activeLock);
        auto* term = modelTerm();
        if (m_remote || !term || term->mirrorClient)
            return false;
        setAnswerbackString(*term, value);
        return true;
    }

    /// Reports whether this session owns a shell-reported directory to reset.
    ///
    /// - Returns: False for host-owned sessions or absent directory reports.
    [[nodiscard]] bool canClearReportedDirectory()
    {
        const std::scoped_lock hold(*m_activeLock);
        const auto* term = modelTerm();
        return !m_remote && term && !term->reportedDirectory.empty();
    }

    /// Clears shell-reported directory metadata without changing the process directory.
    ///
    /// The sampled process directory remains the fallback. Host-owned sessions
    /// require an authoritative protocol operation and cannot be reset locally.
    ///
    /// - Returns: Whether a directory report was cleared.
    [[nodiscard]] bool clearReportedDirectory()
    {
        {
            const std::scoped_lock hold(*m_activeLock);
            auto* term = modelTerm();
            if (m_remote || !term || term->reportedDirectory.empty()) {
                return false;
            }
            (void)setReportedDirectoryString(*term, {});
            if (auto* shell = term->shellClient.get()) {
                shell->setCurrentDirectory(term->currentDirectory);
            }
        }
        m_events.pending.fetch_or(1u << static_cast<unsigned>(EventKind::DirectoryChanged), std::memory_order_release);
        signalDrain();
        return true;
    }

    /// Signals the selected process through the owning session backend.
    ///
    /// - Parameters:
    ///   - processID: The process selected from the inspector snapshot.
    ///   - signal: The requested native signal number.
    /// - Returns: Whether the backend delivered the signal.
    [[nodiscard]] bool signalInspectedProcess(uint32_t processID, int signal)
    {
        if (m_remote || m_externalTerminal || !m_runtime->stream()) {
            return false;
        }
        return Session::signalSessionProcess(m_runtime->stream(), m_runtime->processRecords(), processID, signal);
    }

    /// Changes the default background without replacing the remaining palette.
    ///
    /// - Parameters:
    ///   - red: The sRGB red component.
    ///   - green: The sRGB green component.
    ///   - blue: The sRGB blue component.
    /// - Returns: Whether the model changed or the owning host accepted the request.
    [[nodiscard]] bool setInspectorBackground(uint8_t red, uint8_t green, uint8_t blue)
    {
        TermPalette palette;
        TermPalette baseline;
        {
            const std::scoped_lock hold(*m_activeLock);
            auto* term = modelTerm();
            if (!term) {
                return false;
            }
            if (!m_remote) {
                term->palette.defaultBackground = { channel8To16(red), channel8To16(green), channel8To16(blue) };
                Themes::rethemeDefaults(term);
                return true;
            }
            palette = term->palette;
            baseline = term->paletteBaseline;
        }
        palette.defaultBackground = { channel8To16(red), channel8To16(green), channel8To16(blue) };
        return m_remote->core && m_remote->core->sendPalette(palette, baseline);
    }

    /// Reports whether the live background differs from its reset baseline.
    ///
    /// - Returns: Whether a background override is present.
    [[nodiscard]] bool canResetBackground()
    {
        const std::scoped_lock hold(*m_activeLock);
        const auto* term = modelTerm();
        if (!term) {
            return false;
        }
        const auto& color = term->palette.defaultBackground;
        const auto& baseline = term->paletteBaseline.defaultBackground;
        return color.red != baseline.red || color.green != baseline.green || color.blue != baseline.blue;
    }

    /// Restores the exact baseline background without replacing other palette slots.
    ///
    /// - Returns: Whether the local background changed or the host request was sent.
    [[nodiscard]] bool resetBackground()
    {
        TermPalette palette;
        TermPalette baseline;
        {
            const std::scoped_lock hold(*m_activeLock);
            auto* term = modelTerm();
            if (!term) {
                return false;
            }
            const auto& color = term->palette.defaultBackground;
            const auto& resetColor = term->paletteBaseline.defaultBackground;
            if (color.red == resetColor.red && color.green == resetColor.green && color.blue == resetColor.blue) {
                return false;
            }
            if (!m_remote) {
                term->palette.defaultBackground = resetColor;
                Themes::rethemeDefaults(term);
                return true;
            }
            palette = term->palette;
            baseline = term->paletteBaseline;
        }
        palette.defaultBackground = baseline.defaultBackground;
        return m_remote->core && m_remote->core->sendPalette(palette, baseline);
    }

    /// Finds the catalog theme matching the session's reset baseline.
    ///
    /// Program color overrides do not change which theme OSC resets restore.
    ///
    /// The window chrome asks on every screen update, so an unchanged answer is
    /// served without the model lock: the baseline moves only with the palette
    /// epoch, and while the published epoch is the one the answer was matched
    /// at, the answer stands. Taking the lock instead parks the main thread for
    /// a whole parse bite whenever a flood keeps the parser busy.
    ///
    /// - Returns: The matching catalog index, or -1 for an unmatched palette.
    [[nodiscard]] int currentThemeIndex()
    {
        const std::scoped_lock cacheHold(m_themeIdentityLock);
        const auto adaptation = Accessibility::paletteAdaptation();
        const auto generation = Themes::generation();
        const auto isCatalogCurrent = [&](const ThemeIdentity& identity) {
            return identity.generation == generation && identity.adaptation.mode == adaptation.mode
                && identity.adaptation.contrastStrength == adaptation.contrastStrength;
        };
        if (m_themeIdentity && isCatalogCurrent(*m_themeIdentity) && m_themeIdentity->paletteEpoch == readSnapshot().paletteEpoch) {
            return m_themeIdentity->index;
        }
        TermPalette baseline;
        std::uint64_t paletteEpoch = 0;
        {
            const std::scoped_lock hold(*m_activeLock);
            const auto* term = modelTerm();
            if (!term) {
                return -1;
            }
            baseline = term->paletteBaseline;
            paletteEpoch = term->paletteEpoch;
        }
        if (m_themeIdentity && m_themeIdentity->baseline == baseline && isCatalogCurrent(*m_themeIdentity)) {
            m_themeIdentity->paletteEpoch = paletteEpoch;
            return m_themeIdentity->index;
        }
        m_themeIdentity = ThemeIdentity { baseline, adaptation, generation, paletteEpoch, -1 };
        const auto equalColor = [](const PaletteRGB& left, const PaletteRGB& right) {
            return left.red == right.red && left.green == right.green && left.blue == right.blue;
        };
        const int chosenIndex = Themes::indexForName(m_chosenThemeName.c_str());
        for (int candidateIndex = -1; candidateIndex < Themes::count(); ++candidateIndex) {
            const int index = candidateIndex < 0 ? chosenIndex : candidateIndex;
            const std::optional<TermPalette> candidate = Themes::resolveThemePalette(index, adaptation);
            if (!candidate) {
                continue;
            }
            if (!equalColor(candidate->defaultForeground, baseline.defaultForeground)
                || !equalColor(candidate->defaultBackground, baseline.defaultBackground)
                || !equalColor(candidate->cursor, baseline.cursor)
                || candidate->indexedAlpha != baseline.indexedAlpha) {
                continue;
            }
            bool matches = true;
            for (int slot = 0; slot < 256 && matches; ++slot) {
                matches = equalColor(candidate->indexed[slot], baseline.indexed[slot]);
            }
            for (int slot = 0; slot < specialCount && matches; ++slot) {
                matches = candidate->specialSet[slot] == baseline.specialSet[slot]
                    && (!baseline.specialSet[slot] || equalColor(candidate->special[slot], baseline.special[slot]));
            }
            if (matches) {
                m_themeIdentity->index = index;
                return index;
            }
        }
        return -1;
    }

    /// The status colours of this session's theme.
    ///
    /// The theme is the one ``currentThemeIndex`` matches, so the colours it
    /// names in its file apply; a palette no catalog theme matches derives
    /// them from its own ANSI colours.
    ///
    /// - Returns: One colour per status slot.
    [[nodiscard]] Themes::StatusColors statusColors()
    {
        if (const Themes::Theme* theme = Themes::get(currentThemeIndex()))
            return theme->statusColors();
        const std::scoped_lock hold(*m_activeLock);
        const Ubiquitty::Terminal* term = modelTerm();
        return term ? App::statusColors(*term) : Themes::StatusColors {};
    }

    /// The colour of the tab status this session publishes, under its theme.
    ///
    /// A program's status record is drawn in the theme's colour for it; an
    /// OSC 21337 indicator is the program's own colour.
    ///
    /// - Returns: The colour, with `isSet` false for none.
    [[nodiscard]] TabStatusColor tabStatusColor()
    {
        const RuntimeSnapshot snapshot = readSnapshot();
        if (!snapshot.isTabStatusColorSet)
            return {};
        return snapshot.tabStatusColor(statusColors());
    }

    /// Names the live processes that closing this local session terminates.
    ///
    /// - Returns: Process display names in process-tree order.
    [[nodiscard]] std::vector<std::string> runningProcessNamesForClose()
    {
        if (m_remote || !m_runtime->stream()) {
            return {};
        }
        return Session::runningProcessNamesForClose(m_runtime->processRecords());
    }

    /// Ends the remote half and keeps the terminal.
    ///
    /// A frontend re-homing a surface to another session, or returning to its
    /// picker after a refused reattach, detaches here: ``close`` disposes the
    /// model, and every attach after it is refused for want of a terminal to
    /// drive. The screen the remote left stays until the next attach paints.
    /// Idempotent.
    void detachRemote()
    {
        m_preparedRemote.reset();
        m_restoredPreview.reset();
        if (!m_remote)
            return;
        endRemoteHalf();
        m_presence = {};
        m_screenAccessHistory.reset();
        noteRemoteEvent(EventKind::PresenceChanged);
        // Through the state's own recorder, which clears what only a Failed
        // session carries: a refusal left standing on an Idle session reads as
        // the refusal again to whatever answered it by detaching.
        noteRemoteStateChanged(Ubiquitty::Session::State::Idle, {});
    }

    /// Stops the ingest loop, disposes the model and closes any owned stream.
    /// Idempotent.
    void close() { (void)tryClose(); }

    /// Refuses all teardown before mutation while a local Commit is unresolved.
    [[nodiscard]] std::expected<void, TerminalRuntime::CloseError> tryClose()
    {
        if (!canClose())
            return std::unexpected(TerminalRuntime::CloseError::OwnershipUnresolved);
        auto protectingOwner = retainForLocalAdoption();
        RetainedTerminalSession escrowOwner;
        if (m_localAdoption && m_localAdoption->escrowID)
            escrowOwner = LocalAdoptionEscrow::singleton().release(m_localAdoption->escrowID);
        detachSearch();
        m_localAdoption.reset();
        m_preparedRemote.reset();
        m_restoredPreview.reset();
        endRemoteHalf();
        m_sharing.reset();
        stopIngest();
        m_runtime->close();
        return {};
    }

    /// Stops the loop before the members it reads are torn down.
    ///
    /// The thread member joins on destruction, but a parked loop only exits
    /// through the stop-and-wake pair — a bare join would wait on a poll that
    /// nothing ends.
    ~TerminalSession()
    {
        // Severed first: a deploy push runs on a thread nothing joins, and its
        // completion is already queued. Reading this session as gone is what
        // stops it.
        revokeWeakPtrs();
        // The search may still be held by the worker; detached, it reaches
        // the terminal no more.
        detachSearch();
        // The remote half lends the model's terminal to its carrier, which a
        // workspace shares with other panes and keeps after this session. It
        // ends while the terminal lives: the members would destroy the
        // runtime that owns the terminal before the remote half that lends it.
        endRemoteHalf();
        stopIngest();
        // The frontend unbinds every hosted pane view before a hosting session
        // goes; what is left is the source's own hooks in this terminal.
        endHostedWindows();
        // A session torn down while still adopted unhooks itself first. The
        // owner's terminal and lock are alive by contract until the unbind, and
        // this is the session's last use of them — leaving the measurer
        // installed hands the still-live terminal a CheckedPtr into a member
        // about to be destroyed, which aborts.
        endExternalAdoption();
        // The session's three references; a drive still holding a drained frame
        // keeps that capture alive through its own reference.
        for (Rendering::FrameCapture* capture : m_syncCaptures)
            releaseFrameCapture(capture);
    }

    /// Launches the user's login shell on a fresh local PTY.
    ///
    /// - Returns: Whether the PTY opened.
    bool launchLoginShell() { return m_runtime->launch(LaunchCommand::loginShell(true), Preferences::terminalIdentity(m_configuration)); }

    /// Launches the user's login shell in a chosen working directory.
    ///
    /// The directory is how a split inherits the focused pane's cwd; empty
    /// falls back to the shell's own default, so a caller with nothing to
    /// inherit passes nothing rather than resolving a home of its own.
    ///
    /// - Parameter initialDirectory: The working directory, or empty.
    /// - Returns: Whether the PTY opened.
    bool launchLoginShell(const std::string& initialDirectory)
    {
        LaunchCommand command = LaunchCommand::loginShell(true);
        if (!initialDirectory.empty())
            command.setInitialDirectory(initialDirectory);
        return m_runtime->launch(command, Preferences::terminalIdentity(m_configuration));
    }

    /// Publishes the window's cell and pixel extent to the host.
    ///
    /// ``resize`` covers the steady-state pair; this exists for the moments
    /// the pixel extent is the news — right after launch, and on a backing
    /// scale change — because applications read `ws_xpixel`/`ws_ypixel` for
    /// image geometry and a zero there tells them the window has no pixels.
    ///
    /// - Parameters:
    ///   - columns: The grid's columns.
    ///   - rows: The grid's rows.
    ///   - widthInPixels: The text area's width in pixels.
    ///   - heightInPixels: The text area's height in pixels.
    void setWindowSize(int columns, int rows, int widthInPixels, int heightInPixels)
    {
        paneLink().setWindowSize(columns, rows, widthInPixels, heightInPixels);
    }

    /// Publishes the cell's device-pixel size, the text area's and the
    /// drawable's through their pane owner: what CSI 14/16 t report,
    /// what a sixel or iTerm2 image's cell extent divides by, and what the
    /// SGR-pixel mouse report is clamped to. Points times the scale the frames
    /// are drawn at. Three facts rather than one answer: which extent a
    /// program is told is Core's (`reportedTextAreaPixels`), because DECSET
    /// 7402 changes it between two publishes.
    ///
    /// - Parameters:
    ///   - cellWidthInPixels: One cell's width in device pixels.
    ///   - cellHeightInPixels: One cell's height in device pixels.
    ///   - textAreaWidthInPixels: The grid's width in device pixels.
    ///   - textAreaHeightInPixels: The grid's height in device pixels.
    ///   - drawableWidthInPixels: The whole drawable's width, padding included.
    ///   - drawableHeightInPixels: The whole drawable's height.
    void publishCellPixelSize(int cellWidthInPixels, int cellHeightInPixels, int textAreaWidthInPixels, int textAreaHeightInPixels,
        int drawableWidthInPixels, int drawableHeightInPixels)
    {
        const PixelGeometry geometry { { cellWidthInPixels, cellHeightInPixels },
            { textAreaWidthInPixels, textAreaHeightInPixels }, { drawableWidthInPixels, drawableHeightInPixels } };
        m_pixelGeometry = geometry;
        // This call stages metrics only. The following resize/window-size
        // report sends the complete grid and pixels without holding a model lock.
        paneLink().setPixelGeometry(geometry);
    }

    /// Launches one shell command on a fresh local PTY.
    ///
    /// The command runs under `/bin/sh -c`, which is what a smoke or a script
    /// wants: one line, the shell's parsing, an exit when it finishes.
    ///
    /// - Parameter command: The command line to run.
    /// - Returns: Whether the PTY opened.
    bool launchShellCommand(const std::string& command)
    {
        std::vector<std::string> arguments { "/bin/sh", "-c", command };
        LaunchCommand launch = LaunchCommand::fromArguments(std::move(arguments));
        launch.setArgument0Style(PAL::Argument0Style::ProgramPath);
        return m_runtime->launch(launch, Preferences::terminalIdentity(m_configuration));
    }

    /// Hands the session a transport something else already opened.
    ///
    /// The launch family opens a fresh PTY; a drive hosting a handed-off
    /// session — a daemon's pane, a restored window — arrives holding the
    /// endpoint instead, and the session must own it exactly as it owns a
    /// launched one: closed in ``close()`` and the destructor. This is also
    /// the only transport route on a build whose PAL opens no PTY, which is
    /// how the Core tests drive the ingest surface.
    ///
    /// A null stream is refused without changing the installed endpoint.
    ///
    /// - Parameter stream: The transport to own, returned intact on refusal.
    /// - Returns: Success, or the exact refused owner without installation.
    [[nodiscard]] std::expected<void, std::unique_ptr<PAL::Stream>> adoptStream(std::unique_ptr<PAL::Stream> stream)
    {
        return m_runtime->adoptStream(std::move(stream));
    }

    /// Launches an already-resolved command on a fresh local PTY.
    ///
    /// The named launchers cover the common shells; a frontend that resolved
    /// its own command — arguments, environment, directory — hands it over
    /// whole.
    ///
    /// - Parameter command: The command to run.
    /// - Returns: Whether the PTY opened.
    bool launch(const LaunchCommand& command)
    {
        if (command.tmuxControlHost() && !s_hostedWindowSourceFactory.load(std::memory_order_acquire)) {
            return false;
        }
        if (!m_runtime->launch(command, Preferences::terminalIdentity(m_configuration))) {
            return false;
        }
        m_relaunchCommand = command;
        // A relaunch opens no screen: nothing would write one, and a hold
        // nobody lifts parks the new child's output for the pane's life.
        m_relaunchCommand->setOpeningScreen(OpeningScreen::None);
        if (command.tmuxControlHost()) {
            m_tmuxSSHHost = *command.tmuxControlHost();
            if (!m_tmuxSSHHost.empty()) {
                const std::scoped_lock hold(m_modelLock);
                modelTerm()->currentCapabilityOrigin = CapabilityOrigin::Remote;
            }
            beginHostingWindows(HostedControlState::AwaitingHandshake);
        }
        return true;
    }

    /// Returns a replayable launch only for an ended, locally owned process.
    ///
    /// - Returns: The original command, or null for live or hosted sessions.
    [[nodiscard]] const LaunchCommand* endedLocalCommand() const
    {
        if (!m_runtime->sessionEnd().didEnd || m_remote || m_sharing || m_externalTerminal || m_hostedPaneSource || m_tmuxPrimaryStream || m_hostedWindows) {
            return nullptr;
        }
        return m_relaunchCommand ? &*m_relaunchCommand : nullptr;
    }

    /// Whether this session ends when the child it launched does.
    ///
    /// The `-e` fact, per session. A frontend that closed a window on a
    /// process-wide flag closed the wrong one as soon as a second window
    /// existed, which is why this is asked of the session and not of the app.
    ///
    /// - Returns: Whether the launch asked for the child's lifetime.
    [[nodiscard]] bool doesSessionEndWithChild() const { return m_runtime->doesSessionEndWithChild(); }

    /// Reads the program-reported directory or the host's cached foreground directory.
    ///
    /// The caller holds the model lock.
    ///
    /// - Returns: The directory, or empty when neither source reports one.
    [[nodiscard]] std::string currentDirectory()
    {
        const auto* term = modelTerm();
        if (term && !term->currentDirectory.empty()) {
            return term->currentDirectory;
        }
        return m_paneFacts.currentDirectory;
    }

    /// The whole of what the pane's status says, for a tab's hover text and
    /// an assistive reader: `App::statusHelpText`, bounded and cleaned there.
    ///
    /// The caller holds the model lock.
    ///
    /// - Returns: The text, empty when the pane reports no status or before
    ///   the model exists.
    [[nodiscard]] std::string statusHelpText()
    {
        const auto* term = modelTerm();
        return term ? App::statusHelpText(*term) : std::string();
    }

    /// What the pane's program says it is doing, whole: a window title's
    /// status text beside the program's status symbol.
    ///
    /// - Returns: The text, empty when the pane reports no status, its record
    ///   says nothing, or before the model exists.
    [[nodiscard]] std::string programActivityText()
    {
        const auto* term = modelTerm();
        return term ? App::programActivityText(term->programStatus) : std::string();
    }

    /// The program the pane runs, for the icon a title draws before its name:
    /// the one its status record names, as its snapshot published it, and
    /// failing that its foreground process. Reads no model, so a title can
    /// ask on every screen update without the parser's lock.
    ///
    /// - Parameter reportedProcessIcon: The snapshot's `reportedProcessIcon`.
    /// - Returns: The program's catalog key (`Title::processIconCatalog`), empty
    ///   for a pane running none.
    [[nodiscard]] std::string processIconKey(std::uint8_t reportedProcessIcon) const
    {
        if (!m_remote)
            return m_runtime->processIconKey(reportedProcessIcon);
        return std::string(Title::processIconKeyFor(reportedProcessIcon, m_paneFacts.foreground.name, hostedTerminalHolder()));
    }

    /// Who holds a hosted pane's terminal: the host's own sample, carried in
    /// its facts. A pane the host has named no foreground for is its shell's,
    /// as a local pane is before its first sample, so a status record already
    /// on its screen keys no program.
    ///
    /// - Returns: The holder.
    [[nodiscard]] Title::TerminalHolder hostedTerminalHolder() const
    {
        return m_paneFacts.foreground.name.empty() || m_paneFacts.isAtPrompt ? Title::TerminalHolder::Shell : Title::TerminalHolder::Program;
    }

    /// The foreground process chain, as a title shows it ("-zsh ▸ vim").
    ///
    /// Sampled by the drain's own probe, so a frontend reads it rather than
    /// running a timer and a process walk of its own.
    ///
    /// - Returns: The chain; the launch seed until a walk has resolved one,
    ///   and empty only for a session that was never launched.
    [[nodiscard]] std::string processChain() const { return m_remote ? m_paneFacts.processChain : m_runtime->processChain(); }

    /// The foreground process chain spelled with each program's arguments.
    ///
    /// The same walk as ``processChain``, formatted the other way, for the
    /// `[title]` sub-option that asks for argv.
    ///
    /// - Returns: The chain; the launch seed until a walk has resolved one.
    [[nodiscard]] std::string processChainWithArguments() const { return m_remote ? m_paneFacts.processChainWithArguments : m_runtime->processChainWithArguments(); }

    /// The program this pane runs, for chrome drawn around it, from one
    /// sample: a local pane's runtime reads it under one hold of its facts'
    /// lock, and a hosted pane's from the host's last whole record.
    ///
    /// - Parameter reportedProcessIcon: The snapshot's `reportedProcessIcon`.
    /// - Returns: The program; every field empty for a session that ended,
    ///   here or on the host.
    [[nodiscard]] ForegroundProgram foregroundProgram(std::uint8_t reportedProcessIcon) const
    {
        if (!m_remote)
            return m_runtime->foregroundProgram(reportedProcessIcon);
        if (m_paneFacts.end.didEnd || m_didHostedSessionEnd)
            return {};
        return {
            .name = m_paneFacts.foreground.name,
            .key = processIconKey(reportedProcessIcon),
            .chain = m_paneFacts.processChain,
            .chainWithArguments = m_paneFacts.processChainWithArguments,
        };
    }

    /// Where this session's foreground process leads. See
    /// ``TerminalRuntime::remoteTarget``.
    ///
    /// - Returns: The destination, or a target whose kind is `None`.
    [[nodiscard]] PAL::Process::RemoteTarget remoteTarget() const { return m_runtime->remoteTarget(); }

    /// Returns the host authority reported by the session.
    [[nodiscard]] std::string reportedHost() const { return m_runtime->reportedHost(); }

    /// How Ubiquitty reached this session's PTY.
    ///
    /// A tmux control client dialled to a host is over ssh to that host: the
    /// windows it hosts run no process here a walk could find, so the stamp is
    /// the only thing that knows where they are. Anything else this session
    /// launched is a local PTY; a host session's place is its link's to say.
    ///
    /// - Returns: The origin.
    [[nodiscard]] Ubiquitty::Transport::Origin transportOrigin() const
    {
        if (m_tmuxSSHHost.empty())
            return {};
        return Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::SSH, m_tmuxSSHHost };
    }

    /// Every source on this side that can say where the session is: the
    /// transport stamp, the walk and the far side's report, for
    /// ``Session::resolvePaneHost`` to rank.
    ///
    /// - Returns: The sources.
    [[nodiscard]] Ubiquitty::Session::LocationInputs locationInputs() const
    {
        return Ubiquitty::Session::LocationInputs { transportOrigin(), remoteTarget(), reportedHost() };
    }

    /// Returns the number of window-size deliveries to the local stream.
    [[nodiscard]] std::uint64_t windowSizeDeliveryCount() const { return m_runtime->windowSizeDeliveryCount(); }

    /// Returns the directory that can represent a folder on this machine.
    ///
    /// Remote replicas and foreground connections have no local folder proxy.
    [[nodiscard]] std::string representedLocalDirectory()
    {
        if (m_remote || !Title::isLocalDirectory(m_runtime->remoteTarget(), m_runtime->reportedHost())) {
            return {};
        }
        return currentDirectory();
    }

    /// Reads the directory a title names: where the session is
    /// (`Title::titleDirectory`), which while a connection holds the terminal
    /// is only what its far side reported.
    ///
    /// A new tab, a link's base and the folder proxy keep reading
    /// ``currentDirectory``: a tab opened beside an ssh one still starts where
    /// the ssh was run.
    ///
    /// The caller holds the model lock.
    ///
    /// - Returns: The directory, or empty when the title names none.
    [[nodiscard]] std::string titleDirectory()
    {
        std::string directory = currentDirectory();
        const auto* term = modelTerm();
        if (m_remote || !term)
            return directory;
        return std::string(Title::titleDirectory({
            .reportedDirectory = term->reportedDirectory.view(),
            .paneDirectory = directory,
            .foregroundLeadsTo = m_runtime->remoteTarget().kind,
        }));
    }

    /// The foreground process's own name, without its ancestors.
    ///
    /// - Returns: The name, or empty before the first sample.
    [[nodiscard]] std::string foregroundProcessName() const { return m_remote ? m_paneFacts.foreground.name : m_runtime->foregroundProcessName(); }

    /// The home directory a host reports for a pane it serves, which its
    /// working directory is abbreviated against in a title.
    ///
    /// - Returns: The host's report; empty for a local session, whose home is
    ///   the frontend's own, and before the host has said.
    [[nodiscard]] std::string remoteHomeDirectory() const { return m_remote ? m_paneFacts.homeDirectory : std::string {}; }

    /// The home directory a title abbreviates this pane's directory against:
    /// the pane's own user's, on the pane's own machine. A pane a host serves
    /// is abbreviated against the home the host reports, never the reader's,
    /// which names another machine's folder; until the host has said, against
    /// none, so its path shows whole rather than wrongly shortened.
    ///
    /// - Parameter localHome: The reader's home, for a pane on this machine.
    /// - Returns: The home to abbreviate against; empty for none.
    [[nodiscard]] std::string titleHomeDirectory(const std::string& localHome) const { return m_remote ? m_paneFacts.homeDirectory : localHome; }

    /// The machine named by the admitted host's pane facts, before any local alias.
    [[nodiscard]] std::string remoteHostName() const { return m_remote ? m_paneFacts.hostName : std::string {}; }

    /// The replica tty's basename, for the title segment that names it.
    ///
    /// A host pane's is the host's tty, as the host read it, so the segment
    /// reads as it would in a local window on that host.
    ///
    /// - Returns: The name, or empty for a session with no PTY.
    [[nodiscard]] std::string ttyName() const { return m_remote ? m_paneFacts.terminalName : m_runtime->ttyName(); }

    /// The session-leader shell's name, for the title segment that names it.
    ///
    /// - Returns: The name, or empty before a process walk has found one.
    [[nodiscard]] std::string shellCommandName() const { return m_remote ? m_paneFacts.shellCommandName : m_runtime->shellCommandName(); }

    /// How this session's process ended, once it has. See ``TerminalRuntime::sessionEnd``.
    [[nodiscard]] Title::SessionEnd sessionEnd() const { return m_runtime->sessionEnd(); }

    /// Records the status a frontend collected after this session ended.
    void noteSessionExitStatus(bool wasSignalled, int code) { m_runtime->noteSessionExitStatus(wasSignalled, code); }

    /// Collects this session's exit status. See ``TerminalRuntime::collectSessionExitStatus``.
    void collectSessionExitStatus() { m_runtime->collectSessionExitStatus(); }

    /// The stream's pollable file descriptor, or -1 with no stream.
    ///
    /// For the drive's read source only: the descriptor names what to wait
    /// on, and every read against it stays behind ``drain()``.
    [[nodiscard]] int fileDescriptor() const { return m_runtime->streamFileDescriptor(); }

    /// The owned transport itself, for the platform code that speaks to it
    /// directly — a tmux control channel, a ZMODEM transfer.
    ///
    /// - Returns: The stream, or null with none.
    [[nodiscard]] PAL::Stream* stream() { return m_runtime->stream(); }

    /// Flushes any replies the model queued outside a drain pass.
    ///
    /// A focus event or a query answered between drains sits in the reply
    /// queue until the next drain; a frontend that just posted one flushes
    /// so the application sees it now.
    void flushReplies() { m_runtime->flushReplies(); }

    /// Resolves a pending OSC 52 clipboard read with the frontend's answer.
    ///
    /// - Parameters:
    ///   - setSpec: The selection spec the request named.
    ///   - decision: Allow with the clipboard's bytes, or refuse.
    void completeClipboardRead(std::string_view setSpec, Ubiquitty::OSC::ClipboardReadDecision decision)
    {
        m_runtime->completeClipboardRead(setSpec, decision);
    }

    /// Posts a clipboard-read answer to the thread that owns the model.
    ///
    /// The completion above touches the Terminal, so a frontend whose UI
    /// thread is not the model's answers through here: the decision rides
    /// the command ring and applies under the model lock, like a focus
    /// change does.
    ///
    /// - Parameters:
    ///   - setSpec: The selection spec the request named; the ring carries
    ///     at most its first fifteen bytes, the request's own bound.
    ///   - decision: Allow with the staged capture, or refuse.
    void resolveClipboardRead(std::string_view setSpec, Ubiquitty::OSC::ClipboardReadDecision decision)
    {
        if (m_externalTerminal) {
            const std::scoped_lock hold(*m_activeLock);
            Ubiquitty::OSC::completeClipboardReadQuery(*m_externalTerminal, setSpec, decision);
            if (m_tmuxPrimaryStream) {
                Ubiquitty::Protocol::Tmux::flushPaneReplies(*m_externalTerminal, m_tmuxRoutingClient, m_tmuxRoutingPaneID, *m_tmuxPrimaryStream);
            }
            return;
        }
        Command command;
        command.kind = CommandKind::ResolveClipboardRead;
        command.flag = decision == Ubiquitty::OSC::ClipboardReadDecision::Approved;
        const std::size_t n = std::min(setSpec.size(), command.set.size() - 1);
        std::memcpy(command.set.data(), setSpec.data(), n);
        command.set[n] = 0;
        postCommand(command);
    }

    /// Keeps a heap client alive for the session's life.
    ///
    /// For installers that mint a client per session: the session destroys
    /// it after the model, so a CheckedPtr in the Terminal never outlives
    /// its target and the installer owes no teardown call.
    ///
    /// - Parameters:
    ///   - client: The client to own; attach it to the terminal separately.
    ///   - detach: Clears pointers installed by this client while the model
    ///     lock is held, before an external terminal is released.
    void adoptClientOwnership(std::unique_ptr<Ubiquitty::ClipboardClient> client,
        void (*detach)(Ubiquitty::Terminal&, Ubiquitty::ClipboardClient&))
    {
        m_ownedClients.push_back({ std::move(client), detach });
    }

    /// Pushes the pane's presentation state to the drain gate.
    ///
    /// Only a presented, unthrottled pane rides mid-burst EAGAIN gaps; the
    /// fold with the machine throttle is the runtime's, shared with every
    /// other ingest adapter. Call on presentation edges; the default presumes
    /// a presented pane.
    ///
    /// - Parameter presentation: The pane's presentation state.
    void setPresentation(PanePresentation presentation) { m_runtime->setPresentation(presentation); }
    /// Defers what the frontend does not read of the foreground until it does.
    ///
    /// - Parameter observation: What the frontend reads of the foreground.
    void setForegroundObservation(ForegroundObservation observation) { m_runtime->setForegroundObservation(observation); }

    /// Pushes the machine-wide load lever to the drain gate.
    ///
    /// - Parameter throttle: The machine's throttle state.
    void setMachineThrottle(MachineThrottle throttle) { m_runtime->setMachineThrottle(throttle); }

    /// Whether the drain currently rides mid-burst gaps.
    ///
    /// - Returns: The derived gate, for diagnostics and tests.
    [[nodiscard]] bool drainRidesBurst() const { return m_runtime->drainRidesBurst(); }

    /// Installs the runtime's raw host-byte tap (ZMODEM auto-transfer detect).
    ///
    /// The tap observes every ingested chunk before Core parses it, on the
    /// ingest thread — the sequential drain and the flood pipeline alike.
    ///
    /// - Parameters:
    ///   - tap: The tap, or null to disarm.
    ///   - context: Handed back to the tap verbatim.
    void setRawByteTap(TerminalRuntime::RawByteTap tap, void* context) { m_runtime->setRawByteTap(tap, context); }

    /// Installs the runtime's outbound byte tap (Stream Debug).
    ///
    /// The tap observes every chunk written to the stream (user input + replies),
    /// on whichever thread writes (ingest thread for replies, main for input).
    ///
    /// - Parameters:
    ///   - tap: The tap, or null to disarm.
    ///   - context: Handed back to the tap verbatim.
    void setOutboundByteTap(TerminalRuntime::OutboundByteTap tap, void* context) { m_runtime->setOutboundByteTap(tap, context); }

    /// Installs where this session's replies and input go when it has no
    /// stream.
    ///
    /// An embedder that feeds the host's bytes through ``feedBytes`` sends the
    /// replies they provoke back to the host through this sink, called on the
    /// feeding thread before ``feedBytes`` returns. Everything ``sendInput``
    /// carries — keys, pastes, and the reports the pointer and wheel ladders
    /// encode — reaches it too, on the sending thread, so the sink is the
    /// embedder's whole way to the host.
    ///
    /// - Parameters:
    ///   - sink: The sink, or null to disarm.
    ///   - context: Handed back to the sink verbatim.
    void setReplySink(TerminalRuntime::ReplySink sink, void* context)
    {
        const std::scoped_lock hold(*m_activeLock);
        m_runtime->setReplySink(sink, context);
    }

    /// Watches this session's host stream for a file-transfer preamble.
    ///
    /// The runtime owns the watch; this is the session's face of it, so a
    /// frontend that holds a session rather than a runtime can arm one. A hit
    /// raises ``EventKind::FileTransferDetected`` through the session's event
    /// flags and disarms, and arming again resumes after the transfer.
    void armFileTransferWatch() { m_runtime->armFileTransferWatch(); }

    /// Stops the watch until it is armed again.
    void disarmFileTransferWatch() { m_runtime->disarmFileTransferWatch(); }

    /// Whether the watch would judge the next bytes to arrive.
    [[nodiscard]] bool isFileTransferWatchArmed() const { return m_runtime->isFileTransferWatchArmed(); }

    /// Whether a detected preamble is still holding the ingest gates.
    ///
    /// While held, the loop stops reading the stream so the transfer frame
    /// stays off the screen and in the kernel queue for the transfer agent.
    [[nodiscard]] bool isFileTransferHoldActive() const { return m_runtime->isFileTransferHoldActive(); }

    /// Whether the drain's last pass filled its capacity; see
    /// ``TerminalRuntime::isDrainSaturated``.
    [[nodiscard]] bool isDrainSaturated() const { return m_runtime->isDrainSaturated(); }

    /// Whether the launch is still holding the child's output for the screen
    /// the window opens with; see ``OpeningScreen``.
    [[nodiscard]] bool isOpeningHoldActive() const { return m_runtime->isOpeningHoldActive(); }

    /// Writes the screen the window opens with and lets the child's output
    /// follow it.
    ///
    /// The bytes take the parser path the child's do, ahead of everything the
    /// child has written so far, which the hold kept in the kernel queue. Empty
    /// bytes release the hold alone, for a window whose screen could not be
    /// made. A launch that held nothing ignores the bytes rather than drop them
    /// into the middle of a live stream.
    ///
    /// - Parameter bytes: The opening screen.
    void finishOpeningScreen(ByteSpan bytes UBIQUITTY_NOESCAPE)
    {
        if (!m_runtime->isOpeningHoldActive())
            return;
        feedBytes(bytes);
        m_runtime->releaseOpeningHold();
        wakeIngest();
    }

    /// Lifts the ingest hold a detected preamble set, without re-arming.
    ///
    /// The frontend answers a detection — runs the transfer, declines it, or
    /// ignores the protocol — then calls this so the stream parses again.
    /// ``armFileTransferWatch()`` also lifts the hold.
    void releaseFileTransferHold() { m_runtime->releaseFileTransferHold(); }

    /// The detection the last ``EventKind::FileTransferDetected`` announced.
    ///
    /// - Returns: What fired, or a falsy Detection when nothing has.
    [[nodiscard]] Ubiquitty::Network::Transfer::Detection detectedFileTransfer() const { return m_runtime->detectedFileTransfer(); }

    /// How many flood bursts the ingest loop's pipeline has carried.
    ///
    /// Zero over a flood says the bursts stayed on the sequential passes —
    /// what a throttled or hidden pane must do; at least one says the
    /// pipelined reader engaged — what a presented flood must do.
    ///
    /// - Returns: The burst count since the loop was constructed.
    [[nodiscard]] std::uint64_t floodBurstCount() const { return m_loop.floodBurstCount(); }

    /// Drains every available host byte through Core, as one pass.
    ///
    /// The runtime reads the stream, bites the model lock at its measured
    /// grain, publishes each completed sync frame into the session's flag, and
    /// coalesces events into the sink. Call from the one thread that owns
    /// ingest; the outcome is scalar-only and says why the pass ended.
    ///
    /// - Returns: The pass's summary.
    DrainOutcome drain() { return m_runtime->drainAvailable(m_host); }

    /// Refreshes the cached process facts for a frontend that owns its drain loop.
    ///
    /// Call on the same thread as ``drain()``. The background ingest loop
    /// already owns this polling; remote and ended sessions have no local
    /// process facts to sample. Frontends compose titles from the cache.
    ///
    /// - Returns: Changes observed without consuming any terminal bytes.
    [[nodiscard]] DrainOutcome sampleProcessStateOffDrain()
    {
        if (m_ingestThread.joinable() || m_remote || m_externalTerminal || m_runtime->sessionEnd().didEnd)
            return {};
        return m_runtime->sampleProcessStateOffDrain(m_host);
    }

    /// Asks the coherence rule again for a frame it held at a drain's end, and
    /// announces it when admitted.
    ///
    /// A frontend that drains on its own readiness, rather than through the
    /// ingest loop, calls this when the wait it returned is up: a program that
    /// went quiet writes no byte that would ask for the frame. Same thread as
    /// ``drain()``.
    ///
    /// Also services the CRT saver while the input stream is idle.
    /// - Returns: Microseconds until either timer needs service, or no deadline.
    std::optional<std::uint64_t> publishHeldFrameIfDue()
    {
        auto due = m_runtime->publishHeldFrameIfDue(m_host);
        const auto idle = m_runtime->serviceDisplayIdle(m_host);
        if (idle.didChange)
            m_events.pending.fetch_or(1u << static_cast<unsigned>(EventKind::ScreenDirty), std::memory_order_release);
        if (idle.remainingTicks) {
            const std::uint64_t idleDue = (*idle.remainingTicks * 1'000'000ull + 59) / 60;
            due = due ? std::min(*due, idleDue) : idleDue;
        }
        return due;
    }

    /// Replaces the coherence rule's clock, so a test holds time still rather
    /// than sleeping through a rule's window. Before the first drain.
    ///
    /// - Parameters:
    ///   - clock: The clock, or null for the steady clock.
    ///   - context: Handed back to the clock verbatim.
    void setCoherenceClock(TerminalRuntime::CoherenceClock clock, void* context) { m_runtime->setCoherenceClock(clock, context); }

    /// Starts the ingest thread over the session's transport.
    ///
    /// The loop is the extracted one the Metal arm tuned; the thread is a
    /// C++23 jthread this session owns, so a caller that forgets to stop is a
    /// join on destruction rather than a leak. The loop waits on an
    /// independently owned duplicate of the transport's endpoint, which is
    /// the polling contract ``PAL::PollableByteStream`` states. No scheduling
    /// class is pinned here: that is a platform statement, and the smoke and
    /// the bench are what decide whether the default class costs throughput
    /// before a PAL hook buys it back.
    ///
    /// - Returns: Whether the loop started; false with no pollable transport.
    bool startIngest()
    {
        if (m_ingestThread.joinable())
            return false;
        PAL::Stream* stream = m_runtime->stream();
        if (!stream)
            return false;
        m_pollableEndpoint = stream->duplicateAsPollableStream();
        if (!m_pollableEndpoint)
            return false;
        // The flood reader's own duplicate: it reads raw on the burst's
        // thread, so it cannot share the endpoint the loop polls, and both
        // name the one kernel queue. A transport that cannot duplicate twice
        // keeps floods on the sequential passes, which is the loop's stated
        // fallback rather than a failure. UBIQUITTY_SWIFT_FLOOD=0 disarms the
        // pipeline on purpose, so a run that drives this session can be paired
        // against itself on one binary. It reaches only what constructs a
        // TerminalSession: bench_drain builds a TerminalRuntime and its own
        // reader loop, so the knob is inert there and its --pipeline flag
        // measures that loop rather than this one.
        const char* floodKnob = std::getenv("UBIQUITTY_SWIFT_FLOOD");
        const bool isFloodDisarmed = floodKnob && floodKnob[0] == '0';
        m_loop.adoptFloodEndpoint(isFloodDisarmed ? nullptr : stream->duplicateAsPollableStream());
        (void)m_loop.prepare();
        m_loop.resetStop();
        m_ingestThread = std::jthread([this] {
            // Before the loop, so every pass runs in the band the bridge's
            // ingest queue names: the two drivers run the same Core loop and
            // must differ only in who owns the thread.
            PAL::raiseCurrentThreadToInteractivePriority();
            m_loop.run(m_pollableEndpoint->handle());
        });
        return true;
    }

    /// Stops the ingest thread and joins it. Idempotent.
    ///
    /// The wake ends an idle park immediately, so a stop is bounded by one
    /// pass rather than a poll timeout.
    void stopIngest()
    {
        if (!m_ingestThread.joinable())
            return;
        m_loop.requestStop();
        m_loop.raiseWake();
        m_ingestThread.join();
        m_loop.adoptFloodEndpoint(nullptr);
        m_pollableEndpoint.reset();
    }

    /// Raises the ingest wake, from any thread.
    ///
    /// A queued write (paste backpressure) and a posted command both need the
    /// loop awake; the byte is coalesced, so raising twice costs one wake.
    void wakeIngest() { m_loop.raiseWake(); }

    /// Returns and clears the total bytes the ingest loop has drained.
    ///
    /// A monotonic tap for a smoke or a bench; the per-pass outcome stays on
    /// the loop's thread.
    [[nodiscard]] std::uint64_t takeDrainedBytes() { return m_drainedBytes.exchange(0, std::memory_order_acquire); }

    /// What the ingest loop delivered since the last take, for the perf log.
    struct PerfDrainTally {
        /// Bytes parsed.
        std::uint64_t bytes = 0;
        /// Deliveries that carried them.
        std::uint32_t deliveries = 0;
        /// Parse time, summed over the deliveries.
        std::uint64_t ingestMicroseconds = 0;
        /// The slowest single delivery's parse time.
        std::uint32_t longestIngestMicroseconds = 0;
    };

    /// Starts tallying deliveries for the perf log.
    ///
    /// Off until asked, so a session nobody is measuring pays one relaxed load
    /// per delivery and nothing more. A tally of its own rather than
    /// ``takeDrainedBytes()``, which a bench reading the same session drains.
    void startTallyingPerfDrains() { m_isTallyingPerfDrains.store(true, std::memory_order_relaxed); }

    /// Returns and clears the deliveries tallied since the last take.
    [[nodiscard]] PerfDrainTally takePerfDrainTally()
    {
        PerfDrainTally tally;
        tally.bytes = m_perfDrainBytes.exchange(0, std::memory_order_acquire);
        tally.deliveries = m_perfDrainDeliveries.exchange(0, std::memory_order_acquire);
        tally.ingestMicroseconds = m_perfDrainIngestMicroseconds.exchange(0, std::memory_order_acquire);
        tally.longestIngestMicroseconds = m_perfDrainLongestIngestMicroseconds.exchange(0, std::memory_order_acquire);
        return tally;
    }

    /// Returns and clears the coalesced event flags, one bit per
    /// ``EventKind``.
    ///
    /// The sink only ever sets bits on the writer thread, so a reader taking
    /// them here sees each event once and none twice — the same one-pass-per-
    /// drain discipline the Metal arm's bridge delivers through a delegate.
    ///
    /// Polls the pane link first, which reads the model under the session's
    /// lock and delivers the link's notifications after releasing it, so the
    /// caller must not hold ``lockForRead()``.
    /// Takes the bells this pane rang since the last take, from every path
    /// that delivers them: the ingest loop's drains, a frontend's own feed
    /// and a remote host's bell.
    ///
    /// A frontend plays a bell for the pane whose count this is, so a bell
    /// never sounds for a pane that did not ring.
    ///
    /// - Returns: How many bells.
    [[nodiscard]] std::uint32_t takeBellsRung() { return m_events.bellsRung.exchange(0, std::memory_order_relaxed); }

    [[nodiscard]] std::uint32_t takeEventFlags()
    {
        if (!m_paneLinkObserver)
            observePaneLink(m_paneEventObserver);
        if (auto* observer = m_paneLinkObserver.get()) {
            const auto weakThis = weakPtr();
            auto& link = paneLink();
            link.observe(*observer);
            link.pollEvents(link.clockMicroseconds());
            if (!weakThis)
                return 0;
        }
        // Re-armed before the bits are taken: a pass that lands bits after
        // this store fires the signal again, and one that landed them before
        // it is served by the exchange below. Either way no event waits for a
        // signal that already fired.
        m_isDrainSignalArmed.store(false, std::memory_order_release);
        return m_events.pending.exchange(0, std::memory_order_acquire);
    }

    /// The style byte of the most recent bell, meaningful when the bell bit
    /// was set.
    [[nodiscard]] std::uint32_t lastBellStyle() const { return m_events.bellStyle.load(std::memory_order_relaxed); }

    /// The bell cue this session's configuration names.
    ///
    /// The fold of the two `[bell]` switches, for a path that has no drain
    /// outcome to read one off — a bell reported by a host rather than parsed
    /// from a local stream.
    ///
    /// - Returns: Off, Audio, Visual or Both.
    [[nodiscard]] Ubiquitty::BellMode configuredBellMode() const
    {
        return Preferences::Effective::bellMode(m_configuration);
    }

    /// Bounds one frontend delivery so continuous input cannot monopolize it.
    [[nodiscard]] static constexpr std::size_t maxFontRequestsPerDelivery() { return OSC::FontRequestQueue::maxPendingRequests; }

    /// Owns one immutable resolution and its matching application receipt.
    struct UBIQUITTY_NONCOPYABLE FontRequestDelivery {
        OSC::FontRequestReceipt receipt;
        Preferences::FontSwapResolution resolution;
    };

    /// Leases the oldest font request and resolves it against actual font state.
    ///
    /// Receipt and resolution are captured under the same lock. A notification
    /// is only a reason to look; a lease, not an observed generation, identifies
    /// the operation a frontend may acknowledge.
    ///
    /// - Parameters:
    ///   - configuredFamily: The family the configuration names, empty for none.
    ///   - configuredSizePoints: The size it names, zero for none.
    /// - Returns: An independent request; an invalid receipt means none is ready.
    [[nodiscard]] FontRequestDelivery beginFontRequest(
        const std::string& configuredFamily, double configuredSizePoints)
    {
        const std::scoped_lock hold(*m_activeLock);
        if (Ubiquitty::Terminal* term = modelTerm()) {
            const OSC::FontRequestLease lease = term->fontRequests.begin();
            if (!lease.receipt.isValid()) {
                return {};
            }
            Preferences::FontSwapRequest request;
            request.requestedFamily = lease.operation.family;
            request.requestedSizePoints = lease.operation.sizePoints;
            request.sizeDelta = lease.operation.sizeDelta;
            request.liveFamily = term->fontRequests.actualFamily();
            request.liveSizePoints = term->fontRequests.actualSizePoints();
            request.configuredFamily = configuredFamily;
            request.configuredSizePoints = configuredSizePoints;
            request.isSizeResetOnly = lease.operation.sizeSource == OSC::FontSizeSource::Configured;
            return { lease.receipt, Preferences::resolveFontSwap(request) };
        }
        return {};
    }

    /// Reports one adoption outcome without consuming a newer queued operation.
    ///
    /// Refusal retains the actual font. Successful adoption reports the face
    /// actually drawn, including substitutions. A newer accepted query family
    /// remains visible until its own operation completes.
    ///
    /// - Parameters:
    ///   - receipt: The lease returned with the applied resolution.
    ///   - outcome: Whether the frontend adopted the request.
    ///   - actualFamily: The family drawn after a successful adoption.
    ///   - actualSizePoints: The size drawn after a successful adoption.
    /// - Returns: Whether this receipt completed the current lease.
    [[nodiscard]] bool acknowledgeFontRequest(const OSC::FontRequestReceipt& receipt,
        OSC::FontAdoptionOutcome outcome, const std::string& actualFamily, double actualSizePoints)
    {
        bool shouldSignal = false;
        {
            const std::scoped_lock hold(*m_activeLock);
            Ubiquitty::Terminal* term = modelTerm();
            if (!term || actualFamily.size() > Ubiquitty::Terminal::maxFontLength
                || !term->fontRequests.acknowledge(receipt, outcome, actualFamily, actualSizePoints)) {
                return false;
            }
            if (term->fontRequests.canPublishActualFamily()) {
                Ubiquitty::setFontString(*term, term->fontRequests.actualFamily());
            }
            shouldSignal = term->fontRequests.canBegin();
            if (shouldSignal) {
                m_events.pending.fetch_or(1u << static_cast<unsigned>(EventKind::FontSwapRequested), std::memory_order_release);
            }
        }
        if (shouldSignal)
            signalDrain();
        return true;
    }

    /// Publishes both actual font axes after startup, zoom, or configuration.
    ///
    /// A pending lease is invalidated and retained for resolution against the
    /// external change. Notification callbacks run outside the model lock.
    ///
    /// - Parameters:
    ///   - family: The family the frontend is drawing.
    ///   - sizePoints: The size it is drawing at.
    /// - Returns: Whether the state is valid and was published.
    [[nodiscard]] bool publishActualFont(const std::string& family, double sizePoints)
    {
        bool shouldSignal = false;
        {
            const std::scoped_lock hold(*m_activeLock);
            Ubiquitty::Terminal* term = modelTerm();
            if (!term || family.size() > Ubiquitty::Terminal::maxFontLength || !term->fontRequests.publishActualFont(family, sizePoints)) {
                return false;
            }
            if (term->fontRequests.canPublishActualFamily()) {
                Ubiquitty::setFontString(*term, family);
            }
            shouldSignal = term->fontRequests.canBegin();
            if (shouldSignal) {
                m_events.pending.fetch_or(1u << static_cast<unsigned>(EventKind::FontSwapRequested), std::memory_order_release);
            }
        }
        if (shouldSignal)
            signalDrain();
        return true;
    }

    /// Starts the visual bell's envelope from now.
    ///
    /// The start tick lives here rather than on a view because two of this
    /// session's frame routes read it — the live capture and the boundary
    /// capture, which alternate while output arrives — and a flash spelled in
    /// one of them strobes off on every frame the other produced.
    void noteBellFlashStarted() { m_bellFlashStartTick.store(frameTicksNow(), std::memory_order_relaxed); }

    /// The visual bell's strength at `ticks`.
    ///
    /// The peak is this session's `[bell] visual_intensity`, so the curve and
    /// its height are both Core's and a frontend supplies only the tick it is
    /// asking about.
    ///
    /// - Parameter ticks: The tick to measure the envelope at.
    /// - Returns: The alpha, zero once the envelope has run out.
    [[nodiscard]] float bellFlashAlphaAt(std::uint32_t ticks) const
    {
        return Ubiquitty::bellFlashAlpha(m_bellFlashStartTick.load(std::memory_order_relaxed), ticks,
            static_cast<float>(Preferences::Effective::bellVisualIntensity(m_configuration)));
    }

    /// The reader's `[cursor] alpha`, as the frame's 0..1 multiplier.
    ///
    /// Asked of the session rather than read off the preferences by each
    /// caller, because two of this session's frame routes assemble their
    /// options independently: the live capture on the drive's thread and the
    /// boundary capture on the ingest thread, alternating while a reader
    /// types. A translucent cursor spelled in one of them and defaulted in the
    /// other pulses between the two opacities from one frame to the next.
    ///
    /// - Returns: The alpha in 0..1; opaque when the reader named none.
    [[nodiscard]] float cursorAlphaUnit() const
    {
        return static_cast<float>(Preferences::Effective::cursorAlpha(m_configuration)) / 255.0f;
    }

    /// Everything the most recent bell asked for.
    ///
    /// Read when ``EventKind::Bell`` drains. The four travel together because
    /// they are one decision taken on one drain, and a frontend executing
    /// three of them is the half-implemented bell this shape exists to
    /// prevent.
    ///
    /// - Returns: The cue.
    [[nodiscard]] Ubiquitty::BellCue lastBellCue() const
    {
        Ubiquitty::BellCue cue;
        cue.mode = static_cast<Ubiquitty::BellMode>(m_events.bellStyle.load(std::memory_order_relaxed));
        cue.shouldPopWindow = m_events.bellPopsWindow.load(std::memory_order_relaxed);
        cue.shouldRaiseUrgency = m_events.bellRaisesUrgency.load(std::memory_order_relaxed);
        cue.shouldNotify = m_events.bellWantsNotification.load(std::memory_order_relaxed);
        return cue;
    }

    /// The status of the most recent settled push, meaningful when the
    /// ``EventKind::PastePushSettled`` bit was set.
    [[nodiscard]] std::uint32_t lastPastePushStatus() const { return m_events.pastePushStatus.load(std::memory_order_relaxed); }

    /// Returns and clears whether a completed sync frame is waiting.
    ///
    /// Set under the model lock at each DECSET-2026 boundary, consumed by the
    /// drive when it decides to present. Coalesced: three frames completed
    /// during one pass read as one, which is the newest one — the only frame
    /// still capturable.
    [[nodiscard]] bool takeSyncFrameReady() { return m_frameReady.exchange(false, std::memory_order_acquire); }

    /// Registers the geometry the boundary capture reads, and arms it.
    ///
    /// The session captures each completed DECSET-2026 frame itself, at the
    /// boundary, under the model lock — the only place the capture is
    /// frame-coherent by construction. Polling ``takeSyncFrameReady`` from an
    /// event hop and capturing there reads a terminal that may have advanced
    /// past the boundary: a torn frame, the artefact 2026 exists to prevent.
    /// A capture hook was the other shape, and an ingest-thread-under-lock
    /// callback is a surface that invites a frontend to run arbitrary work
    /// under the model lock; owning the capture here removes the surface
    /// instead of documenting restraint on it.
    ///
    /// Call again whenever the geometry changes — a resize, a scroll, a style
    /// knob. The copy is taken under the model lock, so a capture in flight
    /// sees either the old geometry or the new, never a tear of the two.
    ///
    /// - Parameter geometry: The durable knobs the boundary capture is steered
    ///   by; the per-frame borrowed tiers are assembled at the boundary.
    void setSyncFrameCaptureGeometry(const SyncFrameCaptureGeometry& geometry)
    {
        const std::scoped_lock hold(*m_activeLock);
        if (!m_syncCaptures[0]) {
            for (Rendering::FrameCapture*& capture : m_syncCaptures)
                capture = Rendering::FrameCapture::make();
        }
        // Pending frames carry the previous placement and resolved style.
        // A new registration invalidates both before any old frame can present.
        (void)m_syncSlot.drain();
        m_syncCaptureGeometry = geometry;
        ++m_syncViewGeneration;
        // New geometry, new lineage: the next boundary records Full, and every row is owed.
        m_retainedBaseline = {};
        oweAllSyncRows();
        m_isSyncCaptureArmed = std::ranges::all_of(m_syncCaptures, [](const auto* capture) { return capture; });
        m_host.setDrainPassPresentation(m_isSyncCaptureArmed && m_isSyncFramePresented.load(std::memory_order_relaxed)
                ? PanePresentation::Presented
                : PanePresentation::Hidden);
    }

    /// Enables local renderer captures while its surface can present them.
    ///
    /// Remote snapshot transport has its own capture path. Pending local frames
    /// are discarded on visibility edges so reveal cannot consume stale rows.
    ///
    /// The frontend restates the presentation on every screen update, and only
    /// an edge needs the model lock: an unchanged restatement returns before
    /// it, so it never waits out a parse bite.
    ///
    /// - Parameter presentation: Whether the local renderer is presented.
    void setSyncFramePresentation(PanePresentation presentation)
    {
        const bool isPresented = presentation == PanePresentation::Presented;
        if (m_isSyncFramePresented.load(std::memory_order_relaxed) == isPresented) {
            return;
        }
        const std::scoped_lock hold(*m_activeLock);
        if (m_isSyncFramePresented.load(std::memory_order_relaxed) == isPresented) {
            return;
        }
        m_isSyncFramePresented.store(isPresented, std::memory_order_relaxed);
        m_host.setDrainPassPresentation(m_isSyncCaptureArmed && isPresented ? PanePresentation::Presented : PanePresentation::Hidden);
        (void)m_syncSlot.drain();
        m_retainedBaseline = {};
        oweAllSyncRows();
    }

    /// Records the run under the pointer for the boundary capture to draw.
    ///
    /// Its own entry rather than a field of the placement registration: a
    /// pointer moves far more often than a window changes shape, and the two
    /// folded together would leave a boundary frame emphasising whichever run
    /// was under the pointer when the window was last resized.
    ///
    /// Rows are absolute, so a run recorded once stays the same run as output
    /// scrolls beneath a pointer that never moved; the capture converts them
    /// against its own frame.
    ///
    /// Every frontend that resolves a hover calls this, including with an
    /// inactive run when the pointer leaves — a boundary frame that kept an
    /// ended hover would emphasise a link nobody is pointing at.
    ///
    /// - Parameter hover: The run under the pointer, or an inactive one.
    void setSyncFrameHoverLink(const Rendering::FrameHoverLink& hover)
    {
        const std::scoped_lock hold(*m_activeLock);
        m_syncFrameHover = hover;
    }

    /// Returns the newest boundary capture, retained, or null when none waits.
    ///
    /// Coalesced like ``takeSyncFrameReady``: boundaries that completed between
    /// drains overwrote one another and the newest is the one returned — the
    /// only frame still worth presenting. The capture stays immutable until
    /// the caller releases its retained reference; the producer skips a slot
    /// with an external holder instead of recycling it.
    ///
    /// - Returns: The capture, with a reference the caller owns, or null.
    [[nodiscard]] Rendering::FrameCapture* takeSyncFrame() UBIQUITTY_RETURNS_RETAINED
    {
        const int drained = m_syncSlot.drain();
        if (drained < 0)
            return nullptr;
        Rendering::FrameCapture* capture = m_syncCaptures[drained];
        settleOwedSyncRows(*capture);
        capture->swiftReferences.fetch_add(1, std::memory_order_relaxed);
        return capture;
    }

    /// Boundary frames replaced by a newer capture before a drain — the judder
    /// metric, mirroring the Metal arm's pending-overwrite count.
    [[nodiscard]] std::uint64_t syncFrameOverwriteCount()
    {
        return m_syncSlot.overwriteCount();
    }

    /// Boundary frames a drain promoted toward the screen.
    [[nodiscard]] std::uint64_t syncFramePromoteCount()
    {
        return m_syncSlot.promoteCount();
    }

    /// Boundary captures attempted after the session was armed.
    [[nodiscard]] std::uint64_t syncFrameCaptureAttemptCount() const
    {
        return m_syncCaptureAttemptCount.load(std::memory_order_relaxed);
    }

    /// Boundary captures successfully published into the exchange.
    [[nodiscard]] std::uint64_t syncFramePublicationCount() const
    {
        return m_syncFramePublicationCount.load(std::memory_order_relaxed);
    }

    /// Boundary captures skipped because no unheld slot was available.
    [[nodiscard]] std::uint64_t syncFrameNoSlotCount() const
    {
        return m_syncFrameNoSlotCount.load(std::memory_order_relaxed);
    }

    /// Boundaries announced to the publish hook, counted before the capture's
    /// armed check: zero says the drain never reported one.
    [[nodiscard]] std::uint64_t syncFrameBoundaryAnnounceCount() const
    {
        return m_syncFrameBoundaryAnnounceCount.load(std::memory_order_relaxed);
    }

    /// Whether the grid holds a mutation no capture has carried yet.
    ///
    /// Asked on the ingest thread under the model lock, to tell a publish whose
    /// grid has moved since the standing capture was taken from one whose has
    /// not. The second has nothing to replace: its capture is still what the
    /// screen shows.
    ///
    /// - Returns: Whether anything is unpainted, false when there is no grid.
    [[nodiscard]] bool hasGridWorkSinceLastPaint()
    {
        Ubiquitty::Terminal* term = modelTerm();
        return term && term->grid && term->grid->hasUnpaintedWork();
    }

    /// Settles that replaced a capture nobody had taken.
    ///
    /// A capture still sitting in the slot is older than the settle that found
    /// it, and a consumer preferring the slot would present a grid the screen
    /// has already left. A count rising with no stale glyph on the glass is
    /// this working; a stale glyph while it rises means the replacement is
    /// being stranded further down.
    [[nodiscard]] std::uint64_t staleCaptureReplacedCount() const
    {
        return m_staleCaptureReplacedCount.load(std::memory_order_relaxed);
    }

    /// Whether the session holds the capture storage a boundary writes into.
    [[nodiscard]] bool isSyncCaptureArmed() const { return m_isSyncCaptureArmed; }

    /// Boundaries the drain asked the host to publish, before its throttle.
    [[nodiscard]] std::uint64_t syncFrameBoundaryRequestCount() const { return m_host.boundaryRequestCount(); }

    /// Boundaries the host's publish throttle withheld.
    [[nodiscard]] std::uint64_t syncFrameBoundaryThrottledCount() const { return m_host.boundaryThrottledCount(); }

    /// Completed DECSET-2026 frames the parser reported to a drain.
    [[nodiscard]] std::uint64_t syncClearSeenCount() const { return m_runtime->syncClearSeenCount(); }

    /// Settles that declined to publish for an incoherent frame.
    [[nodiscard]] std::uint64_t settlePublishRefusedCount() const { return m_runtime->settlePublishRefusedCount(); }

    /// Applies a theme's palette to the model, whole, under the model lock.
    ///
    /// The overwrite is the palette's own designed theme operation; runtime
    /// OSC 4/10/11 edits land on the applied table afterwards. The
    /// special-colour slots stay untouched — they are set by the application
    /// through OSC 5, not by a theme.
    ///
    /// - Parameters:
    ///   - indexedRGB: 768 bytes, red/green/blue per index 0–255.
    ///   - defaultsRGB: 9 bytes — default foreground, default background,
    ///     cursor, three each in that order.
    /// - Returns: Whether both spans held their exact lengths and a model
    ///   existed to apply them to.
    [[nodiscard]] bool applyPalette(AlphaSpan indexedRGB UBIQUITTY_NOESCAPE, AlphaSpan defaultsRGB UBIQUITTY_NOESCAPE)
    {
        return applyPalette(indexedRGB, defaultsRGB, AlphaSpan {});
    }

    /// Applies a theme's palette to the model with its per-index opacity.
    ///
    /// The alpha lands in the palette beside the colour, which is where the
    /// frame capture reads it from — one table for every arm, rebuilt with
    /// each capture from the live palette, so an OSC 4 write that made a slot
    /// opaque is what the next frame draws.
    ///
    /// - Parameters:
    ///   - indexedRGB: 768 bytes, red/green/blue per index 0–255.
    ///   - defaultsRGB: 9 bytes — default foreground, default background,
    ///     cursor, three each in that order.
    ///   - indexedAlpha: 256 bytes, one opacity per index, or empty for
    ///     opaque throughout.
    /// - Returns: Whether the spans held their exact lengths and a model
    ///   existed to apply them to.
    [[nodiscard]] bool applyPalette(AlphaSpan indexedRGB UBIQUITTY_NOESCAPE, AlphaSpan defaultsRGB UBIQUITTY_NOESCAPE, AlphaSpan indexedAlpha UBIQUITTY_NOESCAPE)
    {
        if (indexedRGB.size() != 768 || defaultsRGB.size() != 9)
            return false;
        if (!indexedAlpha.empty() && indexedAlpha.size() != 256)
            return false;
        TermPalette palette;
        palette.seedDefaults();
        // Widened, not stored raw. TermPalette's channels are 16-bit and the
        // model converts at its edges — seedDefaults widens with channel8To16,
        // channel16To8 and the renderers narrow back by 257. A byte written
        // straight into the 16-bit field is 1/257 of the colour asked for and
        // narrows back to zero, so a theme applied here rendered black.
        for (int index = 0; index < 256; ++index) {
            palette.indexed[index].red = channel8To16(indexedRGB[static_cast<size_t>(index) * 3 + 0]);
            palette.indexed[index].green = channel8To16(indexedRGB[static_cast<size_t>(index) * 3 + 1]);
            palette.indexed[index].blue = channel8To16(indexedRGB[static_cast<size_t>(index) * 3 + 2]);
            palette.indexedAlpha[static_cast<size_t>(index)] = indexedAlpha.empty() ? 255 : indexedAlpha[static_cast<size_t>(index)];
        }
        palette.defaultForeground = { channel8To16(defaultsRGB[0]), channel8To16(defaultsRGB[1]), channel8To16(defaultsRGB[2]) };
        palette.defaultBackground = { channel8To16(defaultsRGB[3]), channel8To16(defaultsRGB[4]), channel8To16(defaultsRGB[5]) };
        palette.cursor = { channel8To16(defaultsRGB[6]), channel8To16(defaultsRGB[7]), channel8To16(defaultsRGB[8]) };
        return applyResolvedPalette(palette);
    }

    /// Applies a named theme using the same adaptation as session launch.
    ///
    /// - Parameter index: The index in the theme catalog.
    /// - Returns: Whether the theme exists and the session accepts it.
    [[nodiscard]] bool applyTheme(int index)
    {
        const Themes::Theme* theme = Themes::get(index);
        const std::optional<TermPalette> palette = Themes::resolveThemePalette(index, Accessibility::paletteAdaptation());
        if (!theme || !palette || !applyResolvedPalette(*palette)) {
            return false;
        }
        const std::scoped_lock hold(m_themeIdentityLock);
        m_chosenThemeName = theme->name;
        m_themeIdentity.reset();
        return true;
    }

    /// Applies a palette locally or requests its application by the owning host.
    ///
    /// - Parameter palette: The live palette and reset baseline.
    /// - Returns: Whether the local model changed or the host request was sent.
    [[nodiscard]] bool applyResolvedPalette(const TermPalette& palette)
    {
        // A remote mirror changes only through snapshots from its owning host.
        // Transport callbacks can take the model lock, so sending holds no lock.
        if (m_remote)
            return m_remote->core && m_remote->core->sendPalette(palette, palette);
        {
            const std::scoped_lock hold(*m_activeLock);
            Ubiquitty::Terminal* term = modelTerm();
            if (!term)
                return false;
            term->palette = palette;
            term->paletteBaseline = palette;
            Themes::rethemeDefaults(term);
        }
        // The published epoch lags this write until the next drain publishes,
        // so the theme identity would otherwise still match it.
        const std::scoped_lock identityHold(m_themeIdentityLock);
        m_themeIdentity.reset();
        return true;
    }

    /// The shape a frontend's proportional-text measurer takes across a
    /// language seam: a function and the context it reads.
    ///
    /// - Parameters:
    ///   - context: The frontend's object, handed back on every call.
    ///   - codePoints: The run's text.
    ///   - count: How many code points.
    ///   - glyphsOut: Receives the shaped glyphs, up to `capacity`.
    ///   - faceBits: The run's face, `Rendering::FontFace` as its bits: 1
    ///     bold, 2 italic.
    ///   - weight: The run's SGR 1:n variable weight, 0 for the face's own.
    ///   - capacity: How many glyphs `glyphsOut` holds.
    ///   - cellAdvanceOut: Receives one cell's advance in the glyphs' pixels.
    /// - Returns: How many glyphs were shaped, or zero when the frontend
    ///   cannot say.
    using TextMeasureFunction = int (*)(void* context, const std::uint32_t* codePoints, int count, std::uint8_t faceBits, std::uint32_t weight,
        Ubiquitty::ProportionalShapedGlyph* glyphsOut, int capacity, float* cellAdvanceOut);

    /// Installs the frontend's proportional-text measurer on the model
    /// terminal (Docs/proportional-text.md), under the model lock.
    ///
    /// A null function uninstalls it, after which Core answers a measurement
    /// with the grid-face width. The context must outlive the installation; a
    /// renderer replaced under a live window reinstalls with its own.
    ///
    /// - Parameters:
    ///   - measure: The measurer, or null.
    ///   - context: What the measurer reads.
    void setTextMeasurer(TextMeasureFunction measure, void* context)
    {
        {
            const std::scoped_lock hold(*m_activeLock);
            m_textMeasurer.measure = measure;
            m_textMeasurer.context = context;
            if (Ubiquitty::Terminal* term = modelTerm())
                installTextMeasurer(*term);
        }
        publishProportionalFaces();
    }

    /// Publishes the run faces a remote pane's host is owed, once the
    /// frontend's measurer can shape: a renderer that builds its face on its
    /// first frame calls this after that frame (Docs/proportional-text.md).
    /// Main-thread only, outside the model lock; nothing owed costs nothing.
    void publishOwedProportionalFaces()
    {
        if (m_areProportionalFacesOwed)
            publishProportionalFaces();
    }

    /// Installs native rich-text shaping without entering the UI thread from
    /// the parser. The context lives at least as long as this installation.
    ///
    /// - Parameters:
    ///   - shape: The native callback, or null to uninstall it.
    ///   - context: Borrowed callback state.
    void setRichTextShaper(RichText::Shaper::Function shape, void* context)
    {
        const std::scoped_lock hold(*m_activeLock);
        m_richTextShaper = { shape, context };
        if (Ubiquitty::Terminal* term = modelTerm()) {
            term->typography.shaper = m_richTextShaper;
            ++term->typography.epoch;
        }
    }

    /// Installs a revision-pinned native font catalog without entering the UI.
    /// The borrowed context outlives this installation, including remote callbacks.
    /// - Parameters:
    ///   - query: The native callback, or null to uninstall it.
    ///   - context: Borrowed state for the callback's renderer snapshot.
    void setFontCatalogProvider(RichText::FontCatalogProvider::Function query, void* context)
    {
        const std::scoped_lock hold(*m_activeLock);
        m_fontCatalogProvider = { query, context };
        if (Ubiquitty::Terminal* term = modelTerm())
            term->typography.fontCatalog = m_fontCatalogProvider;
    }

    /// Publishes the frontend's typography table to the model terminal, under
    /// the model lock (Docs/rich-text.md, "The measurement model").
    ///
    /// The table rides every model terminal the session later holds. Equal
    /// tables change nothing, so a frontend may publish whenever it cuts a
    /// renderer; a table that is not well formed is refused.
    ///
    /// - Parameter facts: The table.
    /// - Returns: Whether the table was adopted.
    bool publishTypography(const RichText::TypographyFacts& facts)
    {
        if (!RichText::isAdoptable(facts))
            return false;
        {
            const std::scoped_lock hold(*m_activeLock);
            if (m_typography && *m_typography == facts)
                return true;
            m_typography = std::make_shared<const RichText::TypographyFacts>(facts);
            if (Ubiquitty::Terminal* term = modelTerm())
                installTextMeasurer(*term);
        }
        // A remote pane's host measures for its programs, in this client's
        // fonts.
        if (m_remote && m_remote->core)
            m_remote->core->publishTypography(facts);
        publishProportionalFaces();
        return true;
    }

    /// ``publishTypography(const RichText::TypographyFacts&)`` from a builder,
    /// for the Swift renderer.
    ///
    /// - Parameter builder: The filled table.
    /// - Returns: Whether the table was adopted.
    bool publishTypographyFrom(const RichText::TypographyFactsBuilder& builder) { return publishTypography(builder.facts()); }

    /// The table the session publishes, or null; the caller holds the model
    /// lock.
    [[nodiscard]] std::shared_ptr<const RichText::TypographyFacts> publishedTypography() const { return m_typography; }

    /// A run of code points, named for the language seam: Swift imports a
    /// span only through an alias.
    using CodePointSpan = std::span<const std::uint32_t>;

    /// The cells a proportional run needs, as a `q=1` query is answered
    /// (Docs/proportional-text.md): through the installed measurer, else the
    /// grid face. The caller holds the model lock.
    ///
    /// - Parameter codePoints: The run's text, set in the regular face.
    /// - Returns: The cell count, or -1 without a model terminal.
    [[nodiscard]] int proportionalRunCells(CodePointSpan codePoints)
    {
        Ubiquitty::Terminal* term = modelTerm();
        return term ? OSC::TextSizing::proportionalRunCells(*term, codePoints, Ubiquitty::ProportionalRunStyle {}) : -1;
    }

    /// Applies the settings the model terminal carries to it, under the model
    /// lock: the password-prompt heuristic, the bell cue, the ambiguous-width
    /// choice, the OSC 52 clipboard policy and the per-OSC / per-DCS sequence
    /// gate, each of which the seed sets at launch and a live settings change
    /// moves afterwards.
    ///
    /// Named for the model rather than for `[security]` because the set is
    /// what a terminal holds, not what a config section is called: the bell
    /// arrived here for the same reason the heuristic did, and a second walker
    /// for it would take the same lock over the same surfaces one call later.
    ///
    /// - Parameter config: The config fold as it stands.
    void applyTerminalPreferences(const Preferences::Values& config)
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return;
        // A tmux-routed pane is fed by the session in UTF-8 whatever the
        // reader's encoding says, so re-encoding it would decode correct bytes
        // wrongly — the frontend arm skips those panes for the same reason.
        const EncodingReseed encoding = (m_tmuxRoutingClient || m_tmuxPrimaryStream) ? EncodingReseed::KeepCurrent : EncodingReseed::FollowConfiguration;
        reseedTerminalConfiguration(*term, config, encoding);
    }

    /// Applies the cursor preferences after the reader picked a shape, under
    /// the model lock; see
    /// ``Ubiquitty/Preferences/Effective/applyPickedCursorPreferences``.
    ///
    /// Separate from ``applyTerminalPreferences`` because the two events want
    /// opposite answers about `cursorStyle`: a reload leaves a running
    /// program's DECSCUSR standing, a pick hands the shape back to the reader.
    ///
    /// - Parameter config: The config fold as it stands, the pick already in it.
    /// - Returns: Whether a model existed to apply it to.
    bool applyPickedCursorPreferences(const Preferences::Values& config)
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        Preferences::Effective::applyPickedCursorPreferences(*term, config);
        return true;
    }

    /// Re-derives the model's palette from the launch theme, under the model
    /// lock.
    ///
    /// For the moment the palette adaptation moves — the system's contrast
    /// switch, a reloaded colour-vision mode: the theme is applied afresh
    /// under the new adaptation, so the palette is the theme adapted once
    /// rather than the last palette adapted again. Moves the same epoch the
    /// OSC handlers and ``applyPalette`` move.
    ///
    /// - Parameter config: The resolved config the theme ladder reads.
    /// - Returns: Whether a model existed to re-seed.
    bool reseedLaunchTheme(const Preferences::Values& config)
    {
        Accessibility::paletteAdaptation().mode = Preferences::Effective::colorblindMode(config);
        return applyTheme(launchThemeIndex(config));
    }

    /// The frame tick every blink curve in a frame reads.
    ///
    /// The environment's pinned tick when `UBIQUITTY_FAKE_TICKS` is set —
    /// read per call, so a harness can move the pin between frames — and the
    /// monotonic clock otherwise.
    ///
    /// - Returns: The tick.
    [[nodiscard]] static std::uint32_t frameTicksNow()
    {
        if (const char* pinned = std::getenv("UBIQUITTY_FAKE_TICKS"))
            return static_cast<std::uint32_t>(std::strtoul(pinned, nullptr, 10));
        return Ubiquitty::PAL::monotonicTicks().count;
    }

    /// Finds the link under a visible cell, keeping the result for the
    /// accessors below.
    ///
    /// The scan is Core's — `Screen::linkAtVisibleCell` decides what counts as
    /// a link, stitches a soft-wrapped run back together and prefers a
    /// program's own OSC 8 declaration over a guess — and this adds the model
    /// lock and the scratch it wants, neither of which a frontend should be
    /// holding.
    ///
    /// - Parameters:
    ///   - viewOffset: Rows scrolled back from the live grid.
    ///   - row: A zero-based visible row.
    ///   - column: A zero-based column.
    /// - Returns: Whether a link covers the cell.
    /// - Parameter scope: How much of the scan to run; see
    ///   ``Screen/LinkScanScope``. A bare hover asks for `Declared`, which is
    ///   one cell read rather than a sweep of the row.
    bool resolveLinkAtVisibleCell(int viewOffset, int row, int column, Screen::LinkScanScope scope)
    {
        // Announced, like the render capture: this runs on the main thread on
        // every pointer move, and a plain acquire against an unfair mutex is
        // one the ingest thread can hold off indefinitely under a flood — the
        // main thread stalls and the render it drives becomes a slideshow. A
        // try would answer the other way and be no better: measured under a
        // flood it succeeded once in 2196 attempts, which leaves the affordance
        // dead for the whole burst. The announcement makes the drain's unlock
        // donate the lock instead, bounding the wait at about one bite.
        if (scope == Screen::LinkScanScope::Any)
            ++m_wideLinkScanCount;
        lockForRead();
        const bool found = m_linkResolver.resolveAt(modelTerm(), viewOffset, row, column, scope);
        m_resolvedLinkTextDigest = 0;
        m_resolvedLinkID = 0;
        if (found) {
            // Read through the same visible rows the scan read, so the frame's
            // own cells digest alike while the text stands.
            const Ubiquitty::Terminal* term = modelTerm();
            const auto& link = m_linkResolver.link();
            // The hovered cell's id, for an OSC 8 run only: it is the per-cell
            // membership test the frame draws the run by, and a run the text
            // scan found has no id on any of its cells to test against.
            if (link.isHyperlink) {
                const Ubiquitty::Cell* hovered = Screen::visibleCell(term, viewOffset, row, column);
                m_resolvedLinkID = hovered ? hovered->linkID() : std::uint16_t { 0 };
            }
            const int visibleRowFirst = link.absoluteRow0 - term->scrollbackCount + viewOffset;
            const int visibleRowLast = link.absoluteRow1 - term->scrollbackCount + viewOffset;
            m_resolvedLinkTextDigest = Rendering::linkRunTextDigest(visibleRowFirst, visibleRowLast, link.column0, link.column1,
                link.spanColumnFirst, link.spanColumnLast, term->columns, [term, viewOffset](int visibleRow, int column) {
                    const Ubiquitty::Cell* cell = Screen::visibleCell(term, viewOffset, visibleRow, column);
                    return cell ? cell->codePoint : std::uint32_t { 0 };
                });
        }
        unlockForRead();
        return found;
    }

    /// The digest of the resolved run's text, for the frame to check before it
    /// emphasises the run; zero when the last resolve found nothing.
    [[nodiscard]] std::uint64_t resolvedLinkTextDigest() const { return m_resolvedLinkTextDigest; }

    /// The OSC 8 id the resolved run is drawn by, or zero when the last resolve
    /// found nothing or found a run the text scan read out of plain characters.
    ///
    /// A run reaches past the cells it covers: an `id=` makes two runs separated
    /// by blanks one link, and the blanks between them carry no id. The frame
    /// gates each cell of the run's column band on this, through
    /// ``Rendering/linkHoverContainsCell``, so the emphasis lands on the link's
    /// own cells and not on what sits between them.
    [[nodiscard]] std::uint16_t resolvedLinkID() const { return m_resolvedLinkID; }

    /// Reports a keypress as the Tektronix graphic-input trigger.
    ///
    /// The decision and the projection are `tekReportGraphicInputKey`'s, shared
    /// with every frontend; what this adds is the lock the model is read
    /// under and the flush the report has to be shipped by.
    ///
    /// Flushed rather than queued, because the host that asked for GIN is
    /// blocked reading for the report and emits nothing that would wake a
    /// frontend's read source.
    ///
    /// - Parameters:
    ///   - key: The typed byte, seven bits.
    ///   - pointX: The pointer's x in the view, in the view's own units.
    ///   - pointY: Its y, measured from the TOP of the view.
    ///   - viewWidth: The view's width in the same units.
    ///   - viewHeight: Its height.
    /// - Returns: Whether the press was the trigger and a report was sent. A
    ///   false answer leaves the key to whatever would have handled it.
    bool reportTekGraphicInput(char key, double pointX, double pointY, double viewWidth, double viewHeight)
    {
        lockForRead();
        bool didReport = false;
        if (Ubiquitty::Terminal* term = modelTerm()) {
            didReport = Ubiquitty::tekReportGraphicInputKey(*term, key, static_cast<float>(pointX),
                static_cast<float>(pointY), static_cast<float>(viewWidth), static_cast<float>(viewHeight));
        }
        unlockForRead();
        if (didReport)
            flushReplies();
        return didReport;
    }

    /// Steps the viewport to the neighbouring OSC 133 prompt.
    ///
    /// The step itself is ``Screen::stepToPrompt``, which is pure and shared;
    /// what a frontend cannot do for itself is read the marks, because the
    /// scan walks parser-owned rows and must be atomic against the ingest
    /// writer. Held here, both shells hand over a direction and an anchor and
    /// receive the move to apply — neither takes the lock, and neither wires a
    /// mark provider of its own.
    ///
    /// - Parameters:
    ///   - viewOffset: The viewport's current offset into history.
    ///   - anchorRow: The prompt the last step landed on; -1 to re-derive.
    ///   - direction: Negative to step back, positive to step forward.
    /// - Returns: The move, the anchor to keep, and what to do with them.
    [[nodiscard]] Screen::PromptStepResult stepToPrompt(int viewOffset, int anchorRow, int direction)
    {
        lockForRead();
        Ubiquitty::Terminal* term = modelTerm();
        const Screen::PromptStepResult step = term
            ? Screen::stepToPrompt(Screen::scrollbackCount(term), term->rows, viewOffset, anchorRow, direction, [](void* context, int absoluteRow) { return Screen::rowMark(static_cast<const Ubiquitty::Terminal*>(context), absoluteRow); }, term, Screen::osc133MarkCmdExecuted)
            : Screen::PromptStepResult { viewOffset, anchorRow, Screen::PromptStepAction::Bell };
        unlockForRead();
        return step;
    }

    /// How many link scans this session has run at the widening scope.
    ///
    /// The cheap scope is a cost decision and nothing else — the reveal policy
    /// rejects a guess a bare hover was not allowed to show, whatever the scan
    /// found — so the only observable difference between the two is the work
    /// done. This counts it, so a caller can say that an ordinary pointer move
    /// over prose does not sweep the row.
    [[nodiscard]] std::uint64_t wideLinkScanCount() const { return m_wideLinkScanCount; }

    /// The URI the last resolve found, empty when it found none.
    [[nodiscard]] std::string resolvedLinkURI() const { return std::string(m_linkResolver.uri()); }

    /// Whether the program declared that link with OSC 8, rather than the text
    /// merely reading like one. A declaration is trusted further than a guess:
    /// it opens on a plain click where a guess needs a modifier.
    [[nodiscard]] bool resolvedLinkIsHyperlink() const { return m_linkResolver.link().isHyperlink; }

    /// The hovered run's first and last rows, in ABSOLUTE buffer rows.
    ///
    /// Absolute rather than visible because a run recorded in visible rows is a
    /// run frozen at the scroll position it was seen from: scroll, and the
    /// underline stays behind on whatever text moved into those rows.
    [[nodiscard]] int resolvedLinkRowFirst() const { return m_linkResolver.link().absoluteRow0; }
    [[nodiscard]] int resolvedLinkRowLast() const { return m_linkResolver.link().absoluteRow1; }

    /// The run's first and last columns.
    [[nodiscard]] int resolvedLinkColumnFirst() const { return m_linkResolver.link().column0; }
    [[nodiscard]] int resolvedLinkColumnLast() const { return m_linkResolver.link().column1; }

    /// The columns the run was resolved inside — the pane's edges, not the
    /// screen's. The middle rows of a wrapped run cover their pane edge to
    /// edge, and an underline drawn to the screen's width strokes straight
    /// across the neighbour.
    [[nodiscard]] int resolvedLinkSpanColumnFirst() const { return m_linkResolver.link().spanColumnFirst; }
    [[nodiscard]] int resolvedLinkSpanColumnLast() const { return m_linkResolver.link().spanColumnLast; }

    /// Whether the candidate is a path rather than a URL. Text that looks like
    /// a path and nothing more — a caller resolves it against a working
    /// directory and confirms it exists before treating it as a link.
    [[nodiscard]] bool resolvedLinkIsFilePath() const { return m_linkResolver.link().isFilePath; }

    /// How a key chord resolved against the user's [keys] bindings.
    enum class BindingResolution : std::uint8_t {
        /// Unbound: the key belongs to whatever would have had it.
        NoMatch UBIQUITTY_SWIFT_NAME(noMatch) = 0,
        /// A leader or chord prefix is capturing. The key is spent.
        Pending UBIQUITTY_SWIFT_NAME(pending) = 1,
        /// A binding fired; read it with the accessors below.
        Matched UBIQUITTY_SWIFT_NAME(matched) = 2,
    };

    /// Rebuilds the binding table from the live [keys] config.
    ///
    /// Any chord in progress is dropped, because the table it was matching
    /// against may not exist any more — a half-typed leader resolving against
    /// a table the user just edited is a keystroke going somewhere they did
    /// not write.
    void rebuildKeyBindings()
    {
        m_bindingTable.clear();
        for (const Preferences::KeyBindingEntry& binding : m_configuration.keyBindings)
            m_bindingTable.add(binding.spec, binding.action);
        m_chordResolver = std::make_unique<Ubiquitty::Input::ChordResolver>(m_bindingTable);
        // Compose rides the same rebuild: `compose_key` lives in the same
        // `[keys]` block, so one edit changes both and a frontend that asked
        // for them separately would run half the reader's new configuration.
        m_compose.rebuild(m_configuration.composeKey.value_or(std::string()));
    }

    /// Whether a `[keys] compose_key` is configured.
    ///
    /// Asked before the chord is built, because building one costs a layout
    /// fold the common configuration — no compose key — should not pay.
    [[nodiscard]] bool isComposeArmed() const { return m_compose.isArmed(); }

    /// Offers a key press to the compose path.
    ///
    /// Runs after the `[keys]` bindings, so a binding on the same key wins,
    /// and before the input method and the keymap.
    ///
    /// - Parameters:
    ///   - key: The chord's key, as ``Input::resolveChordKey`` spells it.
    ///   - modifiers: The chord's modifiers, already masked to `bindModMask`.
    ///   - typedCodePoint: The character the key produced, zero when it
    ///     produced none.
    /// - Returns: What the frontend should do with the key.
    Ubiquitty::Input::ComposeAction offerKeyToCompose(std::uint32_t key, std::uint32_t modifiers, std::uint32_t typedCodePoint)
    {
        return m_compose.offer(Ubiquitty::Input::KeyChord { key, modifiers }, typedCodePoint);
    }

    /// The composed code point after ``Input::ComposeAction::Write``.
    [[nodiscard]] std::uint32_t composedCharacter() const { return m_compose.composed(); }

    /// Offers a key chord to the bindings.
    ///
    /// The resolver owns every question about leaders, prefixes and
    /// ambiguity; a frontend supplies the chord and obeys the verdict.
    ///
    /// - Parameters:
    ///   - key: The chord's key, as ``Input::resolveChordKey`` spells it.
    ///   - modifiers: The chord's modifiers, already masked to `bindModMask`.
    /// - Returns: What the resolver made of it.
    BindingResolution feedKeyChord(std::uint32_t key, std::uint32_t modifiers)
    {
        m_resolvedBinding = Ubiquitty::Input::BindingAction {};
        if (!m_chordResolver)
            return BindingResolution::NoMatch;
        const Ubiquitty::Input::KeyChord chord { key, modifiers };
        switch (m_chordResolver->feed(chord, m_resolvedBinding)) {
        case Ubiquitty::Input::ResolveResult::Matched:
            return BindingResolution::Matched;
        case Ubiquitty::Input::ResolveResult::Pending:
            return BindingResolution::Pending;
        case Ubiquitty::Input::ResolveResult::NoMatch:
            break;
        }
        return BindingResolution::NoMatch;
    }

    /// Offers a key chord to the built-in defaults, the user's table having
    /// declined it.
    ///
    /// Separate from ``feedKeyChord`` because a frontend asks the two at
    /// different moments: the user's table on the key-down route, the defaults
    /// on the key-equivalent pass, where a summon has to be honoured while an
    /// overlay owns the responder chain. Whether a default still stands is
    /// ``Input::resolveDefaultBinding``'s decision, not a frontend's.
    ///
    /// - Parameters:
    ///   - key: The chord's key, as ``Input::resolveChordKey`` spells it.
    ///   - modifiers: The chord's modifiers, already masked to `bindModMask`.
    /// - Returns: Whether a default claimed it; read the action with
    ///   ``resolvedBindingKind``.
    bool feedDefaultKeyChord(std::uint32_t key, std::uint32_t modifiers)
    {
        m_resolvedBinding = Ubiquitty::Input::BindingAction {};
        return Ubiquitty::Input::resolveDefaultBinding(m_bindingTable, Ubiquitty::Input::KeyChord { key, modifiers }, m_resolvedBinding);
    }

    /// The kind of the binding the last chord fed matched, through either
    /// ``feedKeyChord`` or ``feedDefaultKeyChord``.
    [[nodiscard]] std::uint8_t resolvedBindingKind() const { return static_cast<std::uint8_t>(m_resolvedBinding.kind); }

    /// A `SendBytes` binding's payload, escapes already resolved.
    [[nodiscard]] std::string resolvedBindingBytes() const { return m_resolvedBinding.bytes; }

    /// A named command's identifier, or a macro's name.
    [[nodiscard]] std::string resolvedBindingName() const { return m_resolvedBinding.name; }

    /// Performs a local soft reset — the manual DECSTR.
    ///
    /// The escape hatch for a session a dead peer left wedged: mouse tracking
    /// on, application keys on, and nothing alive to send the resets. The
    /// reset a host-initiated `CSI ! p` performs, plus the mouse, paste and
    /// focus modes DECSTR leaves, republished so a frontend's cached mode
    /// mirrors retire now rather than one drain later.
    ///
    /// A session attached to a host is left as it is: the program's terminal
    /// is the host's, which encodes the pane's input under its own modes, and
    /// only the authority mutates a mirrored grid.
    void softResetTerminal()
    {
        const std::scoped_lock hold(*m_activeLock);
        if (m_remote)
            return;
        if (Ubiquitty::Terminal* term = modelTerm()) {
            Ubiquitty::softReset(term);
            republishModel();
        }
    }

    /// Selects the last command's output, by the shell's OSC 133 marks.
    ///
    /// Every part of finding the region is Core's — this supplies the model
    /// lock, the mark provider that reads it, and the selection the span is
    /// materialised into.
    ///
    /// - Returns: Whether a last-command-output region was found.
    bool hasLastCommandOutput() { return findLastCommandOutput(nullptr); }

    /// Selects the last command's output.
    ///
    /// - Parameter selection: The view's selection, written on success.
    /// - Returns: Whether a last-command-output region was found.
    ///
    /// Taken by reference, not pointer: a Swift-facing parameter that is a
    /// pointer to a C++ type imports as `UnsafeMutablePointer`, and an
    /// OPTIONAL one drops the whole member from the module unless the
    /// importing file already named the type.
    bool selectLastCommandOutput(Ubiquitty::SelectionState& selection) { return findLastCommandOutput(&selection); }

    /// What the pointer is over, for the document-style cursor.
    enum class TextVoidAnswer : std::uint8_t {
        /// The cell holds text, or the void rule does not apply here.
        Text UBIQUITTY_SWIFT_NAME(text),
        /// The cell is past the end of its row's text.
        Empty UBIQUITTY_SWIFT_NAME(empty),
        /// The model was busy. The caller keeps the answer it has.
        Unknown UBIQUITTY_SWIFT_NAME(unknown),
    };

    /// Whether the pointer sits where there is nothing to select.
    ///
    /// Tried rather than waited for: this runs per mouse move and per drain,
    /// where a blocking acquire has no fairness doorway against ingest and a
    /// flood would starve the pointer into a slideshow. A contended read
    /// answers `Unknown`, and the next uncontended one refreshes it.
    ///
    /// - Parameters:
    ///   - visibleRow: The pointer's row, zero-based in the viewport.
    ///   - column: The pointer's column.
    ///   - viewOffset: Rows the viewport is scrolled back.
    /// - Returns: What is under the pointer, or that it could not be read.
    TextVoidAnswer pointerTextVoid(int visibleRow, int column, int viewOffset)
    {
        if (!tryLockForRead())
            return TextVoidAnswer::Unknown;
        const Ubiquitty::Terminal* term = modelTerm();
        const bool isVoid = term != nullptr
            && Ubiquitty::Selection::isCellInTextVoid(*term, viewOffset, visibleRow, column,
                Preferences::Effective::restrictSelectionToText(m_configuration) ? Ubiquitty::Selection::TextRestriction::ToText
                                                                                 : Ubiquitty::Selection::TextRestriction::Unrestricted);
        unlockForRead();
        return isVoid ? TextVoidAnswer::Empty : TextVoidAnswer::Text;
    }

    /// Whether an executed command sits at or above one row.
    ///
    /// - Parameter absoluteRow: The row, in absolute buffer coordinates.
    /// - Returns: Whether a command was found.
    bool hasCommandUnderRow(int absoluteRow)
    {
        const std::scoped_lock hold(*m_activeLock);
        const Ubiquitty::Terminal* term = modelTerm();
        return term != nullptr && Ubiquitty::Selection::hasCommandUnderRow(*term, absoluteRow);
    }

    /// Selects the command line of the command under one row.
    ///
    /// - Parameters:
    ///   - absoluteRow: The row, in absolute buffer coordinates.
    ///   - selection: The view's selection, written on success.
    /// - Returns: Whether a command was found.
    bool selectCommandLineUnderRow(int absoluteRow, Ubiquitty::SelectionState& selection)
    {
        const std::scoped_lock hold(*m_activeLock);
        const Ubiquitty::Terminal* term = modelTerm();
        return term != nullptr && Ubiquitty::Selection::selectCommandLineUnderRow(selection, *term, absoluteRow);
    }

    /// Selects the output of the command under one row.
    ///
    /// - Parameters:
    ///   - absoluteRow: The row, in absolute buffer coordinates.
    ///   - selection: The view's selection, written on success.
    /// - Returns: Whether a command with output was found.
    bool selectCommandOutputUnderRow(int absoluteRow, Ubiquitty::SelectionState& selection)
    {
        const std::scoped_lock hold(*m_activeLock);
        const Ubiquitty::Terminal* term = modelTerm();
        return term != nullptr && Ubiquitty::Selection::selectCommandOutputUnderRow(selection, *term, absoluteRow);
    }

    /// The absolute row a visible row reads from, at one view offset.
    ///
    /// A frontend holding a click knows its visible row and its offset; the
    /// history behind them is the model's, and the mapping is Core's.
    ///
    /// - Parameters:
    ///   - visibleRow: A zero-based row of the visible grid.
    ///   - viewOffset: The offset the view is holding.
    /// - Returns: The absolute row, or -1 with no model to read.
    int absoluteRowForVisibleRow(int visibleRow, int viewOffset)
    {
        const std::scoped_lock hold(*m_activeLock);
        const Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return -1;
        return Ubiquitty::Screen::absoluteRowForVisible(
            Ubiquitty::Screen::scrollableRowCount(term), Ubiquitty::Screen::resolvedViewOffset(term, viewOffset), visibleRow);
    }

private:
    bool findLastCommandOutput(Ubiquitty::SelectionState* selection)
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term || !term->screens.active)
            return false;
        const int history = Ubiquitty::Screen::scrollableRowCount(term);
        const int totalRows = history + term->rows;
        const int ceiling = history + term->screens.active->cursorY;
        Ubiquitty::SelectionBounds region;
        const auto markAt = [](void* context, int absoluteRow) -> std::uint8_t {
            return Ubiquitty::Screen::rowMark(static_cast<const Ubiquitty::Terminal*>(context), absoluteRow);
        };
        if (!Ubiquitty::Selection::lastCommandOutputRegion(markAt, term, totalRows, term->columns, ceiling, &region))
            return false;
        if (selection)
            Ubiquitty::Selection::selectRegion(selection, totalRows, term->columns, region);
        return true;
    }

public:
    /// Hands the focused search match over as a real text selection.
    ///
    /// What closing a find bar owes the user: the match they were looking at
    /// stays highlighted and copyable after the transient find highlights go,
    /// rather than the screen forgetting where they had got to. A match
    /// wrapped over rows is handed over whole, as
    /// `Selection::selectActiveSearchMatch` selects it.
    ///
    /// - Parameter selection: The view's selection to write.
    /// - Returns: Whether a match was handed over.
    bool handOffActiveSearchMatch(Ubiquitty::SelectionState& selection)
    {
        const std::scoped_lock hold(*m_activeLock);
        if (m_search.controller && m_search.controller->status().generation != m_search.controller->requestedGeneration())
            return false;
        const Ubiquitty::Terminal* term = modelTerm();
        return term && Ubiquitty::Selection::selectActiveSearchMatch(selection, *term);
    }

    /// Records a user-initiated input event for the cursor's activity freeze.
    ///
    /// The caret holds solid for a moment after each keystroke instead of
    /// blinking through the text being typed — Core states the rule and every
    /// frontend's keyboard and pointer handlers are what feed it.
    ///
    /// The tick is this session's own, so a harness pinning `UBIQUITTY_FAKE_TICKS`
    /// moves the activity and the blink curve together and the freeze becomes
    /// something a test can step through rather than wait out.
    void noteUserActivity()
    {
        const std::scoped_lock hold(*m_activeLock);
        const auto now = frameTicksNow();
        Ubiquitty::Cursor::noteUserActivity(modelTerm(), now);
        if (auto* term = modelTerm(); term && !term->mirrorClient) {
            if (Modes::DisplayIdle::noteInput(*term, Ticks(now))) {
                if (m_externalTerminal)
                    republishModel();
                else
                    m_runtime->publishDisplayChange(m_host);
                m_events.pending.fetch_or(1u << static_cast<unsigned>(EventKind::ScreenDirty), std::memory_order_release);
            }
        }
    }

    /// Resolves the frame-clock facts against the model, under the lock.
    ///
    /// One tick serves the cursor resolve and the builder's blink curves, so
    /// the caret and SGR 5/6 text stay in the phase the curve defines.
    ///
    /// Taken through the announced acquire, like the render capture: this
    /// runs on every link tick, and a plain acquire against the lock the
    /// ingest thread re-takes per bite parks for a whole bite each time —
    /// under a flood, more than half of the render thread's time, all of it
    /// in one stack. Announced, the drain's unlock donates the lock and the
    /// wait is microseconds.
    ///
    /// - Returns: The tick and the cursor's blink weight at it; the solid
    ///   defaults when no model exists.
    [[nodiscard]] FrameClockFacts frameClockFactsNow()
    {
        FrameClockFacts facts;
        facts.blinkTicks = frameTicksNow();
        lockForRead();
        if (Ubiquitty::Terminal* term = modelTerm())
            facts.cursorMixAlpha = Ubiquitty::Cursor::resolveAppearance(term, facts.blinkTicks).blinkAlpha;
        unlockForRead();
        return facts;
    }

    /// Records the rows of history the surface presents above the tail.
    ///
    /// Written on the main thread at scroll edges, read relaxed by the
    /// animation schedule: while the offset is positive, the schedule keeps
    /// the blink clock armed on the sticky hint alone, since the live-screen
    /// reading cannot see the rows on display. A remote pane also pulls the
    /// history a view this deep reaches past what its client holds; see
    /// ``Session::PaneLink::setViewOffset``.
    ///
    /// - Parameter rows: Rows above the tail; negative clamps to zero.
    void setPresentedViewOffset(int rows)
    {
        m_presentedViewOffset.store(rows < 0 ? 0 : rows, std::memory_order_relaxed);
        paneLink().setViewOffset(rows < 0 ? 0 : rows);
    }

    /// One pointer fact, as a frontend's event loop saw it — coordinates and
    /// modifiers, no decisions. The ladder's own type; the name here is what
    /// the Swift arm spells.
    using PointerFact = Ubiquitty::Input::PointerFact;

    /// One scroll gesture, as a frontend observes it. The ladder's own
    /// type; the name here is what the Swift arm spells.
    using ScrollFact = Ubiquitty::Input::ScrollFact;

    /// What a frontend still does after the scroll ladder ran.
    struct ScrollOutcome {
        /// The viewport owns the event, even before a whole row accumulates.
        bool isViewportScroll = false;
        /// Rows to move the local viewport by, positive toward older history.
        /// Zero on every other route — the view holds still while a program
        /// or the alternate-scroll fold consumes the gesture.
        std::ptrdiff_t linesToOffsetView = 0;
        /// The gesture produced bytes or a view movement.
        bool wasConsumed = false;
    };

    /// What a frontend still does natively after the pipeline ran.
    struct PointerOutcome {
        /// The selection or a highlight changed; repaint.
        bool needsDisplay = false;
        /// The selection changed; tell the observer.
        bool didChangeSelection = false;
        /// The event was consumed — reported, gestured, or swallowed. False
        /// leaves it to the frontend's native fallback (a link click, a
        /// window drag).
        bool wasConsumed = false;
        /// A press opened a local selection gesture; the frontend routes the
        /// drag and the release to it until the gesture ends.
        bool didOpenSelectionGesture = false;
        /// A reserved gesture worth teaching, raised at the moment it would
        /// have helped. The frontend surfaces ``Input::hintText`` for the
        /// lifetime ``Input::isHintPersistent`` states, natively where it has
        /// chrome and through ``Rendering::layoutHintToast`` where it has not.
        Ubiquitty::Input::Hint hint = Ubiquitty::Input::Hint::None;
    };

    /// The encoder's event kind for one fact's kind.
    ///
    /// - Parameter fact: The fact whose kind converts.
    /// - Returns: The mouse encoder's event type.
    [[nodiscard]] static Ubiquitty::Mouse::EventType kindToEventType(const PointerFact& fact)
    {
        switch (fact.kind) {
        case PointerFact::Kind::Press:
            return Ubiquitty::Mouse::EventType::Press;
        case PointerFact::Kind::Release:
            return Ubiquitty::Mouse::EventType::Release;
        case PointerFact::Kind::Motion:
            return Ubiquitty::Mouse::EventType::Motion;
        }
        return Ubiquitty::Mouse::EventType::Motion;
    }

    /// Runs one pointer fact through the ladder every frontend must keep.
    ///
    /// The ladder is `Input::routePointer`: the DEC locator, Tek GIN, the
    /// mode-1001 gesture, xterm reporting, and only then the local selection.
    /// This adds what a session holds -- the lock, the route the bytes take,
    /// the republish, and the copy-on-select the preferences ask for.
    ///
    /// - Parameters:
    ///   - fact: The event's facts.
    ///   - selection: The caller-owned selection session.
    /// - Returns: What remains for the frontend.
    PointerOutcome pointerFact(const PointerFact& fact, Ubiquitty::Selection::Session* selection)
    {
        PointerOutcome outcome;
        Ubiquitty::Input::PointerRoute route;
        {
            const AnnouncedModelLock hold(*this); // per motion event from the frontend's thread
            Ubiquitty::Terminal* term = modelTerm();
            if (!term)
                return outcome;
            auto policy = pointerPolicy();
            if (term->modes.shouldDeleteReadlineSelection && fact.button == Input::PointerButton::Right)
                policy.readlineEraseCharacter = paneLink().lineEraseCharacter();
            route = Ubiquitty::Input::routePointer(m_pointerLadder, *term, selection, fact, policy, frameTicksNow());
            if (route.needsRepublish)
                republishModel();
        }
        if (route.locatorReportLength > 0)
            sendInput(ByteSpan { route.locatorReport.data(), static_cast<std::size_t>(route.locatorReportLength) });
        if (route.reportLength > 0) {
            if (route.reported)
                (void)paneLink().send(Session::PaneInput { .intent = Session::PaneInput::Pointer { Runtime::pointerEventOf(*route.reported) },
                    .localEncoding = std::span(route.report).first(static_cast<std::size_t>(route.reportLength)) });
            else
                sendInput(ByteSpan { route.report.data(), static_cast<std::size_t>(route.reportLength) });
        }
        if (!route.readlineInput.empty())
            sendInput(ByteSpan { route.readlineInput.data(), route.readlineInput.size() });
        if (route.didFinishSelectionGesture)
            applyCopyOnSelect(selection);
        outcome.needsDisplay = route.needsDisplay;
        outcome.hint = route.hint;
        outcome.didChangeSelection = route.didChangeSelection;
        outcome.wasConsumed = route.wasConsumed;
        outcome.didOpenSelectionGesture = route.didOpenSelectionGesture;
        return outcome;
    }

    /// Tells the DEC locator where a stationary pointer is.
    ///
    /// The locator's arm edge, which only a frontend can see: see
    /// ``Input::seedLocator``. A report that comes back is sent, like one
    /// from ``pointerFact``.
    ///
    /// - Parameter fact: Where the pointer is; its kind and button are not read.
    void seedLocator(const PointerFact& fact)
    {
        Ubiquitty::Input::PointerRoute route;
        {
            const std::scoped_lock hold(*m_activeLock);
            Ubiquitty::Terminal* term = modelTerm();
            if (!term)
                return;
            route = Ubiquitty::Input::seedLocator(*term, fact);
        }
        if (route.locatorReportLength > 0)
            sendInput(ByteSpan { route.locatorReport.data(), static_cast<std::size_t>(route.locatorReportLength) });
    }

    /// The preferences the pointer ladder reads, resolved from this session's
    /// configuration.
    ///
    /// - Returns: The policy for one event.
    [[nodiscard]] Ubiquitty::Input::PointerPolicy pointerPolicy() const
    {
        return Ubiquitty::Input::pointerPolicyFromPreferences(m_configuration);
    }

    /// Reports the frontend's pointer grab, so the ladder can keep the lesson
    /// that names the way out for as long as the trap lasts.
    ///
    /// - Parameter grab: Whether the grab is held.
    void notePointerLockGrab(Ubiquitty::Input::PointerLockGrab grab)
    {
        const AnnouncedModelLock hold(*this); // per event or delivery from the frontend's thread
        Ubiquitty::Input::notePointerLockGrab(m_pointerLadder, grab);
    }

    /// Decides whether the frontend should hold the pointer grab.
    ///
    /// The mode is the application's ask, read here under the lock; the focus
    /// is the surface's standing to answer it, which only the frontend sees.
    /// Idempotent: a shell re-asks on every edge that can move either half
    /// and reconciles its OS grab against the answer.
    ///
    /// - Parameter facts: What the surface sees.
    /// - Returns: Whether to hold the grab.
    [[nodiscard]] Ubiquitty::Input::PointerLockVerdict pointerLockVerdict(const Ubiquitty::Input::PointerLockFacts& facts)
    {
        const AnnouncedModelLock hold(*this); // per event or delivery from the frontend's thread
        const Ubiquitty::Terminal* term = modelTerm();
        const auto mode = term && term->modes.mouseRelativePointerLock ? Ubiquitty::Input::PointerLockMode::Set : Ubiquitty::Input::PointerLockMode::Clear;
        return Ubiquitty::Input::resolvePointerLock(mode, facts);
    }

    /// Records the modifier state the frontend saw, for the break-out chord.
    ///
    /// - Parameter keys: The modifiers and whether any other key is down.
    void notePointerLockChord(const Ubiquitty::Input::PointerLockKeyState& keys)
    {
        const AnnouncedModelLock hold(*this); // per event or delivery from the frontend's thread
        Ubiquitty::Input::notePointerLockChord(m_pointerLadder, keys, frameTicksNow());
    }

    /// Decides whether the armed chord has been held for the threshold, and
    /// on a break-out clears the mode itself so a hung application that never
    /// resets 7403 cannot re-trap the pointer.
    ///
    /// - Parameter keysNow: The modifiers as they are now.
    /// - Returns: Whether the frontend releases its grab.
    [[nodiscard]] Ubiquitty::Input::PointerLockChordVerdict tickPointerLockChord(const Ubiquitty::Input::PointerLockKeyState& keysNow)
    {
        Ubiquitty::Input::PointerLockChordVerdict verdict = Ubiquitty::Input::PointerLockChordVerdict::Keep;
        {
            const AnnouncedModelLock hold(*this); // per event or delivery from the frontend's thread
            verdict = Ubiquitty::Input::tickPointerLockChord(m_pointerLadder, keysNow, frameTicksNow());
        }
        if (verdict == Ubiquitty::Input::PointerLockChordVerdict::BreakOut)
            breakOutOfPointerLock();
        return verdict;
    }

    /// How long until the armed chord's hold elapses, or 0 when it is not
    /// armed: the one wake-up a shell books for the chord.
    ///
    /// - Returns: Ticks until the deadline, or 0.
    [[nodiscard]] std::uint32_t pointerLockChordTicksUntilDeadline() const
    {
        const std::scoped_lock hold(*m_activeLock);
        return Ubiquitty::Input::pointerLockChordTicksUntilDeadline(m_pointerLadder, frameTicksNow());
    }

    /// The hint a frontend should be showing now.
    ///
    /// Core's whole answer -- which lesson, if any, still stands -- on the
    /// same clock ``pointerFact`` raised it on; ``Input::hintText`` has the
    /// words. A shell asks after any pointer fact, after any grab report, on
    /// any screen update, and once after ``hintTicksUntilExpiry``, and
    /// otherwise only draws what it was told.
    ///
    /// - Returns: The hint, or ``Input::Hint::None``.
    /// The ladder is the pointer path's own: every call that writes it — a
    /// pointer fact, a grab report, a lock chord, a scroll — arrives on the
    /// thread that asks here, and ingest never touches it. The tick beside it is
    /// a clock reading. So this owes the model lock nothing, which
    /// ``isSelectionGestureOpen`` already reads the ladder on the strength of;
    /// taking it would block this thread against the parser once per screen
    /// update, which is when a shell is told to ask.
    [[nodiscard]] Ubiquitty::Input::Hint hintShowing() const
    {
        return Ubiquitty::Input::hintShowing(m_pointerLadder, frameTicksNow());
    }

    /// How long the standing hint has left, or 0 when it will not withdraw
    /// itself.
    ///
    /// The one wake-up a shell books: it asks ``hintShowing`` again that
    /// many ticks from now.
    ///
    /// - Returns: Ticks until the transient lesson fades, or 0.
    /// Reads the ladder and the clock, so it owes the model lock nothing for the
    /// reason ``hintShowing`` states.
    ///
    /// - Returns: Ticks until the transient lesson fades, or 0.
    [[nodiscard]] std::uint32_t hintTicksUntilExpiry() const
    {
        const std::uint32_t expiry = Ubiquitty::Input::hintExpiryTicks(m_pointerLadder);
        if (expiry == 0)
            return 0;
        const auto left = static_cast<std::int32_t>(expiry - frameTicksNow());
        return left > 0 ? static_cast<std::uint32_t>(left) : 0;
    }

    /// Whether a local selection gesture is open — the routing fact the
    /// pipeline keeps so a frontend needs no bookkeeping of its own.
    ///
    /// - Returns: Whether the next drag extends and the next release ends.
    [[nodiscard]] bool isSelectionGestureOpen() const { return m_pointerLadder.isSelectionGestureOpen; }

    /// How a search pattern is read: verbatim text, or the search grammar.
    enum class SearchPatternForm : std::uint8_t {
        /// Metacharacters match themselves; the pattern is escaped whole.
        Literal UBIQUITTY_SWIFT_NAME(literal),
        /// The pattern is the scrollback search's own regex grammar.
        Regex UBIQUITTY_SWIFT_NAME(regex)
    };

    /// Which way a step moves through the matches.
    enum class SearchStep : std::uint8_t {
        /// Toward older matches, up the scrollback, wrapping past the oldest to the newest.
        Forward UBIQUITTY_SWIFT_NAME(forward),
        /// Toward newer matches, down the scrollback, wrapping past the newest to the oldest.
        Backward UBIQUITTY_SWIFT_NAME(backward)
    };

    /// One search pass's answer, in the terms a find bar shows.
    struct SearchOutcome {
        /// Logical matches found; zero for a cleared or failed search.
        std::ptrdiff_t totalMatches = 0;
        /// The active match, zero-based; -1 when none is.
        std::ptrdiff_t activeIndex = -1;
        /// The view offset that reveals the active match, or -1 to leave
        /// the view where it is.
        std::ptrdiff_t revealViewOffset = -1;
        /// Whether the pattern failed to compile — the bar's warning state.
        bool isPatternInvalid = false;
    };

    /// Starts or replaces the search without waiting for its scan.
    ///
    /// Results arrive through SearchChanged and the published snapshot. An
    /// identical request leaves the running search alone. The frontend calls
    /// without holding the model lock.
    ///
    /// - Parameter request: The pattern and its matching policy; empty clears.
    /// - Returns: Whether a new scan was queued.
    bool setSearchQuery(const Ubiquitty::Search::Request& request)
    {
        const auto controller = searchController();
        if (!controller || !controller->setQuery(request))
            return false;
        Ubiquitty::Search::SearchExecutor::shared().schedule(controller);
        return true;
    }

    /// Starts a search through the scalar frontend seam.
    ///
    /// - Parameters:
    ///   - pattern: The query; empty clears.
    ///   - form: Verbatim text or the search grammar.
    ///   - sensitivity: Whether case distinguishes.
    /// - Returns: Whether a new scan was queued.
    bool setSearchQuery(const std::string& pattern, SearchPatternForm form, Ubiquitty::CaseSensitivity sensitivity)
    {
        return setSearchQuery(searchRequest(pattern, form, sensitivity));
    }

    /// Starts a palette search, naming its row boundaries before enqueueing.
    ///
    /// - Parameters:
    ///   - pattern: The query; empty clears.
    ///   - form: Verbatim text or the search grammar.
    ///   - sensitivity: Whether case distinguishes.
    ///   - firstRow: The first scoped row; negative searches the whole buffer.
    ///   - lastRow: The last scoped row, inclusive.
    ///   - labelLimit: The requested number of result labels, bounded by Core.
    /// - Returns: Whether a new scan was queued.
    bool setSearchQuery(const std::string& pattern, SearchPatternForm form, Ubiquitty::CaseSensitivity sensitivity, int firstRow, int lastRow, std::size_t labelLimit)
    {
        auto request = searchRequest(pattern, form, sensitivity);
        request.labelLimit = labelLimit;
        if (firstRow >= 0) {
            request.scope.emplace();
            const AnnouncedModelLock hold(*this);
            if (const auto* term = modelTerm(); term && firstRow <= lastRow) {
                const auto first = Ubiquitty::Screen::capturePosition(*term, firstRow, 0);
                const auto last = Ubiquitty::Screen::capturePosition(*term, lastRow, 0);
                if (first && last)
                    request.scope = Ubiquitty::Search::Scope { *first, *last };
            }
        }
        return setSearchQuery(request);
    }

    /// Reads labels belonging to the current request under the model lock.
    ///
    /// - Returns: Bounded text and durable positions; empty while a replacement is pending.
    [[nodiscard]] std::vector<Ubiquitty::Search::Label> searchLabels()
    {
        const AnnouncedModelLock hold(*this);
        const auto& controller = m_search.controller;
        if (!controller || controller->status().generation != controller->requestedGeneration())
            return {};
        return controller->labels();
    }

    /// Resolves a palette result when its action is invoked.
    ///
    /// - Parameter position: The position captured with the result.
    /// - Returns: The view offset revealing its text, or -1 when it is gone.
    [[nodiscard]] int searchPositionViewOffset(const Ubiquitty::Screen::LogicalPosition& position)
    {
        const AnnouncedModelLock hold(*this);
        const auto* term = modelTerm();
        if (!term)
            return -1;
        const auto place = Ubiquitty::Screen::resolvePosition(*term, position);
        if (!place)
            return -1;
        return Ubiquitty::Screen::viewOffsetToReveal(Ubiquitty::Screen::scrollbackCount(term), term->rows, place->absoluteRow, Ubiquitty::Screen::RevealPlacement::Match);
    }

    /// Reads the search's last publication without taking the model lock.
    ///
    /// - Returns: The counts, progress, and reveal owed by that publication.
    [[nodiscard]] Ubiquitty::Search::Status searchStatus()
    {
        const auto status = readSnapshot().search;
        const auto generation = m_search.controller ? m_search.controller->requestedGeneration() : status.generation;
        if (generation != status.generation)
            return { .generation = generation };
        return status;
    }

    /// Searches in slices on the calling thread for tests and automation.
    ///
    /// - Parameters:
    ///   - request: The pattern and matching policy.
    ///   - timeoutMilliseconds: The wait bound; an incomplete answer names a timeout.
    /// - Returns: The latest published search status.
    Ubiquitty::Search::Status searchScrollbackAndWait(const Ubiquitty::Search::Request& request, std::uint32_t timeoutMilliseconds = 30'000)
    {
        const auto controller = searchController();
        if (!controller)
            return { .isComplete = true };
        (void)controller->searchAndWait(request, std::chrono::milliseconds(timeoutMilliseconds));
        return searchStatus();
    }

    /// Searches through the scalar automation seam, with a bounded wait.
    ///
    /// - Parameters:
    ///   - pattern: The query; empty clears.
    ///   - form: Verbatim text or the search grammar.
    ///   - sensitivity: Whether case distinguishes.
    ///   - timeoutMilliseconds: The wait bound.
    /// - Returns: The latest published search status.
    Ubiquitty::Search::Status searchScrollbackAndWait(const std::string& pattern, SearchPatternForm form, Ubiquitty::CaseSensitivity sensitivity, std::uint32_t timeoutMilliseconds = 30'000)
    {
        return searchScrollbackAndWait(searchRequest(pattern, form, sensitivity), timeoutMilliseconds);
    }

    /// Builds the request the frontend's scalar arguments name.
    ///
    /// - Parameters:
    ///   - pattern: The query.
    ///   - form: How to read it.
    ///   - sensitivity: Whether case distinguishes.
    /// - Returns: The request with the session's normalization policy.
    [[nodiscard]] static Ubiquitty::Search::Request searchRequest(const std::string& pattern, SearchPatternForm form, Ubiquitty::CaseSensitivity sensitivity)
    {
        return {
            .pattern = pattern,
            .form = form == SearchPatternForm::Regex ? Ubiquitty::Search::PatternForm::Regex : Ubiquitty::Search::PatternForm::Literal,
            .sensitivity = sensitivity,
            .normalization = Ubiquitty::Screen::ScrollSearch::normalizationForm(),
        };
    }

    /// Steps the active match and answers the same facts a fresh search does.
    ///
    /// - Parameter step: Which way to move; both ends wrap.
    /// - Returns: The counts, the new active index and the reveal offset.
    SearchOutcome stepSearchMatch(SearchStep step)
    {
        {
            const std::scoped_lock hold(*m_activeLock);
            Ubiquitty::Terminal* term = modelTerm();
            if (!term || !m_search.controller || term != m_search.terminal)
                return {};
            if (m_search.controller->status().generation != m_search.controller->requestedGeneration())
                return searchOutcome(searchStatus());
            m_search.controller->step(step == SearchStep::Forward ? Ubiquitty::Search::StepDirection::Older : Ubiquitty::Search::StepDirection::Newer);
            m_runtime->setSearchStatus(m_search.controller->status());
            republishModel();
        }
        noteRemoteEvent(EventKind::SearchChanged);
        return searchOutcome(searchStatus());
    }

    /// Drops the search — the bar closed.
    void clearScrollbackSearch()
    {
        if (m_search.controller) {
            (void)m_search.controller->searchAndWait({}, std::chrono::milliseconds::zero());
            return;
        }
        {
            const std::scoped_lock hold(*m_activeLock);
            if (auto* term = modelTerm())
                Ubiquitty::Screen::ScrollSearch::clear(term);
            m_runtime->setSearchStatus({ .isComplete = true });
            republishModel();
        }
        noteRemoteEvent(EventKind::SearchChanged);
    }

    /// Projects a search publication onto the scalar navigation outcome.
    ///
    /// - Parameter status: The current publication.
    /// - Returns: The count, focus, and reveal.
    [[nodiscard]] static SearchOutcome searchOutcome(const Ubiquitty::Search::Status& status)
    {
        return { .totalMatches = static_cast<std::ptrdiff_t>(status.foundCount),
            .activeIndex = static_cast<std::ptrdiff_t>(status.activeIndex),
            .revealViewOffset = status.revealViewOffset,
            .isPatternInvalid = status.isPatternInvalid };
    }

    /// Publishes a worker's results before waking the frontend. The worker
    /// holds its job mutex, so detach waits for this callback to return.
    void searchDidChange()
    {
        {
            const std::scoped_lock hold(*m_search.lock);
            if (m_search.controller->status().generation != m_search.controller->requestedGeneration())
                return;
            m_runtime->setSearchStatus(m_search.controller->status());
            republishModel();
        }
        noteRemoteEvent(EventKind::SearchChanged);
    }

    /// Returns the search of the model's terminal, made on the first search
    /// and again when the session presents another terminal. The caller does
    /// not hold the model lock: replacing a search detaches it, which waits
    /// out a slice that may be waiting on the lock.
    ///
    /// - Returns: The search, or null without a terminal.
    [[nodiscard]] std::shared_ptr<Ubiquitty::Search::SearchController> searchController()
    {
        if (m_search.controller)
            return m_search.controller;
        std::mutex* lock = m_activeLock;
        Ubiquitty::Terminal* term = nullptr;
        {
            const std::scoped_lock hold(*lock);
            term = modelTerm();
        }
        if (!term)
            return nullptr;
        m_search.controller = Ubiquitty::Search::SearchController::make(*term, *lock);
        m_search.terminal = term;
        m_search.lock = lock;
        if (m_search.controller)
            m_search.controller->setChangedCallback([](void* context) { static_cast<TerminalSession*>(context)->searchDidChange(); }, this);
        {
            const std::scoped_lock hold(*lock);
            if (m_search.controller)
                m_runtime->setModelPublishHook([](void* context, const Ubiquitty::Terminal& terminal) {
                    static_cast<Ubiquitty::Search::SearchController*>(context)->noteModelChanged(terminal);
                },
                    m_search.controller.get());
        }
        Ubiquitty::Search::SearchExecutor::shared().schedule(m_search.controller);
        return m_search.controller;
    }

    /// Removes the model observer before waiting for the search worker.
    /// The caller holds no model lock.
    void detachSearch()
    {
        if (!m_search.controller)
            return;
        {
            const std::scoped_lock hold(*m_search.lock);
            m_runtime->setModelPublishHook(nullptr, nullptr);
        }
        m_search.controller->detach();
        m_search = {};
    }

    /// Applies the copy-on-select policy to a finished gesture.
    ///
    /// The two inputs are both this side of the seam — the user's config
    /// preference and DEC mode 1044, the application asking that the
    /// selection double as the clipboard — and the write goes out through
    /// the platform's clipboard backend, so a frontend neither reads the
    /// policy nor touches the pasteboard.
    ///
    /// - Parameter selection: The caller-owned selection session.
    void applyCopyOnSelect(Ubiquitty::Selection::Session* selection)
    {
        if (!selection)
            return;
        bool shouldCopy = Ubiquitty::Preferences::Effective::copyOnSelect(m_configuration);
        std::string text;
        {
            const std::scoped_lock hold(*m_activeLock);
            Ubiquitty::Terminal* term = modelTerm();
            if (!term)
                return;
            shouldCopy = shouldCopy || term->modes.keepClipboard;
            if (!shouldCopy)
                return;
            text = Ubiquitty::Selection::copiedText(*selection, *term);
        }
        if (!text.empty())
            Ubiquitty::PAL::clipboardSetText(text);
    }

    /// Opens a selection gesture at a cell, under the model lock.
    ///
    /// The gesture rules — click-count growth, word and line modes, the
    /// wide-glyph bounds — are the facade's; this brackets the lock, so a
    /// frontend hands over coordinates and holds no borrow.
    ///
    /// - Parameters:
    ///   - selection: The caller-owned selection session.
    ///   - visibleRow: The pressed cell's viewport row.
    ///   - column: The pressed cell's column.
    ///   - clickCount: The press's click count.
    ///   - viewOffset: Rows the viewport is scrolled back.
    ///   - style: The gesture's modifiers.
    /// - Returns: Whether the selection changed.
    bool beginSelectionGesture(Ubiquitty::Selection::Session* selection, std::ptrdiff_t visibleRow, std::ptrdiff_t column, std::ptrdiff_t clickCount, std::ptrdiff_t viewOffset, Ubiquitty::Selection::GestureStyle style)
    {
        if (!selection)
            return false;
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        return Ubiquitty::Selection::beginGesture(*selection, *term, static_cast<int>(visibleRow), static_cast<int>(column), static_cast<int>(clickCount), static_cast<int>(viewOffset), style);
    }

    /// Extends the open selection gesture to a cell, under the model lock.
    ///
    /// - Parameters:
    ///   - selection: The caller-owned selection session.
    ///   - visibleRow: The dragged-to cell's viewport row.
    ///   - column: The dragged-to cell's column.
    ///   - viewOffset: Rows the viewport is scrolled back.
    /// - Returns: Whether the selection changed.
    bool dragSelectionGesture(Ubiquitty::Selection::Session* selection, std::ptrdiff_t visibleRow, std::ptrdiff_t column, std::ptrdiff_t viewOffset)
    {
        if (!selection)
            return false;
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        return Ubiquitty::Selection::dragGesture(*selection, *term, static_cast<int>(visibleRow), static_cast<int>(column), static_cast<int>(viewOffset));
    }

    /// Records an explicit viewport movement in the remote history's coordinates.
    /// - Parameters:
    ///   - selection: The view-owned selection session.
    ///   - viewOffset: Rows above the tail after the movement.
    void observeSelectionViewport(Ubiquitty::Selection::Session* selection, std::ptrdiff_t viewOffset)
    {
        if (!selection)
            return;
        const std::scoped_lock hold(*m_activeLock);
        if (Ubiquitty::Terminal* term = modelTerm(); term && m_remote && !m_tmuxPrimaryStream)
            Ubiquitty::Selection::observeViewport(*selection, *term, static_cast<int>(viewOffset));
        else {
            selection->usesLogicalViewport = false;
            selection->viewportAnchor.reset();
        }
    }

    /// Captures the view's top logical position before a resize.
    /// - Parameters:
    ///   - selection: The view-owned anchor and history observation.
    ///   - viewOffset: Rows above the tail before resizing.
    void noteViewportBeforeResize(Ubiquitty::Selection::Session* selection, std::ptrdiff_t viewOffset)
    {
        if (!selection)
            return;
        const std::scoped_lock hold(*m_activeLock);
        if (const auto* term = modelTerm())
            Ubiquitty::Selection::noteViewportBeforeResize(*selection, *term, static_cast<int>(viewOffset));
    }

    /// Carries a view across a local resize or remote history rewrap.
    /// - Parameters:
    ///   - selection: The view's retained logical anchor.
    ///   - viewOffset: Rows above the tail before resizing.
    /// - Returns: The offset showing the anchored text, or the oldest survivor.
    [[nodiscard]] int selectionViewportAfterResize(Ubiquitty::Selection::Session* selection, std::ptrdiff_t viewOffset)
    {
        const std::scoped_lock hold(*m_activeLock);
        const auto* term = modelTerm();
        if (!term)
            return 0;
        if (selection)
            return Ubiquitty::Selection::viewportAfterResize(*selection, *term, static_cast<int>(viewOffset));
        return Ubiquitty::Screen::clampViewOffset(static_cast<int>(viewOffset), Ubiquitty::Screen::scrollbackCount(term));
    }

    /// Applies an output transition to a selection and its viewport.
    ///
    /// The frontend supplies the one fact it owns, where its view sits, and
    /// the history facts the drain handed it; Core reads the scroll-on-output
    /// mode, keeps the baseline the growth is measured from, retains the
    /// gesture target, and takes the model lock only as far as the transition
    /// needs it.
    ///
    /// - Parameters:
    ///   - selection: The caller-owned selection session.
    ///   - viewOffset: Rows above the tail before the output.
    ///   - history: The drain's published history facts.
    /// - Returns: The new viewport and whether the selection changed.
    [[nodiscard]] Ubiquitty::Selection::OutputUpdate updateSelectionAfterOutput(Ubiquitty::Selection::Session* selection, std::ptrdiff_t viewOffset, const Ubiquitty::Screen::HistoryFacts& history)
    {
        Ubiquitty::Terminal* term = modelTerm();
        if (!selection || !term) {
            return {};
        }
        return Ubiquitty::Selection::updateAfterOutput(*selection, *term, *m_activeLock, static_cast<int>(viewOffset), history);
    }

    /// Selects the whole buffer, under the model lock.
    ///
    /// - Parameter selection: The caller-owned selection session.
    void selectAllText(Ubiquitty::Selection::Session* selection)
    {
        if (!selection)
            return;
        const std::scoped_lock hold(*m_activeLock);
        if (Ubiquitty::Terminal* term = modelTerm())
            Ubiquitty::Selection::selectAllText(*selection, *term);
    }

    /// The selected text, resolved the way the highlight draws, under the
    /// model lock.
    ///
    /// - Parameter selection: The caller-owned selection session.
    /// - Returns: The text; empty when nothing is selected.
    [[nodiscard]] std::string selectedText(Ubiquitty::Selection::Session* selection)
    {
        if (!selection)
            return {};
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return {};
        return Ubiquitty::Selection::selectedText(*selection, *term);
    }

    /// The selection as the flavours a copy writes, under the model lock.
    ///
    /// The decision is `Selection::copyClipboard`'s; this holds the lock and
    /// names the face, which is the configured family at the size the caller
    /// draws at. A frontend writes the bytes it gets back and decides nothing.
    ///
    /// - Parameters:
    ///   - selection: The caller-owned selection session.
    ///   - requested: The flavour the picked item names; `Configured` for ⌘C.
    ///   - pointSize: The em size the view draws at, zoom included; the rich
    ///     flavours name it, and a non-positive size takes the generator's
    ///     own floor.
    /// - Returns: The flavours; `rtf` and `html` empty for a plain copy or an
    ///   empty selection.
    [[nodiscard]] Ubiquitty::Selection::StyledClipboard selectionClipboard(Ubiquitty::Selection::Session* selection, Ubiquitty::Selection::CopyStyle requested, double pointSize)
    {
        if (!selection)
            return {};
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return {};
        const std::string family = m_configuration.fontFamily.value_or(std::string {});
        const Ubiquitty::Selection::StyledFont font { family.empty() ? nullptr : family.c_str(), pointSize };
        return Ubiquitty::Selection::copyClipboard(*selection, *term, m_configuration, requested, font);
    }

    /// Runs one scroll fact through the ladder every frontend must keep.
    ///
    /// Mouse tracking outranks alternate scroll outranks the local view, and
    /// the order is the whole rule: a frontend restating it drops 1001, which
    /// reports like 1000 with a highlight handshake on top, and the wheel then
    /// scrolls history under an application that asked for reports.
    ///
    /// The route is read from the LIVE terminal rather than a published
    /// snapshot: a program that turns tracking on is answered by the next
    /// gesture, not the one after the next frame.
    ///
    /// The sub-line accumulator is the session's, beside the motion dedup it
    /// resembles — a trackpad delivers fractions of a line, and an accumulator
    /// per frontend means the same swipe travels a different distance on each.
    ///
    /// - Parameters:
    ///   - fact: What the frontend observed; see ``ScrollFact``.
    ///   - tuning: The reader's scroll policy, which the frontend owns.
    /// - Returns: The view movement left for the caller, and whether the
    ///   gesture was consumed.
    ScrollOutcome scrollFact(const ScrollFact& fact, const Ubiquitty::Screen::ScrollTuning::Policy& tuning)
    {
        ScrollOutcome outcome;
        Ubiquitty::Input::ScrollRoute route;
        {
            const AnnouncedModelLock hold(*this); // per event or delivery from the frontend's thread
            Ubiquitty::Terminal* term = modelTerm();
            if (!term)
                return outcome;
            route = Ubiquitty::Input::routeScroll(m_pointerLadder, *term, fact, tuning);
        }
        if (route.byteLength > 0)
            sendInput(ByteSpan { route.bytes.data(), static_cast<std::size_t>(route.byteLength) });
        outcome.linesToOffsetView = route.linesToOffsetView;
        outcome.isViewportScroll = route.isViewportScroll;
        outcome.wasConsumed = route.wasConsumed;
        return outcome;
    }

    /// Runs the mode-1001 hilite deadline check, non-blocking.
    ///
    /// Gated on the published await bit, then a try-lock: under a flood the
    /// ingest thread re-acquires the model lock every parse chunk, and a
    /// deferred deadline tick is harmless — the bound is seconds and the
    /// next animation fire re-checks. An abort republishes the snapshot
    /// itself, because it can happen with zero host output and the await bit
    /// would otherwise hold the caller's animation clock armed until the
    /// next drain.
    ///
    /// - Parameter nowTicks: The monotonic 60 Hz tick.
    /// - Returns: Whether an await aborted and a repaint must clear its
    ///   highlight.
    bool tickHiliteDeadline(uint32_t nowTicks)
    {
        if (!readSnapshot().hiliteAwaiting)
            return false;
        const std::unique_lock hold(*m_activeLock, std::try_to_lock);
        if (!hold.owns_lock())
            return false;
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        const bool didAbort = Ubiquitty::Mouse::tickHilite(term, nowTicks);
        if (didAbort)
            republishModel();
        return didAbort;
    }

    /// Runs a clear command against this session's model.
    ///
    /// The command itself is Core's — see ``Screen::runClearCommand`` — and
    /// this adds only what a session holds: the model lock the grid edit needs,
    /// the route the redraw byte takes, and the republish the scroller sizes
    /// itself from.
    ///
    /// - Parameters:
    ///   - command: Which clear the user asked for.
    ///   - selection: The view's selection, dropped when the rows it names go.
    ///     Null for a frontend with no selection model.
    /// - Returns: Whether the viewport must return to the live tail.
    bool runClearCommand(Ubiquitty::Screen::ClearCommand command, Ubiquitty::SelectionState* selection)
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return false;
        if (m_remote) {
            // Only the authority mutates a mirrored grid; local edits would
            // invalidate the baseline used by already queued host deltas. The
            // host runs the same command, redraw request included, and its
            // frame brings the result. The view returns to the tail because
            // the rows it was showing may be the ones the host is removing.
            if (!m_remote->core || !m_remote->core->sendClearBuffer(command))
                return false;
            if (selection)
                Ubiquitty::Selection::clear(selection);
            return true;
        }
        const bool returnsViewToTail = Ubiquitty::Screen::runClearCommand(
            command, term, selection, [this](ByteSpan bytes) { sendInput(bytes); });
        republishModel();
        return returnsViewToTail;
    }

    /// Captures and publishes the read-snapshot now, under the model lock.
    ///
    /// For a model written without a drain pass — a direct feed, a
    /// main-thread mutation — whose mode changes would otherwise stay
    /// unpublished until the next drain.
    void republishSnapshot()
    {
        const std::scoped_lock hold(*m_activeLock);
        republishModel();
    }

    /// Clears the private 7403 pointer-lock mode, for the app-independent
    /// break-out chord.
    ///
    /// The mode goes coherently off — DECRQM reports reset, the published
    /// snapshot cannot re-lock — so a hostile or hung application that never
    /// sends the reset cannot re-trap the pointer without a fresh set.
    void breakOutOfPointerLock()
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term || !term->modes.mouseRelativePointerLock)
            return;
        term->modes.mouseRelativePointerLock = false;
        republishModel();
        // Unsolicited DECRPM lets the host discard its grab until it requests another.
        constexpr std::string_view resetReport = "\x1b[?7403;2$y";
        sendInput(ByteSpan { resetReport.data(), resetReport.size() });
    }

    /// Advances animated images by the wall time since the previous call,
    /// under the model lock; the session owns the elapsed clock.
    ///
    /// Gated on the published animation bit before the blocking acquire: the
    /// common fire has no animating image — a blinking cursor keeps the tick
    /// armed by itself — and must not pay a per-frame acquire that starves
    /// the ingest drain under flood. The bit can lag one publish; a false
    /// positive wastes one acquire and a false negative costs one frame at
    /// animation start, self-corrected by the next tick.
    ///
    /// Wall micros rather than the 60 Hz tick because per-frame image delays
    /// are finer than a tick; the first call after a reset seeds the clock
    /// and advances nothing.
    ///
    /// - Returns: Whether any image advanced; the caller repaints, and the
    ///   renderer's content stamp folds in the frame index so the draw
    ///   re-uploads.
    bool advanceAnimatedImagesNow()
    {
        const std::uint32_t nowMicros = Ubiquitty::PAL::monotonicMicros();
        if (m_lastImageAdvanceMicros == 0) {
            m_lastImageAdvanceMicros = nowMicros;
            return false;
        }
        const std::uint32_t elapsedMilliseconds = (nowMicros - m_lastImageAdvanceMicros) / 1000u; // cyclic delta
        if (elapsedMilliseconds == 0 || !readSnapshot().imagesAnimating)
            return false;
        m_lastImageAdvanceMicros = nowMicros;
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term || !Ubiquitty::Image::anyAnimating(term))
            return false;
        Ubiquitty::Image::advanceFrames(term, elapsedMilliseconds);
        return true;
    }

    /// Resets the image-advance clock, so a fresh animation seeds cleanly
    /// instead of swallowing the idle span since the previous one stopped.
    void resetImageAdvanceClock() { m_lastImageAdvanceMicros = 0; }

    /// Settles the animation schedule against the model, under the lock.
    ///
    /// The same assembly the AppKit frame clock makes from its delegate,
    /// with the inputs this session can answer for: SGR 5/6 blink from the
    /// grid's hint, and the cursor's own fade. Image animation and bell
    /// flash are view tiers a surface host composes; they join the inputs
    /// when a surface carries them.
    ///
    /// Announced, like ``frameClockFactsNow``: this runs after every drawn
    /// frame, and a plain acquire here was the other half of the render
    /// thread's waiting under a flood.
    ///
    /// - Returns: Whether anything animates, and the delay to its next
    ///   visible change.
    [[nodiscard]] AnimationScheduleVerdict nextAnimationDelay()
    {
        AnimationScheduleVerdict verdict;
        const std::uint32_t nowTicks = frameTicksNow();
        lockForRead();
        if (Ubiquitty::Terminal* term = modelTerm()) {
            Ubiquitty::Rendering::AnimationScheduleInputs inputs;
            // The hint is sticky; the live-screen reading beside it is what
            // stands a timer down once the blink content is erased. Scrolled
            // back, the rows on display are outside that reading, so the hint
            // alone keeps the clock conservatively armed — a blink cell in
            // viewed history must not freeze.
            // Rendering::isBlinkContentVisible's rule in its lazy spelling:
            // the offset check goes first because the live-screen reading is
            // a scan over the grid, and a scrolled-back view answers without
            // it.
            if (term->grid && term->grid->hasBlinkHint()
                && (m_presentedViewOffset.load(std::memory_order_relaxed) > 0
                    || Ubiquitty::Rendering::anyCellBlinks(*term)))
                inputs.blinkDelayTicks = Ubiquitty::ticksUntilAnimationRedraw(term->grid.get(), nowTicks);
            inputs.cursorDelayTicks = Ubiquitty::Cursor::ticksUntilAlphaChange(term, nowTicks);
            verdict.hasAnimatingContent = inputs.blinkDelayTicks != 0 || inputs.cursorDelayTicks != 0;
            verdict.delaySeconds = inputs.nextDelaySeconds();
        }
        unlockForRead();
        return verdict;
    }

    /// Acquires the model lock for a read the runtime does not make, the
    /// render capture above all.
    ///
    /// Pairs with ``unlockForRead``, and announces the wait first: the drain's
    /// paced unlock reads the announcement and donates the lock rather than
    /// re-acquiring within nanoseconds, which is the doorway that turned
    /// 49–53 ms render-snapshot waits into microseconds on the Metal arm.
    void lockForRead()
    {
        m_isRenderWaiting.store(true, std::memory_order_release);
        m_activeLock->lock();
        m_isRenderWaiting.store(false, std::memory_order_release);
    }

    /// Releases the model lock taken by ``lockForRead``.
    void unlockForRead() { m_activeLock->unlock(); }

    /// A reader's hold on the model, for a frontend thread that reads the
    /// Terminal directly while the ingest thread parses into it.
    ///
    /// Free when the session drains inline: there the frontend's thread is
    /// the model's own, and a Terminal read inside a drain's own callback —
    /// a banner composing its tab title, say — would wait on the lock that
    /// drain already holds. Nests: the outermost scope takes the lock and the
    /// inner ones count, so a helper that reads the model is callable from
    /// under a caller's scope. One reader thread per session; the depth is
    /// that thread's.
    ///
    /// Not for a region that asks the session something under it: the
    /// session's own members take this same mutex, and it is not recursive.
    /// Read the Terminal, let the scope end, then ask. Nor around
    /// ``stopIngest``, which joins a thread that may be waiting on the lock
    /// the scope holds.
    class [[nodiscard]] ModelReadScope final {
    public:
        /// Takes the hold: the lock when the session's ingest thread runs,
        /// nothing otherwise.
        ///
        /// - Parameter session: The session whose model is read.
        explicit ModelReadScope(TerminalSession& session)
            : m_session(session.m_ingestThread.joinable() ? &session : nullptr)
        {
            if (m_session && m_session->m_modelReadDepth++ == 0)
                m_session->lockForRead();
        }

        /// Ends the hold, releasing the lock with the outermost scope.
        ~ModelReadScope()
        {
            if (m_session && --m_session->m_modelReadDepth == 0)
                m_session->unlockForRead();
        }
        ModelReadScope(const ModelReadScope&) = delete;
        ModelReadScope& operator=(const ModelReadScope&) = delete;

        /// Ends the hold early, for a reader that has read and now asks.
        void release()
        {
            if (m_session && --m_session->m_modelReadDepth == 0)
                m_session->unlockForRead();
            m_session = nullptr;
        }

    private:
        TerminalSession* m_session;
    };

    /// Holds the model for a direct read on the frontend's thread; see
    /// ``ModelReadScope``.
    [[nodiscard]] ModelReadScope modelReadScope() { return ModelReadScope(*this); }

    /// The model lock taken with the render announcement, for a session
    /// member a frontend calls once per delivery.
    ///
    /// A plain `scoped_lock` on the model waits for the parser's whole pass,
    /// which under a flood is the pass's capacity in bytes; the announcement
    /// is what the drain's paced unlock reads to donate the lock between
    /// bites, so the wait is one bite rather than a whole pass.
    class [[nodiscard]] AnnouncedModelLock final {
    public:
        /// Announces and takes the lock.
        ///
        /// - Parameter session: The session whose model is locked.
        explicit AnnouncedModelLock(TerminalSession& session)
            : m_session(session)
        {
            m_session.lockForRead();
        }

        /// Releases the lock.
        ~AnnouncedModelLock() { m_session.unlockForRead(); }
        AnnouncedModelLock(const AnnouncedModelLock&) = delete;
        AnnouncedModelLock& operator=(const AnnouncedModelLock&) = delete;

    private:
        TerminalSession& m_session;
    };

    /// The model lock itself, for a render adapter that brackets with it.
    ///
    /// The adoption seam: an adapter that renders this session's Terminal
    /// takes the same mutex the drain bites, by address, because a second
    /// lock over one model is two opinions about exclusivity. The pointer
    /// lives exactly as long as the session.
    [[nodiscard]] std::mutex* modelLockPointer() { return m_activeLock; }

    /// The same pointer as an integer, for the interop seam.
    ///
    /// `std::mutex` does not import, and the importer also drops a bare
    /// pointer return on a shared-reference type, so the address crosses as
    /// a number — which is the correct amount of access: Swift can neither
    /// dereference nor retype it, and the one consumer casts it back on the
    /// C++ side where the type lives.
    [[nodiscard]] std::uintptr_t modelLockAddressForAdoption() { return reinterpret_cast<std::uintptr_t>(&m_modelLock); }

    /// Becomes a render adapter for an externally owned Terminal.
    ///
    /// Every model accessor reads and mutates the adopted terminal under the
    /// adopted lock — a second lock over one model is two opinions about
    /// exclusivity — and teardown never disposes it: the owner does, after
    /// the frontend unbinds. The session's own runtime terminal stays an
    /// unused publish anchor, and an adopted session never starts ingest.
    /// Both pointers must outlive this session's use of them.
    ///
    /// - Parameters:
    ///   - terminal: The externally owned terminal to render.
    ///   - lockAddress: The owner's model lock, as
    ///     ``modelLockAddressForAdoption`` hands it across the interop seam.
    void adoptExternalTerminal(Ubiquitty::Terminal* terminal, std::uintptr_t lockAddress)
    {
        if (terminal && hasUnresolvedLocalAdoption())
            return;
        detachSearch();
        // Un-install what this session wired into the terminal it is leaving.
        // The text measurer is a session member behind a CheckedPtr; the
        // adopted terminal outlives the session by contract, so a member
        // destroyed while that terminal still references it is the CheckedPtr
        // abort — the exact teardown every tmux pane close was taking.
        if (m_externalTerminal && m_externalTerminal != terminal) {
            const std::scoped_lock uninstall(*m_activeLock);
            for (auto& owned : m_ownedClients) {
                owned.detach(*m_externalTerminal, *owned.client);
            }
            m_ownedClients.clear();
            if (m_externalTerminal->textMeasureClient.get() == &m_textMeasurer)
                m_externalTerminal->textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
            if (m_externalTerminal->typography.shaper.context == m_richTextShaper.context)
                m_externalTerminal->typography.shaper = {};
            detachSessionClients(*m_externalTerminal);
        }
        m_externalTerminal = terminal;
        m_activeLock = terminal ? reinterpret_cast<std::mutex*>(lockAddress) : &m_modelLock;
        if (!terminal) {
            m_hostedPaneSource = nullptr;
            m_hostedPaneID = -1;
        }
        {
            // Another terminal's palette epoch counts from its own start, so a
            // theme identity matched against the old one proves nothing here.
            const std::scoped_lock identityHold(m_themeIdentityLock);
            m_themeIdentity.reset();
        }
        const std::scoped_lock hold(*m_activeLock);
        // The adopted terminal answers to this session: a window verb, a
        // banner or a trigger report it raises is queued on the adapter that
        // renders it, so the frontend attributes it to that pane. The owner's
        // terminal would otherwise keep whatever it was born with, which for a
        // tmux pane is nothing, and every such request from it would be parsed
        // and dropped.
        if (terminal) {
            attachSessionClients(*terminal);
            installTextMeasurer(*terminal);
        }
        m_runtime->setSearchStatus({});
        republishModel();
    }

    /// Whether an adopted terminal's window verbs, banners and trigger
    /// reports reach this session.
    ///
    /// - Returns: Whether all three of the adopted terminal's client slots
    ///   name this session's own clients; false when nothing is adopted.
    [[nodiscard]] bool hasAdoptedTerminalClients() const // bool-ok: a fact about the adoption, not a mode
    {
        if (!m_externalTerminal)
            return false;
        const std::scoped_lock hold(*m_activeLock);
        return m_externalTerminal->windowClient.isInstalled(&m_windowClient) && m_externalTerminal->shellClient.get() == &m_shellClient
            && m_externalTerminal->triggerClient.get() == &m_triggerClient;
    }

    /// Ends an external adoption: the session stops using the owner's pointers.
    ///
    /// The adoption contract is that the owner disposes the terminal after the
    /// frontend unbinds — so an unbind calls this first, and only then may the
    /// owner free. A session that skips this and is torn down later touches the
    /// owner's freed terminal from its own teardown.
    void endExternalAdoption() { adoptExternalTerminal(nullptr, 0); }

    /// The signature of a per-drain signal.
    using DrainSignal = void (*)(void*);

    /// Installs the signal fired by an event producer when the frontend has work.
    ///
    /// For a frontend whose reactions live on another thread: the Metal arm's
    /// bridge dispatches to main from exactly this point in the drain, and a
    /// caller of this hook does the same on its own side of the seam. The
    /// signal fires once per service — see ``signalDrain`` — so the handler
    /// posts, and the post's target takes the flags. Replacing or clearing the
    /// signal waits for active calls before releasing their context. The
    /// caller holds no model lock and must not call this from the signal itself.
    ///
    /// - Parameters:
    ///   - context: Handed back to every call, opaque.
    ///   - signal: The function to call, or null to unregister.
    void setDrainSignal(void* context, DrainSignal signal)
    {
        std::unique_lock hold(m_drainSignalMutex);
        m_drainSignal = nullptr;
        m_drainSignalIdle.wait(hold, [this] { return !m_drainSignalsInFlight; });
        m_drainSignalContext = context;
        m_drainSignal = signal;
        m_isDrainSignalArmed.store(false, std::memory_order_release);
    }

    /// Fires the drain signal once per service.
    ///
    /// Every path that raises an event bit comes through here, and the call
    /// coalesces: a second eventful pass before the frontend has taken the
    /// flags adds its bits and fires nothing, so a frontend that answers the
    /// signal by posting to its main queue holds at most one post in flight
    /// per session however fast the passes come. ``takeEventFlags`` re-arms
    /// it, which is what makes the one post sufficient: the bits are taken
    /// after the re-arm, so a pass that lands bits once the frontend is past
    /// the re-arm fires again, and one that landed them before is served by
    /// that take.
    void signalDrain()
    {
        if (m_isDrainSignalArmed.exchange(true, std::memory_order_acq_rel))
            return;
        DrainSignal signal = nullptr;
        void* context = nullptr;
        {
            const std::scoped_lock hold(m_drainSignalMutex);
            signal = m_drainSignal;
            if (!signal)
                return;
            context = m_drainSignalContext;
            ++m_drainSignalsInFlight;
        }
        signal(context);
        {
            const std::scoped_lock hold(m_drainSignalMutex);
            --m_drainSignalsInFlight;
        }
        m_drainSignalIdle.notify_all();
    }

    /// ``setDrainSignal`` with both addresses as integers, for the interop
    /// seam: a function-pointer parameter is dropped from the generated C++
    /// surface entirely, so the caller hands numbers across and this casts
    /// them back on the side where the types live.
    ///
    /// - Parameters:
    ///   - context: The context's address.
    ///   - function: The signal function's address, or zero to clear.
    void setDrainSignalRaw(std::uintptr_t context, std::uintptr_t function)
    {
        setDrainSignal(reinterpret_cast<void*>(context), reinterpret_cast<DrainSignal>(function));
    }

    /// Tries the model lock without waiting, for the per-drain and per-move
    /// paths — link hover, hilite deadlines, snap-to-tail — where blocking
    /// has no fairness doorway against ingest and a flood starves the caller
    /// into a slideshow. A failed try is harmless there: every such read
    /// re-resolves on the next delivery, scroll, or move.
    ///
    /// No announcement, on either outcome: the announce flag exists so the
    /// drain donates the lock to a WAITER, and a try never waits.
    ///
    /// - Returns: Whether the lock was taken; pair a true with
    ///   ``unlockForRead``.
    [[nodiscard]] bool tryLockForRead() { return m_activeLock->try_lock(); }

    /// Allocates a session reading the configuration it is handed.
    ///
    /// The only way to make one. A stack instance would be released as though
    /// it were heap memory the first time Swift saw a pointer to it, which is
    /// the hazard a shared reference type carries and the reason the
    /// constructor is not public.
    ///
    /// The session borrows `configuration` for its whole life and reads it at
    /// the moment each preference matters, so the owner keeps that instance
    /// alive past the session and a change written into it reaches the
    /// session. A host giving each session its own configuration passes the
    /// session's own instance; nothing here consults ``Preferences::values``.
    ///
    /// - Parameter configuration: The preferences this session reads.
    /// - Returns: A session with one reference, or null if allocation failed.
    [[nodiscard]] static TerminalSession* make(const Preferences::Values& configuration) UBIQUITTY_RETURNS_RETAINED
    {
        return new (std::nothrow) TerminalSession(configuration);
    }

    /// Allocates a session reading the process-wide preferences.
    ///
    /// The frontends' form: one process, one configuration, and a configure
    /// (``Preferences::configure``) reaches every session made this
    /// way because they all borrow the one live instance.
    ///
    /// - Returns: A session with one reference, or null if allocation failed.
    [[nodiscard]] static TerminalSession* make() UBIQUITTY_RETURNS_RETAINED
    {
        return make(Preferences::values());
    }

    /// The reference count Swift manipulates through the global retain and
    /// release. Public because those are free functions.
    std::atomic<int> swiftReferences { 1 };

private:
    /// The in-process endpoint keeps the locked model's encoding and the
    /// existing PTY/tmux output path. It neither constructs wire events nor
    /// retains an input buffer across calls.
    class LocalPaneLink final : public Session::PaneLink {
    public:
        explicit LocalPaneLink(TerminalSession& owner)
            : m_owner(owner)
        {
        }

        bool send(const Session::PaneInput& input) override
        {
            if (input.localEncoding.empty() || !m_owner.m_runtime->isLocalStreamEnabled())
                return false;
            m_owner.sendLocalInput(input.localEncoding);
            return true;
        }

        /// Reads the local PTY erase setting.
        /// - Returns: VERASE, absent when disabled, or DEL without a local PTY.
        std::optional<std::uint8_t> lineEraseCharacter() const override
        {
            if (m_owner.m_externalTerminal)
                return 0x7f;
            auto* stream = m_owner.m_runtime->stream();
            const auto discipline = stream ? stream->process().lineDiscipline() : std::nullopt;
            return discipline ? discipline->eraseCharacter : std::optional<std::uint8_t> { 0x7f };
        }

        Session::PaneView view() override
        {
            auto* const terminal = m_owner.m_externalTerminal ? m_owner.m_externalTerminal : m_owner.m_runtime->borrowTermForRender();
            return { .terminal = terminal,
                .facts = m_owner.m_preparedRemote ? nullptr : m_facts.factsFor(terminal),
                .hasFrameNotifications = !m_owner.m_externalTerminal || (m_owner.m_hostedPaneSource && m_owner.m_hostedPaneSource->hasPanePublications()) };
        }

        void setView(Protocol::Sync::ViewState state) override { m_view = state; }

        void setViewport(const Session::PaneViewport& viewport) override
        {
            if (!m_owner.m_runtime->isLocalStreamEnabled())
                return;
            // A hosted model's source remains its geometry authority.
            if (m_owner.m_externalTerminal && m_owner.m_hostedPaneSource) {
                m_owner.m_hostedPaneSource->resizePane(m_owner.m_hostedPaneID, viewport.columns, viewport.rows);
                const std::scoped_lock hold(*m_owner.m_activeLock);
                m_owner.republishModel();
                return;
            }
            {
                const std::scoped_lock hold(*m_owner.m_activeLock);
                // A non-shell program repaints its own rows on SIGWINCH;
                // rewrapping those rows underneath it duplicates them.
                Ubiquitty::Terminal* term = m_owner.modelTerm();
                const bool allowsReflow = term && Ubiquitty::shouldReflowOnResize(term, m_owner.m_runtime->isForegroundShell());
                m_owner.m_runtime->resizeGrid(viewport.rows, viewport.columns,
                    allowsReflow ? Modes::ReflowMode::Reflow : Modes::ReflowMode::Fixed,
                    viewport.urgency, viewport.anchor);
            }
            setWindowSize(viewport.columns, viewport.rows, viewport.widthInPixels, viewport.heightInPixels);
        }

        void setWindowSize(int columns, int rows, int widthInPixels, int heightInPixels) override
        {
            m_owner.m_runtime->setWindowSize(columns, rows, widthInPixels, heightInPixels);
        }

        void setPixelGeometry(const PixelGeometry& geometry) override
        {
            if (!m_owner.m_runtime->isLocalStreamEnabled())
                return;
            const std::scoped_lock hold(*m_owner.m_activeLock);
            if (!m_owner.m_preparedRemote) {
                if (auto* const terminal = m_owner.modelTerm())
                    Ubiquitty::publishPixelGeometry(*terminal, geometry);
            }
        }

        void observe(Session::PaneLinkObserver& observer) override
        {
            if (m_events.observe(observer))
                m_notifiedFacts = 0;
        }

        void pollEvents(std::uint64_t now) override
        {
            if (!m_events.hasObserver())
                return;
            std::optional<Session::FrameReady> frame;
            std::uint64_t factsGeneration = 0;
            {
                // Polled from the frontend's thread on every delivery, so it
                // waits announced: unannounced it sat behind each flood pass.
                const AnnouncedModelLock hold(m_owner);
                auto* const terminal = view().terminal;
                if (terminal && !m_owner.m_preparedRemote) {
                    if (m_hostFacts.isAdopted != !!m_owner.m_externalTerminal)
                        m_hostFacts = {};
                    if (m_owner.m_externalTerminal)
                        m_hostFacts.isAdopted = true;
                    else
                        m_owner.m_runtime->updatePaneHostFacts(m_hostFacts);
                    // Only this model's measured pixels and OSC reports are
                    // known for an adopted pane; the adapter's idle process
                    // cache belongs to another terminal and must not leak in.
                    const auto textArea = Ubiquitty::reportedTextAreaPixels(*terminal);
                    m_hostFacts.pixels = { .cellWidth = terminal->cellPixelWidth, .cellHeight = terminal->cellPixelHeight, .textAreaWidth = static_cast<std::uint16_t>(std::clamp(textArea.width, 0, 65535)), .textAreaHeight = static_cast<std::uint16_t>(std::clamp(textArea.height, 0, 65535)) };
                    m_facts.refresh(*terminal, m_hostFacts);
                    factsGeneration = m_facts.generation();
                }
                if (m_view == Protocol::Sync::ViewState::Live) {
                    if (m_owner.m_externalTerminal && m_owner.m_hostedPaneSource) {
                        frame = m_owner.m_hostedPaneSource->panePublication(m_owner.m_hostedPaneID, *m_owner.m_externalTerminal);
                    } else if (m_owner.m_externalTerminal) {
                        // Other adopted owners must supply their own publication seam.
                    } else if (m_owner.m_preparedRemote) {
                        frame = m_owner.m_restoredPreview;
                    } else if (const auto frameClass = m_owner.m_runtime->lastFrameClass()) {
                        std::uint64_t generation = 0;
                        for (std::size_t index = 0; index < frameClassCount; ++index)
                            generation += m_owner.m_runtime->frameCount(static_cast<FrameClass>(index));
                        frame = Session::FrameReady { .frameClass = wireFrameClass(*frameClass),
                            .generation = generation,
                            .lagMicroseconds = m_owner.m_runtime->lastFrameLagMicroseconds() };
                    }
                }
            }
            if (frame)
                m_events.noteFrame(*frame);
            if (factsGeneration != 0 && factsGeneration != m_notifiedFacts) {
                m_notifiedFacts = factsGeneration;
                m_events.noteFacts();
            }
            m_events.noteState(linkState(now));
            m_events.deliver();
        }

        void setFocus(Ubiquitty::Modes::FocusState focus) override
        {
            if (m_owner.m_externalTerminal) {
                if (m_owner.m_hostedPaneSource)
                    (void)m_owner.m_hostedPaneSource->setPaneFocus(m_owner.m_hostedPaneID, *m_owner.m_externalTerminal, focus);
                const std::scoped_lock hold(*m_owner.m_activeLock);
                // A raw adopted model has no program-focus route. Its local
                // presentation still follows focus; a hosted source above
                // owns any DEC 1004 report and drains it to that exact pane.
                m_owner.m_externalTerminal->windowFocused = focus == Modes::FocusState::Focused;
                if (focus == Modes::FocusState::Focused)
                    (void)Ubiquitty::markAttentionSeen(*m_owner.m_externalTerminal);
                m_owner.republishModel();
                return;
            }
            Command command;
            command.kind = CommandKind::SetFocus;
            command.flag = focus == Ubiquitty::Modes::FocusState::Focused;
            m_owner.postCommand(command);
        }

        /// Ignores the view's depth: a local terminal holds all its own history.
        void setViewOffset(int) override { }

        bool wantHistory(Protocol::Sync::HistoryPriority priority, std::span<const Protocol::Sync::LineRange> ranges) override
        {
            if (std::to_underlying(priority) > std::to_underlying(Protocol::Sync::HistoryPriority::Backfill)
                || ranges.size() > Protocol::Sync::maxHistoryWantRanges)
                return false;
            for (std::size_t index = 0; index < ranges.size(); ++index) {
                if (ranges[index].from >= ranges[index].to || (index && ranges[index].from < ranges[index - 1].to))
                    return false;
            }
            return true;
        }

        Session::LinkState linkState(std::uint64_t now) const override
        {
            Session::LinkState state;
            if (m_owner.m_preparedRemote)
                return state;
            state.phase = Session::LinkPhase::Live;
            state.transport.lastHeardMicroseconds = now;
            return state;
        }

        std::uint64_t clockMicroseconds() const override
        {
            if (m_owner.m_externalTerminal && m_owner.m_hostedPaneSource && m_owner.m_hostedPaneSource->hasPanePublications())
                return m_owner.m_hostedPaneSource->publicationClockMicroseconds();
            return m_owner.m_preparedRemote ? m_owner.m_preparedRemote->screenClockMicroseconds() : m_owner.m_runtime->frameClockMicroseconds();
        }

    private:
        TerminalSession& m_owner;
        Session::PaneLinkEvents m_events;
        PaneFactsCapture m_facts;
        Protocol::Sync::PaneHostFacts m_hostFacts;
        std::uint64_t m_notifiedFacts = 0;
        Protocol::Sync::ViewState m_view = Protocol::Sync::ViewState::Live;
    };

    /// The native shell's default subscriber, shared by local and remote panes.
    struct PaneEventObserver final : Session::PaneLinkObserver {
        explicit PaneEventObserver(TerminalSession& owner)
            : session(owner)
        {
        }
        void frameReady(const Session::FrameReady& frame) override { session.m_lastPaneLinkFrame = frame; }
        void linkStateChanged(const Session::LinkState&) override
        {
            if (!session.m_remote && !session.m_preparedRemote)
                return;
            session.m_events.pending.fetch_or(1u << static_cast<unsigned>(EventKind::LinkStateChanged), std::memory_order_release);
        }
        TerminalSession& session;
    };

    /// Chooses the current pane owner, including an adopted tmux pane.
    /// - Returns: A borrowed link, used only for the immediate operation.
    [[nodiscard]] Session::PaneLink& paneLink()
    {
        if (m_remote && m_remote->core && !m_tmuxPrimaryStream)
            return *m_remote->core;
        return m_localPaneLink;
    }

    /// Writes an already-resolved local input outside the terminal model lock.
    /// - Parameter bytes: Borrowed encoded bytes; consumed before returning.
    void sendLocalInput(std::span<const char> bytes)
    {
        const auto weak = weakPtr();
        if (m_tmuxPrimaryStream) {
            auto* const stream = m_tmuxPrimaryStream;
            auto* const client = m_tmuxRoutingClient;
            const int pane = m_tmuxRoutingPaneID;
            std::array<char, 4096> wrapped;
            while (!bytes.empty()) {
                const auto piece = bytes.first(std::min<std::size_t>(bytes.size(), 128));
                const int written = Protocol::Tmux::wrapKeyForPane(client, pane,
                    std::string_view { piece.data(), piece.size() }, wrapped);
                if (written <= 0)
                    return;
                stream->writeAll({ wrapped.data(), static_cast<std::size_t>(written) });
                if (!weak || m_tmuxPrimaryStream != stream || m_tmuxRoutingClient != client || m_tmuxRoutingPaneID != pane)
                    return;
                bytes = bytes.subspan(piece.size());
            }
            return;
        }
        m_runtime->sendInputBytes(bytes);
        if (weak && m_runtime->hasPendingWrite())
            m_loop.raiseWake();
    }

    explicit TerminalSession(const Preferences::Values& configuration)
        : m_configuration(configuration)
        , m_host(m_modelLock, &m_isRenderWaiting)
        , m_runtime(std::make_unique<TerminalRuntime>())
    {
        if (const auto* theme = Themes::get(launchThemeIndex(configuration))) {
            m_chosenThemeName = theme->name;
        }
        m_runtime->setEventSink(&m_events);
        // The publish hook raises the session's flag; the pacing above it is
        // the host's, shared with the Metal arm rather than restated here.
        // A boundary is captured into the sync slot: its grid is gone by the
        // next byte, so the ingest thread is the one place its frame exists to
        // be copied. A settle's grid is still there when the render arrives:
        // the Swift live route captures it for itself against its own retained
        // baseline -- the rows that changed, not the screen -- so a whole-grid
        // copy here would only feed the full-rebuild boundary route a frame the
        // cheap route could draw. A saturated pass is captured too, for the
        // opposite reason: under a flood every row changed, and the live route's
        // cost is not the copy but the wait for this lock. The flag rises for
        // every reason, so the wake is unchanged.
        //
        // A settle captures after all when one is already standing. The cheap
        // live route is only cheaper while the slot is empty: a capture nobody
        // has taken is older than this moment, the render prefers the slot to
        // the model, and it would present a grid the screen has already left
        // and then owe nothing -- so whatever the settle was going to say is
        // stranded until an unrelated edge asks for a frame. nvim's showcmd is
        // the case: the pending key is drawn into the last row, the cell is
        // cleared a moment later inside one publish window, and the glyph stays
        // on the glass. Replacing the standing capture costs a whole-grid copy
        // exactly where one is owed, and a quiet screen's settles find the slot
        // empty and still pay nothing.
        m_host.setPublishHook(
            [](void* context, SyncFrameReason reason) {
                auto* session = static_cast<TerminalSession*>(context);
                if (reason == SyncFrameReason::Boundary) {
                    // Counted before the capture's own armed check, so a reader
                    // can tell a boundary that was never announced from one that
                    // was announced to a session holding no capture storage.
                    session->m_syncFrameBoundaryAnnounceCount.fetch_add(1, std::memory_order_relaxed);
                }
                // A boundary, saturated pass, or continuing pass always copies:
                // its admitted grid is gone by the next byte. A settle and a
                // burst end copy only when they carry something the standing capture does
                // not: a capture nobody took, and a grid that has moved since
                // it was taken. Both conditions are needed. Without the first,
                // this is a whole-grid copy per burst end, which is once per
                // keystroke at a prompt; without the second, a capture that is
                // still accurate is replaced by one that re-describes the baked
                // cursor, so the row the cursor left is owed a repaint it does
                // not need.
                const bool isCopyReason
                    = reason == SyncFrameReason::Boundary || reason == SyncFrameReason::SaturatedPass
                    || reason == SyncFrameReason::ContinuingPass;
                const bool isStandingCaptureStale = !isCopyReason && session->m_syncSlot.hasReadyCapture()
                    && session->hasGridWorkSinceLastPaint();
                if (isStandingCaptureStale)
                    session->m_staleCaptureReplacedCount.fetch_add(1, std::memory_order_relaxed);
                if (isCopyReason || isStandingCaptureStale)
                    session->captureSyncFrame();
                session->m_frameReady.store(true, std::memory_order_release);
            },
            this);
        // The publish above raises a flag and wakes nobody, which is enough
        // when a delivery follows it. A release on the rule's clock has none
        // behind it, and the draw the burst's delivery asked for was refused
        // while the rule held the frame; the frame is announced here as a
        // screen to draw, or it waits for bytes that may never come.
        m_loop.setReleaseHook(
            [](void* context) {
                auto* session = static_cast<TerminalSession*>(context);
                session->m_events.pending.fetch_or(1u << static_cast<unsigned>(EventKind::ScreenDirty), std::memory_order_release);
                session->signalDrain();
            },
            this);
        // The drain path speaks through the outcome, not the sink: the
        // runtime's sink fires only on the local-echo and pipelined feeds,
        // and nothing emits Exited at all — the Metal arm's observer derives
        // it from reachedEnd on each delivery. The session derives the same
        // bits here, so a reader of the flags cannot tell which path raised
        // one, which is the point of having one flag surface.
        m_loop.setDeliverHook(
            [](void* context, const DrainOutcome& outcome, std::uint32_t ingestMicros) {
                auto* session = static_cast<TerminalSession*>(context);
                session->m_drainedBytes.fetch_add(
                    static_cast<std::uint64_t>(outcome.bytesDrained), std::memory_order_release);
                if (session->m_isTallyingPerfDrains.load(std::memory_order_relaxed))
                    session->tallyPerfDrain(outcome, ingestMicros);
                std::uint32_t bits = 0;
                if (outcome.bytesDrained > 0)
                    bits |= 1u << static_cast<unsigned>(EventKind::ScreenDirty);
                if (outcome.reachedEnd)
                    bits |= 1u << static_cast<unsigned>(EventKind::Exited);
                session->m_events.bellsRung.fetch_add(outcome.bellsRung, std::memory_order_relaxed);
                if (outcome.bell != Ubiquitty::BellMode::Off) {
                    bits |= 1u << static_cast<unsigned>(EventKind::Bell);
                    session->m_events.bellStyle.store(
                        static_cast<std::uint32_t>(outcome.bell), std::memory_order_relaxed);
                    // Written before the flag below, which is release-ordered:
                    // a frontend reading the cue on the far side of the hop
                    // sees all four or none of them.
                    session->m_events.bellPopsWindow.store(outcome.popOnBell, std::memory_order_relaxed);
                    session->m_events.bellRaisesUrgency.store(
                        outcome.bellRaisesUrgency, std::memory_order_relaxed);
                    session->m_events.bellWantsNotification.store(
                        outcome.bellWantsNotification, std::memory_order_relaxed);
                }
                if (outcome.titleChanged)
                    bits |= 1u << static_cast<unsigned>(EventKind::TitleChanged);
                if (outcome.currentDirectoryChanged)
                    bits |= 1u << static_cast<unsigned>(EventKind::DirectoryChanged);
                if (outcome.processStateChanged) {
                    bits |= 1u << static_cast<unsigned>(EventKind::ProcessStateChanged);
                    session->noteForegroundForHostedWindows();
                }
                if (outcome.fontRequestChanged)
                    bits |= 1u << static_cast<unsigned>(EventKind::FontSwapRequested);
                if (bits != 0) {
                    session->m_events.pending.fetch_or(bits, std::memory_order_release);
                    // After the release-ordered flag write, so a handler that
                    // reads the flags on the far side of the hop sees them.
                    // Eventful passes only: quiet drains stay quiet.
                    session->signalDrain();
                }
            },
            this);
    }

    /// Adds one delivery to the perf log's tally, on the loop's thread.
    ///
    /// - Parameters:
    ///   - outcome: The delivery.
    ///   - ingestMicros: The parse time it covers.
    void tallyPerfDrain(const DrainOutcome& outcome, std::uint32_t ingestMicros)
    {
        m_perfDrainBytes.fetch_add(static_cast<std::uint64_t>(outcome.bytesDrained), std::memory_order_relaxed);
        m_perfDrainIngestMicroseconds.fetch_add(ingestMicros, std::memory_order_relaxed);
        std::uint32_t longest = m_perfDrainLongestIngestMicroseconds.load(std::memory_order_relaxed);
        while (ingestMicros > longest
            && !m_perfDrainLongestIngestMicroseconds.compare_exchange_weak(longest, ingestMicros, std::memory_order_relaxed)) {
        }
        // Last and release-ordered, so a take that sees the delivery sees
        // what it carried.
        m_perfDrainDeliveries.fetch_add(1, std::memory_order_release);
    }

    /// The sink the runtime calls inline on the writer thread.
    ///
    /// Flags only, per the sink contract: no dispatch, no allocation, nothing
    /// that can wait while the model lock is held.
    struct FlagSink final : EventSink {
        /// Coalesces one event into the pending bits.
        ///
        /// - Parameter event: The event, whose kind indexes its bit.
        void onRuntimeEvent(const Event& event) override
        {
            pending.fetch_or(1u << static_cast<unsigned>(event.kind), std::memory_order_release);
            if (event.kind == EventKind::Bell)
                bellStyle.store(event.data0, std::memory_order_relaxed);
            if (event.kind == EventKind::PastePushSettled)
                pastePushStatus.store(event.data0, std::memory_order_relaxed);
            if (event.kind == EventKind::HostSessionTerminateSettled)
                hostSessionTerminateOutcome.store(event.data0, std::memory_order_relaxed);
        }

        /// One bit per ``EventKind``, cleared by the reader.
        std::atomic<std::uint32_t> pending { 0 };

        /// The most recent bell's style byte.
        std::atomic<std::uint32_t> bellStyle { 0 };

        /// What that bell asked for beyond its cue. Stored beside the style
        /// rather than packed into it: three named flags a reader can check
        /// against DECSET 1042 and 1043 beat three bit positions nothing
        /// names.
        std::atomic<bool> bellPopsWindow { false };
        std::atomic<bool> bellRaisesUrgency { false };
        std::atomic<bool> bellWantsNotification { false };

        /// The bells this pane rang since a reader last took them: its own
        /// terminal's route for parsed bytes, one for each far-side bell.
        std::atomic<std::uint32_t> bellsRung { 0 };

        /// The most recent settled push's status.
        std::atomic<std::uint32_t> pastePushStatus { 0 };

        /// The most recent settled host-session terminate's outcome.
        std::atomic<std::uint32_t> hostSessionTerminateOutcome { 0 };
    };

    // Declared before the runtime: members destroy in reverse order, so the
    // runtime — which holds a checked pointer to this sink — dies first.
    FlagSink m_events;
    std::atomic<bool> m_frameReady { false };

    /// Captures the completed boundary frame into the slot's free buffer.
    ///
    /// Runs inside the publish hook, on the ingest thread under the model
    /// lock, so the frame it copies is the frame the boundary completed. A
    /// boundary that arrives while a pending frame waits captures over it —
    /// the pending buffer is read by nobody, and newest-wins is
    /// ``takeSyncFrameReady``'s own coalescing contract. The drive's buffer is
    /// never the target, so the frame a caller holds is stable until its next
    /// drain.
    void captureSyncFrame()
    {
        if (!m_isSyncCaptureArmed || !m_isSyncFramePresented.load(std::memory_order_relaxed))
            return;
        Ubiquitty::Terminal* term = modelTerm();
        if (!term)
            return;
        m_syncCaptureAttemptCount.fetch_add(1, std::memory_order_relaxed);

        std::uint8_t unavailableMask = 0;
        int target = -1;
        for (int candidate = 0; candidate < SyncFrameSlot::slotCount; ++candidate) {
            target = m_syncSlot.captureTarget(unavailableMask);
            if (target < 0) {
                break;
            }
            // A Free slot may still have an external owner from an earlier
            // take. The state transition's acquire observes that take before
            // any bytes are written into the capture.
            if (m_syncCaptures[target]->swiftReferences.load(std::memory_order_acquire) == 1) {
                break;
            }
            unavailableMask |= static_cast<std::uint8_t>(1u << static_cast<unsigned>(target));
            m_syncSlot.cancelCapture(target);
            target = -1;
        }
        if (target < 0) {
            m_syncFrameNoSlotCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Options assembled here, at the boundary, from the registered scalars:
        // the borrowed span tiers stay absent, because a registered span dies
        // before the boundary it would steer. The sync guarantee is about grid
        // content; the view-state tiers are the drive's present-time business.
        Rendering::TerminalSnapshotOptions options;
        options.viewOffset = m_syncCaptureGeometry.viewOffset;
        options.drawCursor = m_syncCaptureGeometry.drawCursor;
        options.isFocused = m_syncCaptureGeometry.isFocused;
        options.cursorMixAlpha = m_syncCaptureGeometry.cursorMixAlpha;
        options.shouldCursorInvertText = m_syncCaptureGeometry.shouldCursorInvertText;
        options.originInPixels = m_syncCaptureGeometry.originInPixels;
        options.viewportSizeInPixels = m_syncCaptureGeometry.viewportSizeInPixels;
        options.cellHeightPoints = m_syncCaptureGeometry.cellHeightPoints;
        options.lineSpacingPoints = m_syncCaptureGeometry.lineSpacingPoints;
        options.promptMarkFontSize = m_syncCaptureGeometry.promptMarkFontSize;
        options.promptMarkFontAscent = m_syncCaptureGeometry.promptMarkFontAscent;
        options.promptMarkFontDescent = m_syncCaptureGeometry.promptMarkFontDescent;
        options.promptMarkBracketInkTopInPixels = m_syncCaptureGeometry.promptMarkBracketInkTopInPixels;
        options.promptMarkBracketInkBottomInPixels = m_syncCaptureGeometry.promptMarkBracketInkBottomInPixels;
        options.promptMarkFontXHeight = m_syncCaptureGeometry.promptMarkFontXHeight;
        options.promptMarkFontCapHeight = m_syncCaptureGeometry.promptMarkFontCapHeight;
        options.promptMarkLineGapTopInPixels = m_syncCaptureGeometry.promptMarkLineGapTopInPixels;
        options.promptMarkAtlasScale = m_syncCaptureGeometry.promptMarkAtlasScale;
        options.style = m_syncCaptureGeometry.style;
        // After the style, which carries the bitmap tier's scale.
        Rendering::applyFrameRaster(options, m_syncCaptureGeometry.raster);
        // Read here rather than registered, for the reason the clock below is:
        // the preference outlives no frame, but a registration refreshed on
        // placement edges would hold the alpha a reader has since changed.
        // The boundary runs on the ingest thread under the lock, so it can
        // own its clock read the way the registration-time copy cannot: a
        // registered blink weight would hold the phase of whatever frame
        // last moved the placement.
        options.blinkTicks = frameTicksNow();
        options.shouldResolveBlink = true;
        options.cursorMixAlpha = Ubiquitty::Cursor::resolveAppearance(term, options.blinkTicks).blinkAlpha;
        options.cursorAlpha = cursorAlphaUnit();
        // The envelope measured at this capture's own tick, beside the blink
        // phase and for the same reason: a registered alpha would hold the
        // step whatever frame last moved the placement was drawn at, and the
        // flash would fade in jumps rather than run out.
        options.bellFlashAlpha = bellFlashAlphaAt(options.blinkTicks);
        options.shouldUnderlineLinks = m_syncCaptureGeometry.shouldUnderlineLinks;
        options.linkUnderlineStyle = m_syncCaptureGeometry.linkUnderlineStyle;
        options.linkColor = m_syncCaptureGeometry.linkColor;
        options.linkHoverColor = m_syncCaptureGeometry.linkHoverColor;
        // The run under the pointer, converted against the scroll position
        // this frame is drawing at rather than the one it was resolved from. Held beside the geometry rather than inside it because the
        // two change on different occasions: the placement moves when a window
        // is resized, and the hover moves when the pointer does, so a hover
        // folded into the placement registration would keep whatever run was
        // current the last time a window changed shape.
        Rendering::applyFrameHoverLink(options, m_syncFrameHover,
            Screen::scrollbackCount(term), options.viewOffset);
        // The status row rides the same borrow the capture reads under: the
        // scratch is session-owned because the options only borrow the cells,
        // and a boundary frame without this fill would drop the status line
        // the live route draws.
        if (Rendering::fillStatusRow(term, term->rows, term->columns, options.viewOffset,
                Rendering::StatusRowCells { m_statusRowScratch.data(), m_statusRowScratch.size() })) {
            options.statusRowCells = m_statusRowScratch.data();
            options.statusRowColumns = term->columns;
        }
        // The boundary capture plans its damage as the live route does: a frame
        // that changed one row records a Delta whose mask names it, and the
        // render Loads the retained grid for the rest. The plan's eligibility
        // (size, view offset, reverse video, palette epoch, images, a peer that
        // drained in between) falls back to Full. Rows of captures published
        // but not yet taken ride in the mask: a superseded slot strands nothing.
        Rendering::FrameDamage damage;
        Rendering::CapturedRowRefresh refresh;
        bool isDelta = false;
        if (term->grid) {
            Rendering::RetainedCaptureRequest request;
            request.rows = term->rows;
            request.columns = term->columns;
            request.viewOffset = options.viewOffset;
            request.shouldTrustDamage = true;
            // The search tiers are absent here as the selection is, and the
            // baseline this capture advances is the one the live route plans
            // against: it records the focused match as unpainted, so the live
            // frame that next draws its ring paints the fill under it too.
            request.isFocusedMatchBaked = false;
            const auto rowCount = static_cast<std::size_t>(std::max(term->rows, 0));
            if (m_syncRowDirty.size() != rowCount)
                m_syncRowDirty.assign(rowCount, 0);
            else
                std::ranges::fill(m_syncRowDirty, std::uint8_t { 0 });
            const Rendering::RetainedCapture plan = Rendering::planRetainedCapture(
                *term->grid, *term, m_retainedBaseline, request, std::span { m_syncRowDirty });
            damage = plan.damage;
            refresh = plan.refresh;
            isDelta = plan.damage.canReuse;
            // Owed rows are named where the capture that owed them drew them.
            // A scroll since has moved those rows, so a capture that scrolls
            // over a debt cannot say where it now stands, and records Full.
            if (isDelta && plan.damage.scrollDelta != 0 && hasOwedSyncRows(m_syncRowDirty.size())) {
                isDelta = false;
                damage.canReuse = false;
                refresh.canReuseRows = false;
            }
            if (isDelta) {
                mergeOwedSyncRows(m_syncRowDirty);
                oweSyncRows(m_syncRowDirty);
                // The blit is part of what this capture presents: superseded,
                // nobody performs it, and every row it moved is owed.
                if (plan.damage.scrollDelta != 0)
                    oweSyncRowRange(plan.damage.scrollTop, plan.damage.scrollBottom);
            } else {
                oweAllSyncRows();
            }
        } else {
            oweAllSyncRows();
        }
        Rendering::FrameRecordHeader header;
        header.sequence = m_nextSyncFrameSequence;
        header.baseSequence = isDelta ? m_lastSyncFrameSequence : 0;
        header.modelGeneration = term->modelEpoch;
        header.paletteGeneration = term->paletteEpoch;
        header.viewGeneration = m_syncViewGeneration;
        header.captureTicks = options.blinkTicks;
        header.kind = isDelta ? Rendering::FrameRecordKind::Delta : Rendering::FrameRecordKind::Full;
        if (!m_syncCaptures[target]->captureRecord(*term, options, m_syncCaptureGeometry.metrics,
                m_syncCaptureGeometry.glyphSlot, header, damage, refresh)) {
            m_syncSlot.cancelCapture(target);
            return;
        }
        m_lastSyncFrameSequence = header.sequence;
        m_nextSyncFrameSequence = m_nextSyncFrameSequence == std::numeric_limits<std::uint64_t>::max()
            ? 1
            : m_nextSyncFrameSequence + 1;
        m_syncFramePublicationCount.fetch_add(1, std::memory_order_relaxed);
        m_syncSlot.captured(target);
    }

    // Geometry the boundary capture reads; guarded by the model lock, written
    // by setSyncFrameCaptureGeometry and read by captureSyncFrame.
    SyncFrameCaptureGeometry m_syncCaptureGeometry;
    // Written only by setSyncFramePresentation, under the model lock; atomic so
    // that setter can see an unchanged value without taking the lock.
    std::atomic<bool> m_isSyncFramePresented { true };
    // The hovered run the boundary capture draws the emphasis over; same lock,
    // its own setter. Separate from the geometry because it changes on pointer
    // moves rather than on placement edges, and one field written on both
    // cadences is a field that holds the wrong one of them.
    Rendering::FrameHoverLink m_syncFrameHover;
    std::uint64_t m_syncViewGeneration = 0;
    std::uint64_t m_nextSyncFrameSequence = 1;

    // The boundary capture's status-row scratch: the capture borrows the
    // cells for the call only, and the ingest thread owns no other stable
    // cell storage to lend it.
    std::array<Cell, maxColumns> m_statusRowScratch {};

    // The pointer ladder's state -- the open selection gesture, the mode-1001
    // gesture, the motion-report dedup -- held here so a frontend needs no
    // bookkeeping of its own. Main-thread state.
    Ubiquitty::Input::PointerLadder m_pointerLadder;

    /// Frames one tmux command line onto the route's control channel; the
    /// server's echo is the reply.
    ///
    /// - Parameter command: The command, without the newline.
    void sendTmuxCommandLine(const std::string& command)
    {
        if (!m_tmuxPrimaryStream || command.empty())
            return;
        std::string line = command;
        line.push_back('\n');
        m_tmuxPrimaryStream->writeAll({ line.data(), line.size() });
    }

    // The tmux routing facts for a render-only pane: input wraps for this
    // pane and rides the primary's stream. Null stream means a local PTY.
    const Ubiquitty::Protocol::Tmux::TmuxClient* m_tmuxRoutingClient = nullptr;

    /// The frontend's measurer behind the client interface Core calls.
    struct CallbackTextMeasureClient final : Ubiquitty::TextMeasureClient {
        TextMeasureFunction measure = nullptr;
        void* context = nullptr;
        float cellAdvance = 0.0f;
        [[nodiscard]] int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle style, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
        {
            if (!measure)
                return 0;
            float advance = 0.0f;
            const int count = measure(context, codePoints.data(), static_cast<int>(codePoints.size()), style.faceBits, style.weight,
                glyphsOut.data(), static_cast<int>(glyphsOut.size()), &advance);
            if (count > 0 && advance > 0.0f)
                cellAdvance = advance;
            return count;
        }
        [[nodiscard]] float proportionalCellAdvanceInPixels() override { return cellAdvance; }
    };
    CallbackTextMeasureClient m_textMeasurer;
    RichText::Shaper m_richTextShaper;
    RichText::FontCatalogProvider m_fontCatalogProvider;
    std::uint64_t m_fontCatalogGeneration = 0;

    /// Tells a remote pane's host the faces this frontend's measurer sets
    /// proportional runs in, so the host answers the pane's programs in the
    /// face the runs are drawn in (Docs/proportional-text.md). A local pane
    /// measures through the measurer itself and publishes nothing.
    ///
    /// Main-thread only, after an attached driver has been acknowledged. The
    /// measurer is copied under the model lock and asked outside it, since
    /// every code point of four faces is shaped; only
    /// the main thread replaces it, so the context outlives the measuring. A
    /// measurer that cannot shape yet leaves the faces owed, for
    /// ``publishOwedProportionalFaces``.
    void publishProportionalFaces()
    {
        if (!m_remote || !m_remote->core || !m_remote->core->mayPublishProportionalFaces())
            return;
        CallbackTextMeasureClient measurer;
        MeasuredFacesKey key;
        {
            const std::scoped_lock hold(*m_activeLock);
            const Ubiquitty::Terminal* terminal = modelTerm();
            if (!terminal || !OSC::TextSizing::mayOfferFaceFacts(*terminal))
                return;
            measurer.measure = m_textMeasurer.measure;
            measurer.context = m_textMeasurer.context;
            key = { m_textMeasurer.measure, m_textMeasurer.context, m_typography.get() };
        }
        // The faces move with the measurer or the fonts a typography table is
        // published for, so the same three measure once.
        if (!measurer.measure || key == m_measuredFacesKey)
            return;
        auto faces = std::unique_ptr<OSC::TextSizing::ProportionalFaceFacts>(new (std::nothrow) OSC::TextSizing::ProportionalFaceFacts());
        if (!faces || !OSC::TextSizing::measureProportionalFaces(measurer, *faces)) {
            m_areProportionalFacesOwed = true;
            return;
        }
        m_areProportionalFacesOwed = false;
        m_measuredFacesKey = key;
        m_remote->core->publishProportionalFaces(*faces);
    }

    /// What the run faces last sent to the remote core were measured from:
    /// the measurer and the typography table published for its fonts. A new
    /// remote core clears it, so the faces reach every core.
    struct MeasuredFacesKey {
        TextMeasureFunction measure = nullptr;
        void* context = nullptr;
        const RichText::TypographyFacts* typography = nullptr;

        friend bool operator==(const MeasuredFacesKey&, const MeasuredFacesKey&) = default;
    };
    MeasuredFacesKey m_measuredFacesKey;
    /// The measurer could not shape when the faces were last due; main-thread
    /// state.
    bool m_areProportionalFacesOwed = false;

    /// Points a terminal at the session's measurer, or at nothing when none
    /// is set.
    ///
    /// The caller holds the model lock.
    ///
    /// - Parameter terminal: The model terminal the session holds.
    void installTextMeasurer(Ubiquitty::Terminal& terminal)
    {
        terminal.textMeasureClient = m_textMeasurer.measure ? Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&m_textMeasurer)
                                                            : Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(nullptr);
        if (m_typography)
            RichText::adoptTypography(terminal, m_typography);
        terminal.typography.shaper = m_richTextShaper;
        terminal.typography.fontCatalog = m_fontCatalogProvider;
    }

    /// The frontend's typography table, adopted by every model terminal the
    /// session holds.
    std::shared_ptr<const RichText::TypographyFacts> m_typography;
    /// This surface's metrics, not pixels borrowed from a remote checkpoint.
    std::optional<PixelGeometry> m_pixelGeometry;

    /// Buffers the window verbs so a frontend can answer them off the drain.
    ///
    /// Every method is called by the parser, inline, with the model lock held,
    /// so each does the least that can be done: append and mark. The drain that
    /// follows raises the event bit and wakes whoever is listening.
    struct BufferingWindowClient final : Ubiquitty::WindowClient {
        TerminalSession* session = nullptr;

        /// Records a font request for the pull-based frontend's next delivery.
        ///
        /// The parser holds the model lock and has authorized the request.
        /// Direct feeds and ingest drains publish the snapshot before raising
        /// the event, so a poll cannot consume it with a stale generation.
        ///
        /// - Parameters:
        ///   - family: The requested family, or empty for a size change or reset.
        ///   - sizePoints: The absolute point size, or zero.
        ///   - sizeDelta: The relative point-size change, or zero.
        [[nodiscard]] bool setFont(std::string_view family, double sizePoints, double sizeDelta) override
        {
            if (!session) {
                return false;
            }
            Ubiquitty::Terminal* term = session->modelTerm();
            if (!term) {
                return false;
            }
            return Ubiquitty::recordFontRequest(*term, family, sizePoints, sizeDelta);
        }

        /// Records mintty's return to the configured size for the pull-based
        /// frontend's next delivery, the family kept.
        ///
        /// The parser holds the model lock and has authorized the request.
        [[nodiscard]] bool resetFontSize() override
        {
            if (!session) {
                return false;
            }
            Ubiquitty::Terminal* term = session->modelTerm();
            if (!term) {
                return false;
            }
            return Ubiquitty::recordFontSizeReset(*term);
        }

        /// Queues CSI 8 t, whose operation number carries the counts.
        ///
        /// Rows first, which is the order `CSI 8 ; rows ; columns t` is written
        /// in and the order `CSI.cpp` forwards. The base declares both
        /// parameters unnamed, so nothing but the call site says which is
        /// which — a frontend that guesses gets a window the wrong way round.
        ///
        /// - Parameters:
        ///   - rows: The rows asked for.
        ///   - columns: The columns asked for.
        void resize(int rows, int columns) override
        {
            push(WindowRequest { WindowRequest::Kind::WindowOp, 8, rows, columns, {} });
        }

        /// Queues DECCOLM's column change.
        ///
        /// - Parameter isColumns132: Whether the new width is 132.
        void setColumnMode(bool isColumns132) override // bool-ok: DECCOLM's own two-valued parameter
        {
            push(WindowRequest { WindowRequest::Kind::ColumnMode, isColumns132 ? 1 : 0, 0, 0, {} });
        }

        /// Queues one CSI t window operation.
        ///
        /// - Parameters:
        ///   - operation: The `Ps` the program wrote.
        ///   - first: Its first argument.
        ///   - second: Its second argument.
        void performWindowOperation(int operation, int first, int second) override
        {
            push(WindowRequest { WindowRequest::Kind::WindowOp, operation, first, second, {} });
        }

        /// Queues a request for the human's attention.
        ///
        /// - Parameter request: How insistently to ask.
        void requestAttention(Ubiquitty::AttentionRequest request) override
        {
            push(WindowRequest { WindowRequest::Kind::Attention, static_cast<int>(request), 0, 0, {} });
        }

        /// Queues OSC 1337 StealFocus.
        void stealFocus() override { push(WindowRequest { WindowRequest::Kind::StealFocus, 0, 0, 0, {} }); }

        /// Queues OSC 1337 SetColors=preset.
        ///
        /// - Parameter name: The theme the program named.
        void selectColorPreset(std::string_view name) override
        {
            push(WindowRequest { WindowRequest::Kind::ColorPreset, 0, 0, 0, std::string(name) });
        }

    private:
        /// Folds a geometry request into the one of its kind queued last.
        ///
        /// A CSI 3, 4 or 8 t arriving behind one of its own kind replaces it:
        /// the window the program asked for is where the run ends, and every
        /// frame change before that is one the frontend would lay out, redraw
        /// and reallocate a drawable for and the program never sees. A kept
        /// axis — an omitted parameter of CSI 4 or 8 t — keeps what the
        /// earlier request named, which is what performing both would have
        /// left. Every other operation keeps its place: CSI 9 t's modes
        /// compose, and a raise or an iconify between two resizes is a
        /// different program.
        ///
        /// - Parameters:
        ///   - request: What the application asked for.
        ///   - queue: The requests not yet taken.
        /// - Returns: Whether the queue's last request now carries it.
        [[nodiscard]] static bool foldIntoQueuedPredecessor(const WindowRequest& request, std::vector<WindowRequest>& queue) // bool-ok: reports what happened, not a mode chosen at the call site
        {
            if (request.kind != WindowRequest::Kind::WindowOp || queue.empty())
                return false;
            WindowRequest& last = queue.back();
            if (last.kind != WindowRequest::Kind::WindowOp || last.first != request.first)
                return false;
            switch (request.first) {
            case 3:
                // A move has no keep-this-axis form: the later point wins whole.
                last.second = request.second;
                last.third = request.third;
                return true;
            case 4:
            case 8:
                // -1 is an omitted axis, which keeps the earlier request's.
                if (request.second >= 0)
                    last.second = request.second;
                if (request.third >= 0)
                    last.third = request.third;
                return true;
            default:
                return false;
            }
        }

        /// Appends unless the queue is full, or the request folds into the
        /// one before it.
        ///
        /// Bounded, because a program can spin these as fast as it can write
        /// bytes and nothing on the far side promises to drain at that rate.
        /// The NEW one is dropped rather than the oldest: a window operation
        /// nobody performed is better than one performed out of order, and the
        /// program that overran learns nothing either way. A run of resizes
        /// never reaches the bound: it folds into the last, so the size a
        /// program asked for is the one the frontend performs.
        ///
        /// - Parameter request: What the application asked for.
        void push(WindowRequest&& request)
        {
            if (!session)
                return;
            std::vector<WindowRequest>& queue = session->m_windowRequests;
            // Folded under the lock the drain takes, so a request that
            // was already taken is not folded into: the queue is then empty
            // and this one is appended and announced on its own.
            if (foldIntoQueuedPredecessor(request, queue))
                return;
            if (queue.size() >= windowRequestQueueLimit)
                return;
            queue.push_back(std::move(request));
            // The bit is raised here rather than in the drain's deliver hook,
            // because that hook fires only for the loop's own passes and bytes
            // also arrive through `feedBytes`. A request that reached the queue
            // must be reported however it got there. No signal from here: the
            // model lock is held, and every path that fills this queue is a
            // path that also drains bytes, whose own report wakes the frontend.
            session->m_events.pending.fetch_or(
                1u << static_cast<unsigned>(EventKind::WindowRequestPending), std::memory_order_release);
        }
    };
    BufferingWindowClient m_windowClient;
    std::vector<WindowRequest> m_windowRequests;

    /// Buffers the desktop notifications so a frontend can post them off the
    /// drain.
    ///
    /// Called by the parser, inline, with the model lock held, past the
    /// `DesktopNotification` capability gate that ``OSC::dispatchNotification``
    /// applies — so this records what the host's policy already admitted and
    /// decides nothing itself beyond the display promotion and the bound.
    struct BufferingShellClient final : Ubiquitty::ShellClient {
        TerminalSession* session = nullptr;

        /// Records one banner for the frontend's next drain.
        ///
        /// The pair is resolved here rather than at the far end so every
        /// frontend on this path posts the same heading for a body-only
        /// dialect, and a wholly empty pair is dropped where the policy that
        /// calls it empty lives.
        ///
        /// - Parameter request: The notification as parsed: its title, empty
        ///   for OSC 9, its body, empty for a title-only OSC 777, and what an
        ///   OSC 99 said of its name and occasion.
        void postNotificationRequest(const NotificationRequest& request) override
        {
            if (!session)
                return;
            OSC::NotificationDisplay display = OSC::resolveNotificationDisplay(request.title, request.body);
            if (!display.shouldPost)
                return;
            // A repeat of the banner already at the tail is the same banner,
            // so it folds onto the one standing rather than queueing twice: a
            // program in a loop emits one sequence over and over, and a queue
            // of eight copies of it costs the reader eight banners to dismiss.
            // The rate limit the backend applies is per window and per second;
            // this is per queue and exact, and the two bound different things.
            if (!session->m_notifications.empty() && session->m_notifications.back().title == display.title
                && session->m_notifications.back().body == display.body && session->m_notifications.back().identifier == request.identifier)
                return;
            if (session->m_notifications.size() >= notificationQueueLimit)
                return; // the new one is dropped, as the window queue drops its overrun
            session->m_notifications.push_back(
                Notification { std::move(display.title), std::move(display.body), std::string(request.identifier), request.occasion, true });
            // Raised here rather than in the drain's deliver hook, for the
            // reason the window queue gives: bytes also arrive through
            // `feedBytes`, and a banner that reached the queue must be
            // reported however it got there. No signal from here — the model
            // lock is held, and every path that fills this queue drains bytes
            // whose own report wakes the frontend.
            session->m_events.pending.fetch_or(
                1u << static_cast<unsigned>(EventKind::NotificationPosted), std::memory_order_release);
        }
    };
    BufferingShellClient m_shellClient;
    DragDrop::Queue m_dragDropQueue;
    std::uint64_t m_nativeDragSession = 0;
    std::uint64_t m_remoteDragEpoch = 0;
    bool m_supportsNativeDrag = false;
    std::vector<Notification> m_notifications;

    /// One matched trigger, as the engine reported it.
    ///
    /// Raw rather than resolved: the gates it passes are read when it is
    /// taken, on the frontend's loop — see ``takeTriggerEffect``.
    struct TriggerMatch {
        std::uint8_t action = 0;
        std::string argument;
        std::string matchedText;
    };

    /// Buffers the trigger reports so a frontend can act on them off the drain.
    ///
    /// Called by the line-commit hook, inline, with the model lock held, past
    /// the `ExecuteTrigger` capability gate the hook applies to `run_command`
    /// — so this records what the host's policy already admitted and decides
    /// nothing beyond which actions dispatch at all and the bound.
    struct BufferingTriggerClient final : Ubiquitty::TriggerClient {
        TerminalSession* session = nullptr;

        /// Records one report for the frontend's next drain.
        ///
        /// - Parameters:
        ///   - action: An `Ubiquitty::Triggers::Action`, widened.
        ///   - argument: The rule's payload.
        ///   - matchedText: The matched span, copied: it points into a stack
        ///     buffer the hook reuses for the next line.
        void onTrigger(std::uint8_t action, std::string_view argument, std::string_view matchedText, int /*column0*/, int /*column1*/) override
        {
            if (!session || !doesTriggerActionDispatch(action))
                return;
            if (session->m_triggerMatches.size() >= triggerQueueLimit)
                return; // the new one is dropped, as the notification queue drops its overrun
            session->m_triggerMatches.push_back(
                TriggerMatch { action, std::string(argument), std::string(matchedText) });
            // Raised here rather than in the drain's deliver hook, for the
            // reason the window queue gives: bytes also arrive through
            // `feedBytes`, and a report that reached the queue must be
            // announced however it got there.
            session->m_events.pending.fetch_or(
                1u << static_cast<unsigned>(EventKind::TriggerMatched), std::memory_order_release);
        }
    };
    BufferingTriggerClient m_triggerClient;
    std::vector<TriggerMatch> m_triggerMatches;

    // The frontend loop every hosting session posts to. Written once at
    // launch by ``enableHostedWindows``, before the factory is published with
    // release ordering; read after acquiring it.
    static inline PostedWorkLoop s_hostedWindowLoop {};
    static inline std::atomic<HostedWindowSourceFactory> s_hostedWindowSourceFactory { nullptr };

    // The windows this session's program hosts. Frontend loop only: made
    // there, drained there, freed there.
    std::unique_ptr<HostedWindowSource> m_hostedWindows;
    std::string m_tmuxSSHHost;
    // Whether the last process walk found a hosting program in the
    // foreground. Ingest thread only.
    bool m_wasForegroundHostingProgram = false;
    // An adopted pane's hosting source, for its resize and client-size
    // report; null for every other session.
    HostedWindowSource* m_hostedPaneSource = nullptr;
    int m_hostedPaneID = -1;

    Ubiquitty::PAL::Stream* m_tmuxPrimaryStream = nullptr;
    int m_tmuxRoutingPaneID = 0;
    LocalPaneLink m_localPaneLink { *this };
    WeakPtr<Session::PaneLinkObserver> m_paneLinkObserver;
    std::optional<Session::FrameReady> m_lastPaneLinkFrame;
    PaneEventObserver m_paneEventObserver { *this };

    /// The thin client's whole seam set, owned together: the dispatcher
    /// over the platform's loop, the plain subprocess transport, and the
    /// observer that folds remote events into the session's own flag
    /// surface — so the frontend's event pump cannot tell a host's screen
    /// frame from a local drain, which is the point.
    struct RemoteParts {
        using PostedDispatcher = Runtime::PostedDispatcher;

        /// The plain transport: the attach command on a subprocess channel.
        struct PlainChannelFactory final : Ubiquitty::Session::ChannelFactory {
            /// The transport command the channel spawns.
            std::string shellCommand;
            /// The host a QUIC carrier dials, empty when QUIC is not offered.
            std::string quicHost;
            /// The command that prints the daemon's QUIC port, token and cert
            /// pin; empty when QUIC is not offered.
            std::string quicBootstrapCommand;
            /// Whether a bootstrap has already failed this session. QUIC is
            /// tried once: a host that could not produce a pin will not
            /// produce one on the next reconnect either, and retrying it per
            /// attempt would pay the subprocess on every reconnect of a
            /// session that has already answered.
            ///
            /// A host install is the one event that makes the answer stale,
            /// and it is exactly the case the latch would otherwise get
            /// wrong: the bootstrap runs the remote binary, so a host with
            /// none fails it, falls back to ssh, and then installs the very
            /// binary that would have answered. Clearing it there is what
            /// lets the reconnect the install triggers dial QUIC.
            bool didQUICBootstrapFail = false;
            /// Why the latch above was set, for a person to read; empty while
            /// QUIC has not fallen back.
            std::string m_quicFallbackReason;
            /// The typed reason the last attempt produced no carrier, set
            /// before its null completion and cleared by the next attempt.
            std::optional<Ubiquitty::Session::ChannelCreationFailure> m_creationFailure;
            /// Posts bootstrap completion to the factory's owning event loop.
            PostedDispatcher dispatcher;

            /// Retries bootstrap after installing the executable it invokes.
            /// The daemon the install starts is another one, so the reach
            /// kept for the old one goes too.
            void hostDeploymentFinished() override
            {
                didQUICBootstrapFail = false;
                m_quicFallbackReason.clear();
                m_quicReach.reset();
            }

            /// Whether a bootstrap's QUIC carrier reached the daemon, so the
            /// carrier can be dialled again with no ssh run.
            [[nodiscard]] bool canResumeChannel() const override { return m_quicReach.has_value(); }

            /// Dials the reach the last bootstrap recorded, presenting a
            /// resume key in place of a token. Nothing is spawned.
            ///
            /// - Parameters:
            ///   - key: What the carrier presents.
            ///   - done: Receives the carrier, or null with no reach recorded.
            void resumeChannel(Ubiquitty::Session::ResumeKey key, Done done) override
            {
#if defined(UBIQUITTY_QUIC)
                if (m_quicReach) {
                    done(std::make_unique<Ubiquitty::Transport::QUICChannel>(m_quicReach->address, m_quicReach->port,
                             Ubiquitty::Transport::ExportedPreamble { .exporterLabel = std::move(key.exporterLabel), .build = std::move(key.buildPreamble) },
                             m_quicReach->certificateSHA256, Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::Daemon, "" }),
                        true);
                    return;
                }
#else
                (void)key;
#endif
                done(nullptr, false);
            }

            /// Why QUIC fell back to the stream carrier, empty while it has
            /// not.
            ///
            /// - Returns: The reason.
            [[nodiscard]] std::string quicFallbackReason() const override { return m_quicFallbackReason; }

            /// Why the last null completion produced no carrier, when the
            /// bootstrap said.
            ///
            /// - Returns: The failure, or `nullopt` for an untyped one.
            [[nodiscard]] std::optional<Ubiquitty::Session::ChannelCreationFailure> creationFailure() const override { return m_creationFailure; }

            /// The reach a resume dials, for the frontend to keep.
            ///
            /// - Returns: The reach, or `nullopt` before a bootstrap reached QUIC.
            [[nodiscard]] std::optional<Ubiquitty::Session::ResumeReach> resumeReach() const override
            {
                if (!m_quicReach || m_quicReach->certificateSHA256.size() != 32)
                    return std::nullopt;
                Ubiquitty::Session::ResumeReach reach { .address = m_quicReach->address, .port = m_quicReach->port, .certificatePin = {} };
                std::ranges::copy(m_quicReach->certificateSHA256, reach.certificatePin.begin());
                return reach;
            }

            /// Adopts the reach a previous process kept, so the first carrier
            /// of a restored attachment is a resume proof and runs no login.
            ///
            /// - Parameter reach: A numeric address, a port and a nonzero pin.
            /// - Returns: Whether it was adopted; never over a reach of its own.
            bool seedResumeReach(const Ubiquitty::Session::ResumeReach& reach) override
            {
                if (m_quicReach || reach.port == 0 || !Session::isNumericAddress(reach.address)
                    || std::ranges::all_of(reach.certificatePin, [](std::uint8_t byte) { return byte == 0; }))
                    return false;
                m_quicReach = QUICReach { .address = reach.address, .port = reach.port, .certificateSHA256 = std::string(reinterpret_cast<const char*>(reach.certificatePin.data()), reach.certificatePin.size()) };
                return true;
            }

            /// Pins both carrier commands to the daemon that accepted the workspace.
            ///
            /// - Parameter socketName: The validated handshake's daemon basename.
            /// - Returns: Whether the identity is valid and agrees with an existing pin.
            bool pinDaemonSocket(std::string_view socketName) override
            {
                if (socketName.empty()) {
                    return m_daemonSocketName.empty();
                }
                if (!Session::isDaemonSocketName(socketName) || (!m_daemonSocketName.empty() && m_daemonSocketName != socketName)) {
                    return false;
                }
                m_daemonSocketName = socketName;
                shellCommand = Session::substituteReattachSocket(shellCommand, socketName);
                quicBootstrapCommand = Session::substituteReattachSocket(quicBootstrapCommand, socketName);
                return true;
            }

            /// A planned successor is discovered through the unpinned bootstrap.
            /// The session retains the predecessor identity in its resume proof.
            void clearHandoverReach() override
            {
                shellCommand = Session::clearReattachSocket(shellCommand, m_daemonSocketName);
                quicBootstrapCommand = Session::clearReattachSocket(quicBootstrapCommand, m_daemonSocketName);
                m_daemonSocketName.clear();
                m_quicReach.reset();
                didQUICBootstrapFail = false;
                m_quicFallbackReason.clear();
            }

            /// Preserves daemon identity across carrier replacement and host binary updates.
            std::string m_daemonSocketName;

            /// Cancels bootstrap work before releasing its completion target.
            ~PlainChannelFactory() override
            {
                m_lifetime.reset();
                m_bootstrapThread.request_stop();
                if (m_bootstrapWaker)
                    m_bootstrapWaker->wake();
                if (m_bootstrapThread.joinable())
                    m_bootstrapThread.join();
            }

            /// Opens the transport and hands it to the core.
            ///
            /// QUIC first when it is offered and has not already failed:
            /// the bootstrap runs, its line is parsed by the same Core
            /// grammar the daemon writes, and the pinned carrier is built.
            /// Anything missing — a failed run, an unparseable line, no cert
            /// pin — falls back to the subprocess carrier, because an
            /// unpinned token is stealable in the middle.
            ///
            /// - Parameters:
            ///   - transport: TCP once QUIC has failed this session.
            ///   - done: Receives the opened channel.
            void createChannel(Ubiquitty::Session::TransportMode transport, Done done) override
            {
                m_creationFailure.reset();
                const Ubiquitty::Transport::Origin origin { Ubiquitty::Transport::OriginKind::Daemon, "" };
#if defined(UBIQUITTY_QUIC)
                // Only a build carrying the QUIC carrier can offer one. A
                // build without it still accepts the host and the bootstrap
                // command — the caller does not have to know which build it
                // is talking to — and reaches the subprocess carrier, which
                // is the fallback that path already documents.
                if (transport == Ubiquitty::Session::TransportMode::QUIC && !didQUICBootstrapFail
                    && !quicHost.empty() && !quicBootstrapCommand.empty()) {
                    m_bootstrapThread.request_stop();
                    if (m_bootstrapWaker)
                        m_bootstrapWaker->wake();
                    if (m_bootstrapThread.joinable())
                        m_bootstrapThread.join();
                    m_bootstrapWaker = PAL::makeWaker();
                    if (m_bootstrapWaker) {
                        auto weakLifetime = std::weak_ptr<int>(m_lifetime);
                        m_bootstrapThread = std::jthread([this, weakLifetime, completionDispatcher = dispatcher,
                                                             command = quicBootstrapCommand, host = quicHost, fallback = shellCommand,
                                                             done = std::move(done), origin](std::stop_token stop) mutable {
                            Session::QUICBootstrapRun ran = Session::runQUICBootstrap(command, host, &sharedConnectionReaches(), stop, *m_bootstrapWaker);
                            auto answer = std::make_unique<BootstrapAnswer>(BootstrapAnswer { .run = std::move(ran),
                                .isCancelled = stop.stop_requested(),
                                .fallback = std::move(fallback),
                                .done = std::move(done),
                                .origin = origin });
                            completionDispatcher.post([this, weakLifetime, answer = std::move(answer)] {
                                auto& [run, isCancelled, fallback, done, origin] = *answer;
                                if (isCancelled || weakLifetime.expired()) {
                                    done(nullptr, false);
                                    return;
                                }
                                switch (run.read) {
                                case Session::QUICBootstrapRead::Answered:
                                    // The address the bootstrap's own connection reached,
                                    // never the host's name resolved again: the token and
                                    // the pin are that machine's.
                                    m_quicReach = QUICReach { .address = run.info.reachedAddress, .port = run.info.port, .certificateSHA256 = run.info.certificateSHA256 };
                                    done(std::make_unique<Ubiquitty::Transport::QUICChannel>(run.info.reachedAddress, run.info.port,
                                             run.info.token, run.info.certificateSHA256, origin),
                                        true);
                                    return;
                                case Session::QUICBootstrapRead::Unreached:
                                    // Nothing was declined, so nothing is latched: no channel
                                    // sends the session to Core's backoff, whose next attempt
                                    // asks for QUIC again. The ssh fallback would dial the
                                    // same unreachable host.
                                    m_quicFallbackReason = run.refusal;
                                    done(nullptr, false);
                                    return;
                                case Session::QUICBootstrapRead::Declined:
                                    didQUICBootstrapFail = true;
                                    m_quicFallbackReason = run.refusal;
                                    done(std::make_unique<Ubiquitty::Transport::SubprocessChannel>(fallback, origin), false);
                                    return;
                                case Session::QUICBootstrapRead::InstanceGone:
                                    // The stream is pinned to the same daemon, so it is
                                    // not tried: the session drops the pin.
                                    m_quicFallbackReason = run.refusal;
                                    m_creationFailure = Session::ChannelCreationFailure { .recovery = Session::ChannelFailureRecovery::InstanceGone, .detail = run.refusal };
                                    done(nullptr, false);
                                    return;
                                }
                            });
                        });
                        return;
                    }
                    didQUICBootstrapFail = true;
                    m_quicFallbackReason = "this device couldn’t wait for the host to offer QUIC";
                }
#else
                if (transport == Ubiquitty::Session::TransportMode::QUIC && !quicHost.empty())
                    m_quicFallbackReason = "this version of Ubiquitty doesn’t include QUIC";
#endif
                done(std::make_unique<Ubiquitty::Transport::SubprocessChannel>(shellCommand, origin), false);
            }

        private:
            /// Where a bootstrap's QUIC carrier reached the daemon: the
            /// address ssh reached, the daemon's port and its certificate
            /// pin. A resume dials it again with no bootstrap.
            struct QUICReach {
                std::string address;
                std::uint16_t port = 0;
                std::string certificateSHA256;
            };

            /// What a finished bootstrap hands back to the session thread.
            /// Boxed when posted: it is larger than a posted unit holds.
            struct BootstrapAnswer {
                Session::QUICBootstrapRun run;
                bool isCancelled = false;
                std::string fallback;
                Done done;
                Ubiquitty::Transport::Origin origin;
            };

            std::optional<QUICReach> m_quicReach;
            std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
            std::unique_ptr<PAL::Waker> m_bootstrapWaker;
            std::jthread m_bootstrapThread;
        };

        /// What a remote connect may install, and how the answer arrives.
        ///
        /// The decision is Core's (``Session::decideDeployAction``) and the
        /// push is a shell command; the one thing neither can supply is the
        /// user's approval the first time a host is pushed to, which the
        /// frontend hears as ``EventKind::DeployConfirmationPending``.
        struct DeployParts {
            /// The ssh host a push targets; empty offers no deploy.
            std::string sshHost;
            /// The bundled binary for the remote's `uname -sm`, by that key.
            /// A platform absent here gets the manual-install answer.
            std::map<std::string, std::string> binaryForPlatform;
            /// Whether config allows an automatic install at all.
            bool isAutoInstallAllowed = false;
            /// The binary a pending confirmation would push, held between
            /// the ask and the answer.
            std::string pendingBinary;
            /// Whether a push is running, so the Failed state that ends it is
            /// reported as the install failing rather than as the host going
            /// away.
            bool isPushInFlight = false;
            /// The `uname -sm` the deploy request named, empty when it named
            /// none.
            std::string requestedPlatform;
            /// The sessions an update would hand over, while the person is asked
            /// about them; 0 otherwise.
            std::size_t pendingLiveSessionCount = 0;
            /// Whether the last install ended waiting for sessions rather than
            /// failing; see ``lastDeployWasDeferred``.
            bool wasDeferred = false;
            /// Expires with these parts. A probe or install finishing on its
            /// own thread posts back only while it is alive, and the
            /// dispatcher it posts to is the thread that frees them.
            std::shared_ptr<int> lifetime = std::make_shared<int>(0);
            /// What the last failed push printed, tail first; see
            /// ``lastDeployFailure``.
            std::string lastFailure;
            /// The remote's `uname -sm` for the last request no automatic
            /// install could answer; see ``manualHostInstallCommands``.
            std::string manualInstallPlatform;
            /// Whether that request went unanswered because automatic install
            /// is turned off, rather than because no bundled binary serves the
            /// platform.
            bool isManualInstallForSetting = false;
        };

        /// Folds the remote events into the session's own flag surface.
        struct EventObserver final : Ubiquitty::Session::RemoteSessionObserver {
            /// The session whose flags the events land in.
            TerminalSession* session = nullptr;
            /// The screen plane rewrote the model — republish and flag dirty.
            void onScreenChanged() override { session->noteRemoteScreenChanged(); }

            void onLinkState(const Protocol::Sync::LinkState&) override { session->noteRemoteEvent(EventKind::LinkStateChanged); }

            /// Input went in doubt, or was decided: the frontend reads the
            /// status and offers the choice, since what the person types
            /// waits until they make it.
            void onInDoubt(const Protocol::Sync::PaneInputStatus&) override { session->noteRemoteEvent(EventKind::InputInDoubtChanged); }

            /// The host has no `ubiquitty-host`: decide, and either push,
            /// ask, or answer that nothing will be installed.
            ///
            /// - Parameter platformArch: The remote's `uname -sm`.
            void onDeployRequested(const std::string& platformArch) override { session->beginDeploy(platformArch); }

            /// The canonical record's program title changed.
            ///
            /// - Parameter utf8: The title bytes.
            void onTitle(std::string_view utf8) override { session->applyRemoteTitle(utf8); }

            /// The host sent the title's INPUTS rather than a finished title,
            /// so this frontend spells it in its own voice.
            ///
            /// The canonical record is whole: empty fields clear the previous
            /// record rather than waiting for a separate projection.
            ///
            /// - Parameter parts: The host's complete pane record.
            void onPaneFacts(const Ubiquitty::Protocol::Sync::PaneFacts& parts) override
            {
                session->applyRemoteTitleParts(parts);
            }

            /// A BELL frame arrived — flag it with the audible style.
            /// A bell from the far side.
            ///
            /// Carried at the reader's own cue rather than as a bare beep:
            /// the mode is the local terminal's, seeded from the
            /// configuration, so a remote bell obeys the same `[bell]`
            /// switches a local one does. A hard-coded audible cue here was a
            /// second bell policy, and it rang against a visual-only
            /// preference.
            void onBell() override
            {
                session->noteRemoteEvent(
                    EventKind::Bell, static_cast<std::uint32_t>(session->configuredBellMode()));
            }

            /// The host shell exited — flag it; the frontend closes.
            void onExit() override { session->noteHostedSessionEnded(); }

            /// The daemon handed back a session this one shared.
            void onReclaimed() override { session->noteReclaimed(); }

            /// The pane's connection retires into another of its workspace;
            /// the pane moves once this turn ends, when nothing it started is
            /// open.
            void onConnectionRetiring() override { session->scheduleConnectionMove(0); }

            /// Reports workspace publication through the pane's connection events.
            void onSharedWorkspaceChanged() override
            {
                session->noteRemoteEvent(EventKind::RemoteStateChanged, static_cast<std::uint32_t>(session->remoteState()));
            }

            /// A pushed paste settled — keep the path and flag the status.
            ///
            /// The path is held on the session because the event carries
            /// scalars only, and it is what the frontend names once the file
            /// exists.
            ///
            /// - Parameter ack: The host's answer.
            void onPastePushAck(const Ubiquitty::Protocol::Control::PastePushReply& ack) override
            {
                session->m_lastPastePushPath = ack.path;
                session->noteRemoteEvent(EventKind::PastePushSettled, static_cast<std::uint32_t>(ack.status));
            }

            /// Publishes the host acknowledgement through the connection event hook.
            void onClipboardRoutingChanged() override
            {
                session->noteRemoteEvent(EventKind::RemoteStateChanged, static_cast<std::uint32_t>(session->m_remote->core->state()));
            }

            /// A program on the host asked for a clipboard its machine does not
            /// have, and this one's is the answer.
            ///
            /// Answered here rather than reported to the frontend: the decision
            /// is portable and lives in ``Session::answerClipboardRequest``,
            /// and the only platform part — reaching a pasteboard — is already
            /// behind the ClipboardClient seam. A frontend that installs a
            /// clipboard gets this without writing any of it.
            ///
            /// - Parameter request: What was asked, and the id to answer with.
            void onClipboardRequest(const Ubiquitty::Protocol::Sync::ClipboardRequestMessage& request) override
            {
                session->answerHostClipboardRequest(request);
            }

            /// Reports the frontend's native-adapter availability.
            /// - Returns: Whether the connection may advertise native drag support.
            bool supportsNativeDrag() const override { return session->m_supportsNativeDrag; }

            /// Copies a remote command into the bounded native queue.
            /// - Parameter message: The command and its owner epoch.
            void onNativeDragCommand(const Protocol::Sync::NativeDragCommand& message) override
            {
                bool didQueue = false;
                {
                    const std::scoped_lock hold(*session->m_activeLock);
                    if (message.epoch != session->m_remoteDragEpoch) {
                        session->m_dragDropQueue.clear();
                        DragDrop::Metadata stop;
                        stop.type = DragDrop::Type::StopAccepting;
                        (void)session->m_dragDropQueue.sendCommand({ .kind = DragDrop::CommandKind::Registration, .metadata = stop });
                        stop.type = DragDrop::Type::Offer;
                        stop.x = 2;
                        (void)session->m_dragDropQueue.sendCommand({ .kind = DragDrop::CommandKind::Registration, .metadata = stop });
                        session->m_remoteDragEpoch = message.epoch;
                    }
                    const auto& command = message.command;
                    didQueue = session->m_dragDropQueue.sendCommand({ .kind = command.kind, .sessionID = command.sessionID, .requestID = command.requestID, .metadata = command.metadata, .payload = { command.bytes.data(), command.bytes.size() }, .isFinal = command.isFinal });
                }
                if (!didQueue) {
                    session->cancelDragDrop();
                    const std::scoped_lock hold(*session->m_activeLock);
                    session->m_dragDropQueue.clear();
                    DragDrop::Metadata stop;
                    stop.type = DragDrop::Type::StopAccepting;
                    (void)session->m_dragDropQueue.sendCommand({ .kind = DragDrop::CommandKind::Registration, .metadata = stop });
                    stop.type = DragDrop::Type::Offer;
                    stop.x = 2;
                    (void)session->m_dragDropQueue.sendCommand({ .kind = DragDrop::CommandKind::Registration, .metadata = stop });
                }
                session->noteRemoteEvent(EventKind::DragDropPending);
            }

            /// Applies exact borrowed typed data through the terminal's route.
            ///
            /// - Parameters:
            ///   - buffer: The original clipboard selector.
            ///   - items: Complete representations valid through this call.
            void onClipboardWriteItems(ClipboardBuffer buffer, std::span<const ClipboardItem> items) override
            {
                session->applyRemoteClipboardWriteItems(buffer, items);
            }

            /// Preserves the host notification's replacement and presentation condition.
            ///
            /// - Parameter request: The admitted borrowed notification.
            void onNotificationRequest(const NotificationRequest& request) override
            {
                session->applyRemoteNotificationRequest(request);
            }

            /// Asks the person about an effect this client's own policy holds.
            ///
            /// - Parameters:
            ///   - token: The held effect.
            ///   - capability: What it would do.
            void onHostEffectHeld(std::uint64_t token, Capability capability) override
            {
                session->noteHostEffectHeld(token, capability);
            }

            /// Closes the question for a held effect that left unanswered.
            ///
            /// - Parameters:
            ///   - token: The withdrawn effect.
            ///   - reason: Whether a newer one replaced it or its connection ended.
            void onHostEffectWithdrawn(std::uint64_t token, Protocol::Sync::HostEffectWithdrawal reason) override
            {
                session->noteHostEffectWithdrawn(token, reason);
            }

            /// Requests a single attention cue under the local effect policy.
            void onAttention() override
            {
                const std::scoped_lock hold(*session->m_activeLock);
                if (auto* term = session->modelTerm()) {
                    if (auto* window = effectClient(*term, term->windowClient, Capability::RequestAttention))
                        window->requestAttention(AttentionRequest::Once);
                }
            }

            /// The connection moved between connecting, connected, dropped and
            /// gone.
            ///
            /// The detail string is dropped rather than carried: an event is
            /// scalars only, and the frontend that wants the reason reads
            /// ``TerminalSession/lastRemoteFailure`` beside the flag, the same
            /// shape a settled paste's path takes.
            ///
            /// - Parameters:
            ///   - state: Where the connection is now.
            ///   - detail: Why, when the state is Failed.
            void onStateChanged(Ubiquitty::Session::State state, const std::string& detail) override
            {
                session->noteRemoteStateChanged(state, detail);
                if (state == Ubiquitty::Session::State::Connected)
                    session->publishProportionalFaces();
                if (state != Ubiquitty::Session::State::Connected && session->m_remote && session->m_remote->closeRequestID) {
                    session->m_remote->closeRequestID = 0;
                    session->m_remote->closeOutcome = RemoteWorkspaceCloseOutcome::Refused;
                    session->noteRemoteEvent(EventKind::WorkspaceChanged);
                }
                if (state != Ubiquitty::Session::State::Connected && session->m_remote && session->m_remote->paneChangeRequestID) {
                    session->m_remote->paneChangeRequestID = 0;
                    session->m_remote->paneChange = { RemoteWorkspacePaneChange::Outcome::Interrupted, 0 };
                    session->noteRemoteEvent(EventKind::WorkspaceChanged);
                }
            }

            /// The host pinned a session id, which the frontend persists to
            /// reattach this shell rather than spawn a new one.
            ///
            /// - Parameter sessionID: The id, nonzero.
            void onSessionEstablished(std::uint32_t sessionID) override
            {
                session->noteRemoteEvent(EventKind::RemoteSessionEstablished, sessionID);
            }

            /// The host would not hand this connection back the session it
            /// asked to reattach.
            ///
            /// - Parameters:
            ///   - sessionID: The session that was refused.
            ///   - reason: The wire reason the host refused with.
            void onReattachRefused(std::uint32_t sessionID, Ubiquitty::Protocol::Sync::ByeReason reason) override
            {
                session->noteReattachRefused(sessionID, reason);
            }

            /// An agent wants to attach, kill or gain a capability, and the
            /// human has to say.
            ///
            /// - Parameter request: Who is asking and for what.
            void onConsentRequest(const Ubiquitty::Protocol::Sync::ConsentQuestion& request) override
            {
                session->noteConsentRequest(request);
            }

            /// Queues the host's exact tree-edit question for native presentation.
            ///
            /// - Parameter request: The pending operation and its consent identity.
            void onStructureConsent(const Protocol::Sync::StructureConsent& request) override
            {
                session->noteStructureConsent(request);
            }
            /// Removes a resolved tree-edit question from native presentation.
            ///
            /// - Parameter requestID: The question retired by the host.
            void onConsentResolved(const Protocol::Sync::ConsentResolution& resolution) override
            {
                session->noteConsentResolved(resolution);
            }

            /// The host answered a file request — queue it for the frontend.
            ///
            /// - Parameter file: The answer, bytes and all.
            void onFileData(Protocol::Control::FileReadReply&& file) override
            {
                session->noteRemoteFile(std::move(file));
            }

            /// The roster of who is attached changed.
            ///
            /// - Parameter presence: The whole roster, rebuilt by the host on
            ///   every attach, detach, grant and revoke.
            void onPresence(const Ubiquitty::Session::PanePresenceSnapshot& presence) override
            {
                session->notePresence(presence);
            }
            /// Retains a new host-observed read and wakes the pane's presentation.
            /// - Parameter event: Identity, time and cursor without terminal contents.
            void onScreenAccess(const Ubiquitty::Session::Event& event) override
            {
                if (session->m_screenAccessHistory.record({ .cursor = event.cursor, .unixMilliseconds = event.count, .claimedLabel = event.text, .verifiedProcess = event.detail }))
                    session->noteRemoteEvent(EventKind::PresenceChanged);
            }

            /// The host sent a structure; the mirror the core holds is the new one.
            void onWorkspaceChanged() override
            {
                session->noteRemoteEvent(EventKind::WorkspaceChanged);
                session->revealRemoteWorkspacePaneIfAsked();
            }

            /// Publishes close outcomes and restores the projection after refused verbs.
            ///
            /// - Parameter result: The host's result, correlated by request identifier.
            void onWorkspaceVerbResult(const Ubiquitty::Session::WorkspaceIntentResult& result) override
            {
                if (!session->m_remote)
                    return;
                if (session->m_remote->paneChangeRequestID && result.requestID == session->m_remote->paneChangeRequestID) {
                    session->m_remote->paneChangeRequestID = 0;
                    session->m_remote->paneChange = {
                        result.wasPerformed ? RemoteWorkspacePaneChange::Outcome::Accepted : RemoteWorkspacePaneChange::Outcome::Refused,
                        result.wasPerformed ? result.paneID : 0
                    };
                    session->noteRemoteEvent(EventKind::WorkspaceChanged);
                }
                if (session->m_remote->closeRequestID && result.requestID == session->m_remote->closeRequestID) {
                    session->m_remote->closeRequestID = 0;
                    session->m_remote->closeOutcome = result.wasPerformed ? RemoteWorkspaceCloseOutcome::Accepted : RemoteWorkspaceCloseOutcome::Refused;
                    session->noteRemoteEvent(EventKind::WorkspaceChanged);
                } else if (!result.wasPerformed) {
                    session->noteRemoteEvent(EventKind::WorkspaceChanged);
                }
            }
        };

        /// Forwards carrier creation while the workspace retains the physical factory.
        class BorrowedFactory final : public Ubiquitty::Session::ChannelFactory {
        public:
            explicit BorrowedFactory(Ubiquitty::Session::ChannelFactory& factory)
                : m_factory(factory)
            {
            }
            void createChannel(Ubiquitty::Session::TransportMode mode, Done done) override { m_factory.createChannel(mode, std::move(done)); }
            void createControlChannel(Done done) override { m_factory.createControlChannel(std::move(done)); }
            [[nodiscard]] std::optional<Session::ChannelCreationFailure> creationFailure() const override { return m_factory.creationFailure(); }
            [[nodiscard]] Session::TransportMode controlTransport() const override { return m_factory.controlTransport(); }
            /// The borrowed carrier keeps the same protected resume reach.
            ///
            /// - Returns: Whether the physical factory can attempt a resume.
            [[nodiscard]] bool canResumeChannel() const override { return m_factory.canResumeChannel(); }
            /// Forwards the proof, never replacing it with a fresh bootstrap.
            ///
            /// - Parameters:
            ///   - key: The attachment identity and proof secret.
            ///   - done: The physical factory's completion callback.
            void resumeChannel(Ubiquitty::Session::ResumeKey key, Done done) override { m_factory.resumeChannel(std::move(key), std::move(done)); }
            /// Forwards completion to the physical factory that owns deployment state.
            void hostDeploymentFinished() override { m_factory.hostDeploymentFinished(); }
            /// Pins the physical factory before any subsequent carrier is created.
            bool pinDaemonSocket(std::string_view socketName) override { return m_factory.pinDaemonSocket(socketName); }
            /// Invalidates the physical factory's predecessor reach for a planned successor.
            void clearHandoverReach() override { m_factory.clearHandoverReach(); }

        private:
            Ubiquitty::Session::ChannelFactory& m_factory;
        };

        PostedDispatcher dispatcher;
        std::unique_ptr<PlainChannelFactory> factory;
        std::shared_ptr<Ubiquitty::Session::WorkspaceConnection> workspaceConnection;
        Session::WorkspaceAuthority authority;
        uint32_t requestedWorkspaceID { 0 };
        uint32_t requestedSessionID { 0 };
        /// The factory the core actually calls.
        ///
        /// `&factory` for ``attachRemote``, which spawns its own carriers; the embedder's for
        /// ``attachRemoteOverCarrier``, which is borrowed and outlives the session. Indirect rather
        /// than a second owned member because the core takes one factory and calls it on every
        /// reconnect attempt, so the choice between them is made once, at attach.
        Ubiquitty::Session::ChannelFactory* activeFactory = nullptr;
        EventObserver observer;
        DeployParts deploy;
        /// The workspace every HELLO on this connection names, empty for an
        /// unnamed attach. Held beside the core rather than passed to
        /// ``startRemote``, because a reconnect rebuilds the HELLO with no
        /// caller present to supply it again.
        std::string workspaceName;
        /// Whether a session created under that name ends when its last human
        /// detaches cleanly.
        bool endsWhenDetached = false; // bool-ok: mirrors the single HELLO flag bit helloSessionEndWhenDetached; an enum here would be a second spelling of one wire bit
        std::uint32_t closeRequestID { 0 };
        RemoteWorkspaceCloseOutcome closeOutcome { RemoteWorkspaceCloseOutcome::Idle };
        std::uint32_t paneChangeRequestID { 0 };
        RemoteWorkspacePaneChange paneChange;
        /// One file this session asked a host for and has no answer to yet.
        struct AskedFile {
            /// The id the host echoes back on its answer.
            std::uint32_t requestID { 0 };
            /// The path this session asked for. Kept because the landing is
            /// named from it: the name carries the extension that decides
            /// which handler opens the file, and that decision is the
            /// reader's click rather than the other end's to make.
            std::string path;
            /// The line the clicked link named, 0 when it named none.
            Session::FileLinkLine line;
        };

        /// One answer, beside the path that was asked for.
        struct ArrivedFile {
            /// What the host sent, its own echoed path included.
            Protocol::Control::FileReadReply message;
            /// The path this session asked for, carried through from the ask.
            std::string requestedPath;
            /// The line the clicked link named, carried through from the ask.
            Session::FileLinkLine requestedLine;
        };

        /// Files asked for and not answered yet, oldest first; see
        /// ``noteAskedRemoteFile``.
        std::deque<AskedFile> askedFileRequests;
        /// Answers the host sent, oldest first; see ``noteRemoteFile``.
        std::deque<ArrivedFile> arrivedFiles;

        /// What the pane a shared window attached first keeps for the
        /// window's other committed panes; see ``attachSharedLocalWindow``.
        struct SharedWindow {
            /// The most panes one window's connection waits to place.
            static constexpr std::size_t maxAwaiting = 256;
            /// How long a pass waits before it tries the waiting panes again.
            static constexpr double retryIntervalSeconds = 0.1;
            /// A committed pane waiting for the host to place its session in
            /// this connection's workspace, or to move here from a connection
            /// of its own, and the loop it posts to.
            struct Member {
                WeakPtr<TerminalSession> session;
                PostedWorkLoop loop;
            };
            std::vector<Member> awaiting;
            /// Hears each change of the connection, for as long as this
            /// pane's remote half lasts.
            Ubiquitty::Session::WorkspaceConnection::Subscription subscription;
            /// The workspace the host confirmed for the window, or zero until
            /// the host's answer to the publication through this pane names it.
            std::uint32_t confirmedWorkspaceID = 0;
            /// The passes left that a timer may start while panes wait; a pane
            /// still waiting once they are spent keeps the connection it has.
            unsigned retryBudget = 600;
            /// Whether a timed pass is already pending.
            bool isRetryScheduled = false;
            /// Whether a pass is running, so the joins it makes start no other.
            bool isServicing = false;
            /// Whether the connection keeps to the workspace the host confirmed.
            bool isResolved = false;
        };
        std::unique_ptr<SharedWindow> sharedWindow;
        /// How long a pane waiting to move to its workspace's surviving
        /// connection waits before it tries again.
        static constexpr double moveRetryIntervalSeconds = 0.1;
        /// The tries left for that move; a pane that cannot settle by then
        /// stays on the connection it has.
        unsigned moveRetryBudget = 600;
        /// Whether a try at that move is already pending.
        bool isMoveScheduled = false;
        std::unique_ptr<Ubiquitty::Session::RemoteSessionCore> core;
    };
    std::unique_ptr<RemoteParts> m_remote;

    /// Whether a committed pane belongs with an anchor's shared window,
    /// posting to this loop, wherever it is attached now.
    ///
    /// - Parameters:
    ///   - anchor: The window's anchor (``anchorSharedLocalWindow``).
    ///   - loop: The loop this pane's endpoint posts to.
    /// - Returns: Whether both are resolved shares into one daemon by one
    ///   bridge, on the loop the anchor's connection posts to.
    [[nodiscard]] bool isPaneOfSharedWindow(const TerminalSession& anchor, const PostedWorkLoop& loop) const
    {
        if (&anchor == this || !m_sharing || hasUnresolvedLocalAdoption() || !loop.isComplete())
            return false;
        if (!anchor.m_sharing || anchor.hasUnresolvedLocalAdoption() || !anchor.m_remote || !anchor.m_remote->core
            || !anchor.m_remote->workspaceConnection || !anchor.m_remote->sharedWindow
            || anchor.m_remote->workspaceConnection->connection().state() == Session::State::Failed)
            return false;
        return anchor.m_remote->dispatcher.loop() == loop
            && Session::isSameLocalShareAuthority(anchor.m_sharing->bridgeCommand,
                Session::AdoptedSession { .sessionID = anchor.m_sharing->sessionID, .daemonInstanceID = anchor.m_sharing->daemonInstanceID },
                m_sharing->bridgeCommand,
                Session::AdoptedSession { .sessionID = m_sharing->sessionID, .daemonInstanceID = m_sharing->daemonInstanceID });
    }

    /// Whether a committed pane with no client yet may ride an anchor's
    /// connection.
    ///
    /// - Parameters:
    ///   - anchor: The window's anchor.
    ///   - loop: The loop this pane's endpoint would post to.
    /// - Returns: Whether it may.
    [[nodiscard]] bool mayRideSharedWindow(const TerminalSession& anchor, const PostedWorkLoop& loop) const
    {
        return !m_remote && isPaneOfSharedWindow(anchor, loop);
    }

    /// Whether a pane attached on a connection of its own may move onto an
    /// anchor's: the same authority, the same loop, and a window
    /// the host gave a workspace.
    ///
    /// - Parameters:
    ///   - anchor: The window's anchor.
    ///   - loop: The loop this pane's endpoint posts to.
    /// - Returns: Whether it may.
    [[nodiscard]] bool mayMoveIntoSharedWindow(const TerminalSession& anchor, const PostedWorkLoop& loop) const
    {
        return m_remote && m_remote->core && m_remote->workspaceConnection && isPaneOfSharedWindow(anchor, loop)
            && m_remote->dispatcher.loop() == loop && m_remote->authority == anchor.m_remote->authority
            && anchor.sharedWindowWorkspaceID();
    }

    /// The workspace the host gave an anchor's window: the one the anchor was
    /// told, or else the one the host's answer to its publication named.
    ///
    /// - Returns: The workspace, or zero while the host has named none.
    [[nodiscard]] std::uint32_t sharedWindowWorkspaceID() const
    {
        if (!m_remote || !m_remote->sharedWindow || !m_remote->workspaceConnection)
            return 0;
        if (m_remote->sharedWindow->confirmedWorkspaceID)
            return m_remote->sharedWindow->confirmedWorkspaceID;
        return m_remote->workspaceConnection->connection().sharedWorkspaceResult().workspaceID;
    }

    /// Retires this pane's own connection into the anchor's, once both carry
    /// the same workspace of the same daemon process.
    ///
    /// The precondition is ``mayMoveIntoSharedWindow``.
    ///
    /// - Parameter anchor: The window's anchor.
    /// - Returns: Whether the connection retires into the anchor's.
    bool retireIntoSharedWindow(TerminalSession& anchor)
    {
        const auto theirs = m_remote->workspaceConnection;
        const auto ours = anchor.m_remote->workspaceConnection;
        return theirs->successor() == ours || theirs->retireInto(ours);
    }

    /// Starts this pane's endpoint on the anchor's connection once the host's
    /// topology places the pane's session in its workspace and no other
    /// endpoint there shows it.
    ///
    /// The precondition is ``mayRideSharedWindow``.
    ///
    /// - Parameters:
    ///   - anchor: The window's anchor.
    ///   - loop: The loop the endpoint posts to.
    /// - Returns: Whether the endpoint started.
    bool rideSharedWindow(TerminalSession& anchor, const PostedWorkLoop& loop)
    {
        const std::uint32_t sessionID = m_sharing->sessionID;
        const auto connection = anchor.m_remote->workspaceConnection;
        Ubiquitty::Terminal* term = modelTerm();
        if (!term || !connection->canAttachSession(sessionID) || connection->connection().daemonInstanceID() != m_sharing->daemonInstanceID)
            return false;
        auto remote = std::make_unique<RemoteParts>();
        remote->workspaceConnection = connection;
        remote->authority = anchor.m_remote->authority;
        remote->requestedSessionID = sessionID;
        remote->dispatcher = RemoteParts::PostedDispatcher(loop);
        remote->observer.session = this;
        remote->core = std::make_unique<Ubiquitty::Session::RemoteSessionCore>(term, *connection, &remote->observer);
        readHostEffectDecisions(*remote->core);
        if (m_pixelGeometry)
            remote->core->setPixelGeometry(*m_pixelGeometry);
        if (m_networkCost)
            remote->core->setNetworkCost(*m_networkCost);
        m_remote = std::move(remote);
        const DimensionSnapshot dimensions = m_runtime->dimensions();
        m_remote->core->start(sessionID, dimensions.columns, dimensions.rows);
        return true;
    }

    /// Has this pane's connection tell the window's account of each change.
    void subscribeSharedWindow()
    {
        m_remote->sharedWindow->subscription = m_remote->workspaceConnection->subscribe([anchor = weakPtr()] {
            if (TerminalSession* session = anchor.get())
                session->serviceSharedWindow();
        });
    }

    /// Starts a timed pass over the window's waiting panes, within its budget.
    ///
    /// A pane waits on events this connection does not hear of: another
    /// connection reaching the workspace, its old input settling there.
    void scheduleSharedWindowRetry()
    {
        if (!m_remote || !m_remote->sharedWindow)
            return;
        RemoteParts::SharedWindow& window = *m_remote->sharedWindow;
        if (window.isRetryScheduled || !window.retryBudget)
            return;
        --window.retryBudget;
        window.isRetryScheduled = true;
        m_remote->dispatcher.postDelayed(RemoteParts::SharedWindow::retryIntervalSeconds, [anchor = weakPtr()] {
            TerminalSession* session = anchor.get();
            if (!session || !session->m_remote || !session->m_remote->sharedWindow)
                return;
            session->m_remote->sharedWindow->isRetryScheduled = false;
            session->serviceSharedWindow();
        });
    }

    /// Joins each awaiting pane its shared window's connection now admits,
    /// retires the connection of each pane attached on one of its own into
    /// this one, and keeps the connection to the workspace the host
    /// confirmed.
    ///
    /// Runs inside the connection's notice of a change, so a pane joins in
    /// the topology update that places it. A question the host asked about
    /// a joining pane in that update, which the carrier can read ahead of
    /// the structure and route while the joins are under way, waits until
    /// they are done and then reaches the pane that joined.
    void serviceSharedWindow()
    {
        if (!m_remote || !m_remote->sharedWindow || !m_remote->workspaceConnection || m_remote->sharedWindow->isServicing)
            return;
        const auto connection = m_remote->workspaceConnection;
        m_remote->sharedWindow->isServicing = true;
        auto members = std::exchange(m_remote->sharedWindow->awaiting, {});
        std::vector<RemoteParts::SharedWindow::Member> waiting;
        {
            const auto hold = connection->holdQuestionsWhileJoining();
            for (RemoteParts::SharedWindow::Member& member : members) {
                TerminalSession* session = member.session.get();
                if (!session || session->sharesRemoteWorkspaceConnection(*this))
                    continue;
                if (session->m_remote) {
                    // It moves itself once it can; until then it waits here.
                    if (!session->mayMoveIntoSharedWindow(*this, member.loop))
                        continue;
                    (void)session->retireIntoSharedWindow(*this);
                    waiting.push_back(std::move(member));
                } else {
                    if (!session->mayRideSharedWindow(*this, member.loop))
                        continue;
                    if (!session->rideSharedWindow(*this, member.loop))
                        waiting.push_back(std::move(member));
                }
                // A join's callbacks can end this pane's remote half, and the
                // window's account with it.
                if (!m_remote || m_remote->workspaceConnection != connection || !m_remote->sharedWindow)
                    return;
            }
        }
        // Routing what the hold kept can reach this pane's own session too.
        if (!m_remote || m_remote->workspaceConnection != connection || !m_remote->sharedWindow)
            return;
        RemoteParts::SharedWindow& window = *m_remote->sharedWindow;
        for (RemoteParts::SharedWindow::Member& member : waiting)
            window.awaiting.push_back(std::move(member));
        Session::RemoteSessionCore& physical = connection->connection();
        const std::uint32_t confirmed = sharedWindowWorkspaceID();
        if (!window.isResolved && confirmed && physical.acknowledged() && physical.state() == Session::State::Connected
            && physical.workspaceID() == confirmed) {
            window.isResolved = true;
            // From here a reconnect greets the workspace, and the connection
            // follows no adopted session out of it.
            physical.setAttachWorkspaceID(confirmed);
            // A workspace another owner here already carries keeps that owner.
            (void)remoteWorkspaceRegistry().adopt(m_remote->authority, connection);
        }
        if (window.isResolved && !connection->successor()) {
            // The window was added to a workspace this process already
            // carries: its panes move onto that owner, and this connection,
            // which published them because the host takes a publication only
            // from a pane it groups, detaches once they have.
            const auto owner = remoteWorkspaceRegistry().indexed(m_remote->authority, *connection);
            if (owner && owner != connection)
                (void)connection->retireInto(owner);
        }
        window.isServicing = false;
        if (!window.awaiting.empty())
            scheduleSharedWindowRetry();
    }

    /// How a pane moving onto another connection of its workspace stands.
    enum class ConnectionMove : std::uint8_t {
        /// It rides that connection.
        Moved,
        /// It will once its pane is placed and pictured there and nothing it
        /// started here is open.
        Waiting,
        /// It may not: another daemon process, or a pane already shown there.
        Refused,
    };

    /// Whether a host question this pane holds still waits on the person or
    /// on the host: a consent question shown or queued, a tree change, or a
    /// held host effect.
    [[nodiscard]] bool hasOpenHostQuestions() const
    {
        return m_pendingConsentToken || m_pendingHostEffectToken || !m_structureConsents.empty()
            || std::ranges::any_of(m_queuedConsents, [](const std::optional<QueuedConsent>& waiting) { return waiting.has_value(); });
    }

    /// Moves this pane from the connection it rides onto another connection
    /// of the same workspace, which survives in its place.
    ///
    /// In order, and only all at once: the successor's topology places the
    /// pane and nothing there shows it; nothing the pane started on its old
    /// connection is open there, so its input, requests and questions retire
    /// where they were made and no input is repeated under another
    /// attachment's numbering; the successor's carrier holds a verified
    /// picture of the pane; the old connection takes back its loan of the
    /// pane's terminal; the successor is lent it; and the old connection's
    /// hold is let go after this turn, so its attachment detaches once its
    /// last pane has gone. The pane's share, presentation and pending answers
    /// for its frontend go with it; nothing of the old attachment does.
    ///
    /// - Parameter successor: The surviving connection of the workspace.
    /// - Returns: How the move stands.
    ConnectionMove moveToWorkspaceConnection(const std::shared_ptr<Session::WorkspaceConnection>& successor)
    {
        if (!m_remote || !m_remote->core || !m_remote->workspaceConnection || !successor || hasUnresolvedLocalAdoption())
            return ConnectionMove::Refused;
        if (m_remote->workspaceConnection == successor)
            return ConnectionMove::Moved;
        Ubiquitty::Terminal* term = modelTerm();
        const std::uint32_t sessionID = m_remote->core->sessionID();
        const Session::RemoteSessionCore& from = m_remote->workspaceConnection->connection();
        const Session::RemoteSessionCore& to = successor->connection();
        if (!term || !sessionID || !to.daemonInstanceID() || to.daemonInstanceID() != from.daemonInstanceID())
            return ConnectionMove::Refused;
        if (!to.acknowledged() || to.state() != Session::State::Connected || !successor->containsSession(sessionID))
            return ConnectionMove::Waiting;
        if (!successor->canAttachSession(sessionID))
            return ConnectionMove::Refused;
        if (!m_remote->core->isSettledForMove() || hasOpenHostQuestions() || m_remote->closeRequestID || m_remote->paneChangeRequestID
            || !m_remote->askedFileRequests.empty() || m_remote->deploy.isPushInFlight)
            return ConnectionMove::Waiting;
        if (!successor->holdsPicture(sessionID))
            return ConnectionMove::Waiting;
        // One carrier writes the pane's terminal at a time: the old one gives
        // its loan back before the successor is lent it.
        m_remote->core->stop();
        if (!m_remote || !m_remote->core)
            return ConnectionMove::Refused;
        std::unique_ptr<RemoteParts> leaving = std::move(m_remote);
        auto remote = std::make_unique<RemoteParts>();
        remote->workspaceConnection = successor;
        remote->authority = leaving->authority;
        remote->requestedWorkspaceID = leaving->requestedWorkspaceID;
        remote->requestedSessionID = leaving->requestedSessionID;
        remote->workspaceName = leaving->workspaceName;
        remote->endsWhenDetached = leaving->endsWhenDetached;
        remote->dispatcher = leaving->dispatcher;
        remote->observer.session = this;
        remote->deploy = std::move(leaving->deploy);
        remote->arrivedFiles = std::move(leaving->arrivedFiles);
        remote->closeOutcome = leaving->closeOutcome;
        remote->paneChange = leaving->paneChange;
        remote->sharedWindow = std::move(leaving->sharedWindow);
        remote->core = std::make_unique<Ubiquitty::Session::RemoteSessionCore>(term, *successor, &remote->observer);
        readHostEffectDecisions(*remote->core);
        if (m_pixelGeometry)
            remote->core->setPixelGeometry(*m_pixelGeometry);
        if (m_networkCost)
            remote->core->setNetworkCost(*m_networkCost);
        remote->core->continueEndpoint(*leaving->core);
        m_remote = std::move(remote);
        if (m_remote->sharedWindow)
            subscribeSharedWindow();
        const DimensionSnapshot dimensions = m_runtime->dimensions();
        m_remote->core->start(sessionID, dimensions.columns, dimensions.rows);
        if (m_remote && m_remote->core)
            m_remote->core->restateView();
        std::shared_ptr<RemoteParts> released(std::move(leaving));
        if (m_remote)
            m_remote->dispatcher.post([released]() mutable { released.reset(); });
        noteRemoteEvent(EventKind::RemoteStateChanged, static_cast<std::uint32_t>(remoteState()));
        return ConnectionMove::Moved;
    }

    /// Tries this pane's move onto its connection's successor once this turn
    /// ends, or after a delay.
    ///
    /// - Parameter delaySeconds: How long to wait; zero waits for the turn only.
    void scheduleConnectionMove(double delaySeconds)
    {
        if (!m_remote || m_remote->isMoveScheduled)
            return;
        m_remote->isMoveScheduled = true;
        const auto attempt = [session = weakPtr()] {
            if (TerminalSession* live = session.get())
                live->attemptConnectionMove();
        };
        if (delaySeconds > 0)
            m_remote->dispatcher.postDelayed(delaySeconds, attempt);
        else
            m_remote->dispatcher.post(attempt);
    }

    /// Tries the move once, and again later while it waits, within its budget.
    void attemptConnectionMove()
    {
        if (!m_remote || !m_remote->workspaceConnection)
            return;
        m_remote->isMoveScheduled = false;
        const auto successor = m_remote->workspaceConnection->successor();
        if (!successor || moveToWorkspaceConnection(successor) != ConnectionMove::Waiting || !m_remote || !m_remote->moveRetryBudget)
            return;
        --m_remote->moveRetryBudget;
        scheduleConnectionMove(RemoteParts::moveRetryIntervalSeconds);
    }

    /// Stops the remote half's core, then frees the half.
    ///
    /// The file-ledger test seams stand up a half that holds the request
    /// ledger and no core: there is no transport to stop.
    void endRemoteHalf()
    {
        if (!m_remote)
            return;
        if (m_remote->core) {
            declineHeldHostQuestions();
            m_remote->core->stop();
        }
        m_remote.reset();
    }

    /// Declines as unshown every host question this pane holds, shown or
    /// waiting, as the pane leaves its connection. A workspace's connection
    /// outlives a pane it carries, and the host would otherwise wait on it for
    /// questions nobody here can see any more. What the pane holds is cleared
    /// by whatever ends it, as for any connection ending.
    void declineHeldHostQuestions()
    {
        if (m_pendingConsentToken && !m_pendingHostEffectToken && m_pendingConsent.requestID)
            declineUnshownQuestion(m_pendingConsent.requestID, static_cast<Protocol::Sync::ConsentKind>(m_pendingConsent.action));
        for (const auto& waiting : m_queuedConsents) {
            if (waiting)
                declineUnshownQuestion(waiting->question.requestID, static_cast<Protocol::Sync::ConsentKind>(waiting->question.action));
        }
        for (const auto& held : m_structureConsents)
            declineUnshownQuestion(held.request.requestID, Protocol::Sync::ConsentKind::Structure);
    }

    /// What a share into the local daemon holds while the daemon drives the PTY.
    ///
    /// Beside ``m_remote`` rather than inside it: the thin client is the
    /// ordinary one, and what is particular to a share is the local side it
    /// leaves standing — the stream, and whether its reader was running.
    struct SharingParts {
        /// Whether a reader drained the stream before the share, and so resumes
        /// on reclaim. A frontend that drains on its own loop had none.
        bool shouldResumeIngest = false;
        /// The id the daemon adopted the PTY under.
        std::uint32_t sessionID = 0;
        std::uint64_t daemonInstanceID = 0;
        /// The command that reaches the daemon, empty when the embedder built
        /// the carrier.
        std::string bridgeCommand;
    };
    std::optional<SharingParts> m_sharing;

    /// What a handoff to the daemon did.
    struct Handoff {
        /// The adopted session's id, or zero when the daemon adopted nothing.
        std::uint32_t sessionID = 0;
        /// How encoding the session ended; `Encoded` when the daemon was asked.
        Session::ShareEncoding encoding = Session::ShareEncoding::NothingToShare;
        Session::ShareOwnership ownership = Session::ShareOwnership::Local;
        /// Why the daemon rejected the session, when it answered with a reason.
        std::optional<Session::AdoptRejection> rejection = std::nullopt;
    };

    struct LocalAdoption {
        std::string socketPath;
        std::string bridgeCommand;
        Session::ShareHandoffLimits limits;
        std::unique_ptr<Session::AdoptClientAttempt> attempt;
        std::uint64_t escrowID = 0;
        bool shouldResumeIngest = false;
        bool isResolving = false;
    };
    std::optional<LocalAdoption> m_localAdoption;

    enum class LocalAdoptionResume : std::uint8_t {
        LiveView,
        RetainedWithoutView,
    };

    [[nodiscard]] Session::ShareOwnership resolveLocalAdoptionWithResume(std::uintptr_t resolveAddress, LocalAdoptionResume resume)
    {
        if (!hasUnresolvedLocalAdoption())
            return m_sharing ? Session::ShareOwnership::Committed : Session::ShareOwnership::Local;
        if (!resolveAddress || !m_localAdoption || !m_localAdoption->attempt || m_localAdoption->isResolving)
            return Session::ShareOwnership::Unresolved;
        auto protectingOwner = retainForLocalAdoption();
        m_localAdoption->isResolving = true;
        const auto operation = m_localAdoption->attempt->operation();
        const auto target = m_localAdoption->attempt->target();
        const auto resolve = reinterpret_cast<Session::AdoptResolve>(resolveAddress);
        auto result = resolve(m_localAdoption->socketPath.c_str(), std::move(m_localAdoption->attempt), m_localAdoption->limits);
        m_localAdoption->isResolving = false;
        if (!result.attempt || result.attempt->operation() != operation
            || (target.sessionID && result.attempt->target() != target)) {
            // A broken resolver cannot replace the operation whose master is
            // retained. Without that owner no later result can unfreeze it.
            result.attempt.reset();
            result.ownership = Session::ShareOwnership::Unresolved;
        }
        return acceptLocalAdoptionResult(std::move(result), resume);
    }

    [[nodiscard]] RetainedTerminalSession retainForLocalAdoption()
    {
        retainTerminalSession(this);
        return RetainedTerminalSession(this);
    }

    [[nodiscard]] Session::ShareOwnership acceptLocalAdoptionResult(Session::ShareHandoffResult result, LocalAdoptionResume resume)
    {
        auto ownership = result.ownership;
        if (ownership != Session::ShareOwnership::Local && ownership != Session::ShareOwnership::Committed
            && ownership != Session::ShareOwnership::Unresolved)
            ownership = Session::ShareOwnership::Unresolved;
        if (ownership == Session::ShareOwnership::Local) {
            if ((result.attempt && !result.attempt->mayResumeLocal()) || result.adopted.sessionID || result.adopted.daemonInstanceID)
                ownership = Session::ShareOwnership::Unresolved;
        } else if (ownership == Session::ShareOwnership::Committed) {
            if (!result.attempt || !result.attempt->isCommitted()
                || !result.adopted.sessionID || !result.adopted.daemonInstanceID
                || result.attempt->target().sessionID != result.adopted.sessionID
                || result.attempt->operation().daemonInstanceID != result.adopted.daemonInstanceID)
                ownership = Session::ShareOwnership::Unresolved;
        }
        m_localAdoption->attempt = std::move(result.attempt);
        const auto escrowID = m_localAdoption->escrowID;
        if (ownership == Session::ShareOwnership::Unresolved)
            return ownership;
        if (ownership == Session::ShareOwnership::Committed) {
            m_sharing = SharingParts { .shouldResumeIngest = m_localAdoption->shouldResumeIngest,
                .sessionID = result.adopted.sessionID,
                .daemonInstanceID = result.adopted.daemonInstanceID,
                .bridgeCommand = std::move(m_localAdoption->bridgeCommand) };
            m_runtime->confirmDaemonStreamOwnership();
            m_localAdoption->escrowID = 0;
            auto released = LocalAdoptionEscrow::singleton().release(escrowID);
            return ownership;
        }
        m_runtime->confirmLocalStreamOwnership();
        LocalAdoptionEscrow::singleton().confirmLocal(escrowID);
        if (resume == LocalAdoptionResume::LiveView && m_localAdoption->shouldResumeIngest)
            (void)startIngest();
        // A reference count cannot distinguish a view from transient resolver
        // owners. Keep exact Local resolution until explicit take/reopen/close.
        return ownership;
    }

    /// Stops the local reader and hands the PTY master to the daemon.
    ///
    /// - Parameters:
    ///   - socketPath: The daemon's control socket.
    ///   - handoffAddress: A ``Session::AdoptHandoff``, as an address.
    /// - Returns: The proven owner, session identity, and capture outcome.
    Handoff handOffToLocalDaemon(const std::string& socketPath, std::uintptr_t handoffAddress)
    {
        if (socketPath.empty() || handoffAddress == 0 || !isShareableLocalSession())
            return {};
        auto protectingOwner = retainForLocalAdoption();
        RetainedTerminalSession previousEscrow;
        if (m_localAdoption && m_localAdoption->escrowID) {
            previousEscrow = LocalAdoptionEscrow::singleton().release(m_localAdoption->escrowID);
            m_localAdoption.reset();
        }
        auto reserved = LocalAdoptionEscrow::singleton().reserve(retainForLocalAdoption());
        if (!reserved)
            return {};
        const bool wasIngesting = m_ingestThread.joinable();
        m_localAdoption.emplace(LocalAdoption { .socketPath = socketPath, .bridgeCommand = {}, .limits = Session::localShareHandoffLimits(), .attempt = {}, .escrowID = *reserved, .shouldResumeIngest = wasIngesting });
        stopIngest();
        const auto descriptor = m_runtime->freezeLocalStreamForAdoption();
        if (!descriptor) {
            m_localAdoption.reset();
            auto released = LocalAdoptionEscrow::singleton().release(*reserved);
            if (wasIngesting)
                (void)startIngest();
            return {};
        }
        std::expected<Protocol::Sync::HandoverBytes, Session::ShareEncoding> terminalBody = std::unexpected(Session::ShareEncoding::NothingToShare);
        {
            const std::scoped_lock hold(*m_activeLock);
            if (Ubiquitty::Terminal* term = modelTerm())
                terminalBody = Session::captureShareTerminal(*term, Protocol::Local::maxAdoptBodyBytes);
        }
        if (!terminalBody) {
            m_runtime->confirmLocalStreamOwnership();
            m_localAdoption.reset();
            auto released = LocalAdoptionEscrow::singleton().release(*reserved);
            if (wasIngesting)
                (void)startIngest();
            return { .sessionID = 0, .encoding = terminalBody.error() };
        }
        const auto handoff = reinterpret_cast<Session::AdoptHandoff>(handoffAddress);
        const auto ownership = acceptLocalAdoptionResult(handoff(socketPath.c_str(), std::as_bytes(terminalBody->view()), *descriptor, m_localAdoption->limits), LocalAdoptionResume::LiveView);
        return { .sessionID = ownership == Session::ShareOwnership::Committed ? m_sharing->sessionID : 0,
            .encoding = Session::ShareEncoding::Encoded,
            .ownership = ownership,
            .rejection = m_localAdoption && m_localAdoption->attempt ? m_localAdoption->attempt->rejection() : std::nullopt };
    }

    /// The daemon handed the shared session back: finish on the next turn of
    /// the loop, because the core reporting it is inside its own callback and
    /// ending the thin client ends that core.
    void noteReclaimed()
    {
        if (!m_sharing || !m_remote)
            return;
        m_remote->dispatcher.post([weak = weakPtr()] {
            if (TerminalSession* session = weak.get())
                session->resumeLocalAfterReclaim();
        });
    }

    /// Ends the thin client and resumes the retained stream.
    ///
    /// The model is current already: the daemon's frames fed this same
    /// terminal while it drove the PTY. What the thin client left behind — the
    /// roster, a consent question — belonged to the daemon's session and goes
    /// with it.
    void resumeLocalAfterReclaim()
    {
        if (!m_sharing)
            return;
        const bool shouldResumeIngest = m_sharing->shouldResumeIngest;
        m_runtime->confirmLocalStreamOwnership();
        m_localAdoption.reset();
        m_sharing.reset();
        endRemoteHalf();
        m_presence = {};
        const bool hadConsent = m_pendingConsentToken || !m_structureConsents.empty();
        m_pendingConsent = {};
        m_pendingConsentToken = 0;
        m_pendingHostEffectToken = 0;
        m_queuedConsents = {};
        m_structureConsents.clear();
        m_isStructureConsentShown = false;
        if (hadConsent)
            noteRemoteEvent(EventKind::ConsentRequestPending);
        if (shouldResumeIngest)
            (void)startIngest();
        noteRemoteEvent(EventKind::PresenceChanged);
        noteRemoteEvent(EventKind::RemoteStateChanged, static_cast<std::uint32_t>(Ubiquitty::Session::State::Idle));
    }

    /// The host's session directory, owned beside the attach rather than
    /// inside it.
    ///
    /// Distinguishes an authenticated listing from an unanswered request.
    enum class DirectoryListSource : std::uint8_t {
        Host,
        Unanswered
    };

    /// A frontend asks a host what it holds before it binds this surface to any
    /// of it, so the directory cannot live on ``RemoteParts`` — those are built
    /// by an attach, and the question is what the attach should be.
    struct DirectoryParts {
        /// Folds the directory's answers into the session's flag surface.
        struct Observer final : Ubiquitty::Session::SessionDirectoryObserver {
            /// The session whose flags the answers land in.
            TerminalSession* session = nullptr;

            /// The host's answer is complete — publish it. The tree it
            /// reported is what a reader draws and decides on; the rows are
            /// that tree read flat, which nothing here keeps.
            void onSessionList(const std::vector<Ubiquitty::Session::SessionRow>&) override
            {
                session->noteHostListingAnswered();
            }

            /// The host described everything it holds; it arrives before
            /// the same answer completes.
            ///
            /// - Parameter tree: The tree, copied.
            void onSessionTree(const Ubiquitty::Session::SessionTree& tree) override
            {
                session->noteHostSessionTree(tree);
            }

            /// The question could not be asked or was not answered.
            ///
            /// Reported as an empty list rather than left alone: a picker
            /// still holding the previous answer would offer rows the host may
            /// no longer have, and attaching to one of those opens a fresh
            /// shell where the human expected their own.
            ///
            /// The reason is dropped rather than carried: an event is scalars,
            /// and a picker with no rows has already said what it can.
            ///
            /// - Parameter report: Why; read only for a change whose call
            ///   was out, which ends as unanswered with this as its reason.
            void onDirectoryFailed(const Ubiquitty::Session::DirectoryFailureReport& report) override
            {
                // A change whose call never came back says nothing about
                // what the host holds: the rows stay.
                if (session->noteHostStructureUnanswered(Ubiquitty::Session::listingFailureNotice(report, Ubiquitty::Session::NoticePlacement::Standalone)))
                    return;
                session->noteHostListingAnswered(DirectoryListSource::Unanswered);
            }

            /// The host answered a change's call.
            ///
            /// - Parameter response: The JSON-RPC response.
            void onCallAnswered(const std::string& response) override
            {
                session->noteHostStructureAnswer(response);
            }

            /// A terminate settled, one way or another.
            ///
            /// - Parameters:
            ///   - sessionID: The session the request named.
            ///   - outcome: What the daemon did.
            void onTerminateSettled(std::uint32_t sessionID, Ubiquitty::Session::TerminateOutcome outcome) override
            {
                session->noteHostSessionTerminateSettled(sessionID, outcome);
            }
        };

        /// Folds what the watch's connection reports into the same tree,
        /// kept apart from the errands' observer so a watch ending is never
        /// read as a listing failing.
        struct WatchObserver final : Ubiquitty::Session::SessionDirectoryObserver {
            /// The session whose tree the reports land in.
            TerminalSession* session = nullptr;

            /// The host's tree, whole, on the connection the watch holds.
            ///
            /// - Parameter tree: The tree, copied.
            void onSessionTree(const Ubiquitty::Session::SessionTree& tree) override
            {
                session->noteHostWatchedTree(tree);
            }

            /// The carrier dropped and the watch is dialling again.
            void onWatchInterrupted(const Ubiquitty::Session::DirectoryFailureReport&) override
            {
                session->noteHostWatchLost();
            }

            /// The host refused the watch, which has ended.
            void onDirectoryFailed(const Ubiquitty::Session::DirectoryFailureReport&) override
            {
                session->noteHostWatchLost();
            }
        };

        RemoteParts::PostedDispatcher dispatcher;
        Observer observer;
        WatchObserver watchObserver;
        /// The spawning carrier the directory dials when the caller supplies no
        /// factory of its own. The attach path's, so the errand and the attach
        /// reach a host the one way; its QUIC arm is unreachable here because
        /// the errand dials in TCP mode alone.
        RemoteParts::PlainChannelFactory factory;
        /// The factory the core was built over, so a second ask on the same one
        /// refreshes rather than rebuilds.
        std::uintptr_t factoryAddress = 0;
        /// The owner of the held answer, independent of a later in-flight
        /// handshake.
        std::string listedDaemonSocketName;
        std::uint64_t listedDaemonInstanceID = 0;
        /// The session the last settled terminate named; see
        /// ``lastHostSessionTerminateSessionID``.
        std::uint32_t lastTerminateSessionID = 0;
        /// The sessions a confirmed terminate has still to ask the host to
        /// end, in order; each is sent as the one before it settles.
        std::vector<std::uint32_t> owedTerminations;
        /// Why the last confirmed terminate sent nothing; see
        /// ``lastHostTerminateRefusal``.
        std::string lastTerminateRefusal;
        /// The question last worded; see ``askHostDestructiveConfirmation``.
        DestructiveConfirmation lastConfirmation;
        /// The changes asked of the host's structure that are out, how the
        /// last one ended, and the one whose call the errand in flight
        /// carries.
        StructureCalls structure;
        StructureResult lastStructureResult;
        std::uint64_t structureRequestInFlight = 0;
        /// Everything the host last said it holds, or null when its last
        /// answer had no tree.
        std::shared_ptr<const Ubiquitty::Session::SessionTree> tree;
        std::uint64_t treeRevision = 0;
        /// How far the host's wall clock was ahead of this machine's when
        /// the tree was reported, in milliseconds.
        std::int64_t hostClockLeadMilliseconds = 0;
        /// Whether the listing in flight reported a tree before it completed.
        bool didListingReportTree = false; // bool-ok: names the one condition it reports
        /// How current ``tree`` is.
        HostDirectoryClient::TreeFreshness freshness = HostDirectoryClient::TreeFreshness::None;
        std::unique_ptr<Ubiquitty::Session::SessionDirectoryCore> core;
        /// The factory the watch was built over, so a second ask over the
        /// same one keeps the watch that is out.
        std::uintptr_t watcherFactoryAddress = 0;
        /// The connection that follows the host; null while nothing does.
        /// Last, so it is torn down before the dispatcher it posts through.
        std::unique_ptr<Ubiquitty::Session::SessionDirectoryCore> watcher;
    };
    std::unique_ptr<DirectoryParts> m_directory;

    /// Records that the host's answer is complete and publishes the change.
    ///
    /// - Parameter source: Whether the answer and its owner came from the
    ///   host.
    void noteHostListingAnswered(DirectoryListSource source = DirectoryListSource::Host)
    {
        if (!m_directory)
            return;
        // An answer with no tree before it leaves none to draw: the previous
        // one would describe a host that has since changed or gone.
        // A watch that is running holds the tree itself and keeps it.
        const bool hasLiveTree = m_directory->freshness == HostDirectoryClient::TreeFreshness::Live;
        if (!m_directory->didListingReportTree && m_directory->tree && !isHostWatchRunning() && !hasLiveTree) {
            m_directory->tree.reset();
            m_directory->freshness = HostDirectoryClient::TreeFreshness::None;
            ++m_directory->treeRevision;
        }
        m_directory->didListingReportTree = false;
        if (!hasLiveTree) {
            const auto* core = source == DirectoryListSource::Host ? m_directory->core.get() : nullptr;
            m_directory->listedDaemonSocketName = core ? core->daemonSocketName() : std::string();
            m_directory->listedDaemonInstanceID = core ? core->daemonInstanceID() : 0;
        }
        noteRemoteEvent(EventKind::HostSessionListChanged);
    }

    /// Keeps the tree a listing reported; the answer's end publishes it.
    ///
    /// - Parameter tree: The tree, copied.
    void noteHostSessionTree(const Ubiquitty::Session::SessionTree& tree)
    {
        if (!m_directory)
            return;
        m_directory->didListingReportTree = true;
        // Listings and watches travel over separate connections. A delayed
        // listing may predate the watch's latest report, so the live watch
        // remains the owner of both the tree and its daemon identity.
        if (m_directory->freshness == HostDirectoryClient::TreeFreshness::Live)
            return;
        m_directory->tree = std::make_shared<const Ubiquitty::Session::SessionTree>(tree);
        ++m_directory->treeRevision;
        m_directory->hostClockLeadMilliseconds = static_cast<std::int64_t>(tree.hostNowUnixMilliseconds)
            - std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        m_directory->freshness = HostDirectoryClient::TreeFreshness::Listed;
    }

    /// Keeps the tree a connected watch reported and publishes it, with the
    /// daemon that reported it as the owner a pick from it attaches to.
    ///
    /// - Parameter tree: The tree, copied.
    void noteHostWatchedTree(const Ubiquitty::Session::SessionTree& tree)
    {
        if (!m_directory)
            return;
        m_directory->tree = std::make_shared<const Ubiquitty::Session::SessionTree>(tree);
        ++m_directory->treeRevision;
        m_directory->hostClockLeadMilliseconds = static_cast<std::int64_t>(tree.hostNowUnixMilliseconds)
            - std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        m_directory->freshness = HostDirectoryClient::TreeFreshness::Live;
        if (const auto* watcher = m_directory->watcher.get()) {
            m_directory->listedDaemonSocketName = watcher->daemonSocketName();
            m_directory->listedDaemonInstanceID = watcher->daemonInstanceID();
        }
        noteRemoteEvent(EventKind::HostSessionListChanged);
    }

    /// Marks a tree a watch was following as followed no longer, and
    /// publishes the change.
    void noteHostWatchLost()
    {
        if (!m_directory || m_directory->freshness != HostDirectoryClient::TreeFreshness::Live)
            return;
        m_directory->freshness = HostDirectoryClient::TreeFreshness::Stale;
        noteRemoteEvent(EventKind::HostSessionListChanged);
    }

    /// Whether a watch is out: following the host, or dialling it.
    [[nodiscard]] bool isHostWatchRunning() const { return m_directory && m_directory->watcher && m_directory->watcher->isWatching(); }

    /// Whether a watch is following the host now.
    [[nodiscard]] bool isHostWatchLive() const { return m_directory && m_directory->watcher && m_directory->watcher->isWatchLive(); }

    /// Stands the directory up and points its posts at the platform's loop.
    ///
    /// - Parameter loop: The platform's loop, both entry points set.
    void prepareDirectory(PostedWorkLoop loop)
    {
        if (!m_directory)
            m_directory = std::make_unique<DirectoryParts>();
        m_directory->dispatcher = PostedDispatcher(loop);
        m_directory->factory.dispatcher = m_directory->dispatcher;
        m_directory->observer.session = this;
        m_directory->watchObserver.session = this;
    }

    /// Shows this session's pane on its host when that was asked for and
    /// the layout that makes it possible has arrived.
    void revealRemoteWorkspacePaneIfAsked()
    {
        if (m_shouldRevealRemoteWorkspacePane && revealRemoteWorkspacePane())
            m_shouldRevealRemoteWorkspacePane = false;
    }

    /// Whether ``revealRemoteWorkspacePane`` is owed once a layout arrives.
    bool m_shouldRevealRemoteWorkspacePane = false; // bool-ok: names the one state it reports

    /// Keeps how a structure change ended and publishes it.
    ///
    /// - Parameter result: How it ended.
    void noteHostStructureSettled(StructureResult result)
    {
        if (!m_directory)
            return;
        m_directory->lastStructureResult = std::move(result);
        noteRemoteEvent(EventKind::HostStructureSettled);
    }

    /// Reads the host's answer to a change's call, and asks the host what it
    /// holds now when the change was served.
    ///
    /// - Parameter response: The JSON-RPC response.
    void noteHostStructureAnswer(const std::string& response)
    {
        if (!m_directory)
            return;
        const std::uint64_t requestID = std::exchange(m_directory->structureRequestInFlight, 0);
        std::optional<StructureResult> result = m_directory->structure.noteAnswer(response);
        if (!result)
            result = m_directory->structure.fail(requestID, "The host answered a different call.");
        if (!result)
            return;
        const bool wasServed = result->outcome == StructureOutcome::Served; // bool-ok: a local reading
        noteHostStructureSettled(std::move(*result));
        // A watch that follows the host reports the change itself.
        if (wasServed && m_directory->core && !isHostWatchLive())
            (void)m_directory->core->refresh();
    }

    /// Ends the change whose call is out as unanswered.
    ///
    /// - Parameter reason: Why, as a sentence.
    /// - Returns: Whether a change's call was out.
    bool noteHostStructureUnanswered(std::string reason)
    {
        if (!m_directory || m_directory->structureRequestInFlight == 0)
            return false;
        if (std::optional<StructureResult> result = m_directory->structure.fail(std::exchange(m_directory->structureRequestInFlight, 0), std::move(reason)))
            noteHostStructureSettled(std::move(*result));
        return true;
    }

    /// Records how a terminate settled and publishes it.
    ///
    /// - Parameters:
    ///   - sessionID: The session the request named, so a frontend knows which
    ///     of its rows the outcome applies to.
    ///   - outcome: What the daemon did.
    void noteHostSessionTerminateSettled(std::uint32_t sessionID, Ubiquitty::Session::TerminateOutcome outcome)
    {
        if (m_directory)
            m_directory->lastTerminateSessionID = sessionID;
        noteRemoteEvent(EventKind::HostSessionTerminateSettled, static_cast<std::uint32_t>(outcome));
        sendOwedHostTerminations();
    }

    /// Asks the host to end the next session a confirmed terminate named.
    ///
    /// One at a time, each sent as the one before it settles. One the host
    /// cannot be asked for settles as failed, so every session the question
    /// named is answered for.
    void sendOwedHostTerminations()
    {
        while (m_directory && !m_directory->owedTerminations.empty()) {
            const std::uint32_t next = m_directory->owedTerminations.front();
            m_directory->owedTerminations.erase(m_directory->owedTerminations.begin());
            if (m_directory->core && m_directory->core->terminate(next))
                break;
            m_directory->lastTerminateSessionID = next;
            noteRemoteEvent(EventKind::HostSessionTerminateSettled, static_cast<std::uint32_t>(Ubiquitty::Session::TerminateOutcome::Failed));
        }
    }

    /// The mirror the remote core carries, or null before the first layout
    /// and on a session that is not remote.
    [[nodiscard]] const Session::Layout::Workspace* remoteWorkspace() const
    {
        return m_remote && m_remote->core ? m_remote->core->workspace() : nullptr;
    }

    /// Sends one verb against the surface holding this session's pane.
    ///
    /// - Parameters:
    ///   - kind: The verb.
    ///   - tab: The tab the verb names, or `invalidTab`.
    ///   - pane: The pane the verb names, or `invalidPane`.
    ///   - orientation: A split's orientation.
    /// - Returns: Whether the verb was sent — false before the first layout or HELLO_ACK.
    bool sendRemoteWorkspaceVerb(Ubiquitty::Session::WorkspaceIntentKind kind, Session::Layout::TabID tab, Session::Layout::PaneID pane, Ubiquitty::Panes::Orientation orientation = Ubiquitty::Panes::Orientation::Vertical)
    {
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.verb = kind;
        verb.tabID = std::to_underlying(tab);
        verb.paneID = std::to_underlying(pane);
        verb.orientation = static_cast<std::uint8_t>(orientation);
        verb.ratio = Ubiquitty::Panes::defaultRatio;
        return sendRemoteWorkspaceVerb(verb);
    }

    /// Sends a verb whose surface is the one holding this session's pane.
    ///
    /// - Parameter verb: The verb, its surface filled in here.
    /// - Returns: Whether the verb was sent — false before the first layout or HELLO_ACK.
    bool sendRemoteWorkspaceVerb(Ubiquitty::Session::WorkspaceIntent verb)
    {
        const Session::Layout::SurfaceID surface = remoteWorkspaceSurface();
        if (surface == Session::Layout::invalidSurface)
            return false;
        verb.surfaceID = std::to_underlying(surface);
        using WorkspaceVerb = Ubiquitty::Session::WorkspaceIntentKind;
        const bool shouldTrackPaneChange = verb.verb == WorkspaceVerb::AddSurface || verb.verb == WorkspaceVerb::AddTab
            || verb.verb == WorkspaceVerb::SplitFocusedPane || verb.verb == WorkspaceVerb::ClosePane;
        if (shouldTrackPaneChange && m_remote->paneChangeRequestID)
            return false; // the outstanding request still owns its eventual answer
        const auto requestID = m_remote->core->sendWorkspaceVerb(verb);
        if (shouldTrackPaneChange) {
            m_remote->paneChangeRequestID = requestID;
            m_remote->paneChange = {
                requestID ? RemoteWorkspacePaneChange::Outcome::Pending : RemoteWorkspacePaneChange::Outcome::Refused, 0
            };
            noteRemoteEvent(EventKind::WorkspaceChanged);
        }
        return requestID != 0;
    }

public:
    /// Consumes an unused protected workspace core before any remote attach.
    /// A preview is copied into this surface; the prepared owner continues to
    /// own every retained terminal until the physical connection takes it.
    ///
    /// - Parameters:
    ///   - ownerAddress: Borrowed address of a unique SyncClientCore owner,
    ///     consumed only on success and valid for this synchronous call.
    ///   - paneLow: Low half of the authenticated cached preview identity.
    ///   - paneHigh: High half of the authenticated cached preview identity.
    /// - Returns: Whether the exact prepared owner was installed without I/O.
    [[nodiscard]] bool prepareRemoteResume(std::uintptr_t ownerAddress, std::uint64_t paneLow, std::uint64_t paneHigh)
    {
        if (hasUnresolvedLocalAdoption() || !ownerAddress || m_remote || m_preparedRemote || !modelTerm())
            return false;
        auto& prepared = *reinterpret_cast<std::unique_ptr<Protocol::Sync::SyncClientCore>*>(ownerAddress);
        if (!prepared)
            return false;
        bool didDraw = false;
        std::optional<std::uint64_t> generation;
        if (paneLow || paneHigh) {
            const std::scoped_lock hold(*m_activeLock);
            generation = prepared->preparedPaneGeneration({ paneLow, paneHigh });
            if (!generation)
                return false;
            didDraw = prepared->drawPreparedPane({ paneLow, paneHigh }, *modelTerm());
            if (!didDraw)
                return false;
        }
        m_remoteClientInstance = prepared->clientInstance();
        m_restoredPreview.reset();
        if (generation)
            m_restoredPreview = Session::FrameReady { .frameClass = Protocol::Sync::FrameClass::Structural,
                .generation = *generation,
                .origin = Session::FrameOrigin::Restored };
        m_preparedRemote = std::move(prepared);
        if (didDraw)
            noteRemoteScreenChanged();
        return true;
    }

    /// Shows this session's pane on its host: its window in front, its tab
    /// the one shown, the pane the one typed into.
    ///
    /// Attaching by a pane's session binds the whole workspace and leaves the
    /// host's own focus where it was, so opening a pane's row lands
    /// somewhere in its workspace. This is what puts the reader on the pane:
    /// only what is not already so is sent.
    ///
    /// - Returns: Whether the pane is shown or everything needed was sent;
    ///   false before the first layout, when nothing was sent.
    bool revealRemoteWorkspacePane()
    {
        using WorkspaceVerb = Ubiquitty::Session::WorkspaceIntentKind;
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        const Session::Layout::PaneID pane = remoteWorkspacePane();
        const Session::Layout::SurfaceID surface = remoteWorkspaceSurface();
        const Session::Layout::TabID tab = remoteWorkspaceTab();
        if (mirror == nullptr || pane == Session::Layout::invalidPane || surface == Session::Layout::invalidSurface || tab == Session::Layout::invalidTab)
            return false;
        if (mirror->activeSurface() != surface && !sendRemoteWorkspaceVerb(WorkspaceVerb::SetActiveSurface, Session::Layout::invalidTab, Session::Layout::invalidPane))
            return false;
        if (mirror->activeTab(surface) != tab && !sendRemoteWorkspaceVerb(WorkspaceVerb::SetActiveTab, tab, Session::Layout::invalidPane))
            return false;
        const Ubiquitty::Panes::Tree* tree = mirror->paneTree(surface, tab);
        const bool isFocused = tree != nullptr && static_cast<std::uint32_t>(tree->focusedPaneID()) == std::to_underlying(pane); // bool-ok: a local reading
        return isFocused || sendRemoteWorkspaceVerb(WorkspaceVerb::FocusPane, Session::Layout::invalidTab, pane);
    }

    /// Asks for ``revealRemoteWorkspacePane`` once, as soon as the host's
    /// layout has arrived: for the session an attach opened at a pane's row
    /// (`Session::AttachTarget::paneID`).
    void revealRemoteWorkspacePaneWhenAttached()
    {
        m_shouldRevealRemoteWorkspacePane = !revealRemoteWorkspacePane();
    }

private:
    /// Sends a pane verb of a remote session to the host.
    ///
    /// The host applies a pane verb to the surface's active tab and its
    /// focused pane, so the tab and the focus are made this session's first
    /// and the verb lands on the pane the user is looking at. The host's
    /// answer comes back as a structure, through ``EventKind::WorkspaceChanged``.
    ///
    /// - Parameter verb: What the user asked of the pane.
    /// - Returns: `Handled` once every frame was sent; `Unsupported` before the
    ///   first layout or HELLO_ACK, when nothing was sent.
    PaneVerbOutcome requestRemoteWorkspaceVerb(PaneVerb verb)
    {
        using WorkspaceVerb = Ubiquitty::Session::WorkspaceIntentKind;
        const Session::Layout::Workspace* mirror = remoteWorkspace();
        const Session::Layout::PaneID pane = remoteWorkspacePane();
        const Session::Layout::SurfaceID surface = remoteWorkspaceSurface();
        const Session::Layout::TabID tab = remoteWorkspaceTab();
        if (mirror == nullptr || pane == Session::Layout::invalidPane || surface == Session::Layout::invalidSurface || tab == Session::Layout::invalidTab)
            return PaneVerbOutcome::Unsupported;
        if (mirror->activeTab(surface) != tab && !sendRemoteWorkspaceVerb(WorkspaceVerb::SetActiveTab, tab, Session::Layout::invalidPane))
            return PaneVerbOutcome::Unsupported;
        const Ubiquitty::Panes::Tree* tree = mirror->paneTree(surface, tab);
        const bool isFocused = tree != nullptr && static_cast<std::uint32_t>(tree->focusedPaneID()) == std::to_underlying(pane);
        bool wasSent = false;
        switch (verb) {
        case PaneVerb::SplitBeside:
        case PaneVerb::SplitBelow:
            wasSent = sendRemoteWorkspaceVerb(WorkspaceVerb::SplitFocusedPane, Session::Layout::invalidTab, pane,
                verb == PaneVerb::SplitBeside ? Ubiquitty::Panes::Orientation::Vertical : Ubiquitty::Panes::Orientation::Horizontal);
            break;
        case PaneVerb::Close:
            wasSent = sendRemoteWorkspaceVerb(WorkspaceVerb::ClosePane, Session::Layout::invalidTab, pane);
            break;
        case PaneVerb::Zoom:
            wasSent = sendRemoteWorkspaceVerb(WorkspaceVerb::ToggleZoom, Session::Layout::invalidTab, pane);
            break;
        case PaneVerb::Focus:
            wasSent = isFocused || sendRemoteWorkspaceVerb(WorkspaceVerb::FocusPane, Session::Layout::invalidTab, pane);
            break;
        }
        return wasSent ? PaneVerbOutcome::Handled : PaneVerbOutcome::Unsupported;
    }

    /// Returns where each shared ssh connection reaches its host.
    ///
    /// Kept for every session in the process: one probe for a master, however
    /// many sessions and redials go over it.
    ///
    /// - Returns: The process-owned record of those reaches.
    static Session::SharedConnectionReaches& sharedConnectionReaches()
    {
        static Session::SharedConnectionReaches reaches;
        return reaches;
    }

    /// Returns the process-owned registry of live workspace carriers.
    ///
    /// - Returns: The registry shared by frontend session projections.
    static Session::WorkspaceConnectionRegistry& remoteWorkspaceRegistry()
    {
        static Session::WorkspaceConnectionRegistry registry;
        return registry;
    }

    /// Stands the remote parts up, short of starting the core.
    ///
    /// Split from the start so the two attach doors share every line that is not about where the
    /// carrier comes from — the one thing they actually differ on.
    ///
    /// The session id is not taken: nothing here reads it, and ``startRemote`` is what
    /// carries it to the handshake. Taking it as well would be two parameters spelling one fact,
    /// and the copy nothing reads is the one that goes stale.
    ///
    /// - Parameters:
    ///   - loop: The platform's loop, as ``attachRemote`` documents.
    ///   - factoryAddress: The embedder's factory, or 0 to use the owned spawning one.
    /// - Returns: Whether the parts were built; false when there is no model, one is already up,
    ///   or the loop lacks an entry point.
    bool beginRemote(PostedWorkLoop loop, std::uintptr_t factoryAddress)
    {
        if (hasUnresolvedLocalAdoption() || !loop.isComplete())
            return false;
        if (!modelTerm() || m_remote)
            return false;
        m_remote = std::make_unique<RemoteParts>();
        m_remote->dispatcher = PostedDispatcher(loop);
        m_remote->factory = std::make_unique<RemoteParts::PlainChannelFactory>();
        m_remote->factory->dispatcher = m_remote->dispatcher;
        if (m_pendingResumeReach)
            (void)m_remote->factory->seedResumeReach(*m_pendingResumeReach);
        m_pendingResumeReach.reset();
        m_remote->activeFactory = factoryAddress != 0
            ? reinterpret_cast<Ubiquitty::Session::ChannelFactory*>(factoryAddress)
            : m_remote->factory.get();
        m_remote->observer.session = this;
        return true;
    }

    /// Builds the core over whichever factory ``beginRemote`` selected, and starts it.
    ///
    /// - Parameter reattachSessionID: The daemon session to re-home to; zero asks for a fresh one.
    /// - Returns: Whether the attach started.
    bool startRemote(std::uint32_t reattachSessionID)
    {
        Ubiquitty::Terminal* term = modelTerm();
        if (!term || !m_remote)
            return false;
        // A share went to this machine's own daemon whatever carries it; any
        // other carrier is placed by the commands this client chose, and an
        // embedder's carrier cannot be placed at all.
        Protocol::Sync::HostLocation hostLocation = Protocol::Sync::HostLocation::AnotherMachine;
        if (m_sharing)
            hostLocation = Protocol::Sync::HostLocation::ThisMachine;
        else if (m_remote->activeFactory == m_remote->factory.get())
            hostLocation = hostLocationOfPlan(m_remote->factory->shellCommand, m_remote->factory->quicHost, m_remote->factory->quicBootstrapCommand);
        std::unique_ptr<Ubiquitty::Session::ChannelFactory> factory;
        if (m_remote->activeFactory == m_remote->factory.get()) {
            factory = std::move(m_remote->factory);
        } else {
            factory = std::make_unique<RemoteParts::BorrowedFactory>(*m_remote->activeFactory);
            m_remote->factory.reset();
        }
        m_remote->activeFactory = nullptr;
        m_remote->workspaceConnection = std::make_shared<Ubiquitty::Session::WorkspaceConnection>(
            std::make_unique<PostedDispatcher>(m_remote->dispatcher), std::move(factory));
        auto& connection = m_remote->workspaceConnection->connection();
        connection.setClipboardServing(Ubiquitty::Preferences::Effective::clipboardServing(m_configuration));
        connection.setHostLocation(hostLocation);
        connection.setWorkspace(m_remote->workspaceName, m_remote->endsWhenDetached);
        connection.setExpectedDaemonInstanceID(m_remote->authority.daemonInstanceID);
        connection.setAuthToken(m_remoteAuthToken);
        if (m_preparedRemote && !connection.adoptPreparedResume(std::move(m_preparedRemote))) {
            m_restoredPreview.reset();
            m_remote.reset();
            return false;
        }
        if (m_remoteClientInstance)
            connection.setClientInstance(*m_remoteClientInstance);
        if (m_sharing) {
            connection.setExpectedDaemonInstanceID(m_sharing->daemonInstanceID);
            connection.followAdoptedSessionWorkspace();
        }
        m_remote->core = std::make_unique<Ubiquitty::Session::RemoteSessionCore>(term, *m_remote->workspaceConnection, &m_remote->observer);
        readHostEffectDecisions(*m_remote->core);
        if (m_pixelGeometry)
            m_remote->core->setPixelGeometry(*m_pixelGeometry);
        if (m_networkCost)
            m_remote->core->setNetworkCost(*m_networkCost);
        std::shared_ptr<const RichText::TypographyFacts> typography;
        {
            const std::scoped_lock hold(*m_activeLock);
            typography = m_typography;
        }
        if (typography)
            m_remote->core->publishTypography(*typography);
        m_measuredFacesKey = {};
        publishProportionalFaces();
        if (m_restoredPreview) {
            m_remote->core->adoptRestoredPreview(*m_restoredPreview);
            m_restoredPreview.reset();
        }
        // Read here rather than at construction: the claim latches into HELLO,
        // so this is the last moment a preference change can still reach the
        // wire, and the moment a reconnect passes through again.

        // Named before the handshake for the reason the clipboard claim is:
        // the name latches into HELLO, and a connection told its workspace
        // after start would send the first one unnamed.

        armHostUpdateBeforeAttach();
        const DimensionSnapshot dimensions = m_runtime->dimensions();
        m_remote->core->start(reattachSessionID, dimensions.columns, dimensions.rows);
        return true;
    }
    /// Where the host landed the last pushed paste; see ``lastPastePushPath``.
    std::string m_lastPastePushPath;

    /// Who is attached to this session; see ``presenceAttendeeCount``.
    ///
    /// The whole roster, because a frontend draws all of it and the host sends
    /// all of it.
    Ubiquitty::Session::PanePresenceSnapshot m_presence;
    Ubiquitty::Session::ScreenAccessHistory m_screenAccessHistory;

    /// The consent question waiting on the human; see ``pendingConsent``.
    ///
    /// Held whole because a prompt has to name who is asking: a request id
    /// alone would ask the user to authorise a number.
    Ubiquitty::Protocol::Sync::ConsentQuestion m_pendingConsent;
    std::uint64_t m_pendingConsentToken = 0;
    /// The held host effect the pending question asks about, or zero when the
    /// question is the host's own.
    std::uint64_t m_pendingHostEffectToken = 0;
    /// A host question waiting behind the one shown, with its place in line.
    struct QueuedConsent {
        std::uint64_t order = 0;
        Ubiquitty::Protocol::Sync::ConsentQuestion question;
    };
    /// How many host questions wait behind the one shown; one past it is
    /// declined as unshown rather than answered unseen.
    static constexpr std::size_t maxQueuedConsents = 4;
    std::array<std::optional<QueuedConsent>, maxQueuedConsents> m_queuedConsents {};
    std::uint64_t m_nextQueuedConsentOrder = 1;
    /// A queued question and the frontend token that identifies this precise
    /// presentation even when the host restarts and reuses a request number.
    struct PendingStructureConsent {
        std::uint64_t token = 0;
        Protocol::Sync::StructureConsent request;
        Security::StructureConsentText words;
    };
    std::deque<PendingStructureConsent> m_structureConsents;
    std::uint64_t m_nextConsentToken = 1;
    bool m_isStructureConsentShown = false; // bool-ok: the active question's kind

    /// Why the connection failed, when it did; see ``lastRemoteFailure``.
    ///
    /// Cleared on any state that is not Failed, so a reconnect that succeeds
    /// does not leave the previous failure's words on a working session.
    std::string m_lastRemoteFailure;

    /// Why the connection last changed state; see ``lastRemoteTransitionDetail``.
    std::string m_lastRemoteTransitionDetail;

    /// Whether the last failure was a refused reattach; see
    /// ``wasReattachRefused``.
    bool m_wasReattachRefused = false;

    /// Which session the refusal named; see ``refusedReattachSessionID``.
    std::uint32_t m_refusedReattachSessionID = 0;
    /// The access token every attach presents; see ``setRemoteAuthToken``.
    std::string m_remoteAuthToken;
    // Kept across replacement of RemoteParts so old native completions cannot
    // authorize a question on a different connection that reused a token.
    std::uint64_t m_nextFileDownloadToken { 0 };
    std::uint64_t m_nextWindowCloseToken { 0 };
    /// The instance the next remote attach greets as, when a frontend set
    /// one; see ``setRemoteClientInstance``.
    std::optional<Protocol::Sync::ID128> m_remoteClientInstance;
    std::unique_ptr<Protocol::Sync::SyncClientCore> m_preparedRemote;
    std::optional<Session::FrameReady> m_restoredPreview;
    /// The reach the next attach's own factory dials a resume by; see
    /// ``seedRemoteResumeReach``.
    std::optional<Session::ResumeReach> m_pendingResumeReach;
    /// The update the next attach runs before it dials; see
    /// ``requestHostUpdateBeforeAttach``.
    std::optional<std::string> m_hostUpdateBeforeAttachPlatform;

    /// How many of a host's file answers are held before the oldest is
    /// dropped. A reader clicks paths one at a time; a host answering more
    /// than a handful at once is answering questions nobody asked.
    static constexpr std::size_t maxQueuedRemoteFiles = 4;

    /// Decides what a deploy request should do, and does the part that is
    /// not a question for the user.
    ///
    /// The binary is chosen by the platform the host's probe reports, not
    /// by the one the request named, so a request whose platform is unknown —
    /// an update prompted by a host too old to say — reaches the same push.
    /// The decision here only needs to know whether any push could follow.
    ///
    /// - Parameter platformArch: The remote's `uname -sm`, empty when unknown.
    void beginDeploy(const std::string& platformArch)
    {
        if (!m_remote || !m_remote->core)
            return;
        RemoteParts::DeployParts& deploy = m_remote->deploy;
        deploy.wasDeferred = false;
        deploy.pendingLiveSessionCount = 0;
        deploy.requestedPlatform = platformArch;
        const bool hasBundledBinary = platformArch.empty() ? hasAnyDeployBinary() : !bundledBinaryFor(platformArch).empty();
        const bool isHostTrusted = Runtime::HostDirectory::isDeployTrusted(deploy.sshHost);
        const Ubiquitty::Session::DeployAction action = Ubiquitty::Session::decideDeployAction(
            deploy.isAutoInstallAllowed && !deploy.sshHost.empty(), platformArch, hasBundledBinary, isHostTrusted);
        switch (action) {
        case Ubiquitty::Session::DeployAction::Off:
        case Ubiquitty::Session::DeployAction::ManualInstall:
            reportManualInstall(platformArch);
            return;
        case Ubiquitty::Session::DeployAction::ConfirmPush:
            // The frontend answers on its own schedule; the core is already
            // waiting on deployFinished, so nothing further happens here.
            noteRemoteEvent(EventKind::DeployConfirmationPending);
            return;
        case Ubiquitty::Session::DeployAction::Push:
            performDeployPush();
            return;
        }
        // Fail closed: an action this switch does not name pushes nothing.
        m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed, "unhandled deploy action");
    }

    /// The bundled binary for a platform, empty when none serves it.
    ///
    /// - Parameter platformArch: The remote's `uname -sm`.
    /// - Returns: The binary's path.
    [[nodiscard]] std::string bundledBinaryFor(const std::string& platformArch) const
    {
        const auto& binaries = m_remote->deploy.binaryForPlatform;
        const auto binary = binaries.find(platformArch);
        return binary == binaries.end() ? std::string() : binary->second;
    }

    /// Whether any bundled binary was offered.
    ///
    /// - Returns: Whether one was.
    [[nodiscard]] bool hasAnyDeployBinary() const
    {
        return std::ranges::any_of(m_remote->deploy.binaryForPlatform, [](const auto& entry) { return !entry.second.empty(); });
    }

    /// Ends a deploy no automatic install can serve, with the commands a
    /// person runs by hand.
    ///
    /// - Parameter platformArch: The remote's `uname -sm`.
    void reportManualInstall(const std::string& platformArch)
    {
        RemoteParts::DeployParts& deploy = m_remote->deploy;
        // A connect that offered no ssh host reaches Off whatever the
        // setting says, so the setting is asked directly there: telling a
        // person to turn on a setting that is already on sends them to the
        // wrong fix.
        const bool isAutoInstallOn = deploy.sshHost.empty()
            ? Preferences::Effective::autoDeployHostOnRemoteConnect()
            : deploy.isAutoInstallAllowed;
        deploy.manualInstallPlatform = platformArch;
        deploy.isManualInstallForSetting = !isAutoInstallOn;
        m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed,
            isAutoInstallOn ? "no bundled ubiquitty-host for this platform" : "automatic host install is turned off");
        noteRemoteEvent(EventKind::DeployNeedsManualInstall);
    }

    /// What a command run on the host over ssh printed, and whether it ran.
    struct RemoteCommandRun {
        /// Everything it printed, standard error included, tail-capped.
        std::string output;
        /// Why it could not be run to the end; empty when it was.
        std::string failure;
    };

    /// Probes the host a person has approved, then installs on it.
    ///
    /// The probe asks the host's platform and how many sessions its running
    /// daemon holds (``Session::hostProbeCommand``); it changes nothing there.
    /// It and the install run on their own threads — a host binary is
    /// megabytes over a network, and the dispatcher this was called on is
    /// the frontend's main loop — and each posts its answer back so the next
    /// step and `deployFinished` land on the session thread like every other
    /// reply.
    void performDeployPush()
    {
        if (!m_remote || !m_remote->core)
            return;
        RemoteParts::DeployParts& deploy = m_remote->deploy;
        deploy.isPushInFlight = true;
        if (!hasAnyDeployBinary() || deploy.sshHost.empty()) {
            m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed, "nothing to push");
            return;
        }
        // The destination reaches a shell command line below, so it is held
        // to the ssh destination grammar here, whatever its caller checked.
        if (!Ubiquitty::Transport::validateSSHHost(deploy.sshHost)) {
            m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed, "the SSH destination is not valid");
            return;
        }
        // A binary this Mac cannot read fails here, before the host is asked
        // anything: the probe would only lead to an upload that cannot start.
        if (!deploy.requestedPlatform.empty() && !std::ifstream(bundledBinaryFor(deploy.requestedPlatform), std::ios::binary)) {
            m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed, "the bundled host binary could not be read");
            return;
        }
        // Standard error joins the output: what ssh and the remote shell say
        // about a failure is the tail the person is shown, and this app has no
        // console for an inherited stderr to reach.
        const std::string command = "ssh " + Ubiquitty::Transport::shellSingleQuote(deploy.sshHost) + " "
            + Ubiquitty::Transport::shellSingleQuote(Ubiquitty::Transport::remoteShellCommand(Ubiquitty::Session::hostProbeCommand())) + " 2>&1";
        // Copied rather than borrowed: nothing joins the thread, and the
        // dispatcher lives inside the session. Two plain fields, so the copy
        // carries no lifetime with it.
        RemoteParts::PostedDispatcher dispatcher = m_remote->dispatcher;
        std::weak_ptr<int> lifetime = deploy.lifetime;
        std::thread([this, lifetime, dispatcher, command]() mutable {
            // Boxed: what the probe printed is larger than a posted unit holds.
            auto run = std::make_unique<RemoteCommandRun>(runRemoteCommand(command, {}));
            // The session is touched only here, on the dispatcher's thread,
            // which is the thread that destroys it. The worker never reads it.
            dispatcher.post([this, lifetime, run = std::move(run)] {
                if (lifetime.lock())
                    deployProbeAnswered(*run);
            });
        }).detach();
    }

    /// Picks the binary and the consent an install needs from the probe.
    ///
    /// - Parameter run: What the probe printed.
    void deployProbeAnswered(const RemoteCommandRun& run)
    {
        if (!m_remote || !m_remote->core)
            return;
        RemoteParts::DeployParts& deploy = m_remote->deploy;
        const std::optional<Ubiquitty::Session::HostProbe> probe = run.failure.empty() ? Ubiquitty::Session::parseHostProbe(run.output) : std::nullopt;
        if (!probe) {
            const std::string detail = run.failure.empty() ? Ubiquitty::Session::deployFailureDetail(run.output) : run.failure;
            m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed, "the host could not be asked what it runs: " + detail);
            return;
        }
        deploy.pendingBinary = bundledBinaryFor(probe->platform);
        if (deploy.pendingBinary.empty()) {
            reportManualInstall(probe->platform);
            return;
        }
        switch (Ubiquitty::Session::decideHostUpdateConsent(probe->liveSessionCount)) {
        case Ubiquitty::Session::HostUpdateConsent::Proceed:
            performDeployInstall(Ubiquitty::Session::HostInstallMode::IfIdle);
            return;
        case Ubiquitty::Session::HostUpdateConsent::AskFirst:
            deploy.pendingLiveSessionCount = probe->liveSessionCount;
            noteRemoteEvent(EventKind::DeployConfirmationPending);
            return;
        }
    }

    /// Carries out the answer to the live-sessions question.
    ///
    /// - Parameter mode: Now or when the sessions end.
    void answerHostUpdateSessions(Ubiquitty::Session::HostInstallMode mode)
    {
        if (!m_remote || !m_remote->core || m_remote->deploy.pendingLiveSessionCount == 0)
            return;
        m_remote->deploy.pendingLiveSessionCount = 0;
        performDeployInstall(mode);
    }

    /// Uploads the chosen binary and installs it under `mode`.
    ///
    /// The digest is taken from the bytes that are sent, so what the host
    /// verifies is exactly what left this machine.
    ///
    /// - Parameter mode: When the running host may be replaced.
    void performDeployInstall(Ubiquitty::Session::HostInstallMode mode)
    {
        RemoteParts::DeployParts& deploy = m_remote->deploy;
        if (!Ubiquitty::Transport::validateSSHHost(deploy.sshHost)) {
            m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed, "the SSH destination is not valid");
            return;
        }
        RemoteParts::PostedDispatcher dispatcher = m_remote->dispatcher;
        std::weak_ptr<int> lifetime = deploy.lifetime;
        std::thread([this, lifetime, dispatcher, sshHost = deploy.sshHost, binaryPath = deploy.pendingBinary, mode]() mutable {
            Ubiquitty::Session::HostInstallOutcome outcome;
            std::ifstream source(binaryPath, std::ios::binary);
            std::string bytes;
            if (source)
                bytes.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
            if (!source || bytes.empty()) {
                outcome.reason = "the bundled host binary could not be read";
            } else {
                const std::string script = Ubiquitty::Session::hostInstallCommand(Ubiquitty::Session::hostUploadDigest(bytes), mode);
                const RemoteCommandRun run = runRemoteCommand("ssh " + Ubiquitty::Transport::shellSingleQuote(sshHost) + " "
                        + Ubiquitty::Transport::shellSingleQuote(Ubiquitty::Transport::remoteShellCommand(script)) + " 2>&1",
                    bytes);
                if (run.failure.empty())
                    outcome = Ubiquitty::Session::readHostInstallOutput(run.output, Ubiquitty::Session::deployFailureDetail(run.output));
                else
                    outcome.reason = run.failure;
            }
            // Boxed: an outcome's failure sentence makes it larger than a
            // posted unit holds.
            dispatcher.post([this, lifetime, outcome = std::make_unique<Ubiquitty::Session::HostInstallOutcome>(std::move(outcome))] {
                if (lifetime.lock())
                    deployInstallFinished(*outcome);
            });
        }).detach();
    }

    /// Reports how an install ended to the connection waiting on it.
    ///
    /// Only an installed host is dialled. A deferred one leaves the
    /// connection Failed with the sentence that says when the new host takes
    /// over, because until then the host this Mac would reach is the old one.
    ///
    /// - Parameter outcome: How it ended.
    void deployInstallFinished(const Ubiquitty::Session::HostInstallOutcome& outcome)
    {
        if (!m_remote || !m_remote->core)
            return;
        switch (outcome.kind) {
        case Ubiquitty::Session::HostInstallOutcomeKind::Installed:
            m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Succeeded, "");
            return;
        case Ubiquitty::Session::HostInstallOutcomeKind::Deferred:
            m_remote->deploy.wasDeferred = true;
            m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed,
                Ubiquitty::Session::hostUpdateDeferredNotice(outcome.waitingSessionCount, m_remote->deploy.sshHost));
            return;
        case Ubiquitty::Session::HostInstallOutcomeKind::HandoverPending:
            m_remote->deploy.wasDeferred = true;
            m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed, outcome.reason);
            return;
        case Ubiquitty::Session::HostInstallOutcomeKind::Failed:
            m_remote->core->deployFinished(Ubiquitty::Session::DeployResult::Failed,
                outcome.reason.empty() ? std::string("the host install did not complete") : outcome.reason);
            return;
        }
    }

    /// Hands the attach about to start the update a caller asked for.
    void armHostUpdateBeforeAttach()
    {
        if (!m_hostUpdateBeforeAttachPlatform)
            return;
        m_remote->core->updateHostBeforeConnect(*m_hostUpdateBeforeAttachPlatform);
        m_hostUpdateBeforeAttachPlatform.reset();
    }

    /// How long a push may make no progress at all before it is given up on.
    ///
    /// An inactivity bound rather than a total one: a host binary is megabytes
    /// over whatever link the person has, and a push that is still moving has
    /// not failed however long it takes. A push that is moving nothing is a
    /// child waiting on something nobody here can answer — an ssh asking for a
    /// password on a console the app has none of — and the thread it runs on
    /// is detached, so it has to end on its own.
    static constexpr int deployStallMilliseconds = 120 * 1000;

    /// How much of a push's output is kept.
    ///
    /// The tail is what a failure is read from, so the oldest goes when the
    /// cap is reached rather than the newest being dropped.
    static constexpr std::size_t maxDeployOutputBytes = 64 * 1024;

    /// Keeps the tail of what a push printed, within the cap.
    ///
    /// - Parameters:
    ///   - output: What it has printed so far.
    ///   - bytes: What it just printed.
    static void appendDeployOutput(std::string& output, std::span<const char> bytes)
    {
        output.append(bytes.data(), bytes.size());
        if (output.size() > maxDeployOutputBytes)
            output.erase(0, output.size() - maxDeployOutputBytes);
    }

    /// Takes everything a push has printed and is holding in its pipe.
    ///
    /// - Parameters:
    ///   - pushed: The running push.
    ///   - output: What it has printed so far, appended to.
    /// - Returns: Whether the pipe is still open. False is the child having
    ///   closed its output or the pipe having failed, which while the binary
    ///   is still being written is the push being over.
    [[nodiscard]] static bool readDeployOutput(PAL::PipedCommand& pushed, std::string& output)
    {
        std::array<char, 8 * 1024> buffer {};
        while (true) {
            const int count = pushed.output->read(std::span<char>(buffer));
            if (count == static_cast<int>(PAL::StreamStatus::Again))
                return true;
            if (count <= 0)
                return false;
            appendDeployOutput(output, std::span<const char>(buffer.data(), static_cast<std::size_t>(count)));
        }
    }

    /// Moves both of a push's pipes until nothing written is still queued.
    ///
    /// ``writeAll`` is non-blocking: what the child's input pipe will not take
    /// now is queued in memory, and closing that input with bytes still queued
    /// hands the remote `cat` a truncated binary. The install's digest check
    /// refuses one, but the push would then fail for a reason that is this
    /// side's. The child's own output is drained in the same loop, because a
    /// child blocked writing to a full pipe stops reading its input and
    /// nothing would ever leave the queue.
    ///
    /// - Parameters:
    ///   - pushed: The running push.
    ///   - output: What it has printed so far, appended to.
    /// - Returns: Whether every queued byte reached the child.
    [[nodiscard]] static bool flushDeployPush(PAL::PipedCommand& pushed, std::string& output)
    {
        while (pushed.input->drainPendingWrite()) {
            if (!readDeployOutput(pushed, output))
                return false;
            if (!pushed.input->hasPendingWrite())
                return true;
            std::array<PAL::Watch, 2> watches { {
                { &pushed.input->handle(), PAL::Ready::Writable },
                { &pushed.output->handle(), PAL::Ready::Readable },
            } };
            if (PAL::waitForReadiness(watches, deployStallMilliseconds) <= 0)
                return false;
            // A pipe whose far end is gone is ready forever; without this the
            // drain above would spin on a child that has already died.
            if (PAL::contains(watches[0].result, PAL::Ready::Error | PAL::Ready::HangUp | PAL::Ready::Invalid))
                return false;
        }
        return true;
    }

    /// Runs one command on the host to completion, off the dispatcher.
    ///
    /// - Parameters:
    ///   - command: The ssh command line.
    ///   - input: What it reads on standard input; empty closes it at once.
    /// - Returns: What it printed, and why it could not be run to the end
    ///   when it could not — the tail of its output where it printed any.
    [[nodiscard]] static RemoteCommandRun runRemoteCommand(const std::string& command, std::string_view input)
    {
        RemoteCommandRun run;
        PAL::PipedCommand pushed = PAL::spawnPipedCommand(command);
        if (!pushed) {
            run.failure = "could not start ssh to the host";
            return run;
        }
        constexpr std::size_t chunkBytes = 64 * 1024;
        for (std::size_t offset = 0; offset < input.size(); offset += chunkBytes) {
            const std::string_view chunk = input.substr(offset, chunkBytes);
            if (pushed.input->writeAll(std::span<const char>(chunk.data(), chunk.size())) < 0) {
                run.failure = Ubiquitty::Session::deployFailureDetail(run.output);
                return run;
            }
            if (!flushDeployPush(pushed, run.output)) {
                run.failure = Ubiquitty::Session::deployFailureDetail(run.output);
                return run;
            }
        }
        if (!flushDeployPush(pushed, run.output)) {
            run.failure = Ubiquitty::Session::deployFailureDetail(run.output);
            return run;
        }
        pushed.input.reset(); // the remote `cat` ends on EOF
        // Read to EOF: the verdict is the last line, and draining to the end
        // is also what lets the child exit rather than block on a full pipe
        // nobody is reading. A read that would block waits for the pipe
        // rather than ending the drain — the verdict is printed only after the
        // checks and the rename have run.
        std::array<char, 64 * 1024> buffer {};
        while (true) {
            const int count = pushed.output->read(std::span<char>(buffer));
            if (count == static_cast<int>(PAL::StreamStatus::Again)) {
                std::array<PAL::Watch, 1> watches { { { &pushed.output->handle(), PAL::Ready::Readable } } };
                if (PAL::waitForReadiness(watches, deployStallMilliseconds) <= 0)
                    break;
                continue;
            }
            if (count <= 0)
                break;
            appendDeployOutput(run.output, std::span<const char>(buffer.data(), static_cast<std::size_t>(count)));
        }
        return run;
    }

    /// Answers a host program's clipboard request from this machine's own.
    ///
    /// Runs on the frontend's loop, because that is the thread it handed the
    /// session as its dispatcher and the one where a platform pasteboard may be
    /// touched. Every decision inside belongs to
    /// ``Session::answerClipboardRequest``; what is here is the two facts it
    /// needs and the frame it produces.
    ///
    /// The read gate is asked of the terminal rather than of the preference,
    /// because that is the value the terminal is running under — a reload
    /// applies to a live terminal through ``Runtime::seedOSC52Policy``, and
    /// reading the preference again here could answer with a policy this
    /// session has not adopted.
    ///
    /// - Parameter request: What was asked, and the id to answer with.
    void answerHostClipboardRequest(const Ubiquitty::Protocol::Sync::ClipboardRequestMessage& request)
    {
        if (!m_remote || !m_remote->core)
            return;
        Ubiquitty::Terminal* term = modelTerm();
        Ubiquitty::Clipboard::ServeFacts gate;
        gate.didClaimServe = m_remote->core->didClaimClipboardServe();
        gate.isClipboardReadDenied = term != nullptr
            && term->osc52ClipboardReadPolicy == Ubiquitty::OSC::ClipboardReadPolicy::Deny;
        Ubiquitty::ClipboardClient* clip = term != nullptr ? term->clipboardClient.get() : nullptr;
        const Ubiquitty::Protocol::Sync::ClipboardDataMessage answer
            = Ubiquitty::Session::answerClipboardRequest(clip, request, gate);
        m_remote->core->serveClipboard(answer.requestID, answer.status, answer.items);
    }

    /// Applies admitted complete data without copying its payload owner.
    /// The host selected this client and admitted the effect; this terminal's
    /// own clipboard-write denial still wins, just as for a local program.
    ///
    /// - Parameters:
    ///   - buffer: A validated destination selector.
    ///   - items: Borrowed representations that remain live through delivery.
    void applyRemoteClipboardWriteItems(ClipboardBuffer buffer, std::span<const ClipboardItem> items)
    {
        const std::scoped_lock hold(*m_activeLock);
        Ubiquitty::Terminal* term = modelTerm();
        if (!term || !term->clipboardClient || term->osc52ClipboardWritePolicy == Ubiquitty::OSC::ClipboardWritePolicy::Deny)
            return;
        term->clipboardClient->writeClipboardItems(buffer, items);
    }

    /// Posts a full host-admitted notification through the local shell client.
    ///
    /// - Parameter request: Validated fields borrowed for this call.
    void applyRemoteNotificationRequest(const NotificationRequest& request)
    {
        {
            const std::scoped_lock hold(*m_activeLock);
            Ubiquitty::Terminal* term = modelTerm();
            Ubiquitty::ShellClient* shell = term ? term->shellClient.get() : nullptr;
            if (!shell)
                return;
            shell->postNotificationRequest(request);
        }
        // The queue raises its flag and leaves the waking to the drain that
        // fed it; a remote banner arrives with no drain behind it.
        signalDrain();
    }

    /// Records a connection-state move and publishes it.
    ///
    /// A method rather than three lines in the observer, so a test can drive
    /// the state machine's report without standing up a transport: the
    /// observer is a forwarder and this is the behaviour, which means a case
    /// against this is a case against what actually runs.
    ///
    /// - Parameters:
    ///   - state: Where the connection went.
    ///   - detail: Why, when the state is Failed.
    void noteRemoteStateChanged(Ubiquitty::Session::State state, const std::string& detail)
    {
        if (state != Ubiquitty::Session::State::Connected) {
            {
                const std::scoped_lock hold(*m_activeLock);
                m_remoteDragEpoch = 0;
                m_dragDropQueue.clear();
                DragDrop::Metadata stop;
                stop.type = DragDrop::Type::StopAccepting;
                (void)m_dragDropQueue.sendCommand({ .kind = DragDrop::CommandKind::Registration, .metadata = stop });
                stop.type = DragDrop::Type::Offer;
                stop.x = 2;
                (void)m_dragDropQueue.sendCommand({ .kind = DragDrop::CommandKind::Registration, .metadata = stop });
            }
            noteRemoteEvent(EventKind::DragDropPending);
            const bool hadConsent = m_pendingConsentToken || !m_structureConsents.empty();
            // The endpoint withdraws the effects it held as its connection
            // ends; the host's questions end with the connection too.
            m_pendingHostEffectToken = 0;
            m_queuedConsents = {};
            m_pendingConsent = {};
            m_pendingConsentToken = 0;
            m_structureConsents.clear();
            m_isStructureConsentShown = false;
            if (hadConsent)
                noteRemoteEvent(EventKind::ConsentRequestPending);
        }
        // Cleared on any state that is not Failed: a reconnect that succeeds
        // must not leave the previous failure's words on a working session,
        // where a frontend would show them beside a live screen. The refusal
        // flag is one of those words and clears with them.
        const bool isFailed = state == Ubiquitty::Session::State::Failed;
        m_lastRemoteFailure = isFailed ? detail : std::string();
        m_lastRemoteTransitionDetail = detail;
        if (!isFailed) {
            m_wasReattachRefused = false;
            m_refusedReattachSessionID = 0;
        }
        // The push holds the connection in Deploying until it settles, so the
        // first other state is its verdict: Failed carries what it printed, and
        // anything else is the reconnect a finished push asks for.
        if (m_remote && m_remote->deploy.isPushInFlight && state != Ubiquitty::Session::State::Deploying) {
            m_remote->deploy.isPushInFlight = false;
            if (isFailed) {
                m_remote->deploy.lastFailure = detail;
                noteRemoteEvent(EventKind::DeployFailed);
            }
        }
        noteRemoteEvent(EventKind::RemoteStateChanged, static_cast<std::uint32_t>(state));
    }

    /// Records a refused reattach and publishes it as the failure it is.
    ///
    /// Reported through ``EventKind::RemoteStateChanged`` rather than a kind of
    /// its own: a frontend already draws Failed, and this is which Failed it
    /// is. The flags beside it are what separate a session the host will not
    /// give back from a host that went away, and the wording is the core's so
    /// the state and the flag surface cannot describe one refusal differently.
    ///
    /// - Parameters:
    ///   - sessionID: The session the host refused.
    ///   - reason: The wire reason the host refused with.
    void noteReattachRefused(std::uint32_t sessionID, Ubiquitty::Protocol::Sync::ByeReason reason)
    {
        m_wasReattachRefused = true;
        m_refusedReattachSessionID = sessionID;
        noteRemoteStateChanged(Ubiquitty::Session::State::Failed,
            Ubiquitty::Session::reattachRefusedNotice(sessionID, reason));
    }

    /// Records the roster and publishes the change.
    ///
    /// Kept whole because the frontend draws it whole: the host rebuilds the
    /// list on every attach, detach, grant and revoke, so this is a
    /// replacement rather than a delta and there is nothing to merge.
    ///
    /// - Parameter presence: The roster.
    void notePresence(const Ubiquitty::Session::PanePresenceSnapshot& presence)
    {
        m_presence = presence;
        noteRemoteEvent(EventKind::PresenceChanged);
    }

    /// Remembers that this session asked for a file, so the answer to it can
    /// be told from a file nobody asked for.
    ///
    /// Bounded by the same cap the answers are, because the two are one
    /// conversation: with a fifth ask in flight the oldest answer would be
    /// dropped on arrival anyway, and a reader clicks paths one at a time.
    ///
    /// - Parameters:
    ///   - requestID: The id the host will echo back.
    ///   - path: The path asked for, which the answer's landing is named
    ///     from rather than the path the host echoes.
    ///   - line: The line the clicked link named, which the landing opens at.
    void noteAskedRemoteFile(std::uint32_t requestID, const std::string& path, Session::FileLinkLine line = {})
    {
        if (!m_remote)
            return;
        while (m_remote->askedFileRequests.size() >= maxQueuedRemoteFiles)
            m_remote->askedFileRequests.pop_front();
        m_remote->askedFileRequests.push_back(RemoteParts::AskedFile { requestID, path, line });
    }

    /// Queues a file the host answered with and publishes that it arrived.
    ///
    /// Only an answer to a question this session asked is taken. A host is the
    /// other end of a connection and can say anything on it, and what the
    /// frontend does with an answer is write it to this disk and hand it to
    /// whatever opens that kind of file — so an id nobody asked for, or one
    /// already answered, is dropped rather than landed.
    ///
    /// Queued rather than kept as one, because a reader can click two paths
    /// before either crosses and a single slot would drop the first answer.
    /// The queue is bounded, and the oldest goes first because the newest
    /// answer is the one a reader is still waiting on.
    ///
    /// - Parameter file: What the host sent back.
    void noteRemoteFile(Protocol::Control::FileReadReply&& file)
    {
        if (!m_remote)
            return;
        const auto asked = std::ranges::find(m_remote->askedFileRequests, file.requestID, &RemoteParts::AskedFile::requestID);
        if (asked == m_remote->askedFileRequests.end())
            return;
        const std::string requestedPath = asked->path;
        const Session::FileLinkLine requestedLine = asked->line;
        m_remote->askedFileRequests.erase(asked);
        while (m_remote->arrivedFiles.size() >= maxQueuedRemoteFiles)
            m_remote->arrivedFiles.pop_front();
        m_remote->arrivedFiles.push_back(RemoteParts::ArrivedFile { std::move(file), requestedPath, requestedLine });
        noteRemoteEvent(EventKind::RemoteFileArrived);
    }

    /// Records a consent request and publishes it, or puts it in line behind
    /// the question shown.
    ///
    /// The whole message is kept rather than its scalars: the prompt has to
    /// name who is asking, and a frontend that showed only a process id would
    /// be asking the user to authorise a number. Nothing is answered for a
    /// question the person has not seen: an answer from any recipient decides
    /// it for all of them. One that finds the line full, or arrives once this
    /// session can show no more, is declined as unshown, which leaves it to
    /// the others asked and has the host refuse it once none is left.
    ///
    /// - Parameter request: Who is asking and for what.
    void noteConsentRequest(const Ubiquitty::Protocol::Sync::ConsentQuestion& request)
    {
        const auto isSame = [&](const Ubiquitty::Protocol::Sync::ConsentQuestion& held) {
            return held.requestID == request.requestID && held.sessionID == request.sessionID && held.action == request.action;
        };
        if ((m_pendingConsentToken && !m_pendingHostEffectToken && isSame(m_pendingConsent))
            || std::ranges::any_of(m_queuedConsents, [&](const auto& waiting) { return waiting && isSame(waiting->question); }))
            return;
        if (m_pendingConsentToken) {
            const auto slot = std::ranges::find_if(m_queuedConsents, [](const auto& waiting) { return !waiting.has_value(); });
            if (slot != m_queuedConsents.end())
                *slot = QueuedConsent { .order = m_nextQueuedConsentOrder++, .question = request };
            else
                declineUnshownQuestion(request.requestID, static_cast<Protocol::Sync::ConsentKind>(request.action));
            return;
        }
        if (m_nextConsentToken == 0) {
            declineUnshownQuestion(request.requestID, static_cast<Protocol::Sync::ConsentKind>(request.action));
            return;
        }
        m_pendingConsent = request;
        m_pendingConsentToken = m_nextConsentToken++;
        // No scalar rides the event: every field a prompt needs is read from
        // the message beside it, and carrying two of them in the flag would
        // put the id in two places that can disagree. A structure question
        // shown keeps the screen until it is answered.
        if (!m_isStructureConsentShown)
            noteRemoteEvent(EventKind::ConsentRequestPending);
    }

    /// Asks the person about an effect a host on another machine delivered,
    /// as a capability question this client asks of its own policy.
    ///
    /// The question names this device, as a program the person runs from it
    /// asking about its clipboard, banners, speakers or window. The endpoint
    /// owns the held effects, one of each kind; this shows one question at a
    /// time and asks the endpoint for the next when it closes. A newer effect
    /// of the kind already asked about takes that question.
    ///
    /// - Parameters:
    ///   - token: The effect ``Session::RemoteSessionCore`` holds.
    ///   - capability: What it would do.
    void noteHostEffectHeld(std::uint64_t token, Capability capability)
    {
        if (m_pendingHostEffectToken && m_pendingConsent.capabilityCode == std::to_underlying(capability)) {
            m_pendingHostEffectToken = token;
            return;
        }
        if (m_pendingConsentToken == 0 && !m_isStructureConsentShown)
            showNextConsent();
    }

    /// Closes the question shown for a held effect that left unanswered.
    ///
    /// This is the one place a held effect's question closes, whatever took
    /// the effect. A newer one of its kind replaced it: the next question is
    /// shown. Its connection ended: the next is shown a turn later, so a
    /// change of state that ends the connection's questions with it, which
    /// follows at once, ends them first and nothing is shown in between; a
    /// workspace re-establishing the pane, which changes no state, shows what
    /// waited behind it then.
    ///
    /// - Parameters:
    ///   - token: The withdrawn effect.
    ///   - reason: Whether a newer one replaced it or its connection ended.
    void noteHostEffectWithdrawn(std::uint64_t token, Protocol::Sync::HostEffectWithdrawal reason)
    {
        if (!m_pendingHostEffectToken || m_pendingHostEffectToken != token)
            return;
        m_pendingConsent = {};
        m_pendingConsentToken = 0;
        m_pendingHostEffectToken = 0;
        if (reason == Protocol::Sync::HostEffectWithdrawal::Replaced) {
            showNextConsent();
            return;
        }
        noteRemoteEvent(EventKind::ConsentRequestPending);
        if (m_remote) {
            m_remote->dispatcher.post([weak = weakPtr()] {
                TerminalSession* session = weak.get();
                if (session && session->m_pendingConsentToken == 0 && !session->m_isStructureConsentShown)
                    session->showNextConsent();
            });
        }
    }

    /// Tells the host this session will never show a question, without
    /// answering it for the person.
    ///
    /// - Parameters:
    ///   - requestID: The host's question.
    ///   - kind: Its kind.
    void declineUnshownQuestion(std::uint32_t requestID, Protocol::Sync::ConsentKind kind)
    {
        if (m_remote && m_remote->core)
            m_remote->core->respondToConsent(requestID, kind, std::to_underlying(Protocol::Sync::ConsentChoice::Unshown));
    }

    /// Puts the next question waiting in the free slot: the host's, in the
    /// order they came, then the held effect that has waited longest.
    void presentNextQuestion()
    {
        if (m_pendingConsentToken)
            return;
        if (m_nextConsentToken == 0) {
            // Nothing more can be shown: every held effect is refused, and the
            // host's questions are declined as unshown.
            for (const auto& waiting : std::exchange(m_queuedConsents, {})) {
                if (waiting)
                    declineUnshownQuestion(waiting->question.requestID, static_cast<Protocol::Sync::ConsentKind>(waiting->question.action));
            }
            while (m_remote && m_remote->core) {
                const auto held = m_remote->core->oldestHeldHostEffect();
                if (!held)
                    break;
                m_remote->core->answerHeldHostEffect(held->token, Protocol::Sync::ConsentChoice::Deny);
            }
            return;
        }
        const auto queued = std::ranges::min_element(m_queuedConsents, [](const auto& left, const auto& right) {
            return left && (!right || left->order < right->order);
        });
        if (queued != m_queuedConsents.end() && queued->has_value()) {
            m_pendingConsent = (*queued)->question;
            queued->reset();
            m_pendingConsentToken = m_nextConsentToken++;
            return;
        }
        const auto held = m_remote && m_remote->core ? m_remote->core->oldestHeldHostEffect() : std::nullopt;
        if (!held)
            return;
        m_pendingConsent = Protocol::Sync::ConsentQuestion {
            .requestID = 0,
            .sessionID = m_remote->core->sessionID(),
            .peerProcessID = 0,
            .peerProcess = {},
            .agentLabel = {},
            .action = std::to_underlying(Protocol::Sync::ConsentKind::Capability),
            .capabilityCode = std::to_underlying(held->capability),
            .capabilityOrigin = std::to_underlying(CapabilityOrigin::Remote),
            .isRecipientsOwnRequest = true,
        };
        m_pendingConsentToken = m_nextConsentToken++;
        m_pendingHostEffectToken = held->token;
    }

    /// Has a pane's endpoint read this session's own Remote-origin verdicts
    /// at each effect its host delivers, so a settings change reaches an open
    /// connection.
    ///
    /// The session outlives the endpoint it owns, so the configuration it
    /// borrows does too.
    ///
    /// - Parameter core: The pane's endpoint.
    void readHostEffectDecisions(Session::RemoteSessionCore& core)
    {
        core.setHostEffectDecisions([&configuration = m_configuration](Capability capability) {
            return Preferences::Effective::capabilityDecision(configuration, CapabilityOrigin::Remote, capability);
        });
    }

    /// Where the host a carrier built from these commands runs.
    ///
    /// - Parameters:
    ///   - shellCommand: The stream carrier's command.
    ///   - quicHost: The QUIC carrier's host.
    ///   - quicBootstrapCommand: The QUIC bootstrap's command.
    /// - Returns: This machine only for this machine's own bridge.
    [[nodiscard]] static Protocol::Sync::HostLocation hostLocationOfPlan(std::string_view shellCommand, std::string_view quicHost, std::string_view quicBootstrapCommand)
    {
        return Session::isLocalTransportPlan(shellCommand, quicHost, quicBootstrapCommand) ? Protocol::Sync::HostLocation::ThisMachine
                                                                                           : Protocol::Sync::HostLocation::AnotherMachine;
    }

    /// Publishes the next question without replacing one already displayed.
    void showNextConsent()
    {
        m_isStructureConsentShown = m_pendingConsentToken == 0 && !m_structureConsents.empty();
        if (!m_isStructureConsentShown)
            presentNextQuestion();
        noteRemoteEvent(EventKind::ConsentRequestPending);
    }

    /// Keeps bounded immutable questions.
    ///
    /// One that cannot be shown is declined as unshown, never answered unseen:
    /// an answer from any recipient decides it for all of them.
    ///
    /// - Parameter request: The question routed to this attached pane.
    void noteStructureConsent(const Protocol::Sync::StructureConsent& request)
    {
        if (std::ranges::any_of(m_structureConsents, [&](const PendingStructureConsent& held) { return held.request.requestID == request.requestID; }))
            return;
        Security::StructureConsentText words = Security::structureConsentText(request);
        if (m_structureConsents.size() >= Protocol::Sync::maxPendingStructureConsents || m_nextConsentToken == 0 || words.question.empty()) {
            declineUnshownQuestion(request.requestID, Protocol::Sync::ConsentKind::Structure);
            return;
        }
        m_structureConsents.push_back({ .token = m_nextConsentToken++, .request = request, .words = std::move(words) });
        if (!m_isStructureConsentShown && m_pendingConsentToken == 0)
            showNextConsent();
    }

    /// Dismisses a host-retired question and publishes whichever stands next.
    ///
    /// - Parameter resolution: The exact kind, request and session retired.
    void noteConsentResolved(const Protocol::Sync::ConsentResolution& resolution)
    {
        if (resolution.kind != Protocol::Sync::ConsentKind::Structure) {
            for (auto& waiting : m_queuedConsents) {
                if (waiting && waiting->question.requestID == resolution.requestID && waiting->question.sessionID == resolution.sessionID
                    && waiting->question.action == std::to_underlying(resolution.kind))
                    waiting.reset();
            }
            if (m_pendingHostEffectToken || m_pendingConsent.requestID != resolution.requestID || m_pendingConsent.sessionID != resolution.sessionID
                || m_pendingConsent.action != std::to_underlying(resolution.kind))
                return;
            m_pendingConsent = {};
            m_pendingConsentToken = 0;
            if (!m_isStructureConsentShown)
                showNextConsent();
            return;
        }
        const auto matches = [&](const PendingStructureConsent& held) {
            return held.request.requestID == resolution.requestID && held.request.sessionID == resolution.sessionID;
        };
        const bool wasDisplayed = m_isStructureConsentShown && matches(m_structureConsents.front());
        std::erase_if(m_structureConsents, matches);
        if (wasDisplayed)
            showNextConsent();
    }

    /// Folds one remote event into the session's flag surface and wakes the
    /// frontend's pump — the same path a local drain's events take.
    ///
    /// - Parameters:
    ///   - kind: The event.
    ///   - data0: The scalar the kind carries — a bell's style, a settled
    ///     push's status, a settled terminate's outcome. Zero for a kind whose
    ///     payload is read beside the flag instead.
    void noteRemoteEvent(EventKind kind, std::uint32_t data0 = 0)
    {
        if (kind == EventKind::Bell) {
            m_events.bellsRung.fetch_add(1, std::memory_order_relaxed);
            m_events.bellStyle.store(data0, std::memory_order_relaxed);
            // A bell from the far side carries its cue and nothing else: the
            // modes that escalate are the local terminal's, and leaving the
            // last local bell's flags standing would have a remote beep pop a
            // window the host never asked to raise.
            m_events.bellPopsWindow.store(false, std::memory_order_relaxed);
            m_events.bellRaisesUrgency.store(false, std::memory_order_relaxed);
            m_events.bellWantsNotification.store(false, std::memory_order_relaxed);
        }
        if (kind == EventKind::PastePushSettled)
            m_events.pastePushStatus.store(data0, std::memory_order_relaxed);
        if (kind == EventKind::HostSessionTerminateSettled)
            m_events.hostSessionTerminateOutcome.store(data0, std::memory_order_relaxed);
        m_events.pending.fetch_or(1u << static_cast<unsigned>(kind), std::memory_order_release);
        signalDrain();
    }

    /// A checkpoint, Update or records batch changed the model: publish its
    /// snapshot before announcing the screen and any changed chrome.
    void noteRemoteScreenChanged()
    {
        bool didProgressChange = false;
        bool didTabStatusChange = false;
        {
            const std::scoped_lock hold(*m_activeLock);
            if (const Ubiquitty::Terminal* term = modelTerm()) {
                didProgressChange = term->progressState != m_remoteProgressSeen.state || term->progressValue != m_remoteProgressSeen.value;
                m_remoteProgressSeen = { term->progressState, term->progressValue };
                didTabStatusChange = term->tabStatusGeneration != m_remoteTabStatusGenerationSeen;
                m_remoteTabStatusGenerationSeen = term->tabStatusGeneration;
            }
            republishModel();
        }
        if (didProgressChange)
            noteRemoteEvent(EventKind::ProgressChanged);
        if (didTabStatusChange)
            noteRemoteEvent(EventKind::TabStatusChanged);
        noteRemoteEvent(EventKind::ScreenDirty);
    }

    /// Last status generation announced from the remote replica, not the
    /// local runtime's model. One screen batch can also carry a record edge.
    std::uint32_t m_remoteTabStatusGenerationSeen = 0;

    /// Last progress announced, independently of snapshot publication: another
    /// operation may publish the replica before its screen event is drained.
    OSC::ProgressSample m_remoteProgressSeen;

    /// The host reported its program title: the model takes it, and the frontend
    /// reads it back through the same accessor a local OSC title feeds.
    ///
    /// - Parameter utf8: The title bytes.
    void applyRemoteTitle(std::string_view utf8)
    {
        {
            const std::scoped_lock hold(*m_activeLock);
            if (Ubiquitty::Terminal* term = modelTerm())
                term->titleUTF8.assign(std::string_view(utf8.data(), utf8.size()));
        }
        noteRemoteEvent(EventKind::TitleChanged);
    }

    /// Takes a host pane's facts, whole, without replacing the program's OSC title.
    ///
    /// The record replaces the last one outright: the host sends every field
    /// each time, so a field this frame leaves empty is empty on the host too
    /// rather than a part some other frame owns.
    ///
    /// - Parameter facts: The record the host sent.
    void applyRemoteTitleParts(const Ubiquitty::Protocol::Sync::PaneFacts& facts)
    {
        const bool didDirectoryChange = m_paneFacts.currentDirectory != facts.currentDirectory;
        const bool didProcessChange = m_paneFacts.processChain != facts.processChain
            || m_paneFacts.processChainWithArguments != facts.processChainWithArguments
            || m_paneFacts.foreground.name != facts.foreground.name
            || m_paneFacts.isAtPrompt != facts.isAtPrompt
            || m_paneFacts.end.didEnd != facts.end.didEnd
            || m_paneFacts.shellCommandName != facts.shellCommandName
            || m_paneFacts.terminalName != facts.terminalName;
        m_paneFacts = facts;
        if (didDirectoryChange)
            noteRemoteEvent(EventKind::DirectoryChanged);
        if (didProcessChange)
            noteRemoteEvent(EventKind::ProcessStateChanged);
        if (didDirectoryChange || didProcessChange)
            noteRemoteEvent(EventKind::TitleChanged);
    }

    /// The last pane facts the host sent; see ``applyRemoteTitleParts``.
    Ubiquitty::Protocol::Sync::PaneFacts m_paneFacts;

    /// The host said the session ended: its facts frame and its end can
    /// arrive in either order, and from the end on nothing runs there.
    void noteHostedSessionEnded()
    {
        m_didHostedSessionEnd = true;
        noteRemoteEvent(EventKind::Exited);
    }

    /// Whether the host has said the session ended; see ``noteHostedSessionEnded``.
    bool m_didHostedSessionEnd = false;

    // Wall micros at the previous image-frame advance, cyclic; zero when the
    // clock awaits its seed. Main-thread state, like the gesture's.
    std::uint32_t m_lastImageAdvanceMicros = 0;
    bool m_isSyncCaptureArmed = false;
    // Three owned captures: one the drive reads, one ready, and one the ingest
    // thread can write without waiting for either.
    std::array<Rendering::FrameCapture*, SyncFrameSlot::slotCount> m_syncCaptures {};
    SyncFrameSlot m_syncSlot;
    /// Settles that found a standing capture and replaced it rather than
    /// announcing over it. A rising count with no lingering glyph is the
    /// replacement doing its work; the same count against a visible linger
    /// means the replacement itself is being stranded somewhere later.
    std::atomic<std::uint64_t> m_staleCaptureReplacedCount { 0 };
    // Delta lineage of the boundary captures. The baseline and the mask scratch
    // are the ingest thread's, under the model lock. The owed words are the rows
    // published but not yet taken: the ingest thread sets bits before it
    // publishes, the render thread clears the taken frame's own bits, so a
    // capture superseded in the slot leaves its rows in the next capture's mask.
    Rendering::RetainedFrameBaseline m_retainedBaseline;
    std::vector<std::uint8_t> m_syncRowDirty;
    std::array<std::atomic<std::uint64_t>, (maxRows + 63) / 64> m_syncOwedRows {};
    std::uint64_t m_lastSyncFrameSequence = 0;

    /// Marks every row set in `rows` as owed to the next sync capture.
    ///
    /// A capture superseded in the slot hands its rows on rather than losing
    /// them, so the next capture's mask names them beside its own.
    ///
    /// - Parameters:
    ///   - rows: One byte per row, non-zero where the row is dirty.
    void oweSyncRows(std::span<const std::uint8_t> rows)
    {
        for (std::size_t row = 0; row < rows.size() && row < static_cast<std::size_t>(maxRows); ++row) {
            if (rows[row] != 0)
                m_syncOwedRows[row / 64].fetch_or(std::uint64_t { 1 } << (row % 64), std::memory_order_acq_rel);
        }
    }

    /// Marks the rows `firstRow` through `lastRow` owed to the next sync capture.
    ///
    /// - Parameters:
    ///   - firstRow: The first row owed.
    ///   - lastRow: The last row owed, inclusive.
    void oweSyncRowRange(int firstRow, int lastRow)
    {
        for (int row = std::max(firstRow, 0); row <= lastRow && row < maxRows; ++row)
            m_syncOwedRows[static_cast<std::size_t>(row) / 64].fetch_or(std::uint64_t { 1 } << (row % 64), std::memory_order_acq_rel);
    }

    /// Whether any of the first `rows` rows is owed to the next sync capture.
    ///
    /// - Parameter rows: The visible row count.
    /// - Returns: Whether a row is owed.
    [[nodiscard]] bool hasOwedSyncRows(std::size_t rows) const
    {
        for (std::size_t row = 0; row < rows && row < static_cast<std::size_t>(maxRows); ++row) {
            if ((m_syncOwedRows[row / 64].load(std::memory_order_acquire) >> (row % 64)) & 1u)
                return true;
        }
        return false;
    }

    /// Marks every row owed, so the next sync capture is a full one.
    void oweAllSyncRows()
    {
        for (auto& word : m_syncOwedRows)
            word.store(~std::uint64_t { 0 }, std::memory_order_release);
    }

    /// Folds the owed rows into `rows`, so a capture about to be taken names
    /// them beside its own damage.
    ///
    /// - Parameters:
    ///   - rows: One byte per row; set to 1 where a row is owed, left as it
    ///     is elsewhere.
    void mergeOwedSyncRows(std::span<std::uint8_t> rows)
    {
        for (std::size_t row = 0; row < rows.size() && row < static_cast<std::size_t>(maxRows); ++row) {
            if ((m_syncOwedRows[row / 64].load(std::memory_order_acquire) >> (row % 64)) & 1u)
                rows[row] = 1;
        }
    }

    /// Clears the debt for every row `capture` carries as dirty: a row the
    /// capture names is owed no longer.
    ///
    /// - Parameters:
    ///   - capture: The sync capture that was taken.
    void settleOwedSyncRows(const Rendering::FrameCapture& capture)
    {
        const int rows = std::min(capture.rows(), maxRows);
        // A taken capture's blit is performed, so the rows it moves, owed in
        // case it was superseded, are settled with the rows it repaints. A
        // capture only ever scrolls over no older debt; it records Full
        // otherwise.
        const bool isScrolled = capture.scrollDelta() != 0;
        for (int row = 0; row < rows; ++row) {
            const bool isMoved = isScrolled && row >= capture.scrollTop() && row <= capture.scrollBottom();
            if (capture.isRowDirty(row) || isMoved)
                m_syncOwedRows[static_cast<std::size_t>(row) / 64].fetch_and(~(std::uint64_t { 1 } << (row % 64)), std::memory_order_acq_rel);
        }
    }
    std::atomic<std::uint64_t> m_syncCaptureAttemptCount { 0 };
    // The visual bell's envelope start: written on the frontend's thread when
    // a cue asks for a flash, read on the ingest thread by the boundary
    // capture and on the drive's by the live one.
    std::atomic<std::uint32_t> m_bellFlashStartTick { 0 };
    std::atomic<std::uint64_t> m_syncFramePublicationCount { 0 };
    std::atomic<std::uint64_t> m_syncFrameNoSlotCount { 0 };
    std::atomic<std::uint64_t> m_syncFrameBoundaryAnnounceCount { 0 };
    std::atomic<bool> m_isRenderWaiting { false };
    std::uint64_t m_wideLinkScanCount = 0;
    std::atomic<std::uint64_t> m_drainedBytes { 0 };
    // The perf log's delivery tally, fed on the loop's thread while asked for.
    std::atomic<bool> m_isTallyingPerfDrains { false };
    std::atomic<std::uint64_t> m_perfDrainBytes { 0 };
    std::atomic<std::uint32_t> m_perfDrainDeliveries { 0 };
    std::atomic<std::uint64_t> m_perfDrainIngestMicroseconds { 0 };
    std::atomic<std::uint32_t> m_perfDrainLongestIngestMicroseconds { 0 };
    // Rows of history the surface presents above the tail; the animation
    // schedule's conservative blink arm reads it (see setPresentedViewOffset).
    std::atomic<int> m_presentedViewOffset { 0 };
    /// The network cost the frontend last reported, for a remote session
    /// started after it; absent until a frontend with a source reports one.
    std::optional<PAL::NetworkCost> m_networkCost;
    // Clients adopted for the session's life (adoptClientOwnership).
    // Declared above the runtime so they are destroyed after it: the
    // Terminal inside holds CheckedPtrs to them, and a client dying first
    // is the dangling reference the pointer type exists to trap.
    /// Keeps a client and its external-model detach operation together.
    struct OwnedClient {
        std::unique_ptr<Ubiquitty::ClipboardClient> client;
        void (*detach)(Ubiquitty::Terminal&, Ubiquitty::ClipboardClient&);
    };
    std::vector<OwnedClient> m_ownedClients;
    std::mutex m_modelLock;
    // The lock this session actually takes. Normally its own; an adopted
    // render adapter repoints it at the external owner's, because a second
    // lock over one model is two opinions about exclusivity. The drain host
    // keeps binding m_modelLock directly: an adopted session never starts
    // ingest, and its own runtime terminal is an unused publish anchor.
    std::mutex* m_activeLock = &m_modelLock;
    /// The find search and the terminal and lock it was made for; detached
    /// when it is replaced and when the session ends.
    struct SearchBinding {
        std::shared_ptr<Ubiquitty::Search::SearchController> controller;
        Ubiquitty::Terminal* terminal = nullptr;
        std::mutex* lock = nullptr;
    };
    SearchBinding m_search;
    // Non-owning borrow of an externally owned Terminal (null for an owning
    // session). When set, every model accessor above reads and mutates it,
    // and teardown never disposes it — the owner does.
    Ubiquitty::Terminal* m_externalTerminal = nullptr;

    /// The terminal every model accessor operates on: the adopted one, else
    /// the runtime's own.
    ///
    /// - Returns: The presented terminal.
    [[nodiscard]] Ubiquitty::Terminal* modelTerm() { return paneLink().view().terminal; }

    /// Publishes the read-snapshot for the model this session presents; the
    /// caller holds the active lock.
    ///
    /// An adopted terminal publishes through the external-term path, because
    /// the runtime's own capture would read the unused anchor.
    void republishModel()
    {
        if (m_externalTerminal)
            m_runtime->republishSnapshotForTerm(m_externalTerminal);
        else
            m_runtime->republishSnapshot();
    }

    /// Takes the rising edge of a foreground program that asks to host its
    /// windows here — a typed `tmux -CC` — and starts hosting them.
    ///
    /// The strict test is the process table's: the foreground tmux's command
    /// line carries `-CC`. A plain `tmux attach` must not trip it, or the
    /// source's kickstart command would be typed into that client's pane.
    /// An edge, so a session that stopped hosting starts again only after the
    /// program left the foreground and came back. Ingest thread; the source
    /// is made on the frontend's loop, where every other use of it runs.
    void noteForegroundForHostedWindows()
    {
        const bool isHostingProgram = m_runtime->isForegroundControlModeTmux();
        const bool isRisingEdge = isHostingProgram && !m_wasForegroundHostingProgram;
        m_wasForegroundHostingProgram = isHostingProgram;
        if (!isRisingEdge || !s_hostedWindowSourceFactory.load(std::memory_order_acquire) || !s_hostedWindowLoop.isComplete())
            return;
        PostedDispatcher dispatcher(s_hostedWindowLoop);
        dispatcher.post([weak = weakPtr()] {
            if (TerminalSession* session = weak.get())
                session->beginHostingWindows();
        });
    }

    /// Makes and attaches the hosting source over this session's terminal
    /// and channel. Frontend loop.
    ///
    /// - Parameter initialControlState: Whether commands can use an established
    ///   control stream or must wait for authentication and its first handshake.
    void beginHostingWindows(HostedControlState initialControlState = HostedControlState::Established)
    {
        const HostedWindowSourceFactory factory = s_hostedWindowSourceFactory.load(std::memory_order_acquire);
        if (!factory || m_hostedWindows || m_remote || m_externalTerminal)
            return;
        Ubiquitty::Terminal* term = modelTerm();
        PAL::Stream* stream = m_runtime->stream();
        if (!term || !stream)
            return;
        HostedWindowSeams seams;
        seams.primary = term;
        seams.primaryLock = &m_modelLock;
        seams.stream = stream;
        seams.configuration = &m_configuration;
        seams.initialControlState = initialControlState;
        seams.sessionContext = this;
        seams.wakeIngest = [](void* context) { static_cast<TerminalSession*>(context)->wakeIngest(); };
        seams.announce = [](void* context) {
            static_cast<TerminalSession*>(context)->m_events.pending.fetch_or(
                1u << static_cast<unsigned>(EventKind::HostedWindowsChanged), std::memory_order_release);
        };
        seams.wakePresentation = [](void* context) {
            auto* session = static_cast<TerminalSession*>(context);
            session->signalDrain();
        };
        seams.clockMicroseconds = [](void* context) { return static_cast<TerminalSession*>(context)->m_runtime->frameClockMicroseconds(); };
        seams.clockTicks = [](void*) { return PAL::monotonicTicks(); };
        seams.loop = s_hostedWindowLoop;
        m_hostedWindows = factory(seams);
        if (m_hostedWindows)
            m_hostedWindows->attach();
    }

    /// Makes this session's own clients the ones an adopted terminal calls,
    /// leaving any slot another owner already filled. Held under the adopted
    /// lock by the caller.
    ///
    /// - Parameter terminal: The terminal being adopted.
    void attachSessionClients(Ubiquitty::Terminal& terminal)
    {
        if (!terminal.dragDropClient.installed()) {
            m_dragDropQueue.setWake([](void* context) {
                auto& session = *static_cast<TerminalSession*>(context);
                session.m_events.pending.fetch_or(1u << static_cast<unsigned>(EventKind::DragDropPending), std::memory_order_release);
            },
                this);
            terminal.dragDropClient = &m_dragDropQueue;
        }
        if (!terminal.windowClient.installed()) {
            m_windowClient.session = this;
            terminal.windowClient = &m_windowClient;
        }
        if (terminal.shellClient.get() == nullptr) {
            m_shellClient.session = this;
            terminal.shellClient = &m_shellClient;
        }
        if (terminal.triggerClient.get() == nullptr) {
            m_triggerClient.session = this;
            terminal.triggerClient = &m_triggerClient;
        }
    }

    /// Takes this session's clients back out of a terminal it stops adopting:
    /// the terminal outlives the session, and a slot left pointing at one of
    /// its members is the CheckedPtr abort at the session's teardown. Held
    /// under the adopted lock by the caller.
    ///
    /// - Parameter terminal: The terminal being released.
    void detachSessionClients(Ubiquitty::Terminal& terminal)
    {
        if (terminal.dragDropClient.isInstalled(&m_dragDropQueue)) {
            DragDrop::reset(terminal);
            terminal.dragDropClient = nullptr;
        }
        if (terminal.windowClient.isInstalled(&m_windowClient))
            terminal.windowClient = nullptr;
        if (terminal.shellClient.get() == &m_shellClient)
            terminal.shellClient = nullptr;
        if (terminal.triggerClient.get() == &m_triggerClient)
            terminal.triggerClient = nullptr;
    }

    // One per session, because the scan's scratch is what makes a hover free
    // of allocation and a session is what a hover happens over.
    Ubiquitty::Screen::LinkResolver m_linkResolver;
    std::uint64_t m_resolvedLinkTextDigest = 0;
    std::uint16_t m_resolvedLinkID = 0;

    // The user's [keys] bindings and the chord state machine over them. Per
    // session because a half-typed leader belongs to the surface the user is
    // typing at, not to the process.
    Ubiquitty::Input::BindingTable m_bindingTable;
    std::unique_ptr<Ubiquitty::Input::ChordResolver> m_chordResolver;
    Ubiquitty::Input::ComposeState m_compose;
    Ubiquitty::Input::BindingAction m_resolvedBinding;

    // Borrowed, never owned: the maker keeps the instance alive past this
    // session, and every preference read below goes through it rather than
    // the process-wide accessor.
    std::optional<LaunchCommand> m_relaunchCommand;
    const Preferences::Values& m_configuration;
    std::mutex m_drainSignalMutex;
    std::condition_variable m_drainSignalIdle;
    std::size_t m_drainSignalsInFlight = 0;
    void* m_drainSignalContext = nullptr;
    DrainSignal m_drainSignal = nullptr;
    // Whether a drain signal has fired that no ``takeEventFlags`` has answered yet.
    std::atomic<bool> m_isDrainSignalArmed { false };
    // The reader thread's nesting depth of ``ModelReadScope``; that thread's alone.
    int m_modelReadDepth = 0;
    PacedDrainHost m_host;
    /// Caches theme identity against all inputs that determine its catalog match.
    ///
    /// Unchanged host snapshots retain both positive and unmatched answers.
    struct ThemeIdentity {
        TermPalette baseline;
        Accessibility::PaletteAdaptation adaptation;
        uint64_t generation;
        // The model's palette epoch the baseline above was read at.
        uint64_t paletteEpoch;
        int index;
    };
    std::mutex m_themeIdentityLock;
    std::optional<ThemeIdentity> m_themeIdentity;
    // Equal palettes may have different materials; preserve the explicitly chosen name.
    std::string m_chosenThemeName;
    std::unique_ptr<TerminalRuntime> m_runtime;
    // Declared after the runtime so the loop — which references it — is
    // destroyed first; the endpoint and the thread after both, so a
    // still-running loop joins before anything it reads dies.
    IngestLoop m_loop { *m_runtime, m_host };
    std::unique_ptr<PAL::PollableByteStream> m_pollableEndpoint;
    std::jthread m_ingestThread;
} UBIQUITTY_SHARED_REFERENCE(retainTerminalSession, releaseTerminalSession);

} // namespace Ubiquitty::Runtime

/// Claims a reference to `session` on Swift's behalf.
///
/// - Parameter session: The session to retain. A null pointer is ignored.
inline void retainTerminalSession(Ubiquitty::Runtime::TerminalSession* session)
{
    if (session)
        session->swiftReferences.fetch_add(1, std::memory_order_relaxed);
}

/// Drops a reference to `session`, destroying it at zero.
///
/// - Parameter session: The session to release. A null pointer is ignored.
inline void releaseTerminalSession(Ubiquitty::Runtime::TerminalSession* session)
{
    if (session && session->swiftReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete session;
}
