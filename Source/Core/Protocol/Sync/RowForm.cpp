// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/RowForm.h"

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellContent.h"
#include "Core/ClusterPool.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Terminal.h"

#include <cstring>
#include <limits>

namespace Ubiquitty::Protocol::Sync {

namespace {

void stagePen(std::uint8_t* at, const Pen& pen)
{
    const std::uint16_t attributes = pen.attributes.toRaw();
    at[0] = static_cast<std::uint8_t>(attributes & 0xFF);
    at[1] = static_cast<std::uint8_t>(attributes >> 8);
    at[2] = static_cast<std::uint8_t>(pen.foregroundIndex & 0xFF);
    at[3] = static_cast<std::uint8_t>(pen.foregroundIndex >> 8);
    at[4] = static_cast<std::uint8_t>(pen.backgroundIndex & 0xFF);
    at[5] = static_cast<std::uint8_t>(pen.backgroundIndex >> 8);
    const auto underline = static_cast<std::uint16_t>(pen.underlineIndex | (static_cast<std::uint16_t>(pen.semanticRole) << 9)
        | (static_cast<std::uint16_t>(pen.isForegroundAssignedDefault) << 11) | (static_cast<std::uint16_t>(pen.isBackgroundAssignedDefault) << 12));
    at[6] = static_cast<std::uint8_t>(underline & 0xFF);
    at[7] = static_cast<std::uint8_t>(underline >> 8);
    at[8] = pen.foregroundRed;
    at[9] = pen.foregroundGreen;
    at[10] = pen.foregroundBlue;
    at[11] = pen.backgroundRed;
    at[12] = pen.backgroundGreen;
    at[13] = pen.backgroundBlue;
    at[14] = pen.underlineRed;
    at[15] = pen.underlineGreen;
    at[16] = pen.underlineBlue;
    at[17] = pen.underlineStyle;
    at[18] = pen.extendedAttributes.toRaw();
    at[19] = pen.font;
}

std::uint64_t clusterEntryHash(const ClusterPool::Entry& entry)
{
    std::array<std::uint8_t, 1 + ClusterPool::maxCodePoints * 4> bytes {};
    std::size_t length = 0;
    bytes[length++] = entry.length;
    for (int index = 0; index < entry.length; ++index) {
        const std::uint32_t codePoint = entry.codePoints[static_cast<std::size_t>(index)];
        for (int shift = 0; shift < 32; shift += 8)
            bytes[length++] = static_cast<std::uint8_t>(codePoint >> shift);
    }
    return blockHash({ bytes.data(), length });
}

} // namespace

std::array<std::uint8_t, penWireBytes> encodePen(const Pen& pen)
{
    std::array<std::uint8_t, penWireBytes> bytes {};
    stagePen(bytes.data(), pen);
    return bytes;
}

void writePen(WireWriter& writer, const Pen& pen)
{
    writer.raw(encodePen(pen));
}

Pen readPen(WireReader& reader)
{
    Pen pen {};
    pen.attributes = Ubiquitty::VT::AttributeSet::fromRaw(reader.u16());
    pen.foregroundIndex = reader.u16();
    pen.backgroundIndex = reader.u16();
    const std::uint16_t underline = reader.u16();
    pen.underlineIndex = underline & 0x1FF;
    pen.semanticRole = static_cast<SemanticRole>((underline >> 9) & 3);
    pen.isForegroundAssignedDefault = (underline >> 11) & 1;
    pen.isBackgroundAssignedDefault = (underline >> 12) & 1;
    if (pen.underlineIndex > colorRGB || pen.semanticRole > SemanticRole::Input || (underline >> 13) != 0)
        reader.fail();
    pen.foregroundRed = reader.u8();
    pen.foregroundGreen = reader.u8();
    pen.foregroundBlue = reader.u8();
    pen.backgroundRed = reader.u8();
    pen.backgroundGreen = reader.u8();
    pen.backgroundBlue = reader.u8();
    pen.underlineRed = reader.u8();
    pen.underlineGreen = reader.u8();
    pen.underlineBlue = reader.u8();
    pen.underlineStyle = reader.u8();
    pen.extendedAttributes = Ubiquitty::VT::ExtendedAttributes::fromRaw(reader.u8());
    pen.font = reader.u8();
    return pen;
}

std::optional<LinkDefinition> linkDefinition(const Ubiquitty::Terminal& terminal, std::uint16_t id)
{
    const Ubiquitty::Attributes::Hyperlinks::LinkTable* table = Ubiquitty::Attributes::Hyperlinks::table(&terminal);
    if (!table || id == 0)
        return std::nullopt;
    for (int index = 0; index < terminal.linkCount; ++index) {
        const auto slot = static_cast<std::size_t>((terminal.linkHead + index) % Ubiquitty::VT::linkTableCapacity);
        if (table->ids[slot] != id)
            continue;
        LinkDefinition definition;
        definition.id = id;
        definition.key.assign(table->keys[slot].data(), ::strnlen(table->keys[slot].data(), table->keys[slot].size()));
        definition.uri.assign(table->uris[slot].data(), ::strnlen(table->uris[slot].data(), table->uris[slot].size()));
        return definition;
    }
    return std::nullopt;
}

std::size_t PaneDictionaries::PenKeyHash::operator()(const PenKey& key) const
{
    return static_cast<std::size_t>(blockHash(key.bytes));
}

PaneDictionaries::Snapshot PaneDictionaries::snapshot() const
{
    return { m_epoch, m_checkpointed, m_pens, m_links, m_clusterHashes };
}

std::optional<PaneDictionaries> PaneDictionaries::restored(Snapshot&& snapshot)
{
    if (snapshot.pens.size() > std::numeric_limits<std::uint32_t>::max()
        || snapshot.clusterHashes.size() > std::numeric_limits<std::uint16_t>::max()
        || snapshot.checkpointed.pens > snapshot.pens.size() || snapshot.checkpointed.links > snapshot.links.size()
        || snapshot.checkpointed.clusters > snapshot.clusterHashes.size())
        return std::nullopt;
    PaneDictionaries result;
    for (std::size_t index = 0; index < snapshot.pens.size(); ++index) {
        PenKey key;
        stagePen(key.bytes.data(), snapshot.pens[index]);
        if (!result.m_penIDs.emplace(key, static_cast<std::uint32_t>(index)).second)
            return std::nullopt;
    }
    for (std::size_t index = 0; index < snapshot.links.size(); ++index) {
        if (!snapshot.links[index].id)
            return std::nullopt;
        result.m_latestLink[snapshot.links[index].id] = index;
    }
    result.m_epoch = snapshot.epoch;
    result.m_checkpointed = snapshot.checkpointed;
    result.m_pens = std::move(snapshot.pens);
    result.m_links = std::move(snapshot.links);
    result.m_clusterHashes = std::move(snapshot.clusterHashes);
    return result;
}

void PaneDictionaries::roll()
{
    ++m_epoch;
    m_checkpointed = {};
    m_pens.clear();
    m_penIDs.clear();
    m_links.clear();
    m_latestLink.clear();
    m_clusterHashes.clear();
}

void PaneDictionaries::noteCheckpointed()
{
    m_checkpointed = { .pens = m_pens.size(), .links = m_links.size(), .clusters = m_clusterHashes.size() };
}

bool PaneDictionaries::isPastEpochLimits() const
{
    return m_pens.size() > m_checkpointed.pens + maxPensPerEpoch || m_links.size() > m_checkpointed.links + maxLinksPerEpoch
        || m_clusterHashes.size() > m_checkpointed.clusters + maxClustersPerEpoch;
}

std::uint32_t PaneDictionaries::penID(const Pen& pen)
{
    PenKey key;
    stagePen(key.bytes.data(), pen);
    const auto [found, isNew] = m_penIDs.try_emplace(key, static_cast<std::uint32_t>(m_pens.size()));
    if (isNew)
        m_pens.push_back(pen);
    return found->second;
}

void PaneDictionaries::noteLink(const Ubiquitty::Terminal& terminal, std::uint16_t id)
{
    std::optional<LinkDefinition> definition = linkDefinition(terminal, id);
    if (!definition)
        return;
    if (const auto latest = m_latestLink.find(id); latest != m_latestLink.end() && m_links[latest->second] == *definition)
        return;
    m_latestLink[id] = m_links.size();
    m_links.push_back(std::move(*definition));
}

bool PaneDictionaries::takeClusters(const Ubiquitty::Terminal& terminal)
{
    const std::span<const ClusterPool::Entry> entries = terminal.clusterPool.entries();
    if (entries.size() < m_clusterHashes.size())
        return false;
    for (std::size_t slot = m_clusterHashes.size(); slot < entries.size(); ++slot) {
        if (entries[slot].id != slot + 1)
            return false;
        m_clusterHashes.push_back(clusterEntryHash(entries[slot]));
    }
    return true;
}

bool PaneDictionaries::noteCluster(const Ubiquitty::Terminal& terminal, std::uint16_t id)
{
    if (id == 0)
        return false;
    if (id > m_clusterHashes.size() && !takeClusters(terminal))
        return false;
    const std::span<const ClusterPool::Entry> entries = terminal.clusterPool.entries();
    const std::size_t slot = static_cast<std::size_t>(id) - 1;
    if (slot >= m_clusterHashes.size() || slot >= entries.size() || entries[slot].id != id)
        return false;
    return clusterEntryHash(entries[slot]) == m_clusterHashes[slot];
}

namespace {

// How a screen row names what its cells refer to: pens through the pane's
// dictionary, links and long clusters by the host's own ids.
class PaneRowNames {
public:
    PaneRowNames(const Ubiquitty::Terminal& terminal, PaneDictionaries& dictionaries, RunTextDefinitions runTexts)
        : m_terminal(terminal)
        , m_dictionaries(dictionaries)
        , m_runTexts(runTexts)
    {
    }

