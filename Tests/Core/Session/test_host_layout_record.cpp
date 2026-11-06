// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_host_layout_record.cpp — the record a host keeps of its named
// workspaces across a restart: a record round-trips field for field, and bytes
// that do not decode whole — truncated, trailing, out of bounds, repeated —
// decode to nothing rather than to part of a record.

#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/Identity.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using Ubiquitty::Session::decodeHostLayoutRecords;
using Ubiquitty::Session::encodeHostLayoutRecords;
using Ubiquitty::Session::HostLayoutHolder;
using Ubiquitty::Session::HostLayoutPane;
using Ubiquitty::Session::HostLayoutRecord;
using Ubiquitty::Session::PaneNode;
using Ubiquitty::Session::WorkspaceTab;
using Ubiquitty::Session::WorkspaceWindow;
using Ubiquitty::Transport::Origin;
using Ubiquitty::Transport::OriginKind;

namespace {

PaneNode leaf(std::string directory, std::string title = {}, Origin origin = {})
{
    PaneNode node = PaneNode::makeLeaf(std::move(origin), std::move(title));
    node.directory = std::move(directory);
    return node;
}

/// Two windows: a tab of one pane and a renamed tab split three ways, and a
/// window holding one pane launched as a remote session.
HostLayoutRecord sampleRecord(std::string name, std::uint32_t workspaceID = 7)
{
    HostLayoutRecord record;
    record.arrangement.name = std::move(name);
    WorkspaceWindow first;
    WorkspaceTab single;
    single.root = leaf("/home/user", "shell");
    first.tabs.push_back(single);
    WorkspaceTab split;
    split.title = "logs";
    split.root = PaneNode::makeSplit(false, 0.25, leaf("/var/log"), PaneNode::makeSplit(true, 0.5, leaf("/tmp/a b"), leaf("/srv")));
    split.focusedPaneIndex = 2;
    first.tabs.push_back(split);
    first.activeTab = 1;
    first.zoomedPaneIndex = 1;
    record.arrangement.windows.push_back(first);
    WorkspaceWindow second;
    WorkspaceTab remote;
    remote.root = leaf("/home/user/project", "remote", Origin { OriginKind::SSH, "user@host" });
    second.tabs.push_back(remote);
    record.arrangement.windows.push_back(second);
    record.arrangement.activeWindow = 1;
    record.windowLabels = { "main", "" };
    record.workspaceLabel = "client work";
    record.tabLabels = { "", "tailing", "far" };
    record.windowTags = { { "production", "billing" }, {} };
    record.workspaceTags = { "client", "billable" };
    record.tabTags = { {}, { "noisy" }, { "far", "slow" } };
    record.panes = { { 80, 24, "", "", {}, {} }, { 100, 30, "tail", "", { "watch" }, {} }, { 50, 15, "", "scratch", {}, { "throwaway", "mine" } }, { 50, 14, "", "", {}, {} }, { 132, 43, "", "", { "a", "b" }, { "c" } } };
    record.workspaceID = workspaceID;
    record.heldUnixSeconds = 1'800'000'000;
    record.holders = { { .daemonInstanceID = 0x1122334455667788ull, .workspaceID = workspaceID }, { .daemonInstanceID = 42, .workspaceID = workspaceID } };
    return record;
}

std::vector<std::uint8_t> encode(const std::vector<HostLayoutRecord>& records)
{
    std::vector<std::uint8_t> bytes;
    encodeHostLayoutRecords(records, bytes);
    return bytes;
}

bool sameRatio(double a, double b) { return a - b < 1.0 / 30000 && b - a < 1.0 / 30000; }

bool sameNode(const PaneNode& a, const PaneNode& b)
{
    if (a.leaf != b.leaf)
        return false;
    if (a.leaf)
        return a.directory == b.directory && a.title == b.title && a.origin.transport == b.origin.transport && a.origin.host == b.origin.host;
    return a.horizontalSplit == b.horizontalSplit && sameRatio(a.ratio, b.ratio) && a.children.size() == 2 && b.children.size() == 2
        && sameNode(a.children[0], b.children[0]) && sameNode(a.children[1], b.children[1]);
}

bool sameRecord(const HostLayoutRecord& a, const HostLayoutRecord& b)
{
    if (a.arrangement.name != b.arrangement.name || a.arrangement.activeWindow != b.arrangement.activeWindow || a.windowLabels != b.windowLabels || a.workspaceLabel != b.workspaceLabel || a.tabLabels != b.tabLabels || a.windowTags != b.windowTags || a.workspaceTags != b.workspaceTags || a.tabTags != b.tabTags || a.panes != b.panes
        || a.workspaceID != b.workspaceID || a.holders != b.holders || a.heldUnixSeconds != b.heldUnixSeconds
        || a.arrangement.windows.size() != b.arrangement.windows.size())
        return false;
    for (std::size_t window = 0; window < a.arrangement.windows.size(); ++window) {
        const auto& x = a.arrangement.windows[window];
        const auto& y = b.arrangement.windows[window];
        if (x.activeTab != y.activeTab || x.zoomedPaneIndex != y.zoomedPaneIndex || x.tabs.size() != y.tabs.size())
            return false;
        for (std::size_t tab = 0; tab < x.tabs.size(); ++tab) {
            if (x.tabs[tab].title != y.tabs[tab].title || x.tabs[tab].focusedPaneIndex != y.tabs[tab].focusedPaneIndex || !sameNode(x.tabs[tab].root, y.tabs[tab].root))
                return false;
        }
    }
    return true;
}

/// Gives every leaf under a node a directory as long as a record keeps.
void lengthenDirectories(PaneNode& node)
{
    if (node.leaf)
        node.directory.assign(Ubiquitty::Session::maxHostLayoutDirectoryLength, 'd');
    for (PaneNode& child : node.children)
        lengthenDirectories(child);
}

/// A record whose one tab is a chain of splits holding `leaves` panes.
HostLayoutRecord chainRecord(std::size_t leaves)
{
    HostLayoutRecord record;
    record.arrangement.name = "deep";
    PaneNode root = leaf("/0");
    for (std::size_t index = 1; index < leaves; ++index)
        root = PaneNode::makeSplit(false, 0.5, leaf("/" + std::to_string(index)), std::move(root));
    WorkspaceWindow window;
    WorkspaceTab tab;
    tab.root = std::move(root);
    window.tabs.push_back(std::move(tab));
    record.arrangement.windows.push_back(std::move(window));
    record.windowLabels = { "" };
    record.tabLabels = { "" };
    record.windowTags = { {} };
    record.tabTags = { {} };
    record.panes.assign(leaves, HostLayoutPane { 80, 24, "", "", {}, {} });
    return record;
}

} // namespace

