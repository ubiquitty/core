// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Process-wide platform-service hooks live in one immutable PAL::Context,
// installed once at startup, reached through a synchronized-once accessor.
// Immutability after install makes every read race-free by construction; the
// Context must be installed before any worker thread starts (the ordering is the
// race invariant).

#include "PAL/PAL.h"

#include <atomic>
#include <cstdlib>

namespace Ubiquitty::PAL {

namespace {
// The atomic owns synchronization: installContext publishes with release, the
// accessors load with acquire, so a reader (incl. an off-thread decode worker)
// that observes a non-null pointer also observes the fully-built Context behind
// it. Set once at startup, never mutated after (ScopedContext is test-only).
std::atomic<const Context*> activeContext { nullptr };
// Process-lifetime storage for the installed context (installContext copies into
// it). A separate all-null context backs reads before any install so a Core TU
// linked without a frontend degrades instead of dereferencing null.
Context installedContext {};
const Context emptyContext {};
} // namespace

void installContext(const Context& c)
{
    // Fail CLOSED on a missing required hook — a startup misconfiguration must
    // abort here, not surface as a degraded/null read once the terminal is live.
    if (!c.hasRequiredHooks())
        std::abort();
    // Install once: a second install would make this a mutable setter.
    if (activeContext.load(std::memory_order_relaxed) != nullptr)
        std::abort();
    installedContext = c;
    activeContext.store(&installedContext, std::memory_order_release);
}

const Context& context()
{
    const Context* c = activeContext.load(std::memory_order_acquire);
    return c ? *c : emptyContext;
}

ScopedContext::ScopedContext(const Context& c)
    : m_context(c)
{
    if (!m_context.hasRequiredHooks())
        std::abort();
    m_prior = activeContext.exchange(&m_context, std::memory_order_acq_rel);
}

ScopedContext::~ScopedContext() { activeContext.store(m_prior, std::memory_order_release); }

MonotonicClockHook monotonicClock() { return context().monotonicClock; }
ImageDecodeCallback imageDecoder() { return context().imageDecoder; }
PlayTonesCallback playTones() { return context().playTones; }
SoundStatusCallback soundStatus() { return context().soundStatus; }
SelectionColorSetHandler selectionBackgroundSetter() { return context().selectionBackgroundSet; }
SelectionColorResetHandler selectionBackgroundResetter() { return context().selectionBackgroundReset; }
SelectionColorSetHandler selectionForegroundSetter() { return context().selectionForegroundSet; }
SelectionColorResetHandler selectionForegroundResetter() { return context().selectionForegroundReset; }

} // namespace Ubiquitty::PAL