    std::uint64_t pen(const Pen& pen) { return m_dictionaries.penID(pen); }

    std::uint64_t link(std::uint16_t id)
    {
        m_dictionaries.noteLink(m_terminal, id);
        return id;
    }

    std::optional<std::uint64_t> cluster(const Cell& lead)
    {
        const std::uint16_t cluster = lead.clusterID();
        if (Ubiquitty::isRunTextAnchor(&m_terminal, lead)) {
            if (m_runTexts == RunTextDefinitions::Absent || Ubiquitty::runTextCodePoints(&m_terminal, lead).empty())
                return std::nullopt;
        } else if (!m_dictionaries.noteCluster(m_terminal, cluster)) {
            return std::nullopt;
        }
        return cluster;
    }

private:
    const Ubiquitty::Terminal& m_terminal;
    PaneDictionaries& m_dictionaries;
    RunTextDefinitions m_runTexts;
};

} // namespace

bool encodeRowRecord(const Ubiquitty::Terminal& terminal, std::span<const Cell> cells, const RowAttributes& attributes,
    PaneDictionaries& dictionaries, RunTextDefinitions runTexts, std::vector<std::uint8_t>& output)
{
    const std::size_t before = output.size();
    WireWriter writer(output);
    writer.u8(attributes.lineAttribute);
    writer.u8(attributes.isWrapped ? 1 : 0);
    writer.u8(attributes.mark);
    PaneRowNames names(terminal, dictionaries, runTexts);
    if (!Screen::writeRowForm(cells, names, writer)) {
        output.resize(before);
        return false;
    }
    return true;
}

bool decodeRowRecord(WireReader& reader, std::span<const Pen> pens, const RowReferences& references,
    std::span<Cell> cells, RowAttributes& attributes)
{
    const auto resolves = [&references](std::uint64_t cluster) {
        return cluster < references.clusters.size() && references.clusters[static_cast<std::size_t>(cluster)];
    };
    return decodeRowRecordWithNames(reader, pens, resolves, cells, attributes);
}

} // namespace Ubiquitty::Protocol::Sync