TEST(host_layout_record_round_trip)
{
    const std::vector<HostLayoutRecord> records { sampleRecord("work"), sampleRecord("play", 8) };
    const auto bytes = encode(records);
    const auto decoded = decodeHostLayoutRecords(bytes);
    CHECK(decoded.has_value());
    if (!decoded)
        return;
    CHECK(decoded->size() == 2);
    CHECK(sameRecord((*decoded)[0], records[0]));
    CHECK(sameRecord((*decoded)[1], records[1]));
    CHECK(Ubiquitty::Session::hostLayoutPaneCount((*decoded)[0].arrangement) == 5);

    // No records is a file of its own, and decodes to none rather than to nothing.
    const auto empty = decodeHostLayoutRecords(encode({}));
    CHECK(empty.has_value() && empty->empty());
}

TEST(host_layout_record_malformed_is_absent)
{
    const auto bytes = encode({ sampleRecord("work") });
    // Every truncation is refused: no prefix of a record is a record.
    for (std::size_t length = 0; length < bytes.size(); ++length)
        CHECK(!decodeHostLayoutRecords(std::span(bytes).first(length)).has_value());
    // A byte past the end is not ignored.
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK(!decodeHostLayoutRecords(trailing).has_value());
    // Another file's bytes are not read as a record.
    auto foreign = bytes;
    foreign[0] = 'X';
    CHECK(!decodeHostLayoutRecords(foreign).has_value());
    // A file larger than any host writes is not read at all.
    std::vector<std::uint8_t> huge(Ubiquitty::Session::maxHostLayoutFileBytes + 1, 0);
    CHECK(!decodeHostLayoutRecords(huge).has_value());
    // Nor is one whose every record is sound: the bound is the file's.
    const auto wide = [](std::size_t count) {
        std::vector<HostLayoutRecord> records;
        for (std::size_t index = 0; index < count; ++index) {
            HostLayoutRecord record = chainRecord(Ubiquitty::Session::maxHostLayoutPanes);
            record.arrangement.name = "wide " + std::to_string(index);
            lengthenDirectories(record.arrangement.windows[0].tabs[0].root);
            records.push_back(std::move(record));
        }
        return records;
    };
    const std::vector<std::uint8_t> under = encode(wide(3));
    const std::vector<std::uint8_t> over = encode(wide(5));
    CHECK(under.size() <= Ubiquitty::Session::maxHostLayoutFileBytes && over.size() > Ubiquitty::Session::maxHostLayoutFileBytes);
    CHECK(decodeHostLayoutRecords(under).has_value());
    CHECK(Ubiquitty::Session::readHostLayoutFile(over).verdict == Ubiquitty::Session::HostLayoutFileVerdict::Damaged);
}

