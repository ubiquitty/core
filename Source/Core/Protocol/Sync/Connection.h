// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Connection.h — the host's and the client's ends of one Sync
// connection: the handshake, the screen plane's sender and receiver, pacing
// and the lanes (Docs/sync.md §6.2–6.5, §7.2–7.4).
//
// Both ends are transport-free state machines. The owner feeds them the bytes
// and datagrams it reads, with the time, and moves the bytes they produce:
// the host's stream one frame at a time through `popStreamFrame`, so the
// lanes decide what goes next, and datagrams whole.
//
// **Handshake.** The client opens with `Greet` and the wire version it speaks;
// the host answers `Welcome` with its own, or `Bye{VersionMismatch}` when the
// two differ, and a client refuses a `Welcome` at another version the same way
// (Envelope.h).
//
// **Baselines.** The host keeps, per pane per attachment, the generation it
// last sent and the one the client last acknowledged, with the row hashes of
// each, and of every generation sent since the acknowledgement.
//
// - On a stream-only connection every update is a *chain* update from the
//   sent generation, which the stream delivers in order. A client that
//   skipped generations gets the rows whose hashes moved, never a forced
//   checkpoint.
// - On a connection with datagrams, once the client has acknowledged a
//   checkpoint every update is a *superset* update from the acknowledged
//   generation: it carries every row that differs between the newest frame
//   and any generation the client may hold — the acknowledged one or one sent
//   since — so it applies on any of them, and a lost datagram costs only the
//   next update. It travels as a datagram when it fits, on the stream
//   otherwise. The acknowledged generation moves only on an `Ack`, never
//   because a datagram was queued.
// - A checkpoint goes as a bulk transfer; until the client acknowledges it,
//   updates chain from it on the stream, and the client buffers them behind
//   the transfer.
//
// **Pacing.** One window bounds every frame the connection sends: updates,
// checkpoints and the pane's typed records. An update
// is encoded only when the window is open and no update of the pane waits for
// the transport, and sent only when it fits; otherwise the attachment is owed
// and nothing queues. When an `Ack` opens the window, one fresh update goes
// from the sent generation to the newest. No more than one update leaves per
// refresh interval the client reported, two under power saving. A checkpoint
// standing in for an update is paced as the update would be, and one the
// client asked for goes at once; either way an attachment has at most one
// checkpoint body queued, so a newer one waits for the older to leave rather
// than queuing behind it.
//
// **Owed order** (Docs/sync.md §6.4). When the window opens, the panes
// holding input — written to the program after the generation last sent —
// are served first, in bind order; the rest by deficit round-robin over the
// bytes each was sent, so a flooding pane never delays the echo beside it and
// panes that all flood share the window by bytes. Service stops at the first
// frame the window refuses: a smaller frame of a later pane slipping into the
// room would delay the one ahead of it.
//
// **Records.** Pane facts and status precede the pane's screen (§7.8). Each
// published revision replaces the one still owed, so a stalled reader receives
// the newest records when its window opens. Larger typed bodies travel under
// their own announced, bounded bulk transfers.
//
// **Drift.** The client checks every update and checkpoint against the
// digest it carries, and on a mismatch asks for one checkpoint with `Resync`.
// Until a checkpoint applies it ignores updates, so a mismatch costs exactly
// one `Resync` however many updates were already in flight.
//
// The screen plane carries what `ScreenPlane.h` encodes: a checkpoint of the
// whole pane, and updates that carry the rows of both screens, the state
// blocks whose bytes moved and the history appended since the base, so a
// screen switch travels as an update. A pane whose geometry or dictionary
// epoch moved is brought up by a checkpoint.
//

#include "Core/Modes/TerminalModes.h"
#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/EffectDelivery.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/Geometry.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/HistoryWants.h"
#include "Core/Protocol/Sync/Images.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/Liveness.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeDrag.h"
#include "Core/Protocol/Sync/Pacer.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/RichTextForm.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/WireVersion.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {

/// How a frame arrived.
enum class Carrier : std::uint8_t {
    /// The reliable, ordered stream.
    Stream,
    /// A datagram, which may be lost or reordered.
    Datagram,
};

/// When input is reported written and settled (Docs/sync.md §6.7).
struct InputPolicy {
    /// The echo timer `E`: a write settles once it was written at least this
    /// long before a read of the PTY that found nothing to read.
    std::uint64_t echoSettleMicroseconds;
    /// How long after a write a host waits for an update to carry its
    /// receipt before it sends a standalone `Receipt`.
    std::uint64_t receiptDelayMicroseconds;
    /// The paste window a `Welcome` states: the most of a paste a client may
    /// have sent beyond what the host reported written.
    std::uint32_t pasteWindowBytes;
};

/// When a client suspects its transport (Docs/sync.md §6.9).
struct LivenessPolicy {
    /// The stall bound is `max(roundTripMultiple × srtt, floor)`: past it
    /// with input unanswered the client probes, and past it again with
    /// nothing heard it declares the transport `Lost`.
    std::uint32_t roundTripMultiple;
    /// The stall bound's floor.
    std::uint64_t floorMicroseconds;
    /// How long data may stay outstanding with no round trip before the
    /// client moves its transport to a fresh path.
    std::uint64_t repathMicroseconds;
};

/// The default input-settlement and receipt policy
/// (Docs/sync-measurements.md, "Input and liveness").
///
/// - Returns: The policy.
[[nodiscard]] constexpr InputPolicy measuredInputPolicy()
{
    return {
        // Comfortably above the slowest echo seen from a shell on a loaded
        // machine, for slower programs; a longer wait costs settlement time
        // only.
        .echoSettleMicroseconds = 50'000,
        // An echo's update leaves within the echo (p99 5 ms loaded) and the
        // coherence hold after input (p99 2 ms); a standalone receipt covers
        // the rest.
        .receiptDelayMicroseconds = 20'000,
        // The pane bounds unwritten paste input; the rest stays with the
        // client until the program makes room.
        .pasteWindowBytes = static_cast<std::uint32_t>(maxPendingInputBytes),
    };
}

/// The default transport-liveness policy.
///
/// - Returns: The policy.
[[nodiscard]] constexpr LivenessPolicy measuredLivenessPolicy()
{
    return {
        // Over the corpus on three shaped links a frame's wait was at most
        // 1.68 times the smoothed round trip, and at most 262 ms where the
        // round trip was under a quarter of the floor; a receipt adds the
        // upward half round trip to that.
        .roundTripMultiple = 4,
        .floorMicroseconds = 750'000,
        // Move unanswered input to a fresh transport path after this bound.
        .repathMicroseconds = 10'000'000,
    };
}

/// The generations a pane keeps the first change of, for `Update.lagMicros`;
/// the touched ring's length (Docs/sync.md §6.2).
inline constexpr std::size_t paneDirtyRingCapacity = 64;

/// What a host reads of a pane's echo, once per frame.
struct PaneEchoFacts {
    /// The program turned echo off.
    bool isSecureEntry = false;
    /// The shell reads a command line.
    bool isAtPrompt = false;
    /// What the line discipline signals on; a new terminal's where the
    /// discipline cannot be read, as behind a pipe.
    InterruptCharacters interruptCharacters = InterruptCharacters::terminalDefaults();
    std::uint32_t foregroundGeneration = 0;
    /// VERASE from the PTY; absent when disabled.
    std::optional<std::uint8_t> eraseCharacter = 0x7f;

    friend bool operator==(const PaneEchoFacts&, const PaneEchoFacts&) = default;
};

/// Whether input holds a character the pane's line discipline signals on: a
/// raw byte, or a key whose legacy byte is one. Such input typed during a
/// paste gives the paste up (Docs/sync.md §6.7).
///
/// - Parameters:
///   - intent: The input.
///   - characters: The pane's characters.
[[nodiscard]] bool isInterrupt(const InputIntent& intent, const InterruptCharacters& characters);

/// The most of a paste a client hands its transport at once: a key typed
/// behind one piece on a 1 Mbit/s uplink waits about 66 ms, well inside the
/// two round trips an interrupt may take. The host takes up to its own
/// limit, `Welcome.pasteLimits.chunkBytes`.
inline constexpr std::size_t clientPastePieceBytes = std::size_t { 8 } << 10;

/// One pane's screen plane on the host, shared by every connection attached
/// to the pane.
class PaneSource {
public:
    /// Publication identity carried independently of input, effects and authority.
    struct PublicationContinuation {
        ID128 paneID;
        std::uint64_t generation = 0;
        std::uint64_t structuralGeneration = 0;
        std::uint64_t changes = 0;
        std::uint32_t modesRevision = 0;
        std::uint64_t publishedModesHash = 0;
        std::array<std::uint64_t, paneDirtyRingCapacity> dirtySince {};
        FrameClass frameClass = FrameClass::Structural;
        ScreenPublisher::Continuation screen;
        HistoryPublisher::Continuation history;
        ImagePlacements images;
    };

    /// Captures a held publication without conflating it with whole-pane state.
    /// - Returns: Its identity and publisher continuations, or no held encodable frame.
    [[nodiscard]] std::optional<PublicationContinuation> publicationContinuation() const;

    /// Restores publication into a pristine source for the same stable pane.
    ///
    /// The owner restores the terminal first and carries input, effects, facts,
    /// geometry and authority separately. This does not activate a handover.
    /// - Parameter continuation: Owned, admitted publication state.
    /// - Returns: False without changing the source on invalid or mismatching state.
    [[nodiscard]] bool restorePublication(PublicationContinuation continuation);

    /// Complete frozen source state; effect bytes are borrowed.
    /// The terminal and effect log remain unchanged until this view is encoded.
    struct Continuation {
        PublicationContinuation publication;
        EffectLog::Continuation effects;
        InputLedger::Continuation input;
        GeometryArbiter::Continuation geometry;
        PaneInputQueue::Continuation queue;
        InputPolicy inputPolicy;
        std::uint64_t inputAwaitedGeneration = 0;
        PaneEchoFacts echoFacts;
        PaneFacts facts;
        PaneActivityFacts revisedActivity;
        StatusRecords status;
        std::vector<ID128> frameWriters;
        std::vector<SettledMark> settledAtPublish;
        std::vector<ID128> focusedDrivers;
        std::vector<ID128> focusedPersons;
        std::uint64_t personFocusGains = 0;
        std::uint64_t inputNow = 0;
    };

    /// Captures a held source with its bounded intents and paste progress.
    /// - Returns: Exact fixed/bounded continuation, or refusal without mutation.
    [[nodiscard]] std::optional<Continuation> continuation() const;

    /// Builds a private source without running input or presenting any effect.
    /// - Parameters:
    ///   - terminal: Exact restored terminal, which must outlive the result.
    ///   - continuation: Admitted state, consumed while its effect bytes remain valid.
    /// - Returns: A complete independent source, or no published owner on refusal.
    [[nodiscard]] static std::unique_ptr<PaneSource> restored(const Ubiquitty::Terminal& terminal, Continuation continuation);

    /// Creates the source of a pane.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal; it must outlive the source.
    ///   - paneID: The pane's identity, never reused.
    ///   - input: When the pane's input is reported settled.
    ///   - effects: The pane's retained-effect policy, fixed for its lifetime.
    PaneSource(const Ubiquitty::Terminal& terminal, ID128 paneID, InputPolicy input = measuredInputPolicy(), EffectPolicy effects = {})
        : m_terminal(terminal)
        , m_paneID(paneID)
        , m_effects(paneID, effects)
        , m_inputPolicy(input)
    {
    }

    /// Records that input was written to the pane's program: the frame that
    /// shows its effect is a later generation than any published yet, and
    /// until one is sent the pane is served ahead of the others.
    void noteInputWritten() { m_inputAwaitedGeneration = m_generation + 1; }

    /// The generation that may first show the input written last, or 0 when
    /// none was written.
    [[nodiscard]] std::uint64_t inputAwaitedGeneration() const { return m_inputAwaitedGeneration; }

    /// Lends the pane the writer its input goes to; until one is lent, input
    /// waits in the queue.
    ///
    /// - Parameter writer: The writer; it must outlive the source or be
    ///   withdrawn first.
    void setInputWriter(InputWriter& writer) { m_inputWriter = &writer; }

    /// Withdraws the writer; input waits until another is lent.
    void clearInputWriter() { m_inputWriter = nullptr; }

    /// Queues a client's numbered event and writes what the program takes.
    ///
    /// - Parameters:
    ///   - clientInstance: The client that numbered it.
    ///   - inputSeq: Its number.
    ///   - intent: The event.
    ///   - now: The time.
    ///   - authority: The server attachment that admitted the event.
    ///   - room: The most its sender may add, which its connection's bound
    ///     across all of its panes leaves.
    /// - Returns: What the queue did with it.
    EnqueueOutcome queueInput(ID128 clientInstance, std::uint64_t inputSeq, InputIntent intent, std::uint64_t now, ID128 authority = {}, std::size_t room = SIZE_MAX);

    /// Gives up a client's paste in progress, as its `PasteAbort` asks, and
    /// writes what the program takes: the bracket's end, then what the client
    /// typed after.
    ///
    /// - Parameters:
    ///   - clientInstance: The client.
    ///   - inputSeq: The paste's number; another paste's changes nothing.
    ///   - now: The time.
    ///   - authority: The server attachment that owns the paste.
    void abortPaste(ID128 clientInstance, std::uint64_t inputSeq, std::uint64_t now, ID128 authority = {});

    /// Cancels the exact retired attachment's queued input and stream suffix.
    /// - Parameters:
    ///   - authority: Server attachment identity, independent of replay identity.
    ///   - now: Monotonic retirement time.
    void withdrawInputAuthority(ID128 authority, std::uint64_t now);

    /// Gives up whatever paste a client has in progress, its attachment gone,
    /// so the program is not left in paste mode.
    ///
    /// - Parameters:
    ///   - clientInstance: The client.
    ///   - now: The time.
    void abandonPaste(ID128 clientInstance, std::uint64_t now);

    /// Records what the host reads of the pane's echo, once per frame before
    /// it is published: the program's line discipline, the shell's prompt
    /// and the foreground program.
    ///
    /// - Parameter facts: The facts.
    void noteEchoFacts(const PaneEchoFacts& facts) { m_echoFacts = facts; }

    /// Takes the published frame again under the next generation, with no
    /// capture: an update to it says no row, and carries what rides an
    /// update and changed with no frame, the echo context.
    ///
    /// - Parameter now: The time, which the generation's lag is counted from.
    void republish(std::uint64_t now);

    /// The echo context a client is told: the facts noted, the modes'
    /// revision, and whether another client's input reached the program
    /// while the published frame was drawn.
    ///
    /// - Parameter clientInstance: The client.
    /// - Returns: The context.
    [[nodiscard]] EchoContext echoContextFor(ID128 clientInstance) const;

    /// The bytes taken of a client's paste in progress.
    ///
    /// - Parameters:
    ///   - clientInstance: The client.
    ///   - inputSeq: The paste's number.
    /// - Returns: The bytes, or `nullopt` when it has none in progress.
    [[nodiscard]] std::optional<std::uint64_t> pasteTaken(ID128 clientInstance, std::uint64_t inputSeq) const { return m_inputQueue.pasteTaken(clientInstance, inputSeq); }

    /// Writes what waits, as far as the program takes it: the owner calls this
    /// when the program's input has room again.
    ///
    /// - Parameter now: The time.
    void drainInput(std::uint64_t now);

    /// What was written to the pane, per client instance.
    [[nodiscard]] const InputLedger& inputLedger() const { return m_inputLedger; }

    /// Whose size the pane takes (Docs/sync.md §6.10): every attachment's
    /// typing and viewport reach it, and the owner reads what is due.
    [[nodiscard]] GeometryArbiter& geometry() { return m_geometry; }

    /// Whose size the pane takes.
    [[nodiscard]] const GeometryArbiter& geometry() const { return m_geometry; }

    /// Records a read of the program's output that found nothing: what was
    /// written at least `E` before it began is settled.
    ///
    /// - Parameter readBegan: When the read began, never when it ended.
    void noteReadWouldBlock(std::uint64_t readBegan) { m_inputLedger.noteReadWouldBlock(readBegan, m_inputPolicy.echoSettleMicroseconds); }

    /// When the oldest unsettled write may first settle: the owner checks
    /// the program's output for quiet then, as a read that finds nothing.
    [[nodiscard]] std::optional<std::uint64_t> nextSettleMicroseconds() const { return m_inputLedger.nextSettleMicroseconds(m_inputPolicy.echoSettleMicroseconds); }

    /// How far an instance's input was settled when the newest generation
    /// was published: the value a frame of that generation reports, since
    /// its picture holds every echo settled by then.
    ///
    /// - Parameter clientInstance: The client.
    /// - Returns: Its number, 0 when none.
    [[nodiscard]] std::uint64_t settledAtPublish(ID128 clientInstance) const;

    /// The input bytes the pane holds unwritten.
    [[nodiscard]] std::size_t heldInputBytes() const { return m_inputQueue.heldBytes(); }

    /// The input bytes the pane holds unwritten for one client instance.
    ///
    /// - Parameter clientInstance: The client.
    [[nodiscard]] std::size_t heldInputBytes(ID128 clientInstance) const { return m_inputQueue.heldBytes(clientInstance); }

    /// How much of a client's paste in progress the program was written.
    ///
    /// - Parameter clientInstance: The client.
    /// - Returns: The paste and its bytes written, or `nullopt` when the
    ///   client has no paste in progress.
    [[nodiscard]] std::optional<PaneInputQueue::PasteWritten> pasteWritten(ID128 clientInstance) const { return m_inputQueue.pasteWritten(clientInstance); }

    /// How many typed events have been written to the program
    /// (``PaneInputQueue::typedWrites``). A host compares it across a pass to
    /// learn that someone typed, once per pass rather than once per event.
    [[nodiscard]] std::uint64_t typedInputWrites() const { return m_inputQueue.typedWrites(); }

    /// Whether input waits to be written.
    [[nodiscard]] bool hasPendingInput() const { return !m_inputQueue.isEmpty(); }

    /// Records whether one attachment's driver holds the focus. A driver is
    /// focused when its last `Focus` for the pane said so and its window is
    /// focused (Docs/sync.md §6.11). The pane reports focus to its program
    /// when the first driver gains it and when the last one loses it, never
    /// for a second driver coming or going.
    ///
    /// A driver that stands as a person and gains the focus has seen the
    /// pane, which ``personFocusGains()`` counts.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment whose driver it is.
    ///   - focus: Whether that driver holds the focus.
    ///   - standing: What the driver stands as: the lesser of the kind it
    ///     declared and the kind the host verified (`standingKind`).
    void noteDriverFocus(ID128 attachmentID, Ubiquitty::Modes::FocusState focus, ClientKind standing);

    /// How many times a person's driver has gained the focus on the pane.
    /// A host compares it across a pass to learn that a person looked at the
    /// pane, as it does ``typedInputWrites()``. An agent's driver and a
    /// viewer never count: neither is a person seeing what the pane shows.
    [[nodiscard]] std::uint64_t personFocusGains() const { return m_personFocusGains; }

    /// Whether a person's driver holds the focus on the pane now: somebody
    /// is looking at it, so what its program raises is seen as it arrives.
    [[nodiscard]] bool isAnyPersonFocused() const { return !m_focusedPersons.empty(); }

    /// Whether any driver of the pane holds the focus.
    [[nodiscard]] bool isAnyDriverFocused() const { return !m_focusedDrivers.empty(); }

    /// Records a coherent frame the owner took, as the next generation.
    ///
    /// - Parameters:
    ///   - frameClass: Why the frame was taken.
    ///   - dirtySinceMicroseconds: The host time of the first change the
    ///     frame holds that the frame before did not.
    /// - Precondition: The terminal holds the frame.
    void publish(FrameClass frameClass, std::uint64_t dirtySinceMicroseconds);

    /// The host time of the first change of a generation, or of the oldest
    /// generation the ring still holds when this one has left it.
    ///
    /// - Parameter generation: The generation.
    /// - Returns: The time, 0 before the first publish.
    [[nodiscard]] std::uint64_t dirtySince(std::uint64_t generation) const;

    /// Records that the terminal has changed since the last `publish`.
    ///
    /// An update or checkpoint is encoded from the terminal as it is when it is
    /// sent, so a pane whose terminal has moved on since its last `publish`
    /// sends nothing until the next one. A pane no client shows is never
    /// published; this is how it is known to have changed.
    void noteTerminalChanged()
    {
        m_isFrameHeld = false;
        ++m_changes;
    }

    /// How many times the terminal was recorded changed or published: what a
    /// `Dormant` attachment compares to learn the pane moved.
    [[nodiscard]] std::uint64_t changes() const { return m_changes; }

    /// Whether the terminal still holds the newest published frame.
    [[nodiscard]] bool isFrameHeld() const { return m_isFrameHeld; }

    /// The newest generation, 0 before the first `publish`.
    [[nodiscard]] std::uint64_t generation() const { return m_generation; }

    /// The generation of the newest frame published `Structural`: the one a
    /// topology leaf names, since that frame gave the pane the leaf's size
    /// (Docs/sync.md §7.3). 0 before any.
    [[nodiscard]] std::uint64_t structuralGeneration() const { return m_structuralGeneration; }

    /// Why the newest frame was taken.
    [[nodiscard]] FrameClass frameClass() const { return m_frameClass; }

    /// Whether the newest frame's rows could be said by an update.
    [[nodiscard]] bool isEncodable() const { return m_isEncodable; }

    /// The pane's identity.
    [[nodiscard]] ID128 paneID() const { return m_paneID; }

    /// Borrows the pane's bounded log for authenticated attachment maintenance.
    ///
    /// - Returns: The log, whose identities outlive individual connections.
    [[nodiscard]] EffectLog& effects() { return m_effects; }
    /// Reads the pane's effect high-water marks and retained batches.
    ///
    /// - Returns: The same pane-local log without mutation authority.
    [[nodiscard]] const EffectLog& effects() const { return m_effects; }
    /// Appends a validated presentation or command event at the published picture.
    ///
    /// Clipboard requires separate payload ownership and is refused here.
    /// The caller supplies the currently elected primary instance.
    ///
    /// - Parameters:
    ///   - body: The typed non-clipboard body to validate and copy.
    ///   - primary: The elected human frontend, or zero when no primary exists.
    /// - Returns: The pane-local effect sequence, or a refusal without mutation.
    [[nodiscard]] std::expected<std::uint64_t, EffectRefusal> appendEffect(const EffectBody& body, ID128 primary);

    /// Appends the bells a program rang since the last append as one
    /// `Bell{count}` at the published picture.
    ///
    /// - Parameters:
    ///   - count: How many bells, at least one.
    ///   - primary: The elected human frontend, or zero when no primary exists.
    /// - Returns: The pane-local effect sequence, or a refusal without mutation.
    [[nodiscard]] std::expected<std::uint64_t, EffectRefusal> appendBells(std::uint32_t count, ID128 primary);

    /// Retains one complete already-authorized clipboard operation for its
    /// original human recipient. Ownership moves into the log without a data copy.
    ///
    /// - Parameters:
    ///   - buffer: The exact destination selector.
    ///   - bundle: All MIME representations, including an empty clear.
    ///   - primary: The nonzero elected recipient; delivery never retargets it.
    /// - Returns: A retained sequence, or refusal with no log mutation.
    [[nodiscard]] std::expected<std::uint64_t, EffectRefusal> appendClipboardWrite(ClipboardBuffer buffer,
        const EffectClipboardBundle& bundle, ID128 primary);

    /// Moves a validated pending clipboard bundle into the log without copying
    /// its input-sized bytes. Refusal preserves the caller's exact owner.
    ///
    /// - Parameters:
    ///   - buffer: The exact destination selector.
    ///   - encoded: Complete encoded MIME bundle, consumed only on success.
    ///   - primary: The immutable nonzero human recipient.
    /// - Returns: Retained sequence, or refusal without changing either owner.
    [[nodiscard]] std::expected<std::uint64_t, EffectRefusal> appendClipboardWrite(ClipboardBuffer buffer,
        EffectCodecBytes& encoded, ID128 primary);

