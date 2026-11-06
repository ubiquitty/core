// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Input.h"

#include "Core/Keys/KeyTranslation.h"
#include "Core/Keys/Keymap.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

bool isLedgerContinuationValid(const InputLedger::Continuation& state)
{
    if (!state.capacity || state.capacity > maxLastWrittenEntries || state.entryCount > state.capacity
        || state.recentWriterCount > state.capacity || state.writes == UINT64_MAX || state.discardSerial == UINT64_MAX
        || (state.entryCount == 0 && (state.writes || state.discardSerial || state.recentWriterCount)))
        return false;
    for (unsigned index = 0; index < state.entryCount; ++index) {
        const auto& entry = state.entries[index];
        if (entry.clientInstance == ID128 {} || !entry.inputSeq || entry.settledSeq > entry.inputSeq
            || !entry.order || entry.order > state.writes || entry.unsettledCount > maxUnsettledWrites
            || entry.discardCount > maxInputDiscardRecords)
            return false;
        for (unsigned previous = 0; previous < index; ++previous) {
            if (state.entries[previous].clientInstance == entry.clientInstance || state.entries[previous].order == entry.order)
                return false;
        }
        auto sequence = entry.settledSeq;
        std::uint64_t writtenAt = 0;
        for (unsigned writeIndex = 0; writeIndex < entry.unsettledCount; ++writeIndex) {
            const auto& write = entry.unsettled[writeIndex];
            if (write.inputSeq <= sequence || write.inputSeq > entry.inputSeq)
                return false;
            sequence = write.inputSeq;
            writtenAt = write.writtenAt;
        }
        if (sequence != entry.inputSeq || (entry.unsettledCount && writtenAt != entry.writtenAt))
            return false;
        std::uint64_t through = 0;
        std::uint64_t serial = 0;
        for (unsigned discardIndex = 0; discardIndex < entry.discardCount; ++discardIndex) {
            const auto& discard = entry.discards[discardIndex];
            if (discard.inputWritten < through || discard.inputWritten >= discard.throughSeq || discard.throughSeq > entry.inputSeq
                || discard.serial <= serial || discard.serial > state.discardSerial)
                return false;
            for (unsigned previous = 0; previous < index; ++previous) {
                const auto& other = state.entries[previous];
                for (unsigned otherIndex = 0; otherIndex < other.discardCount; ++otherIndex) {
                    if (other.discards[otherIndex].serial == discard.serial)
                        return false;
                }
            }
            through = discard.throughSeq;
            serial = discard.serial;
        }
    }
    for (unsigned index = 0; index < state.recentWriterCount; ++index) {
        if (state.recentWriters[index] == ID128 {})
            return false;
        for (unsigned previous = 0; previous < index; ++previous) {
            if (state.recentWriters[previous] == state.recentWriters[index])
                return false;
        }
    }
    return true;
}

} // namespace

std::optional<InputLedger::Continuation> InputLedger::continuation() const
{
    if (m_capacity > maxLastWrittenEntries || m_entries.size() > m_capacity || m_recentWriters.size() > m_capacity)
        return std::nullopt;
    Continuation state;
    state.capacity = static_cast<std::uint8_t>(m_capacity);
    state.entryCount = static_cast<std::uint8_t>(m_entries.size());
    state.recentWriterCount = static_cast<std::uint8_t>(m_recentWriters.size());
    state.writes = m_writes;
    state.discardSerial = m_discards;
    std::ranges::copy(m_recentWriters, state.recentWriters.begin());
    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        const auto& source = m_entries[index];
        if (source.unsettled.size() > maxUnsettledWrites || source.discards.size() > maxInputDiscardRecords)
            return std::nullopt;
        auto& entry = state.entries[index];
        entry.clientInstance = source.clientInstance;
        entry.inputSeq = source.inputSeq;
        entry.writtenAt = source.writtenAt;
        entry.settledSeq = source.settledSeq;
        entry.order = source.order;
        entry.unsettledCount = static_cast<std::uint8_t>(source.unsettled.size());
        entry.discardCount = static_cast<std::uint8_t>(source.discards.size());
        for (std::size_t write = 0; write < source.unsettled.size(); ++write)
            entry.unsettled[write] = { source.unsettled[write].inputSeq, source.unsettled[write].writtenAt };
        std::ranges::copy(source.discards, entry.discards.begin());
    }
    return isLedgerContinuationValid(state) ? std::optional { state } : std::nullopt;
}

bool InputLedger::restoreContinuation(const Continuation& state)
{
    if (state.capacity != m_capacity || !isLedgerContinuationValid(state))
        return false;
    std::vector<Entry> entries;
    entries.reserve(m_capacity);
    for (unsigned index = 0; index < state.entryCount; ++index) {
        const auto& source = state.entries[index];
        Entry entry { .clientInstance = source.clientInstance, .inputSeq = source.inputSeq, .writtenAt = source.writtenAt, .settledSeq = source.settledSeq, .unsettled = {}, .order = source.order, .discards = {} };
        entry.unsettled.reserve(source.unsettledCount);
        for (unsigned write = 0; write < source.unsettledCount; ++write)
            entry.unsettled.push_back({ source.unsettled[write].inputSeq, source.unsettled[write].writtenAt });
        entry.discards.assign(source.discards.begin(), source.discards.begin() + source.discardCount);
        entries.push_back(std::move(entry));
    }
    std::vector<ID128> recent(state.recentWriters.begin(), state.recentWriters.begin() + state.recentWriterCount);
    m_entries = std::move(entries);
    m_recentWriters = std::move(recent);
    m_writes = state.writes;
    m_discards = state.discardSerial;
    return true;
}

