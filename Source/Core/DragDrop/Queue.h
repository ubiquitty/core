// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/DragDrop/Client.h"

#include <memory>
#include <string>

namespace Ubiquitty::DragDrop {

/// An owning command that can cross the model lock without borrowing parser data.
struct CommandRecord {
    CommandKind kind {};
    std::uint64_t sessionID = 0;
    std::uint64_t requestID = 0;
    Metadata metadata {};
    FallibleVector<char> bytes {};
    bool isFinal = false;
    bool isPresent = false;

    [[nodiscard]] std::string payload() const { return bytes.empty() ? std::string {} : std::string(bytes.data(), bytes.size()); }
    [[nodiscard]] std::int64_t x() const { return metadata.x.value_or(0); }
    [[nodiscard]] std::int64_t y() const { return metadata.y.value_or(0); }
    [[nodiscard]] std::int64_t pixelX() const { return metadata.pixelX.value_or(0); }
    [[nodiscard]] std::int64_t pixelY() const { return metadata.pixelY.value_or(0); }
    [[nodiscard]] std::uint32_t operation() const { return metadata.operation.value_or(0); }
    [[nodiscard]] char type() const { return static_cast<char>(metadata.type); }
    [[nodiscard]] std::size_t payloadSize() const { return bytes.size(); }
    [[nodiscard]] std::uint8_t payloadByte(std::size_t index) const { return index < bytes.size() ? static_cast<std::uint8_t>(bytes[index]) : 0; }
};

/// A model-lock-protected queue. Cancellation removes undelivered data first.
class Queue final : public Client {
public:
    /// Releases pending payloads without recursive node destruction.
    ~Queue() override;
    [[nodiscard]] bool sendCommand(const Command&) override;
    [[nodiscard]] CommandRecord take();
    [[nodiscard]] const CommandRecord* front() const { return m_head ? &m_head->record : nullptr; }
    /// Discards all pending commands and their owned payloads.
    void clear();
    [[nodiscard]] bool isEmpty() const { return !m_head; }
    /// Called under the model lock; it may set an event bit, but cannot reenter.
    void setWake(void (*wake)(void*), void* context)
    {
        m_wake = wake;
        m_context = context;
    }

private:
    /// Owns one payload and the following node; destruction is iterative.
    struct Node {
        CommandRecord record;
        std::unique_ptr<Node> next;
    };

    /// Removes undelivered commands belonging to one native session.
    void discard(std::uint64_t);
    std::unique_ptr<Node> m_head;
    Node* m_tail = nullptr;
    std::size_t m_size = 0;
    std::size_t m_bytes = 0;
    void (*m_wake)(void*) = nullptr;
    void* m_context = nullptr;
};

} // namespace Ubiquitty::DragDrop