    /// Captures a sound's exact sample bytes before retaining its effect.
    ///
    /// - Parameters:
    ///   - request: The parsed request borrowing its captured PCM until return.
    ///   - sampleSerial: The bank revision captured with this exact request.
    ///   - primary: The elected human frontend, or zero when none exists.
    /// - Returns: The sequence, or refusal without consuming a pending request.
    [[nodiscard]] std::expected<std::uint64_t, EffectRefusal> appendSound(const PAL::PlayTonesRequest& request, std::uint32_t sampleSerial, ID128 primary);

    /// The pane's terminal.
    [[nodiscard]] const Ubiquitty::Terminal& terminal() const { return m_terminal; }

    /// The encoder every attachment shares.
    [[nodiscard]] ScreenPublisher& publisher() { return m_publisher; }

    /// The history encoder every attachment shares.
    [[nodiscard]] HistoryPublisher& history() { return m_history; }

    /// The captured image identities and whole placement record. Pixel copies
    /// are made only after a connection has reserved their retained capacity.
    [[nodiscard]] const ImagePublisher& images() const { return m_images; }

    /// Where the history stood at the newest publish.
    [[nodiscard]] Ubiquitty::Screen::HistoryAddress historyAddress() const { return m_history.address(); }

    /// How a change to a pane's activity stamps alone is carried.
    enum class ActivityStamps : std::uint8_t {
        /// The stamps ride with whatever else next moves the record.
        RideAlong,
        /// New stamps move the record's revision by themselves; a host asks
        /// for this no more often than it means to send a frame for them.
        Publish,
    };

    /// Sets what the host says about the pane beside its picture
    /// (Docs/sync.md §7.7). The facts are cut to what a frame carries, the
    /// pane's geometry status is taken from its arbiter, and the revision
    /// moves only when something a client is shown changed.
    ///
    /// - Parameters:
    ///   - facts: The facts as the host reads them now; its `revision` and
    ///     `geometry` are ignored.
    ///   - stamps: Whether new activity stamps alone move the revision.
    void setFacts(PaneFacts facts, ActivityStamps stamps = ActivityStamps::RideAlong);

    /// Sets the pane's status records. The revision moves only when they
    /// changed.
    ///
    /// - Parameter records: The records as the host reads them now; its
    ///   `revision` is ignored.
    void setStatus(StatusRecords records);

    /// The pane's facts; revision zero until a host sets them.
    ///
    /// - Returns: The facts last set.
    [[nodiscard]] const PaneFacts& facts() const { return m_facts; }

    /// The pane's status records; revision zero until a host sets them.
    ///
    /// - Returns: The records last set.
    [[nodiscard]] const StatusRecords& status() const { return m_status; }

private:
    const Ubiquitty::Terminal& m_terminal;
    ID128 m_paneID;
    EffectLog m_effects;
    ScreenPublisher m_publisher;
    HistoryPublisher m_history;
    ImagePublisher m_images { m_terminal };
    std::uint64_t m_generation = 0;
    std::uint64_t m_structuralGeneration = 0;
    std::uint64_t m_changes = 0;
    /// The first change of each of the newest generations, by generation
    /// modulo the capacity.
    std::array<std::uint64_t, paneDirtyRingCapacity> m_dirtySince {};
    std::uint64_t m_inputAwaitedGeneration = 0;
    InputPolicy m_inputPolicy;
    PaneEchoFacts m_echoFacts;
    PaneFacts m_facts;
    /// The activity stamps as of the facts' last revision: what a client was
    /// sent. Stamps that rode along since are newer than these.
    PaneActivityFacts m_revisedActivity;
    StatusRecords m_status;
    /// Moves when the published frame's modes block changes.
    std::uint32_t m_modesRevision = 0;
    std::uint64_t m_publishedModesHash = 0;
    /// Who wrote while the published frame was drawn.
    std::vector<ID128> m_frameWriters;
    InputLedger m_inputLedger;
    GeometryArbiter m_geometry;
    /// Each instance's settled mark as the newest generation was published.
    std::vector<SettledMark> m_settledAtPublish;
    PaneInputQueue m_inputQueue;
    /// Borrowed from the pane's owner; null until it lends one.
    InputWriter* m_inputWriter = nullptr;
    /// The attachments whose driver holds the focus.
    std::vector<ID128> m_focusedDrivers;
    std::vector<ID128> m_focusedPersons;
    std::uint64_t m_personFocusGains = 0;
    /// The newest time input arrived or drained, which an unnumbered focus
    /// report drains at.
    std::uint64_t m_inputNow = 0;
    FrameClass m_frameClass = FrameClass::Structural;
    bool m_isEncodable = false;
    bool m_isFrameHeld = false;
};

/// Where a connection's handshake stands.
enum class ConnectionState : std::uint8_t {
    /// The client has not yet been welcomed.
    Greeting,
    /// Welcomed with identity only; no pane or operation authority exists.
    AwaitingConsent,
    /// A decision waits for bounded own-status delivery and fresh revalidation.
    ApprovedAwaitingDelivery,
    /// The handshake completed; screen frames flow.
    Attached,
    /// The path went silent: bytes stayed outstanding with nothing heard past
    /// the silence bound on the host, or input stayed unanswered through a
    /// probe on the client. Nothing more is read or sent; the owner drops the
    /// transport and, on the client, dials again.
    Lost,
    /// A `Bye` was sent or received; nothing more is read or sent but the `Bye`.
    Closed,
};

/// The default per-connection caps on outstanding calls, queued bulk and
/// pending ranges (Docs/sync.md §7.9).
inline constexpr Limits provisionalLimits {
    .maxOutstandingCalls = 32,
    .maxQueuedBulkBytes = 16u << 20,
    .maxPendingRanges = 64,
};

/// How the host serves a connection's history (Docs/sync.md §6.6, §7.9).
struct HistoryPolicy {
    /// The most lines one history transfer carries.
    std::uint32_t backfillChunkLines;
    /// The largest body one history transfer carries.
    std::size_t maxHistoryTransferBytes;
    /// The bulk bytes the connection may hold queued before a history
    /// transfer waits.
    std::size_t maxQueuedBulkBytes;
};

/// The default history bounds: at most 256 lines and 4 MiB in one transfer,
/// with 16 MiB of queued bulk. The cap on pending want ranges is the one
/// `Limits` advertises.
inline constexpr HistoryPolicy provisionalHistoryPolicy {
    .backfillChunkLines = 256,
    .maxHistoryTransferBytes = maxHistoryTransferBytes,
    .maxQueuedBulkBytes = std::size_t { 16 } << 20,
};

/// Builds one canonical closing message with the sender's bounded diagnostics.
///
/// - Parameters:
///   - wireVersion: The wire version the sender speaks.
///   - version: The app's presentation version, never an authority claim.
///   - platform: The sender's platform, empty when unknown.
///   - reason: Why the sender closes.
///   - detail: What it says.
/// - Returns: The `Bye` to send.
[[nodiscard]] Bye spokenBye(std::uint16_t wireVersion, std::string_view version, std::string_view platform, ByeReason reason, std::string_view detail);

/// The host's constants for one connection.
///
/// The connection owner supplies them; `measuredHostPolicy` provides the
/// defaults.
struct HostPolicy {
    PacingPolicy pacing;
    LanePolicy lanes;
    /// The largest datagram the carrier sends, or 0 when the connection has
    /// none, as over SSH.
    std::size_t datagramMaxBytes;
    /// The largest checkpoint body a transfer may carry.
    std::size_t maxCheckpointBytes;
    /// The caps the connection holds a client to, which `Welcome` advertises.
    Limits limits = provisionalLimits;
    /// How history is served.
    HistoryPolicy history = provisionalHistoryPolicy;
    /// When input is reported written and settled.
    InputPolicy input = measuredInputPolicy();
    /// The interactive bytes the connection may hold unsent before it stops
    /// taking input: what bounds a client that writes calls and never reads
    /// their answers (Docs/sync.md §6.5). Interactive frames leave as the
    /// transport takes them, never on an `Ack`, so this cannot deadlock.
    /// The owner may supply a different bound.
    std::size_t maxQueuedInteractiveBytes = std::size_t { 1 } << 20;
    /// The least time between two updates of one pane on a `Constrained`
    /// link: at most 20 updates a second by default (Docs/sync.md §6.4).
    std::uint64_t constrainedIntervalMicroseconds = 50'000;
};

/// The default host-connection policy.
///
/// - Parameter datagramMaxBytes: The largest datagram the carrier sends, 0 for
///   a carrier without datagrams.
/// - Returns: The policy.
[[nodiscard]] HostPolicy measuredHostPolicy(std::size_t datagramMaxBytes);

/// The first wait before a pane unbound for its checkpoint's size is measured
/// again, doubling with each refusal in a row.
inline constexpr std::uint64_t oversizedRetryMicroseconds = 250'000;

/// The longest wait between two measurements of an oversized pane.
inline constexpr std::uint64_t maxOversizedRetryMicroseconds = 30'000'000;

/// The checkpoint body size at and below which a body crosses as it is
/// (Docs/sync.md §7.4): deflating less saves too little to pay for.
inline constexpr std::size_t minDeflatedCheckpointBytes = 4 * 1024;

/// What the host tells a client it welcomes.
struct HostIdentity {
    ID128 hostInstanceID {};
    ID128 attachmentID {};
    ID128 resumeSecret {};
    /// The fingerprint this host states and admits; this build's unless a
    /// test stands up a host of another build.
    EngineFingerprint engineFingerprint = wireFingerprint;
    /// The most the client's credential lets its attachment be. The Welcome
    /// grants this or less: `grantRole` of what the Greet asks.
    Role maxRole = Role::Owner;
    std::string principal {};
    /// Whether the client may send input: its authority's Drive scope. A
    /// client that may not, or a `Viewer`, sending input is malformed.
    bool mayDrive = false;
    /// The most the host verified the client to be. Authority kept for a
    /// person, owning a pane's size, goes by the lesser of this and the kind
    /// the client declares (`standingKind`); an unverified client is an
    /// agent.
    ClientKind verifiedKind = ClientKind::Agent;
    /// The wire version this host speaks; this build's unless a test stands
    /// up a peer at another.
    std::uint16_t wireVersion = Sync::wireVersion;
    /// The inherited issuer for this attachment, established by the host's
    /// resume authority, not by the client's requested instance.
    std::optional<ID128> predecessorHostInstanceID = std::nullopt;
    /// The exact held attach, absent when no human decision is required.
    std::optional<ConsentBinding> pendingConsent = std::nullopt;
    /// Real serving values supplied by the physical host, never fabricated by Core.
    ServingSelection selection {};
    std::string appVersion = std::string(Sync::appVersion());
};

/// What a host connection has done, for the journal and the tests.
struct HostCounters {
    std::uint64_t checkpointsSent = 0;
    std::uint64_t chainUpdatesSent = 0;
    std::uint64_t supersetUpdatesSent = 0;
    /// `attachments` notifies sent.
    std::uint64_t attachmentsNotifiesSent = 0;
    std::uint64_t datagramsSent = 0;
    std::uint64_t datagramsLost = 0;
    /// Times an update or a Sync frame was due and the window refused it.
    std::uint64_t windowRefusals = 0;
    /// `Echo`s sent in answer to the client's probes.
    std::uint64_t echoesSent = 0;
    /// Standalone `Receipt`s: input written with no update to carry it.
    std::uint64_t receiptsSent = 0;
    /// Input events dropped as already written or queued: a resend the
    /// pane's ledger caught.
    std::uint64_t inputDuplicates = 0;
    /// Updates that carried an echo context.
    std::uint64_t echoContextsSent = 0;
    /// `PasteReceipt`s sent.
    std::uint64_t pasteReceiptsSent = 0;
    /// `InputDiscarded`s sent.
    std::uint64_t inputDiscardsSent = 0;
    /// `PaneDirty` frames sent for `Dormant` panes.
    std::uint64_t paneDirtiesSent = 0;
    /// `PanePalette` frames sent for `Dormant` panes.
    std::uint64_t palettesSent = 0;
    /// `PaneFacts` frames sent.
    std::uint64_t paneFactsSent = 0;
    /// Status sets sent whole, however many `StatusRecords` frames each took.
    std::uint64_t statusSetsSent = 0;
    /// Whole image-placement records queued, including empty replacements.
    std::uint64_t imagePlacementsSent = 0;
    /// Image heads queued with a pixel transfer, rather than an offer alone.
    std::uint64_t imageTransfersSent = 0;
    /// Decoded pixel bytes queued in immutable image transfer owners.
    std::uint64_t imagePixelBytesQueued = 0;
    /// The bytes those owners send: the decoded pixels, or their zlib stream
    /// for a client that takes deflated images.
    std::uint64_t imageWireBytesQueued = 0;
    /// `WorkspaceStructure` frames sent: structures and withdrawals, inline
    /// or announcing a transfer.
    std::uint64_t structuresSent = 0;
    /// The ones among them whose record left as a bulk transfer.
    std::uint64_t structureTransfersSent = 0;
    /// Transfers abandoned after their head left, each told with a
    /// `BulkAbort`.
    std::uint64_t bulkAborts = 0;
    /// `HistoryReset`s sent, one per clear a pane's client was told of.
    std::uint64_t historyResetsSent = 0;
    /// Checkpoints the client refused, by its `Resync`.
    std::uint64_t checkpointsRefused = 0;
    /// Times a checkpoint was due whose body a client had already refused:
    /// it is not sent again until the pane changes.
    std::uint64_t refusedCheckpointsWithheld = 0;
    /// Panes unbound because their checkpoint passed `HostPolicy::maxCheckpointBytes`.
    std::uint64_t checkpointsTooLarge = 0;
    /// Panes bound again once their checkpoint fit after such an unbind.
    std::uint64_t oversizedPanesRebound = 0;
    /// Checkpoints encoded to measure whether an unbound pane fits again.
    std::uint64_t oversizedPanesMeasured = 0;
    /// Rows, of both screens, the updates sent carried.
    std::uint64_t rowsSent = 0;
    std::uint64_t probesSent = 0;
    /// Times the silence rule moved the connection to `Lost`: at most once.
    std::uint64_t lostDeclared = 0;
    std::uint64_t resyncsReceived = 0;
    /// The generation the last `Resync` named.
    std::uint64_t lastResyncGeneration = 0;
    /// The first row the last `Resync`'s hashes disagree on with what the
    /// host sent that generation, or -1 when none is known.
    int lastResyncFirstRow = -1;
    /// History lines sent, inline or in transfers.
    std::uint64_t historyLinesSent = 0;
    /// `HistoryWant`s refused whole for taking the sets past the cap.
    std::uint64_t historyWantsRefused = 0;
    /// The bytes of the history frames sent: inline frames, heads and their
    /// bodies.
    std::uint64_t historyBytesSent = 0;
    /// Open lines sent, withdrawals included: at most one per update.
    std::uint64_t historyOpenLinesSent = 0;
    /// The body bytes of the newest checkpoint sent.
    std::uint64_t lastCheckpointBodyBytes = 0;
    /// The bytes the newest checkpoint's body took on the wire, deflated or
    /// not.
    std::uint64_t lastCheckpointWireBytes = 0;
    /// Checkpoints sent deflated.
    std::uint64_t checkpointsDeflated = 0;
    /// `Spans` ops the updates sent carried.
    std::uint64_t spansOps = 0;
    /// `ScrollRect` ops the updates sent carried.
    std::uint64_t scrollRectOps = 0;
    /// `Call`s handed to the owner to answer.
    std::uint64_t callsAdmitted = 0;
    /// `Call`s the connection answered `busy` itself: over the
    /// outstanding-call cap at arrival, or with an answer past the
    /// queued-bulk cap.
    std::uint64_t callsAnsweredBusy = 0;
};

/// The existing per-client request limit also bounds typed clipboard owners.
inline constexpr std::size_t maxPendingClipboardReads = 8;

/// Owns one exact request and, when present, its canonical answer bytes.
/// The same fixed metadata is used on either end; no view outlives its owner.
struct ClipboardReadState {
    std::uint32_t handle = 0;
    std::uint32_t requestID = 0;
    EffectCodecBytes request;
    EffectCodecBytes data;
    EffectClipboardStatus status = EffectClipboardStatus::Refused;
    std::uint32_t transfer = 0;
    std::uint32_t nextIndex = 0;
    std::size_t offset = 0;
    std::array<std::uint8_t, 32> sha256 {};
    bool hasAnswer = false;
    bool isComplete = false;
};

/// Whether a call arrived within the outstanding-call cap.
enum class CallAdmission : std::uint8_t {
    /// It is outstanding until answered, and its answer is counted until it
    /// has left.
    Admitted,
    /// The cap was reached when it arrived: it is answered `busy`, and the
    /// connection stays (Docs/sync.md §6.5).
    OverLimit,
};

/// A credited control upload may claim a chunk; other chunks retain their
/// existing exact clipboard-request admission.
enum class ClientBulkDisposition : std::uint8_t {
    Unhandled,
    Accepted,
    Refused,
};

/// A `Call` a client sent, for the owner to answer by `callIndex`.
struct ReceivedCall {
    /// The connection's own number for the call, never reused.
    std::uint32_t callIndex = 0;
    /// The JSON-RPC 2.0 request, valid UTF-8, unparsed.
    std::string text;
    CallAdmission admission = CallAdmission::Admitted;
};

/// A driver's newest `Viewport` of a pane, for the owner to read the pixels
/// it draws that pane in. The grid is already the pane's arbiter's.
struct ViewportReport {
    /// The pane's handle on the connection.
    std::uint32_t handle = 0;
    /// What the client sent, its grid clamped to what a terminal holds.
    Viewport viewport;
};

/// How much of a listed pane's facts a connection is sent.
///
/// A listing is served to a connection that may not read a pane's screen
/// (Docs/sync.md §7.3). What runs in the pane, where, and what it is called
/// are what a listing is for. A command line is not: its arguments carry
/// what a screen carries, a token or a password typed after a flag, so they
/// go only where the screen could.
enum class ListedDetail : std::uint8_t {
    /// The facts as the pane has them, for a connection that could read the
    /// pane's screen.
    Whole,
    /// The process chain with its arguments is sent as the chain of program
    /// names alone.
    WithoutArguments,
};

/// What `answerCall` did.
enum class CallAnswer : std::uint8_t {
    /// The answer is queued.
    Sent,
    /// The answer would take the queued bulk past its cap: nothing was
    /// queued, and the call stays outstanding for `refuseCall`.
    OverBulkLimit,
    /// No admitted call has the index, or the connection is not attached.
    NotOutstanding,
};

/// Why a call is answered without its method running.
enum class CallRefusal : std::uint8_t {
    /// Its text is not a JSON-RPC 2.0 request with an id.
    Unreadable,
    /// It is over the outstanding-call cap, or its answer over the
    /// queued-bulk cap.
    Busy,
};

/// The host's end of one connection.
class HostConnection {
public:
    /// Creates a connection awaiting the client's `Greet`.
    ///
    /// - Parameters:
    ///   - policy: The connection's constants.
    ///   - identity: What `Welcome` tells the client.
    HostConnection(const HostPolicy& policy, HostIdentity identity);

    /// Authenticates one strict Greet before any attachment or observation is admitted.
    ///
    /// Called once after range and body validation. The callback must not reenter,
    /// reset or destroy this connection. A physical host may mark its client for
    /// deferred removal, but returns a typed refusal without binding a pane.
    /// A table installed by serveFromTable receives the returned authority only
    /// after success; placeholder authority cannot admit an attachment.
    ///
    /// - Parameter admission: Returns the exact verified identity or one canonical Bye.
    void setGreetingAdmission(std::function<std::expected<HostIdentity, Bye>(const Greet&)> admission)
    {
        m_greetingAdmission = std::move(admission);
    }

    /// Reads bytes from the stream.
    ///
    /// - Parameters:
    ///   - bytes: The next bytes, in stream order.
    ///   - now: The host clock in microseconds; never 0, which the wire
    ///     reserves for "no time".
    void receiveStream(std::span<const std::uint8_t> bytes, std::uint64_t now);

    /// Lets the carrier charge the prospective body capacity before allocation.
    /// The callback must not destroy this connection; false refuses the frame.
    void setInboundFrameAdmission(std::function<bool(std::size_t)> admit) { m_inboundFrameAdmission = std::move(admit); }

    /// Lets the host bound the input all of its panes hold unwritten together:
    /// the callback answers how much more it takes. Unset, a sender is held
    /// only to its share of a pane and to its connection's bound.
    void setHeldInputRoom(std::function<std::size_t()> room) { m_heldInputRoom = std::move(room); }
    [[nodiscard]] std::size_t inboundFrameBytes() const { return m_decoder.retainedBytes(); }
    [[nodiscard]] std::size_t pendingInboundFrameBytes() const { return m_decoder.pendingBytes(); }

    /// Checks operations whose completion still belongs to this physical stream.
    /// Accepted pane input, images and screen baselines have independent carried
    /// owners and do not block. Calls, clipboard reads and partial messages must
    /// finish before a handover can discard this connection.
    /// - Returns: Whether replacing the connection would lose an operation.
    [[nodiscard]] bool hasHandoverBlockedOperations() const;

    /// Reads one datagram.
    ///
    /// - Parameters:
    ///   - datagram: The datagram's payload, exactly one envelope.
    ///   - now: The host clock in microseconds.
    void receiveDatagram(std::span<const std::uint8_t> datagram, std::uint64_t now);

    /// Binds a pane to this connection; its checkpoint follows on the next
    /// `service`.
    ///
    /// - Parameter source: The pane; it must outlive the connection.
    /// - Returns: The pane's handle, or 0 when the connection is not attached
    ///   or already holds `maxPanesPerConnection` panes bound.
    [[nodiscard]] std::uint32_t bindPane(PaneSource& source);

    /// Lists a pane on this connection: the client is sent the pane's facts
    /// and status set, at the listing's cadence, and nothing else of it. A
    /// listed pane has no view, no screen and no history, takes no input,
    /// counts as nobody's attachment and has no say in the pane's size: a
    /// frame the client addresses to its handle closes the connection as one
    /// for a handle nothing was bound to.
    ///
    /// - Parameters:
    ///   - source: The pane; it must outlive the listing.
    ///   - detail: How much of the pane's facts this connection is sent. It
    ///     is fixed when the pane is listed: listing a listed pane again
    ///     changes nothing.
    /// - Returns: The handle the pane is listed under, the one it already
    ///   has when it is listed, or 0 when the connection is not attached, the
    ///   client takes no listing, or `maxPanesPerConnection` are listed.
    [[nodiscard]] std::uint32_t listPane(PaneSource& source, ListedDetail detail);

    /// Ends a pane's listing and tells the client with a `PaneUnbind`.
    ///
    /// - Parameter handle: The handle `listPane` returned; another is ignored.
    void unlistPane(std::uint32_t handle);

    /// The panes listed, each with its handle.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, ID128>> listedPanes() const;

    /// Owes the client a workspace's structure. The newest revision offered
    /// replaces one not yet sent, so a workspace that changes faster than
    /// the client reads is never queued twice; a revision the client was
    /// sent, or an older one, is ignored. The record leaves in one frame when
    /// it fits and as a bulk transfer when it does not, and a later revision
    /// waits for a transfer of the same workspace still leaving.
    ///
    /// - Parameters:
    ///   - workspaceID: The workspace.
    ///   - revision: The structure's revision, above zero.
    ///   - record: The record's bytes, shared with every connection offered
    ///     them; at most `maxWorkspaceStructureRecordBytes`.
    void offerWorkspaceStructure(std::uint32_t workspaceID, std::uint64_t revision, std::shared_ptr<const std::vector<std::uint8_t>> record);

    /// Owes the client the news that a workspace is gone, when it was told
    /// of the workspace; one it was never told of is forgotten in silence.
    ///
    /// - Parameter workspaceID: The workspace.
    void withdrawWorkspaceStructure(std::uint32_t workspaceID);

    /// The bytes the connection holds in queued frames and typed transfers.
    [[nodiscard]] std::size_t queuedBytes() const { return m_scheduler.queuedBytes(); }

    /// The bulk the lanes hold materialised: what transfers and runs have
    /// still to send. A checkpoint is encoded only when no other is in the
    /// lanes, so this holds at most one.
    [[nodiscard]] std::size_t queuedBulkBytes() const { return m_scheduler.queuedBulkBytes(); }