TEST(host_layout_record_bounds)
{
    // Two records under one name are one name too many.
    CHECK(!decodeHostLayoutRecords(encode({ sampleRecord("twice"), sampleRecord("twice") })).has_value());
    // An unnamed workspace is reached by its id, so a record with neither
    // reaches nothing; two records under one id are one workspace twice.
    CHECK(decodeHostLayoutRecords(encode({ sampleRecord("") })).has_value());
    CHECK(!decodeHostLayoutRecords(encode({ sampleRecord("", 0) })).has_value());
    CHECK(!decodeHostLayoutRecords(encode({ sampleRecord("", 9), sampleRecord("other", 9) })).has_value());
    // A pane recorded with no grid has nothing to be restored at.
    auto gridless = sampleRecord("gridless");
    gridless.panes[3].columns = 0;
    CHECK(!decodeHostLayoutRecords(encode({ gridless })).has_value());
    // A directory longer than any path is refused, not truncated.
    auto longPath = sampleRecord("long");
    longPath.arrangement.windows[0].tabs[0].root.directory.assign(Ubiquitty::Session::maxHostLayoutDirectoryLength + 1, 'd');
    CHECK(!decodeHostLayoutRecords(encode({ longPath })).has_value());
    // A focus or zoom past the tab's leaves names no pane.
    auto focus = sampleRecord("focus");
    focus.arrangement.windows[0].tabs[1].focusedPaneIndex = 3;
    CHECK(!decodeHostLayoutRecords(encode({ focus })).has_value());
    auto zoom = sampleRecord("zoom");
    zoom.arrangement.windows[0].zoomedPaneIndex = 3;
    CHECK(!decodeHostLayoutRecords(encode({ zoom })).has_value());
    // A label longer than a label may be is refused, at every level.
    auto longWorkspaceLabel = sampleRecord("label");
    longWorkspaceLabel.workspaceLabel.assign(Ubiquitty::Session::Identity::maxLabelLength + 1, 'w');
    CHECK(!decodeHostLayoutRecords(encode({ longWorkspaceLabel })).has_value());
    auto longTabLabel = sampleRecord("label");
    longTabLabel.tabLabels[2].assign(Ubiquitty::Session::Identity::maxLabelLength + 1, 't');
    CHECK(!decodeHostLayoutRecords(encode({ longTabLabel })).has_value());
    auto longestLabels = sampleRecord("label");
    longestLabels.workspaceLabel.assign(Ubiquitty::Session::Identity::maxLabelLength, 'w');
    longestLabels.tabLabels[2].assign(Ubiquitty::Session::Identity::maxLabelLength, 't');
    const auto longest = decodeHostLayoutRecords(encode({ longestLabels }));
    CHECK(longest.has_value() && sameRecord((*longest)[0], longestLabels));
    // A window's tags are bounded in number and in length, like its label.
    auto manyTags = sampleRecord("tags");
    manyTags.windowTags[1].assign(Ubiquitty::Session::Identity::maxTagsPerObject, "t");
    const auto mostTags = decodeHostLayoutRecords(encode({ manyTags }));
    CHECK(mostTags.has_value() && sameRecord((*mostTags)[0], manyTags));
    manyTags.windowTags[1].push_back("one more");
    CHECK(!decodeHostLayoutRecords(encode({ manyTags })).has_value());
    auto longTag = sampleRecord("tags");
    longTag.windowTags[0][1].assign(Ubiquitty::Session::Identity::maxLabelLength + 1, 't');
    CHECK(!decodeHostLayoutRecords(encode({ longTag })).has_value());
    // A window the record names no tags for is recorded with none.
    auto fewerTags = sampleRecord("tags");
    fewerTags.windowTags = {};
    const auto noTags = decodeHostLayoutRecords(encode({ fewerTags }));
    CHECK(noTags.has_value() && (*noTags)[0].windowTags == std::vector<std::vector<std::string>>(2));
    // So are the tags of every other level: the workspace, a tab, a pane and
    // the session behind it.
    using TagsOf = std::vector<std::string>& (*)(HostLayoutRecord&);
    const TagsOf levels[] = {
        [](HostLayoutRecord& record) -> std::vector<std::string>& { return record.workspaceTags; },
        [](HostLayoutRecord& record) -> std::vector<std::string>& { return record.tabTags[1]; },
        [](HostLayoutRecord& record) -> std::vector<std::string>& { return record.panes[3].tags; },
        [](HostLayoutRecord& record) -> std::vector<std::string>& { return record.panes[3].sessionTags; },
    };
    for (const TagsOf tagsOf : levels) {
        auto most = sampleRecord("tags");
        tagsOf(most).assign(Ubiquitty::Session::Identity::maxTagsPerObject, std::string(Ubiquitty::Session::Identity::maxLabelLength, 't'));
        const auto kept = decodeHostLayoutRecords(encode({ most }));
        CHECK(kept.has_value() && sameRecord((*kept)[0], most));
        auto longName = most;
        tagsOf(longName).back().push_back('t');
        CHECK(!decodeHostLayoutRecords(encode({ longName })).has_value());
        tagsOf(most).push_back("one more");
        CHECK(!decodeHostLayoutRecords(encode({ most })).has_value());
    }
    // A tab the record names no tags for is recorded with none.
    auto fewerTabTags = sampleRecord("tags");
    fewerTabTags.tabTags = { { "first" } };
    const auto fewTabTags = decodeHostLayoutRecords(encode({ fewerTabTags }));
    CHECK(fewTabTags.has_value() && (*fewTabTags)[0].tabTags == (std::vector<std::vector<std::string>> { { "first" }, {}, {} }));
    // A tab the record names no label for is recorded with none.
    auto fewerLabels = sampleRecord("label");
    fewerLabels.tabLabels = { "only the first" };
    const auto fewer = decodeHostLayoutRecords(encode({ fewerLabels }));
    CHECK(fewer.has_value() && (*fewer)[0].tabLabels == std::vector<std::string>({ "only the first", "", "" }));
    // A tab of as many panes as a host runs decodes; one more does not.
    CHECK(decodeHostLayoutRecords(encode({ chainRecord(Ubiquitty::Session::maxHostLayoutPanes) })).has_value());
    CHECK(!decodeHostLayoutRecords(encode({ chainRecord(Ubiquitty::Session::maxHostLayoutPanes + 1) })).has_value());
}

