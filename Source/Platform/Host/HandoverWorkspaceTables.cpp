// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/SessionTreeWire.h"
#include "Platform/Host/HandoverTables.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>

namespace Ubiquitty::Host {
namespace {

namespace Sync = Protocol::Sync;
namespace Layout = Ubiquitty::Session::Layout;
namespace Identity = Ubiquitty::Session::Identity;
using Error = Sync::HandoverBodyError;
using Reader = Sync::WireReader;
constexpr std::uint16_t workspaceVersion = 2;
constexpr std::uint32_t workspaceMagic = 0x57544248;

class Writer {
public:
    explicit Writer(std::size_t limit, std::span<std::uint8_t> output = {})
        : m_limit(limit)
        , m_output(output)
    {
    }
    void fail() { m_didOverflow = true; }
    bool ok() const { return !m_didOverflow; }
    std::size_t size() const { return m_size; }
    void raw(std::span<const std::uint8_t> bytes)
    {
        if (!ok() || bytes.size() > m_limit - m_size) {
            fail();
            return;
        }
        if (!m_output.empty() && !bytes.empty())
            std::memcpy(m_output.data() + m_size, bytes.data(), bytes.size());
        m_size += bytes.size();
    }
    template<typename T> void scalar(T value)
    {
        static_assert(std::is_unsigned_v<T>);
        std::array<std::uint8_t, sizeof(T)> bytes {};
        for (std::size_t index = 0; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
        raw(bytes);
    }
    void integer(int value) { scalar(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(value))); }
    void flag(bool isSet) { scalar(static_cast<std::uint8_t>(isSet)); } // bool-ok: Encodes a stored Boolean field.
    void count(std::size_t size)
    {
        if (size > UINT32_MAX)
            fail();
        scalar(static_cast<std::uint32_t>(size));
    }
    void blob(std::span<const std::uint8_t> bytes)
    {
        count(bytes.size());
        raw(bytes);
    }
    void text(std::string_view text) { blob({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }); }

private:
    std::size_t m_limit;
    std::span<std::uint8_t> m_output;
    std::size_t m_size = 0;
    bool m_didOverflow = false;
};

struct Budget {
    std::size_t workspaces;
    std::size_t windows;
    std::size_t tabs;
    std::size_t panes;
    std::size_t bindings;
    std::size_t structures;
    std::size_t text;
    std::size_t records;
    bool didExceed = false;
};

std::size_t count(Reader& reader, std::size_t maximum, std::size_t minBytes, Budget& budget)
{
    const auto value = reader.u32();
    if (value > maximum) {
        budget.didExceed = true;
        reader.fail();
        return 0;
    }
    if (minBytes && value > reader.remaining() / minBytes) {
        reader.fail();
        return 0;
    }
    return value;
}

std::size_t debit(Reader& reader, std::size_t& remaining, std::size_t maximum, std::size_t minBytes, Budget& budget)
{
    const auto value = count(reader, std::min(remaining, maximum), minBytes, budget);
    remaining -= value;
    return value;
}

bool flag(Reader& reader)
{
    const auto value = reader.u8();
    if (value > 1)
        reader.fail();
    return value != 0;
}

int integer(Reader& reader) { return std::bit_cast<std::int32_t>(reader.u32()); }

std::string_view text(Reader& reader, Budget& budget, std::size_t maximum = UINT32_MAX)
{
    const auto size = debit(reader, budget.text, maximum, 1, budget);
    const auto bytes = reader.take(size);
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

std::span<const std::uint8_t> blob(Reader& reader, Budget& budget)
{
    return reader.take(debit(reader, budget.records, UINT32_MAX, 1, budget));
}

void writeStrings(Writer& writer, const std::vector<std::string>& strings)
{
    writer.count(strings.size());
    for (const auto& string : strings)
        writer.text(string);
}

void readStrings(Reader& reader, Budget& budget, std::vector<std::string>* target, std::size_t maximum)
{
    const auto size = count(reader, maximum, 4, budget);
    for (std::size_t index = 0; index < size && reader.ok(); ++index) {
        const auto value = text(reader, budget);
        if (target)
            target->emplace_back(value);
    }
}

/// Writes the daemon instances that held a workspace, newest first, each
/// with the id it held the workspace under.
void writeHolders(Writer& writer, std::span<const Ubiquitty::Session::HostLayoutHolder> holders)
{
    writer.count(holders.size());
    for (const auto& holder : holders) {
        writer.scalar(holder.daemonInstanceID);
        writer.scalar(holder.workspaceID);
    }
}

/// Reads what `writeHolders` wrote, refusing more than a record names and a
/// zero or repeated instance.
void readHolders(Reader& reader, Budget& budget, std::vector<Ubiquitty::Session::HostLayoutHolder>* target)
{
    using Ubiquitty::Session::HostLayoutHolder;
    const auto holders = count(reader, Ubiquitty::Session::maxHostLayoutHolders, 12, budget);
    for (std::size_t index = 0; index < holders && reader.ok(); ++index) {
        HostLayoutHolder holder { .daemonInstanceID = reader.u64(), .workspaceID = reader.u32() };
        if (!holder.daemonInstanceID
            || (target && std::ranges::find(*target, holder.daemonInstanceID, &HostLayoutHolder::daemonInstanceID) != target->end()))
            reader.fail();
        if (target)
            target->push_back(holder);
    }
}

void writeStringLists(Writer& writer, const std::vector<std::vector<std::string>>& lists)
{
    writer.count(lists.size());
    for (const auto& list : lists)
        writeStrings(writer, list);
}

void readStringLists(Reader& reader, Budget& budget, std::vector<std::vector<std::string>>* target, std::size_t maximum)
{
    const auto size = count(reader, maximum, 4, budget);
    for (std::size_t index = 0; index < size && reader.ok(); ++index) {
        if (target)
            target->emplace_back();
        readStrings(reader, budget, target ? &target->back() : nullptr, Identity::maxTagsPerObject);
    }
}

void writeRecipeNode(Writer& writer, const Ubiquitty::Session::PaneNode& node, unsigned depth)
{
    if (depth >= Panes::maxPanes || !writer.ok()) {
        writer.fail();
        return;
    }
    writer.flag(node.leaf);
    writer.scalar(std::to_underlying(node.origin.transport));
    writer.text(node.origin.host);
    writer.text(node.title);
    writer.text(node.directory);
    writer.flag(node.horizontalSplit);
    writer.scalar(std::bit_cast<std::uint64_t>(node.ratio));
    writer.count(node.children.size());
    for (const auto& child : node.children)
        writeRecipeNode(writer, child, depth + 1);
}

std::size_t readRecipeNode(Reader& reader, Budget& budget, Ubiquitty::Session::PaneNode* target, unsigned depth)
{
    if (depth >= Panes::maxPanes || !reader.ok()) {
        reader.fail();
        return 0;
    }
    const bool isLeaf = flag(reader);
    const auto transport = reader.u8();
    const auto host = text(reader, budget);
    const auto title = text(reader, budget);
    const auto directory = text(reader, budget);
    const bool isHorizontal = flag(reader);
    const auto ratio = std::bit_cast<double>(reader.u64());
    const auto children = count(reader, 2, 1, budget);
    if (transport > std::to_underlying(Transport::OriginKind::Daemon) || !std::isfinite(ratio)
        || (isLeaf ? children != 0 : children != 2 || ratio <= 0 || ratio >= 1))
        reader.fail();
    if (target) {
        target->leaf = isLeaf;
        target->origin = { static_cast<Transport::OriginKind>(transport), std::string(host) };
        target->title = title;
        target->directory = directory;
        target->horizontalSplit = isHorizontal;
        target->ratio = ratio;
    }
    if (isLeaf) {
        if (!budget.panes) {
            budget.didExceed = true;
            reader.fail();
        } else
            --budget.panes;
        return 1;
    }
    std::size_t leaves = 0;
    for (std::size_t index = 0; index < children && reader.ok(); ++index) {
        if (target)
            target->children.emplace_back();
        leaves += readRecipeNode(reader, budget, target ? &target->children.back() : nullptr, depth + 1);
    }
    if (leaves > Panes::maxPanes)
        reader.fail();
    return leaves;
}

void writeRecipe(Writer& writer, const Ubiquitty::Session::HostLayoutRecord& record)
{
    writer.text(record.arrangement.name);
    writer.integer(record.arrangement.activeWindow);
    writer.count(record.arrangement.windows.size());
    for (const auto& window : record.arrangement.windows) {
        writer.integer(window.activeTab);
        writer.integer(window.zoomedPaneIndex);
        writer.count(window.tabs.size());
        for (const auto& tab : window.tabs) {
            writer.text(tab.title);
            writer.integer(tab.focusedPaneIndex);
            writeRecipeNode(writer, tab.root, 0);
        }
    }
    writer.text(record.workspaceLabel);
    writeStrings(writer, record.workspaceTags);
    writer.scalar(record.workspaceID);
    writeHolders(writer, record.holders);
    writer.scalar(record.heldUnixSeconds);
    writeStrings(writer, record.windowLabels);
    writeStrings(writer, record.tabLabels);
    writeStringLists(writer, record.windowTags);
    writeStringLists(writer, record.tabTags);
    writer.count(record.panes.size());
    for (const auto& pane : record.panes) {
        writer.scalar(pane.columns);
        writer.scalar(pane.rows);
        writer.text(pane.label);
        writer.text(pane.sessionLabel);
        writeStrings(writer, pane.tags);
        writeStrings(writer, pane.sessionTags);
    }
}

void readRecipe(Reader& reader, Budget& budget, Ubiquitty::Session::HostLayoutRecord* target)
{
    const auto name = text(reader, budget);
    const auto activeWindow = integer(reader);
    const auto windows = debit(reader, budget.windows, Layout::maxSurfaces, 12, budget);
    if (!windows || activeWindow < 0 || static_cast<std::size_t>(activeWindow) >= windows)
        reader.fail();
    if (target) {
        target->arrangement.name = name;
        target->arrangement.activeWindow = activeWindow;
    }
    std::size_t allTabs = 0;
    std::size_t allPanes = 0;
    for (std::size_t index = 0; index < windows && reader.ok(); ++index) {
        const auto activeTab = integer(reader);
        const auto zoom = integer(reader);
        const auto tabs = debit(reader, budget.tabs, Layout::maxTabsPerSurface, 8, budget);
        allTabs += tabs;
        if (!tabs || activeTab < 0 || static_cast<std::size_t>(activeTab) >= tabs || zoom < -1)
            reader.fail();
        Ubiquitty::Session::WorkspaceWindow* window = nullptr;
        if (target) {
            target->arrangement.windows.emplace_back();
            window = &target->arrangement.windows.back();
            window->activeTab = activeTab;
            window->zoomedPaneIndex = zoom;
        }
        for (std::size_t tabIndex = 0; tabIndex < tabs && reader.ok(); ++tabIndex) {
            const auto title = text(reader, budget);
            const auto focus = integer(reader);
            Ubiquitty::Session::WorkspaceTab* tab = nullptr;
            if (window) {
                window->tabs.emplace_back();
                tab = &window->tabs.back();
                tab->title = title;
                tab->focusedPaneIndex = focus;
            }
            const auto leaves = readRecipeNode(reader, budget, tab ? &tab->root : nullptr, 0);
            allPanes += leaves;
            if (focus < 0 || static_cast<std::size_t>(focus) >= leaves
                || (static_cast<int>(tabIndex) == activeTab && zoom >= 0 && static_cast<std::size_t>(zoom) >= leaves))
                reader.fail();
        }
    }
    const auto label = text(reader, budget);
    if (target)
        target->workspaceLabel = label;
    readStrings(reader, budget, target ? &target->workspaceTags : nullptr, Identity::maxTagsPerObject);
    const auto recordedWorkspaceID = reader.u32();
    // A recipe is found by its name or its id, so it carries one of them.
    if (name.empty() && !recordedWorkspaceID)
        reader.fail();
    if (target)
        target->workspaceID = recordedWorkspaceID;
    readHolders(reader, budget, target ? &target->holders : nullptr);
    const auto recordedHeldUnixSeconds = reader.u64();
    if (target)
        target->heldUnixSeconds = recordedHeldUnixSeconds;
    readStrings(reader, budget, target ? &target->windowLabels : nullptr, windows);
    readStrings(reader, budget, target ? &target->tabLabels : nullptr, allTabs);
    readStringLists(reader, budget, target ? &target->windowTags : nullptr, windows);
    readStringLists(reader, budget, target ? &target->tabTags : nullptr, allTabs);
    const auto panes = count(reader, allPanes, 12, budget);
    for (std::size_t index = 0; index < panes && reader.ok(); ++index) {
        const auto columns = reader.u16();
        const auto rows = reader.u16();
        const auto paneLabel = text(reader, budget);
        const auto sessionLabel = text(reader, budget);
        Ubiquitty::Session::HostLayoutPane* pane = nullptr;
        if (target) {
            target->panes.emplace_back();
            pane = &target->panes.back();
            pane->columns = columns;
            pane->rows = rows;
            pane->label = paneLabel;
            pane->sessionLabel = sessionLabel;
        }
        readStrings(reader, budget, pane ? &pane->tags : nullptr, Identity::maxTagsPerObject);
        readStrings(reader, budget, pane ? &pane->sessionTags : nullptr, Identity::maxTagsPerObject);
    }
}

void writeTree(Writer& writer, const Panes::Tree::Continuation& tree)
{
    writer.integer(tree.root);
    writer.integer(tree.focused);
    writer.integer(tree.paneCount);
    writer.integer(tree.nodeCount);
    for (int index = 0; index < Panes::maxNodes; ++index) {
        writer.flag(tree.used[index]);
        if (!tree.used[index])
            continue;
        const auto& node = tree.nodes[index];
        writer.integer(node.parent);
        writer.integer(node.first);
        writer.integer(node.second);
        writer.integer(node.paneID);
        writer.scalar(std::to_underlying(node.orientation));
        writer.scalar(std::bit_cast<std::uint32_t>(node.ratio));
    }
}

void readTree(Reader& reader, Budget& budget, Panes::Tree::Continuation* target)
{
    Panes::Tree::Continuation tree;
    tree.root = integer(reader);
    tree.focused = integer(reader);
    tree.paneCount = integer(reader);
    tree.nodeCount = integer(reader);
    for (int index = 0; index < Panes::maxNodes && reader.ok(); ++index) {
        tree.used[index] = flag(reader);
        if (!tree.used[index])
            continue;
        auto& node = tree.nodes[index];
        node.parent = integer(reader);
        node.first = integer(reader);
        node.second = integer(reader);
        node.paneID = integer(reader);
        node.orientation = static_cast<Panes::Orientation>(reader.u8());
        node.ratio = std::bit_cast<float>(reader.u32());
        if (node.first == Panes::invalidNode && node.paneID <= 0)
            reader.fail();
    }
    Panes::Tree candidate;
    if (!candidate.restoreContinuation(tree))
        reader.fail();
    if (tree.paneCount < 0 || static_cast<std::size_t>(tree.paneCount) > budget.panes) {
        budget.didExceed = true;
        reader.fail();
    } else
        budget.panes -= static_cast<std::size_t>(tree.paneCount);
    if (target)
        *target = tree;
}

void writeTables(Writer& writer, const HandoverWorkspaceTables& state)
{
    writer.scalar(workspaceVersion);
    writer.scalar(workspaceMagic);
    writer.scalar(state.identities.ceiling);
    for (const auto next : state.identities.next)
        writer.scalar(next);
    writer.scalar(state.nextSessionID);
    writer.scalar(state.nextClientID);
    writer.scalar(state.nextStructureConsentID);
    writer.count(state.workspaces.size());
    for (const auto& workspace : state.workspaces) {
        writer.scalar(workspace.id);
        writer.text(workspace.name);
        writer.flag(workspace.shouldEndWhenDetached);
        writer.flag(workspace.isClosing);
        writer.flag(workspace.wasRestoredAfterHostRestart);
        writeHolders(writer, workspace.formerHolders);
        writer.scalar(workspace.heldUnixSeconds);
        writer.flag(workspace.recordedLayout.has_value());
        if (workspace.recordedLayout)
            writeRecipe(writer, *workspace.recordedLayout);
        writer.scalar(workspace.activeWindow);
        writer.count(workspace.windows.size());
        for (const auto& window : workspace.windows) {
            writer.scalar(window.id);
            writer.scalar(window.activeTab);
            writer.scalar(window.zoomedPane);
            writer.count(window.tabs.size());
            for (const auto& tab : window.tabs) {
                writer.scalar(tab.id);
                writer.text(tab.title);
                writeTree(writer, tab.panes);
            }
        }
        writer.count(workspace.paneSessions.size());
        for (const auto& [pane, session] : workspace.paneSessions) {
            writer.scalar(pane);
            writer.scalar(session);
        }
    }
    writer.count(state.restorable.size());
    for (const auto& workspace : state.restorable) {
        writer.scalar(workspace.id);
        writeRecipe(writer, workspace.record);
    }
    writer.count(state.structures.size());
    for (const auto& structure : state.structures) {
        writer.scalar(structure.workspaceID);
        writer.scalar(structure.revision);
        writer.blob(structure.unrevised);
        writer.flag(structure.record.has_value());
        if (structure.record)
            writer.blob(*structure.record);
    }
    writer.count(state.labels.size());
    for (const auto& [id, label] : state.labels) {
        writer.scalar(std::to_underlying(id.kind));
        writer.scalar(id.number);
        writer.text(label);
    }
    writer.count(state.tags.size());
    for (const auto& [id, tags] : state.tags) {
        writer.scalar(std::to_underlying(id.kind));
        writer.scalar(id.number);
        writeStrings(writer, tags);
    }
    writer.text(state.layoutRecordPath);
    writer.scalar(state.layoutRecordDueMilliseconds);
    writer.scalar(state.layoutRecordSignMilliseconds);
    writer.blob(state.writtenLayoutRecords);
    writer.flag(state.isLayoutRecordKept);
    writer.flag(state.isStructureDirty);
}

std::expected<void, Error> readTables(std::span<const std::uint8_t> bytes, const HandoverWorkspaceLimits& limits, HandoverWorkspaceTables* target)
{
    if (bytes.size() > limits.maxSectionBytes)
        return std::unexpected(Error::OverCapacity);
    Reader reader(bytes);
    const auto version = reader.u16();
    if (reader.ok() && version > workspaceVersion)
        return std::unexpected(Error::NewerVersion);
    if (version != workspaceVersion || reader.u32() != workspaceMagic)
        return std::unexpected(Error::Malformed);
    Budget budget { limits.maxWorkspaces, limits.maxWindows, limits.maxTabs, limits.maxPanes, limits.maxPanes,
        limits.maxStructures, limits.maxStringBytes, limits.maxRecordBytes };
    Identity::Allocator::Continuation identities;
    identities.ceiling = reader.u64();
    for (auto& next : identities.next)
        next = reader.u64();
    const auto nextSession = reader.u32();
    const auto nextClient = reader.u32();
    const auto nextConsent = reader.u32();
    if (identities.ceiling != UINT32_MAX)
        reader.fail();
    for (const auto next : identities.next) {
        if (next > static_cast<std::uint64_t>(UINT32_MAX) + 1)
            reader.fail();
    }
    if (target) {
        target->identities = identities;
        target->nextSessionID = nextSession;
        target->nextClientID = nextClient;
        target->nextStructureConsentID = nextConsent;
    }
    const auto workspaces = debit(reader, budget.workspaces, UINT32_MAX, 20, budget);
    for (std::size_t index = 0; index < workspaces && reader.ok(); ++index) {
        const auto id = reader.u32();
        const auto name = text(reader, budget);
        const bool shouldEnd = flag(reader);
        const bool isClosing = flag(reader);
        const bool wasRestored = flag(reader);
        std::vector<Ubiquitty::Session::HostLayoutHolder> formerHolders;
        readHolders(reader, budget, &formerHolders);
        const auto heldUnixSeconds = reader.u64();
        const bool hasRecord = flag(reader);
        HandoverWorkspace* workspace = nullptr;
        if (target) {
            target->workspaces.emplace_back();
            workspace = &target->workspaces.back();
            workspace->formerHolders = std::move(formerHolders);
            workspace->heldUnixSeconds = heldUnixSeconds;
            workspace->id = id;
            workspace->name = name;
            workspace->shouldEndWhenDetached = shouldEnd;
            workspace->isClosing = isClosing;
            workspace->wasRestoredAfterHostRestart = wasRestored;
            if (hasRecord)
                workspace->recordedLayout.emplace();
        }
        if (hasRecord)
            readRecipe(reader, budget, workspace ? &*workspace->recordedLayout : nullptr);
        const auto activeWindow = reader.u32();
        if (workspace)
            workspace->activeWindow = activeWindow;
        const auto windows = debit(reader, budget.windows, Layout::maxSurfaces, 16, budget);
        if (!windows || !id)
            reader.fail();
        for (std::size_t windowIndex = 0; windowIndex < windows && reader.ok(); ++windowIndex) {
            const auto windowID = reader.u32();
            const auto activeTab = reader.u32();
            const auto zoom = reader.u32();
            HandoverWindow* window = nullptr;
            if (workspace) {
                workspace->windows.emplace_back();
                window = &workspace->windows.back();
                window->id = windowID;
                window->activeTab = activeTab;
                window->zoomedPane = zoom;
            }
            const auto tabs = debit(reader, budget.tabs, Layout::maxTabsPerSurface, 8, budget);
            if (!tabs || !windowID)
                reader.fail();
            for (std::size_t tabIndex = 0; tabIndex < tabs && reader.ok(); ++tabIndex) {
                const auto tabID = reader.u32();
                const auto title = text(reader, budget);
                HandoverTab* tab = nullptr;
                if (window) {
                    window->tabs.emplace_back();
                    tab = &window->tabs.back();
                    tab->id = tabID;
                    tab->title = title;
                }
                if (!tabID)
                    reader.fail();
                readTree(reader, budget, tab ? &tab->panes : nullptr);
            }
        }
        const auto bindings = debit(reader, budget.bindings, UINT32_MAX, 8, budget);
        for (std::size_t binding = 0; binding < bindings && reader.ok(); ++binding) {
            const auto pane = reader.u32();
            const auto session = reader.u32();
            if (!pane || !session)
                reader.fail();
            if (workspace)
                workspace->paneSessions.emplace_back(pane, session);
        }
    }
    const auto restorable = debit(reader, budget.workspaces, Ubiquitty::Session::maxHostLayoutRecords, 8, budget);
    for (std::size_t index = 0; index < restorable && reader.ok(); ++index) {
        const auto id = reader.u32();
        if (target) {
            target->restorable.emplace_back();
            target->restorable.back().id = id;
        }
        if (!id)
            reader.fail();
        readRecipe(reader, budget, target ? &target->restorable.back().record : nullptr);
    }
    const auto structures = debit(reader, budget.structures, UINT32_MAX, 17, budget);
    for (std::size_t index = 0; index < structures && reader.ok(); ++index) {
        const auto id = reader.u32();
        const auto revision = reader.u64();
        const auto unrevised = blob(reader, budget);
        const bool hasRecord = flag(reader);
        const auto record = hasRecord ? blob(reader, budget) : std::span<const std::uint8_t> {};
        if (!id || revision == UINT64_MAX || (!hasRecord && !unrevised.empty())
            || (hasRecord && (!revision || record.size() < 8 || record.size() != unrevised.size())))
            reader.fail();
        if (hasRecord && reader.ok()) {
            for (unsigned byte = 0; byte < 8; ++byte) {
                if (unrevised[byte] || record[byte] != static_cast<std::uint8_t>(revision >> (byte * 8)))
                    reader.fail();
            }
            if (!std::ranges::equal(record.subspan(8), unrevised.subspan(8)))
                reader.fail();
        }
        if (target) {
            target->structures.push_back({ id, revision, { unrevised.begin(), unrevised.end() }, std::nullopt });
            if (hasRecord)
                target->structures.back().record.emplace(record.begin(), record.end());
        }
    }
    const auto labels = count(reader, Identity::LabelBook::capacity, 13, budget);
    for (std::size_t index = 0; index < labels && reader.ok(); ++index) {
        const auto kind = reader.u8();
        const auto number = reader.u64();
        const auto label = text(reader, budget, Identity::maxLabelLength);
        if (kind >= Identity::objectKindCount || !number || label.empty() || Identity::checkLabel(label) != Identity::LabelVerdict::Accepted)
            reader.fail();
        if (target)
            target->labels.emplace_back(Identity::ObjectID { static_cast<Identity::ObjectKind>(kind), number }, label);
    }
    const auto tags = count(reader, Identity::TagBook::capacity, 13, budget);
    for (std::size_t index = 0; index < tags && reader.ok(); ++index) {
        const auto kind = reader.u8();
        const auto number = reader.u64();
        if (kind >= Identity::objectKindCount || !number)
            reader.fail();
        if (target)
            target->tags.emplace_back(Identity::ObjectID { static_cast<Identity::ObjectKind>(kind), number }, std::vector<std::string> {});
        readStrings(reader, budget, target ? &target->tags.back().second : nullptr, Identity::maxTagsPerObject);
    }
    const auto path = text(reader, budget);
    const auto due = reader.u64();
    const auto sign = reader.u64();
    const auto written = blob(reader, budget);
    const bool isKept = flag(reader);
    const bool isDirty = flag(reader);
    if (target) {
        target->layoutRecordPath = path;
        target->layoutRecordDueMilliseconds = due;
        target->layoutRecordSignMilliseconds = sign;
        target->writtenLayoutRecords.assign(written.begin(), written.end());
        target->isLayoutRecordKept = isKept;
        target->isStructureDirty = isDirty;
    }
    const auto digestOffset = reader.offset();
    const Sync::Digest128 digest { reader.u64(), reader.u64() };
    if (budget.didExceed)
        return std::unexpected(Error::OverCapacity);
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(Error::Malformed);
    if (Sync::digestOf(bytes.first(digestOffset)) != digest)
        return std::unexpected(Error::DigestMismatch);
    return {};
}

bool isLayoutValid(const HandoverWorkspace& workspace)
{
    if (workspace.windows.empty() || workspace.windows.size() > Layout::maxSurfaces)
        return false;
    std::vector<std::uint32_t> windows;
    std::vector<std::uint32_t> tabs;
    std::vector<std::uint32_t> panes;
    for (const auto& window : workspace.windows) {
        if (!window.id || std::ranges::find(windows, window.id) != windows.end()
            || window.tabs.empty() || window.tabs.size() > Layout::maxTabsPerSurface)
            return false;
        windows.push_back(window.id);
        bool hasActive = false;
        for (const auto& tab : window.tabs) {
            if (!tab.id || std::ranges::find(tabs, tab.id) != tabs.end())
                return false;
            tabs.push_back(tab.id);
            Panes::Tree tree;
            if (!tree.restoreContinuation(tab.panes))
                return false;
            bool hasUniquePanes = true;
            Panes::forEachLeaf(tree, [&](int pane) {
                if (pane <= 0 || std::ranges::find(panes, static_cast<std::uint32_t>(pane)) != panes.end())
                    hasUniquePanes = false;
                panes.push_back(static_cast<std::uint32_t>(pane));
            });
            if (!hasUniquePanes)
                return false;
            if (tab.id == window.activeTab) {
                hasActive = true;
                if (window.zoomedPane && (tree.paneCount() <= 1 || !tree.holdsPane(static_cast<int>(window.zoomedPane))))
                    return false;
            }
        }
        if (!hasActive)
            return false;
    }
    if (std::ranges::find(windows, workspace.activeWindow) == windows.end() || workspace.paneSessions.size() != panes.size())
        return false;
    for (const auto pane : panes) {
        if (std::ranges::count(workspace.paneSessions, pane, &std::pair<std::uint32_t, std::uint32_t>::first) != 1)
            return false;
    }
    return true;
}

bool referencesAreValid(const HandoverWorkspaceTables& state)
{
    const auto wasIssued = [&](Identity::ObjectID id) {
        if (!id.number || id.number > state.identities.ceiling || std::to_underlying(id.kind) >= Identity::objectKindCount)
            return false;
        auto next = state.identities.next[std::to_underlying(id.kind)];
        if (id.kind == Identity::ObjectKind::Workspace || id.kind == Identity::ObjectKind::Session)
            next = state.nextSessionID;
        else if (id.kind == Identity::ObjectKind::Client)
            next = state.nextClientID;
        return !next || id.number < next;
    };
    std::vector<Identity::ObjectID> seen;
    const auto admit = [&](Identity::ObjectID id) {
        if (!wasIssued(id) || std::ranges::find(seen, id) != seen.end())
            return false;
        seen.push_back(id);
        return true;
    };
    for (const auto& workspace : state.workspaces) {
        if (!admit({ Identity::ObjectKind::Workspace, workspace.id }) || !isLayoutValid(workspace))
            return false;
        for (const auto& window : workspace.windows) {
            if (!admit({ Identity::ObjectKind::Window, window.id }))
                return false;
            for (const auto& tab : window.tabs) {
                if (!admit({ Identity::ObjectKind::Tab, tab.id }))
                    return false;
                for (int index = 0; index < Panes::maxNodes; ++index) {
                    if (tab.panes.used[index] && tab.panes.nodes[index].first == Panes::invalidNode
                        && !admit({ Identity::ObjectKind::Pane, static_cast<std::uint64_t>(tab.panes.nodes[index].paneID) }))
                        return false;
                }
            }
        }
        for (const auto& [pane, session] : workspace.paneSessions) {
            if (!pane || !wasIssued({ Identity::ObjectKind::Session, session }))
                return false;
        }
    }
    for (const auto& workspace : state.restorable) {
        if (!admit({ Identity::ObjectKind::Workspace, workspace.id }))
            return false;
    }
    std::vector<std::uint32_t> structures;
    for (const auto& structure : state.structures) {
        if (!wasIssued({ Identity::ObjectKind::Workspace, structure.workspaceID })
            || std::ranges::find(structures, structure.workspaceID) != structures.end())
            return false;
        structures.push_back(structure.workspaceID);
        if (structure.record) {
            const auto decoded = Ubiquitty::Session::decodeWorkspaceStructure(*structure.record);
            if (!decoded || decoded->id != structure.workspaceID || decoded->revision != structure.revision)
                return false;
        }
    }
    Identity::LabelBook labels;
    for (const auto& [id, label] : state.labels) {
        if (!wasIssued(id) || !labels.label(id).empty() || label.empty() || labels.set(id, label) != Identity::LabelVerdict::Accepted)
            return false;
    }
    Identity::TagBook tags;
    for (const auto& [id, names] : state.tags) {
        if (!wasIssued(id) || !tags.tags(id).empty() || names.empty() || tags.set(id, names) != Identity::TagVerdict::Accepted
            || !std::ranges::equal(tags.tags(id), names))
            return false;
    }
    return true;
}

} // namespace

void captureHandoverLayout(const Layout::Workspace& layout, HandoverWorkspace& target)
{
    target.windows.clear();
    target.activeWindow = std::to_underlying(layout.activeSurface());
    for (int index = 0; index < layout.surfaceCount(); ++index) {
        const auto& source = *layout.surface(layout.surfaceIDAt(index));
        HandoverWindow window { std::to_underlying(source.id), std::to_underlying(source.tabs[source.activeTab].id),
            std::to_underlying(source.zoomedPane), {} };
        for (int tab = 0; tab < source.tabCount; ++tab)
            window.tabs.push_back({ std::to_underlying(source.tabs[tab].id), source.tabs[tab].title, source.tabs[tab].panes.continuation() });
        target.windows.push_back(std::move(window));
    }
}

bool restoreHandoverLayout(const HandoverWorkspace& source, Layout::Workspace& target)
{
    if (!isLayoutValid(source))
        return false;
    std::vector<std::vector<Panes::Tree>> trees(source.windows.size());
    std::vector<std::vector<Layout::AdoptedTab>> tabs(source.windows.size());
    std::vector<Layout::AdoptedSurface> windows;
    for (std::size_t index = 0; index < source.windows.size(); ++index) {
        const auto& window = source.windows[index];
        trees[index].reserve(window.tabs.size());
        for (const auto& tab : window.tabs) {
            trees[index].emplace_back();
            if (!trees[index].back().restoreContinuation(tab.panes))
                return false;
            tabs[index].push_back({ Layout::TabID { tab.id }, &trees[index].back(), tab.title });
        }
        windows.push_back({ Layout::SurfaceID { window.id }, Layout::TabID { window.activeTab }, Layout::PaneID { window.zoomedPane }, tabs[index] });
    }
    return target.adoptWorkspace(windows, Layout::SurfaceID { source.activeWindow });
}

std::expected<Sync::HandoverBytes, Error> encodeHandoverWorkspaces(const HandoverWorkspaceTables& state, const HandoverWorkspaceLimits& limits)
{
    Writer measure(limits.maxSectionBytes);
    writeTables(measure, state);
    measure.scalar<std::uint64_t>(0);
    measure.scalar<std::uint64_t>(0);
    if (!measure.ok())
        return std::unexpected(Error::OverCapacity);
    if (!referencesAreValid(state))
        return std::unexpected(Error::Malformed);
    Sync::HandoverBytes result { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[measure.size()]), measure.size() };
    if (!result.data)
        return std::unexpected(Error::AllocationRefused);
    Writer writer(result.size, { result.data.get(), result.size });
    writeTables(writer, state);
    const auto digest = Sync::digestOf(result.view().first(writer.size()));
    writer.scalar(digest.low);
    writer.scalar(digest.high);
    if (const auto admitted = readTables(result.view(), limits, nullptr); !admitted)
        return std::unexpected(admitted.error());
    return result;
}

std::expected<HandoverWorkspaceTables, Error> decodeHandoverWorkspaces(std::span<const std::uint8_t> bytes, const HandoverWorkspaceLimits& limits)
{
    if (const auto admitted = readTables(bytes, limits, nullptr); !admitted)
        return std::unexpected(admitted.error());
    HandoverWorkspaceTables result;
    if (const auto restored = readTables(bytes, limits, &result); !restored)
        return std::unexpected(restored.error());
    if (!referencesAreValid(result))
        return std::unexpected(Error::Malformed);
    return result;
}

} // namespace Ubiquitty::Host