    /// Supplies the connection and daemon's existing outbound budgets and all
    /// their other retained owners before an image copy. This is a pure query:
    /// it must not mutate, destroy or re-enter the connection. The carrier must
    /// include the resulting source charges in every later output admission.
    /// The optional preemption callback retires image sources across the same
    /// aggregate. It may only call `preemptImageTransfers`, not destroy, feed,
    /// service or otherwise mutate connections. Without it only this
    /// connection's image sources can be preempted.
    void setImageSourceAdmission(std::function<BulkSourceAdmission()> admission, std::function<void()> preempt = {})
    {
        m_imageSourceAdmission = std::move(admission);
        m_preemptAggregateImages = std::move(preempt);
    }

    /// Materialized outbound payload capacity, not just its unsent suffix.
    /// Image-source reservations are charged separately by the carrier's
    /// ledgers. Shared structure records are conservatively charged per reader.
    [[nodiscard]] std::size_t retainedOutboundPayloadBytes() const;

    /// Releases the lowest-priority immutable image owners before the carrier
    /// materializes higher-priority output. Announced partial transfers receive
    /// BulkAbort; installed client images are never evicted. A later service
    /// re-offers the affected metadata and the client requests only missing data.
    void preemptImageTransfers();

    /// Whether a call may be answered now with an answer of a size, or must
    /// be answered busy: the answers not yet sent are under the
    /// outstanding-call cap, and the answer fits under the queued-bulk cap.
    ///
    /// - Parameter answerBytes: The answer's bytes; 0 before it is computed.
    /// - Returns: Whether it may.
    [[nodiscard]] bool admitsCall(std::size_t answerBytes) const;

    /// Takes the calls the client sent since the last call, in arrival
    /// order. An admitted one stays outstanding, counted against the cap,
    /// until it is answered and the answer has left; one over the cap waits
    /// only for `refuseCall`. `Sync::serveCalls` (Control.h) answers both.
    [[nodiscard]] std::vector<ReceivedCall> takeCalls() { return std::exchange(m_receivedCalls, {}); }

    /// Applies the pane input and sizes held behind calls, once the owner
    /// has taken those calls and served them. `Sync::serveCallsDeferred`
    /// calls it after the last answer; the next receive does too.
    void releaseInputBehindCalls();

    /// Whether the client has been sent, whole, the structure of a workspace
    /// as the owner last offered it.
    ///
    /// The owner binds a pane only after the structure that names it has
    /// left (Docs/sync.md §7.8), and asks here.
    ///
    /// - Parameter workspaceID: The workspace.
    /// - Returns: False while the newest offered revision is owed or its
    ///   transfer is in the lanes, and for a workspace never offered.
    [[nodiscard]] bool isWorkspaceStructureSent(std::uint32_t workspaceID) const;

    /// Writes the answer to a call that arrived past the outstanding-call
    /// cap: the text of a `Result` that names the call and says `busy`, or
    /// says why the call cannot be read.
    using BusyAnswerer = std::string (*)(std::string_view call);

    /// Has calls past the outstanding-call cap answered as they are read.
    ///
    /// With an answerer the connection keeps nothing of such a call: its
    /// answer is queued where the call is read, so the calls held for the
    /// owner are never more than may be outstanding, however many a client
    /// sends in one read. What it then holds for a client that reads nothing
    /// is those answers, on the interactive lane, which `wantsInput` bounds
    /// as it bounds every answer. An owner that serves calls installs one
    /// (`Sync::answerBusyAsRead`, Control.h); without one a call past the
    /// cap is held for `refuseCall`.
    ///
    /// - Parameter answerer: The writer, or null for none.
    void setBusyAnswerer(BusyAnswerer answerer) { m_busyAnswerer = answerer; }

    /// Checks additional typed clipboard reply capacity before allocation.
    /// This lookup must not mutate or re-enter the connection.
    void setInboundAssemblyAdmission(std::function<bool(std::size_t)> admit) { m_inboundAssemblyAdmission = std::move(admit); }
    /// Capacity retained for correlated clipboard replies, including delivery.
    [[nodiscard]] std::size_t inboundClipboardBytes() const;
    /// Releases pending typed clipboard requests, retaining an active callback's charge.
    void discardInboundClipboard();
    /// Issues one exact request only after the carrier establishes authority.
    ///
    /// - Parameters:
    ///   - handle: The authorized source pane's bound handle.
    ///   - request: A fresh nonzero ID and the exact requested operation.
    /// - Returns: Whether the bounded request owner and frame were accepted.
    [[nodiscard]] bool requestClipboard(std::uint32_t handle, const ClipboardRead& request);
    /// Requests bounded native measurements from the font-owning attachment.
    ///
    /// - Parameters:
    ///   - handle: The bound pane.
    ///   - request: The correlated document and constraints.
    ///   - now: Monotonic microseconds for expiry.
    ///   - waitMicroseconds: Time remaining in the original admission deadline.
    /// - Returns: Whether the request was queued.
    [[nodiscard]] bool requestRichText(std::uint32_t handle, const RichText::MeasureRequest& request, std::uint64_t now, std::uint64_t waitMicroseconds = RichText::maxMeasurementWaitMilliseconds * 1000ULL);
    /// Retires a measurement when its pane or deadline is no longer valid.
    ///
    /// - Parameters:
    ///   - handle: The bound pane.
    ///   - token: The outstanding request token.
    void cancelRichText(std::uint32_t handle, std::uint32_t token);
    /// Retires work no longer present in the terminal's authoritative query queue.
    /// - Parameters:
    ///   - handle: The pane whose requests are reconciled.
    ///   - tokens: Every request token still live on that pane.
    void retainRichTextRequests(std::uint32_t handle, std::span<const std::uint32_t> tokens);
    /// Transfers the bounded set of correlated measurement replies.
    ///
    /// - Returns: Replies accepted since the previous drain.
    [[nodiscard]] std::vector<RichTextMeasureReply> takeRichTextReplies() { return std::exchange(m_richTextReplies, {}); }
    /// Retires a request after timeout, cancellation or authority withdrawal.
    ///
    /// - Parameter requestID: The original request ID; unknown IDs do nothing.
    void cancelClipboardRequest(std::uint32_t requestID);
    /// Installs a non-reentrant authority query, checked before each response
    /// allocation, chunk and delivery. Absence leaves exact-request correlation
    /// to this connection's owner; the callback grants no new permission.
    void setClipboardReplyAdmission(std::function<bool(std::uint32_t, std::uint32_t)> admit) { m_clipboardReplyAdmission = std::move(admit); }
    /// Rechecked before accepting and again before delivering native drag input.
    [[nodiscard]] bool supportsNativeDrag() const { return m_capabilities.contains(ClientCapability::NativeDrag); }
    /// Whether the client asks its own person about the clipboard writes and
    /// notifications delivered to it.
    ///
    /// - Returns: Whether its Greet named ``ClientCapability/AsksAboutHostEffects``.
    [[nodiscard]] bool asksAboutHostEffects() const { return m_capabilities.contains(ClientCapability::AsksAboutHostEffects); }
    /// Whether the client shows a person none of the host's questions.
    ///
    /// - Returns: Whether its Greet named ``ClientCapability/ShowsNoQuestions``.
    [[nodiscard]] bool showsNoQuestions() const { return m_capabilities.contains(ClientCapability::ShowsNoQuestions); }
    /// Whether Greet offered proportional faces for hosted measure queries.
    ///
    /// - Returns: Whether the client's renderer draws and publishes run faces.
    [[nodiscard]] bool publishesFaces() const { return m_capabilities.contains(ClientCapability::PublishesFaces); }
    /// Withdraws unsent native commands before an owner loses its pane.
    /// - Parameter handle: The pane channel whose commands are revoked.
    void cancelNativeDragCommands(std::uint32_t handle) { (void)m_scheduler.withdraw(FrameKind::NativeDragCommand, handle); }
    void setNativeDragAdmission(std::function<bool(std::uint32_t, std::uint64_t)> admit) { m_nativeDragAdmission = std::move(admit); }
    [[nodiscard]] bool sendNativeDragCommand(std::uint32_t handle, std::uint64_t epoch, const DragDrop::Command&);
    [[nodiscard]] std::vector<NativeDragEvent> takeNativeDragEvents() { return std::exchange(m_nativeDragEvents, {}); }
    /// Routes only an explicitly credited control upload's raw chunks. The
    /// callbacks may query authority and copy bounded memory, but must not
    /// perform effects, mutate/reenter or destroy this connection. The owner
    /// charges all retained upload capacity to its existing inbound budgets.
    /// - Parameters:
    ///   - receive: Claims a reserved transfer or leaves clipboard handling intact.
    ///   - isReserved: Refuses a clipboard response reusing an active upload ID.
    void setControlUploadReceiver(std::function<ClientBulkDisposition(const BulkChunk&)> receive, std::function<bool(std::uint32_t)> isReserved)
    {
        m_controlUploadReceiver = std::move(receive);
        m_isControlUploadReserved = std::move(isReserved);
    }
    /// Whether an already announced clipboard reply owns a transfer ID.
    /// - Parameter transfer: A nonzero proposed control-upload credit.
    /// - Returns: Whether the owner must choose another credit.
    [[nodiscard]] bool isClipboardTransferInUse(std::uint32_t transfer) const;
    /// Delivers completed replies while their borrowed data remains charged.
    ///
    /// - Parameter deliver: Exact pane, request, status and canonical bundle.
    /// - Returns: False after a callback stops delivery or closes this owner.
    [[nodiscard]] bool drainClipboardReplies(const std::function<bool(std::uint32_t, const ClipboardRead&, EffectClipboardStatus, const EffectClipboardBundle&)>& deliver);
    /// Takes the viewports the client's driver reported since the last
    /// call, the newest of each pane, at most one per bound pane.
    [[nodiscard]] std::vector<ViewportReport> takeViewportReports() { return std::exchange(m_viewportReports, {}); }

    /// Answers an admitted call with a JSON-RPC 2.0 response. One larger than
    /// a frame travels as a bulk transfer the `Result` announces.
    ///
    /// - Parameters:
    ///   - callIndex: The call's `ReceivedCall::callIndex`.
    ///   - response: The response's text.
    /// - Returns: What was done; an answer over the queued-bulk cap is not
    ///   queued, and the call waits for `refuseCall`.
    [[nodiscard]] CallAnswer answerCall(std::uint32_t callIndex, std::string response);

    /// Answers a call without its method: one over a cap, or one that cannot
    /// be read. The inline answer remains outstanding until it leaves the
    /// connection.
    ///
    /// - Parameters:
    ///   - callIndex: The call's index; one already answered is ignored.
    ///   - response: The JSON-RPC 2.0 error response, at most one frame.
    ///   - refusal: Why, for the counters.
    void refuseCall(std::uint32_t callIndex, std::string_view response, CallRefusal refusal);

    /// The attachments list the client is owed (Docs/sync.md §6.10): those
    /// that share a pane with it, itself among them, when they, their states,
    /// or a shared pane's size owner or primary differ from what it was last
    /// sent. Its owner words it as a `Notify` and notes it sent
    /// (`serveNotifies`); a list not noted sent is owed again.
    ///
    /// - Returns: The list under a fresh revision, or `nullopt` when what the
    ///   client was sent still holds, the connection is served from no table,
    ///   or the client speaks no `Notify`.
    [[nodiscard]] std::optional<AttachmentsList> owedAttachments();

    /// Supplies presentation-only facts for one exact live base row. The pane
    /// span and self bit are borrowed for this call. The synchronous provider
    /// must not reenter, send, change base authority or destroy this connection.
    /// - Parameter provider: The owner-thread projection, or an empty function.
    void setAttachmentPresenceProvider(std::function<std::optional<AttachmentPresence>(ID128, std::span<const ID128>, bool)> provider)
    {
        m_attachmentPresenceProvider = std::move(provider);
        noteAttachmentsChanged();
    }

    /// Reconsiders the one roster after its owner changes presentation facts.
    void noteAttachmentsChanged() { m_areAttachmentsStale = true; }

    /// Notes that the client was sent an attachments list.
    ///
    /// - Parameter list: The list `owedAttachments` returned.
    void noteAttachmentsSent(const AttachmentsList& list);

    /// Sends the client a `Notify`.
    ///
    /// - Parameter body: A whole `Notify` body, a kind and its record, as the
    ///   kind's encoder built it.
    /// - Returns: False, sending nothing, when the connection is not
    ///   attached, the body is not one frame, or the interactive bytes held
    ///   unsent are past `HostPolicy::maxQueuedInteractiveBytes`.
    [[nodiscard]] bool sendNotify(std::span<const std::uint8_t> body);

    /// Sends the exact pending or terminal own status without granting authority.
    /// Pending must be admitted once before a terminal status. Denial or expiry
    /// queues NoConsent after the status and erases the unapproved record.
    /// - Parameter status: This Welcome's exact question and a non-approval phase.
    /// - Returns: Whether its bounded frame was admitted; false changes no authority.
    [[nodiscard]] bool sendConsentStatus(const ConsentStatus& status);

    /// Delivers and promotes one approved attach as an owner-thread transaction.
    /// The owner rechecks requester, approver, target and credential authority
    /// on every retry, including after output backpressure. No callback or
    /// reentry occurs between status admission and exact-record promotion.
    /// - Parameters:
    ///   - question: This Welcome's exact question and target session.
    ///   - authority: Its freshly revalidated connection authority.
    ///   - now: The current monotonic time.
    ///   - deadline: The original consent expiry in the same units.
    /// - Returns: Whether approval was queued and the same record was promoted.
    [[nodiscard]] bool deliverConsentApproval(ConsentBinding question, const ConnectionAuthority& authority, std::uint64_t now, std::uint64_t deadline);

    /// The interactive bytes queued and not yet handed to the transport.
    [[nodiscard]] std::size_t queuedInteractiveBytes() const { return m_scheduler.queuedBytes() - m_scheduler.queuedBulkBytes(); }

    /// Whether the owner should read more of the client's stream: false while
    /// the interactive bytes held unsent are past
    /// `HostPolicy::maxQueuedInteractiveBytes`, so a client that writes calls
    /// and reads nothing holds the host to that bound.
    [[nodiscard]] bool wantsInput() const { return queuedInteractiveBytes() <= m_policy.maxQueuedInteractiveBytes; }

    /// Unbinds a pane, telling the client with `PaneUnbind`. A frame the
    /// client sent for the handle before it learned is ignored, and the handle
    /// is bound again only once an `Ack` counts past the `PaneUnbind`: every
    /// frame the client sends after that one was written knowing the handle
    /// was free, so none can be taken for the pane bound to it next.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - reason: Why, as `PaneUnbind` carries it.
    void unbindPane(std::uint32_t handle, std::uint8_t reason);

    /// A pane the connection unbound with `CheckpointTooLarge` and bound
    /// again once its checkpoint fit, under a new handle. The owner moves
    /// what it keeps of `previousHandle` to `handle`; one it no longer wants
    /// it unbinds by the new handle.
    struct PaneRebind {
        std::uint32_t previousHandle = 0;
        std::uint32_t handle = 0;
    };

    /// Takes the panes bound again since the last call, oldest first.
    ///
    /// - Returns: The rebinds; the connection keeps none.
    [[nodiscard]] std::vector<PaneRebind> takePaneRebinds() { return std::exchange(m_paneRebinds, {}); }

    /// The panes unbound for their checkpoint's size and not yet bound again.
    ///
    /// - Returns: Their count.
    [[nodiscard]] std::size_t oversizedPaneCount() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(m_oversizedPanes, [](const OversizedPane& pane) { return !pane.isRebound; }));
    }

    /// Sends what the panes owe, as far as the rate and the window admit, and
    /// probes when no screen data has flowed for the probe interval. A pane
    /// whose terminal no longer holds its newest frame waits for the next
    /// `PaneSource::publish`.
    ///
    /// - Parameter now: The host clock.
    void service(std::uint64_t now);

    /// When `service` next has something to do that no frame from the client
    /// and no publish will prompt: a pane's rate interval ending, a probe, or
    /// a datagram's loss timeout.
    ///
    /// - Parameter now: The host clock.
    /// - Returns: The host clock at which to call `service`, or `nullopt` when
    ///   only the client or a publish can make anything due.
    [[nodiscard]] std::optional<std::uint64_t> nextServiceMicroseconds(std::uint64_t now) const;

    /// Whether a stream frame is queued, sendable now or not.
    [[nodiscard]] bool hasQueuedStreamFrames() const { return !m_scheduler.isEmpty(); }

    /// Takes the next interactive stream frame, holding every bulk one: what
    /// an owner calls to move control, updates and typed records into a
    /// transport whose own buffer bounds them, without letting a checkpoint
    /// past the window.
    ///
    /// - Parameter output: The frame is appended here.
    /// - Returns: Whether a frame was taken.
    [[nodiscard]] bool popInteractiveFrame(std::vector<std::uint8_t>& output);

    /// Takes the next stream frame for the transport.
    ///
    /// - Parameters:
    ///   - output: The frame is appended here.
    ///   - now: The host clock.
    /// - Returns: The frame's lane, or `nullopt` when nothing may leave.
    [[nodiscard]] std::optional<Lane> popStreamFrame(std::vector<std::uint8_t>& output, std::uint64_t now);

    /// Takes the datagrams queued since the last call.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> takeDatagrams();

    /// The handle slots the connection keeps, bound or waiting to be bound
    /// again: what bounds its per-pane state and the cost of a service.
    [[nodiscard]] std::size_t handleSlots() const { return m_panes.size(); }

    /// Where the handshake stands.
    [[nodiscard]] ConnectionState state() const { return m_state; }

    /// Why the connection closed, when it has.
    [[nodiscard]] std::optional<ByeReason> byeReason() const { return m_byeReason; }

    /// The role the Welcome granted the client: the less privileged of what
    /// its Greet asked and what its credential allows. A viewer until then.
    ///
    /// - Returns: The role.
    [[nodiscard]] Role grantedRole() const { return m_grantedRole; }

    /// Reports whether the admitted role, kind and credential permit input.
    ///
    /// - Returns: Whether the connection holds input authority.
    [[nodiscard]] bool mayDrive() const;

    /// The client's latest facts.
    [[nodiscard]] const Facts& facts() const { return m_facts; }

    /// Returns the typography this client published for one bound pane.
    /// - Parameter handle: The pane's handle on this connection.
    [[nodiscard]] std::shared_ptr<const RichText::TypographyFacts> typography(std::uint32_t handle) const;

    /// The faces the client last said it draws proportional runs in, or
    /// null; the host counts the runs of a pane the client views in them
    /// (Docs/proportional-text.md).
    [[nodiscard]] const std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts>& proportionalFaces() const { return m_proportionalFaces; }

    /// What the connection has done.
    [[nodiscard]] const HostCounters& counters() const { return m_counters; }

    /// The caps the connection holds its client to.
    [[nodiscard]] const Limits& limits() const { return m_policy.limits; }

    /// The view the client holds of a bound pane, or `nullopt` for a handle
    /// that names none.
    [[nodiscard]] std::optional<ViewState> paneView(std::uint32_t handle) const;

    /// Whether screen content on this connection has been handed to its carrier.
    /// - Parameter handle: A currently bound pane handle.
    /// - Returns: False for an unbound pane or a resume baseline alone.
    [[nodiscard]] bool hasDisclosedScreen(std::uint32_t handle) const;

    /// Whether some bound pane is `Live`.
    [[nodiscard]] bool hasLivePane() const;

    /// Whether the host probes an idle connection: some pane is `Live` and
    /// the client is focused. A connection whose panes are all `Dormant` or
    /// `RecordsOnly`, or whose client is in the background, is never probed,
    /// so a phone's radio stays asleep (Docs/sync.md §6.4).
    [[nodiscard]] bool isProbing() const;

    /// Records the client's input reaching its program, which restarts the
    /// idle probe schedule as screen data does.
    ///
    /// - Parameter now: The host clock.
    void noteClientInput(std::uint64_t now) { m_pacer.noteActivity(now); }

    /// Takes this connection's driver focus off every pane: it closes, is
    /// lost, or its owner drops it.
    void releaseDriverFocus();

    /// The digest of what the host last sent a pane, and that generation:
    /// what a client holding the generation computes from its replica.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The generation and digest, or `nullopt` for a pane not bound
    ///   or sent nothing yet.
    [[nodiscard]] std::optional<std::pair<std::uint64_t, Digest128>> sentDigest(std::uint32_t handle) const;

    /// Serves the attachment from a table instead of the identity's own: the
    /// `Greet` either resumes one of the table's attachments or creates one,
    /// and the panes' baselines live in its record, so a later connection can
    /// continue them (Docs/sync.md §6.9). Called before the `Greet` arrives.
    ///
    /// - Parameters:
    ///   - table: The host's attachments; it must outlive the connection.
    ///   - authority: What this connection's peer was authenticated as.
    ///   - connection: The owner's name for this connection.
    ///   - resolvePane: Finds a pane a resume names, or null when it is gone.
    void serveFromTable(AttachmentTable& table, ConnectionAuthority authority, std::uint64_t connection, std::function<PaneSource*(ID128)> resolvePane);

    /// Closes the connection because a resume moved its attachment to another
    /// one (`Bye{Superseded}`); it reads and sends nothing more but the `Bye`.
    void supersede();

    /// Whether this host superseded the connection itself, having moved its
    /// attachment to another. A peer's `Bye` naming `Superseded` does not
    /// count: it is only a link that peer ended.
    ///
    /// - Returns: Whether ``supersede()`` closed it.
    [[nodiscard]] bool wasSuperseded() const { return m_wasSuperseded; }

    /// Stops this connection and releases its borrowed attachment state without
    /// sending a frame. The carrier owner sends the authorization refusal and
    /// may then retire the attachment, after releasing any superseded borrower.
    /// Already-admitted pane input is owned separately by each pane.
    void withdrawAuthority();
    /// Queues a canonical authority refusal after every already admitted frame.
    /// The carrier owner then retires authority and drains the ordered queue.
    /// - Parameter reason: A typed authentication or operation refusal.
    void refuse(ByeReason reason);

    /// Ends a connection normally after its already queued interactive frames.
    void finish();

    /// Discards every frame queued and not yet taken, then queues a `Bye` and
    /// closes, so the `Bye` is the next and last frame taken. For an owner
    /// shedding a reader that fell behind: what was queued will never be
    /// read, and the reason is the one thing still owed.
    ///
    /// - Parameters:
    ///   - reason: Why.
    ///   - detail: The sentence the `Bye` carries.
    void closeDiscardingQueue(ByeReason reason, std::string_view detail);

    /// The panes this connection bound, by handle: what a resume bound itself
    /// as well as what the owner bound.
    ///
    /// - Returns: Each bound pane's handle and identity.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, ID128>> boundPanes() const;

    /// The attachment this connection serves, once its `Greet` was admitted
    /// from a table.
    [[nodiscard]] const AttachmentRecord* attachment() const { return m_record; }

    /// Why the `Greet`'s resume was refused, when it was; the connection then
    /// serves a fresh attachment.
    [[nodiscard]] std::optional<ResumeRefusal> resumeRefusal() const { return m_resumeRefusal; }

    /// The client instance its `Greet` named, whose input it sends; zero
    /// before the `Greet`.
    [[nodiscard]] ID128 clientInstance() const { return m_clientInstance; }

    /// When the silence rule moves the connection to `Lost`: the first send
    /// since the client was last heard, plus the silence bound. `nullopt`
    /// while nothing is outstanding — no unacknowledged bytes and no probe
    /// unanswered — so an idle connection is never lost by silence, and one
    /// whose `Ack`s or `Echo`s keep arriving never is either.
    [[nodiscard]] std::optional<std::uint64_t> silenceDeadlineMicroseconds() const;

    /// Whether bytes that owe an `Ack`, or a probe, await the client's
    /// answer. Stream bytes that carried only probes are answered by the
    /// `Echo` and owe none.
    [[nodiscard]] bool isAwaitingClient() const;

    /// The optional ops a chain update to this client may use, from its
    /// capabilities.
    [[nodiscard]] ChainOps chainOps() const;

    /// The least time between two updates of one pane: the client's refresh
    /// interval, doubled under `Saver`, and at least
    /// `HostPolicy::constrainedIntervalMicroseconds` on a `Constrained` link.
    [[nodiscard]] std::uint64_t rateIntervalMicroseconds() const;

    /// The attachment's pacing.
    [[nodiscard]] const AttachmentPacer& pacer() const { return m_pacer; }

    /// The history head a pane's client last acknowledged holding every
    /// append below, for the tests and the journal.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The head, or `nullopt` for a handle not bound.
    [[nodiscard]] std::optional<std::uint64_t> historyAcked(std::uint32_t handle) const;

    /// A history transfer of a pane the lanes still hold, for the tests.
    struct QueuedHistoryTransfer {
        std::uint32_t transfer = 0;
        /// Whether its head has left, so the client knows of it.
        bool isAnnounced = false;
    };

    /// The pane's history transfers the lanes still hold.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The transfers: appends, the open line, then the wants'.
    [[nodiscard]] std::vector<QueuedHistoryTransfer> queuedHistoryTransfers(std::uint32_t handle) const;

    /// The transfers a pane's wants at one priority hold in the lanes: at
    /// most one.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - priority: The priority.
    /// - Returns: The count.
    [[nodiscard]] std::size_t historyTransfers(std::uint32_t handle, HistoryPriority priority) const;

    /// The ranges a pane's client wants at one priority and has not been
    /// sent, for the tests.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - priority: The priority.
    /// - Returns: The ranges, ascending.
    [[nodiscard]] std::vector<LineRange> wantedHistory(std::uint32_t handle, HistoryPriority priority) const;

