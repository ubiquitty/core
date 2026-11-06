// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The one answer to "which machine is this session on", from the two facts that
// can give one.
//
// They are different facts and both are real. ``Transport::Origin`` is how
// Ubiquitty reached the PTY — stamped when the session was created, so a daemon
// session knows its host even though nothing in its process tree says so.
// ``PAL::Process::RemoteTarget`` is where the program inside leads — discovered
// by walking, so a plainly local session that ran `ssh example-host` knows it too, and
// the transport does not.
//
// Resolving them in one place is what keeps the answer single. A window tag
// matching `match_host`, the titlebar's remote badge and an identity check are
// three readers of the same question, and three copies of this precedence would
// be three chances for them to disagree about where the reader is.

#include "Core/Session/HealthBadge.h"
#include "Core/Transport/Origin.h"
#include "PAL/RemoteTarget.h"
#include <string>
#include <string_view>

namespace Ubiquitty::Session {

/// Where a session is, once the transport and the process tree have both been
/// asked.
struct SessionLocation {
    /// What kind of place, or `None` for a session on this machine.
    PAL::Process::RemoteKind kind = PAL::Process::RemoteKind::None;
    /// The destination: a host, a container, a pod, a box, or a device.
    std::string name;
    /// The login that was named, empty when none was.
    std::string user;
    /// How it was reached, for a reader to see beside the name — `ssh`,
    /// `docker exec`, `daemon`.
    std::string via;

    /// Whether this session is somewhere other than this machine.
    ///
    /// - Returns: `true` when a tag or a badge has somewhere to name.
    [[nodiscard]] bool isRemote() const { return kind != PAL::Process::RemoteKind::None; }
};

/// Everything that can say where a session is.
///
/// A struct rather than a parameter list because the sources are not
/// interchangeable and a reader has to see which is which: two of them are
/// strings that mean host, and an argument order is a poor place to keep that
/// straight.
struct LocationInputs {
    // Every member carries an explicit default, which is what lets a caller
    // name only the sources it has. GCC warns on an omitted member that has
    // none — `-Wmissing-field-initializers`, which this tree builds with
    // `-Werror` on Linux and which clang does not raise for a designated
    // initialiser — so a caller naming one of three fields breaks that arm
    // alone while every clang arm stays green.

    /// How Ubiquitty reached this session's PTY.
    Transport::Origin origin {};
    /// Where the foreground process leads, from the walk.
    PAL::Process::RemoteTarget target {};
    /// The host a shell on the far side reported, in OSC 7's authority. Empty
    /// when none arrived, when it named this machine, or once the program that
    /// reported it left the foreground.
    std::string reportedHost {};
};

/// Resolves where a session is, from every source that can say.
///
/// A REPORTED host outranks the walk, because it is the only source that sees
/// past a nested ssh: this machine's process tree holds one ssh however many
/// the reader has opened, so the walk names the first hop and the innermost
/// shell names where they actually are.
///
/// The walk still wins when it names something a report cannot describe. OSC 7
/// says "some host"; only the walk knows the session is in a CONTAINER, on a
/// virtual machine, or on a serial line, and those are nearer and more specific
/// than any hostname a shell inside them would report.
///
/// The transport answers when neither does — every daemon session, whose local
/// process is not an `ssh` and whose far side may report nothing.
///
/// - Parameter inputs: The sources, any of which may be empty.
/// - Returns: The single answer a tag rule, a badge and a title should read.
[[nodiscard]] SessionLocation resolveSessionLocation(const LocationInputs& inputs);

/// The pairing key a frontend dials this machine's own daemon under.
inline constexpr std::string_view thisMachineHostKey = "local";

/// ``thisMachineHostKey``, for a frontend that cannot read a view.
///
/// - Returns: The key.
[[nodiscard]] inline std::string thisMachineHostKeySpelling() { return std::string { thisMachineHostKey }; }

/// Everything a frontend holds about where one pane's session lives.
///
/// The link facts and the location sources travel together because they
/// answer one question. A chip that read the link while a tag rule read the
/// location named one machine in the title row and matched another.
struct PaneHostInputs {
    /// Whether the pane is a thin client of a ubiquitty-host session, attached
    /// or still being re-attached.
    bool isHostSession = false;
    /// Whether the session is a local shell this window shared into this
    /// machine's daemon.
    bool isShared = false;
    /// Whether the host session's bytes ride QUIC rather than ssh-stdio.
    bool usesQUIC = false;
    /// The key the host was dialled under: `[user@]host`, or
    /// ``thisMachineHostKey`` for this machine's daemon. Empty when nothing
    /// was dialled.
    std::string hostKey {};
    /// The name the dialled host was saved under, for a reader to see. Empty
    /// when it has none of its own. A session that is not a host session wears
    /// it only while it is on the machine the key names.
    std::string hostLabel {};
    /// This machine's own name. Empty asks the platform.
    std::string thisMachineName {};
    /// The transport stamp, the walk and the far side's report, read for a
    /// pane that is not a host session.
    LocationInputs location {};
};

/// Where one pane's session lives, as every surface that names it reads it.
///
/// The host chip draws `place`, `label` and `carrier`; a tab's host mark and a
/// pane bar draw `place` and `label`; a `[[tags]]` rule's `match_host` reads
/// `machine` and `label`. All of them come out of one ``resolvePaneHost``, so
/// a window cannot name a host its tags never heard of.
///
/// Every member carries an explicit default, as ``LocationInputs``'s do, so a
/// caller may name only the facts it has: GCC's `-Wmissing-field-initializers`
/// refuses a designated initializer that leaves out a member without one.
struct PaneHost {
    /// Which place, or `None` for a plain local shell.
    HostPlace place = HostPlace::None;
    /// Whether the place is a ubiquitty-host link whose health a chip reports.
    /// A shell that reached its host by running `ssh` has no link of ours to
    /// report on: it is steady for as long as it is there.
    bool isHostSession = false;
    /// The machine the session runs on, without a login: this machine's own
    /// name for its daemon, the destination for anything else.
    std::string machine {};
    /// What the chrome calls a remote place: the host's saved label, else the
    /// machine. Empty for this machine, which a frontend names in its own words.
    std::string label {};
    /// How the bytes get there, for a chip that names its carrier: `quic` or
    /// `ssh` for a host link, the tool the walk saw (`ssh`, `mosh`,
    /// `docker exec`) for anything else. Empty for this machine.
    std::string carrier {};

    /// Whether the session is anywhere a chip, a tab or a tag can name.
    [[nodiscard]] bool isNamed() const { return place != HostPlace::None; }
};

/// Resolves where one pane's session lives.
///
/// A host session is placed by its link: this machine's daemon (the key
/// ``thisMachineHostKey``, or a shell shared into it) is this machine under
/// its own name, any other daemon is the machine its key names. Anything else
/// is placed by ``resolveSessionLocation`` over the transport stamp, the walk
/// and the far side's report, so a plain `ssh`, a `tmux -CC` reached over ssh
/// and a shell stepped into a container are each remote, named and carried.
///
/// - Parameter inputs: What the frontend knows about the pane.
/// - Returns: The single answer the chip, the tab and the tag rules read.
[[nodiscard]] PaneHost resolvePaneHost(const PaneHostInputs& inputs);

} // namespace Ubiquitty::Session