// The file says what version it is and how long each record is, and a
// reader says which of five things reading it came to.
TEST(host_layout_file_names_its_version_and_each_records_length)
{
    using Ubiquitty::Session::describeUnreadHostLayoutFile;
    using Ubiquitty::Session::HostLayoutFile;
    using Ubiquitty::Session::HostLayoutFileVerdict;
    using Ubiquitty::Session::hostLayoutFileVersion;
    using Ubiquitty::Session::readHostLayoutFile;
    const std::vector<HostLayoutRecord> records { sampleRecord("work"), chainRecord(3) };
    const std::vector<std::uint8_t> bytes = encode(records);
    const auto u16At = [](const std::vector<std::uint8_t>& from, std::size_t at) { return static_cast<std::uint16_t>(from[at] | (from[at + 1] << 8)); };
    const auto u32At = [](const std::vector<std::uint8_t>& from, std::size_t at) {
        return static_cast<std::uint32_t>(from[at]) | (static_cast<std::uint32_t>(from[at + 1]) << 8) | (static_cast<std::uint32_t>(from[at + 2]) << 16) | (static_cast<std::uint32_t>(from[at + 3]) << 24);
    };
    REQUIRE(bytes.size() > 11);
    CHECK(bytes[0] == 'U' && bytes[1] == 'B' && bytes[2] == 'Q' && bytes[3] == 'N');
    CHECK_EQ(u16At(bytes, 4), hostLayoutFileVersion);
    CHECK_EQ(bytes[6], 2);
    // The two lengths are the two records, end to end, and nothing else.
    const std::size_t firstLength = u32At(bytes, 7);
    REQUIRE(bytes.size() > 11 + firstLength + 4);
    const std::size_t secondLength = u32At(bytes, 11 + firstLength);
    CHECK_EQ(7 + 4 + firstLength + 4 + secondLength, bytes.size());

    const HostLayoutFile whole = readHostLayoutFile(bytes);
    CHECK(whole.verdict == HostLayoutFileVerdict::Read && whole.version == hostLayoutFileVersion && whole.records.size() == 2);
    CHECK(whole.records.size() == 2 && sameRecord(whole.records[0], records[0]) && sameRecord(whole.records[1], records[1]));
    CHECK(describeUnreadHostLayoutFile(whole).empty());

    // A version this build does not know is refused as that, with its
    // number, whatever follows it: it is not a damaged file and not an empty
    // one.
    for (const std::uint16_t version : { static_cast<std::uint16_t>(hostLayoutFileVersion + 1), static_cast<std::uint16_t>(0xFFFF) }) {
        auto newer = bytes;
        newer[4] = static_cast<std::uint8_t>(version & 0xFF);
        newer[5] = static_cast<std::uint8_t>(version >> 8);
        const HostLayoutFile refused = readHostLayoutFile(newer);
        CHECK(refused.verdict == HostLayoutFileVerdict::NewerVersion && refused.version == version && refused.records.empty());
        const std::string why = describeUnreadHostLayoutFile(refused);
        CHECK_MSG(why.find(std::to_string(version)) != std::string::npos && why.find(std::to_string(hostLayoutFileVersion)) != std::string::npos, "the reason names the file's version and this build's");
        CHECK(!decodeHostLayoutRecords(newer).has_value());
        newer.resize(6);
        CHECK_MSG(readHostLayoutFile(newer).verdict == HostLayoutFileVerdict::NewerVersion, "nothing past the version is read to say so");
        newer.resize(1'200'000, 0);
        CHECK_MSG(readHostLayoutFile(newer).verdict == HostLayoutFileVerdict::NewerVersion, "nor is this build's size bound held against a version it does not know");
    }
    // Version zero was never written.
    auto zero = bytes;
    zero[4] = 0;
    zero[5] = 0;
    CHECK(readHostLayoutFile(zero).verdict == HostLayoutFileVerdict::Damaged);

    // A record is held to its own bytes. One byte fewer leaves it cut short,
    // and one more hands it a byte no field accounts for, though the file as
    // a whole still ends where its lengths say.
    auto shorter = bytes;
    shorter[7] = static_cast<std::uint8_t>(shorter[7] - 1);
    CHECK(readHostLayoutFile(shorter).verdict == HostLayoutFileVerdict::Damaged);
    auto longer = bytes;
    longer.insert(longer.begin() + static_cast<std::ptrdiff_t>(11 + firstLength), 0);
    longer[7] = static_cast<std::uint8_t>(longer[7] + 1);
    CHECK_EQ(u32At(longer, 7), firstLength + 1);
    CHECK(readHostLayoutFile(longer).verdict == HostLayoutFileVerdict::Damaged);
    // A length past the end of the file is not read past it.
    auto past = bytes;
    past[11 + firstLength + 2] = 0xFF;
    CHECK(readHostLayoutFile(past).verdict == HostLayoutFileVerdict::Damaged);

    // What is damaged is said to be, and bytes that open as no record file
    // are said to be that.
    auto trailing = bytes;
    trailing.push_back(0);
    const HostLayoutFile damaged = readHostLayoutFile(trailing);
    CHECK(damaged.verdict == HostLayoutFileVerdict::Damaged && damaged.version == hostLayoutFileVersion && damaged.records.empty());
    CHECK(!describeUnreadHostLayoutFile(damaged).empty());
    auto foreign = bytes;
    foreign[0] = 'X';
    const HostLayoutFile other = readHostLayoutFile(foreign);
    CHECK(other.verdict == HostLayoutFileVerdict::NotARecordFile && other.version == 0);
    CHECK(!describeUnreadHostLayoutFile(other).empty());
    CHECK(readHostLayoutFile({}).verdict == HostLayoutFileVerdict::NotARecordFile);
    CHECK(describeUnreadHostLayoutFile(damaged) != describeUnreadHostLayoutFile(other));
}