private:
    /// What one pass of serving a pane did.
    struct PaneService {
        /// The bytes it handed the lanes.
        std::size_t bytes = 0;
        /// Whether the window refused a frame it owed.
        bool isRefused = false;
    };

    /// A transfer a want's lines travel in.
    struct WantTransfer {
        std::uint32_t transfer = 0;
        LineRange lines;
    };

    /// What one attachment has been sent of a pane's history.
    struct HistoryState {
        /// The epoch the numbers below are counted in.
        std::uint32_t epoch = 0;
        /// The highest head the attachment was sent lines up to; a head that
        /// retreats below it sends nothing.
        std::uint64_t sentHead = 0;
        /// The head the client acknowledged holding every append below.
        std::uint64_t ackedHead = 0;
        /// The publisher's revision when the attachment was last sent its
        /// appends: a line rewritten since is sent again.
        std::uint64_t revision = 0;
        /// The transfers carrying appends, which the update after them waits
        /// for.
        std::vector<std::uint32_t> appendTransfers;
        /// The lines the client wants and has not been sent, one set per
        /// priority; an unmetered attach's backfill is the `Backfill` set
        /// until the client sends its own.
        HistoryWants wants { provisionalLimits.maxPendingRanges };
        /// The transfer each priority's wants hold in the lanes, and its
        /// lines: one at a time.
        std::array<std::optional<WantTransfer>, 3> wantTransfers;
        /// The open line the client holds as it was sent, or none.
        std::optional<OpenLine> sentOpenLine;
        /// The transfer carrying the open line, when it was too long for a
        /// frame.
        std::optional<std::uint32_t> openLineTransfer;
    };

    /// How far a reader of a pane's records has been sent them.
    struct RecordsProgress {
        /// The revision of the pane's facts the reader was last sent; zero
        /// for none, which no set facts carry.
        std::uint32_t sentFactsRevision = 0;
        /// The revision of the status set the reader was last sent whole.
        std::uint32_t sentStatusRevision = 0;
        /// The revision of the status set whose frames are leaving, zero when
        /// none is. A set that moves while its frames leave starts again from
        /// its first record.
        std::uint32_t statusRevisionInTransit = 0;
        /// The index of that set's next record.
        std::size_t statusCursor = 0;
    };

    /// What a client holding a pane `Dormant` has been told of its palette
    /// in this span (Docs/sync.md §6.8).
    struct PaletteProgress {
        /// The pane's change count when its palette was last examined; none
        /// before the span's first look. A frame taken since is looked at once.
        std::optional<std::uint64_t> examinedChanges;
        /// The terminal's palette epoch at that look: a frame that moved no
        /// colour is not encoded to be compared.
        std::uint64_t examinedEpoch = 0;
        /// The hash of the palette block the client holds as far as the host
        /// can name it; none where it cannot, which the next look sends.
        std::optional<std::uint64_t> heldHash;
    };

    /// One pane's baselines on this connection.
    struct PaneAttachment {
        PaneSource* source = nullptr;
        std::uint32_t handle = 0;
        /// Native font facts belong to this pane, including its local zoom.
        std::shared_ptr<const RichText::TypographyFacts> typography;
        /// The paste progress the client was last sent a `PasteReceipt` of.
        std::optional<PaneInputQueue::PasteWritten> sentPasteWritten;
        /// The serial of the newest discard of the client's input it was
        /// told of, or that stood when the pane was bound: a run dropped
        /// before the bind is the resume's `inputWritten` to report.
        std::uint64_t reportedDiscardSerial = 0;
        /// The pane's size owner and primary as the client was last listed
        /// them, to see either move without building the list.
        std::optional<ID128> listedOwner;
        std::optional<ID128> listedPrimary;
        /// The echo context the client was last sent on the stream.
        std::optional<EchoContext> sentEchoContext;
        bool needsCheckpoint = true;
        std::optional<std::uint64_t> unacknowledgedCheckpoint;
        /// The transfer carrying the latest checkpoint's body.
        std::optional<std::uint32_t> checkpointTransfer;
        /// What was sent and acknowledged of the pane: the attachment
        /// record's when the connection serves one from a table, so a resume
        /// continues from it; otherwise `ownedBase`. Never null.
        PaneBaselines* base = nullptr;
        std::unique_ptr<PaneBaselines> ownedBase;
        bool isLossPending = false;
        bool hasSentUpdate = false;
        bool hasDisclosedScreen = false;
        std::uint64_t lastUpdateMicroseconds = 0;
        /// The bytes this pane was sent while owed, from which the deficit
        /// round-robin reads how far it is behind the others.
        std::uint64_t servedBytes = 0;
        /// For a handle unbound, the stream offset just past its `PaneUnbind`
        /// once the transport took it; an `Ack` counting past it frees the
        /// handle.
        std::optional<std::uint64_t> unbindOffset;
        /// The client's own `inputWritten` as the newest update on the stream
        /// or `Receipt` told it.
        std::uint64_t reportedInputWritten = 0;
        /// The client's own `inputSettled` as the newest update on the stream
        /// or `Receipt` told it.
        std::uint64_t reportedInputSettled = 0;
        /// What the client's last `Focus` for the pane said.
        Ubiquitty::Modes::FocusState clientFocus = Ubiquitty::Modes::FocusState::Unfocused;
        /// The client's view; a pane starts `Dormant`, or `RecordsOnly` for
        /// an agent, until the client sets it.
        ViewState view = ViewState::Dormant;
        /// Whether the client has said how it holds the pane, by a `View` or
        /// by its resume. Until then the pane is sent no status set.
        bool isViewStated = false;
        /// The pane's change count when it left `Live`: a higher one is a
        /// change the client is told of once.
        std::uint64_t dormantChanges = 0;
        bool hasSentDirty = false;
        /// What the client has been told of the pane's palette this span.
        PaletteProgress palette;
        /// How far the client has been sent the pane's records.
        RecordsProgress records;
        /// Complete effect batches queued on this physical connection.
        std::uint64_t effectsQueued = 0;
        /// A fresh reader skips old effects through an explicit completion.
        bool isEffectBaselineOwed = false;
        /// The immutable selected batch borrows the reader-retained log prefix.
        std::optional<EffectBatch> effectBatch;
        std::size_t effectCursor = 0;
        std::uint64_t effectGeneration = 0;
        bool didSkipEffectPictures = false;
        /// The exact slot identity issued on this handle and its last use.
        struct SampleDelivery {
            std::optional<SampleIdentity> identity;
            std::uint32_t transfer = 0;
            std::uint64_t lastUsingSequence = 0;
        };
        std::array<SampleDelivery, VT::sampleSlotCount> samples;
        /// Slot replacement requires this handle's ACK, not another alias's.
        std::uint64_t sampleEffectsAcknowledged = 0;
        std::uint64_t sampleAckAwaited = 0;
        /// A clipboard batch is sent only after its exact final bulk chunk.
        struct ClipboardDelivery {
            std::uint32_t transfer = 0;
            std::uint64_t throughSequence = 0;
        };
        std::optional<ClipboardDelivery> clipboard;
        /// One content identity per pane, regardless of how many image IDs or
        /// placements refer to it. Removed identities retire their transfers.
        struct ImageDelivery {
            ImageAssetIdentity asset;
            /// A sole displayed placement can be coalesced with its next
            /// same-geometry frame. Multiple placements do not infer a swap.
            std::optional<Image::TerminalImagePlacement> placement;
            /// The pixels captured with the record that named them, charged
            /// to the outbound ledgers, until their head leaves.
            std::unique_ptr<BulkSource> source;
            std::uint32_t transfer = 0;
            bool isOffered = false;
            bool isWanted = false;
        };
        std::array<std::optional<ImageDelivery>, Image::maxTerminalImageCount> images;
        std::uint64_t sentImageRevision = 0;
        /// Only the first record after a resume offers without pushing pixels;
        /// retained client pixels are discovered after admission by ImageWant.
        bool shouldOfferRetainedImages = false;
        /// The state digest of the newest checkpoint sent.
        std::optional<Digest128> sentCheckpointDigest;
        /// The state digest of a checkpoint the client refused, and how many
        /// times in a row it refused that state. Once is a body the path may
        /// have spoiled, so it is sent again; twice is a state this client
        /// cannot take, and no body of it is sent again.
        std::optional<Digest128> refusedCheckpointDigest;
        std::uint32_t refusalsOfDigest = 0;
        /// The generation whose checkpoint was withheld as refused; the pane
        /// owes nothing until it publishes another.
        std::optional<std::uint64_t> withheldGeneration;
        HistoryState history;
    };

    /// The attachment's identity: its record's when served from a table.
    [[nodiscard]] ID128 attachmentIdentity() const;

    /// A pane a resume continues, bound once the `Welcome` leaves.
    struct PaneResumeBinding {
        PaneSource* source = nullptr;
        PaneResume resume;
        /// The baseline the client's digest matched, or none for a pane that
        /// takes a checkpoint.
        std::optional<SentGeneration> baseline;
    };

    /// Admits the `Greet`'s attachment from the table: resumed, or fresh when
    /// it asked for none or was refused. Fills the `Welcome`'s identity and
    /// each resumed pane's outcome.
    ///
    /// - Parameters:
    ///   - greet: The `Greet`.
    ///   - welcome: The answer, filled in.
    ///   - bindings: Receives the panes to bind after the `Welcome`.
    ///   - now: The host clock.
    /// - Returns: `nullopt` when the client is attached, or the `Bye` it is
    ///   answered instead: `ResumeInProgress` for a resume racing one still
    ///   in its handshake, and `Malformed` for a connection a resume proof
    ///   admitted whose `Greet` does anything but resume that attachment
    ///   without a secret.
    [[nodiscard]] std::optional<ByeReason> admitAttachment(const Greet& greet, Welcome& welcome, std::vector<PaneResumeBinding>& bindings, std::uint64_t now);

    /// The baseline a resuming client's digest names: the acknowledged one,
    /// or one sent after it.
    ///
    /// - Parameters:
    ///   - baselines: What was sent the attachment of the pane.
    ///   - digest: The client's digest.
    ///   - generation: Its generation, preferred when identical pictures recur.
    /// - Returns: The baseline, or `nullopt` when none matches.
    [[nodiscard]] static std::optional<SentGeneration> matchingBaseline(const PaneBaselines& baselines, const Digest128& digest, std::uint64_t generation);

    /// Lets go of the attachment record: each pane keeps a copy of its
    /// baselines, and the connection no longer reaches the record.
    void releaseRecord();

    /// Binds a resumed pane, continuing from the baseline the client holds.
    ///
    /// - Parameter binding: The pane.
    void continueResumedPane(const PaneResumeBinding& binding);

    /// Queues a numbered input frame's event on its pane, closing the
    /// connection on input it may not send or that is past the pane's bound.
    ///
    /// - Parameters:
    ///   - frame: A `Raw`, `Keys` or `Pointer`.
    ///   - carrier: How it arrived.
    ///   - now: The host clock.
    void handleInput(const Frame& frame, Carrier carrier, std::uint64_t now);

    /// Tells a pane whether this connection's driver holds the focus: its
    /// last `Focus` said so, its window is focused and it may drive.
    ///
    /// - Parameter attachment: The pane.
    void noteDriverFocus(PaneAttachment& attachment);

    /// What the client may do to a pane's size: its standing kind, never more
    /// than the host verified, and its Drive scope.
    [[nodiscard]] GeometryStanding geometryStandingOfClient() const;

    /// Gives up the client's paste in progress on a pane, as its `PasteAbort`
    /// asks.
    ///
    /// - Parameters:
    ///   - frame: The frame.
    ///   - carrier: What carried it; never a datagram.
    ///   - now: The time.
    void handlePasteAbort(const Frame& frame, Carrier carrier, std::uint64_t now);

    /// Reports the client's viewport to a pane's geometry arbiter.
    ///
    /// - Parameter attachment: The bound pane.
    void noteViewport(PaneAttachment& attachment);

    /// Takes a `Viewport`: a driver's size of one pane, which its arbiter
    /// hears as that hand's viewport. One from a client that is no driver
    /// closes the connection with the reason; a size of no rows or no
    /// columns is dropped, and one past what a terminal holds is clamped.
    ///
    /// - Parameters:
    ///   - frame: The frame.
    ///   - carrier: What carried it; never a datagram.
    ///   - now: The time.
    [[nodiscard]] std::string_view viewportAuthorityRefusal() const;
    void handleViewport(const Frame& frame, Carrier carrier, std::uint64_t now);

    /// When a pane owes the client a standalone `Receipt`: the receipt delay
    /// after its newest write no update on the stream has reported, or now
    /// when a write settled that the client was not told of and the frame it
    /// was last sent is the one the terminal still holds.
    ///
    /// - Parameter attachment: The pane.
    /// - Returns: The time, or `nullopt` when nothing is owed.
    [[nodiscard]] std::optional<std::uint64_t> receiptDeadlineMicroseconds(const PaneAttachment& attachment, std::uint64_t now) const;

    /// The `inputSettled` a standalone `Receipt` may report: the ledger's own
    /// only while the client was sent the newest generation and the terminal
    /// still holds it, so the settled echo is in the picture the client has;
    /// otherwise what it was last told, until an update carries more.
    ///
    /// - Parameter attachment: The pane.
    /// - Returns: The number.
    [[nodiscard]] std::uint64_t receiptSettled(const PaneAttachment& attachment) const;

    /// Whether the pane's due checkpoint is one the client refused, held
    /// back until the pane publishes another generation.
    ///
    /// - Parameter pane: The pane.
    /// - Returns: Whether it is withheld.
    [[nodiscard]] static bool isCheckpointWithheld(const PaneAttachment& pane);

    /// Whether a pane's client is owed a `PasteReceipt`: its paste in
    /// progress was written further than it was last told.
    ///
    /// - Parameter attachment: The pane.
    [[nodiscard]] bool isPasteReceiptOwed(const PaneAttachment& attachment) const;

    /// Whether a pane's client is owed an `InputDiscarded`: it takes the
    /// kind, and a run of its events was dropped that it was not told of.
    ///
    /// - Parameter attachment: The pane.
    [[nodiscard]] bool isInputDiscardOwed(const PaneAttachment& attachment) const;

    /// Whether a pane's client takes the echo context and was last told
    /// another than the pane's, or none since its checkpoint. A pane with no
    /// frame published, or whose frame cannot be said in an update, owes
    /// nothing: its checkpoint would only owe it again.
    ///
    /// - Parameter attachment: The pane.
    [[nodiscard]] bool isEchoContextOwed(const PaneAttachment& attachment) const;

    /// Sets a pane's view, as the client's `View` asks. A pane leaving `Live`
    /// owes one `PaneDirty` the first time it changes after, and its status
    /// set whole; one returning is sent one update from what it was last
    /// sent, and no more of a status set.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - state: The view.
    void setPaneView(PaneAttachment& pane, ViewState state);

    /// Whether a `Dormant` pane owes its client a `PaneDirty`.
    [[nodiscard]] static bool isDirtyNoticeOwed(const PaneAttachment& pane);

    /// Whether a `Dormant` pane's palette is owed a look.
    ///
    /// - Parameter pane: The pane.
    /// - Returns: True when a frame was taken of the pane since the last look,
    ///   or none has been taken this span, and the client has said how it
    ///   holds the pane.
    [[nodiscard]] static bool isPaletteLookOwed(const PaneAttachment& pane);

    /// Sends a `Dormant` pane its palette block when it differs from the one
    /// its client holds, at most once per frame taken of the pane.
    ///
    /// The client holds the block its updates carried, then the span's newest
    /// `PanePalette`. A pane never sent a frame keeps the client's own
    /// colours until its palette moves; one whose sent and acknowledged
    /// blocks disagree is sent the block at the span's first look. A sent
    /// block moves the pane's baselines' palette hash with it, as applying it
    /// moves the client's replica, so a resume still matches.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - now: The time.
    /// - Returns: What was sent; refused when the window had no room.
    PaneService servicePalette(PaneAttachment& pane, std::uint64_t now);

    /// Whether a pane owes its client a record: facts whose revision the
    /// client has not been sent, in any view state, or a status set.
    ///
    /// - Parameter pane: The attachment.
    /// - Returns: True when `serviceRecords` has a frame to send.
    [[nodiscard]] bool isRecordOwed(const PaneAttachment& pane) const;

    /// Whether a pane owes its client a status set: one whose revision the
    /// client has not been sent, for a pane it does not hold `Live`. A
    /// `Live` pane's status travels in its screen, and nowhere else.
    ///
    /// - Parameter pane: The attachment, bound to a source.
    /// - Returns: True when `serviceRecords` has a status frame to send.
    [[nodiscard]] bool isStatusOwed(const PaneAttachment& pane) const;

    /// Sends a pane's records whose revision moved, ahead of its picture and
    /// counted in the window. A frame the window refuses is not queued: the
    /// pane stays owed, and the next opening sends the newest revision.
    ///
    /// - Parameters:
    ///   - pane: The attachment.
    ///   - now: The host clock.
    /// - Returns: The bytes sent, and whether the window refused a frame.
    PaneService serviceRecords(PaneAttachment& pane, std::uint64_t now);

    /// Reports an effect batch not yet queued, including a fresh-reader cursor.
    ///
    /// - Parameter pane: The bound pane's delivery state.
    /// - Returns: Whether an ordered effect packet remains owed.
    [[nodiscard]] bool isEffectOwed(const PaneAttachment& pane) const;
    /// Queues at most one bounded packet after its reliable screen dependency.
    ///
    /// - Parameters:
    ///   - pane: The authenticated pane binding and frozen batch cursor.
    ///   - now: The pacer's monotonic clock.
    /// - Returns: Admitted bytes and whether transport pressure refused them.
    PaneService serviceEffects(PaneAttachment& pane, std::uint64_t now);

    /// Whether placement metadata or an admitted missing asset is owed.
    [[nodiscard]] bool isImageOwed(const PaneAttachment&) const;
    /// Whether a newer image record waits for the pictures of the one sent:
    /// every picture it drops is replaced in place and still to finish.
    [[nodiscard]] bool isImageRevisionHeld(const PaneAttachment&) const;
    /// Queues a whole placement record before its asset heads. Pixel owners
    /// stay in the bulk lane, behind screen and history, until sent or aborted.
    ///
    /// - Parameters:
    ///   - pane: The authenticated binding and its pending image offers.
    ///   - now: The pacer's monotonic clock.
    /// - Returns: Admitted bytes and whether transport pressure refused them.
    PaneService serviceImages(PaneAttachment& pane, std::uint64_t now);
    /// Gives up a pane's immutable image owners, telling the receiver about any
    /// announced transfer before changing the pane's metadata or view.
    void abortImages(PaneAttachment&);
    /// Whether another image source fits the connection's slot and byte caps.
    [[nodiscard]] bool admitsImageSource(const PaneAttachment&, Digest128 hash) const;
    /// How this client takes image pixels: deflated when it named
    /// `DeflateImages`, raw otherwise.
    [[nodiscard]] Compression imageCoding() const { return m_capabilities.contains(ClientCapability::DeflateImages) ? Compression::Deflate : Compression::None; }
    /// Copies an admitted asset's pixels into an immutable source charged to
    /// both outbound ledgers, beside the head that will announce it.
    /// - Returns: The source, or null when the pane no longer holds the asset
    ///   or the ledgers refuse it.
    [[nodiscard]] std::unique_ptr<BulkSource> captureImageSource(const PaneAttachment&, Digest128 hash) const;

    /// Whether a pane's status set is sent as records to a reader.
    enum class StatusCarriage : std::uint8_t {
        /// The reader has the status elsewhere, or is owed none.
        Withheld,
        /// The set is sent, a frame a pass.
        Sent,
    };

    /// Sends a reader the records of a pane it is behind on: the facts whole,
    /// then one frame of the status set, each counted in the window.
    ///
    /// - Parameters:
    ///   - handle: The channel the reader takes the pane's records on.
    ///   - source: The pane.
    ///   - progress: How far the reader has been sent; advanced.
    ///   - carriage: Whether the status set is sent.
    ///   - detail: How much of the facts the reader is sent.
    ///   - now: The host clock.
    /// - Returns: The bytes sent, and whether the window refused a frame.
    PaneService sendRecords(std::uint32_t handle, const PaneSource& source, RecordsProgress& progress, StatusCarriage carriage, ListedDetail detail, std::uint64_t now);

    /// A pane listed on the connection.
    struct ListedPane {
        std::uint32_t handle = 0;
        PaneSource* source = nullptr;
        ListedDetail detail = ListedDetail::WithoutArguments;
        RecordsProgress progress;
        /// When its records were last sent, which the listing's cadence is
        /// counted from; absent before the first.
        std::optional<std::uint64_t> servedAt;
    };

    /// Whether a listed pane's client is behind on its facts or status set.
    [[nodiscard]] static bool isListedOwed(const ListedPane& listed);

    /// Sends every listed pane the records it is owed, each no more often
    /// than the listing's cadence; a status set begun finishes a frame a pass.
    ///
    /// - Parameter now: The host clock.
    /// - Returns: False when the window refused a frame.
    bool serviceListed(std::uint64_t now);

    /// A workspace's structure the client is owed, or was last sent.
    struct OwedStructure {
        std::uint64_t revision = 0;
        /// The record, or null when the workspace is gone.
        std::shared_ptr<const std::vector<std::uint8_t>> record;
        std::uint64_t sentRevision = 0;
        /// The transfer carrying the last revision sent, or 0.
        std::uint32_t transfer = 0;
    };

    /// Whether a structure can be sent now: one the client is behind on,
    /// with no transfer of the same workspace still leaving.
    [[nodiscard]] bool isStructureDue(const OwedStructure& owed) const;

    /// Sends the structures the client is owed, in workspace order.
    ///
    /// - Parameter now: The host clock.
    /// - Returns: False when the window refused a frame.
    bool serviceStructures(std::uint64_t now);

    /// Records a frame handed to the transport, and where a `PaneUnbind`
    /// among them ends.
    ///
    /// - Parameters:
    ///   - output: The buffer the frame was appended to.
    ///   - before: Where in it the frame starts.
    void noteStreamFramePopped(const std::vector<std::uint8_t>& output, std::size_t before);

    /// Acts on one frame from the client, closing on one the host does not
    /// accept in the connection's state.
    ///
    /// - Parameters:
    ///   - frame: The frame.
    ///   - carrier: How it arrived.
    ///   - now: The host clock.
    void handleFrame(const Frame& frame, Carrier carrier, std::uint64_t now);

    /// Reports whether a kind waits behind unserved calls.
    ///
    /// - Parameter kind: The frame's kind.
    /// - Returns: Whether it is a pane's input or size.
    [[nodiscard]] static bool isHeldBehindCalls(FrameKind kind);

    /// Keeps a pane frame until the calls before it are served, or closes
    /// the connection past the bound.
    ///
    /// - Parameter frame: The frame, whose body is copied.
    void holdBehindCalls(const Frame& frame);

    /// Moves acknowledged generations and feeds the pacer.
    ///
    /// - Parameters:
    ///   - ack: The decoded `Ack`.
    ///   - now: The host clock.
    void handleAck(const Ack& ack, std::uint64_t now);

    /// Counts a `Resync`, names the first row it disagrees on, and owes the
    /// pane a checkpoint.
    ///
    /// - Parameters:
    ///   - pane: The pane the `Resync` names.
    ///   - resync: The decoded `Resync`.
    void handleResync(PaneAttachment& pane, const Resync& resync);

    /// Sends what one pane owes, as far as the rate and the window admit:
    /// its Sync frames, then its screen.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - now: The host clock.
    /// - Returns: What it sent, and whether the window refused it.
    PaneService servicePane(PaneAttachment& pane, std::uint64_t now);

    /// Sends a pane's screen as far as the rate and the window admit.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - now: The host clock.
    /// - Returns: What it sent, and whether the window refused it.
    PaneService serviceScreen(PaneAttachment& pane, std::uint64_t now);

    /// Whether a pane holds input the client has not been sent the effect of.
    [[nodiscard]] static bool holdsInput(const PaneAttachment& pane);

    /// Whether a checkpoint body is in the lanes: the next one is encoded only
    /// once it has left.
    [[nodiscard]] bool isCheckpointInLanes() const;

    /// Queues a checkpoint of the pane's newest frame as a bulk transfer.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - now: The host clock.
    void sendCheckpoint(PaneAttachment& pane, std::uint64_t now);

    /// Binds again each pane unbound for its checkpoint's size whose checkpoint
    /// fits now. A pane is measured only after it publishes a newer frame and
    /// its back-off has passed, so one that hovers at the cap costs at most one
    /// encode per back-off, which doubles with each refusal up to
    /// `maxOversizedRetryMicroseconds`.
    ///
    /// - Parameter now: The host clock.
    void serviceOversizedPanes(std::uint64_t now);

    /// Abandons the pane's checkpoint in transfer, if one has chunks still to
    /// send: they are dropped, a `BulkAbort` follows its head on the
    /// interactive lane, and the pane owes a checkpoint again, since the
    /// client holds no body of that generation.
    ///
    /// - Parameter pane: The pane.
    void abortCheckpoint(PaneAttachment& pane);

    /// Withdraws a transfer and reopens scheduling if its unsent head released
    /// the reservation that had refused the window. No Ack is owed for bytes
    /// that never reached the transport.
    ///
    /// - Parameter transfer: The queued transfer.
    /// - Returns: Whether its head had already left.
    [[nodiscard]] TransferAbort abortTransfer(std::uint32_t transfer);

    /// One history transfer about to be queued.
    struct HistoryTransferPlan {
        std::uint64_t firstLine = 0;
        std::uint16_t count = 0;
        HistoryOrder order = HistoryOrder::Ascending;
        std::vector<std::uint8_t> body;
        /// The transfer carries the open line rather than appends.
        bool isOpenLine = false;
    };

    /// The history frames that go ahead of a pane's next update.
    struct HistoryAppends {
        /// `HistoryInline` bodies, header included.
        std::vector<std::vector<std::uint8_t>> inlineBodies;
        /// Appends too many for one frame, as `Viewport` transfers.
        std::vector<HistoryTransferPlan> transfers;
        /// What the interactive lane carries of them, on the wire.
        std::size_t interactiveBytes = 0;
        std::uint64_t lines = 0;
        /// The head the attachment is sent up to once they are queued.
        std::uint64_t sentHead = 0;
        /// The open line the client holds once they are queued.
        std::optional<OpenLine> sentOpenLine;
    };

    /// Builds the history frames a pane owes ahead of its next update: the
    /// lines appended past the highest head it was sent, the lines below it
    /// rewritten since (Docs/sync.md §6.6, §7.8), and the open line when it
    /// is not the one the client holds. A head that retreated owes no line.
    ///
    /// - Parameter pane: The pane.
    /// - Returns: The frames, nothing queued yet.
    [[nodiscard]] HistoryAppends historyAppends(const PaneAttachment& pane);

    /// Queues frames `historyAppends` built.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - appends: The frames.
    void sendHistoryAppends(PaneAttachment& pane, HistoryAppends appends);

    /// Queues the next transfer of a pane's wants at a priority, sized to
    /// the queued bulk's room.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - priority: The priority.
    void serviceWant(PaneAttachment& pane, HistoryPriority priority);

    /// Whether a pane's wants at a priority could queue their next transfer
    /// now: the checkpoint has left, the one before at that priority has, and
    /// the queued bulk has room. `Viewport` and `Search` are not otherwise
    /// held back; `Backfill` waits for a Live pane.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - priority: The priority.
    [[nodiscard]] bool isWantOwed(const PaneAttachment& pane, HistoryPriority priority) const;

    /// Why a pane's backfill is set.
    enum class BackfillCause : std::uint8_t {
        /// The client is sent its first checkpoint of an epoch.
        EpochServed,
        /// The link turned metered or unmetered.
        LinkChanged,
    };

    /// Sets a pane's backfill to what the link allows (Docs/sync.md §6.6):
    /// unmetered, down to the depth the client chose; metered, the one
    /// viewport above the screen when the epoch is first served and nothing
    /// after, a backfill in the lanes aborted.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - cause: Why it is set.
    void setImplicitBackfill(PaneAttachment& pane, BackfillCause cause);

    /// Whether the client's link is paid for by the byte or held back.
    [[nodiscard]] bool isMetered() const { return m_facts.link != LinkCost::Unmetered; }

    /// Takes a client's want: it replaces the set at its priority, and a
    /// transfer no set wants any more is aborted (Docs/sync.md §6.6).
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - want: The want.
    void takeWant(PaneAttachment& pane, const HistoryWant& want);

    /// The ids of every history transfer of a pane this connection queued.
    ///
    /// - Parameter history: The pane's history state.
    /// - Returns: The ids: appends, the open line, then the wants'.
    [[nodiscard]] static std::vector<std::uint32_t> historyTransferIDs(const HistoryState& history);

    /// Whether a pane owes its client the open line with no update to carry
    /// it: the screen holds still while a line straddles the seam, as after
    /// an attach. It goes once the checkpoint has left, which it follows.
    [[nodiscard]] bool isOpenLineOwed(const PaneAttachment& pane) const;

    /// Opens the attachment's history in the epoch its pane's terminal moved
    /// to: every history transfer of the old epoch is aborted — with a
    /// `BulkAbort` when its head left, silently otherwise — and a
    /// `HistoryReset` is queued, ahead of any other history or update frame
    /// of the pane. The lines of the new epoch below its head follow as
    /// appends. The reset is owed state: it is queued whatever the window
    /// says.
    ///
    /// - Parameter pane: The pane.
    void resetHistory(PaneAttachment& pane);

    /// Aborts a queued history transfer, telling the client with a
    /// `BulkAbort` when its head already left.
    ///
    /// - Parameter transfer: The transfer.
    void abortHistoryTransfer(std::uint32_t transfer);

    /// Whether a pane's terminal opened a history epoch its client has not
    /// been told of.
    [[nodiscard]] static bool isResetOwed(const PaneAttachment& pane);

    /// The baseline a superset update is encoded against: the acknowledged
    /// one, with every row any in-flight generation disagrees on marked moved.
    ///
    /// - Parameter pane: The pane.
    /// - Returns: The baseline.
    [[nodiscard]] AttachmentBaseline supersetBaseline(const PaneAttachment& pane) const;

    /// Queues an interactive frame and counts it against the window.
    ///
    /// - Parameters:
    ///   - kind: The frame's kind.
    ///   - channel: Its channel.
    ///   - body: Its body.
    ///   - role: Whether it answers a call.
    void enqueue(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body, FrameRole role = FrameRole::Message);

    /// Retires lower-priority image owners before additional payload storage
    /// grows into their reservation. This preserves existing nonimage policy;
    /// it does not introduce a new blanket refusal or free any client pixels.
    ///
    /// - Parameter bytes: Additional payload capacity about to be allocated.
    void prepareOutboundGrowth(std::size_t bytes);

    /// Reads a client's `Call`: admits it within the outstanding-call cap,
    /// and marks it to be answered `busy` past it.
    ///
    /// - Parameter call: The decoded `Call`.
    void handleCall(Call call);

    /// Queues a `Result`, inline when it fits a frame and as a transfer
    /// otherwise.
    ///
    /// - Parameters:
    ///   - response: The JSON-RPC 2.0 response's text.
    ///   - role: `Reply` for an answer counted against the outstanding-call
    ///     cap until it has left; `Message` for one the connection gave
    ///     itself.
    void sendResult(std::string response, FrameRole role);

    /// The calls outstanding: admitted and unanswered, and answers not yet sent.
    [[nodiscard]] std::size_t outstandingCalls() const { return m_pendingCalls.size() + m_scheduler.queuedReplies(); }

    /// Queues a `Bye` and closes the connection.
    ///
    /// - Parameters:
    ///   - reason: Why.
    ///   - detail: The sentence the `Bye` carries.
    void close(ByeReason reason, std::string_view detail);
    /// Queues an admission refusal, retaining its structured peer diagnostics.
    void close(Bye refusal);

    /// The pane bound to a handle.
    ///
    /// - Parameter handle: The handle.
    /// - Returns: The pane, or null for a handle never bound or since unbound.
    [[nodiscard]] PaneAttachment* pane(std::uint32_t handle);

    /// Whether a handle was bound on this connection and has been unbound.
    [[nodiscard]] bool isUnbound(std::uint32_t handle) const;

    /// Whether a pane has a screen frame to send.
    [[nodiscard]] bool isOwed(const PaneAttachment& pane) const;

    HostPolicy m_policy;

    HostIdentity m_identity;
    std::function<std::expected<HostIdentity, Bye>(const Greet&)> m_greetingAdmission;
    ConnectionState m_state = ConnectionState::Greeting;
    bool m_hasSentPendingConsent = false;
    std::optional<ByeReason> m_byeReason;
    /// Set by ``supersede()`` alone, never by a received `Bye`.
    bool m_wasSuperseded = false;
    Facts m_facts;
    /// When the client last reported its facts: its viewport's time.
    std::uint64_t m_factsAt = 0;
    std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts> m_proportionalFaces;
    FrameIngress m_decoder { maxFrameBodyBytes };
    bool m_isReceiving = false;
    std::function<bool(std::size_t)> m_inboundFrameAdmission;
    std::function<std::size_t()> m_heldInputRoom;
    LaneScheduler m_scheduler;
    std::function<BulkSourceAdmission()> m_imageSourceAdmission;
    std::function<void()> m_preemptAggregateImages;
    bool m_isPreemptingImages = false;
    std::uint64_t m_imagePreemptionGeneration = 0;
    AttachmentPacer m_pacer;
    std::vector<std::vector<std::uint8_t>> m_datagrams;
    std::vector<PaneAttachment> m_panes;
    /// Non-table readers expire without dereferencing panes during teardown.
    std::shared_ptr<const EffectReaderLifetime> m_effectReaderLifetime;
    [[nodiscard]] bool admitsCatalogCapacity(std::size_t additional) const;
    /// Correlates a measurement reply with its pane, size bound and deadline.
    struct PendingRichText {
        std::uint32_t handle;
        std::uint32_t token;
        std::size_t characters;
        std::uint64_t expiresAt;
        std::uint64_t rendererRevision;
        std::optional<RichText::FontCatalogQuery> fontCatalog {};
        RichText::FontCatalogScope fontCatalogScope = RichText::FontCatalogScope::Unavailable;
        std::uint32_t totalBytes = 0;
        std::vector<std::uint8_t> bytes {};
    };
    std::vector<PendingRichText> m_pendingRichText;
    std::vector<RichTextMeasureReply> m_richTextReplies;
    std::array<std::optional<ClipboardReadState>, maxPendingClipboardReads> m_clipboardReads;
    std::size_t m_clipboardDeliveryBytes = 0;
    bool m_isDrainingClipboard = false;
    std::function<bool(std::uint32_t, std::uint32_t)> m_clipboardReplyAdmission;
    std::function<ClientBulkDisposition(const BulkChunk&)> m_controlUploadReceiver;
    std::function<bool(std::uint32_t)> m_isControlUploadReserved;
    /// Reads a response only for an outstanding authorized request.
    ///
    /// - Parameters:
    ///   - handle: The responding pane's bound handle.
    ///   - body: The complete typed response header and optional inline data.
    void receiveClipboardData(std::uint32_t handle, std::span<const std::uint8_t> body);
    /// Appends one exact raw response chunk after rechecking authority.
    ///
    /// - Parameter chunk: The decoded transfer chunk borrowed during dispatch.
    void receiveClipboardReplyChunk(const BulkChunk& chunk);
    std::function<bool(std::size_t)> m_inboundAssemblyAdmission;
    /// What kind of client greeted, which sets where its panes' views start.
    ClientKind m_clientKind = ClientKind::Human;
    std::function<bool(std::uint32_t, std::uint64_t)> m_nativeDragAdmission;
    std::vector<NativeDragEvent> m_nativeDragEvents;
    Role m_grantedRole = Role::Viewer;
    /// The client instance its `Greet` named: whose input it sends.
    ID128 m_clientInstance;
    /// The table the attachment is served from, null when the identity is the
    /// attachment.
    AttachmentTable* m_table = nullptr;
    /// The workspaces' structures the client is owed or was sent, by id.
    std::map<std::uint32_t, OwedStructure> m_structures;
    /// The panes listed, in the order they were.
    std::vector<ListedPane> m_listed;
    /// The handle the next listed pane takes; handles are not reused, so no
    /// record sent to one is ever read as another pane's.
    std::uint32_t m_nextListedHandle = firstListedHandle;
    /// The entries the client was last sent, absent before the first notify.
    std::optional<std::vector<AttachmentEntry>> m_sentAttachments;
    /// The table's revision the list was last built at.
    std::uint64_t m_listedTableRevision = 0;
    /// Whether the list must be built again whatever the table says: none
    /// was sent yet, or a notify found no room.
    bool m_areAttachmentsStale = true;
    std::function<std::optional<AttachmentPresence>(ID128, std::span<const ID128>, bool)> m_attachmentPresenceProvider;
    /// The record this connection serves; null without a table.
    AttachmentRecord* m_record = nullptr;
    std::optional<ResumeRefusal> m_resumeRefusal;
    /// Whether the client spoke since its `Welcome`, which ends a resume's
    /// handshake window.
    bool m_hasHeardSinceWelcome = false;
    ConnectionAuthority m_authority;
    std::uint64_t m_connection = 0;
    std::function<PaneSource*(ID128)> m_resolvePane;
    /// The lines of each pane's history the client keeps, as its `Greet`
    /// announced.
    std::uint32_t m_historyCacheLines = 0;
    /// What the client's `Greet` says it can take.
    ClientCapabilities m_capabilities;
    /// The backfill depth the client's `Greet` chose.
    std::uint32_t m_backfillDepthLines = 0;
    /// The first time the host found something awaiting the client since it
    /// last heard from it, from which the silence rule counts.
    std::optional<std::uint64_t> m_unheardSince;
    /// The `Echo` owed the client's newest probe, and how it came; a flood of
    /// probes owes one.
    std::optional<Echo> m_owedEcho;
    std::uint64_t m_owedEchoReceivedAt = 0;
    Carrier m_owedEchoCarrier = Carrier::Stream;
    /// The stream bytes handed to the transport, in stream order.
    std::uint64_t m_streamBytesPopped = 0;
    /// The handles whose `PaneUnbind` is queued, in queue order.
    std::deque<std::uint32_t> m_queuedUnbinds;
    /// A pane unbound with `CheckpointTooLarge`, kept after it is bound again
    /// so a pane that passes the cap again waits longer, until its owner
    /// unbinds it. At most one per pane source.
    struct OversizedPane {
        PaneSource* source = nullptr;
        /// Its handle while unbound, then the handle it was bound again under.
        std::uint32_t handle = 0;
        /// The newest generation measured past the cap.
        std::uint64_t refusedGeneration = 0;
        /// When it may next be measured.
        std::uint64_t retryAt = 0;
        /// Refusals in a row, which set the back-off.
        std::uint32_t refusals = 0;
        /// Bound again and waiting for its checkpoint to leave.
        bool isRebound = false;
    };
    std::vector<OversizedPane> m_oversizedPanes;
    std::vector<PaneRebind> m_paneRebinds;
    /// Calls admitted and not yet answered.
    std::unordered_set<std::uint32_t> m_pendingCalls;
    /// Calls over the cap the owner has not yet refused.
    std::unordered_set<std::uint32_t> m_callsOverLimit;
    /// Writes the answer of a call past the outstanding-call cap; null to
    /// hold such a call for the owner.
    BusyAnswerer m_busyAnswerer = nullptr;
    /// Calls not yet taken by the owner.
    std::vector<ReceivedCall> m_receivedCalls;
    /// A pane frame the stream carried after a call the owner has not served.
    struct HeldFrame {
        FrameKind kind = FrameKind::Raw;
        std::uint32_t channel = 0;
        std::vector<std::uint8_t> body;
    };
    /// Bounds what waits behind unserved calls; one read pass of the owner
    /// carries far less.
    static constexpr std::size_t maxFramesHeldBehindCalls = 4096;
    static constexpr std::size_t maxBytesHeldBehindCalls = std::size_t { 4 } << 20;
    /// Pane input and sizes behind unserved calls, in stream order.
    std::vector<HeldFrame> m_inputBehindCalls;
    std::size_t m_bytesHeldBehindCalls = 0;
    /// The clock of the receive the held frames arrived in.
    std::uint64_t m_lastReceiveNow = 0;
    /// Viewports not yet taken by the owner, one per pane.
    std::vector<ViewportReport> m_viewportReports;
    std::uint32_t m_nextCallIndex = 1;
    /// The pane the deficit round-robin visits first on the next service.
    std::size_t m_roundRobinCursor = 0;
    /// The window refused a frame; nothing but an `Ack` or a datagram's loss
    /// can make room for it, so nothing the window gates is due until then.
    bool m_isWindowRefused = false;
    HostCounters m_counters;
};

