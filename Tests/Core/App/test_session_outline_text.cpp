// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_session_outline_text.cpp — an outline as lines and as JSON.
//
// Both spellings read a row and decide nothing, so the rows here are stated
// outright and the lines and objects they print are compared whole: a field a
// spelling drops, reorders or words differently is a failing line, not a
// listing that quietly says less than the window beside it.

#include "Core/App/SessionOutline.h"
#include "Core/App/SessionOutlineText.h"
#include "Core/Codec/JSON.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/Panes/PaneTreeNode.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/Identity.h"
#include "Core/Session/IllFormedTree.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/StatedPane.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty::App;
using Ubiquitty::Session::Identity::ObjectKind;
namespace OSC = Ubiquitty::OSC;

namespace {

/// A folded workspace with everything a workspace row can carry.
OutlineRow workspaceRow()
{
    OutlineRow row;
    row.level = ListedLevel::Workspace;
    row.depth = 0;
    row.id = { ObjectKind::Workspace, 9 };
    row.workspaceState = Ubiquitty::Session::TreeWorkspaceState::RestoredAfterHostRestart;
    row.endsWhenDetached = true;
    row.title = "work";
    row.labelChips = { "nine" };
    row.tags = { { .name = "prod", .color = { .red = 200, .green = 30, .blue = 30, .alpha = 255, .set = true }, .prominence = Ubiquitty::Session::TagProminence::Loud }, { .name = "eu", .color = {}, .prominence = Ubiquitty::Session::TagProminence::Quiet } };
    row.status.kind = StatusKind::Blocked;
    row.status.count = 1;
    row.status.paneCount = 3;
    row.status.blockKind = OSC::ProgramBlockKind::Permission;
    row.status.badgeCount = 2;
    row.status.tabColor = Ubiquitty::ColorRGBA { 1, 2, 3, 255 };
    row.statusSlot = Ubiquitty::Themes::StatusColorSlot::Blocked;
    row.statusColor = { 0x20, 0x40, 0x60, 255 };
    row.attendance = { .people = 2, .agents = 1 };
    row.isExpandable = true;
    row.isExpanded = false;
    row.collapsedSummary = OutlineSummary { .windows = 2, .tabs = 3, .panes = 5, .worstStatus = StatusKind::Blocked, .worstStatusCount = 1, .newestActivity = {} };
    return row;
}

/// An untitled rule tag: a blue dot a reader hears as "Blue".
Ubiquitty::Session::ResolvedTag untitledBlueTag()
{
    Ubiquitty::Session::ResolvedTag tag;
    tag.color = { .red = 105, .green = 179, .blue = 255, .alpha = 255, .set = true };
    tag.source = Ubiquitty::Session::TagSource::Rule;
    tag.ruleIndex = 0;
    tag.rulePattern = "home-*";
    tag.colorName = "blue";
    return tag;
}

/// A titled rule tag whose rule hides its name: drawn as its icon or a dot,
/// and still heard and listed as "acme".
Ubiquitty::Session::ResolvedTag hiddenNameTag()
{
    Ubiquitty::Session::ResolvedTag tag;
    tag.name = "acme";
    tag.color = { .red = 31, .green = 111, .blue = 235, .alpha = 255, .set = true };
    tag.source = Ubiquitty::Session::TagSource::Rule;
    tag.ruleIndex = 1;
    tag.rulePattern = "*.acme.example";
    tag.icon = "buildings";
    tag.showsName = false;
    return tag;
}

/// An unfolded window the host's layout has in front.
OutlineRow windowRow()
{
    OutlineRow row;
    row.level = ListedLevel::Window;
    row.depth = 1;
    row.id = { ObjectKind::Window, 4 };
    row.foldedTab = Ubiquitty::Session::Identity::ObjectID { ObjectKind::Tab, 6 };
    row.title = "ops";
    row.status.kind = StatusKind::Failed;
    row.status.count = 2;
    row.status.paneCount = 4;
    row.statusSlot = Ubiquitty::Themes::StatusColorSlot::Failed;
    row.isHostCurrent = true;
    row.isExpandable = true;
    row.isExpanded = true;
    return row;
}

/// A pane that matches the search, with everything a pane row can carry.
OutlineRow paneRow()
{
    OutlineRow row;
    row.level = ListedLevel::Pane;
    row.depth = 2;
    row.id = { ObjectKind::Pane, 7 };
    row.title = "make all";
    row.secondaryTitle = "make";
    row.status.kind = StatusKind::Working;
    row.status.count = 1;
    row.status.paneCount = 1;
    row.status.progress = { OSC::ProgressNormal, 40 };
    row.status.hasUnseenBell = true;
    row.status.hasUnseenNotification = true;
    row.status.badgeCount = 1;
    row.status.indicatorColor = Ubiquitty::ColorRGBA { 0xAB, 0xCD, 0xEF, 255 };
    row.statusSlot = Ubiquitty::Themes::StatusColorSlot::Working;
    row.badge = "x";
    row.sinceOutput = { .isKnown = true, .seconds = 125 };
    row.sinceInput = { .isKnown = true, .seconds = 3 };
    row.attendance = { .people = 1, .agents = 0 };
    row.program = "make";
    row.directory = "/srv/app";
    row.columns = 132;
    row.rows = 43;
    row.isMatch = true;
    // What the row's actions read.
    row.open.opening = Ubiquitty::Session::AttachOpening::BySession;
    row.open.sessionID = 41;
    row.open.workspaceID = 9;
    row.open.workspaceName = "work";
    row.open.paneID = 7;
    row.own = { { .id = { ObjectKind::Tab, 6 }, .label = "", .tags = {}, .name = "builds" }, { .id = { ObjectKind::Pane, 7 }, .label = "build", .tags = { "prod" }, .name = "" }, { .id = { ObjectKind::Session, 41 }, .label = "", .tags = {}, .name = "" } };
    row.tabPlace = Ubiquitty::App::OutlineTabPlace { .tab = { ObjectKind::Tab, 6 }, .window = { ObjectKind::Window, 4 }, .index = 1, .tabCount = 3, .canMoveEarlier = true, .canMoveLater = true, .acceptedIndices = { 0, 2 }, .otherWindows = { { .window = { ObjectKind::Window, 5 }, .title = "logs", .index = 2 } } };
    row.here = Ubiquitty::App::OutlineHere::Here;
    row.attentionTitle = "Build failed";
    row.attentionBody = "2 errors";
    row.accessory = Ubiquitty::App::TabAccessory::StatusDot;
    row.accessoryInk = Ubiquitty::ColorRGBA { 0xAB, 0xCD, 0xEF, 255 };
    return row;
}

/// A tab of a workspace nothing runs in: no identity, no status.
OutlineRow recordedRow()
{
    OutlineRow row;
    row.level = ListedLevel::Tab;
    row.depth = 1;
    row.recordedKey = OutlineRecordedRowKey { .workspaceID = 5, .level = ListedLevel::Tab, .window = 0, .tab = 1 };
    row.workspaceState = Ubiquitty::Session::TreeWorkspaceState::Restorable;
    row.title = "logs";
    row.labelChips = { "tailing" };
    row.status.paneCount = 3;
    row.isExpandable = true;
    row.isExpanded = false;
    row.collapsedSummary = OutlineSummary { .windows = 0, .tabs = 0, .panes = 3, .worstStatus = StatusKind::None, .worstStatusCount = 0, .newestActivity = {} };
    return row;
}

} // namespace

