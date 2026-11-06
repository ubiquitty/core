// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// HostLayoutRecord.cpp — see HostLayoutRecord.h.
//
// The file: the four bytes "UBQN", the version as a u16, a one-byte record
// count, then each record as a u32 byte length and that many bytes:
//
//   name             string
//   label            string
//   tags             tags
//   workspaceID      u32, the id the writing host listed it under; 0 for none
//   holderCount      u8, then per holder, newest first:
//     instance       u64, a daemon instance that held the workspace live
//     workspaceID    u32, the id it held it under
//   heldUnixSeconds  u64, when a host last held the workspace live
//   activeWindow     u8
//   windowCount      u8, then per window:
//     label          string
//     tags           tags
//     activeTab      u8
//     zoomedPane     u8, the active tab's leaf index; 0xFF for none
//     tabCount       u8, then per tab:
//       title        string
//       label        string
//       tags         tags
//       focusedPane  u8, a leaf index
//       root         node
//
//   node: u8 kind — 0 a leaf, 1 a split side by side, 2 a split stacked
//     split: u16 ratio as a fraction of 65535, then the first and second nodes
//     leaf:  u16 columns, u16 rows, string directory, string remote host
//            (empty for a pane that runs the host's shell), string title,
//            string label, string session label, tags, the session's tags
//
// A string is a u16 byte count and the bytes; tags are a u8 count and that
// many strings. Integers are little-endian.
//
// A field is added by taking the next version. The one reader reads only
// `hostLayoutFileVersion`, and a record's length holds each record to its own
// bytes. No other form is read: a file that opens with anything else is not a
// record file, and one that names another version is refused unread.