/// What a diagnostic client keeps of a pane to replay a divergence offline
/// (Docs/sync.md §6.2, §8): the checkpoint body at the last verified
/// state, and every update body applied after it, through the one whose
/// digest did not match.
struct DivergenceFixture {
    /// The checkpoint body, inflated, as `applyCheckpoint` took it.
    std::vector<std::uint8_t> checkpoint;
    /// The row-update bodies after it, header stripped, as `applyRowUpdate`
    /// took them; the last is the one that diverged once `hasDiverged`.
    std::vector<std::vector<std::uint8_t>> updates;
    /// The update bytes held, which the buffered-update cap bounds.
    std::size_t updateBytes = 0;
    /// An update was refused for its digest; the fixture holds still until
    /// it is taken or the next checkpoint applies.
    bool hasDiverged = false;
    /// The updates outgrew the bound and some were let go, so the fixture
    /// cannot replay what led to the divergence.
    bool isIncomplete = false;
};

/// The history a client keeps of each pane unless its owner says otherwise:
/// the scrollback depth of a configured session.
inline constexpr std::uint32_t defaultHistoryCacheLines = 10'000;

/// The largest checkpoint body either end handles (Docs/sync-measurements.md,
/// "Memory admission"): above the largest body a legal terminal at the largest
/// legal geometry was measured to produce, so only a defect is ever past it.
inline constexpr std::size_t defaultMaxCheckpointBytes = std::size_t { 128 } << 20;

/// Installed image pixels summed over a connection's panes: one terminal's
/// image budget (`Image::terminalImageBytesBudgetDefault`).
inline constexpr std::size_t defaultMaxInstalledImageBytes = std::size_t { 256 } << 20;

/// What a connection's receive owner admits: one checkpoint deflated and
/// inflated at once, the preparation allowance, the installed-image and
/// catalog allowances, and 64 MiB for history, result and structure
/// assemblies beside them.
inline constexpr std::size_t defaultMaxReceiveBytes = 2 * defaultMaxCheckpointBytes + (std::size_t { 128 } << 20) + defaultMaxInstalledImageBytes + (std::size_t { 72 } << 20) + (std::size_t { 64 } << 20);

/// What a client's retained replicas may hold together: eight panes at the
/// history byte cap, or one at the largest legal geometry with a full cache
/// and room for every ordinary pane beside it.
inline constexpr std::size_t defaultMaxRetainedReplicaBytes = std::size_t { 512 } << 20;

/// How far below the head a client asks an unmetered attach to be
/// backfilled unless its owner says otherwise (Docs/sync.md §6.6).
inline constexpr std::uint32_t defaultBackfillDepthLines = 2'000;

/// The client's constants for one connection.
///
/// Each value is supplied by the connection's owner.
struct ClientPolicy {
    /// The largest checkpoint body the client reassembles.
    std::size_t maxCheckpointBytes;
    /// The most update bytes buffered behind a checkpoint in transfer.
    std::size_t maxBufferedUpdateBytes;
    /// The least time the client holds an `Ack` to coalesce updates into it,
    /// on an unmetered link: `clamp(srtt / 4, this, maxAckIntervalMicroseconds)`
    /// (Docs/sync.md §6.4), and the hold before the client has a round trip.
    std::uint64_t minAckIntervalMicroseconds;
    /// The longest hold on an unmetered link.
    std::uint64_t maxAckIntervalMicroseconds = 50'000;
    /// The least hold on a metered or constrained link, where the interval is
    /// `clamp(srtt / 2, this, maxMeteredAckIntervalMicroseconds)` and a radio's
    /// uplink is the cost.
    std::uint64_t minMeteredAckIntervalMicroseconds = 20'000;
    /// The longest hold on a metered or constrained link.
    std::uint64_t maxMeteredAckIntervalMicroseconds = 100'000;
    /// The most lines of each pane's history the client keeps, which its
    /// `Greet` announces. The cap is the client's own (Docs/sync.md §6.6): a
    /// client keeping fewer lines than the host holds the newest of them, and
    /// one keeping none is sent no history but `HistoryReset`.
    std::uint32_t historyCacheLines = defaultHistoryCacheLines;
    /// How far below the head an unmetered attach is backfilled, which the
    /// client's `Greet` announces; the host goes no deeper than the cache.
    std::uint32_t backfillDepthLines = defaultBackfillDepthLines;
    /// The least time input may wait for a frame before the client suspects
    /// the path (Docs/sync.md §6.9); the wait is the larger of this and four
    /// smoothed round trips.
    std::uint64_t stallFloorMicroseconds = 750'000;
    /// The wait before asking again for a pane's checkpoint after the second
    /// refusal in a row, doubling with each one after; the first is asked
    /// for again at once, since one refusal may be a body cut short.
    std::uint64_t checkpointRetryMicroseconds = 250'000;
    /// The longest wait between two asks for a pane's checkpoint.
    std::uint64_t maxCheckpointRetryMicroseconds = 8'000'000;
    /// The refusals in a row after which the client stops asking and
    /// reports the pane failed.
    std::uint32_t maxCheckpointRefusals = 6;
    /// Whether the client keeps a `DivergenceFixture` per pane: a diagnostic
    /// build's choice. A release build keeps none and holds no bytes for it.
    bool shouldRetainDivergenceFixture = false;
    /// The largest `Result` the client reassembles from a transfer: the
    /// queued-bulk cap a host holds one answer to.
    std::size_t maxResultBytes = std::size_t { 16 } << 20;
    /// What the client can take past what every client takes, which its
    /// `Greet` announces. The bound on an inflated body is the cap on the
    /// transfer it arrives in.
    ClientCapabilities capabilities { ClientCapability::DeflateCheckpoints, ClientCapability::SpanOps, ClientCapability::ScrollRectOps, ClientCapability::EchoContext, ClientCapability::DeflateImages };
    /// The largest datagram the carrier sends, or 0 when it has none. With
    /// datagrams the client's own `Probe` goes as one (Docs/sync.md §6.4); an
    /// `Echo` always answers on the carrier its `Probe` came on.
    std::size_t datagramMaxBytes = 0;
    /// The combined capacity allowed for a checkpoint's two temporary decoded
    /// screen-cell arrays, checked before preparation allocations. This local
    /// allowance is independent of the encoded-body cap and applies
    /// to live receive and persisted pictures alike. Zero admits no screen.
    /// It excludes destination grids, dictionaries, history, and other scratch
    /// owners; it is not an aggregate terminal or connection memory bound.
    std::size_t maxDecodedScreenCellBytes = std::size_t { 32 } << 20;
    /// The complete checkpoint preparation capacity plus destination backing
    /// peak, including exact text/dictionary containers and fixed scratch.
    /// This per-checkpoint allowance excludes allocator metadata,
    /// borrowed wire bytes, and previously retained non-grid model owners.
    /// It is not an aggregate connection memory limit. Zero admits nothing.
    std::size_t maxCheckpointStorageCost = std::size_t { 128 } << 20;
    /// The connection's receive owner (Docs/sync.md §7.9, §9): what checkpoint,
    /// history, result and structure assemblies, inflated bodies, the
    /// checkpoint being prepared, installed image pixels summed over every
    /// pane, and the catalog families (image transfers, samples, deferred
    /// effects, clipboard payloads) may hold together. A transfer past it is
    /// refused with a reason; zero admits nothing.
    std::size_t maxReceiveBytes = defaultMaxReceiveBytes;
    /// Installed image pixels, and destinations reserved for transfers in
    /// flight, summed over every pane the connection binds: one terminal's
    /// image budget, so pixels can never crowd the screen out of the receive
    /// owner.
    std::size_t maxInstalledImageBytes = defaultMaxInstalledImageBytes;
    /// The bytes of each pane's history cache, beside its line cap: the
    /// oldest lines go first past either.
    std::size_t historyCacheBytes = defaultHistoryCacheBytes;
    /// What every retained replica's grid backing and history cache may hold
    /// together, live and on cold restore (`ReplicaSet::memory`). A checkpoint
    /// whose grid does not fit is refused with a reason; history past it
    /// evicts the ingesting pane's oldest lines.
    std::size_t maxRetainedReplicaBytes = defaultMaxRetainedReplicaBytes;
};