std::size_t intentBytes(const InputIntent& intent)
{
    // A key or pointer event costs what its encoding may take at most: a
    // Kitty report or an SGR mouse report is under 32 bytes.
    constexpr std::size_t encodedEventBytes = 32;
    struct Cost {
        std::size_t operator()(const RawInput& raw) const { return raw.bytes.size(); }
        std::size_t operator()(const KeyInput& keys) const
        {
            std::size_t bytes = 0;
            for (const KeyEvent& event : keys.events)
                bytes += encodedEventBytes + event.text.size();
            return bytes;
        }
        std::size_t operator()(const PointerInput& pointer) const { return pointer.events.size() * encodedEventBytes; }
        std::size_t operator()(const FocusInput&) const { return 3; }
        // A piece's text and, at most, the bracket around the paste.
        std::size_t operator()(const PasteInput& paste) const { return paste.text.size() + 12; }
    };
    return std::visit(Cost {}, intent);
}

std::optional<std::uint8_t> legacyControlCharacterOf(const KeyEvent& event)
{
    constexpr std::uint16_t locks = ::Ubiquitty::Keys::Keymap::keymapModifierCapsLock | ::Ubiquitty::Keys::Keymap::keymapModifierNumLock;
    if (event.kind == KeyEventKind::Release || (event.modifiers & ::Ubiquitty::Keys::characterModifierMask & ~locks) != ::Ubiquitty::Keys::Keymap::keymapModifierCtrl)
        return std::nullopt;
    const auto isLatin = [](std::uint32_t codePoint) { return (codePoint >= 'a' && codePoint <= 'z') || (codePoint >= 'A' && codePoint <= 'Z') || codePoint == '\\'; };
    const std::uint32_t codePoint = isLatin(event.unshiftedCodePoint) ? event.unshiftedCodePoint : event.baseLayoutCodePoint;
    if (!isLatin(codePoint))
        return std::nullopt;
    return static_cast<std::uint8_t>(codePoint & 0x1f);
}

InputLedger::InputLedger(std::size_t capacity)
    : m_capacity(std::max<std::size_t>(capacity, 1))
{
    m_entries.reserve(m_capacity);
}

const InputLedger::Entry* InputLedger::find(ID128 clientInstance) const
{
    const auto found = std::ranges::find(m_entries, clientInstance, &Entry::clientInstance);
    return found == m_entries.end() ? nullptr : &*found;
}

InputAdmission InputLedger::admit(ID128 clientInstance, std::uint64_t inputSeq) const
{
    const Entry* entry = find(clientInstance);
    return entry && inputSeq <= entry->inputSeq ? InputAdmission::Duplicate : InputAdmission::Write;
}

InputLedger::Entry& InputLedger::entry(ID128 clientInstance)
{
    auto found = std::ranges::find(m_entries, clientInstance, &Entry::clientInstance);
    if (found == m_entries.end()) {
        if (m_entries.size() == m_capacity)
            m_entries.erase(std::ranges::min_element(m_entries, {}, &Entry::order));
        m_entries.push_back({ .clientInstance = clientInstance, .inputSeq = 0, .writtenAt = 0, .settledSeq = 0, .unsettled = {}, .order = 0, .discards = {} });
        found = m_entries.end() - 1;
    }
    ++m_writes;
    found->order = m_writes;
    if (found->unsettled.size() == maxUnsettledWrites) {
        // The oldest two become one that settles when the later would.
        found->unsettled[1].writtenAt = std::max(found->unsettled[0].writtenAt, found->unsettled[1].writtenAt);
        found->unsettled.erase(found->unsettled.begin());
    }
    return *found;
}

void InputLedger::noteWritten(ID128 clientInstance, std::uint64_t inputSeq, std::uint64_t now)
{
    if (std::ranges::find(m_recentWriters, clientInstance) == m_recentWriters.end() && m_recentWriters.size() < m_capacity)
        m_recentWriters.push_back(clientInstance);
    Entry& written = entry(clientInstance);
    written.inputSeq = std::max(written.inputSeq, inputSeq);
    written.writtenAt = now;
    written.unsettled.push_back({ .inputSeq = inputSeq, .writtenAt = now });
}