// A file in a form written before the file carried a version is not a record
// file this build reads: it says so, and reads nothing out of it.
TEST(host_layout_file_in_an_unversioned_form_is_not_read)
{
    using Ubiquitty::Session::HostLayoutFile;
    using Ubiquitty::Session::HostLayoutFileVerdict;
    using Ubiquitty::Session::readHostLayoutFile;
    for (const char form : { 'L', 'M' }) {
        std::vector<std::uint8_t> bytes { 'U', 'B', 'Q', static_cast<std::uint8_t>(form), 1 };
        const auto put = [&](std::string_view text) {
            bytes.push_back(static_cast<std::uint8_t>(text.size() & 0xFF));
            bytes.push_back(static_cast<std::uint8_t>(text.size() >> 8));
            bytes.insert(bytes.end(), text.begin(), text.end());
        };
        put("kept");
        if (form == 'M')
            put("client work");
        bytes.insert(bytes.end(), { 0, 1 });
        put("main");
        if (form == 'M')
            bytes.push_back(0);
        bytes.insert(bytes.end(), { 0, 0xFF, 1 });
        put("logs");
        if (form == 'M')
            put("tailing");
        bytes.insert(bytes.end(), { 0, 0, 80, 0, 24, 0 });
        put("/var/log");
        put("");
        put("tail");
        put("pane label");
        put("session label");
        const HostLayoutFile file = readHostLayoutFile(bytes);
        CHECK_MSG(file.verdict == HostLayoutFileVerdict::NotARecordFile && file.records.empty() && file.version == 0, "a file of an unversioned form was read");
        CHECK(!Ubiquitty::Session::describeUnreadHostLayoutFile(file).empty());
        CHECK(!decodeHostLayoutRecords(bytes).has_value());
    }
}