/// The storage a replica's picture holds: its grid's backing arrays at their
/// capacity and its scrollback store's owned backing, excluding the terminal
/// object, allocator bookkeeping and lazily built rendering caches.
///
/// - Parameter terminal: The picture.
/// - Returns: The bytes.
[[nodiscard]] std::size_t retainedPictureBytes(const Ubiquitty::Terminal& terminal);

/// The client's constants, as measured on the corpus over shaped links by
/// test_sync_shaped_link.
///
/// - Parameter historyCacheLines: The most lines of each pane's history the
///   client keeps; 0 keeps none.
/// - Returns: The policy.
[[nodiscard]] ClientPolicy measuredClientPolicy(std::uint32_t historyCacheLines = defaultHistoryCacheLines);

/// The panes' replicas a client keeps across its connections (Docs/sync.md
/// §6.9): the terminal it draws and what it knows of each screen, by pane, so
/// a connection that replaces a dropped one binds the pane to the same picture
/// rather than to a blank one, and can say what it holds.
///
/// The replicas belong to one host instance. A pane that instance no longer
/// has, or every pane when the host restarted unplanned, ends: its replica is
/// kept read-only and is never bound again, so nothing cached of one instance
/// mixes with another's (§6.9 "Host restarted").
class ReplicaSet {
public:
    /// One pane's replica.
    struct Held {
        /// The pane's terminal, when the set owns it.
        std::unique_ptr<Ubiquitty::Terminal> terminal;
        ScreenReplica replica;
        /// Whether a checkpoint was applied, so `replica` describes the
        /// picture.
        bool hasCheckpoint = false;
        /// The pane's terminal, when the client draws the pane into one of
        /// its own (`ClientPaneDelegate::terminalForPane`): lent, never
        /// owned, and it outlives the set's use of it by the lender's word.
        /// What is known of the pane is known of this terminal and no other.
        Ubiquitty::Terminal* lentTerminal = nullptr;
        /// The status the screen last carried, while a status set the host
        /// sent for a view that is not live stands in the picture in its
        /// place: `replica` describes the picture with this status in it.
        std::optional<OSC::ProgramStatusStore> screenStatus;

        /// The terminal the replica describes, owned or lent.
        [[nodiscard]] Ubiquitty::Terminal* picture() const { return terminal ? terminal.get() : lentTerminal; }
    };

    /// Why a pane ended.
    enum class EndReason : std::uint8_t {
        /// Its host restarted unplanned; its program is gone.
        HostRestarted,
        /// Its host no longer has it.
        PaneGone,
    };

    /// The replica of a pane that ended, and what its owner tells the person
    /// about it.
    struct Ended {
        /// The instance whose pane it was.
        ID128 hostInstanceID;
        ID128 paneID;
        std::unique_ptr<Held> held;
        EndReason reason = EndReason::PaneGone;
        /// Input events typed for the pane that were never written and never
        /// will be: input for a program that is gone is not sent elsewhere.
        std::size_t discardedInputEvents = 0;
    };

    /// The most ended replicas kept; past it the oldest is dropped.
    static constexpr std::size_t maxEnded = 256;

    /// Notes the instance a `Welcome` named, before it binds any pane. One
    /// that is neither the instance the replicas came from nor its named
    /// successor ends every replica.
    ///
    /// - Parameters:
    ///   - instance: The `Welcome`'s `hostInstanceID`.
    ///   - predecessor: Its `predecessorHostInstanceID`.
    /// - Returns: Whether the replicas ended.
    bool noteHostInstance(ID128 instance, std::optional<ID128> predecessor);

    /// The instance the replicas came from, absent before any `Welcome`.
    [[nodiscard]] std::optional<ID128> hostInstanceID() const { return m_hostInstanceID; }

    /// Ends a pane's replica.
    ///
    /// - Parameters:
    ///   - paneID: The pane.
    ///   - reason: Why it ended.
    void end(ID128 paneID, EndReason reason);

    /// An ended replica, to note what was discarded with it.
    ///
    /// - Parameters:
    ///   - hostInstanceID: The instance whose pane it was.
    ///   - paneID: The pane.
    /// - Returns: The newest such replica, or null.
    [[nodiscard]] Ended* findEnded(ID128 hostInstanceID, ID128 paneID);

    /// The ended replicas, oldest first.
    [[nodiscard]] std::span<const Ended> ended() const { return m_ended; }

    /// The replica of a pane, or null when none is held.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The replica, its address stable until it is forgotten.
    [[nodiscard]] Held* find(ID128 paneID);

    /// The replica of a pane, to read, or null when none is held.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The replica.
    [[nodiscard]] const Held* find(ID128 paneID) const;

    /// Reads effect receipt state independently of a replaceable screen picture.
    ///
    /// - Parameter paneID: The stable pane on this host instance.
    /// - Returns: Its retained cursors, or zero before an effect arrives.
    [[nodiscard]] EffectReceiptState effectReceipt(ID128 paneID) const;
    /// Retains consumed effects before an application callback may reconnect.
    ///
    /// - Parameters:
    ///   - paneID: The stable pane on this host instance.
    ///   - state: The validated monotonic receipt state to retain.
    /// - Returns: False if the bounded receipt table is full or state regresses.
    [[nodiscard]] bool keepEffectReceipt(ID128 paneID, EffectReceiptState state);

    /// Retires receipt state for a definitively unbound or ended pane.
    ///
    /// Transport loss and picture replacement must retain these cursors so
    /// a resumed partial batch cannot repeat an application callback.
    ///
    /// - Parameter paneID: The exact stable pane whose attachment has ended.
    void forgetEffectReceipt(ID128 paneID);

    /// Borrows the stable pane's shared history owner across connection changes.
    /// - Parameter paneID: The exact pane in this host instance.
    /// - Returns: The owner, or none before history is retained.
    [[nodiscard]] std::shared_ptr<HistoryCache> history(ID128 paneID) const;

    /// Retains a bounded cache independently of a replaceable screen picture.
    /// - Parameter history: The cache for this set's authenticated host instance.
    /// - Returns: False for another host, an empty pane identity, or a full table.
    [[nodiscard]] bool keepHistory(std::shared_ptr<HistoryCache> history);

    /// Retires history for a definitively unbound pane, never mere transport loss.
    /// - Parameter paneID: The exact pane whose attachment ended.
    void forgetHistory(ID128 paneID);

    /// Keeps a pane's replica.
    ///
    /// - Parameters:
    ///   - paneID: The pane.
    ///   - terminal: Its terminal.
    /// - Returns: The replica, its address stable until it is forgotten.
    Held& hold(ID128 paneID, std::unique_ptr<Ubiquitty::Terminal> terminal);

    /// Keeps the replica of a pane the client draws into a terminal of its
    /// own, in place of anything kept for the pane.
    ///
    /// - Parameters:
    ///   - paneID: The pane.
    ///   - terminal: The client's terminal, which the client keeps alive
    ///     while the pane is held.
    /// - Returns: The replica, its address stable until it is forgotten.
    Held& holdLent(ID128 paneID, Ubiquitty::Terminal& terminal);

    /// The terminal a pane's replica draws, or null.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The terminal.
    [[nodiscard]] const Ubiquitty::Terminal* terminal(ID128 paneID) const;

    /// Drops a pane's picture, preserving its independent effect receipts.
    ///
    /// - Parameter paneID: The pane.
    void forget(ID128 paneID);

    /// Drops every picture held in a terminal the client lent and is taking
    /// back, preserving the panes' effect receipts and history. A pane held
    /// while unbound keeps its lent picture for a rebind, so a lender going
    /// away must say so here or the set reads a freed terminal.
    ///
    /// - Parameter terminal: The lent terminal.
    void forgetLent(const Ubiquitty::Terminal& terminal);

    /// What the replicas hold: each picture's grid backing and scrollback
    /// store, counted once however many panes draw it, ended ones included,
    /// and each retained history cache. Its owner holds this to
    /// `ClientPolicy::maxRetainedReplicaBytes`.
    ///
    /// - Returns: The sum.
    [[nodiscard]] std::size_t retainedBytes() const;

private:
    std::vector<std::pair<ID128, std::unique_ptr<Held>>> m_held;
    std::vector<Ended> m_ended;
    std::optional<ID128> m_hostInstanceID;
    std::vector<std::pair<ID128, EffectReceiptState>> m_effectReceipts;
    std::vector<std::shared_ptr<HistoryCache>> m_history;
};

class ClientConnection;

/// What a client connection's owner supplies for its panes and learns about
/// them.
///
/// Notifications run inside `receiveStream`, `receiveDatagram` or `tick`, on
/// the thread that called it. An owning ClientAttachment retains the reader
/// when a notification reconnects it. Lookup callbacks (`terminalForPane` and
/// `viewOnBind`) must not re-enter or replace the connection or its attachment;
/// `terminalForPane` is also queried while constructing a resume.
class ClientPaneDelegate {
public:
    virtual ~ClientPaneDelegate() = default;

    /// Reports one complete canonical frame before its frame-specific callbacks.
    /// Like other notifications, this may retire or replace the connection;
    /// the owner retains the reader until this receive returns.
    /// - Parameters:
    ///   - kind: The decoded envelope kind.
    ///   - wireBytes: The exact envelope header and body bytes, not a read's size.
    virtual void connectionFrameDecoded(FrameKind /*kind*/, std::size_t /*wireBytes*/) { }

    /// Reports a frame after its state and records have been applied, before
    /// another frame is decoded. The reader remains retained through this
    /// notification, which may retire or replace it.
    /// - Parameter connection: The reader that applied this frame.
    virtual void connectionFrameApplied(const ClientConnection& /*connection*/) { }

    /// Reports an admitted Welcome before any pane or effect is delivered.
    /// Like other notifications, this may retire or replace the connection.
    /// - Parameter welcome: The authenticated identity, borrowed for this call.
    virtual void hostWelcomed(const Welcome& /*welcome*/) { }

    /// Reports one validated terminal own-status before its following Bye.
    /// Like Welcome, this callback may retire or replace the connection.
    /// - Parameter status: The exact denied or expired pending question.
    virtual void consentRefused(const ConsentStatus& /*status*/) { }

    /// The terminal a newly bound pane replicates into.
    ///
    /// A frontend that renders a terminal of its own returns it, so the
    /// replica is the terminal it draws; the terminal must outlive the
    /// binding. This is a non-reentrant lookup: it must not stop or reconnect
    /// the owner, and the returned terminal must remain valid throughout use.
    ///
    /// - Parameter paneID: The pane the host bound.
    /// - Returns: The terminal, or null for one the connection owns.
    virtual Ubiquitty::Terminal* terminalForPane(ID128 /*paneID*/) { return nullptr; }

    /// Reports that a bound pane's terminal took a checkpoint, an update or,
    /// for a pane held `Dormant`, a palette block.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle on this connection.
    ///   - paneID: The pane.
    virtual void paneChanged(std::uint32_t /*handle*/, ID128 /*paneID*/) { }

    /// Consumes one new typed effect; all arguments borrow the current frame.
    ///
    /// Receipt state is retained before this callback, so a reconnect cannot
    /// invoke it twice. A deferred native action must copy its needed values.
    /// Command data is delivered even when presentation is Ignore for an agent.
    ///
    /// - Parameters:
    ///   - handle: The current physical connection's pane handle.
    ///   - effect: Stable identity, primary and count of the admitted effect.
    ///   - body: Its validated non-clipboard body.
    ///   - disposition: Native presentation standing, never an authority grant.
    virtual void paneEffect(std::uint32_t /*handle*/, const EffectView& /*effect*/, const EffectBody& /*body*/, EffectDisposition /*disposition*/) { }

    /// Asks the frontend to apply its local clipboard-read permission policy.
    /// All views borrow the active frame, retained through this notification.
    ///
    /// - Parameters:
    ///   - handle: The authenticated pane's physical handle.
    ///   - paneID: Its stable identity.
    ///   - request: The exact operation, never an authorization grant.
    /// - Returns: Whether a handler accepted responsibility for answering.
    virtual bool paneClipboardRequest(std::uint32_t /*handle*/, ID128 /*paneID*/, const ClipboardRead& /*request*/) { return false; }
    /// Delivers a validated command while its payload remains borrowed.
    virtual void paneNativeDragCommand(std::uint32_t /*handle*/, ID128 /*paneID*/, const NativeDragCommand&) { }

    /// Reports a pane the host bound.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle on this connection.
    ///   - paneID: The pane.
    virtual void paneBound(std::uint32_t /*handle*/, ID128 /*paneID*/) { }

    /// Reports a pane the host unbound; its handle names nothing after this.
    ///
    /// - Parameters:
    ///   - handle: The handle the pane held.
    ///   - paneID: The pane.
    virtual void paneUnbound(std::uint32_t /*handle*/, ID128 /*paneID*/) { }

    /// The view a newly bound pane takes, or `nullopt` for the connection's
    /// own: `Live` for a human or a viewer, `RecordsOnly` for an agent.
    /// This is a non-reentrant lookup; it must not stop or reconnect the owner.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle on this connection.
    ///   - paneID: The pane.
    /// - Returns: The view.
    virtual std::optional<ViewState> viewOnBind(std::uint32_t /*handle*/, ID128 /*paneID*/) { return std::nullopt; }

    /// Reports that the client stopped asking for a pane's checkpoint after
    /// refusing it `maxCheckpointRefusals` times in a row: the pane keeps the
    /// picture it had, and its view is `RecordsOnly` from then on.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle on this connection.
    ///   - paneID: The pane.
    ///   - reason: Why the last checkpoint was refused.
    virtual void paneFailed(std::uint32_t /*handle*/, ID128 /*paneID*/, std::string_view /*reason*/) { }

    /// Reports that a `Dormant` pane changed since it went `Dormant`.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle on this connection.
    ///   - paneID: The pane.
    virtual void paneDirty(std::uint32_t /*handle*/, ID128 /*paneID*/) { }

    /// Reports that a bound pane's facts or status records were replaced by
    /// newer ones; `ClientConnection::paneFacts` and `paneStatus` hold them.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle on this connection.
    ///   - paneID: The pane.
    virtual void paneRecordsChanged(std::uint32_t /*handle*/, ID128 /*paneID*/) { }

    /// Reports that the host listed a pane, whose records will follow.
    ///
    /// - Parameter paneID: The pane.
    virtual void paneListed(ID128 /*paneID*/) { }

    /// Reports that a listed pane's facts or status set were replaced by
    /// newer ones; `ClientConnection::listedPaneFacts` and
    /// `listedPaneStatus` hold them.
    ///
    /// - Parameter paneID: The pane.
    virtual void listedPaneChanged(ID128 /*paneID*/) { }

    /// Reports that the host ended a pane's listing: the pane is gone, or
    /// the listing is.
    ///
    /// - Parameter paneID: The pane.
    virtual void paneUnlisted(ID128 /*paneID*/) { }

    /// Reports that a workspace's structure arrived or was withdrawn;
    /// `ClientConnection::workspaceStructures` holds what stands.
    ///
    /// - Parameter workspaceID: The workspace.
    virtual void workspaceStructureChanged(std::uint32_t /*workspaceID*/) { }

    /// Offers a complete Result at its exact position in the input stream.
    /// The callback must not re-enter ingress or write to the carrier.
    /// - Parameter text: Borrowed only until this callback returns.
    /// - Returns: Whether the operation owner consumed it; other replies queue normally.
    virtual bool consumeControlResult(std::string_view /*text*/) { return false; }
};

/// What the client tells the host about itself.
struct ClientIdentity {
    ID128 clientInstance {};
    ClientKind kind = ClientKind::Human;
    Role requestedRole = Role::Owner;
    /// The fingerprint this client states and accepts; this build's unless a
    /// test stands up a client of another build.
    EngineFingerprint engineFingerprint = wireFingerprint;
    Facts facts {};
    /// The attachment to continue, and what is held of each pane; absent for
    /// a fresh attachment.
    std::optional<ResumeRequest> resume {};
    /// The wire version this client speaks; this build's unless a test
    /// stands up a peer at another.
    std::uint16_t wireVersion = Sync::wireVersion;
    AdmissionRequest request {};
    /// Per-connection credentials are not part of any protected resume record.
    std::optional<std::vector<std::uint8_t>> credential {};
    std::string appVersion = std::string(Sync::appVersion());
    std::string platform {};
};

/// What a client connection has done, for the journal and the tests.
struct ClientCounters {
    /// Panes bound and unbound, so an observer can tell a handle bound anew
    /// from the pane it held before.
    std::uint64_t paneBindings = 0;
    std::uint64_t checkpointsApplied = 0;
    std::uint64_t updatesApplied = 0;
    /// Updates already held, older than the held generation, or arriving while
    /// a `Resync` is outstanding.
    std::uint64_t updatesIgnored = 0;
    std::uint64_t digestMismatches = 0;
    std::uint64_t resyncsSent = 0;
    std::uint64_t acksSent = 0;
    std::uint64_t paneDirtiesReceived = 0;
    /// `PanePalette` blocks applied to a pane's terminal.
    std::uint64_t palettesApplied = 0;
    /// Records taken: a `PaneFacts`, or a status set once it is whole.
    std::uint64_t recordsApplied = 0;
    /// Workspace structures taken, and withdrawals of ones held.
    std::uint64_t structuresApplied = 0;
    /// Transfers the host abandoned, whose partial bodies were freed.
    std::uint64_t bulkAbortsReceived = 0;
    /// History epochs retired, by a `HistoryReset` or a checkpoint of a later
    /// epoch.
    std::uint64_t historyResetsApplied = 0;
    /// Panes the client stopped asking checkpoints for.
    std::uint64_t panesFailed = 0;
    /// Checkpoints refused, for any reason, each answered by the retry policy.
    std::uint64_t checkpointsRefused = 0;
    /// Transfers the receive owner had no room for (`ClientPolicy::maxReceiveBytes`).
    std::uint64_t transfersOverBudget = 0;
    /// History stores after which a pane's cache was trimmed to keep every
    /// retained replica within `ClientPolicy::maxRetainedReplicaBytes`.
    std::uint64_t historyTrimsForReplicas = 0;
    /// `Probe`s the client sent: at the welcome, and when input waited.
    std::uint64_t probesSent = 0;
    /// The host's `Notify`s dropped for arriving while `maxUnreadNotifies`
    /// waited unread.
    std::uint64_t notifiesDropped = 0;
    /// The host's `Notify`s of a kind this build does not know, skipped by
    /// their length.
    std::uint64_t notifiesSkipped = 0;
};

/// The host `Notify`s a client holds unread before it drops the next.
inline constexpr std::size_t maxUnreadNotifies = 256;

/// A synchronous identity barrier before a Welcome changes client state.
/// The sink must not reenter or destroy its connection or attachment.
class ClientWelcomeSink {
public:
    virtual ~ClientWelcomeSink() = default;
    /// Admits the authenticated identity before any pane or effect is accepted.
    ///
    /// - Parameter welcome: The fully decoded, version-validated Welcome.
    /// - Returns: Whether this identity may replace the current client state.
    [[nodiscard]] virtual bool acceptWelcome(const Welcome& welcome) = 0;
};

/// The client's end of one connection.
class ClientConnection {
public:
    /// Creates a connection; `start` sends its `Greet`.
    ///
    /// - Parameters:
    ///   - policy: The connection's constants.
    ///   - identity: What `Greet` tells the host.
    ///   - delegate: Supplies the panes' terminals and hears them change, or
    ///     null for a connection that owns every replica; it must outlive the
    ///     connection.
    ///   - purgeSink: Told of every history epoch a `HistoryReset` retires, so
    ///     the copies of a pane's history the connection does not own — a
    ///     resume record, a disk cache — are purged before anything else
    ///     applies (Docs/sync.md §6.6); borrowed, may be null, and must
    ///     outlive the connection.
    ///   - replicas: Where the panes' replicas live, so they outlive this
    ///     connection: a pane bound again is bound to the picture held; null
    ///     for a connection whose replicas go with it. Borrowed; it must
    ///     outlive the connection.
    ///   - effectSink: Commits receipts before callbacks or ACKs; null for
    ///     process-local delivery. Borrowed and must outlive the connection.
    ///   - welcomeSink: Admits the authenticated identity before any replica
    ///     changes; borrowed, optional, and must outlive the connection.
    ClientConnection(const ClientPolicy& policy, ClientIdentity identity, ClientPaneDelegate* delegate = nullptr, HistoryPurgeSink* purgeSink = nullptr, ReplicaSet* replicas = nullptr, EffectReceiptSink* effectSink = nullptr, ClientWelcomeSink* welcomeSink = nullptr);

    /// Writes each bound pane's replica back to the replica set, so a later
    /// connection binds them as this one leaves them. The destructor does
    /// this too.
    void storeReplicas();

    /// Destroys the connection and its replica terminals.
    ~ClientConnection();

    /// Queues the `Greet`.
    void start();

    /// Publishes the native typography of one bound pane; equal facts send nothing.
    ///
    /// - Parameters:
    ///   - handle: The pane's current binding, not a connection-wide default.
    ///   - facts: The table; one that is not well formed is ignored.
    void publishTypography(std::uint32_t handle, const RichText::TypographyFacts& facts);

    /// Tells the host the faces the client draws proportional runs in, now
    /// when attached and on attaching otherwise; equal faces send nothing.
    ///
    /// - Parameter faces: The faces; ones a host cannot count in are ignored.
    void publishProportionalFaces(const OSC::TextSizing::ProportionalFaceFacts& faces);

    /// Reads bytes from the stream.
    ///
    /// - Parameters:
    ///   - bytes: The next bytes, in stream order.
    ///   - now: The client clock in microseconds.
    void receiveStream(std::span<const std::uint8_t> bytes, std::uint64_t now);

    /// Reads one datagram.
    ///
    /// - Parameters:
    ///   - datagram: The datagram's payload, exactly one envelope.
    ///   - now: The client clock in microseconds.
    void receiveDatagram(std::span<const std::uint8_t> datagram, std::uint64_t now);

    /// Sends an `Ack` when one is due.
    ///
    /// - Parameter now: The client clock.
    void tick(std::uint64_t now);