void InputLedger::noteDiscarded(ID128 clientInstance, std::uint64_t throughSeq, std::uint64_t now)
{
    const Entry* known = find(clientInstance);
    if (throughSeq <= (known ? known->inputSeq : 0))
        return;
    Entry& dropped = entry(clientInstance);
    if (dropped.discards.size() == maxInputDiscardRecords) {
        dropped.discards[1].inputWritten = dropped.discards[0].inputWritten;
        dropped.discards.erase(dropped.discards.begin());
    }
    dropped.discards.push_back({ .inputWritten = dropped.inputSeq, .throughSeq = throughSeq, .serial = ++m_discards });
    dropped.inputSeq = throughSeq;
    dropped.writtenAt = now;
    // Settled in turn, as a write made now would be: the instance's settled
    // count passes the run no earlier than what was written before it.
    dropped.unsettled.push_back({ .inputSeq = throughSeq, .writtenAt = now });
}

std::span<const InputDiscardRecord> InputLedger::discards(ID128 clientInstance) const
{
    const Entry* known = find(clientInstance);
    return known ? std::span<const InputDiscardRecord> { known->discards } : std::span<const InputDiscardRecord> {};
}

void InputLedger::noteReadWouldBlock(std::uint64_t readBegan, std::uint64_t echoSettleMicroseconds)
{
    for (Entry& entry : m_entries) {
        // Receipts stay in input-sequence order. An overtaking signal can have
        // an earlier write time, but cannot settle before the preceding prefix.
        const auto firstUnsettled = std::ranges::find_if(entry.unsettled, [&](const Unsettled& write) {
            return readBegan < write.writtenAt || readBegan - write.writtenAt < echoSettleMicroseconds;
        });
        if (firstUnsettled == entry.unsettled.begin())
            continue;
        entry.settledSeq = std::max(entry.settledSeq, std::prev(firstUnsettled)->inputSeq);
        entry.unsettled.erase(entry.unsettled.begin(), firstUnsettled);
    }
}

void InputLedger::copySettled(std::vector<SettledMark>& marks) const
{
    marks.clear();
    for (const Entry& entry : m_entries)
        marks.push_back({ .clientInstance = entry.clientInstance, .settledSeq = entry.settledSeq });
}

std::uint64_t InputLedger::settledThrough(ID128 clientInstance) const
{
    const Entry* entry = find(clientInstance);
    return entry ? entry->settledSeq : 0;
}

std::optional<std::uint64_t> InputLedger::nextSettleMicroseconds(std::uint64_t echoSettleMicroseconds) const
{
    std::optional<std::uint64_t> next;
    for (const Entry& entry : m_entries) {
        if (!entry.unsettled.empty()) {
            const auto writtenAt = entry.unsettled.front().writtenAt;
            const auto limit = std::numeric_limits<std::uint64_t>::max();
            const auto deadline = writtenAt > limit - echoSettleMicroseconds ? limit : writtenAt + echoSettleMicroseconds;
            next = std::min(next.value_or(limit), deadline);
        }
    }
    return next;
}

std::optional<std::uint64_t> InputLedger::lastWritten(ID128 clientInstance) const
{
    const Entry* entry = find(clientInstance);
    return entry ? std::optional { entry->inputSeq } : std::nullopt;
}

std::optional<std::uint64_t> InputLedger::lastWrittenAt(ID128 clientInstance) const
{
    const Entry* entry = find(clientInstance);
    return entry ? std::optional { entry->writtenAt } : std::nullopt;
}