UBIQUITTY_TEST_MAIN()

TEST(host_layout_record_answers_only_the_reference_it_was_written_for)
{
    using Ubiquitty::Session::holdersAfter;
    using Ubiquitty::Session::isRecordOfReference;
    using Ubiquitty::Session::maxHostLayoutHolders;
    const HostLayoutRecord record = sampleRecord("work");
    CHECK(isRecordOfReference(record, 0x1122334455667788ull, 7));
    CHECK(isRecordOfReference(record, 42, 7));
    // An id alone is every restarted host's, an instance alone held every
    // workspace it served; and nothing answers a reference with a zero half.
    CHECK(!isRecordOfReference(record, 0x1122334455667788ull, 8));
    CHECK(!isRecordOfReference(record, 43, 7));
    CHECK(!isRecordOfReference(record, 0, 7));
    HostLayoutRecord unidentified = record;
    unidentified.workspaceID = 0;
    CHECK(!isRecordOfReference(unidentified, 42, 0));

    // The holder now comes first, an earlier mention of its instance goes,
    // and the list keeps the newest few, each with the id it held.
    const std::vector<HostLayoutHolder> former { { 5, 50 }, { 9, 90 }, { 3, 30 }, { 1, 10 } };
    CHECK(holdersAfter(former, { 9, 91 }) == std::vector<HostLayoutHolder>({ { 9, 91 }, { 5, 50 }, { 3, 30 }, { 1, 10 } }));
    CHECK(holdersAfter(former, { 8, 80 }) == std::vector<HostLayoutHolder>({ { 8, 80 }, { 5, 50 }, { 9, 90 }, { 3, 30 } }));
    CHECK(holdersAfter(former, { 8, 80 }).size() == maxHostLayoutHolders);
    CHECK(holdersAfter({}, { 8, 80 }) == std::vector<HostLayoutHolder>({ { 8, 80 } }));
}

