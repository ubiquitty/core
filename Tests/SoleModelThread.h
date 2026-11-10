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