bool PaneInputQueue::isValidContinuation(const Continuation& state, const InputLedger& ledger)
{
    if (state.pending.size() > maxContinuationIntents || state.passed.size() > maxContinuationIntents
        || state.pastes.size() > maxLastWrittenEntries || state.typedWrites == UINT64_MAX
        || state.heldBytes > maxPaneHeldInputBytes
        || (state.isProgramGone && (!state.pending.empty() || !state.pastes.empty() || !state.passed.empty())))
        return false;
    std::size_t normalBytes = 0;
    std::size_t signalBytes = 0;
    std::size_t ownBytes = 0;
    std::vector<Share> shares;
    bool hasOrdinary = false;
    for (std::size_t pendingIndex = 0; pendingIndex < state.pending.size(); ++pendingIndex) {
        const auto& pending = state.pending[pendingIndex];
        if ((pending.inputSeq == 0) != (pending.clientInstance == ID128 {})
            || (!pending.inputSeq && (pending.authority != ID128 {} || pending.isSignal))
            || pending.endedPastes.size() > maxLastWrittenEntries || (!pending.isSignal && !pending.endedPastes.empty()))
            return false;
        if (pending.isSignal && hasOrdinary)
            return false;
        hasOrdinary |= !pending.isSignal;
        if (std::ranges::find(pending.endedPastes, ID128 {}) != pending.endedPastes.end())
            return false;
        if (const auto* raw = std::get_if<RawInput>(&pending.intent)) {
            if (raw->bytes.size() > maxHeldInputBytes)
                return false;
        } else if (const auto* keys = std::get_if<KeyInput>(&pending.intent)) {
            if (keys->events.size() > maxHeldInputBytes / 32)
                return false;
            for (const auto& event : keys->events) {
                if (event.key > UINT16_MAX || event.kind > KeyEventKind::Release || event.text.size() > maxKeyEventTextBytes
                    || !isValidUTF8({ reinterpret_cast<const std::uint8_t*>(event.text.data()), event.text.size() }))
                    return false;
            }
        } else if (const auto* pointer = std::get_if<PointerInput>(&pending.intent)) {
            if (pointer->events.size() > maxHeldInputBytes / 32
                || std::ranges::any_of(pointer->events, [](const auto& event) { return event.kind > PointerEventKind::Motion; }))
                return false;
        } else if (const auto* focus = std::get_if<FocusInput>(&pending.intent)) {
            if (focus->focus > Modes::FocusState::Focused)
                return false;
        } else if (const auto* paste = std::get_if<PasteInput>(&pending.intent)) {
            if (paste->end > PasteEnd::Aborted || paste->text.size() > maxHeldInputBytes
                || paste->offset > UINT64_MAX - paste->text.size() || !pending.inputSeq || pending.isSignal)
                return false;
            const auto progress = std::ranges::find_if(state.pastes, [&](const auto& held) {
                return held.clientInstance == pending.clientInstance && held.inputSeq == pending.inputSeq;
            });
            if (progress == state.pastes.end() || progress->authority != pending.authority)
                return false;
        }
        const auto bytes = intentBytes(pending.intent);
        if (pending.isSignal) {
            if (bytes > maxPendingSignalBytes - signalBytes)
                return false;
            signalBytes += bytes;
        } else if (!pending.inputSeq) {
            // The pane's own reports live in the slack.
            if (bytes > maxInputShareSlack - ownBytes)
                return false;
            ownBytes += bytes;
            normalBytes += bytes;
        } else {
            // Each instance within its own share and the slack the host adds
            // to it, and no more instances than a pane holds input from.
            auto share = std::ranges::find(shares, pending.clientInstance, &Share::clientInstance);
            if (share == shares.end()) {
                if (shares.size() >= maxInputInstances)
                    return false;
                shares.push_back({ .clientInstance = pending.clientInstance, .bytes = 0, .events = 0, .entries = 0, .highestSeq = 0 });
                share = shares.end() - 1;
            }
            if (bytes > maxHeldInputBytes + maxInputShareSlack - share->bytes || share->events > maxHeldInputEvents)
                return false;
            share->bytes += bytes;
            ++share->events;
            normalBytes += bytes;
        }
        if (pending.inputSeq && !pending.isSignal && pending.inputSeq <= ledger.lastWritten(pending.clientInstance).value_or(0))
            return false;
        for (std::size_t prior = 0; pending.inputSeq && prior < pendingIndex; ++prior) {
            const auto& earlier = state.pending[prior];
            if (earlier.clientInstance != pending.clientInstance)
                continue;
            if (earlier.inputSeq == pending.inputSeq
                && (!std::holds_alternative<PasteInput>(earlier.intent) || !std::holds_alternative<PasteInput>(pending.intent)))
                return false;
            if (!earlier.isSignal && !pending.isSignal && !std::holds_alternative<PasteInput>(pending.intent)
                && earlier.inputSeq > pending.inputSeq)
                return false;
        }
    }
    if (state.heldBytes != normalBytes + signalBytes)
        return false;
    for (std::size_t index = 0; index < state.pastes.size(); ++index) {
        const auto& paste = state.pastes[index];
        if (paste.clientInstance == ID128 {} || !paste.inputSeq || paste.bytesWritten > paste.bytesTaken
            || paste.inputSeq <= ledger.lastWritten(paste.clientInstance).value_or(0))
            return false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (state.pastes[prior].clientInstance == paste.clientInstance && state.pastes[prior].inputSeq == paste.inputSeq)
                return false;
        }
        auto offset = paste.bytesWritten;
        bool hasEnd = false;
        for (const auto& pending : state.pending) {
            if (pending.clientInstance != paste.clientInstance || pending.inputSeq != paste.inputSeq)
                continue;
            const auto* piece = std::get_if<PasteInput>(&pending.intent);
            if (!piece || hasEnd)
                return false;
            if (piece->end == PasteEnd::Aborted) {
                if (!piece->text.empty() || piece->offset != paste.bytesTaken)
                    return false;
                offset = paste.bytesTaken;
            } else {
                if (piece->offset != offset || piece->text.size() > paste.bytesTaken - offset)
                    return false;
                offset += piece->text.size();
            }
            hasEnd = piece->end != PasteEnd::More;
        }
        if (offset != paste.bytesTaken || hasEnd != paste.hasEnded)
            return false;
    }
    for (std::size_t index = 0; index < state.passed.size(); ++index) {
        const auto& passed = state.passed[index];
        if (passed.clientInstance == ID128 {} || !passed.inputSeq
            || passed.inputSeq <= ledger.lastWritten(passed.clientInstance).value_or(0)
            || std::ranges::any_of(state.pending, [&](const auto& held) {
                   return held.clientInstance == passed.clientInstance && held.inputSeq == passed.inputSeq;
               })
            || !std::ranges::any_of(state.pending, [&](const auto& held) {
                   return held.clientInstance == passed.clientInstance && held.inputSeq && held.inputSeq < passed.inputSeq;
               }))
            return false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (state.passed[prior].clientInstance == passed.clientInstance && state.passed[prior].inputSeq == passed.inputSeq)
                return false;
        }
    }
    return true;
}

