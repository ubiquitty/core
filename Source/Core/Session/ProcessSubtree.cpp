// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/ProcessSubtree.cpp — see ProcessSubtree.h.

#include "Core/Session/ProcessSubtree.h"

namespace Ubiquitty::Session::Process {

namespace {

const RawNode* findNode(const RawNode* nodes, int count, uint32_t processID)
{
    if (processID == 0)
        return nullptr;
    for (int i = 0; i < count; ++i) {
        if (nodes[i].processID == processID)
            return &nodes[i];
    }
    return nullptr;
}

} // namespace

bool processInSubtree(const RawNode* nodes, int count, const RawNode& node, uint32_t leader, int maxHops)
{
    if (leader == 0)
        return false;
    if (node.processID == leader)
        return true;
    // Walk the parent-PID chain toward the leader. Stop at pid 1 / pid 0 (init
    // or the unrooted top), when a hop names a pid absent from the list, or
    // after maxHops — any of which means this node is not under the leader.
    uint32_t ancestor = node.parentProcessID;
    for (int hop = 0; hop < maxHops && ancestor > 1; ++hop) {
        if (ancestor == leader)
            return true;
        const RawNode* parent = findNode(nodes, count, ancestor);
        if (!parent)
            return false;
        ancestor = parent->parentProcessID;
    }
    return false;
}

int gatherSubtree(const RawNode* nodes, int count, uint32_t leader, uint32_t foregroundGroup, std::string_view tty, std::span<PAL::Process::Record> outputBuffer)
{
    if (outputBuffer.empty() || leader == 0 || !nodes || count <= 0)
        return 0;

    const int capacity = static_cast<int>(outputBuffer.size());
    int written = 0;
    for (int i = 0; i < count && written < capacity; ++i) {
        const RawNode& node = nodes[i];
        if (node.processID == 0)
            continue;
        if (!processInSubtree(nodes, count, node, leader))
            continue;

        PAL::Process::Record& record = outputBuffer[written];
        record = PAL::Process::Record {};
        record.processID = node.processID;
        record.parentProcessID = node.parentProcessID;
        record.processGroupID = node.processGroupID;
        record.isForeground = (foregroundGroup != 0 && node.processGroupID == foregroundGroup);
        record.isSessionLeader = (node.processID == leader);
        // name / commandLine / user stay empty for the platform to fill per
        // admitted pid; only the tty is policy this layer can stamp.
        record.tty = std::string(tty);
        ++written;
    }
    return written;
}

} // namespace Ubiquitty::Session::Process
