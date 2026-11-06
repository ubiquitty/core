// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Workspace.cpp — see Workspace.h.

#include "Core/Session/Workspace.h"

#include "Core/Codec/Base64.h"
#include "Core/Protocol/Sync/Topology.h"
#include "Core/Protocol/Sync/WireFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

namespace Ubiquitty::Session {

namespace {

using Protocol::Sync::putF64;
using Protocol::Sync::putU32;
using Protocol::Sync::putU8;
using Reader = Protocol::Sync::Reader;

constexpr std::uint8_t workspaceVersion = 3;
// The one nesting bound of a split tree, which guards a corrupt capture.
constexpr int maxPaneDepth = static_cast<int>(Protocol::Sync::maxTopologyDepth);

// The workspace capture's string dialect: [u32 length][bytes] (wider
// than the Sync protocol's u16 prefix; both formats persist, so
// neither can silently adopt the other's width).
void putString(std::vector<std::uint8_t>& out, const std::string& s)
{
    putU32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

std::string readString(Reader& reader)
{
    const std::uint32_t length = reader.u32();
    const std::span<const std::uint8_t> bytes = reader.bytes(length);
    if (!reader.ok)
        return {};
    return { bytes.begin(), bytes.end() };
}

void serializePane(const PaneNode& node, std::vector<std::uint8_t>& out)
{
    putU8(out, node.leaf ? 1 : 0);
    if (node.leaf) {
        putU8(out, static_cast<std::uint8_t>(node.origin.transport));
        putString(out, node.origin.host);
        putString(out, node.title);
        putString(out, node.directory);
    } else {
        putU8(out, node.horizontalSplit ? 1 : 0);
        putF64(out, node.ratio);
        // A well-formed split has exactly two children; serialize both.
        serializePane(node.children.size() > 0 ? node.children[0] : PaneNode {}, out);
        serializePane(node.children.size() > 1 ? node.children[1] : PaneNode {}, out);
    }
}

// Deserialization checks reader.ok after every read group, not just at
// the end: a truncated buffer must stop the pane recursion immediately
// (a latched-but-ignored reader would keep "reading" zero split nodes
// to the depth cap).
bool deserializePane(Reader& reader, PaneNode& node, int depth)
{
    if (depth > maxPaneDepth)
        return false;
    node.leaf = reader.u8() != 0;
    if (!reader.ok)
        return false;
    if (node.leaf) {
        node.origin.transport = static_cast<Transport::OriginKind>(reader.u8());
        node.origin.host = readString(reader);
        node.title = readString(reader);
        node.directory = readString(reader);
        return reader.ok;
    }
    node.horizontalSplit = reader.u8() != 0;
    node.ratio = reader.f64();
    if (!reader.ok)
        return false;
    node.children.resize(2);
    return deserializePane(reader, node.children[0], depth + 1) && deserializePane(reader, node.children[1], depth + 1);
}

} // namespace

void serializeWorkspace(const Workspace& workspace, std::vector<std::uint8_t>& out)
{
    putU8(out, workspaceVersion);
    putString(out, workspace.name);
    putU32(out, static_cast<std::uint32_t>(workspace.activeWindow));
    putU32(out, static_cast<std::uint32_t>(workspace.windows.size()));
    for (const WorkspaceWindow& window : workspace.windows) {
        putU32(out, static_cast<std::uint32_t>(window.activeTab));
        putU32(out, static_cast<std::uint32_t>(window.zoomedPaneIndex) + 1);
        putU32(out, static_cast<std::uint32_t>(window.tabs.size()));
        for (const WorkspaceTab& tab : window.tabs) {
            putString(out, tab.title);
            putU32(out, static_cast<std::uint32_t>(tab.focusedPaneIndex));
            serializePane(tab.root, out);
        }
    }
}

bool deserializeWorkspace(std::span<const std::uint8_t> bytes, Workspace& out)
{
    Reader reader(bytes);
    const std::uint8_t version = reader.u8();
    if (!reader.ok || version != workspaceVersion)
        return false;
    out.name = readString(reader);
    out.activeWindow = static_cast<int>(reader.u32());
    const std::uint32_t windowCount = reader.u32();
    if (!reader.ok)
        return false;
    // A count can't exceed the bytes left (each window/tab consumes ≥1 byte),
    // so cap the allocation against the buffer — an attacker-chosen 4-billion
    // count must reject, not try to allocate (and -fno-exceptions would abort).
    if (windowCount > reader.remaining())
        return false;
    out.windows.clear();
    out.windows.resize(windowCount);
    for (std::uint32_t w = 0; w < windowCount; ++w) {
        const std::uint32_t activeTab = reader.u32();
        const std::uint32_t zoomedPane = reader.u32();
        const std::uint32_t tabCount = reader.u32();
        if (!reader.ok || tabCount > reader.remaining())
            return false;
        out.windows[w].activeTab = static_cast<int>(activeTab);
        out.windows[w].zoomedPaneIndex = zoomedPane == 0 ? -1 : static_cast<int>(zoomedPane - 1);
        out.windows[w].tabs.resize(tabCount);
        for (std::uint32_t t = 0; t < tabCount; ++t) {
            out.windows[w].tabs[t].title = readString(reader);
            out.windows[w].tabs[t].focusedPaneIndex = static_cast<int>(reader.u32());
            if (!reader.ok)
                return false;
            if (!deserializePane(reader, out.windows[w].tabs[t].root, 0))
                return false;
        }
    }
    return reader.ok && reader.remaining() == 0; // reject trailing garbage
}

std::string serializeWorkspaceText(const Workspace& workspace)
{
    std::vector<std::uint8_t> bytes;
    serializeWorkspace(workspace, bytes);
    std::string text(Codec::base64EncodedLength(bytes.size()), '\0');
    // The codec treats unsigned archive bytes as their character representation.
    const std::span<const char> characters(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto encoded = Codec::base64Encode(characters, text);
    if (!encoded) {
        return {};
    }
    text.resize(encoded->size());
    return text;
}

bool deserializeWorkspaceText(const std::string& text, Workspace& workspace)
{
    constexpr std::size_t maximumTextSize = 4 * 1024 * 1024;
    if (text.empty() || text.size() > maximumTextSize) {
        return false;
    }
    std::vector<char> decoded(text.size());
    const auto archive = Codec::base64Decode(text, decoded);
    if (!archive) {
        return false;
    }
    // The archive reader consumes the same bytes as unsigned octets.
    return deserializeWorkspace({ reinterpret_cast<const std::uint8_t*>(archive->data()), archive->size() }, workspace);
}

bool equalPaneNodes(const PaneNode& a, const PaneNode& b)
{
    if (a.leaf != b.leaf)
        return false;
    if (a.leaf) {
        return a.origin.transport == b.origin.transport && a.origin.host == b.origin.host && a.title == b.title && a.directory == b.directory;
    }
    // Compare the ratio BIT-WISE, not with !=, so a serialized-then-restored
    // value matches itself even when it is NaN (NaN != NaN, but a faithful
    // round trip preserves the exact bits). This is the "same bits" identity a
    // serialization round trip must satisfy.
    if (a.horizontalSplit != b.horizontalSplit || std::memcmp(&a.ratio, &b.ratio, sizeof(double)) != 0)
        return false;
    if (a.children.size() != b.children.size())
        return false;
    for (std::size_t i = 0; i < a.children.size(); ++i)
        if (!equalPaneNodes(a.children[i], b.children[i]))
            return false;
    return true;
}

namespace {

int normalizePane(PaneNode& node)
{
    if (node.leaf)
        return 1;
    if (!std::isfinite(node.ratio) || node.ratio < 0.0 || node.ratio > 1.0)
        node.ratio = std::isfinite(node.ratio) ? (node.ratio < 0.0 ? 0.0 : 1.0) : 0.5;
    int count = 0;
    for (PaneNode& child : node.children) {
        count += normalizePane(child);
    }
    return count;
}

} // namespace

void normalizeWorkspace(Workspace& workspace)
{
    workspace.activeWindow = std::clamp(workspace.activeWindow, 0, std::max(0, static_cast<int>(workspace.windows.size()) - 1));
    for (WorkspaceWindow& window : workspace.windows) {
        const int tabCount = static_cast<int>(window.tabs.size());
        if (window.activeTab < 0)
            window.activeTab = 0;
        else if (window.activeTab >= tabCount)
            window.activeTab = tabCount > 0 ? tabCount - 1 : 0;
        for (std::size_t tabIndex = 0; tabIndex < window.tabs.size(); ++tabIndex) {
            WorkspaceTab& tab = window.tabs[tabIndex];
            const int count = normalizePane(tab.root);
            tab.focusedPaneIndex = std::clamp(tab.focusedPaneIndex, 0, std::max(0, count - 1));
            if (static_cast<int>(tabIndex) == window.activeTab && (count <= 1 || window.zoomedPaneIndex >= count || window.zoomedPaneIndex < -1)) {
                window.zoomedPaneIndex = -1;
            }
        }
        if (window.tabs.empty()) {
            window.zoomedPaneIndex = -1;
        }
    }
}

bool equalWorkspaces(const Workspace& a, const Workspace& b)
{
    if (a.name != b.name || a.activeWindow != b.activeWindow || a.windows.size() != b.windows.size())
        return false;
    for (std::size_t w = 0; w < a.windows.size(); ++w) {
        const WorkspaceWindow& wa = a.windows[w];
        const WorkspaceWindow& wb = b.windows[w];
        if (wa.activeTab != wb.activeTab || wa.zoomedPaneIndex != wb.zoomedPaneIndex || wa.tabs.size() != wb.tabs.size())
            return false;
        for (std::size_t t = 0; t < wa.tabs.size(); ++t) {
            if (wa.tabs[t].title != wb.tabs[t].title || wa.tabs[t].focusedPaneIndex != wb.tabs[t].focusedPaneIndex)
                return false;
            if (!equalPaneNodes(wa.tabs[t].root, wb.tabs[t].root))
                return false;
        }
    }
    return true;
}

} // namespace Ubiquitty::Session