std::optional<PaneInputQueue::Continuation> PaneInputQueue::continuation(const InputLedger& ledger) const
{
    if (m_pending.size() > maxContinuationIntents || m_passed.size() > maxContinuationIntents
        || m_pastes.size() > maxLastWrittenEntries || m_bytes > maxPaneHeldInputBytes)
        return std::nullopt;
    Continuation state { { m_pending.begin(), m_pending.end() }, m_pastes, m_passed,
        static_cast<std::uint64_t>(m_bytes), m_typedWrites, m_isProgramGone };
    return isValidContinuation(state, ledger) ? std::optional { std::move(state) } : std::nullopt;
}

bool PaneInputQueue::restoreContinuation(Continuation&& state, const InputLedger& ledger)
{
    if (!m_pending.empty() || !m_pastes.empty() || !m_passed.empty() || m_bytes || m_typedWrites || m_isProgramGone
        || !isValidContinuation(state, ledger))
        return false;
    std::deque<Pending> pending;
    for (auto& intent : state.pending)
        pending.push_back(std::move(intent));
    m_pending = std::move(pending);
    m_pastes = std::move(state.pastes);
    m_passed = std::move(state.passed);
    recountShares();
    m_typedWrites = state.typedWrites;
    m_isProgramGone = state.isProgramGone;
    state = {};
    return true;
}

EnqueueOutcome PaneInputQueue::enqueue(const InputLedger& ledger, ID128 clientInstance, std::uint64_t inputSeq, InputIntent intent, ID128 authority, std::size_t room)
{
    if (auto* piece = std::get_if<PasteInput>(&intent))
        return enqueuePaste(ledger, clientInstance, inputSeq, std::move(*piece), authority, room);
    if (inputSeq == 0 || ledger.admit(clientInstance, inputSeq) == InputAdmission::Duplicate)
        return EnqueueOutcome::Duplicate;
    if (isHeld(clientInstance, inputSeq))
        return EnqueueOutcome::Duplicate;
    if (m_isProgramGone || !admits(clientInstance, intentBytes(intent), room))
        return EnqueueOutcome::Refused;
    insert(m_pending.end(), { .clientInstance = clientInstance, .authority = authority, .inputSeq = inputSeq, .intent = std::move(intent), .isSignal = false, .endedPastes = {} });
    return EnqueueOutcome::Queued;
}

const PaneInputQueue::Share* PaneInputQueue::share(ID128 clientInstance) const
{
    const auto found = std::ranges::find(m_shares, clientInstance, &Share::clientInstance);
    return found == m_shares.end() ? nullptr : &*found;
}

std::size_t PaneInputQueue::heldBytes(ID128 clientInstance) const
{
    const Share* held = share(clientInstance);
    return held ? held->bytes : 0;
}

bool PaneInputQueue::admits(ID128 clientInstance, std::size_t bytes, std::size_t room) const
{
    // Each instance is held to its own share: what another sent never counts
    // against it, so one sender's flood refuses nobody else.
    const Share* held = share(clientInstance);
    if (bytes > room)
        return false;
    if (!held)
        return m_shares.size() < maxInputInstances && bytes <= maxHeldInputBytes;
    return held->events < maxHeldInputEvents && bytes <= maxHeldInputBytes - held->bytes;
}

void PaneInputQueue::insert(std::deque<Pending>::iterator place, Pending&& pending)
{
    const std::size_t bytes = intentBytes(pending.intent);
    m_bytes += bytes;
    if (pending.isSignal)
        m_signalBytes += bytes;
    // An event no client numbered, a focus report, is the pane's own.
    if (pending.inputSeq != 0) {
        auto found = std::ranges::find(m_shares, pending.clientInstance, &Share::clientInstance);
        if (found == m_shares.end()) {
            m_shares.push_back({ .clientInstance = pending.clientInstance, .bytes = 0, .events = 0, .entries = 0, .highestSeq = 0 });
            found = m_shares.end() - 1;
        }
        if (!pending.isSignal) {
            found->bytes += bytes;
            ++found->events;
        }
        ++found->entries;
        found->highestSeq = std::max(found->highestSeq, pending.inputSeq);
    }
    m_pending.insert(place, std::move(pending));
}