// Two hosts both number their first workspace 1. A host that takes in a
// stopped one's record while it holds a workspace 1 of its own lists the
// record under another id, and the reference a client saved still names it:
// the instance and the id it held the workspace under, never the id it is
// listed by here.
TEST(host_layout_record_answers_the_reference_under_the_id_its_holder_knew)
{
    using Ubiquitty::Session::holdersAfter;
    using Ubiquitty::Session::isRecordOfReference;
    constexpr std::uint64_t stopped = 0x51;
    constexpr std::uint64_t restoring = 0x52;
    HostLayoutRecord record = sampleRecord("", 1);
    record.holders = { { .daemonInstanceID = stopped, .workspaceID = 1 } };
    // Listed under 2 by the host that took it in, which never held it.
    record.workspaceID = 2;
    CHECK(isRecordOfReference(record, stopped, 1));
    CHECK_MSG(!isRecordOfReference(record, stopped, 2), "the id a later host listed it under is no id the stopped instance held it by");
    CHECK(!isRecordOfReference(record, restoring, 2));
    const auto listed = decodeHostLayoutRecords(encode({ record }));
    REQUIRE(listed && listed->size() == 1);
    CHECK(sameRecord(listed->front(), record));
    CHECK(isRecordOfReference(listed->front(), stopped, 1));

    // Restored there under 2, its next record names both holders, each with
    // its own id.
    record.holders = holdersAfter(record.holders, { .daemonInstanceID = restoring, .workspaceID = 2 });
    CHECK(isRecordOfReference(record, restoring, 2));
    CHECK(isRecordOfReference(record, stopped, 1));
    CHECK(!isRecordOfReference(record, stopped, 2));
    CHECK(!isRecordOfReference(record, restoring, 1));
}

// A file of an earlier version is refused as that, with both numbers, before
// anything past its version is read: it is not read in this version's layout,
// and it is not a damaged file.
TEST(host_layout_file_of_an_earlier_version_is_refused_unread)
{
    using Ubiquitty::Session::describeUnreadHostLayoutFile;
    using Ubiquitty::Session::HostLayoutFileVerdict;
    using Ubiquitty::Session::hostLayoutFileVersion;
    using Ubiquitty::Session::readHostLayoutFile;
    std::vector<std::uint8_t> bytes = encode({ sampleRecord("work", 7) });
    for (std::uint16_t version = 1; version < hostLayoutFileVersion; ++version) {
        bytes[4] = static_cast<std::uint8_t>(version & 0xFF);
        bytes[5] = static_cast<std::uint8_t>(version >> 8);
        const auto file = readHostLayoutFile(bytes);
        CHECK(file.verdict == HostLayoutFileVerdict::OlderVersion && file.version == version && file.records.empty());
        const std::string why = describeUnreadHostLayoutFile(file);
        CHECK_MSG(why.find(std::to_string(version)) != std::string::npos && why.find(std::to_string(hostLayoutFileVersion)) != std::string::npos,
            "the reason names the file's version and this build's");
        CHECK(!decodeHostLayoutRecords(bytes).has_value());
    }
}

