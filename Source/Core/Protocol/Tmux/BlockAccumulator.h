// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The marker-then-content state machine shared by every tmux command-response
// recovery: screen capture and the copy-mode clipboard pull.
//
// A recovery issues a display-message marker followed by the real command. The
// marker's reply names the target; the NEXT command-response block carries the
// content. Both are delimited by the protocol's %begin/%end boundaries, never
// by body text, so content that looks like a marker cannot corrupt or end a
// block early.
//
// Idle -> MarkerSeen (marker line) -> Capturing (marker's block end) -> Idle
// (content block end). Past `maxBytes` lines are still swallowed so the block
// end routes cleanly; dropping state mid-block would misparse the rest of the
// response as pane records.
//
// Single-threaded, holds no locks, dispatches nothing. Marker payload parsing
// and the decision made at block end belong to the owner.

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace Ubiquitty::Protocol::Tmux {

class BlockAccumulator {
public:
    // What a command-response body line turned out to be.
    enum class LineKind {
        NotOurs, // belongs to normal command-response parsing
        Marker, // the recovery's marker; payload is the text after the prefix
        Body, // content, accumulated (or swallowed past the limit)
    };

    // How body lines join. Screen rows are newline-TERMINATED, so a capture's
    // last row ends the line like every other. A clipboard buffer is
    // newline-SEPARATED: a trailing one would append a newline the user never
    // copied.
    enum class LineJoin {
        TerminateEachLine,
        SeparateLines,
    };

    BlockAccumulator(std::string_view markerPrefix, size_t maxBytes, LineJoin join)
        : m_markerPrefix(markerPrefix)
        , m_maxBytes(maxBytes)
        , m_join(join)
    {
    }

    // Abandon any in-flight block — a fresh attach, %exit — so half-accumulated
    // state cannot swallow the next session's rows. Safe to call repeatedly.
    void reset()
    {
        m_state = State::Idle;
        m_buffer.clear();
    }

    // A marker has been seen and its content block has not yet closed.
    [[nodiscard]] bool active() const { return m_state != State::Idle; }

    // True while body lines are being accumulated.
    [[nodiscard]] bool capturing() const { return m_state == State::Capturing; }

    /// Routes one body line and reports what it was.
    ///
    /// Idle recognises nothing but the marker. MarkerSeen is transient, between
    /// the marker line and its block end, and sees no lines at all — a line
    /// arriving in that state is not ours rather than a body line, because the
    /// block it would belong to has not opened.
    ///
    /// - Parameters:
    ///   - line: One body line, without its terminator.
    ///   - markerPayload: On `Marker`, receives the text after the prefix. It
    ///     views `line`, so it outlives neither the argument nor the next call.
    ///     Untouched for any other result.
    /// - Returns: Which of the three kinds the line was.
    [[nodiscard]] LineKind consumeLine(std::string_view line, std::string_view& markerPayload)
    {
        if (m_state == State::Idle) {
            if (!line.starts_with(m_markerPrefix))
                return LineKind::NotOurs;
            markerPayload = line.substr(m_markerPrefix.size());
            m_buffer.clear();
            m_state = State::MarkerSeen;
            return LineKind::Marker;
        }
        if (m_state == State::Capturing) {
            const bool needsSeparator = m_join == LineJoin::SeparateLines && !m_buffer.empty();
            const bool needsTerminator = m_join == LineJoin::TerminateEachLine;
            // Exactly the bytes the append writes: the first line under
            // SeparateLines takes no separator.
            const size_t added = line.size() + ((needsSeparator || needsTerminator) ? 1 : 0);
            if (m_buffer.size() + added <= m_maxBytes) {
                if (needsSeparator)
                    m_buffer.push_back('\n');
                m_buffer.append(line);
                if (needsTerminator)
                    m_buffer.push_back('\n');
            }
            return LineKind::Body;
        }
        return LineKind::NotOurs;
    }

    // A command-response block ended (%end or %error). The marker's block end
    // arms Capturing; the content block's end yields the accumulated bytes in
    // `content` and returns to Idle. Returns false outside a content block, in
    // which case `content` is untouched.
    [[nodiscard]] bool blockEnded(std::string& content)
    {
        if (m_state == State::MarkerSeen) {
            m_state = State::Capturing;
            return false;
        }
        if (m_state != State::Capturing)
            return false;
        content = std::move(m_buffer);
        reset();
        return true;
    }

private:
    enum class State {
        Idle,
        MarkerSeen,
        Capturing,
    };

    std::string_view m_markerPrefix;
    size_t m_maxBytes;
    LineJoin m_join;
    State m_state = State::Idle;
    std::string m_buffer;
};

} // namespace Ubiquitty::Protocol::Tmux