void PaneInputQueue::eraseAt(std::size_t index)
{
    const auto at = m_pending.begin() + static_cast<std::ptrdiff_t>(index);
    const ID128 clientInstance = at->clientInstance;
    const std::uint64_t inputSeq = at->inputSeq;
    const bool isSignal = at->isSignal;
    const std::size_t bytes = intentBytes(at->intent);
    m_pending.erase(at);
    m_bytes -= bytes;
    if (isSignal)
        m_signalBytes -= bytes;
    if (inputSeq == 0)
        return;
    const auto found = std::ranges::find(m_shares, clientInstance, &Share::clientInstance);
    if (found == m_shares.end())
        return;
    if (!isSignal) {
        found->bytes -= bytes;
        --found->events;
    }
    if (--found->entries == 0) {
        m_shares.erase(found);
        return;
    }
    // An instance's events leave from the front in its own order, so its
    // highest goes last, with the share; only a signal, written ahead of the
    // rest, leaves the highest behind it.
    if (inputSeq == found->highestSeq) {
        found->highestSeq = 0;
        for (const Pending& pending : m_pending) {
            if (pending.clientInstance == clientInstance)
                found->highestSeq = std::max(found->highestSeq, pending.inputSeq);
        }
    }
}

void PaneInputQueue::recountShares()
{
    m_shares.clear();
    m_bytes = 0;
    m_signalBytes = 0;
    std::deque<Pending> pending = std::exchange(m_pending, {});
    for (Pending& entry : pending)
        insert(m_pending.end(), std::move(entry));
}

bool PaneInputQueue::isHeld(ID128 clientInstance, std::uint64_t inputSeq) const
{
    // An event at or below one of its instance's still queued is a resend of
    // it: the queue keeps each instance's numbers ascending, but for a signal,
    // which stands ahead of them.
    const Share* held = share(clientInstance);
    return (held && inputSeq <= held->highestSeq)
        || std::ranges::any_of(m_passed, [&](const Passed& passed) { return passed.clientInstance == clientInstance && inputSeq <= passed.inputSeq; });
}

EnqueueOutcome PaneInputQueue::enqueueSignal(InputLedger& ledger, ID128 clientInstance, std::uint64_t inputSeq, InputIntent intent, SignalEffect effect, std::uint64_t now, ID128 authority)
{
    if (inputSeq == 0 || ledger.admit(clientInstance, inputSeq) == InputAdmission::Duplicate || isHeld(clientInstance, inputSeq))
        return EnqueueOutcome::Duplicate;
    if (m_isProgramGone || intentBytes(intent) > maxPendingSignalBytes - m_signalBytes)
        return EnqueueOutcome::Refused;
    std::vector<ID128> endedPastes;
    if (effect == SignalEffect::SignalsAndDiscards) {
        // What waits unwritten goes with the input the terminal discards:
        // every instance's events, and every paste in progress, whose number
        // is consumed whole. A signal queued earlier stays ahead.
        std::vector<std::pair<ID128, std::uint64_t>> dropped;
        const auto drop = [&](ID128 instance, std::uint64_t seq) {
            if (seq == 0)
                return;
            const auto found = std::ranges::find(dropped, instance, &std::pair<ID128, std::uint64_t>::first);
            if (found == dropped.end())
                dropped.emplace_back(instance, seq);
            else
                found->second = std::max(found->second, seq);
        };
        for (const Pending& pending : m_pending) {
            if (!pending.isSignal)
                drop(pending.clientInstance, pending.inputSeq);
        }
        for (const PasteProgress& paste : m_pastes) {
            drop(paste.clientInstance, paste.inputSeq);
            endedPastes.push_back(paste.clientInstance);
        }
        // The event's own instance loses only what it numbered before the
        // event: nothing it has queued is numbered at or above it.
        for (const auto& [instance, seq] : dropped)
            ledger.noteDiscarded(instance, seq, now);
        std::erase_if(m_pending, [](const Pending& pending) { return !pending.isSignal; });
        m_pastes.clear();
        recountShares();
    }
    const auto place = std::ranges::find_if(m_pending, [](const Pending& pending) { return !pending.isSignal; });
    insert(place, Pending { .clientInstance = clientInstance, .authority = authority, .inputSeq = inputSeq, .intent = std::move(intent), .isSignal = true, .endedPastes = std::move(endedPastes) });
    return EnqueueOutcome::Queued;
}

void PaneInputQueue::settlePassed(InputLedger& ledger)
{
    std::erase_if(m_passed, [&](const Passed& passed) {
        const bool isBehind = std::ranges::any_of(m_pending, [&](const Pending& pending) { return pending.clientInstance == passed.clientInstance && pending.inputSeq != 0 && pending.inputSeq < passed.inputSeq; });
        if (isBehind)
            return false;
        ledger.noteWritten(passed.clientInstance, passed.inputSeq, passed.writtenAt);
        return true;
    });
}

