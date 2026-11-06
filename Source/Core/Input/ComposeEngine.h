// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Terminal-decoded compose-key sequences: press the compose key, then a short run
// of character keys resolving to one composed character (Compose ' e → é). The
// X11/GTK "Compose" model in the input layer, independent of the platform IME.
//
// A small state machine fed one codepoint per key press (pure, no allocation).
// Usage: on the compose key, begin(); for each subsequent key, feed(codePoint) —
// swallow while Pending, emit result() on Composed, on Invalid the frontend
// decides and the engine resets. The built-in table covers common Latin /
// punctuation / symbol sequences from X11's en_US.UTF-8 Compose (not the full file).

#include "Base/SwiftInterop.h"
#include "Base/Types.h"
#include "Core/Input/Keybindings.h"

#include <string>

namespace Ubiquitty::Input {

enum class ComposeResult : uint8_t {
    Inactive, // not composing — caller handles the key normally
    Pending, // sequence in progress (prefix of a known entry) — swallow the key
    Composed, // sequence complete — emit the resolved codepoint (see result())
    Invalid, // no entry starts with what was typed — engine reset, caller decides
};

class ComposeEngine {
public:
    // Maximum codepoints in a compose sequence's key-run (after the compose key).
    static constexpr int maxSequenceLength = 4;

    ComposeEngine() = default;

    // Enter compose mode: the compose key was pressed. The next feed() begins a
    // fresh sequence.
    void begin();

    // True while a sequence is being captured (between begin() and a
    // terminating Composed / Invalid).
    [[nodiscard]] bool active() const { return m_active; }

    // Feed the next key's codepoint.
    //   - Inactive when not composing (begin() was never called / sequence ended).
    //   - Pending while the captured prefix matches the start of some entry.
    //   - Composed when the captured run exactly matches an entry; result() then
    //     holds the resolved codepoint and the engine resets.
    //   - Invalid when the run can't extend any entry; the engine resets.
    ComposeResult feed(uint32_t codePoint);

    // Resolved codepoint after a Composed result. Undefined otherwise.
    [[nodiscard]] uint32_t result() const { return m_result; }

    // Abort an in-progress sequence (e.g. Escape, focus loss).
    void reset();

private:
    bool m_active = false;
    int m_length = 0;
    uint32_t m_result = 0;
    uint32_t m_buffer[maxSequenceLength] = { 0, 0, 0, 0 };
};

/// What a frontend does with a key offered to the compose path.
enum class ComposeAction : uint8_t {
    /// Not composing: the key belongs to whatever would have had it.
    Pass UBIQUITTY_SWIFT_NAME(pass),
    /// The key is spent — it armed a sequence, extended one, or ended one that
    /// resolved to nothing.
    Swallow UBIQUITTY_SWIFT_NAME(swallow),
    /// The sequence resolved. Write ``ComposeState::composed`` to the host.
    Write UBIQUITTY_SWIFT_NAME(write),
};

/// The `[keys] compose_key` trigger and the engine behind it.
///
/// One object because the two are rebuilt together and are meaningless apart:
/// a trigger with no engine arms nothing, and an engine with no trigger can
/// never start. Every frontend that offers compose holds one of these and asks
/// it, rather than spelling the ladder — press the trigger, feed the printable
/// keys, act on the three outcomes — for itself. It was spelled three times
/// here, and two of the spellings disagreed about what a non-printable key
/// mid-sequence does.
class ComposeState {
public:
    /// Rebuilds the trigger from a `[keys] compose_key` spec.
    ///
    /// Compose is ONE key: a spec naming a chord sequence is rejected rather
    /// than half-honoured, because a compose "key" you have to type twice is
    /// not the model this implements. Any sequence in progress is dropped —
    /// the reader just changed what starts one.
    ///
    /// - Parameter spec: The configured spec, empty to disarm compose.
    void rebuild(const std::string& spec)
    {
        m_isArmed = false;
        m_engine.reset();
        if (spec.empty())
            return;
        std::vector<KeyChord> sequence;
        if (parseKeySpec(spec, sequence) && sequence.size() == 1) {
            m_trigger = sequence.front();
            m_isArmed = true;
        }
    }

    /// Whether a compose key is configured.
    [[nodiscard]] bool isArmed() const { return m_isArmed; }

    /// Offers a key press to the compose path.
    ///
    /// A non-printable key mid-sequence aborts the capture and takes its
    /// ordinary path: the alternative — staying armed while the reader moves
    /// the cursor — makes the next quote or letter they type turn into
    /// something they did not ask for, a long way from the key that armed it.
    ///
    /// - Parameters:
    ///   - chord: The key as a binding chord, for the trigger comparison.
    ///   - typedCodePoint: The character the key produced, zero when it
    ///     produced none.
    /// - Returns: What the frontend should do with the key.
    ComposeAction offer(const KeyChord& chord, uint32_t typedCodePoint)
    {
        if (!m_isArmed)
            return ComposeAction::Pass;
        if (chord == m_trigger) {
            m_engine.begin();
            return ComposeAction::Swallow;
        }
        if (!m_engine.active())
            return ComposeAction::Pass;
        if (typedCodePoint == 0) {
            m_engine.reset();
            return ComposeAction::Pass;
        }
        switch (m_engine.feed(typedCodePoint)) {
        case ComposeResult::Composed:
            return ComposeAction::Write;
        case ComposeResult::Pending:
        case ComposeResult::Invalid:
            return ComposeAction::Swallow;
        case ComposeResult::Inactive:
            break;
        }
        return ComposeAction::Pass;
    }

    /// The resolved code point after ``ComposeAction::Write``.
    [[nodiscard]] uint32_t composed() const { return m_engine.result(); }

    /// Drops a sequence in progress, the trigger left armed.
    void reset() { m_engine.reset(); }

private:
    ComposeEngine m_engine;
    KeyChord m_trigger {};
    bool m_isArmed = false;
};

} // namespace Ubiquitty::Input
