// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/DragDrop/Queue.h"

#include <new>
#include <utility>

namespace Ubiquitty::DragDrop {

Queue::~Queue()
{
    clear();
}

void Queue::clear()
{
    while (m_head) {
        auto node = std::move(m_head);
        m_head = std::move(node->next);
    }
    m_tail = nullptr;
    m_size = 0;
    m_bytes = 0;
}

void Queue::discard(std::uint64_t session)
{
    auto* link = &m_head;
    m_tail = nullptr;
    while (*link) {
        auto& node = **link;
        if (node.record.sessionID == session) {
            --m_size;
            m_bytes -= node.record.bytes.size();
            auto removed = std::move(*link);
            *link = std::move(removed->next);
        } else {
            m_tail = link->get();
            link = &node.next;
        }
    }
}

bool Queue::sendCommand(const Command& command)
{
    constexpr std::size_t maxQueuedCommands = 32768;
    constexpr std::size_t maxQueuedBytes = maxPreparedDataBytes + maxImageBytes + maxMIMEListBytes * 4;
    if (command.kind == CommandKind::CancelDrag || command.kind == CommandKind::FinishDrop)
        discard(command.sessionID);
    if (command.kind == CommandKind::Registration) {
        const auto sameRegistration = [&](const Metadata& previous) {
            if (command.metadata.type == Type::Offer)
                return previous.type == Type::Offer && (command.metadata.x == 2 || previous.x != 2);
            if (command.metadata.type == Type::StopAccepting)
                return previous.type == Type::Accept || previous.type == Type::StopAccepting;
            return previous.type == Type::Accept && previous.x.value_or(0) == command.metadata.x.value_or(0);
        };
        auto* link = &m_head;
        m_tail = nullptr;
        while (*link) {
            auto& node = **link;
            if (node.record.kind == CommandKind::Registration && sameRegistration(node.record.metadata)) {
                --m_size;
                m_bytes -= node.record.bytes.size();
                auto removed = std::move(*link);
                *link = std::move(removed->next);
            } else {
                m_tail = link->get();
                link = &node.next;
            }
        }
    }
    if (m_size >= maxQueuedCommands || command.payload.size() > maxQueuedBytes - m_bytes)
        return false;
    auto node = std::unique_ptr<Node>(new (std::nothrow) Node);
    if (!node || !node->record.bytes.tryAssign(command.payload.begin(), command.payload.end()))
        return false;
    node->record.kind = command.kind;
    node->record.sessionID = command.sessionID;
    node->record.requestID = command.requestID;
    node->record.metadata = command.metadata;
    node->record.isFinal = command.isFinal;
    node->record.isPresent = true;
    auto* tail = node.get();
    if (m_tail)
        m_tail->next = std::move(node);
    else
        m_head = std::move(node);
    m_tail = tail;
    ++m_size;
    m_bytes += command.payload.size();
    if (m_wake)
        m_wake(m_context);
    return true;
}

CommandRecord Queue::take()
{
    if (!m_head)
        return {};
    auto node = std::move(m_head);
    m_head = std::move(node->next);
    if (!m_head)
        m_tail = nullptr;
    --m_size;
    m_bytes -= node->record.bytes.size();
    return std::move(node->record);
}

} // namespace Ubiquitty::DragDrop
