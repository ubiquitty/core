// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"
#include "Base/FallibleString.h"
#include "Base/FallibleVector.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::OSC {

constexpr std::size_t contextDepthLimit = 64;
constexpr std::size_t programStatusLimit = 256;
/// The longest title a program's status report may carry, in UTF-8 bytes.
/// A report whose decoded title is longer is refused whole.
constexpr std::size_t maxProgramStatusTitleBytes = 192;
/// The longest message a program's status report may carry, in UTF-8 bytes.
/// A report whose decoded message is longer is refused whole.
constexpr std::size_t maxProgramStatusMessageBytes = 2048;

/// Names the metadata fields defined by OSC 3008.
enum class ContextField : std::uint8_t {
    Type,
    User,
    Hostname,
    MachineID,
    BootID,
    PID,
    PIDFDID,
    Command,
    WorkingDirectory,
    CommandLine,
    VM,
    Container,
    TargetUser,
    TargetHost,
    SessionID,
    Exit,
    Status,
    Signal,
    Count
};

/// Distinguishes active context metadata from a completed context's exit fields.
enum class ContextPhase {
    Active,
    Ended
};

/// Holds one decoded context and its optional metadata.
struct Context {
    FallibleString id;
    std::array<FallibleString, static_cast<std::size_t>(ContextField::Count)> fields;

    /// Returns the decoded value of a context metadata field.
    [[nodiscard]] const FallibleString& value(ContextField field) const { return fields[static_cast<std::size_t>(field)]; }
    bool operator==(const Context&) const = default;
};

/// Borrows a context's decoded fields while validating wire state.
struct ContextView {
    std::string_view id;
    std::array<std::string_view, static_cast<std::size_t>(ContextField::Count)> fields {};
};

/// Stores active contexts in ancestor order and the most recently ended context.
struct ContextState {
    FallibleVector<Context> active;
    std::optional<Context> lastEnded;
    bool operator==(const ContextState&) const = default;
};

/// Describes the state reported by a program; None denotes no record.
enum class UBIQUITTY_CLOSED_ENUM ProgramState : std::uint8_t {
    None,
    Idle,
    Working,
    Done,
    Blocked,
    Error
};
/// Describes the input a blocked program requires.
enum class UBIQUITTY_CLOSED_ENUM ProgramBlockKind : std::uint8_t {
    None,
    Permission,
    Question,
    Auth
};

/// Holds the complete last report for one program status identifier.
struct ProgramStatus {
    FallibleString id;
    FallibleString app;
    FallibleString title;
    FallibleString message;
    ProgramState state = ProgramState::None;
    ProgramBlockKind kind = ProgramBlockKind::None;
    int progress = -1;
    bool operator==(const ProgramStatus&) const = default;
};

/// Borrows one complete status record without allocating its text.
struct ProgramStatusView {
    std::string_view id;
    std::string_view app;
    std::string_view title;
    std::string_view message;
    ProgramState state = ProgramState::None;
    ProgramBlockKind kind = ProgramBlockKind::None;
    int progress = -1;
};

/// Stores reports from least to most recently updated for bounded eviction.
struct ProgramStatusStore {
    FallibleVector<ProgramStatus> records;

    /// Finds the nearest explicitly named application, including the record itself.
    [[nodiscard]] std::string_view effectiveApp(const ProgramStatus&) const;
    /// Selects the most urgent record, breaking ties by most recent update.
    [[nodiscard]] const ProgramStatus* primary() const;
    bool operator==(const ProgramStatusStore&) const = default;
};

/// Applies a context command without changing state when its identifier is invalid.
[[nodiscard]] bool applyContext(ContextState&, std::string_view payload);
/// Applies a complete status report atomically after validating every pair.
[[nodiscard]] bool applyProgramStatus(ProgramStatusStore&, std::string_view payload);
/// Validates a decoded context received in a Sync snapshot.
[[nodiscard]] bool validContext(const Context&, ContextPhase phase);
/// Applies the same context invariants to borrowed decoded fields.
[[nodiscard]] bool validContext(const ContextView&, ContextPhase phase);
/// Validates a decoded program status received in a Sync snapshot.
[[nodiscard]] bool validProgramStatus(const ProgramStatus&);
/// Applies the same status invariants without allocating an owned record.
[[nodiscard]] bool validProgramStatus(const ProgramStatusView&);
/// Removes working and blocked records when their process ends or a prompt begins.
void finishProgramActivity(Terminal&);
/// Ends working and blocked records because the shell has the terminal back,
/// for a shell that marks no prompt.
///
/// A shell with the integration marks each prompt (OSC 133;A), and that is
/// where its records end: its own functions report a state and then run
/// programs, and a program handing the terminal back is not the function's
/// end. A shell that has marked none gives no other sign that a program
/// ended, and without this the record of an interrupted program stands for as
/// long as the pane does.
///
/// - Parameter terminal: The terminal whose foreground returned to its shell.
void foregroundReturnedToShell(Terminal& terminal);
/// Publishes a metadata change to snapshot and status consumers.
void activityChanged(Terminal&);
/// Consumes an OSC 3008 context command.
///
/// - Parameters:
///   - terminal: The terminal receiving the command.
///   - payload: The bytes following the OSC selector and separator.
void handleContext(Terminal& terminal, std::string_view payload);
/// Consumes an OSC 7501 report or fixed feature query.
///
/// - Parameters:
///   - terminal: The terminal receiving the report.
///   - payload: The bytes following the OSC selector and separator.
void handleProgramStatus(Terminal& terminal, std::string_view payload);

} // namespace Ubiquitty::OSC