#include "Core/Session/HostLayoutRecord.h"

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/WireFormat.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace Ubiquitty::Session {

namespace {

using Protocol::Sync::putU16;
using Protocol::Sync::putU32;
using Protocol::Sync::putU64;
using Protocol::Sync::putU8;
using Reader = Protocol::Sync::Reader;

constexpr std::array<std::uint8_t, 4> fileMagic { 'U', 'B', 'Q', 'N' };

constexpr std::uint8_t noZoomedPane = 0xFF;
constexpr double ratioScale = std::numeric_limits<std::uint16_t>::max();

enum class NodeKind : std::uint8_t {
    Leaf = 0,
    SideBySide = 1,
    Stacked = 2,
};

void putString(std::vector<std::uint8_t>& out, std::string_view text)
{
    const std::size_t length = std::min<std::size_t>(text.size(), std::numeric_limits<std::uint16_t>::max());
    putU16(out, static_cast<std::uint16_t>(length));
    out.insert(out.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(length));
}

/// Reads a string no longer than `maxLength`, latching the reader on a longer one.
std::string readString(Reader& reader, std::size_t maxLength)
{
    const std::uint16_t length = reader.u16();
    if (length > maxLength) {
        reader.ok = false;
        return {};
    }
    const std::span<const std::uint8_t> bytes = reader.bytes(length);
    if (!reader.ok)
        return {};
    return { bytes.begin(), bytes.end() };
}

/// Writes an object's tags: a count, then each name. More than an object
/// carries are written as given, and refused on the way back in.
void putTags(std::vector<std::uint8_t>& out, std::span<const std::string> tags)
{
    const std::span<const std::string> written = tags.first(std::min<std::size_t>(tags.size(), std::numeric_limits<std::uint8_t>::max()));
    putU8(out, static_cast<std::uint8_t>(written.size()));
    for (const std::string& tag : written)
        putString(out, tag);
}

/// Reads an object's tags, latching the reader on more than an object
/// carries or a name longer than a tag's.
std::vector<std::string> readTags(Reader& reader)
{
    std::vector<std::string> tags;
    const std::uint8_t count = reader.u8();
    if (!reader.ok || count > Identity::maxTagsPerObject) {
        reader.ok = false;
        return tags;
    }
    for (std::uint8_t index = 0; index < count && reader.ok; ++index)
        tags.push_back(readString(reader, Identity::maxLabelLength));
    return tags;
}

/// The tags at `index` of a list kept one entry an object, none past its end.
std::span<const std::string> tagsAt(const std::vector<std::vector<std::string>>& list, std::size_t index)
{
    return index < list.size() ? std::span<const std::string>(list[index]) : std::span<const std::string>();
}

std::size_t leafCount(const PaneNode& node)
{
    if (node.leaf)
        return 1;
    std::size_t count = 0;
    for (const PaneNode& child : node.children)
        count += leafCount(child);
    return count;
}

void encodeNode(const PaneNode& node, std::span<const HostLayoutPane> panes, std::size_t& paneIndex, std::vector<std::uint8_t>& out)
{
    if (node.leaf) {
        putU8(out, std::to_underlying(NodeKind::Leaf));
        const HostLayoutPane facts = paneIndex < panes.size() ? panes[paneIndex] : HostLayoutPane {};
        ++paneIndex;
        putU16(out, facts.columns);
        putU16(out, facts.rows);
        putString(out, node.directory);
        putString(out, node.origin.transport == Transport::OriginKind::SSH ? std::string_view(node.origin.host) : std::string_view());
        putString(out, node.title);
        putString(out, facts.label);
        putString(out, facts.sessionLabel);
        putTags(out, facts.tags);
        putTags(out, facts.sessionTags);
        return;
    }
    putU8(out, std::to_underlying(node.horizontalSplit ? NodeKind::Stacked : NodeKind::SideBySide));
    const double ratio = std::isfinite(node.ratio) ? std::clamp(node.ratio, 0.0, 1.0) : 0.5;
    putU16(out, static_cast<std::uint16_t>(std::lround(ratio * ratioScale)));
    for (std::size_t child = 0; child < 2; ++child)
        encodeNode(child < node.children.size() ? node.children[child] : PaneNode {}, panes, paneIndex, out);
}

/// What one record's decode has used of its budgets so far.
struct DecodeBudget {
    std::size_t panes = 0;
};

bool decodeNode(Reader& reader, PaneNode& node, std::vector<HostLayoutPane>& panes, DecodeBudget& budget, std::size_t& tabLeaves, int depth)
{
    if (depth >= Layout::maxPanesPerTab)
        return false;
    const std::uint8_t kind = reader.u8();
    if (!reader.ok)
        return false;
    if (kind == std::to_underlying(NodeKind::Leaf)) {
        if (++budget.panes > maxHostLayoutPanes || ++tabLeaves > static_cast<std::size_t>(Layout::maxPanesPerTab))
            return false;
        HostLayoutPane facts;
        facts.columns = reader.u16();
        facts.rows = reader.u16();
        node.leaf = true;
        node.directory = readString(reader, maxHostLayoutDirectoryLength);
        std::string remoteHost = readString(reader, maxHostLayoutRemoteHostLength);
        node.title = readString(reader, maxHostLayoutTitleLength);
        facts.label = readString(reader, Identity::maxLabelLength);
        facts.sessionLabel = readString(reader, Identity::maxLabelLength);
        facts.tags = readTags(reader);
        facts.sessionTags = readTags(reader);
        if (!reader.ok || !facts.columns || !facts.rows)
            return false;
        node.origin.transport = remoteHost.empty() ? Transport::OriginKind::Local : Transport::OriginKind::SSH;
        node.origin.host = std::move(remoteHost);
        panes.push_back(std::move(facts));
        return true;
    }
    if (kind != std::to_underlying(NodeKind::SideBySide) && kind != std::to_underlying(NodeKind::Stacked))
        return false;
    node.leaf = false;
    node.horizontalSplit = kind == std::to_underlying(NodeKind::Stacked);
    node.ratio = reader.u16() / ratioScale;
    if (!reader.ok)
        return false;
    node.children.resize(2);
    return decodeNode(reader, node.children[0], panes, budget, tabLeaves, depth + 1)
        && decodeNode(reader, node.children[1], panes, budget, tabLeaves, depth + 1);
}

bool decodeRecord(Reader& reader, HostLayoutRecord& record)
{
    record.arrangement.name = readString(reader, Protocol::Sync::maxWorkspaceNameLength);
    record.workspaceLabel = readString(reader, Identity::maxLabelLength);
    record.workspaceTags = readTags(reader);
    record.workspaceID = reader.u32();
    const std::uint8_t holderCount = reader.u8();
    if (!reader.ok || holderCount > maxHostLayoutHolders)
        return false;
    for (std::uint8_t index = 0; index < holderCount; ++index) {
        const HostLayoutHolder holder { .daemonInstanceID = reader.u64(), .workspaceID = reader.u32() };
        if (!reader.ok || !holder.daemonInstanceID
            || std::ranges::find(record.holders, holder.daemonInstanceID, &HostLayoutHolder::daemonInstanceID) != record.holders.end())
            return false;
        record.holders.push_back(holder);
    }
    record.heldUnixSeconds = reader.u64();
    const std::uint8_t activeWindow = reader.u8();
    const std::uint8_t windowCount = reader.u8();
    // A workspace is found by its name or by its id, so a record carries at
    // least one of them.
    if (!reader.ok || (record.arrangement.name.empty() && !record.workspaceID) || windowCount == 0 || windowCount > Layout::maxSurfaces || activeWindow >= windowCount)
        return false;
    record.arrangement.activeWindow = activeWindow;
    DecodeBudget budget;
    for (std::uint8_t windowIndex = 0; windowIndex < windowCount; ++windowIndex) {
        WorkspaceWindow window;
        record.windowLabels.push_back(readString(reader, Identity::maxLabelLength));
        record.windowTags.push_back(readTags(reader));
        const std::uint8_t activeTab = reader.u8();
        const std::uint8_t zoomedPane = reader.u8();
        const std::uint8_t tabCount = reader.u8();
        if (!reader.ok || tabCount == 0 || tabCount > Layout::maxTabsPerSurface || activeTab >= tabCount)
            return false;
        window.activeTab = activeTab;
        for (std::uint8_t tabIndex = 0; tabIndex < tabCount; ++tabIndex) {
            WorkspaceTab tab;
            tab.title = readString(reader, maxHostLayoutTitleLength);
            record.tabLabels.push_back(readString(reader, Identity::maxLabelLength));
            record.tabTags.push_back(readTags(reader));
            const std::uint8_t focusedPane = reader.u8();
            std::size_t tabLeaves = 0;
            if (!reader.ok || !decodeNode(reader, tab.root, record.panes, budget, tabLeaves, 0) || focusedPane >= tabLeaves)
                return false;
            tab.focusedPaneIndex = focusedPane;
            if (tabIndex == activeTab && zoomedPane != noZoomedPane && zoomedPane >= tabLeaves)
                return false;
            window.tabs.push_back(std::move(tab));
        }
        window.zoomedPaneIndex = zoomedPane == noZoomedPane ? -1 : zoomedPane;
        record.arrangement.windows.push_back(std::move(window));
    }
    return true;
}

/// Whether an earlier record of the file names the same workspace.
bool isWorkspaceRepeated(const std::vector<HostLayoutRecord>& records, const HostLayoutRecord& record)
{
    return std::ranges::any_of(records, [&](const HostLayoutRecord& earlier) { return isSameRecordedWorkspace(earlier, record); });
}

} // namespace

std::size_t hostLayoutPaneCount(const Workspace& arrangement)
{
    std::size_t count = 0;
    for (const WorkspaceWindow& window : arrangement.windows) {
        for (const WorkspaceTab& tab : window.tabs)
            count += leafCount(tab.root);
    }
    return count;
}

bool isRecordOfReference(const HostLayoutRecord& record, std::uint64_t daemonInstanceID, std::uint32_t workspaceID)
{
    const HostLayoutHolder reference { .daemonInstanceID = daemonInstanceID, .workspaceID = workspaceID };
    return daemonInstanceID && workspaceID && std::ranges::find(record.holders, reference) != record.holders.end();
}

std::vector<HostLayoutHolder> holdersAfter(std::span<const HostLayoutHolder> former, HostLayoutHolder holder)
{
    std::vector<HostLayoutHolder> holders;
    holders.reserve(maxHostLayoutHolders);
    if (holder.daemonInstanceID)
        holders.push_back(holder);
    for (const HostLayoutHolder& earlier : former) {
        if (holders.size() >= maxHostLayoutHolders)
            break;
        if (earlier.daemonInstanceID
            && std::ranges::find(holders, earlier.daemonInstanceID, &HostLayoutHolder::daemonInstanceID) == holders.end())
            holders.push_back(earlier);
    }
    return holders;
}

bool isSameRecordedWorkspace(const HostLayoutRecord& a, const HostLayoutRecord& b)
{
    return (a.workspaceID && a.workspaceID == b.workspaceID)
        || (!a.arrangement.name.empty() && a.arrangement.name == b.arrangement.name);
}

void orderByRecentHold(std::span<HostLayoutRecord> records)
{
    std::ranges::stable_sort(records, [](const HostLayoutRecord& a, const HostLayoutRecord& b) { return a.heldUnixSeconds > b.heldUnixSeconds; });
}

void encodeHostLayoutRecords(std::span<const HostLayoutRecord> records, std::vector<std::uint8_t>& out)
{
    out.insert(out.end(), fileMagic.begin(), fileMagic.end());
    putU16(out, hostLayoutFileVersion);
    const std::size_t count = std::min(records.size(), maxHostLayoutRecords);
    putU8(out, static_cast<std::uint8_t>(count));
    std::vector<std::uint8_t> body;
    for (const HostLayoutRecord& record : records.first(count)) {
        body.clear();
        const Workspace& arrangement = record.arrangement;
        putString(body, arrangement.name);
        putString(body, record.workspaceLabel);
        putTags(body, record.workspaceTags);
        putU32(body, record.workspaceID);
        const std::size_t holderCount = std::min(record.holders.size(), maxHostLayoutHolders);
        putU8(body, static_cast<std::uint8_t>(holderCount));
        for (const HostLayoutHolder& holder : std::span(record.holders).first(holderCount)) {
            putU64(body, holder.daemonInstanceID);
            putU32(body, holder.workspaceID);
        }
        putU64(body, record.heldUnixSeconds);
        putU8(body, static_cast<std::uint8_t>(std::max(arrangement.activeWindow, 0)));
        putU8(body, static_cast<std::uint8_t>(arrangement.windows.size()));
        std::size_t paneIndex = 0;
        std::size_t tabIndex = 0;
        for (std::size_t windowIndex = 0; windowIndex < arrangement.windows.size(); ++windowIndex) {
            const WorkspaceWindow& window = arrangement.windows[windowIndex];
            putString(body, windowIndex < record.windowLabels.size() ? std::string_view(record.windowLabels[windowIndex]) : std::string_view());
            putTags(body, tagsAt(record.windowTags, windowIndex));
            putU8(body, static_cast<std::uint8_t>(std::max(window.activeTab, 0)));
            putU8(body, window.zoomedPaneIndex < 0 ? noZoomedPane : static_cast<std::uint8_t>(window.zoomedPaneIndex));
            putU8(body, static_cast<std::uint8_t>(window.tabs.size()));
            for (const WorkspaceTab& tab : window.tabs) {
                putString(body, tab.title);
                putString(body, tabIndex < record.tabLabels.size() ? std::string_view(record.tabLabels[tabIndex]) : std::string_view());
                putTags(body, tagsAt(record.tabTags, tabIndex));
                ++tabIndex;
                putU8(body, static_cast<std::uint8_t>(std::max(tab.focusedPaneIndex, 0)));
                encodeNode(tab.root, record.panes, paneIndex, body);
            }
        }
        putU32(out, static_cast<std::uint32_t>(std::min<std::size_t>(body.size(), std::numeric_limits<std::uint32_t>::max())));
        out.insert(out.end(), body.begin(), body.end());
    }
}

HostLayoutFile readHostLayoutFile(std::span<const std::uint8_t> bytes)
{
    HostLayoutFile file;
    if (bytes.size() < fileMagic.size())
        return file;
    if (!std::equal(fileMagic.begin(), fileMagic.end(), bytes.begin()))
        return file;
    file.verdict = HostLayoutFileVerdict::Damaged;
    Reader reader(bytes.subspan(fileMagic.size()));
    file.version = reader.u16();
    if (!reader.ok || file.version == 0)
        return file;
    // Said before anything else is read: what follows the version is laid
    // out as that version lays it out, which this build does not know.
    if (file.version > hostLayoutFileVersion) {
        file.verdict = HostLayoutFileVerdict::NewerVersion;
        return file;
    }
    if (file.version < hostLayoutFileVersion) {
        file.verdict = HostLayoutFileVerdict::OlderVersion;
        return file;
    }
    if (bytes.size() > maxHostLayoutFileBytes)
        return file;
    const std::uint8_t count = reader.u8();
    if (!reader.ok || count > maxHostLayoutRecords)
        return file;
    std::vector<HostLayoutRecord> records;
    records.reserve(count);
    for (std::uint8_t index = 0; index < count; ++index) {
        const std::uint32_t length = reader.u32();
        if (!reader.ok || length > reader.remaining())
            return file;
        // A record is read from its own bytes and must use every one.
        Reader body(reader.bytes(length));
        HostLayoutRecord record;
        if (!decodeRecord(body, record) || !body.ok || body.remaining() != 0 || isWorkspaceRepeated(records, record))
            return file;
        records.push_back(std::move(record));
    }
    if (!reader.ok || reader.remaining() != 0)
        return file;
    file.verdict = HostLayoutFileVerdict::Read;
    file.records = std::move(records);
    return file;
}

std::string describeUnreadHostLayoutFile(const HostLayoutFile& file)
{
    switch (file.verdict) {
    case HostLayoutFileVerdict::Read:
        return {};
    case HostLayoutFileVerdict::NotARecordFile:
        return "it is not a layout record file";
    case HostLayoutFileVerdict::NewerVersion:
        return "it was written as version " + std::to_string(file.version) + " and this build reads up to version " + std::to_string(hostLayoutFileVersion);
    case HostLayoutFileVerdict::OlderVersion:
        return "it was written as version " + std::to_string(file.version) + " and this build reads only version " + std::to_string(hostLayoutFileVersion);
    case HostLayoutFileVerdict::Damaged:
        return "its records do not read whole";
    }
    return "it could not be read";
}

std::optional<std::vector<HostLayoutRecord>> decodeHostLayoutRecords(std::span<const std::uint8_t> bytes)
{
    HostLayoutFile file = readHostLayoutFile(bytes);
    if (file.verdict != HostLayoutFileVerdict::Read)
        return std::nullopt;
    return std::move(file.records);
}

} // namespace Ubiquitty::Session