EnqueueOutcome PaneInputQueue::enqueuePaste(const InputLedger& ledger, ID128 clientInstance, std::uint64_t inputSeq, PasteInput piece, ID128 authority, std::size_t room)
{
    if (inputSeq == 0 || ledger.admit(clientInstance, inputSeq) == InputAdmission::Duplicate)
        return EnqueueOutcome::Duplicate;
    auto progress = std::ranges::find_if(m_pastes, [&](const PasteProgress& paste) { return paste.clientInstance == clientInstance && paste.inputSeq == inputSeq; });
    if (progress != m_pastes.end() && progress->authority != authority)
        return EnqueueOutcome::Refused;
    // Everything of a paste after its end was taken, a resend or a piece an
    // abort overtook, is dropped.
    if (progress != m_pastes.end() && progress->hasEnded)
        return EnqueueOutcome::Duplicate;
    const std::uint64_t taken = progress != m_pastes.end() ? progress->bytesTaken : 0;
    // Only what overlaps what was taken is trimmed, a resend from the start;
    // a piece past it skips bytes no correct client skips.
    if (piece.offset > taken)
        return EnqueueOutcome::Malformed;
    const std::uint64_t end = piece.offset + piece.text.size();
    if (end <= taken && piece.end == PasteEnd::More)
        return EnqueueOutcome::Duplicate;
    const std::size_t overlap = static_cast<std::size_t>(std::min<std::uint64_t>(taken - piece.offset, piece.text.size()));
    piece.text.erase(piece.text.begin(), piece.text.begin() + static_cast<std::ptrdiff_t>(overlap));
    piece.offset = taken;
    if (m_isProgramGone || !admits(clientInstance, intentBytes(piece), room))
        return EnqueueOutcome::Refused;
    if (progress == m_pastes.end()) {
        // One paste in progress per instance the ledger remembers.
        if (m_pastes.size() >= maxLastWrittenEntries)
            return EnqueueOutcome::Refused;
        m_pastes.push_back({ .clientInstance = clientInstance, .authority = authority, .inputSeq = inputSeq, .bytesTaken = 0, .bytesWritten = 0, .hasEnded = false });
        progress = m_pastes.end() - 1;
    }
    progress->bytesTaken = std::max(taken, end);
    progress->hasEnded = piece.end != PasteEnd::More;
    insert(m_pending.end(), { .clientInstance = clientInstance, .authority = authority, .inputSeq = inputSeq, .intent = std::move(piece), .isSignal = false, .endedPastes = {} });
    return EnqueueOutcome::Queued;
}

bool PaneInputQueue::isHeldBehindPaste(const Pending& pending) const
{
    // A paste's progress stands until its end is written; what its client
    // numbered after it waits until then.
    return pending.inputSeq != 0 && std::ranges::any_of(m_pastes, [&](const PasteProgress& paste) { return paste.clientInstance == pending.clientInstance && paste.inputSeq < pending.inputSeq; });
}

void PaneInputQueue::endPaste(PasteProgress& progress)
{
    // Where the end goes: where its first unwritten piece stood, or ahead of
    // the first thing its client numbered after it, so the bracket closes
    // before any of it.
    std::optional<std::size_t> place;
    for (std::size_t index = 0; index < m_pending.size();) {
        Pending& pending = m_pending[index];
        const bool isSameClient = pending.clientInstance == progress.clientInstance;
        if (isSameClient && pending.inputSeq == progress.inputSeq && std::holds_alternative<PasteInput>(pending.intent)) {
            place = place.value_or(index);
            eraseAt(index);
            continue;
        }
        if (!place && isSameClient && pending.inputSeq > progress.inputSeq)
            place = index;
        ++index;
    }
    PasteInput end { .text = {}, .offset = progress.bytesTaken, .end = PasteEnd::Aborted };
    insert(m_pending.begin() + static_cast<std::ptrdiff_t>(place.value_or(m_pending.size())), Pending { .clientInstance = progress.clientInstance, .authority = progress.authority, .inputSeq = progress.inputSeq, .intent = std::move(end), .isSignal = false, .endedPastes = {} });
    progress.hasEnded = true;
}

bool PaneInputQueue::abortPaste(const InputLedger& ledger, ID128 clientInstance, std::uint64_t inputSeq, ID128 authority)
{
    if (m_isProgramGone || inputSeq == 0 || ledger.admit(clientInstance, inputSeq) == InputAdmission::Duplicate)
        return false;
    const auto progress = std::ranges::find_if(m_pastes, [&](const PasteProgress& paste) { return paste.clientInstance == clientInstance; });
    if (progress != m_pastes.end()) {
        // Only the paste in progress is given up.
        if (progress->inputSeq != inputSeq || progress->hasEnded || progress->authority != authority)
            return false;
        endPaste(*progress);
        return true;
    }
    // An abort that overtook the paste's first piece, on a carrier whose
    // lanes reorder: the paste ends before it starts, and its pieces are
    // dropped when they come.
    if (m_pastes.size() >= maxLastWrittenEntries)
        return false;
    m_pastes.push_back({ .clientInstance = clientInstance, .authority = authority, .inputSeq = inputSeq, .bytesTaken = 0, .bytesWritten = 0, .hasEnded = false });
    endPaste(m_pastes.back());
    return true;
}

void PaneInputQueue::abandonPaste(ID128 clientInstance)
{
    const auto progress = std::ranges::find_if(m_pastes, [&](const PasteProgress& paste) { return paste.clientInstance == clientInstance && !paste.hasEnded; });
    if (progress != m_pastes.end() && !m_isProgramGone)
        endPaste(*progress);
}