TEST(anOutlineIsPrintedALineARowWithEveryFieldTheRowHas)
{
    SessionOutline outline;
    outline.revision = 31;
    outline.matchCount = 1;
    outline.rows = { workspaceRow(), windowRow(), paneRow(), recordedRow() };
    const std::string expected = " + work [nine] #prod #eu  blocked 1/3: permission  tab #010203  2 people, 1 agent  2 windows, 3 tabs, 5 panes  restored  ends when detached  workspace:9\n"
                                 "   - *ops  failed 2/4  window:4\n"
                                 ">      make all (make)  working  40%  bell  notified  {x}  /srv/app  132x43  out 2m  in now  1 person  pane:7\n"
                                 "   + logs [tailing]  3 panes  restorable\n";
    const std::string printed = outlineText(outline);
    CHECK_MSG(printed == expected, printed.c_str());
    CHECK(outlineText(SessionOutline {}).empty());
}

TEST(aRowSaysOnlyWhatItHas)
{
    // A bare row is its title and nothing else.
    OutlineRow bare;
    bare.level = ListedLevel::Pane;
    bare.depth = 3;
    bare.title = "zsh";
    SessionOutline outline;
    outline.rows = { bare };
    CHECK_MSG(outlineText(outline) == "         zsh\n", outlineText(outline).c_str());

    // A live workspace says no state; a restorable one says so.
    OutlineRow workspace;
    workspace.title = "kept";
    workspace.id = { ObjectKind::Workspace, 5 };
    workspace.workspaceState = Ubiquitty::Session::TreeWorkspaceState::Restorable;
    outline.rows = { workspace };
    CHECK_MSG(outlineText(outline) == "   kept  restorable  workspace:5\n", outlineText(outline).c_str());
    outline.rows[0].workspaceState = Ubiquitty::Session::TreeWorkspaceState::Live;
    CHECK_MSG(outlineText(outline) == "   kept  workspace:5\n", outlineText(outline).c_str());

    // A row under a workspace nothing runs in says so too: it has no
    // identity to be addressed by, and the word is why. A row under one that
    // was restored and runs again does not.
    OutlineRow under;
    under.level = ListedLevel::Tab;
    under.depth = 1;
    under.title = "logs";
    under.workspaceState = Ubiquitty::Session::TreeWorkspaceState::Restorable;
    outline.rows = { under };
    CHECK_MSG(outlineText(outline) == "     logs  restorable\n", outlineText(outline).c_str());
    outline.rows[0].workspaceState = Ubiquitty::Session::TreeWorkspaceState::RestoredAfterHostRestart;
    CHECK_MSG(outlineText(outline) == "     logs\n", outlineText(outline).c_str());

    // One pane of its kind needs no count; a block that names nothing says
    // only that it is blocked; agents alone are counted alone.
    OutlineRow pane = paneRow();
    pane.isMatch = false;
    pane.depth = 0;
    pane.secondaryTitle.clear();
    pane.status = {};
    pane.status.kind = StatusKind::Blocked;
    pane.status.count = 1;
    pane.status.paneCount = 1;
    pane.badge.clear();
    pane.directory.clear();
    pane.columns = 0;
    pane.sinceOutput = {};
    pane.sinceInput = { .isKnown = true, .seconds = 7200 };
    pane.attendance = { .people = 0, .agents = 2 };
    outline.rows = { pane };
    CHECK_MSG(outlineText(outline) == "   make all  blocked  in 2h  2 agents  pane:7\n", outlineText(outline).c_str());
}