TEST(host_layout_record_refuses_holders_it_cannot_name)
{
    using Ubiquitty::Session::maxHostLayoutHolders;
    HostLayoutRecord record = sampleRecord("work");
    record.holders = { { 1, 7 }, { 1, 8 } };
    CHECK(!decodeHostLayoutRecords(encode({ record })).has_value());
    record.holders = { { 0, 7 } };
    CHECK(!decodeHostLayoutRecords(encode({ record })).has_value());
    // The encoder writes no more than a record names; a file that says more
    // is refused rather than cut.
    record.holders.clear();
    for (std::uint64_t holder = 1; holder <= maxHostLayoutHolders + 2; ++holder)
        record.holders.push_back({ .daemonInstanceID = holder, .workspaceID = 7 });
    const auto decoded = decodeHostLayoutRecords(encode({ record }));
    CHECK(decoded && decoded->front().holders.size() == maxHostLayoutHolders);
}

TEST(host_layout_record_keeps_an_unnamed_workspace_by_its_id)
{
    const std::vector<HostLayoutRecord> records { sampleRecord("", 11), sampleRecord("", 12), sampleRecord("named", 13) };
    const auto decoded = decodeHostLayoutRecords(encode(records));
    CHECK(decoded && decoded->size() == 3);
    if (decoded && decoded->size() == 3) {
        CHECK(sameRecord((*decoded)[0], records[0]));
        CHECK((*decoded)[0].arrangement.name.empty() && (*decoded)[0].workspaceID == 11);
        CHECK((*decoded)[1].workspaceID == 12);
    }
    using Ubiquitty::Session::isSameRecordedWorkspace;
    CHECK(!isSameRecordedWorkspace(sampleRecord("", 11), sampleRecord("", 12)));
    CHECK(isSameRecordedWorkspace(sampleRecord("", 11), sampleRecord("named", 11)));
    CHECK(isSameRecordedWorkspace(sampleRecord("named", 1), sampleRecord("named", 2)));
    CHECK(!isSameRecordedWorkspace(sampleRecord("", 0), sampleRecord("", 0)));
}

TEST(host_layout_records_go_least_recently_held_first)
{
    using Ubiquitty::Session::heldTimeAfter;
    using Ubiquitty::Session::hostLayoutHeldResolutionSeconds;
    std::vector<HostLayoutRecord> records { sampleRecord("a", 1), sampleRecord("b", 2), sampleRecord("c", 3), sampleRecord("d", 4) };
    records[0].heldUnixSeconds = 100;
    records[1].heldUnixSeconds = 300;
    records[2].heldUnixSeconds = 200;
    records[3].heldUnixSeconds = 300;
    Ubiquitty::Session::orderByRecentHold(records);
    // The most recently held first, ties in their order, so the tail is the
    // least recently held.
    CHECK(records[0].arrangement.name == "b" && records[1].arrangement.name == "d" && records[2].arrangement.name == "c" && records[3].arrangement.name == "a");

    // A workspace in use is not rewritten for the time alone.
    CHECK(heldTimeAfter(0, 5000) == 5000);
    CHECK(heldTimeAfter(5000, 5000 + hostLayoutHeldResolutionSeconds - 1) == 5000);
    CHECK(heldTimeAfter(5000, 5000 + hostLayoutHeldResolutionSeconds) == 5000 + hostLayoutHeldResolutionSeconds);
    // A clock that moved back is believed.
    CHECK(heldTimeAfter(5000, 4000) == 4000);
}