    /// When `tick` next has an `Ack` to send.
    ///
    /// - Returns: The client time, or `nullopt` when nothing is owed.
    [[nodiscard]] std::optional<std::uint64_t> nextTickMicroseconds() const;

    /// Asks for a checkpoint of a pane whose terminal no longer holds what the
    /// connection applied to it.
    ///
    /// A frontend that rewrites a replica's grid itself — a local clear or a
    /// local reflow — leaves the row hashes the next update is checked
    /// against describing a screen the terminal no longer shows; an update
    /// would then apply onto the wrong rows and pass its digest. Until the
    /// checkpoint arrives the pane applies no update.
    ///
    /// - Parameter handle: The pane's handle; an unbound one is ignored.
    void requestCheckpoint(std::uint32_t handle);

    /// Moves a bound pane onto another terminal and asks for a checkpoint to
    /// fill it.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle; an unbound one is ignored.
    ///   - terminal: The terminal to replicate into, which must outlive the
    ///     binding, or null for one the connection makes.
    void setPaneTerminal(std::uint32_t handle, Ubiquitty::Terminal* terminal);

    /// Moves every bound pane replicating into a lent terminal onto one the
    /// connection makes, before the lender frees it.
    ///
    /// - Parameter terminal: The lent terminal.
    void releaseTerminal(const Ubiquitty::Terminal& terminal);

    /// Asks the host for history lines of a bound pane: the ranges replace
    /// what the client wanted at the priority (Docs/sync.md §6.6).
    ///
    /// The client keeps its own sets, and never sends a want that would take
    /// the lines it still misses of them past the cap `Welcome` advertised.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - priority: The want's priority.
    ///   - ranges: The lines, sorted, disjoint and none empty; none withdraws
    ///     the want.
    /// - Returns: False, sending nothing, for a pane not bound or whose
    ///   checkpoint has not arrived, ranges not as above or more than
    ///   `maxHistoryWantRanges`, or a want past the cap.
    [[nodiscard]] bool wantHistory(std::uint32_t handle, HistoryPriority priority, std::span<const LineRange> ranges);

    /// Sets the view the client holds of a bound pane, telling the host with
    /// `View` when it changes (Docs/sync.md §6.8).
    ///
    /// - Parameters:
    ///   - handle: The pane's handle; an unbound one is ignored.
    ///   - state: The view.
    void setView(std::uint32_t handle, ViewState state);

    /// The view the client holds of a bound pane, or `nullopt`.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The view.
    [[nodiscard]] std::optional<ViewState> paneView(std::uint32_t handle) const;

    /// Records input sent to the host, and sends the `Ack` owed with it, so a
    /// keystroke and the acknowledgement it would otherwise wait behind leave
    /// in one carrier write. Input no receipt has covered for
    /// `max(4 × srtt, stallFloor)` draws a `Probe`; the same span again with
    /// no frame of any kind moves the connection to `Lost`, so a dead path is
    /// found while the user types rather than on a transport timeout
    /// (Docs/sync.md §6.9). A frame that answers is contact: the input's wait
    /// starts again, and it draws another probe only after another span.
    ///
    /// - Parameter now: The client clock.
    void noteInputSent(std::uint64_t now);

    /// Probes the host before the client trusts the connection again: when
    /// the app returns to the foreground, or on an OS network-path event.
    /// The probe travels on the stream, so a path that is back a moment
    /// later still carries it, and only its own `Echo` answers it: frames
    /// the host sent before it read the probe, queued while the process was
    /// suspended or the old path was up, arrive ahead and say nothing of the
    /// path the connection is on now. The owner times the silence
    /// (`isLivenessUnproven`); nothing here declares the connection `Lost`
    /// for it.
    ///
    /// - Parameter now: The client clock.
    void checkLiveness(std::uint64_t now);

    /// Notes that the transport is gone: the connection reads and sends
    /// nothing more. Its owner may be inside one of its reads, which returns
    /// after the frame in hand. The final replicas are stored now, and this
    /// retired connection never writes their metadata again when destroyed.
    void abandon();

    /// Whether a `checkLiveness` probe is still unanswered.
    [[nodiscard]] bool isLivenessUnproven() const { return m_livenessProbe.has_value(); }

    /// The link as the frontend shows it: whether the host has gone silent
    /// past its probe schedule, and whether input waits for its receipt.
    ///
    /// - Parameter now: The client clock.
    /// - Returns: The link's state.
    [[nodiscard]] LinkState linkState(std::uint64_t now) const;

    /// When the link turns stalled if nothing more arrives
    /// (`nextStallMicroseconds`).
    ///
    /// - Parameter now: The client clock.
    /// - Returns: The time, or `nullopt` when no indicator is waiting to turn.
    [[nodiscard]] std::optional<std::uint64_t> nextLinkStallMicroseconds(std::uint64_t now) const;

    /// Sets whether the client is focused, telling the host with `Facts` when
    /// it changes after the `Greet`; before, the `Greet` carries it. The host
    /// probes an idle connection only while its client is focused.
    ///
    /// - Parameter focus: Whether the client is focused.
    void setFocus(Ubiquitty::Modes::FocusState focus);

    /// Sets the theme the client draws in, which the host answers the
    /// program's colour queries from while this client owns the pane's size
    /// (Docs/sync.md §6.11). It travels in a `Facts` of its own: the first
    /// frame after the `Welcome`, ahead of any input, and again whenever it
    /// changes.
    ///
    /// - Parameter palette: The theme, or `nullopt` for none.
    void setThemePalette(std::optional<ThemePalette> palette);

    /// Sets the size of the client's window, telling the host with `Facts`
    /// when it changes after the `Greet`; before, the `Greet` carries it. It
    /// is the size of a pane that is all the window shows; a pane laid out
    /// beside others takes its size from the client's `Viewport` of it.
    ///
    /// - Parameter viewport: The window's grid and the pixels it is drawn
    ///   in; the drawable's are a pane's alone and are not carried.
    void setWindowViewport(const Viewport& viewport);

    /// Sets what the client's network path costs, telling the host with
    /// `Facts` when it changes after the `Greet`; before, the `Greet` carries
    /// it. A metered or constrained link is pushed no history beyond one
    /// viewport, and a backfill in flight is aborted (Docs/sync.md §6.6).
    ///
    /// - Parameter link: The link's cost.
    void setLinkCost(LinkCost link);

    /// Sends one event under a number its owner chose: a client whose input
    /// outlives the connection numbers it across connections, and resends an
    /// event under the number it first had, so the host writes it once.
    /// Later events this connection numbers itself follow it.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - inputSeq: The event's number, from 1.
    ///   - intent: The event: bytes, keys, pointer events, or a whole paste,
    ///     one final piece from offset 0. Focus is not numbered and goes
    ///     through `sendFocus`.
    /// - Returns: Whether it was sent; nothing is sent before the `Welcome`,
    ///   once closed, for a pane not bound, for focus, for an event that is
    ///   not one frame, or of a kind the host's version lacks.
    [[nodiscard]] bool sendNumbered(std::uint32_t handle, std::uint64_t inputSeq, const InputIntent& intent);

    /// Pastes text into a pane, numbered next in the pane's order: one event
    /// whose pieces ride the bulk lane, at most one piece per
    /// `takeStreamOutput` behind everything interactive, so a key typed
    /// during the paste never waits behind more than one piece. The host
    /// writes it under bracketed paste and the paste normaliser (Docs/sync.md
    /// §6.7).
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - text: UTF-8 text, no longer than the host's paste cap.
    /// - Returns: The paste's number, or `nullopt`, sending nothing, before
    ///   the `Welcome`, once closed, for a pane not bound, for text past the
    ///   cap or not UTF-8, or to a host that takes no paste.
    [[nodiscard]] std::optional<std::uint64_t> sendPaste(std::uint32_t handle, std::string_view text);

    /// Says goodbye and closes. `Normal` leaves the attachment for good;
    /// `Suspending` tells the host the client is about to be suspended and
    /// means to resume, so the host holds the attachment `Lost` at once
    /// rather than finding the silence (Docs/sync.md §6.9). Nothing is sent
    /// before the `Welcome` or once closed.
    ///
    /// - Parameter reason: `Normal` or `Suspending`.
    void leave(ByeReason reason);

    /// Gives up a pane's paste in progress: the pieces not yet sent are
    /// dropped and, when any was sent, a `PasteAbort` goes on the interactive
    /// lane, ahead of what follows.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The paste's number, or `nullopt` when none was in progress.
    std::optional<std::uint64_t> abortPaste(std::uint32_t handle);

    /// Gives up a paste numbered on an earlier connection, whose pieces the
    /// host may hold some of: a `PasteAbort` under its number, which ends it
    /// where the host's taking stopped, or before it starts.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - inputSeq: The paste's number.
    /// - Returns: Whether it was sent; not before the `Welcome`, once closed,
    ///   for a pane not bound, or to a host that takes no paste.
    [[nodiscard]] bool sendPasteAbort(std::uint32_t handle, std::uint64_t inputSeq);

    /// The characters a pane's line discipline signals on, as its echo
    /// context last said, or a new terminal's before it says.
    ///
    /// - Parameter handle: The pane's handle.
    [[nodiscard]] InterruptCharacters interruptCharacters(std::uint32_t handle) const;

    /// Whether a pane has a paste with pieces still to send.
    ///
    /// - Parameter handle: The pane's handle.
    [[nodiscard]] bool isPasting(std::uint32_t handle) const;

    /// Takes the runs of this client's events the host dropped unwritten
    /// since the last call (`InputDiscarded`), oldest first. A run may
    /// arrive after a count that passes it, on a carrier that reorders: the
    /// run, not the count, says what became of its events.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The runs, none for a pane not bound.
    [[nodiscard]] std::vector<InputDiscarded> takeInputDiscards(std::uint32_t handle);

    /// Sends input as bytes, written to the pane's program verbatim, numbered
    /// next in the pane's order from 1. Bytes holding one of the pane's
    /// interrupt characters give up its paste in progress first, so the
    /// program reads the bracket's end and then them; other input follows
    /// the whole paste.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - bytes: The input.
    /// - Returns: The event's number, or `nullopt`, sending nothing, before
    ///   the `Welcome`, once closed, for a pane not bound, for bytes that are
    ///   not one frame, or to a host speaking a version without `Raw`.
    [[nodiscard]] std::optional<std::uint64_t> sendRaw(std::uint32_t handle, std::span<const std::uint8_t> bytes);

    /// Sends keys the user typed into a pane, numbered next in the pane's
    /// order. The host encodes them under the modes live when it writes them.
    /// An interrupt among them gives up the pane's paste first, as for
    /// `sendRaw`.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - events: The keys, in order; at most `maxInputEventsPerFrame`.
    /// - Returns: The number, or `nullopt`, sending nothing, as for `sendRaw`,
    ///   or for no events or too many.
    [[nodiscard]] std::optional<std::uint64_t> sendKeys(std::uint32_t handle, std::span<const KeyEvent> events);

    /// Sends pointer events the pointer ladder routed to a pane's program,
    /// numbered next in the pane's order.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - events: The events, in order; at most `maxInputEventsPerFrame`.
    /// - Returns: The number, or `nullopt`, sending nothing, as for `sendKeys`.
    [[nodiscard]] std::optional<std::uint64_t> sendPointer(std::uint32_t handle, std::span<const PointerEvent> events);
    [[nodiscard]] bool sendNativeDragEvent(std::uint32_t handle, const DragDrop::NativeEvent&, std::span<const char>);

    /// Tells the host whether this client's view of a pane holds the focus.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - focus: Whether it does.
    /// - Returns: Whether it was sent; not before the `Welcome`, once closed,
    ///   for a pane not bound, or to a host speaking a version without `Focus`.
    [[nodiscard]] bool sendFocus(std::uint32_t handle, Ubiquitty::Modes::FocusState focus);

    /// Tells the host the size this client shows a pane at. Only a person's
    /// client has one: an agent's resize is the call `pane.overrideGeometry`,
    /// and a viewer sizes nothing.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - viewport: Its grid, and the pixels the client draws it in, zero
    ///     where it has measured none.
    /// - Returns: Whether it was sent; not before the `Welcome`, once closed,
    ///   for a pane not bound, for a size of no rows or no columns, or from
    ///   a client that is not a person's.
    [[nodiscard]] bool sendViewport(std::uint32_t handle, const Viewport& viewport);

    /// How far the host reports a pane's input from this client written and
    /// settled: the newest of every `Update` and `Receipt` it sent.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The report, zero for a pane not bound or not yet reported.
    [[nodiscard]] Receipt receipt(std::uint32_t handle) const;

    /// Sends a `Call`; its `Result` arrives through `takeResults`.
    ///
    /// - Parameter request: A JSON-RPC 2.0 request with an id.
    /// - Returns: False, sending nothing, before the `Greet`, once closed, or
    ///   for text that is not UTF-8 or not one frame.
    [[nodiscard]] bool call(std::string_view request);

    /// Answers one still-pending clipboard request after local permission.
    /// Payload bytes are copied into exact fallible ownership before staging.
    ///
    /// - Parameters:
    ///   - requestID: The request accepted on this physical connection.
    ///   - status: The permission or read result.
    ///   - bundle: The requested inventory or exact MIME item, empty otherwise.
    /// - Returns: Whether one response was staged; unknown/repeated IDs refuse.
    [[nodiscard]] bool answerClipboard(std::uint32_t requestID, EffectClipboardStatus status, const EffectClipboardBundle& bundle = {});

    /// Queues one raw chunk after an operation's owner matched upload credit.
    /// The owner enforces exact correlation, original pane and bounded source
    /// capacity; this keeps normal stream-byte accounting and envelope framing.
    /// - Parameter chunk: One nonempty raw bounded chunk, in credited index order.
    /// - Returns: Whether it was queued on the current attached connection.
    [[nodiscard]] bool sendControlUploadChunk(const BulkChunk& chunk);

    /// Takes the `Result` texts received since the last call, inline or
    /// reassembled, in the order they completed.
    [[nodiscard]] std::vector<std::string> takeResults() { return std::exchange(m_results, {}); }

    /// Takes the host's `Notify`s received since the last call, in arrival
    /// order: each of a kind this build knows, its record not yet read.
    [[nodiscard]] std::vector<Notify> takeNotifies() { return std::exchange(m_notifies, {}); }

    /// The client's smoothed round trip, from the `Echo`s of its own probes,
    /// or `nullopt` before the first.
    [[nodiscard]] std::optional<std::uint64_t> smoothedRoundTripMicroseconds() const { return m_smoothedRoundTrip; }

    /// The stream bytes the host sent a bound pane: its frames, and the chunks
    /// of its checkpoints.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The bytes, 0 for a handle that names no pane.
    [[nodiscard]] std::uint64_t paneBytesReceived(std::uint32_t handle) const;

    /// Takes the stream bytes queued since the last call.
    [[nodiscard]] std::vector<std::uint8_t> takeStreamOutput();

    /// Takes the datagrams queued since the last call: the `Echo`s of the
    /// host's datagram probes, and the client's own probes when the carrier
    /// has datagrams. Each is exactly one envelope. An owner whose carrier
    /// refuses one sends it on the stream instead, where the host reads it
    /// the same.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> takeDatagrams();

    /// Where the handshake stands.
    [[nodiscard]] ConnectionState state() const { return m_state; }

    /// Why the connection closed, when it has.
    [[nodiscard]] std::optional<ByeReason> byeReason() const { return m_byeReason; }

    /// The peer's complete closing message, present only after a validated Bye.
    [[nodiscard]] const std::optional<Bye>& bye() const { return m_receivedBye; }
    /// What a host this client refused for its build said about that build:
    /// the Welcome's prefix, read before the rest of it. Empty unless this
    /// client closed with `VersionMismatch` on a Welcome.
    [[nodiscard]] const std::optional<PeerBuild>& refusedHostBuild() const { return m_refusedHostBuild; }

    /// The bounded local rejection detail, independent of any peer's Bye.
    [[nodiscard]] std::string_view localCloseDetail() const { return m_localCloseDetail; }

    /// The sentence a frontend shows for a closed connection, empty while open.
    [[nodiscard]] std::string_view notice() const;

    /// The host's welcome, once received.
    [[nodiscard]] const std::optional<Welcome>& welcome() const { return m_welcome; }

    /// A bound pane's replica terminal, or null.
    [[nodiscard]] const Ubiquitty::Terminal* paneTerminal(std::uint32_t handle) const;

    /// A bound pane's replica terminal, for the frontend's own viewing state;
    /// or null.
    [[nodiscard]] Ubiquitty::Terminal* paneTerminal(std::uint32_t handle);

    /// The facts the host last sent for a bound pane.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The facts, or null for a pane not bound or sent none yet.
    [[nodiscard]] const PaneFacts* paneFacts(std::uint32_t handle) const;

    /// A bound pane's status records, from the one place the host sends them
    /// to this client: the replica of a pane held `Live`, whose screen
    /// carries them, or the `StatusRecords` set last sent whole for a pane in
    /// any other view.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The records, with revision zero when read from a replica;
    ///   `nullopt` for a pane not bound, a `Live` pane with no replica, or a
    ///   pane sent no set yet.
    [[nodiscard]] std::optional<StatusRecords> paneStatus(std::uint32_t handle) const;

    /// The generation a bound pane holds, or 0.
    [[nodiscard]] std::uint64_t paneGeneration(std::uint32_t handle) const;

    /// The class of the newest verified picture, absent before a checkpoint.
    [[nodiscard]] std::optional<FrameClass> paneFrameClass(std::uint32_t handle) const;

    /// The digest of what the client holds of a pane, as a resume names it.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The digest, or `nullopt` for a pane not bound or with no
    ///   checkpoint applied yet.
    [[nodiscard]] std::optional<Digest128> paneDigest(std::uint32_t handle) const;

    /// Whether a bound pane's terminal holds a checkpoint the next update can
    /// build on: one has applied and no resync is outstanding since.
    [[nodiscard]] bool isPaneCurrent(std::uint32_t handle) const;

    /// The panes bound, each with its handle.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, ID128>> boundPanes() const;

    /// The handle a pane is bound to, or 0 when it is not bound.
    [[nodiscard]] std::uint32_t handleForPane(ID128 paneID) const;

    /// The panes the host listed, in the order it listed them.
    [[nodiscard]] std::vector<ID128> listedPanes() const;

    /// The facts the host last sent for a listed pane.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The facts, or null for a pane not listed or sent none yet.
    [[nodiscard]] const PaneFacts* listedPaneFacts(ID128 paneID) const;

    /// The status set the host last sent whole for a listed pane.
    ///
    /// - Parameter paneID: The pane.
    /// - Returns: The set, or null for a pane not listed or sent none yet.
    [[nodiscard]] const StatusRecords* listedPaneStatus(ID128 paneID) const;

    /// A workspace's structure as the host last sent it.
    struct HeldStructure {
        std::uint64_t revision = 0;
        /// The record's bytes, for `Session::decodeWorkspaceStructure`.
        std::vector<std::uint8_t> record;
    };

    /// The structures the host sent and has not withdrawn, by workspace id.
    [[nodiscard]] const std::map<std::uint32_t, HeldStructure>& workspaceStructures() const { return m_structures; }

    /// The echo context the host last sent for a pane.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: It, or `nullopt` before the first or for a pane not bound.
    [[nodiscard]] std::optional<EchoContext> echoContext(std::uint32_t handle) const;

    /// What the connection has done.
    [[nodiscard]] const ClientCounters& counters() const { return m_counters; }

    /// The transfers the client is reassembling.
    [[nodiscard]] std::size_t transfersHeld() const { return m_reassembler.transferCount(); }

    /// Exact pending image pixel allocations, including an owner being
    /// delivered to its terminal. Installed pixels remain in that terminal's
    /// image budget, not this receive-assembly allowance.
    [[nodiscard]] std::size_t inboundImageBytes() const;
    /// Counts exact sample, deferred packet and active callback owners.
    ///
    /// - Returns: Bytes charged with image assemblies to the catalog
    ///   allowance, inside the receive owner.
    [[nodiscard]] std::size_t inboundEffectBytes() const;

    /// The connection's receive owner, which assemblies, installed images and
    /// the catalog families charge (`ClientPolicy::maxReceiveBytes`).
    [[nodiscard]] const MemoryBudget& receiveMemory() const { return *m_receiveMemory; }

    /// Installed image pixels and reserved destinations, summed over every
    /// terminal the connection's panes draw into, each counted once.
    [[nodiscard]] std::size_t installedImageBytes();

    /// What the retained replicas hold (`ReplicaSet::retainedBytes`), or this
    /// connection's own pictures and caches when it keeps no set.
    [[nodiscard]] std::size_t retainedReplicaBytes();

    /// The lag the newest update applied to a pane carried: how far behind
    /// the picture it delivered was when it left (Docs/sync.md §7.4).
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The lag in microseconds, 0 for a pane no update reached.
    [[nodiscard]] std::uint64_t paneLagMicroseconds(std::uint32_t handle) const;

    /// Why a pane failed, or `nullopt` for one that has not: the reason the
    /// last of its refused checkpoints was refused.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The reason.
    [[nodiscard]] std::optional<std::string> paneFailure(std::uint32_t handle) const;

    /// The history a pane holds.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: Its cache, or null for a handle not bound.
    [[nodiscard]] const HistoryCache* historyCache(std::uint32_t handle) const;

    /// Takes a pane's divergence fixture once an update diverged from it.
    ///
    /// - Parameter handle: The pane's handle.
    /// - Returns: The fixture, or `nullopt` for a pane that has not diverged
    ///   or keeps none.
    [[nodiscard]] std::optional<DivergenceFixture> takeDivergenceFixture(std::uint32_t handle);

    /// The bytes the client holds in divergence fixtures, across every pane.
    [[nodiscard]] std::size_t divergenceFixtureBytes() const;

private:
    /// A checkpoint whose body is in transfer.
    struct PendingCheckpoint {
        std::uint64_t generation = 0;
        std::uint32_t transfer = 0;
        Digest128 stateDigest;
    };

    /// One bound pane's replica.
    struct PaneReplica {
        std::uint32_t handle = 0;
        ID128 paneID;
        /// The exact native configuration the host may request measurements from.
        std::optional<RichText::TypographyFacts> publishedTypography;
        /// The replica the connection made, null when the delegate supplied one.
        // Without a retained ReplicaSet, aliased handles share this one owner.
        std::shared_ptr<Ubiquitty::Terminal> ownedTerminal;
        /// The terminal checkpoints and updates apply to; never null.
        Ubiquitty::Terminal* terminal = nullptr;
        std::unique_ptr<ImageReplica> images;
        /// Assets offered on this binding, bounded by its image table.
        std::array<std::optional<ImageAssetIdentity>, Image::maxTerminalImageCount> imageOffers;
        ScreenReplica replica;
        bool hasCheckpoint = false;
        std::optional<PendingCheckpoint> pending;
        /// Ordered batch state belongs to this handle, not its aliases.
        std::unique_ptr<EffectReceiveCursor> effects;
        /// Stable-pane high-water marks, copied to every live alias before
        /// callbacks so any remaining alias preserves presentation deduplication.
        EffectReceiptState effectReceipt;
        /// At most one exact sample identity per slot. A changed identity is
        /// announced only after the previous using batch was acknowledged.
        struct SampleSlot {
            SampleIdentity identity;
            std::uint32_t transferID = 0;
            std::optional<SampleTransfer> incoming;
            std::optional<SamplePCM> pcm;
        };
        std::array<SampleSlot, VT::sampleSlotCount> samples;
        std::deque<std::vector<std::uint8_t>> buffered;
        std::size_t bufferedBytes = 0;
        bool isResyncOutstanding = false;
        bool isAckOwed = false;
        /// The view the host holds for the pane.
        ViewState view = ViewState::Live;
        /// The lag the newest update applied carried.
        std::uint64_t lagMicroseconds = 0;
        /// Only a verified checkpoint or update changes the presentation class.
        FrameClass frameClass = FrameClass::Structural;
        /// The number the next input event to the pane takes.
        std::uint64_t nextInputSeq = 1;
        /// The newest input report of any update or `Receipt`.
        Receipt receipt;
        /// The runs of the client's events the host dropped, not yet taken.
        /// The pane's facts and status records as the host last sent them,
        /// and a status set whose frames are still arriving.
        std::optional<PaneFacts> facts;
        std::optional<StatusRecords> status;
        StatusAssembly statusAssembly;
        /// The status the screen last carried, kept while a status set sent
        /// for a view that is not live stands in `terminal` in its place: the
        /// host's next screen goes on from the status it sent the screen. The
        /// retained set keeps it across connections beside `replica`
        /// (`ReplicaSet::Held::screenStatus`).
        std::optional<OSC::ProgramStatusStore> screenStatus;
        std::vector<InputDiscarded> discards;
        /// The stream bytes the host sent the pane.
        std::uint64_t bytesReceived = 0;
        /// The checkpoints refused in a row since the last one applied.
        std::uint32_t refusedCheckpoints = 0;
        /// When the client asks again for a refused checkpoint.
        std::optional<std::uint64_t> resyncDueAt;
        /// Why the pane failed, once it has.
        std::optional<std::string> failure;
        /// The pane's history, projected into `terminal`'s scrollback.
        std::shared_ptr<HistoryCache> history;
        /// The ranges last wanted at each priority.
        std::array<std::vector<LineRange>, 3> wantedHistory;
        /// What a diagnostic client keeps to replay a divergence.
        std::optional<DivergenceFixture> fixture;
    };