TEST(anUntitledTagIsADotSpokenByItsColour)
{
    // A printed line writes the dot and the words a reader would hear, never
    // a bare "#"; the JSON carries those words for a reader that draws a dot.
    OutlineRow row;
    row.title = "zsh";
    row.tags = { untitledBlueTag() };
    SessionOutline outline;
    outline.rows = { row };
    CHECK_MSG(outlineText(outline) == "   zsh \xE2\x97\x8F"
                                      "Blue\n",
        outlineText(outline).c_str());
    const Ubiquitty::Codec::JSON::Value described = outlineJSON(outline);
    const auto& tag = described.find("rows")->items()[0].find("tags")->items()[0];
    CHECK_MSG(Ubiquitty::Codec::JSON::serialize(tag) == R"json({"name":"","color":"#69b3ff","loud":false,"label":"Blue"})json", Ubiquitty::Codec::JSON::serialize(tag).c_str());
    const std::vector<OutlineDetailLine> details = detailLines(row);
    CHECK(std::find(details.begin(), details.end(), OutlineDetailLine { .label = "Tags", .value = "Blue" }) != details.end());
}

TEST(aTagWhoseRuleHidesItsNameIsDrawnAsADotAndStillNamed)
{
    // A reader that draws chips draws this one as the untitled tag's mark,
    // its icon or a dot, and speaks it by its name; a printed line and the
    // details still name it, since the name is still the tag's.
    OutlineRow row;
    row.title = "zsh";
    row.tags = { hiddenNameTag() };
    SessionOutline outline;
    outline.rows = { row };
    CHECK_MSG(outlineText(outline) == "   zsh #acme\n", outlineText(outline).c_str());
    const Ubiquitty::Codec::JSON::Value described = outlineJSON(outline);
    const auto& tag = described.find("rows")->items()[0].find("tags")->items()[0];
    CHECK_MSG(Ubiquitty::Codec::JSON::serialize(tag) == R"json({"name":"acme","color":"#1f6feb","loud":false,"icon":"buildings","iconWeight":"fill","showName":false,"label":"acme"})json", Ubiquitty::Codec::JSON::serialize(tag).c_str());
    const std::vector<OutlineDetailLine> details = detailLines(row);
    CHECK(std::find(details.begin(), details.end(), OutlineDetailLine { .label = "Tags", .value = "acme" }) != details.end());

    // Shown, the same tag carries neither field.
    row.tags[0].showsName = true;
    outline.rows = { row };
    const Ubiquitty::Codec::JSON::Value shown = outlineJSON(outline);
    CHECK_MSG(Ubiquitty::Codec::JSON::serialize(shown.find("rows")->items()[0].find("tags")->items()[0]) == R"json({"name":"acme","color":"#1f6feb","loud":false,"icon":"buildings","iconWeight":"fill"})json",
        Ubiquitty::Codec::JSON::serialize(shown.find("rows")->items()[0].find("tags")->items()[0]).c_str());
}

TEST(agesProgressAndCountsAreWordedOneWay)
{
    CHECK(ageWord(0) == "now" && ageWord(4) == "now");
    CHECK(ageWord(5) == "5s" && ageWord(59) == "59s");
    CHECK(ageWord(60) == "1m" && ageWord(3599) == "59m");
    CHECK(ageWord(3600) == "1h" && ageWord(86'399) == "23h");
    CHECK(ageWord(86'400) == "1d" && ageWord(10 * 86'400) == "10d");

    using SessionOutlineTextDetail::progressWords;
    CHECK(progressWords({ OSC::ProgressNone, 50 }).empty());
    CHECK(progressWords({ OSC::ProgressNormal, 0 }) == "0%");
    CHECK(progressWords({ OSC::ProgressPaused, 70 }) == "paused 70%");
    CHECK(progressWords({ OSC::ProgressError, 70 }) == "progress error");
    CHECK(progressWords({ OSC::ProgressIndeterminate, 0 }) == "in progress");

    using SessionOutlineTextDetail::attendanceWords;
    CHECK(attendanceWords({}).empty());
    CHECK(attendanceWords({ .people = 1, .agents = 1 }) == "1 person, 1 agent");
    CHECK(attendanceWords({ .people = 3, .agents = 0 }) == "3 people");

    // A drawn row says its workspace's state on the workspace's own row and
    // only when it is not live; the rows under it are drawn as that state,
    // not labelled with it.
    using SessionOutlineTextDetail::stateWords;
    OutlineRow stated;
    CHECK(stateWords(stated).empty());
    stated.workspaceState = Ubiquitty::Session::TreeWorkspaceState::Restorable;
    CHECK(stateWords(stated) == "restorable");
    stated.workspaceState = Ubiquitty::Session::TreeWorkspaceState::RestoredAfterHostRestart;
    CHECK(stateWords(stated) == "restored");
    stated.level = ListedLevel::Tab;
    CHECK(stateWords(stated).empty());

    // Every kind, slot, level and state has a word, and no two share one.
    for (const StatusKind kind : { StatusKind::Idle, StatusKind::Running, StatusKind::Working, StatusKind::Done, StatusKind::Attention, StatusKind::Blocked, StatusKind::Failed })
        CHECK(!statusKindWord(kind).empty());
    CHECK(statusKindWord(StatusKind::None).empty());
    CHECK(statusKindWord(StatusKind::Failed) == "failed" && statusKindWord(StatusKind::Attention) == "attention");
    CHECK(blockKindWord(OSC::ProgramBlockKind::Auth) == "credential" && blockKindWord(OSC::ProgramBlockKind::Question) == "question");
    CHECK(statusSlotWord(Ubiquitty::Themes::StatusColorSlot::Progress) == "progress" && statusSlotWord(Ubiquitty::Themes::StatusColorSlot::Neutral) == "neutral");
    CHECK(colorWord(0, 0x0f, 0xff) == "#000fff");
}

TEST(anOutlineIsSpelledAsJSONWithEveryFieldNamed)
{
    SessionOutline outline;
    outline.revision = 31;
    outline.matchCount = 1;
    outline.rows = { workspaceRow(), paneRow(), recordedRow() };
    const Ubiquitty::Codec::JSON::Value described = outlineJSON(outline);
    CHECK_EQ(static_cast<int>(described.numberOr("revision")), 31);
    CHECK_EQ(static_cast<int>(described.numberOr("matchCount")), 1);
    const auto* rows = described.find("rows");
    REQUIRE(rows && rows->isArray() && rows->items().size() == 3);

    const std::string workspace = Ubiquitty::Codec::JSON::serialize(rows->items()[0]);
    const std::string expectedWorkspace = R"json({"level":"workspace","depth":0,"id":"workspace:9","state":"restored","endsWhenDetached":true,"title":"work","secondaryTitle":"","labels":["nine"],"tags":[{"name":"prod","color":"#c81e1e","loud":true},{"name":"eu","loud":false}],"status":{"kind":"blocked","count":1,"panes":3,"blockKind":"permission","bell":false,"notification":false,"badges":2,"tabColor":"#010203"},"statusSlot":"blocked","statusColor":"#204060","badge":"","attendance":{"people":2,"agents":1},"program":"","directory":"","columns":0,"rows":0,"hostCurrent":false,"expandable":true,"expanded":false,"collapsed":{"windows":2,"tabs":3,"panes":5,"worstStatus":"blocked","worstStatusCount":1},"match":false,"open":{"how":"cannot","workspaceName":""},"own":[],"accessory":"none","details":[{"label":"Status","value":"blocked 1/3: permission"},{"label":"Attached","value":"2 people, 1 agent"},{"label":"Labels","value":"nine"},{"label":"Tags","value":"prod, eu"},{"label":"State","value":"restored"}],"words":{"status":"blocked 1/3: permission","attendance":"2 people, 1 agent","collapsed":"2 windows, 3 tabs, 5 panes","state":"restored","secondLine":"blocked 1/3: permission"}})json";
    CHECK_MSG(workspace == expectedWorkspace, workspace.c_str());

    const std::string pane = Ubiquitty::Codec::JSON::serialize(rows->items()[1]);
    const std::string expectedPane = R"json({"level":"pane","depth":2,"id":"pane:7","state":"live","endsWhenDetached":false,"title":"make all","secondaryTitle":"make","labels":[],"tags":[],"status":{"kind":"working","count":1,"panes":1,"progress":{"state":"normal","value":40},"bell":true,"notification":true,"badges":1,"indicatorColor":"#abcdef"},"statusSlot":"working","statusColor":"#000000","badge":"x","sinceOutputSeconds":125,"sinceInputSeconds":3,"attendance":{"people":1,"agents":0},"program":"make","directory":"/srv/app","columns":132,"rows":43,"hostCurrent":false,"expandable":false,"expanded":false,"match":true,"open":{"how":"session","session":"session:41","pane":"pane:7","workspace":"workspace:9","workspaceName":"work"},"own":[{"id":"tab:6","label":"","tags":[],"name":"builds"},{"id":"pane:7","label":"build","tags":["prod"],"name":""},{"id":"session:41","label":"","tags":[],"name":""}],"tabPlace":{"tab":"tab:6","window":"window:4","index":1,"tabCount":3,"canMoveEarlier":true,"canMoveLater":true,"acceptedIndices":[0,2],"otherWindows":[{"window":"window:5","title":"logs","index":2}]},"attention":{"title":"Build failed","body":"2 errors"},"accessory":"statusDot","here":"is","accessoryInk":"#abcdef","details":[{"label":"Program","value":"make"},{"label":"Directory","value":"/srv/app"},{"label":"Size","value":"132×43"},{"label":"Status","value":"working"},{"label":"Progress","value":"40%"},{"label":"Notification","value":"Build failed: 2 errors"},{"label":"Attached","value":"1 person"},{"label":"Last output","value":"2m ago"},{"label":"Last input","value":"just now"}],"words":{"status":"working","progress":"40%","sinceOutput":"2m","sinceInput":"now","attendance":"1 person","secondLine":"Build failed"}})json";
    CHECK_MSG(pane == expectedPane, pane.c_str());

    // A row with no identity of its own names its place in the record.
    const Ubiquitty::Codec::JSON::Value& recorded = rows->items()[2];
    CHECK(recorded.find("id") == nullptr);
    const auto* key = recorded.find("recorded");
    REQUIRE(key != nullptr);
    CHECK_MSG(Ubiquitty::Codec::JSON::serialize(*key) == R"json({"workspace":5,"level":"tab","window":0,"tab":1})json", Ubiquitty::Codec::JSON::serialize(*key).c_str());
    CHECK(recorded.stringOr("state") == "restorable");

    // A folded row names what it stands for.
    outline.rows = { windowRow() };
    const Ubiquitty::Codec::JSON::Value window = outlineJSON(outline);
    const auto* folded = window.find("rows")->items()[0].find("folded");
    REQUIRE(folded != nullptr);
    CHECK_MSG(Ubiquitty::Codec::JSON::serialize(*folded) == R"json({"tab":"tab:6"})json", Ubiquitty::Codec::JSON::serialize(*folded).c_str());
    CHECK(window.find("rows")->items()[0].boolOr("hostCurrent") && window.find("rows")->items()[0].boolOr("expanded"));
}

// The rows the web page's renderer is tested against are these, as Core
// spells them: the fixture is this outline's JSON, byte for byte. A field
// Core renames, drops or words differently fails here, and the fixture that
// replaces it is what the page's test then reads. Run with
// UBQ_WRITE_OUTLINE_FIXTURE=1 to write the fixture from the rows above.
TEST(theRowsTheWebPageIsTestedAgainstAreTheOnesCoreSpells)
{
    SessionOutline outline;
    outline.revision = 31;
    outline.matchCount = 1;
    OutlineRow marked = paneRow();
    marked.id = { ObjectKind::Pane, 8 };
    marked.title = "<b>bold</b> & co";
    marked.isMatch = false;
    marked.status = {};
    marked.status.kind = StatusKind::Idle;
    marked.status.count = 1;
    marked.status.paneCount = 1;
    marked.badge.clear();
    marked.secondaryTitle.clear();
    marked.tags = { untitledBlueTag(), hiddenNameTag() };
    outline.rows = { workspaceRow(), windowRow(), paneRow(), marked, recordedRow() };
    const std::string spelled = Ubiquitty::Codec::JSON::serialize(outlineJSON(outline)) + "\n";
    const char* path = UBQ_OUTLINE_ROWS_FIXTURE;
    if (std::getenv("UBQ_WRITE_OUTLINE_FIXTURE")) {
        std::FILE* out = std::fopen(path, "wb");
        REQUIRE(out != nullptr);
        std::fwrite(spelled.data(), 1, spelled.size(), out);
        std::fclose(out);
    }
    std::string recorded;
    if (std::FILE* in = std::fopen(path, "rb")) {
        std::array<char, 4096> chunk {};
        for (std::size_t got = std::fread(chunk.data(), 1, chunk.size(), in); got > 0; got = std::fread(chunk.data(), 1, chunk.size(), in))
            recorded.append(chunk.data(), got);
        std::fclose(in);
    }
    CHECK_MSG(recorded == spelled, ("Tests/Core/App/outline-rows.json is not what Core spells for these rows; it would be:\n" + spelled).c_str());
}

// A program's bytes and a person's reach an outline as text: whatever a tree
// was built from, the lines are UTF-8 and the JSON is a document a reader
// decodes.
TEST(anOutlineOfIllFormedBytesIsStillTextAndStillJSON)
{
    const auto isText = [](std::string_view text) { return Ubiquitty::Protocol::Sync::isValidUTF8({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }); };
    const Ubiquitty::Session::SessionTree tree = UbiquittyTest::illFormedTree();
    const std::vector<Ubiquitty::Preferences::TagEntry> rules { { .name = "bad\xFFrule", .color = "red", .matchHost = "", .matchProfile = "", .prominence = "" } };
    OutlineConfiguration configuration;
    for (Ubiquitty::Preferences::Effective::TitleComposition* composition : { &configuration.tabTitle, &configuration.paneTitle }) {
        composition->showsAppTitle = true;
        composition->showsDirectory = true;
        composition->showsDirectoryFullPath = true;
        composition->showsProcess = true;
        composition->showsProcessArguments = true;
        composition->showsShellCommand = true;
        composition->showsTTYName = true;
        composition->showsProgramStatus = true;
    }
    configuration.tagRules = &rules;
    // With no search, with one that finds every row, and with one for the
    // byte itself, which no row holds any more.
    for (const char* query : { "", "bad", "\xFF" }) {
        configuration.query = query;
        const SessionOutline outline = buildSessionOutline(tree, configuration);
        const std::string printed = outlineText(outline);
        CHECK_MSG(isText(printed), "the lines of an outline are not UTF-8");
        if (std::string_view(query) == "\xFF") {
            CHECK(outline.rows.empty());
            continue;
        }
        REQUIRE(outline.rows.size() >= 3);
        CHECK(printed.find("bad\xEF\xBF\xBDworkspace") != std::string::npos);
        const std::string spelled = Ubiquitty::Codec::JSON::serialize(outlineJSON(outline));
        CHECK_MSG(isText(spelled), "the JSON of an outline is not UTF-8");
        bool isJSON = false; // bool-ok: the parser's out flag
        const Ubiquitty::Codec::JSON::Value read = Ubiquitty::Codec::JSON::parse(spelled, isJSON);
        REQUIRE(isJSON);
        REQUIRE(read.find("rows") != nullptr);
        CHECK_EQ(read.find("rows")->items().size(), outline.rows.size());
        CHECK_STR_EQ(read.find("rows")->items().at(0).stringOr("title"), "bad\xEF\xBF\xBDworkspace");
        for (const OutlineRow& row : outline.rows) {
            for (const std::string* text : { &row.title, &row.secondaryTitle, &row.badge, &row.program, &row.directory })
                CHECK_MSG(isText(*text), text->c_str());
            for (const std::string& chip : row.labelChips)
                CHECK_MSG(isText(chip), chip.c_str());
        }
    }
}

// The two spellings are of the rows an outline builds, one each, in order.
TEST(aBuiltOutlinePrintsALineAndAnObjectForEachRow)
{
    using namespace Ubiquitty::Session;
    auto layout = std::make_unique<Layout::Workspace>();
    const Layout::SurfaceID surface = layout->surfaceIDAt(0);
    const Layout::PaneID first = layout->focusedPane(surface);
    const Layout::PaneID second = layout->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Vertical);
    std::map<std::uint32_t, UbiquittyTest::StatedPane> panes;
    panes[std::to_underlying(first)].title.programTitle = "make all";
    panes[std::to_underlying(first)].status.programState = OSC::ProgramState::Working;
    panes[std::to_underlying(second)].title.programTitle = "vim notes";
    panes[std::to_underlying(second)].status.isAtPrompt = true;
    const std::array sources { TreeWorkspaceSource { 9, "work", layout.get() } };
    const SessionTree tree = buildSessionTree({ .revision = 4, .nowUnixMilliseconds = 100'000, .workspaces = sources }, Identity::LabelBook {},
        [&](std::uint32_t, Layout::PaneID id) { return panes[std::to_underlying(id)].treeFacts(); });
    OutlineConfiguration configuration;
    configuration.tabTitle.showsAppTitle = true;
    configuration.paneTitle.showsAppTitle = true;
    const SessionOutline outline = buildSessionOutline(tree, configuration);
    REQUIRE(outline.rows.size() == 3);

    const std::string printed = outlineText(outline);
    CHECK_EQ(std::ranges::count(printed, '\n'), 3);
    CHECK_MSG(printed.find("work") != std::string::npos && printed.find("make all  working") != std::string::npos && printed.find("vim notes  idle") != std::string::npos, printed.c_str());
    CHECK(printed.find(Identity::format({ ObjectKind::Pane, std::to_underlying(second) })) != std::string::npos);

    const Ubiquitty::Codec::JSON::Value described = outlineJSON(outline);
    CHECK_EQ(static_cast<int>(described.numberOr("revision")), 4);
    REQUIRE(described.find("rows") && described.find("rows")->items().size() == 3);
    CHECK(described.find("rows")->items()[1].stringOr("title") == outline.rows[1].title);
    CHECK(described.find("rows")->items()[2].stringOr("id") == Identity::format(outline.rows[2].id));
}

UBIQUITTY_TEST_MAIN()
