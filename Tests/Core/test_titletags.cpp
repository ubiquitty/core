// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_titletags.cpp — pins Title::resolveTags, the `[[tags]]` rule half of
// window tags: which declared markers a window wears given the host it is
// connected to and the profile applied to it.
//
// The rule source is the one that carries a safety signal, because nothing has
// to remember to apply it. So what is pinned here is mostly what must not
// match: an unconstrained entry marking every window, a glob matching more
// than it says, or a name resolving to a colour it was not given.

#include "Core/Session/Tags.h"

#include <sstream>
#include <string>
#include <vector>

using Ubiquitty::Preferences::TagEntry;
using Ubiquitty::Session::appendTags;
using Ubiquitty::Session::firstMatchedTagRule;
using Ubiquitty::Session::isValidTagRule;
using Ubiquitty::Session::PaneHost;
using Ubiquitty::Session::removeTagRule;
using Ubiquitty::Session::resolveManualTag;
using Ubiquitty::Session::resolveTags;
using Ubiquitty::Session::resolveWindowTags;
using Ubiquitty::Session::serializeTag;
using Ubiquitty::Session::tagColorFromName;
using Ubiquitty::Session::tagNamesEqual;
using Ubiquitty::Session::tagPatternMatches;
using Ubiquitty::Session::TagProminence;
using Ubiquitty::Session::TagSource;
using Ubiquitty::Session::tagSpokenName;
using Ubiquitty::Session::writeTagRule;

static TagEntry makeRule(std::string name, std::string color, std::string host, std::string profile)
{
    TagEntry entry;
    entry.name = std::move(name);
    entry.color = std::move(color);
    entry.matchHost = std::move(host);
    entry.matchProfile = std::move(profile);
    return entry;
}

