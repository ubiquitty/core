// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// ProofOfLife.h — how a client finds a path that died without saying so.
//
// A transport that drops tells its owner. A path that goes dead underneath
// one (a laptop that changed networks, a link that swallows bytes) tells
// nobody, and a client that waits for the transport to notice shows a host
// it no longer hears as one it is following. So a client asks: it sends the
// screen plane's probe, which the host is bound to answer, and a connection
// that has not answered within the window below is replaced.
//
// An attached session asks when the app wakes or the network path changes.
// A watch has no such moment, so it asks whenever its connection has been
// quiet for a while.
//

#include "Core/Protocol/Sync/Pacer.h"

namespace Ubiquitty::Session {

/// How long a connection has to answer a probe before it is replaced.
inline constexpr double proofOfLifeDeadlineSeconds = 3.0;

/// How long a watch's connection may be quiet before the watch probes it: a
/// third of the silence a host allows a connection before it drops it
/// (`PacingPolicy::lostSilenceMicroseconds`), so a dead path is found, and the
/// tree marked stale, well inside the time a live host would take to give up
/// on a client.
inline constexpr double watchProbeIntervalSeconds
    = static_cast<double>(Protocol::Sync::PacingPolicy { .windowFloorBytes = 0, .probeIdleMicroseconds = 0, .roundTripStaleMicroseconds = 0, .rateFilterMicroseconds = 0, .datagramLossFloorMicroseconds = 0 }.lostSilenceMicroseconds) / 3'000'000.0;
static_assert(watchProbeIntervalSeconds == 10.0, "a watch probes a quiet connection every ten seconds");

/// How long a directory errand gives the carrier it dialled to bring a host
/// that has welcomed it. A dial into a dead path opens and then carries
/// nothing, with no connection yet to probe and no close to report; past
/// this it is a dial that failed. An ssh carrier spends seconds of it
/// signing in.
inline constexpr double directoryHandshakeSeconds = 15.0;

/// How long a host that has welcomed an errand has to answer what the errand
/// asked: a call its result, a listing its rows. A host answers in the round
/// trip that carried the ask; past this it is not going to.
inline constexpr double directoryAnswerSeconds = 15.0;

} // namespace Ubiquitty::Session
