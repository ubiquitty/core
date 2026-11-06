// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/SessionLocation.cpp — see SessionLocation.h.

#include "Core/Session/SessionLocation.h"

#include "PAL/PAL.h"

namespace Ubiquitty::Session {

namespace {

// Splits `[user@]host`, the spelling both a transport origin and a reported
// host arrive in. The machine is what a badge shows and what `match_host` asks
// about: two sessions on one host under two logins are one machine.
void splitLogin(const std::string& spelling, std::string& name, std::string& user)
{
    const std::size_t at = spelling.find_last_of('@');
    if (at == std::string::npos) {
        name = spelling;
        return;
    }
    user = spelling.substr(0, at);
    name = spelling.substr(at + 1);
}

} // namespace

SessionLocation resolveSessionLocation(const LocationInputs& inputs)
{
    const PAL::Process::RemoteTarget& target = inputs.target;

    // A kind the walk alone can name outranks everything. A shell inside a
    // container reports a hostname like any other, and taking it would turn a
    // container the reader can name into a machine they cannot.
    const bool isNearerThanAHost = target.isRemote()
        && target.kind != PAL::Process::RemoteKind::Host && !target.name.empty();
    if (isNearerThanAHost)
        return SessionLocation { target.kind, target.name, target.user, target.via };

    if (!inputs.reportedHost.empty()) {
        SessionLocation location;
        location.kind = PAL::Process::RemoteKind::Host;
        splitLogin(inputs.reportedHost, location.name, location.user);
        // The tool that got there is still the walk's to name: a report says
        // where a shell is, never how the reader arrived.
        location.via = target.isRemote() ? target.via : std::string { "ssh" };
        return location;
    }

    if (target.isRemote() && !target.name.empty())
        return SessionLocation { target.kind, target.name, target.user, target.via };

    switch (inputs.origin.transport) {
    case Transport::OriginKind::Local:
        break;
    case Transport::OriginKind::SSH:
    case Transport::OriginKind::Daemon: {
        if (inputs.origin.host.empty())
            break;
        SessionLocation location;
        location.kind = PAL::Process::RemoteKind::Host;
        splitLogin(inputs.origin.host, location.name, location.user);
        location.via = inputs.origin.transport == Transport::OriginKind::SSH ? "ssh" : "daemon";
        return location;
    }
    }
    return {};
}

PaneHost resolvePaneHost(const PaneHostInputs& inputs)
{
    PaneHost host;
    if (inputs.isHostSession) {
        host.isHostSession = true;
        const bool isOnThisMachine = inputs.isShared || inputs.hostKey == thisMachineHostKey;
        host.place = hostPlaceFor({ .isHostSession = true, .isOnThisMachine = isOnThisMachine });
        if (isOnThisMachine) {
            host.machine = inputs.thisMachineName.empty() ? PAL::systemHostName() : inputs.thisMachineName;
            return host;
        }
        // The key is what was dialled, `[user@]host`; the machine is its host
        // half, as it is for an origin stamped in the same spelling.
        std::string user;
        splitLogin(inputs.hostKey.empty() ? inputs.hostLabel : inputs.hostKey, host.machine, user);
        host.label = inputs.hostLabel.empty() ? host.machine : inputs.hostLabel;
        host.carrier = inputs.usesQUIC ? "quic" : "ssh";
        return host;
    }
    const SessionLocation location = resolveSessionLocation(inputs.location);
    if (!location.isRemote())
        return host;
    host.place = HostPlace::Remote;
    host.machine = location.name;
    host.carrier = location.via;
    // The label the host was saved under names it while the session is still
    // on the machine that was dialled; once the reader has gone on from there,
    // the label would name the wrong machine.
    std::string dialled;
    std::string user;
    splitLogin(inputs.hostKey, dialled, user);
    const bool isOnTheDialledMachine = !dialled.empty() && dialled == location.name;
    host.label = isOnTheDialledMachine && !inputs.hostLabel.empty() ? inputs.hostLabel : location.name;
    return host;
}

} // namespace Ubiquitty::Session
