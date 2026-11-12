// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// One pane a hosting session's source owns, the only kind of model a view
// adopts: a primary terminal running tmux in control mode lists one window
// with one pane, and its TmuxControlHost owns that pane's model, lock and
// publication. Output reaches the pane as tmux sends it, on a %output line
// the primary parses, and the host advances the pane's publication on its
// own clock, which a case sets.

#include "Core/ModelLock.h"
#include "Core/Runtime/HostedWindows.h"
#include "Core/Runtime/TmuxControlHost.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Platform/StreamDouble.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace UbiquittyTest {

/// One window of panes a TmuxControlHost owns, which a view adopts.
class HostedPaneFixture {
public:
    /// The pane the host lists first, and alone without a layout.
    static constexpr int paneID = 1;

    /// Lists one window of `rows` by `columns`, holding one pane, `%1`, or
    /// the panes `layout` lays out.
    ///
    /// - Parameters:
    ///   - rows: The window's rows.
    ///   - columns: The window's columns.
    ///   - layout: A tmux layout naming its panes by number, `%1` among them;
    ///     empty for one pane filling the window.
    ///   - filesystemLocation: The carrier owner's filesystem provenance.
    explicit HostedPaneFixture(int rows = 24, int columns = 80, std::string_view layout = {}, Ubiquitty::Runtime::FilesystemLocation filesystemLocation = Ubiquitty::Runtime::FilesystemLocation::Unknown)
        : m_primary(std::make_unique<Ubiquitty::Terminal>())
    {
        Ubiquitty::resize(*m_primary, 24, 80);
        Ubiquitty::Runtime::HostedWindowSeams seams;
        seams.primary = m_primary.get();
        seams.primaryLock = &m_primaryLock;
        seams.stream = &m_stream;
        seams.filesystemLocation = filesystemLocation;
        seams.sessionContext = this;
        seams.clockMicroseconds = [](void* context) { return static_cast<HostedPaneFixture*>(context)->m_nowMicroseconds; };
        seams.clockTicks = [](void* context) { return Ubiquitty::Ticks(static_cast<HostedPaneFixture*>(context)->m_nowTicks); };
        m_host = std::make_unique<Ubiquitty::Runtime::TmuxControlHost>(seams);
        m_host->attach();
        sendToPrimary("%begin 1 1 0\n%end 1 1 0\n");
        const std::string size = std::to_string(columns) + "x" + std::to_string(rows);
        const std::string windowLayout = layout.empty() ? "ef01," + size + ",0,0,1" : std::string(layout);
        sendToPrimary("%begin 2 2 1\n@1 %1 " + std::to_string(columns) + " " + std::to_string(rows) + " 1 " + windowLayout + " work editor\n%end 2 2 1\n");
    }

    /// Takes the host back out of the primary terminal.
    ~HostedPaneFixture() { m_host->detach(); }

    HostedPaneFixture(const HostedPaneFixture&) = delete;
    HostedPaneFixture& operator=(const HostedPaneFixture&) = delete;

    /// Returns the address a view adopts the pane through.
    ///
    /// - Returns: The host, as ``TerminalSession::hostedWindowSourceAddress``
    ///   names a hosting session's source.
    [[nodiscard]] std::uintptr_t sourceAddress() const { return reinterpret_cast<std::uintptr_t>(m_host.get()); }

    /// Returns the host itself.
    ///
    /// - Returns: The pane's source.
    [[nodiscard]] Ubiquitty::Runtime::TmuxControlHost& host() const { return *m_host; }

    /// Returns a pane's model, which the host owns.
    ///
    /// - Parameter pane: The pane's number.
    /// - Returns: The model, or null when no such pane is live.
    [[nodiscard]] Ubiquitty::Terminal* paneTerminal(int pane = paneID) const { return reinterpret_cast<Ubiquitty::Terminal*>(m_host->paneFacts(pane).terminalAddress); }

    /// Returns a pane model's lock.
    ///
    /// - Parameter pane: The pane's number.
    /// - Returns: The lock, or null when no such pane is live.
    [[nodiscard]] Ubiquitty::ModelLock* paneLock(int pane = paneID) const { return reinterpret_cast<Ubiquitty::ModelLock*>(m_host->paneFacts(pane).lockAddress); }

    /// Sends `bytes` as a pane's program wrote them, on a %output line the
    /// primary parses; the host advances the pane's publication with them.
    ///
    /// - Parameters:
    ///   - bytes: The program's bytes, escaped as tmux escapes them: `\033`
    ///     for ESC.
    ///   - pane: The pane's number.
    void sendToPane(std::string_view bytes, int pane = paneID) { sendToPrimary("%output %" + std::to_string(pane) + " " + std::string(bytes) + "\n"); }

    /// Sends one line of tmux's control stream as the server wrote it, such
    /// as a `%layout-change`.
    ///
    /// - Parameter line: The line, without its newline.
    void sendControlLine(std::string_view line) { sendToPrimary(std::string(line) + "\n"); }

    /// Moves the host's clocks.
    ///
    /// - Parameters:
    ///   - ticks: Now, on the 60 Hz hold-budget clock.
    ///   - microseconds: Now, on the publication clock.
    void setClock(std::uint32_t ticks, std::uint64_t microseconds)
    {
        m_nowTicks = ticks;
        m_nowMicroseconds = microseconds;
    }

private:
    /// Parses a line tmux sent, as the primary's ingest does, under its lock.
    ///
    /// - Parameter line: The line, newline included.
    void sendToPrimary(const std::string& line)
    {
        const std::lock_guard hold(m_primaryLock);
        Ubiquitty::processBytes(m_primary.get(), line);
    }

    std::unique_ptr<Ubiquitty::Terminal> m_primary;
    Ubiquitty::ModelLock m_primaryLock;
    Ubiquitty::TestDoubles::StreamDouble m_stream;
    std::unique_ptr<Ubiquitty::Runtime::TmuxControlHost> m_host;
    std::uint32_t m_nowTicks = 1;
    std::uint64_t m_nowMicroseconds = 1'000'000;
};

} // namespace UbiquittyTest