std::optional<PaneInputQueue::PasteWritten> PaneInputQueue::pasteWritten(ID128 clientInstance) const
{
    const auto progress = std::ranges::find(m_pastes, clientInstance, &PasteProgress::clientInstance);
    return progress == m_pastes.end() ? std::nullopt : std::optional { PasteWritten { .inputSeq = progress->inputSeq, .bytesWritten = progress->bytesWritten } };
}

std::optional<std::uint64_t> PaneInputQueue::pasteTaken(ID128 clientInstance, std::uint64_t inputSeq) const
{
    const auto progress = std::ranges::find_if(m_pastes, [&](const PasteProgress& paste) { return paste.clientInstance == clientInstance && paste.inputSeq == inputSeq; });
    return progress == m_pastes.end() ? std::nullopt : std::optional { progress->bytesTaken };
}

EnqueueOutcome PaneInputQueue::enqueueUnnumbered(InputIntent intent)
{
    if (m_isProgramGone)
        return EnqueueOutcome::Refused;
    // The newest report that waits is replaced; the head may be partly
    // written, so a report there stays as it is.
    for (std::size_t index = m_pending.size(); index-- > 1;) {
        Pending& waiting = m_pending[index];
        if (waiting.inputSeq != 0)
            continue;
        if (waiting.intent.index() != intent.index())
            break;
        m_bytes -= intentBytes(waiting.intent);
        waiting.intent = std::move(intent);
        m_bytes += intentBytes(waiting.intent);
        return EnqueueOutcome::Queued;
    }
    insert(m_pending.end(), { .clientInstance = {}, .authority = {}, .inputSeq = 0, .intent = std::move(intent), .isSignal = false, .endedPastes = {} });
    return EnqueueOutcome::Queued;
}

void PaneInputQueue::withdraw(InputLedger& ledger, ID128 authority)
{
    if (authority == ID128 {})
        return;
    std::erase_if(m_pending, [&](const Pending& pending) { return pending.authority == authority; });
    std::erase_if(m_pastes, [&](const PasteProgress& paste) { return paste.authority == authority; });
    recountShares();
    settlePassed(ledger);
}

bool PaneInputQueue::drain(InputWriter& writer, InputLedger& ledger, std::uint64_t now)
{
    bool hasWritten = false;
    for (std::size_t index = 0; index < m_pending.size();) {
        Pending& entry = m_pending[index];
        // Another client's input does not wait behind this client's paste,
        // and a signal waits behind nothing.
        if (!entry.isSignal && isHeldBehindPaste(entry)) {
            ++index;
            continue;
        }
        const WriteStatus status = entry.isSignal ? writer.writeSignalOwned(entry.authority, entry.clientInstance, entry.intent, entry.endedPastes) : writer.writeOwned(entry.authority, entry.clientInstance, entry.intent);
        if (status == WriteStatus::Refused)
            break;
        if (status == WriteStatus::Withdrawn) {
            const ID128 authority = entry.authority;
            withdraw(ledger, authority);
            writer.withdraw(authority);
            index = 0;
            continue;
        }
        if (status == WriteStatus::Closed) {
            m_isProgramGone = true;
            m_pending.clear();
            m_pastes.clear();
            m_passed.clear();
            recountShares();
            break;
        }
        if (entry.isSignal) {
            // Counted written once nothing its instance numbered before it
            // waits: until then the instance's count stays below them.
            m_passed.push_back({ .clientInstance = entry.clientInstance, .inputSeq = entry.inputSeq, .writtenAt = now });
            eraseAt(index);
            settlePassed(ledger);
            hasWritten = true;
            continue;
        }
        // A paste counts as written once its end is; until then its client
        // is told how far it got.
        const auto* piece = std::get_if<PasteInput>(&entry.intent);
        if (entry.inputSeq != 0 && (!piece || piece->end != PasteEnd::More))
            ledger.noteWritten(entry.clientInstance, entry.inputSeq, now);
        if (piece) {
            const auto progress = std::ranges::find_if(m_pastes, [&](const PasteProgress& paste) { return paste.clientInstance == entry.clientInstance && paste.inputSeq == entry.inputSeq; });
            if (progress != m_pastes.end())
                progress->bytesWritten = std::max<std::uint64_t>(progress->bytesWritten, piece->offset + piece->text.size());
        }
        if (piece && piece->end != PasteEnd::More)
            std::erase_if(m_pastes, [&](const PasteProgress& paste) { return paste.clientInstance == entry.clientInstance && paste.inputSeq == entry.inputSeq; });
        // Typed: raw bytes and keys as each is written, a paste once, when the
        // whole of it has been. A pointer event and a focus report are not.
        if (std::holds_alternative<RawInput>(entry.intent) || std::holds_alternative<KeyInput>(entry.intent) || (piece && piece->end == PasteEnd::Final))
            ++m_typedWrites;
        const bool hasEndedPaste = piece && piece->end != PasteEnd::More;
        eraseAt(index);
        settlePassed(ledger);
        hasWritten = true;
        // What waited behind a paste whose end was written may go now.
        if (hasEndedPaste)
            index = 0;
    }
    return hasWritten;
}

} // namespace Ubiquitty::Protocol::Sync
