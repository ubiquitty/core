// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A test that drives a TerminalRuntime on its own thread, with no ingest
// thread and no frontend reading, touches the model as the model's owner
// does: there is no other thread to lock against. Called at the top of such a
// test, this tells Clang's thread-safety analysis so, in place of a lock the
// runtime's contract asks of a threaded caller.

#include "Core/ModelLock.h"

/// Declares that the calling test thread is its model's only thread.
inline void ownModelOnTestThread() UBIQUITTY_ASSERTS_CAPABILITY(Ubiquitty::modelLockRole) { }

// A test that starts an ingest thread owns the model only before the thread
// starts and after it joins. It claims the model through the two helpers
// below, whose claim ends with the call, so no claim spans a window in which
// a drain runs, and the analysis checks every access the test makes there.

/// Readies `runtime` to adopt its stream, on the thread that owns its model
/// until the test starts its ingest thread.
///
/// - Parameter runtime: The runtime about to adopt a stream.
/// - Returns: What `prepareAdoption()` returns.
template<typename Runtime>
[[nodiscard]] bool prepareAdoptionBeforeIngest(Runtime& runtime)
{
    ownModelOnTestThread();
    return runtime.prepareAdoption();
}

/// Runs `read` on `runtime`'s model, on the thread that owns it again once
/// the test has joined its ingest thread.
///
/// - Parameters:
///   - runtime: The runtime whose model is read.
///   - read: Called with the model, or null without one; its result is
///     returned.
/// - Returns: What `read` returns.
template<typename Runtime, typename Read>
decltype(auto) withModelAfterIngest(Runtime& runtime, Read&& read)
{
    ownModelOnTestThread();
    return read(runtime.borrowTermForRender());
}

/// Runs `access` on a session's model under the session's own model lock, for
/// a test that drives the session from its frontend's thread.
///
/// The lock is held for `access` alone, so a test calls no session member
/// that takes the lock from inside it.
///
/// - Parameters:
///   - session: The session whose model is read or written.
///   - access: Called with the model; its result is returned.
/// - Returns: What `access` returns.
template<typename Session, typename Access>
decltype(auto) withSessionModel(Session& session, Access&& access) UBIQUITTY_EXCLUDES(Ubiquitty::modelLockRole)
{
    const Ubiquitty::ModelLockHold hold(*session.modelLockPointer());
    return access(*session.terminal());
}