TEST(titleTagsMatchGlobsWithoutCaseOrOverreach)
{
    CHECK(tagPatternMatches("prod-*", "prod-web-01"));
    CHECK(tagPatternMatches("prod-*", "PROD-WEB-01")); // a host name is not case-sensitive
    CHECK(tagPatternMatches("*", "anything"));
    CHECK(tagPatternMatches("*.example.com", "db.example.com"));
    CHECK(tagPatternMatches("prod-*-01", "prod-web-01"));
    CHECK(tagPatternMatches("exact", "exact"));

    // A prefix rule must not claim a host that merely contains it, which is
    // what would mark a staging box as production.
    CHECK(!tagPatternMatches("prod-*", "staging-prod-web"));
    CHECK(!tagPatternMatches("prod-*", "prod"));
    CHECK(!tagPatternMatches("exact", "exactly"));
    CHECK(!tagPatternMatches("*.example.com", "example.com"));
    CHECK(!tagPatternMatches("prod-*", ""));

    // Many stars must stay linear rather than backtrack exponentially. The
    // pattern ends in a literal the value never supplies, so the matcher has
    // to exhaust every split before it can answer.
    CHECK(!tagPatternMatches("*a*a*a*a*a*a*z", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"));
}

TEST(titleTagsResolveNamedColorsAndHexAlike)
{
    Ubiquitty::Preferences::ColorEntry color {};
    CHECK(tagColorFromName("red", color));
    CHECK(color.set && color.red == 252 && color.green == 122 && color.blue == 107);
    CHECK(tagColorFromName("RED", color)); // the config author's spelling is not a fact about the colour
    CHECK(color.red == 252);
    CHECK(tagColorFromName("grey", color) && color.red == 181);
    CHECK(tagColorFromName("gray", color) && color.red == 181); // both spellings, unlike the system's
    CHECK(!tagColorFromName("chartreuse", color));

    const std::vector<TagEntry> rules { makeRule("literal", "#102030", "*", "") };
    const auto resolved = resolveTags(rules, "anything", "");
    CHECK(resolved.size() == 1);
    CHECK(resolved[0].color.set);
    CHECK(resolved[0].color.red == 0x10 && resolved[0].color.green == 0x20 && resolved[0].color.blue == 0x30);
}

TEST(titleTagsMatchOnHostProfileOrBoth)
{
    const std::vector<TagEntry> rules {
        makeRule("production", "red", "prod-*", ""),
        makeRule("staging", "yellow", "", "Staging"),
        makeRule("prod-staging", "purple", "prod-*", "Staging"),
    };

    const auto onProduction = resolveTags(rules, "prod-web-01", "Default");
    CHECK(onProduction.size() == 1);
    CHECK(onProduction[0].name == "production");

    const auto onStagingProfile = resolveTags(rules, "dev-box", "Staging");
    CHECK(onStagingProfile.size() == 1);
    CHECK(onStagingProfile[0].name == "staging");

    // Both constraints present means both must hold, so this wears all three.
    const auto onBoth = resolveTags(rules, "prod-web-01", "Staging");
    CHECK(onBoth.size() == 3);

    // A local window names no host and matches only what asks for no host.
    const auto local = resolveTags(rules, "", "Default");
    CHECK(local.empty());
}

TEST(titleTagsRefuseToMarkEveryWindowByAccident)
{
    // An entry that constrains nothing declares a colour for the other tag
    // sources to use by name. Matching every window would make the marker
    // meaningless exactly where it matters most.
    const std::vector<TagEntry> unconstrained { makeRule("everywhere", "red", "", "") };
    CHECK(resolveTags(unconstrained, "prod-web-01", "Default").empty());
    CHECK(resolveTags(unconstrained, "", "").empty());

    // An entry with neither a name nor a colour draws nothing and is not a
    // tag; a name of only blanks is no name.
    const std::vector<TagEntry> empty { makeRule("", "", "*", ""), makeRule("  ", "magenta", "*", "") };
    CHECK(resolveTags(empty, "anything", "").empty());

    // A repeated name resolves once, keeping the first declaration's colour.
    const std::vector<TagEntry> duplicated {
        makeRule("production", "red", "prod-*", ""),
        makeRule("production", "blue", "*", ""),
    };
    const auto resolved = resolveTags(duplicated, "prod-web-01", "");
    CHECK(resolved.size() == 1);
    CHECK(resolved[0].color.red == 252);

    // A tag may name something without claiming a colour.
    const std::vector<TagEntry> colorless { makeRule("audited", "", "*", "") };
    const auto plain = resolveTags(colorless, "host", "");
    CHECK(plain.size() == 1);
    CHECK(!plain[0].color.set);
}

// A machine reached under several names, its own hostname and a tailnet
// name, is named by one rule: each pattern of a key may match, written as an
// array or separated by commas, and the rule is written back as the array.
TEST(aTagRuleMatchesAnyOfSeveralHostPatterns)
{
    const std::string text = "[[tags]]\n"
                             "name = \"laptop\"\n"
                             "color = \"blue\"\n"
                             "match_host = [\"studio-mac*\", \"studio\", \" \"]\n"
                             "match_profile = \"Work, Home\"\n";
    std::istringstream in(text);
    Ubiquitty::Preferences::Values configuration;
    CHECK(Ubiquitty::Preferences::parse(in, configuration, Ubiquitty::Preferences::ParseMode::Strict));
    CHECK(configuration.tags.size() == 1);
    if (configuration.tags.size() != 1)
        return;
    const TagEntry& rule = configuration.tags[0];
    CHECK_STR_EQ(rule.matchHost, "studio-mac*, studio");
    CHECK(resolveTags(configuration.tags, "studio-mac.tailnet.example", "Work").size() == 1);
    CHECK(resolveTags(configuration.tags, "STUDIO", "home").size() == 1);
    CHECK(resolveTags(configuration.tags, "build-01", "Work").empty());
    CHECK(resolveTags(configuration.tags, "studio", "Default").empty());
    CHECK_STR_EQ(Ubiquitty::Session::tagRulePattern(rule), "studio-mac*, studio (profile Work, Home)");

    const std::string written = serializeTag(rule);
    CHECK_MSG(written.find("match_host = [\"studio-mac*\", \"studio\"]\n") != std::string::npos, written.c_str());
    CHECK_MSG(written.find("match_profile = [\"Work\", \"Home\"]\n") != std::string::npos, written.c_str());
    std::istringstream reread(written);
    Ubiquitty::Preferences::Values again;
    CHECK(Ubiquitty::Preferences::parse(reread, again, Ubiquitty::Preferences::ParseMode::Strict));
    CHECK(again.tags.size() == 1 && again.tags[0].matchHost == rule.matchHost && again.tags[0].matchProfile == "Work, Home");

    // Commas and spaces alone are no pattern: an untitled rule needs a real
    // one to mean anything.
    TagEntry onlySeparators;
    onlySeparators.color = "red";
    onlySeparators.matchHost = " , ,";
    CHECK(!isValidTagRule(onlySeparators));
}

TEST(titleTagsParseFromTheConfigFile)
{
    // The seam between the file and the resolver: a `[[tags]]` block is an
    // array of tables like `[[themes]]`, and a following `[section]` must end
    // it rather than keep absorbing keys.
    const std::string text = "[[tags]]\n"
                             "name = \"production\"\n"
                             "color = \"red\"\n"
                             "match_host = \"prod-*\"\n"
                             "\n"
                             "[[tags]]\n"
                             "name = \"staging\"\n"
                             "color = \"yellow\"\n"
                             "match_profile = \"Staging\"\n"
                             "\n"
                             "[appearance]\n"
                             "folder_icon = false\n";
    std::istringstream in(text);
    Ubiquitty::Preferences::Values configuration;
    CHECK(Ubiquitty::Preferences::parse(in, configuration, Ubiquitty::Preferences::ParseMode::Strict));
    CHECK(configuration.tags.size() == 2);
    CHECK(configuration.tags[0].name == "production");
    CHECK(configuration.tags[0].color == "red");
    CHECK(configuration.tags[0].matchHost == "prod-*");
    CHECK(configuration.tags[0].matchProfile.empty());
    CHECK(configuration.tags[1].name == "staging");
    CHECK(configuration.tags[1].matchProfile == "Staging");
    // The section after the block landed where it belongs, not in a tag.
    CHECK(configuration.folderIcon.has_value() && configuration.folderIcon.value() == false);

    const auto resolved = resolveTags(configuration.tags, "prod-web-01", "Default");
    CHECK(resolved.size() == 1);
    CHECK(resolved[0].name == "production");
    CHECK(resolved[0].color.red == 252);
}

TEST(titleTagNamesCompareAsNamesNotPatterns)
{
    CHECK(tagNamesEqual("production", "PRODUCTION")); // a name's case is not identity
    CHECK(tagNamesEqual("", ""));
    CHECK(!tagNamesEqual("prod", "production"));

    // A star inside a name is a character of the name, never a glob: a tag
    // literally called "prod*" must not swallow "prodX".
    CHECK(tagNamesEqual("prod*", "prod*"));
    CHECK(!tagNamesEqual("prod*", "prodX"));
}

TEST(titleTagsDeduplicateNamesWithoutCase)
{
    // Two rules differing only in case are one tag to every reader; the first
    // declaration wins, exactly as it does for two identical spellings.
    const std::vector<TagEntry> rules {
        makeRule("Prod", "red", "*", ""),
        makeRule("prod", "blue", "*", ""),
    };
    const auto resolved = resolveTags(rules, "any-host", "");
    CHECK(resolved.size() == 1);
    CHECK(resolved[0].name == "Prod");
    CHECK(resolved[0].color.red == 252);
}

TEST(titleManualTagsResolveByDeclarationThenPaletteThenBare)
{
    std::vector<TagEntry> rules { makeRule("critical", "#102030", "", "") };
    rules[0].prominence = "loud";

    // A declared entry with no match constraint exists exactly for this: it
    // names the colour and prominence a hand-set tag of that name wears.
    const auto declared = resolveManualTag(rules, "CRITICAL");
    CHECK(declared.name == "CRITICAL"); // the user's spelling survives
    CHECK(declared.color.set && declared.color.red == 0x10 && declared.color.blue == 0x30);
    CHECK(declared.prominence == TagProminence::Loud);

    // No declaration: the name itself may be one of the seven system colours.
    const auto palette = resolveManualTag(rules, "red");
    CHECK(palette.color.set && palette.color.red == 252);
    CHECK(palette.prominence == TagProminence::Quiet);

    // Neither: the tag names something without claiming a colour.
    const auto bare = resolveManualTag(rules, "experiments");
    CHECK(bare.name == "experiments");
    CHECK(!bare.color.set);
}

TEST(titleTagSourcesUnionInTrustOrder)
{
    const std::vector<TagEntry> rules { makeRule("production", "red", "prod-*", "") };
    auto tags = resolveTags(rules, "prod-web-01", "");
    CHECK(tags.size() == 1);

    // A hand-set duplicate of a rule tag must not double the marker — the
    // rule's resolution wins because it was appended first — while a new name
    // joins the list after it.
    appendTags(tags, { resolveManualTag(rules, "PRODUCTION"), resolveManualTag(rules, "staging") });
    CHECK(tags.size() == 2);
    CHECK(tags[0].name == "production");
    CHECK(tags[0].color.set && tags[0].color.red == 252);
    CHECK(tags[1].name == "staging");
}

static Ubiquitty::Preferences::Values parseConfiguration(const std::string& text)
{
    std::istringstream in(text);
    Ubiquitty::Preferences::Values configuration;
    CHECK(Ubiquitty::Preferences::parse(in, configuration, Ubiquitty::Preferences::ParseMode::Strict));
    return configuration;
}

TEST(untitledTagRuleParsesResolvesAndSpeaks)
{
    // A colour and no title is a rule: a dot of that colour. A missing name
    // and an empty one are the same rule.
    const auto configuration = parseConfiguration("[[tags]]\n"
                                                  "color = \"blue\"\n"
                                                  "match_host = \"home-*\"\n"
                                                  "\n"
                                                  "[[tags]]\n"
                                                  "name = \"\"\n"
                                                  "color = \"#123456\"\n"
                                                  "match_profile = \"Work\"\n");
    CHECK(configuration.tags.size() == 2);
    CHECK(configuration.tags[0].name.empty() && configuration.tags[0].color == "blue");
    CHECK(isValidTagRule(configuration.tags[0]));
    CHECK(isValidTagRule(configuration.tags[1]));
    CHECK(!isValidTagRule(makeRule("", "", "*", "")));
    CHECK(!isValidTagRule(makeRule("", "not-a-colour", "*", "")));
    CHECK(isValidTagRule(makeRule("audited", "", "*", "")));
    // An untitled entry that matches nothing can neither mark a window by
    // rule nor be set by hand.
    CHECK(!isValidTagRule(makeRule("", "blue", "", "")));
    CHECK(isValidTagRule(makeRule("", "blue", "", "Work")));

    const auto home = resolveTags(configuration.tags, "home-mini", "Work");
    CHECK(home.size() == 2);
    CHECK(home[0].isUntitled() && home[0].color.set && home[0].color.blue == 255);
    CHECK(home[0].source == TagSource::Rule && home[0].ruleIndex == 0 && home[0].rulePattern == "home-*");
    // A VoiceOver user hears the colour, or the rule's pattern where the
    // colour has no name.
    CHECK(tagSpokenName(home[0]) == "Blue");
    CHECK(home[1].isUntitled() && home[1].colorName.empty());
    CHECK(tagSpokenName(home[1]) == "Work");
    CHECK(tagSpokenName(resolveManualTag(configuration.tags, "release")) == "release");

    // Two untitled rules of one colour are one dot; another colour is another
    // dot; an untitled rule never hides a titled tag.
    const std::vector<TagEntry> dots {
        makeRule("", "red", "*", ""),
        makeRule("", "RED", "prod-*", ""),
        makeRule("", "green", "*", ""),
        makeRule("production", "red", "*", ""),
    };
    const auto drawn = resolveTags(dots, "prod-web-01", "");
    CHECK(drawn.size() == 3);
    CHECK(drawn[0].isUntitled() && drawn[0].ruleIndex == 0);
    CHECK(drawn[1].isUntitled() && drawn[1].colorName == "green");
    CHECK(drawn[2].name == "production");

    // A hand-set name never borrows an untitled rule's colour.
    const auto manual = resolveManualTag(dots, "");
    CHECK(!manual.color.set);
}

TEST(windowTagsCarryTheirSourceAndTheFirstMatchedRule)
{
    std::vector<TagEntry> rules {
        makeRule("critical", "orange", "", ""),
        makeRule("", "blue", "home-*", ""),
        makeRule("production", "red", "home-*", ""),
    };
    PaneHost host;
    host.place = Ubiquitty::Session::HostPlace::Remote;
    host.machine = "home-mini";
    const auto tags = resolveWindowTags(rules, host, "", { "critical", "  ", "Production", "experiments" });
    CHECK(tags.size() == 4);
    CHECK(tags[0].isUntitled() && tags[0].source == TagSource::Rule && tags[0].ruleIndex == 1);
    CHECK(tags[1].name == "production" && tags[1].source == TagSource::Rule);
    // The hand-set copy of a rule's name is the rule's; a blank name is
    // dropped; the rest are the reader's own.
    CHECK(tags[2].name == "critical" && tags[2].source == TagSource::Manual && tags[2].ruleIndex == 0);
    CHECK(tags[2].colorName == "orange");
    CHECK(tags[3].name == "experiments" && tags[3].source == TagSource::Manual && tags[3].ruleIndex == -1);
    CHECK(firstMatchedTagRule(tags) == 1);
    CHECK(firstMatchedTagRule(resolveWindowTags(rules, PaneHost {}, "", { "critical" })) == -1);
}

TEST(untitledTagRuleRoundTripsThroughTheSettingsWriteBack)
{
    // The untitled rule is written without a name line, and reads back as the
    // rule it was.
    TagEntry dot = makeRule("", "blue", "home-*", "");
    dot.prominence = "loud";
    const std::string block = serializeTag(dot);
    CHECK(block == "[[tags]]\ncolor = \"blue\"\nmatch_host = \"home-*\"\nprominence = \"loud\"\n");
    CHECK(serializeTag(makeRule(" ", "red", "*", "")).find("name") == std::string::npos);
    const auto reread = parseConfiguration(block);
    CHECK(reread.tags.size() == 1);
    CHECK(reread.tags[0].name.empty() && reread.tags[0].color == "blue" && reread.tags[0].matchHost == "home-*");
    CHECK(reread.tags[0].prominence == "loud");

    // The Tags pane addresses a rule by its position: two untitled rules have no
    // names to tell them apart. A commented-out block is not a position.
    const std::string document = "# [[tags]]\n"
                                 "# name = \"example\"\n"
                                 "\n"
                                 "[[tags]]\n"
                                 "color = \"red\"\n"
                                 "match_host = \"a-*\"\n"
                                 "\n"
                                 "[[ tags ]]\n"
                                 "color = \"red\"\n"
                                 "match_host = \"b-*\"\n"
                                 "\n"
                                 "[appearance]\n"
                                 "folder_icon = false\n";
    CHECK(parseConfiguration(document).tags.size() == 2);

    const std::string edited = writeTagRule(document, 1, makeRule("", "green", "b-*", ""));
    auto parsed = parseConfiguration(edited);
    CHECK(parsed.tags.size() == 2);
    CHECK(parsed.tags[0].color == "red" && parsed.tags[0].matchHost == "a-*");
    CHECK(parsed.tags[1].color == "green" && parsed.tags[1].matchHost == "b-*" && parsed.tags[1].name.empty());
    CHECK(parsed.folderIcon.has_value() && parsed.folderIcon.value() == false);
    CHECK(edited.find("# name = \"example\"") != std::string::npos);

    // Giving the first a title keeps its place; appending adds a third.
    const std::string titled = writeTagRule(edited, 0, makeRule("home", "red", "a-*", ""));
    const std::string appended = writeTagRule(titled, -1, makeRule("", "purple", "c-*", ""));
    parsed = parseConfiguration(appended);
    CHECK(parsed.tags.size() == 3);
    CHECK(parsed.tags[0].name == "home" && parsed.tags[1].color == "green" && parsed.tags[2].color == "purple");

    // Removing by position takes that block alone.
    parsed = parseConfiguration(removeTagRule(appended, 1));
    CHECK(parsed.tags.size() == 2);
    CHECK(parsed.tags[0].name == "home" && parsed.tags[1].color == "purple");
    CHECK(removeTagRule(appended, 7) == appended);
    CHECK(removeTagRule(appended, -1) == appended);
}

TEST(tagIconsParseResolveAndWriteBack)
{
    // An icon rides with the rule that names it: to the window by rule, to a
    // hand-set tag of the same name, and back to the file in the weight it was
    // given. Fill is the default, so only a regular icon writes its weight.
    using Ubiquitty::Session::TagIconWeight;
    const auto configuration = parseConfiguration("[[tags]]\n"
                                                  "name = \"production\"\n"
                                                  "color = \"red\"\n"
                                                  "match_host = \"prod-*\"\n"
                                                  "icon = \"flame\"\n"
                                                  "\n"
                                                  "[[tags]]\n"
                                                  "name = \"lab\"\n"
                                                  "color = \"#336699\"\n"
                                                  "icon = \"flask\"\n"
                                                  "icon_weight = \"Regular\"\n");
    CHECK(configuration.tags.size() == 2);
    CHECK(configuration.tags[0].icon == "flame" && configuration.tags[0].iconWeight.empty());
    CHECK(configuration.tags[1].icon == "flask" && configuration.tags[1].iconWeight == "Regular");

    const auto byRule = resolveTags(configuration.tags, "prod-db-02", "");
    CHECK(byRule.size() == 1);
    CHECK(byRule[0].icon == "flame" && byRule[0].iconWeight == TagIconWeight::Fill);
    const auto byHand = resolveManualTag(configuration.tags, "LAB");
    CHECK(byHand.icon == "flask" && byHand.iconWeight == TagIconWeight::Regular);
    CHECK(resolveManualTag(configuration.tags, "elsewhere").icon.empty());

    // A hand-set name that is a colour literal wears that colour, the way a
    // palette name does: the popover's custom colour arrives as one.
    const auto custom = resolveManualTag(configuration.tags, "#3a7bd5");
    CHECK(custom.color.set && custom.color.red == 0x3a && custom.color.green == 0x7b && custom.color.blue == 0xd5);
    CHECK(custom.colorName.empty());
    CHECK(!resolveManualTag(configuration.tags, "#3a7bd").color.set);

    CHECK(serializeTag(configuration.tags[0]).find("icon = \"flame\"\n") != std::string::npos);
    CHECK(serializeTag(configuration.tags[0]).find("icon_weight") == std::string::npos);
    const std::string regular = serializeTag(configuration.tags[1]);
    CHECK(regular.find("icon = \"flask\"\nicon_weight = \"regular\"\n") != std::string::npos);
    const auto reread = parseConfiguration(regular);
    CHECK(reread.tags.size() == 1 && reread.tags[0].icon == "flask" && reread.tags[0].iconWeight == "regular");

    // Two untitled dots of one colour are one marker; with different icons
    // they are two.
    TagEntry server = makeRule("", "blue", "srv-*", "");
    server.icon = "hard-drives";
    TagEntry laptop = makeRule("", "blue", "srv-*", "");
    laptop.icon = "laptop";
    CHECK(resolveTags({ server, laptop }, "srv-1", "").size() == 2);
    CHECK(resolveTags({ server, server }, "srv-1", "").size() == 1);
}

// The rule a reader writes to mark one machine marks it however the window
// reached it. Each kind of remote pane is described by the facts its frontend
// holds, Core places it, and the same `match_host` glob tags it — the chip and
// the tags read the one `PaneHost`, so the place the chip names is the place
// the rule matched.
TEST(oneHostRuleMarksTheMachineForEveryKindOfRemotePane)
{
    using Ubiquitty::Session::HostPlace;
    using Ubiquitty::Session::PaneHost;
    using Ubiquitty::Session::PaneHostInputs;
    using Ubiquitty::Session::resolvePaneHost;
    using Ubiquitty::Session::resolveTagsForPaneHost;
    const std::vector<TagEntry> rules { makeRule("lab", "purple", "lab-mac*", "") };
    const auto isTagged = [&](const PaneHost& host) {
        const auto tags = resolveTagsForPaneHost(rules, host, "");
        return tags.size() == 1 && tags[0].name == "lab";
    };

    // A ubiquitty-host session over this machine's own socket is on this
    // machine, under the name the machine goes by rather than the key "local".
    PaneHostInputs localSocket;
    localSocket.isHostSession = true;
    localSocket.hostKey = "local";
    localSocket.hostLabel = "Ubiquitty Host";
    localSocket.thisMachineName = "lab-mac-mini.local";
    const PaneHost onThisMachine = resolvePaneHost(localSocket);
    CHECK(onThisMachine.place == HostPlace::ThisMachine);
    CHECK(onThisMachine.machine == "lab-mac-mini.local");
    CHECK(onThisMachine.carrier.empty());
    CHECK_MSG(isTagged(onThisMachine), "the daemon on this machine is tagged by this machine's name");

    // A shell shared into this machine's daemon is there too, with no key.
    PaneHostInputs shared;
    shared.isHostSession = true;
    shared.isShared = true;
    shared.thisMachineName = "lab-mac-mini.local";
    CHECK(resolvePaneHost(shared).place == HostPlace::ThisMachine);
    CHECK_MSG(isTagged(resolvePaneHost(shared)), "a shared shell is tagged by this machine's name");

    // A host launched over ssh: the login is not the machine, and the label
    // the reader saved it under is a name it answers to as well.
    PaneHostInputs overSSH;
    overSSH.isHostSession = true;
    overSSH.hostKey = "ada@lab-mac-studio";
    overSSH.hostLabel = "Studio";
    const PaneHost studio = resolvePaneHost(overSSH);
    CHECK(studio.place == HostPlace::Remote);
    CHECK(studio.isHostSession);
    CHECK(studio.machine == "lab-mac-studio");
    CHECK(studio.label == "Studio");
    CHECK(studio.carrier == "ssh");
    CHECK_MSG(isTagged(studio), "an ssh-launched host is tagged by its machine");
    const std::vector<TagEntry> byLabel { makeRule("studio", "blue", "studio", "") };
    CHECK_MSG(resolveTagsForPaneHost(byLabel, studio, "").size() == 1, "and by the label it was saved under");

    // The same host over QUIC differs only in its carrier.
    PaneHostInputs overQUIC = overSSH;
    overQUIC.usesQUIC = true;
    const PaneHost studioOverQUIC = resolvePaneHost(overQUIC);
    CHECK(studioOverQUIC.carrier == "quic");
    CHECK(studioOverQUIC.machine == studio.machine);
    CHECK_MSG(isTagged(studioOverQUIC), "a QUIC host is tagged by its machine");

    // A plain local shell that ran ssh: no link of ours, found by the walk.
    PaneHostInputs typedSSH;
    typedSSH.location.target = { Ubiquitty::PAL::Process::RemoteKind::Host, "lab-mac-pro", "ada", "ssh" };
    const PaneHost pro = resolvePaneHost(typedSSH);
    CHECK(pro.place == HostPlace::Remote);
    CHECK(!pro.isHostSession);
    CHECK(pro.machine == "lab-mac-pro" && pro.label == "lab-mac-pro" && pro.carrier == "ssh");
    CHECK_MSG(isTagged(pro), "a plain ssh session is tagged by the machine it reached");

    // tmux control mode reached over ssh: the transport stamp names the host,
    // since the hosted panes run no process of their own to walk.
    PaneHostInputs tmuxControl;
    tmuxControl.location.origin.transport = Ubiquitty::Transport::OriginKind::SSH;
    tmuxControl.location.origin.host = "ada@lab-mac-air";
    const PaneHost air = resolvePaneHost(tmuxControl);
    CHECK(air.place == HostPlace::Remote);
    CHECK(air.machine == "lab-mac-air" && air.carrier == "ssh");
    CHECK_MSG(isTagged(air), "a tmux control-mode session is tagged by its host");

    // A plain ssh session dialled from a saved host keeps the label it was
    // saved under while it is on that machine, and drops it once the reader
    // has gone on from there.
    PaneHostInputs savedSSH;
    savedSSH.hostKey = "ada@lab-mac-pro";
    savedSSH.hostLabel = "Pro";
    savedSSH.location.origin.transport = Ubiquitty::Transport::OriginKind::SSH;
    savedSSH.location.origin.host = "ada@lab-mac-pro";
    const PaneHost saved = resolvePaneHost(savedSSH);
    CHECK(saved.place == HostPlace::Remote && !saved.isHostSession);
    CHECK(saved.machine == "lab-mac-pro" && saved.label == "Pro" && saved.carrier == "ssh");
    CHECK_MSG(isTagged(saved), "a saved ssh host is tagged by its machine");
    PaneHostInputs wentOn = savedSSH;
    wentOn.location.reportedHost = "build-01";
    const PaneHost onward = resolvePaneHost(wentOn);
    CHECK(onward.machine == "build-01" && onward.label == "build-01");
    CHECK_MSG(!isTagged(onward), "a session that left the tagged machine is not tagged by it");

    // A plain local shell is nowhere to name, and the rule leaves it alone.
    const PaneHost local = resolvePaneHost(PaneHostInputs { .thisMachineName = "lab-mac-mini.local" });
    CHECK(local.place == HostPlace::None);
    CHECK(!local.isNamed());
    CHECK_MSG(!isTagged(local), "a plain local shell wears no host tag");

    // A machine the rule does not name is not marked by it.
    PaneHostInputs elsewhere = overSSH;
    elsewhere.hostKey = "build-01";
    elsewhere.hostLabel = "";
    CHECK(!isTagged(resolvePaneHost(elsewhere)));
}

TEST(aRuleShowsItsNameAndEverySurfaceUnlessItSaysOtherwise)
{
    // Every `show_*` key defaults on, which is the look a rule had before it
    // could say otherwise; a key of the wrong type is dropped, as every other
    // mistyped `[[tags]]` key is, and leaves the default.
    const auto configuration = parseConfiguration("[[tags]]\n"
                                                  "name = \"plain\"\n"
                                                  "color = \"green\"\n"
                                                  "match_host = \"*\"\n"
                                                  "\n"
                                                  "[[tags]]\n"
                                                  "name = \"acme\"\n"
                                                  "color = \"blue\"\n"
                                                  "match_host = \"*\"\n"
                                                  "show_name = false\n"
                                                  "show_in_title_bar = false\n"
                                                  "show_in_tabs = false\n"
                                                  "show_in_pane_headers = false\n"
                                                  "\n"
                                                  "[[tags]]\n"
                                                  "name = \"typo\"\n"
                                                  "color = \"red\"\n"
                                                  "match_host = \"*\"\n"
                                                  "show_name = \"no\"\n"
                                                  "show_in_tabs = 0\n");
    REQUIRE(configuration.tags.size() == 3);
    const TagEntry& plain = configuration.tags[0];
    CHECK(plain.showsName && plain.showsInTitleBar && plain.showsInTabs && plain.showsInPaneHeaders);
    const TagEntry& acme = configuration.tags[1];
    CHECK(!acme.showsName && !acme.showsInTitleBar && !acme.showsInTabs && !acme.showsInPaneHeaders);
    const TagEntry& typo = configuration.tags[2];
    CHECK(typo.showsName && typo.showsInTabs);
    CHECK(TagEntry {}.showsName && TagEntry {}.showsInTitleBar && TagEntry {}.showsInTabs && TagEntry {}.showsInPaneHeaders);
}

TEST(aHiddenNameDrawsTheIconOrTheDotAndStaysTheTagsName)
{
    using Ubiquitty::Session::isSameTag;
    using Ubiquitty::Session::TagIconWeight;
    TagEntry withIcon = makeRule("acme", "blue", "*.acme.example", "");
    withIcon.icon = "buildings";
    withIcon.showsName = false;
    TagEntry withoutIcon = makeRule("lab", "purple", "lab-*", "");
    withoutIcon.showsName = false;

    // With an icon, the marker is the icon alone; without one it is the
    // colour's dot, the mark an untitled rule draws.
    const auto byIcon = resolveTags({ withIcon }, "db.acme.example", "");
    REQUIRE(byIcon.size() == 1);
    CHECK(!byIcon[0].drawsName() && !byIcon[0].isUntitled());
    CHECK(byIcon[0].icon == "buildings" && byIcon[0].iconWeight == TagIconWeight::Fill && byIcon[0].color.set);
    const auto byDot = resolveTags({ withoutIcon }, "lab-1", "");
    REQUIRE(byDot.size() == 1);
    CHECK(!byDot[0].drawsName() && byDot[0].icon.empty() && byDot[0].color.set);

    // The name is still the tag's: what a screen reader hears, what a
    // hand-set name of it matches, and what the window's list holds.
    CHECK(byIcon[0].name == "acme");
    CHECK(tagSpokenName(byIcon[0]) == "acme");
    const auto byHand = resolveManualTag({ withIcon }, "ACME");
    CHECK(byHand.name == "ACME" && !byHand.drawsName() && byHand.icon == "buildings");
    CHECK(isSameTag(byIcon[0], byHand));
    const auto window = resolveWindowTags({ withIcon }, PaneHost { .place = Ubiquitty::Session::HostPlace::Remote, .machine = "db.acme.example" }, "", { "acme" });
    CHECK(window.size() == 1 && window[0].name == "acme" && window[0].source == TagSource::Rule);

    // Shown, the same rule draws its name; an untitled rule never does,
    // whatever it says.
    withIcon.showsName = true;
    CHECK(resolveTags({ withIcon }, "db.acme.example", "")[0].drawsName());
    TagEntry untitled = makeRule("", "blue", "home-*", "");
    CHECK(!resolveTags({ untitled }, "home-1", "")[0].drawsName());
}

TEST(showKeysWriteBackOnlyWhenTheyTurnTheirDefaultOff)
{
    TagEntry shown = makeRule("acme", "blue", "*.acme.example", "");
    CHECK(serializeTag(shown).find("show_") == std::string::npos);

    TagEntry hidden = shown;
    hidden.showsName = false;
    hidden.showsInTabs = false;
    const std::string block = serializeTag(hidden);
    CHECK_MSG(block == "[[tags]]\nname = \"acme\"\ncolor = \"blue\"\nmatch_host = \"*.acme.example\"\nshow_name = false\nshow_in_tabs = false\n",
        block.c_str());
    const auto reread = parseConfiguration(block);
    REQUIRE(reread.tags.size() == 1);
    CHECK(!reread.tags[0].showsName && reread.tags[0].showsInTitleBar && !reread.tags[0].showsInTabs && reread.tags[0].showsInPaneHeaders);

    hidden.showsInTitleBar = false;
    hidden.showsInPaneHeaders = false;
    const auto everywhereOff = parseConfiguration(serializeTag(hidden));
    REQUIRE(everywhereOff.tags.size() == 1);
    CHECK(!everywhereOff.tags[0].showsInTitleBar && !everywhereOff.tags[0].showsInPaneHeaders);

    // The editor's write-back replaces the block in place and keeps the rest.
    const std::string document = "[[tags]]\nname = \"acme\"\ncolor = \"blue\"\nmatch_host = \"*\"\nshow_name = false\n\n[appearance]\nfolder_icon = false\n";
    TagEntry edited = parseConfiguration(document).tags[0];
    edited.showsName = true;
    const auto written = parseConfiguration(writeTagRule(document, 0, edited));
    REQUIRE(written.tags.size() == 1);
    CHECK(written.tags[0].showsName);
    CHECK(written.folderIcon.has_value() && !written.folderIcon.value());
}

TEST(eachTitleSurfaceDrawsTheTagsItsRulesPlaceThereInOrder)
{
    using Ubiquitty::Session::tagsDrawnOn;
    using Ubiquitty::Session::TitleSurface;
    TagEntry everywhere = makeRule("company", "blue", "*", "");
    TagEntry titleOnly = makeRule("prod", "red", "*", "");
    titleOnly.showsInTabs = false;
    titleOnly.showsInPaneHeaders = false;
    TagEntry tabOnly = makeRule("db", "green", "*", "");
    tabOnly.showsInTitleBar = false;
    tabOnly.showsInPaneHeaders = false;
    const std::vector<TagEntry> rules { everywhere, titleOnly, tabOnly };
    const auto tags = resolveWindowTags(rules, PaneHost { .place = Ubiquitty::Session::HostPlace::Remote, .machine = "web-1" }, "", { "loose" });
    REQUIRE(tags.size() == 4);

    const auto names = [](const Ubiquitty::Session::ResolvedTags& drawn) {
        std::string joined;
        for (const auto& tag : drawn)
            joined += (joined.empty() ? "" : ",") + tag.name;
        return joined;
    };
    // A hand-set name no rule declares is drawn everywhere, after the rules.
    CHECK_MSG(names(tagsDrawnOn(tags, TitleSurface::Window)) == "company,prod,loose", names(tagsDrawnOn(tags, TitleSurface::Window)).c_str());
    CHECK_MSG(names(tagsDrawnOn(tags, TitleSurface::Tab)) == "company,db,loose", names(tagsDrawnOn(tags, TitleSurface::Tab)).c_str());
    CHECK_MSG(names(tagsDrawnOn(tags, TitleSurface::Pane)) == "company,loose", names(tagsDrawnOn(tags, TitleSurface::Pane)).c_str());

    // A hand-set tag of a rule's name is placed as that rule places it.
    const auto declared = resolveManualTag(rules, "PROD");
    CHECK(declared.isDrawnOn(TitleSurface::Window) && !declared.isDrawnOn(TitleSurface::Tab) && !declared.isDrawnOn(TitleSurface::Pane));
}

TEST(windowTagsDrawRulesInFileOrderAheadOfHandSetOnes)
{
    const std::string document = "# The company comes first.\n"
                                 "[[tags]]\n"
                                 "name = \"company\"\n"
                                 "color = \"blue\"\n"
                                 "match_host = \"*\"\n"
                                 "\n"
                                 "[[tags]]\n"
                                 "name = \"prod\"\n"
                                 "color = \"red\"\n"
                                 "match_host = \"prod-*\"\n"
                                 "\n"
                                 "[[tags]]\n"
                                 "color = \"green\"\n"
                                 "match_host = \"prod-web-*\"\n";
    const auto namesOf = [](const Ubiquitty::Session::ResolvedTags& tags) {
        std::string joined;
        for (const auto& tag : tags)
            joined += (joined.empty() ? "" : ",") + tagSpokenName(tag);
        return joined;
    };
    const PaneHost host { .place = Ubiquitty::Session::HostPlace::Remote, .machine = "prod-web-01" };
    const auto inOrder = resolveWindowTags(parseConfiguration(document).tags, host, "", { "billing", "prod" });
    // The rules in the file's order, then the hand-set names in the order set,
    // a name a rule already drew not drawn twice and not moved.
    CHECK_MSG(namesOf(inOrder) == "company,prod,Green,billing", namesOf(inOrder).c_str());
    CHECK(inOrder[0].ruleIndex == 0 && inOrder[1].ruleIndex == 1 && inOrder[2].ruleIndex == 2);
    CHECK(inOrder[3].source == TagSource::Manual);

    // Moving the last rule to the front moves its tag to the front, still
    // ahead of every hand-set one.
    const auto moved = resolveWindowTags(parseConfiguration(Ubiquitty::Session::moveTagRule(document, 2, 0)).tags, host, "", { "billing", "prod" });
    CHECK_MSG(namesOf(moved) == "Green,company,prod,billing", namesOf(moved).c_str());

    // The union keeps the order of both lists: a hand-set duplicate of a rule
    // does not move the rule's tag.
    auto tags = resolveTags(parseConfiguration(document).tags, "prod-web-01", "");
    appendTags(tags, { resolveManualTag({}, "zeta"), resolveManualTag({}, "PROD"), resolveManualTag({}, "alpha") });
    CHECK_MSG(namesOf(tags) == "company,prod,Green,zeta,alpha", namesOf(tags).c_str());
}

TEST(movingATagRuleCarriesItsBlockAndLeavesTheRestOfTheFile)
{
    using Ubiquitty::Session::moveTagRule;
    const std::string document = "[appearance]\n"
                                 "folder_icon = false\n"
                                 "\n"
                                 "# Production first.\n"
                                 "[[tags]]\n"
                                 "name = \"prod\"\n"
                                 "color = \"red\"\n"
                                 "match_host = \"prod-*\"\n"
                                 "# icon = \"flame\"\n"
                                 "\n"
                                 "[[tags]]\n"
                                 "name = \"stage\"\n"
                                 "color = \"yellow\"\n"
                                 "future_key = \"kept\"\n"
                                 "\n"
                                 "[cursor]\n"
                                 "blink = false\n"
                                 "\n"
                                 "# The company's own.\n"
                                 "[[tags]]\n"
                                 "name = \"company\"\n"
                                 "color = \"blue\"\n"
                                 "match_host = \"*\"";
    const std::string moved = moveTagRule(document, 2, 0);
    // The blocks trade slots: each slot keeps the file around it, and each
    // block keeps its comments above and inside, and a key this build does
    // not read. The last block gains the newline it ended the file without.
    const std::string expected = "[appearance]\n"
                                 "folder_icon = false\n"
                                 "\n"
                                 "# The company's own.\n"
                                 "[[tags]]\n"
                                 "name = \"company\"\n"
                                 "color = \"blue\"\n"
                                 "match_host = \"*\"\n"
                                 "\n"
                                 "# Production first.\n"
                                 "[[tags]]\n"
                                 "name = \"prod\"\n"
                                 "color = \"red\"\n"
                                 "match_host = \"prod-*\"\n"
                                 "# icon = \"flame\"\n"
                                 "\n"
                                 "[cursor]\n"
                                 "blink = false\n"
                                 "\n"
                                 "[[tags]]\n"
                                 "name = \"stage\"\n"
                                 "color = \"yellow\"\n"
                                 "future_key = \"kept\"\n";
    CHECK_MSG(moved == expected, moved.c_str());
    const auto parsed = parseConfiguration(moved);
    REQUIRE(parsed.tags.size() == 3);
    CHECK(parsed.tags[0].name == "company" && parsed.tags[1].name == "prod" && parsed.tags[2].name == "stage");
    CHECK(parsed.folderIcon.has_value() && !parsed.folderIcon.value());

    // Down one place and back again is the file it was, short of the newline.
    CHECK(moveTagRule(moveTagRule(document, 0, 1), 1, 0) == document + "\n");
    const auto down = parseConfiguration(moveTagRule(document, 0, 1)).tags;
    REQUIRE(down.size() == 3);
    CHECK(down[0].name == "stage" && down[1].name == "prod" && down[2].name == "company");

    // A position that names no rule, or no move at all, leaves the text alone.
    CHECK(moveTagRule(document, 1, 1) == document);
    CHECK(moveTagRule(document, 3, 0) == document);
    CHECK(moveTagRule(document, 0, 3) == document);
    CHECK(moveTagRule(document, -1, 0) == document);
    CHECK(moveTagRule("", 0, 1).empty());
}

TEST(aSyncWindowsTabsLeaveItsConnectionsTagsToTheTitleBar)
{
    using Ubiquitty::Session::LocationInputs;
    using Ubiquitty::Session::PaneHostInputs;
    using Ubiquitty::Session::resolvePaneHost;
    using Ubiquitty::Session::resolveTabTags;
    using Ubiquitty::Session::TabGroupHosts;
    std::vector<TagEntry> rules {
        makeRule("lab", "purple", "lab-host", ""),
        makeRule("build", "green", "build-*", ""),
        makeRule("ops", "blue", "", "Ops"),
    };
    rules[0].icon = "flask";
    const auto names = [](const Ubiquitty::Session::ResolvedTags& tags) {
        std::string joined;
        for (const auto& tag : tags)
            joined += (joined.empty() ? "" : ",") + tag.name;
        return joined;
    };

    // A window attached to lab-host over Sync: the chip says lab-host and lab, so
    // none of its three tabs on lab-host draws the host's tag or the profile's,
    // and each still names both for its tooltip and a screen reader.
    PaneHostInputs sync;
    sync.isHostSession = true;
    sync.usesQUIC = true;
    sync.hostKey = "user@lab-host";
    const auto connection = resolvePaneHost(sync);
    REQUIRE(connection.isHostSession && connection.machine == "lab-host");
    for (int tab = 0; tab < 3; ++tab) {
        const auto onHost = resolveTabTags(rules, connection, LocationInputs {}, "Ops", {}, TabGroupHosts::Shared);
        CHECK_MSG(onHost.drawn.empty(), names(onHost.drawn).c_str());
        CHECK_MSG(names(onHost.named) == "lab,ops", names(onHost.named).c_str());
    }
    // A shell on lab-host that reports lab-host is still on the connection's host.
    LocationInputs stillHost;
    stillHost.reportedHost = "user@lab-host";
    CHECK(resolveTabTags(rules, connection, stillHost, "Ops", {}, TabGroupHosts::Shared).drawn.empty());

    // One tab ssh'd on to build-01 draws that host's tag; the profile's is
    // still the window's.
    LocationInputs onward;
    onward.reportedHost = "build-01";
    const auto onBuild = resolveTabTags(rules, connection, onward, "Ops", {}, TabGroupHosts::Shared);
    CHECK_MSG(names(onBuild.drawn) == "build", names(onBuild.drawn).c_str());
    CHECK_MSG(names(onBuild.named) == "build,ops", names(onBuild.named).c_str());

    // A name set on the window by hand is the tab's to draw; one that is the
    // connection's own tag is the connection's.
    const auto handSet = resolveTabTags(rules, connection, LocationInputs {}, "Ops", { "review", "LAB" }, TabGroupHosts::Shared);
    CHECK_MSG(names(handSet.drawn) == "review", names(handSet.drawn).c_str());
    CHECK_MSG(names(handSet.named) == "lab,ops,review", names(handSet.named).c_str());

    // A tab grouped with tabs on other hosts draws its host's tag: its marks
    // are how it is told from them.
    const auto amongOthers = resolveTabTags(rules, connection, LocationInputs {}, "Ops", {}, TabGroupHosts::Mixed);
    CHECK_MSG(names(amongOthers.drawn) == "lab,ops", names(amongOthers.drawn).c_str());

    // A window on lab-host by a plain ssh, no Sync connection, keeps drawing its
    // tags on every tab, which is how tabs on several hosts are told apart.
    PaneHostInputs plainSSH;
    plainSSH.location.origin.transport = Ubiquitty::Transport::OriginKind::SSH;
    plainSSH.location.origin.host = "user@lab-host";
    const auto overSSH = resolvePaneHost(plainSSH);
    REQUIRE(!overSSH.isHostSession && overSSH.machine == "lab-host");
    const auto plain = resolveTabTags(rules, overSSH, plainSSH.location, "", {}, TabGroupHosts::Shared);
    CHECK_MSG(names(plain.drawn) == "lab", names(plain.drawn).c_str());
    CHECK(names(plain.named) == "lab");

    // A rule kept off tabs is off both lists.
    rules[1].showsInTabs = false;
    const auto offTabs = resolveTabTags(rules, connection, onward, "Ops", {}, TabGroupHosts::Shared);
    CHECK_MSG(offTabs.drawn.empty(), names(offTabs.drawn).c_str());
    CHECK_MSG(names(offTabs.named) == "ops", names(offTabs.named).c_str());
}

TEST(aSyncWindowsPaneHeadersLeaveItsConnectionsTagsToTheTitleBar)
{
    using Ubiquitty::Session::LocationInputs;
    using Ubiquitty::Session::PaneHostInputs;
    using Ubiquitty::Session::resolvePaneHeaderTags;
    using Ubiquitty::Session::resolvePaneHost;
    std::vector<TagEntry> rules {
        makeRule("lab", "purple", "lab-host", ""),
        makeRule("build", "green", "build-*", ""),
        makeRule("ops", "blue", "", "Ops"),
    };
    const auto names = [](const Ubiquitty::Session::ResolvedTags& tags) {
        std::string joined;
        for (const auto& tag : tags)
            joined += (joined.empty() ? "" : ",") + tag.name;
        return joined;
    };
    PaneHostInputs sync;
    sync.isHostSession = true;
    sync.hostKey = "user@lab-host";
    const auto paneHost = resolvePaneHost(sync);

    // A split window attached over Sync: neither pane's header draws what
    // the chip already says, and each names it for a screen reader.
    for (int pane = 0; pane < 2; ++pane) {
        const auto header = resolvePaneHeaderTags(rules, paneHost, LocationInputs {}, "Ops", {});
        CHECK_MSG(header.drawn.empty(), names(header.drawn).c_str());
        CHECK_MSG(names(header.named) == "lab,ops", names(header.named).c_str());
    }

    // The pane ssh'd on to build-01 draws that host's tag.
    LocationInputs onward;
    onward.reportedHost = "build-01";
    const auto onBuild = resolvePaneHeaderTags(rules, paneHost, onward, "Ops", {});
    CHECK_MSG(names(onBuild.drawn) == "build", names(onBuild.drawn).c_str());
    CHECK_MSG(names(onBuild.named) == "build,ops", names(onBuild.named).c_str());

    // A hand-set name is the header's to draw.
    CHECK(names(resolvePaneHeaderTags(rules, paneHost, LocationInputs {}, "Ops", { "review" }).drawn) == "review");

    // A local window's pane on lab-host by a plain ssh draws its tags.
    PaneHostInputs plainSSH;
    plainSSH.location.origin.transport = Ubiquitty::Transport::OriginKind::SSH;
    plainSSH.location.origin.host = "user@lab-host";
    const auto plain = resolvePaneHeaderTags(rules, resolvePaneHost(plainSSH), plainSSH.location, "", {});
    CHECK_MSG(names(plain.drawn) == "lab" && names(plain.named) == "lab", names(plain.drawn).c_str());

    // A rule kept off pane headers is off both lists.
    rules[1].showsInPaneHeaders = false;
    const auto offHeaders = resolvePaneHeaderTags(rules, paneHost, onward, "Ops", {});
    CHECK_MSG(offHeaders.drawn.empty() && names(offHeaders.named) == "ops", names(offHeaders.named).c_str());
}

UBIQUITTY_TEST_MAIN()