    /// Acts on one frame from the host, closing on one the client does not
    /// accept in the connection's state.
    ///
    /// - Parameters:
    ///   - frame: The frame.
    ///   - carrier: How it arrived.
    ///   - now: The client clock.
    void handleFrame(const Frame& frame, Carrier carrier, std::uint64_t now);

    /// Distinguishes admitted ready data from in-flight or invalid data.
    enum class EffectPayloadReadiness : std::uint8_t {
        Ready,
        Pending,
        Refused,
    };
    /// Owns borrowed sink data through a callback that may retire its pane.
    struct PreparedEffect {
        std::optional<SamplePCM> sample;
        std::uint8_t sampleSlot = 0;
        EffectCodecBytes clipboard;
    };
    /// Owns one validated packet while its exact payload or prefix is pending.
    struct DeferredEffect {
        std::uint32_t handle = 0;
        ID128 paneID;
        std::unique_ptr<std::uint8_t[]> bytes;
        std::size_t size = 0;
        /// Exact clipboard bytes belong to this operation, not a mutable slot.
        EffectCodecBytes clipboard;
        std::uint32_t clipboardTransfer = 0;
        std::uint32_t clipboardNextIndex = 0;
        std::size_t clipboardReceived = 0;
        bool isClipboardComplete = false;
    };
    /// Admits a complete bounded packet without consuming an unready effect.
    ///
    /// - Parameters:
    ///   - handle: The current authenticated binding.
    ///   - body: One complete Effect packet, borrowed only during admission.
    ///   - now: The monotonic clock for completed-batch acknowledgements.
    void receiveEffect(std::uint32_t handle, std::span<const std::uint8_t> body, std::uint64_t now);
    /// Drains ready per-handle prefixes; callbacks never retain a pane reference.
    ///
    /// - Parameter now: The monotonic clock for completed-batch acknowledgements.
    void drainEffects(std::uint64_t now);
    /// Resolves exact payload ownership before receipt mutation, without callbacks.
    ///
    /// - Parameters:
    ///   - replica: The exact live binding to check without retaining it.
    ///   - record: The validated record and immutable primary.
    ///   - body: Receives a borrowed sink pointer only after readiness.
    ///   - prepared: Owns that borrowed sink data through the callback.
    /// - Returns: Ready, waiting for its admitted transfer, or refusal.
    [[nodiscard]] EffectPayloadReadiness prepareEffectPayload(PaneReplica& replica, const EffectView& record, EffectBody& body, PreparedEffect& prepared);
    /// Releases unconsumed payloads and fences pending sample chunks for a binding.
    ///
    /// - Parameter handle: The exact binding being retired, not its aliases.
    void abandonEffects(std::uint32_t handle);
    /// Admits one exact sample identity and its bounded PCM allocation.
    ///
    /// - Parameters:
    ///   - handle: The authenticated pane binding.
    ///   - body: One exact SampleHead record.
    ///   - now: The monotonic clock for effects made ready by an empty sample.
    void receiveSampleHead(std::uint32_t handle, std::span<const std::uint8_t> body, std::uint64_t now);
    /// Consumes a matching sample chunk; completion wakes deferred effects.
    ///
    /// - Parameters:
    ///   - chunk: The bounded decoded chunk, borrowed during this call.
    ///   - now: The monotonic clock for effects made ready by completion.
    /// - Returns: Whether a sample owns the transfer, including refusal.
    [[nodiscard]] bool receiveSampleChunk(const BulkChunk& chunk, std::uint64_t now);

    /// Completes only the exact clipboard operation that admitted a transfer.
    ///
    /// - Parameters:
    ///   - chunk: Raw bounded bytes with ordered indices and a final digest.
    ///   - now: The monotonic clock for completion acknowledgements.
    /// - Returns: Whether a pending clipboard owns this transfer.
    [[nodiscard]] bool receiveClipboardChunk(const BulkChunk& chunk, std::uint64_t now);

    /// Applies an image record, admitting exact pixel owners only after its
    /// pane and current binding identities are checked.
    ///
    /// - Parameters:
    ///   - handle: The destination pane's bound handle.
    ///   - body: The complete encoded image identity and transfer offer.
    void receiveImageHead(std::uint32_t handle, std::span<const std::uint8_t> body);
    /// Replaces one pane's whole placement revision after validating its assets.
    ///
    /// - Parameters:
    ///   - handle: The destination pane's bound handle.
    ///   - body: The complete encoded placement revision.
    void receiveImagePlacements(std::uint32_t handle, std::span<const std::uint8_t> body);
    /// Returns true when the transfer belongs to an image, including refusal.
    [[nodiscard]] bool receiveImageChunk(const BulkChunk&);
    /// Requests offered missing assets under the connection-wide slot bound.
    void requestMissingImages();
    /// How this connection takes image pixels, as its `Greet` named.
    [[nodiscard]] Compression imageCoding() const { return m_policy.capabilities.contains(ClientCapability::DeflateImages) ? Compression::Deflate : Compression::None; }
    /// Retires a pane's retained pictures when a missing asset's destination
    /// does not fit beside them, so a replacement is never kept out by the
    /// picture it replaces.
    /// - Parameter replica: The pane whose record was just applied.
    /// - Returns: Whether any picture was retired.
    bool retireRetainedPicturesForRoom(PaneReplica& replica);
    /// Releases a pane's pending owners and fences chunks already in flight.
    ///
    /// - Parameter handle: The retiring pane's bound handle.
    void abandonImages(std::uint32_t handle);
    /// Destination bytes reserved by this terminal's other image transfers.
    [[nodiscard]] std::size_t pendingImagePixelBytes(const Ubiquitty::Terminal&) const;
    /// Revalidates pending image allocations after metadata or storage changes.
    ///
    /// - Parameter replica: The binding whose destination budget may have changed.
    void refreshImageReservations(PaneReplica& replica);

    /// Applies an update, or buffers it behind a checkpoint in transfer.
    ///
    /// - Parameters:
    ///   - pane: The pane the update is for.
    ///   - body: The whole `Update` body, header included.
    ///   - now: The client clock.
    void handleUpdate(PaneReplica& pane, std::span<const std::uint8_t> body, std::uint64_t now);

    /// Applies an update onto the generation the pane holds, or asks for a
    /// checkpoint when it cannot.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - header: The update's decoded header.
    ///   - body: The whole `Update` body, header included.
    ///   - now: The client clock.
    void applyUpdate(PaneReplica& pane, const DecodedUpdateHeader& header, std::span<const std::uint8_t> body, std::uint64_t now);

    /// Decodes a history body and stores its lines in a pane's cache.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - epoch: The epoch the body was sent under.
    ///   - body: The history body.
    ///   - firstLine: The number of its first line.
    ///   - order: Whether the numbers ascend or descend from it.
    /// - Returns: False when the body is malformed, which closes the connection.
    [[nodiscard]] bool storeHistory(PaneReplica& pane, std::uint32_t epoch, std::span<const std::uint8_t> body, std::uint64_t firstLine, HistoryOrder order);

    /// Confirms external retirement before an epoch or picture changes.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - reset: The new epoch and where it starts.
    /// - Returns: Confirmed retirement, or refusal after closing the connection.
    [[nodiscard]] HistoryPurgeOutcome retireHistory(PaneReplica& pane, const HistoryReset& reset);

    /// Clears the in-memory history, old transfers and replica scrollback
    /// after external retirement has succeeded.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - reset: The new epoch and where it starts.
    void purgeHistory(PaneReplica& pane, const HistoryReset& reset);

    /// Stands a pane's whole status set in its terminal, for a view the host
    /// sends no screen: the set is then the pane's status, as the screen's
    /// records are while it is live. The screen's are kept to put back.
    ///
    /// - Parameter pane: The pane, whose `status` is whole.
    void standStatusSetInTerminal(PaneReplica& pane);

    /// Puts back the status the screen last carried, for a pane live again.
    ///
    /// - Parameter pane: The pane.
    /// - Returns: Whether its terminal's status changed.
    [[nodiscard]] bool restoreScreenStatus(PaneReplica& pane);

    /// Applies a reassembled checkpoint, then the updates buffered behind it.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - body: The checkpoint body.
    ///   - now: The client clock.
    void applyCheckpointBody(PaneReplica& pane, std::span<const std::uint8_t> body, std::uint64_t now);

    /// Counts a refused checkpoint and asks again by the bounded retry policy:
    /// at once the first time, then after a doubling wait, and never past
    /// `maxCheckpointRefusals`, when the pane fails with `reason`.
    ///
    /// - Parameters:
    ///   - pane: The pane whose checkpoint was refused.
    ///   - reason: What the person is told if the pane fails.
    ///   - now: The clock.
    void refuseCheckpoint(PaneReplica& pane, std::string_view reason, std::uint64_t now);

    /// Answers a transfer the receive owner had no room for: a checkpoint is
    /// refused by the retry policy, and any other transfer closes the
    /// connection with `TransferRefused` and the reason.
    ///
    /// - Parameters:
    ///   - transfer: The abandoned transfer.
    ///   - now: The clock.
    void refuseOverBudget(std::uint32_t transfer, std::uint64_t now);

    /// Mirrors the installed images and the catalog families into the receive
    /// owner, so an admission sees every holding.
    void refreshReceiveCharges();

    /// The bytes a catalog owner may still take: the catalog allowance's
    /// remainder, and no more than the receive owner has free.
    ///
    /// - Parameter retained: The catalog families' holdings.
    /// - Returns: The room, zero when either is exhausted.
    [[nodiscard]] std::size_t catalogRoom(std::size_t retained);

    /// Whether a pane's terminal may reserve `needed` more image bytes, in its
    /// own image budget and in the connection-wide installed-image allowance.
    ///
    /// - Parameters:
    ///   - replica: The pane.
    ///   - needed: The destination bytes the image would take.
    ///   - otherReserved: Destination bytes already reserved for the terminal,
    ///     less the transfer being reconsidered.
    [[nodiscard]] bool admitsImageDestination(const PaneReplica& replica, std::size_t needed, std::size_t otherReserved);

    /// Asks for a checkpoint unless one is already asked for.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - reason: Why.
    ///   - generation: The generation `rowHashes` describe.
    ///   - rowHashes: The active screen's row hashes at `generation`.
    void requestResync(PaneReplica& pane, ResyncReason reason, std::uint64_t generation, std::span<const std::uint64_t> rowHashes);

    /// Asks for a checkpoint, describing the generation the pane holds.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - reason: Why.
    void requestResync(PaneReplica& pane, ResyncReason reason);

    /// Records the newest `hostTime` received, which the next `Ack` echoes.
    ///
    /// - Parameters:
    ///   - hostTime: The time an update carried.
    ///   - now: The client clock when it arrived.
    void noteHostTime(std::uint64_t hostTime, std::uint64_t now);

    /// Marks a pane's generation as owed an acknowledgement.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - now: The client clock, from which the `Ack` is held.
    void owesAck(PaneReplica& pane, std::uint64_t now);

    /// Appends a frame to the stream output.
    ///
    /// - Parameters:
    ///   - kind: The frame's kind.
    ///   - channel: Its channel.
    ///   - body: Its body.
    void send(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body);

    /// Sends the published run faces once the connection is attached, and
    /// not again until others are published.
    void sendProportionalFaces();

    /// Sends a `Bye` and closes the connection.
    ///
    /// - Parameters:
    ///   - reason: Why.
    ///   - detail: The sentence the `Bye` carries.
    void close(ByeReason reason, std::string_view detail);

    /// The pane bound to a handle.
    ///
    /// - Parameter handle: The handle.
    /// - Returns: The pane, or null for a handle never bound.
    [[nodiscard]] PaneReplica* pane(std::uint32_t handle);
    /// Returns the stable pane's retained receipt across all bound aliases.
    ///
    /// - Parameter paneID: The authenticated pane identity.
    /// - Returns: Monotonic complete and consumed high-water marks.
    [[nodiscard]] EffectReceiptState effectReceiptForPane(ID128 paneID) const;
    /// Sends a numbered input frame for a pane, numbering it next.
    ///
    /// - Parameters:
    ///   - kind: `Raw`, `Keys` or `Pointer`.
    ///   - handle: The pane's handle.
    ///   - encodeBody: Appends the body for the number it is given.
    /// - Returns: The number, or `nullopt` when nothing was sent.
    [[nodiscard]] std::optional<std::uint64_t> sendInput(FrameKind kind, std::uint32_t handle, const std::function<void(std::vector<std::uint8_t>&, std::uint64_t)>& encodeBody);

    /// Sends one input frame under a given number; the pane's next number
    /// follows the highest sent.
    ///
    /// - Parameters:
    ///   - kind: `Raw`, `Keys` or `Pointer`.
    ///   - handle: The pane's handle.
    ///   - inputSeq: The number.
    ///   - encodeBody: Writes the body under the number.
    /// - Returns: Whether it was sent.
    [[nodiscard]] bool sendInputNumbered(FrameKind kind, std::uint32_t handle, std::uint64_t inputSeq, const std::function<void(std::vector<std::uint8_t>&, std::uint64_t)>& encodeBody);

    /// Folds an input report into a pane's: each number only rises.
    ///
    /// - Parameters:
    ///   - replica: The pane.
    ///   - receipt: What an `Update` or a `Receipt` reported.
    static void noteReceipt(PaneReplica& replica, const Receipt& receipt);

    ClientPolicy m_policy;

    ClientIdentity m_identity;
    std::map<std::uint32_t, std::uint64_t> m_nativeDragEpochs;
    ClientPaneDelegate* m_delegate = nullptr;
    HistoryPurgeSink* m_purgeSink = nullptr;
    EffectReceiptSink* m_effectSink = nullptr;
    ClientWelcomeSink* m_welcomeSink = nullptr;
    ReplicaSet* m_replicas = nullptr;
    ConnectionState m_state = ConnectionState::Greeting;
    std::optional<ByeReason> m_byeReason;
    std::optional<Welcome> m_welcome;
    std::optional<Bye> m_receivedBye;
    std::optional<PeerBuild> m_refusedHostBuild;
    std::string m_localCloseDetail;
    std::optional<ConsentStatusPhase> m_ownConsentPhase;
    std::unique_ptr<const OSC::TextSizing::ProportionalFaceFacts> m_proportionalFaces;
    bool m_areProportionalFacesSent = false;
    FrameIngress m_decoder { maxFrameBodyBytes };
    bool m_isReceiving = false;
    /// The connection's receive owner (`ClientPolicy::maxReceiveBytes`).
    std::shared_ptr<MemoryBudget> m_receiveMemory;
    BulkReassembler m_reassembler;
    /// Installed pixels and reserved destinations summed over the panes, and
    /// the catalog families' holdings, mirrored into the receive owner.
    MemoryCharge m_installedImageCharge;
    MemoryCharge m_catalogCharge;
    /// Scratch for counting each pane's terminal once, at its capacity.
    std::vector<const Ubiquitty::Terminal*> m_distinctTerminals;
    /// An admitted image transfer or a bounded discard fence awaiting its end.
    struct IncomingImage {
        std::uint32_t handle;
        std::uint32_t transfer;
        ImageAssetIdentity asset;
        Ubiquitty::Terminal* terminal;
        std::size_t destinationBytes;
        /// Empty while discarding an announced transfer. The fixed slot still
        /// prevents a premature Want until its final chunk or host abort.
        std::optional<ImageTransfer> pixels;
    };
    /// One fixed image-table-sized set across the entire connection, not one
    /// unbounded transfer map per pane. Discard fences use the same slots.
    /// Pixel allocations share the bounded receive allowance; other bulk
    /// kinds retain their own caps.
    std::array<std::optional<IncomingImage>, Image::maxTerminalImageCount> m_incomingImages;
    /// An offered missing asset whose Want has already been emitted.
    struct WantedImage {
        std::uint32_t handle;
        ImageAssetIdentity asset;
    };
    std::array<std::optional<WantedImage>, Image::maxTerminalImageCount> m_wantedImages;
    std::size_t m_deliveringImageBytes = 0;
    /// Together with image transfers, these owners share the bounded
    /// receive allowance. Checkpoint/history storage has separate accounting.
    std::deque<DeferredEffect> m_deferredEffects;
    std::size_t m_deferredEffectBytes = 0;
    std::size_t m_deliveringEffectBytes = 0;
    std::size_t m_deliveringSampleBytes = 0;
    std::size_t m_deliveringClipboardBytes = 0;
    bool m_isDrainingEffects = false;
    std::unordered_map<std::uint32_t, std::uint32_t> m_transferPanes;
    /// A history transfer a `HistoryHead` announced and the pane it names.
    struct HistoryTransfer {
        std::uint32_t handle = 0;
        HistoryHead head;
    };
    std::unordered_map<std::uint32_t, HistoryTransfer> m_historyTransfers;
    std::vector<PaneReplica> m_panes;
    std::vector<std::uint8_t> m_output;
    std::vector<std::vector<std::uint8_t>> m_datagrams;
    std::uint64_t m_streamBytesReceived = 0;
    std::uint64_t m_streamBytesAcknowledged = 0;
    std::uint64_t m_latestHostTime = 0;
    std::uint64_t m_latestHostTimeReceivedAt = 0;
    std::optional<std::uint64_t> m_ackOwedSince;
    bool m_isAckImmediate = false;
    /// A paste whose pieces are still to send.
    struct OutgoingPaste {
        std::uint32_t handle = 0;
        std::uint64_t inputSeq = 0;
        std::string text;
        std::size_t sent = 0;
        /// How much of it the host reported written (`PasteReceipt`).
        std::uint64_t written = 0;
    };
    /// Pastes in the order they were numbered; the head sends a piece per
    /// `takeStreamOutput`.
    std::deque<OutgoingPaste> m_pastes;
    std::array<std::optional<ClipboardReadState>, maxPendingClipboardReads> m_clipboardReads;
    std::uint32_t m_nextClipboardTransfer = 1;
    /// Emits one bounded answer chunk behind interactive output.
    void appendClipboardReplyChunk();
    /// Admits a pane-bound request before any frontend notification.
    ///
    /// - Parameters:
    ///   - handle: The source pane's authenticated handle.
    ///   - body: The complete encoded clipboard request.
    void receiveClipboardRequest(std::uint32_t handle, std::span<const std::uint8_t> body);
    /// Since when input has waited for a receipt, restarted by contact.
    std::optional<std::uint64_t> m_inputWaitingSince;
    /// Since when the oldest input not yet receipted has waited.
    std::optional<std::uint64_t> m_unreceiptedSince;
    /// When the last frame arrived.
    std::optional<std::uint64_t> m_lastHeardAt;
    /// The host's probes since its last screen data: the step of its idle
    /// schedule it is at, which bounds how long it may stay silent.
    std::size_t m_hostProbesSinceData = 0;
    /// When the probe that input's wait drew left.
    std::optional<std::uint64_t> m_stallProbeSentAt;
    /// What is known of the link, as `judgeLink` reads it.
    [[nodiscard]] LinkFacts linkFacts() const;

    /// The time the client's probe not yet answered carried.
    std::optional<std::uint64_t> m_outstandingProbe;
    /// The time the `checkLiveness` probe not yet answered carried.
    std::optional<std::uint64_t> m_livenessProbe;
    std::optional<std::uint64_t> m_smoothedRoundTrip;
    bool m_isGreetSent = false;
    /// The transfers a `Result` announced, whose text is still arriving.
    std::unordered_set<std::uint32_t> m_resultTransfers;
    /// A structure a `WorkspaceStructure` announced, whose record is still
    /// arriving.
    struct StructureTransfer {
        std::uint32_t workspaceID = 0;
        std::uint64_t revision = 0;
    };
    std::unordered_map<std::uint32_t, StructureTransfer> m_structureTransfers;
    std::map<std::uint32_t, HeldStructure> m_structures;
    /// The bytes `m_structures` holds, bounded by `maxHeldStructureBytes`.
    std::size_t m_structureBytes = 0;
    /// A pane the host listed.
    struct ListedPane {
        std::uint32_t handle = 0;
        ID128 paneID;
        std::optional<PaneFacts> facts;
        std::optional<StatusRecords> status;
        StatusAssembly statusAssembly;
    };
    std::vector<ListedPane> m_listed;

    /// Finds a listed pane by its handle.
    ///
    /// - Parameter handle: The handle.
    /// - Returns: The pane, or null.
    [[nodiscard]] ListedPane* listedPane(std::uint32_t handle);

    /// Takes a structure the host sent, or its withdrawal, keeping the newest
    /// revision of each workspace.
    ///
    /// - Parameters:
    ///   - head: The head that announced it.
    ///   - record: The record's bytes; empty for a withdrawal.
    void takeStructure(const WorkspaceStructureHead& head, std::span<const std::uint8_t> record);
    std::vector<std::string> m_results;
    std::vector<Notify> m_notifies;

    /// Sends a `Call`.
    ///
    /// - Parameters:
    ///   - kind: `Call`.
    ///   - text: The JSON-RPC 2.0 text.
    /// - Returns: Whether it was sent.
    [[nodiscard]] bool sendControl(FrameKind kind, std::string_view text);
    ClientCounters m_counters;

    /// Stops asking for a pane's checkpoint and reports it failed.
    ///
    /// - Parameters:
    ///   - pane: The pane.
    ///   - reason: Why its last checkpoint was refused.
    void failPane(PaneReplica& pane, std::string_view reason);

    /// Tells the host the view the client holds of a pane, whether or not it
    /// is the one the host already has.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - state: The view.
    void sendView(std::uint32_t handle, ViewState state);

    /// Sends a `Probe` carrying the client clock.
    ///
    /// - Parameter now: The client clock.
    void sendProbe(std::uint64_t now);

    /// Ends the input's wait once a receipt covers every event this
    /// connection sent.
    void settleInputWait();

    /// Sends the client's `Facts`, the theme with them when the host reads
    /// it.
    void sendFacts();

    /// Queues a paste under a number its owner chose.
    ///
    /// - Parameters:
    ///   - handle: The pane's handle.
    ///   - inputSeq: The number.
    ///   - text: The text.
    /// - Returns: Whether it was queued.
    [[nodiscard]] bool queuePaste(std::uint32_t handle, std::uint64_t inputSeq, std::string_view text);

    /// Appends the head paste's next piece behind what is queued: at most
    /// `chunkBytes`, cut on a UTF-8 scalar boundary.
    void appendPastePiece();

    /// How long input may wait for a frame before the client probes, and then
    /// declares the path lost.
    [[nodiscard]] std::uint64_t stallBoundMicroseconds() const;

    /// How long an owed `Ack` is held to coalesce the updates behind it: from
    /// the client's round trip and its link and power facts (Docs/sync.md
    /// §6.4).
    [[nodiscard]] std::uint64_t ackIntervalMicroseconds() const;

    /// Sends the `Ack` owed now, whatever its interval says.
    ///
    /// - Parameter now: The client clock.
    void sendAck(std::uint64_t now);
};

} // namespace Ubiquitty::Protocol::Sync
