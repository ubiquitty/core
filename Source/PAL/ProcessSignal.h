// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The POSIX signals a user may send to a process from the UI, as a table rather
// than a switch: the inspector renders it directly, so label and mnemonic are
// presentation data and the number is what reaches kill(2).
//
// Nested under Process, not beside it: a bare PAL::Signal would read as signal
// HANDLING infrastructure, which this is not. These are the signals a user may
// send to a process, so the process is the domain and this is a facet of it.
// Its own header so a consumer wanting only process facts does not compile the
// table.

#include <span>
#include <string_view>

namespace Ubiquitty::PAL::Process::Signal {

struct Entry {
    std::string_view label;
    std::string_view mnemonic;
    int number = 0;
};

[[nodiscard]] std::span<const Entry> table();

/// Looks up the platform's number for a mnemonic such as "TERM" or "KILL".
///
/// - Returns: The signal number, or 0 when no entry has that mnemonic. Signal 0
///   delivers nothing, which is what lets it stand for not found.
[[nodiscard]] int numberForMnemonic(std::string_view);

} // namespace Ubiquitty::PAL::Process::Signal
