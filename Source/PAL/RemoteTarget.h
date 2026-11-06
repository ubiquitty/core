// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Where a session actually is, when its foreground process leads somewhere else.
//
// A reader running `ssh example-host` is on example-host, and a window tag, a title and an
// identity check all want to say so. The local process is called `ssh`, which
// answers none of them, and the host is sitting in argv where nothing looks.
//
// The host is a fact about the session, not a rendering of its arguments, so
// it is reported regardless of whether the title shows the command line.
//
// Typed rather than a string, because `web-1` is a different claim depending on
// whether it names a machine or a container, and a rule that wants either
// should be able to say which.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Ubiquitty::PAL::Process {

/// What kind of place a command leads to.
enum class RemoteKind : std::uint8_t {
    /// Nowhere — the command runs here.
    None,
    /// A machine reached over the network: ssh, mosh, telnet.
    Host,
    /// A namespace on some host: docker, podman, kubectl, lxc.
    Container,
    /// A machine managed locally: vagrant, lima, multipass, virsh.
    VirtualMachine,
    /// Hardware on the other end of a line: screen, picocom, minicom.
    Serial,
};

/// Where a command line leads, and what it is that goes there.
struct RemoteTarget {
    /// What kind of place, or `None` when the command stays here.
    RemoteKind kind = RemoteKind::None;
    /// The destination as the invocation spelled it: a host, a container, a
    /// pod, or a device path. Never carries the user or a port.
    std::string name;
    /// The login the invocation named, empty when it named none.
    std::string user;
    /// The tool that goes there, spelled as the reader typed it —
    /// `ssh`, `tailscale ssh`, `docker exec`. What a title puts before the name.
    std::string via;

    /// Whether this names somewhere other than here.
    ///
    /// - Returns: `true` when the command leads off this machine or into a
    ///   container, `false` for an ordinary local command.
    [[nodiscard]] bool isRemote() const { return kind != RemoteKind::None; }
};

/// Reads a command line and says where it leads.
///
/// The argument is argv joined by spaces, which is what a process walk carries
/// (``PAL::Process::Record::commandLine``). Rejoining is lossy for an argument
/// that contains a space, and the loss is survivable here: a destination never
/// contains one, and a split option value shifts which token is the operand
/// only for invocations that already cannot be told apart from the outside.
///
/// Unknown commands, and known ones with no destination yet (`ssh` alone,
/// `ssh -V`), answer `RemoteKind::None`.
///
/// - Parameter commandLine: argv joined by spaces.
/// - Returns: Where it leads, or a target whose `kind` is `None`.
[[nodiscard]] RemoteTarget remoteTargetForCommand(std::string_view commandLine);

/// Reads already-split arguments and says where they lead.
///
/// The form to prefer wherever real argv survives, since it cannot be confused
/// by an argument containing a space.
///
/// - Parameter arguments: argv, `arguments[0]` being the program.
/// - Returns: Where it leads, or a target whose `kind` is `None`.
[[nodiscard]] RemoteTarget remoteTargetForArguments(std::span<const std::string_view> arguments);

} // namespace Ubiquitty::PAL::Process
