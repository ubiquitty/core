// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Core/test_notification.cpp — the desktop-notification policy every
// frontend's backend shares (Core/OSC/Notification.h).
//
// `resolveNotificationDisplay` turns the pair a program sent into text fit to
// show: a body-only message promotes its body into the title, a body that only
// repeats the title is dropped, both are sanitized and bounded, and a wholly
// empty pair is not posted.
//
// `composeBanner` lays the resolved pair and the session's place out in a
// banner's three slots, `isBannerWanted` says whether the person is already
// looking at the pane that asked, and `bannerRequestIdentifier` names the
// request so a pane's next banner replaces its last.

#include "Core/OSC/Notification.h"

#include <cstdio>
#include <string>

using Ubiquitty::OSC::NotificationDisplay;
using Ubiquitty::OSC::resolveNotificationDisplay;

static void expectDisplay(const std::string& rawTitle, const std::string& rawBody, bool wantPost, const std::string& wantTitle, const std::string& wantBody, const char* label)
{
    const NotificationDisplay got = resolveNotificationDisplay(rawTitle, rawBody);
    if (got.shouldPost != wantPost || got.title != wantTitle || got.body != wantBody) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage),
            "%s: resolveNotificationDisplay(\"%s\", \"%s\") = {post=%d, title=\"%s\", body=\"%s\"}, "
            "want {post=%d, title=\"%s\", body=\"%s\"}",
            label, rawTitle.c_str(), rawBody.c_str(), got.shouldPost ? 1 : 0, got.title.c_str(), got.body.c_str(), wantPost ? 1 : 0, wantTitle.c_str(), wantBody.c_str());
        CHECK_MSG(false, ubqMessage);
    }
}

TEST(notificationCases)
{

    // Body-only (OSC 9 / kitty OSC 99): body promotes into the heading.
    expectDisplay("", "Build finished", true, "Build finished", "", "body-only promotes to title");
    expectDisplay("", "x", true, "x", "", "single-char body promotes");

    // Titled message (urxvt OSC 777, or any title+body) passes through unchanged.
    expectDisplay("Deploy", "succeeded", true, "Deploy", "succeeded", "title+body unchanged");
    expectDisplay("Just a title", "", true, "Just a title", "", "title-only unchanged (empty body stays empty)");

    // Wholly empty pair is dropped.
    expectDisplay("", "", false, "", "", "empty pair dropped");

    // A title of blanks says nothing, so the body is the heading.
    expectDisplay(" ", "body", true, "body", "", "a blank title is no title");
    expectDisplay("\t\n", " ", false, "", "", "a pair of blanks is an empty pair");
}

TEST(aBodyThatRepeatsTheTitleIsDropped)
{
    expectDisplay("Agent", "Agent", true, "Agent", "", "an exact repeat");
    expectDisplay("Agent", " agent\n", true, "Agent", "", "a repeat differing in case and blanks");
    expectDisplay("Agent:", "Agent", true, "Agent", "", "a repeat differing in trailing noise");
    expectDisplay("Agent", "Agent is waiting", true, "Agent", "Agent is waiting", "a body that only starts alike is its own text");
}

TEST(trailingNoiseIsTrimmedAndTheWordsAreLeftAlone)
{
    expectDisplay("Build: ", "done.\n", true, "Build", "done.", "a dangling colon and a newline");
    expectDisplay("Deploy —", "finished - ", true, "Deploy", "finished", "dangling dashes");
    expectDisplay("Tests |", "3 failed;,", true, "Tests", "3 failed", "dangling bar, semicolon and comma");
    // Punctuation that ends a sentence is the sentence's.
    expectDisplay("", "Running...", true, "Running...", "", "an ellipsis of full stops stays");
    expectDisplay("", "Done!", true, "Done!", "", "an exclamation mark stays");
    expectDisplay("", "Continue?", true, "Continue?", "", "a question mark stays");
    expectDisplay("", "Loading…", true, "Loading…", "", "an ellipsis stays");
    // Noise inside the text is not trailing.
    expectDisplay("a - b: c", "", true, "a - b: c", "", "separators between words stay");
    // Text that is nothing but separators is still what the program said.
    expectDisplay("", "---", true, "---", "", "all-separator text is kept");
}

TEST(bothFieldsAreSanitized)
{
    expectDisplay("a\x1b[1mb\x07", "line one\r\nline  two\x7f", true, "a [1mb", "line one line two", "controls become blanks and blanks fold");
    expectDisplay("safe\xe2\x80\xae"
                  "gpj.exe",
        "x\xe2\x81\xa7y\xe2\x80\x8f", true, "safegpj.exe", "xy", "bidirectional controls are dropped");
    expectDisplay("ok\xff", "b\xe6\xbc", true, "ok", "b", "bytes that are not UTF-8 are dropped");
    // The level mark is only a character outside a place line.
    expectDisplay("a › b", "", true, "a › b", "", "the level mark stays in a message");
}

TEST(bothFieldsAreBounded)
{
    using Ubiquitty::OSC::maxNotificationTextColumns;
    const std::string longText(400, 'x');
    const std::string cut = std::string(static_cast<std::size_t>(maxNotificationTextColumns - 1), 'x') + "…";
    expectDisplay("", longText, true, cut, "", "a long message is cut with an ellipsis");
    expectDisplay("T", longText, true, "T", cut, "a long body is cut with an ellipsis");
    // Resolving what was already resolved changes nothing, so a frontend
    // handed a resolved pair may pass it through the policy again.
    expectDisplay(cut, "", true, cut, "", "a resolved heading resolves to itself");
    expectDisplay("T", cut, true, "T", cut, "a resolved pair resolves to itself");
}

namespace {

using Ubiquitty::OSC::Banner;
using Ubiquitty::OSC::composeBanner;

void expectBanner(const std::string& title, const std::string& body, const std::string& place, const Banner& want, const char* label)
{
    const Banner got = composeBanner(title, body, place);
    if (got.shouldPost != want.shouldPost || got.title != want.title || got.subtitle != want.subtitle || got.body != want.body) {
        char ubqMessage[1024];
        std::snprintf(ubqMessage, sizeof(ubqMessage),
            "%s: composeBanner(\"%s\", \"%s\", \"%s\") = {post=%d, title=\"%s\", subtitle=\"%s\", body=\"%s\"}, "
            "want {post=%d, title=\"%s\", subtitle=\"%s\", body=\"%s\"}",
            label, title.c_str(), body.c_str(), place.c_str(), got.shouldPost ? 1 : 0, got.title.c_str(), got.subtitle.c_str(), got.body.c_str(),
            want.shouldPost ? 1 : 0, want.title.c_str(), want.subtitle.c_str(), want.body.c_str());
        CHECK_MSG(false, ubqMessage);
    }
}

const std::string place = "Studio › Main › ~ — ssh build-box › 4";

} // namespace

TEST(aBannerHeadsWithTheActionAndNamesTheSenderBeneathIt)
{
    // A program that sent both: its title is who is speaking, its body is what
    // it wants, and what it wants is the heading.
    expectBanner("Agent", "Agent is waiting for your input", place,
        Banner { .title = "Agent is waiting for your input", .subtitle = "Agent", .body = place, .shouldPost = true }, "title and body, with a place");
    expectBanner("Agent", "Agent is waiting for your input", "",
        Banner { .title = "Agent is waiting for your input", .subtitle = "Agent", .body = "", .shouldPost = true }, "title and body, no place");
}

TEST(aBannerWithOneTextHasNoSender)
{
    expectBanner("", "Agent needs your permission", place,
        Banner { .title = "Agent needs your permission", .subtitle = "", .body = place, .shouldPost = true }, "body only, with a place");
    expectBanner("", "Agent needs your permission", "",
        Banner { .title = "Agent needs your permission", .subtitle = "", .body = "", .shouldPost = true }, "body only, no place");

    // A title alone is the only thing said, so it is the action; no sender is
    // made up to stand beneath it.
    expectBanner("Build finished", "", place,
        Banner { .title = "Build finished", .subtitle = "", .body = place, .shouldPost = true }, "title only, with a place");
    expectBanner("Build finished", "", "",
        Banner { .title = "Build finished", .subtitle = "", .body = "", .shouldPost = true }, "title only, no place");
}

TEST(aSenderThatRepeatsTheActionIsNotShown)
{
    expectBanner("Build finished", "Build finished", place,
        Banner { .title = "Build finished", .subtitle = "", .body = place, .shouldPost = true }, "title equal to body, with a place");
    expectBanner("Build finished", "build finished.", "",
        Banner { .title = "Build finished", .subtitle = "", .body = "", .shouldPost = true }, "title equal to body but for case, no place");
}

TEST(anEmptyPairIsNoBannerWhateverThePlace)
{
    expectBanner("", "", place, Banner {}, "empty pair, with a place");
    expectBanner(" ", "\n", "", Banner {}, "blank pair, no place");
}

TEST(anActionTooLongToHeadABannerBecomesItsRunningText)
{
    using Ubiquitty::OSC::maxBannerHeadingColumns;
    const std::string longAction = "The deployment to the staging cluster finished with three warnings";
    CHECK(static_cast<int>(longAction.size()) > maxBannerHeadingColumns);

    // With a sender and a place, the sender heads it and the place stands
    // beneath; the action wraps below, whole.
    expectBanner("Agent", longAction, place,
        Banner { .title = "Agent", .subtitle = place, .body = longAction, .shouldPost = true }, "long action, sender, place");
    expectBanner("Agent", longAction, "",
        Banner { .title = "Agent", .subtitle = "", .body = longAction, .shouldPost = true }, "long action, sender, no place");

    // With no sender the place is what identifies it, so the place heads it.
    expectBanner("", longAction, place,
        Banner { .title = place, .subtitle = "", .body = longAction, .shouldPost = true }, "long action, no sender, place");
    expectBanner(longAction, "", place,
        Banner { .title = place, .subtitle = "", .body = longAction, .shouldPost = true }, "long title-only action, place");

    // With neither there is nothing else to head it with.
    expectBanner("", longAction, "",
        Banner { .title = longAction, .subtitle = "", .body = "", .shouldPost = true }, "long action, no sender, no place");
}

TEST(theHeadingBoundIsCountedInColumns)
{
    using Ubiquitty::OSC::maxBannerHeadingColumns;
    const std::string atBound(static_cast<std::size_t>(maxBannerHeadingColumns), 'a');
    expectBanner("Agent", atBound, place,
        Banner { .title = atBound, .subtitle = "Agent", .body = place, .shouldPost = true }, "an action exactly at the bound is a heading");
    expectBanner("Agent", atBound + "b", place,
        Banner { .title = "Agent", .subtitle = place, .body = atBound + "b", .shouldPost = true }, "one column more is not");

    // Wide characters take two columns each: twenty fit, twenty-one do not.
    std::string wide;
    for (int index = 0; index < maxBannerHeadingColumns / 2; ++index)
        wide += "漢";
    expectBanner("", wide, place, Banner { .title = wide, .subtitle = "", .body = place, .shouldPost = true }, "twenty wide characters head a banner");
    expectBanner("", wide + "漢", place,
        Banner { .title = place, .subtitle = "", .body = wide + "漢", .shouldPost = true }, "twenty-one do not");
}

TEST(noLayoutSaysOneTextTwice)
{
    const std::string actions[] = { "short", "The deployment to the staging cluster finished with three warnings" };
    const std::string senders[] = { "", "Agent" };
    const std::string places[] = { "", place };
    for (const std::string& action : actions) {
        for (const std::string& sender : senders) {
            for (const std::string& where : places) {
                const Banner banner = sender.empty() ? composeBanner("", action, where) : composeBanner(sender, action, where);
                CHECK(banner.shouldPost);
                CHECK(!banner.title.empty());
                CHECK(banner.subtitle.empty() || banner.subtitle != banner.title);
                CHECK(banner.body.empty() || (banner.body != banner.title && banner.body != banner.subtitle));
                // Each thing said is said once, somewhere.
                const std::string all = banner.title + "\n" + banner.subtitle + "\n" + banner.body;
                CHECK_CONTAINS(all, action);
                if (!sender.empty())
                    CHECK_CONTAINS(all, sender);
                if (!where.empty())
                    CHECK_CONTAINS(all, where);
            }
        }
    }
}

TEST(aTwoSlotSystemReadsTheSubtitleAsTheFirstLineOfItsText)
{
    using Ubiquitty::OSC::bannerRunningText;
    CHECK_STR_EQ(bannerRunningText(composeBanner("Agent", "is waiting", place)), "Agent\n" + place);
    CHECK_STR_EQ(bannerRunningText(composeBanner("Agent", "is waiting", "")), std::string("Agent"));
    CHECK_STR_EQ(bannerRunningText(composeBanner("", "is waiting", place)), place);
    CHECK_STR_EQ(bannerRunningText(composeBanner("", "is waiting", "")), std::string());
}

TEST(aBannersSenderIsCutShorterThanItsAction)
{
    using Ubiquitty::OSC::maxBannerSenderColumns;
    const std::string longSender(200, 's');
    const Banner banner = composeBanner(longSender, "done", "");
    CHECK_STR_EQ(banner.title, std::string("done"));
    CHECK_STR_EQ(banner.subtitle, std::string(static_cast<std::size_t>(maxBannerSenderColumns - 1), 's') + "…");
}

TEST(noBannerIsWantedByAPersonLookingAtThePane)
{
    using Ubiquitty::OSC::BannerAudience;
    using Ubiquitty::OSC::isBannerWanted;
    const BannerAudience looking { .isAppActive = true, .isWindowKey = true, .isPaneFocused = true, .isPaneVisible = true };
    CHECK(!isBannerWanted(looking));

    // Any one of the four failing means the pane is not in front of them.
    BannerAudience elsewhere = looking;
    elsewhere.isAppActive = false;
    CHECK_MSG(isBannerWanted(elsewhere), "another app is in front");
    elsewhere = looking;
    elsewhere.isWindowKey = false;
    CHECK_MSG(isBannerWanted(elsewhere), "another window has the keyboard");
    elsewhere = looking;
    elsewhere.isPaneFocused = false;
    CHECK_MSG(isBannerWanted(elsewhere), "another pane of the window has the focus");
    elsewhere = looking;
    elsewhere.isPaneVisible = false;
    CHECK_MSG(isBannerWanted(elsewhere), "the pane is behind another tab or a zoomed pane");

    // A frontend that reported nothing has not said anyone is looking.
    CHECK(isBannerWanted(BannerAudience {}));
}

TEST(aProgramsOccasionNarrowsWhenItsBannerIsWanted)
{
    using Ubiquitty::NotificationOccasion;
    using Ubiquitty::OSC::BannerAudience;
    using Ubiquitty::OSC::isBannerWanted;
    const BannerAudience looking { .isAppActive = true, .isWindowKey = true, .isPaneFocused = true, .isPaneVisible = true };
    BannerAudience besideTheFocus = looking;
    besideTheFocus.isPaneFocused = false;
    BannerAudience behindAnotherApp = looking;
    behindAnotherApp.isAppActive = false;
    BannerAudience otherTab = looking;
    otherTab.isWindowKey = false;
    otherTab.isPaneVisible = false;

    // No occasion shows a banner to someone looking at the pane.
    for (const NotificationOccasion occasion : { NotificationOccasion::Always, NotificationOccasion::Unfocused, NotificationOccasion::Invisible })
        CHECK(!isBannerWanted(looking, occasion));

    // Unfocused: whenever the pane does not have the keyboard.
    CHECK(isBannerWanted(besideTheFocus, NotificationOccasion::Unfocused));
    CHECK(isBannerWanted(behindAnotherApp, NotificationOccasion::Unfocused));
    CHECK(isBannerWanted(otherTab, NotificationOccasion::Unfocused));

    // The one place it parts from Always: a pane that holds the keyboard
    // while it is not drawn, as under a window another has covered.
    BannerAudience typingUnseen = looking;
    typingUnseen.isPaneVisible = false;
    CHECK(isBannerWanted(typingUnseen, NotificationOccasion::Always));
    CHECK(!isBannerWanted(typingUnseen, NotificationOccasion::Unfocused));

    // Invisible: only while the pane is not drawn.
    CHECK(!isBannerWanted(besideTheFocus, NotificationOccasion::Invisible));
    CHECK(!isBannerWanted(behindAnotherApp, NotificationOccasion::Invisible));
    CHECK(isBannerWanted(otherTab, NotificationOccasion::Invisible));

    // Always is the rule with no occasion stated.
    CHECK(isBannerWanted(besideTheFocus, NotificationOccasion::Always) == isBannerWanted(besideTheFocus));
    CHECK(isBannerWanted(otherTab, NotificationOccasion::Always));
}

TEST(aPanesNextBannerReplacesItsLast)
{
    using Ubiquitty::OSC::bannerRequestIdentifier;
    using Ubiquitty::OSC::BannerSource;
    const std::string first = bannerRequestIdentifier(BannerSource::Program, "12", 4);
    CHECK_STR_EQ(first, bannerRequestIdentifier(BannerSource::Program, "12", 4));
    CHECK_STR_EQ(first, std::string("ubiquitty.banner.program.12.pane.4"));

    // Another pane, another window and another kind of banner each stand
    // beside it rather than in its place.
    CHECK(first != bannerRequestIdentifier(BannerSource::Program, "12", 5));
    CHECK(first != bannerRequestIdentifier(BannerSource::Program, "13", 4));
    CHECK(first != bannerRequestIdentifier(BannerSource::Bell, "12", 4));
    CHECK(first != bannerRequestIdentifier(BannerSource::Consent, "12", 4));
    CHECK(bannerRequestIdentifier(BannerSource::Bell, "12", 4) != bannerRequestIdentifier(BannerSource::Consent, "12", 4));

    // An event of the window rather than of a pane is the window's one banner
    // of its kind.
    const std::string windowLevel = bannerRequestIdentifier(BannerSource::Program, "12", 0);
    CHECK_STR_EQ(windowLevel, std::string("ubiquitty.banner.program.12"));
    CHECK(windowLevel != first);

    // A program that names its notifications chooses which replace which:
    // the same name again replaces, another name stands beside it, and
    // neither touches the pane's unnamed one.
    const std::string named = bannerRequestIdentifier(BannerSource::Program, "12", 4, "build");
    CHECK_STR_EQ(named, std::string("ubiquitty.banner.program.12.pane.4.name.build"));
    CHECK_STR_EQ(named, bannerRequestIdentifier(BannerSource::Program, "12", 4, "build"));
    CHECK(named != bannerRequestIdentifier(BannerSource::Program, "12", 4, "tests"));
    CHECK(named != first);
    CHECK(named != bannerRequestIdentifier(BannerSource::Program, "12", 5, "build"));

    // A window "1" with pane 24 and a window "12" with pane 4 are different
    // panes; the identifier does not run its numbers together.
    CHECK(bannerRequestIdentifier(BannerSource::Program, "1", 24) != bannerRequestIdentifier(BannerSource::Program, "12", 4));
}

TEST(bannersGroupByTheWindowThatPostedThem)
{
    using Ubiquitty::OSC::bannerThreadIdentifier;
    CHECK_STR_EQ(bannerThreadIdentifier("12"), std::string("ubiquitty.window.12"));
    CHECK(bannerThreadIdentifier("12") != bannerThreadIdentifier("13"));
}

TEST(aBannerHandedBackAnswersForExactlyOneProducer)
{
    using Ubiquitty::NotificationOccasion;
    using Ubiquitty::OSC::BannerOrigin;
    using Ubiquitty::OSC::BannerOriginFacts;
    using Ubiquitty::OSC::decideBannerOrigin;
    using Ubiquitty::OSC::liveBannerPrefix;
    using Ubiquitty::OSC::retainedBannerPrefix;
    const BannerOriginFacts retained {
        .identifier = retainedBannerPrefix("H", "P") + "event",
        .host = "H",
        .permission = "P",
        .incarnation = {},
        .daemon = 0,
        .session = 42,
        .occasion = 0,
    };
    CHECK(decideBannerOrigin(retained).origin == BannerOrigin::Retained);
    CHECK(decideBannerOrigin(retained).occasion == NotificationOccasion::Always);
    const BannerOriginFacts live {
        .identifier = liveBannerPrefix("H", "M") + "program",
        .host = "H",
        .permission = {},
        .incarnation = "M",
        .daemon = 7,
        .session = 42,
        .occasion = 1,
    };
    CHECK(decideBannerOrigin(live).origin == BannerOrigin::Live);
    CHECK(decideBannerOrigin(live).occasion == NotificationOccasion::Unfocused);

    // Renewing a permission or reattaching moves the namespace.
    CHECK(retainedBannerPrefix("H", "P") != retainedBannerPrefix("H", "Q"));
    CHECK(liveBannerPrefix("H", "M") != liveBannerPrefix("H", "N"));
    CHECK(retainedBannerPrefix("H", "M") != liveBannerPrefix("H", "M"));

    const auto refuses = [](BannerOriginFacts facts, const char* why) {
        CHECK_MSG(decideBannerOrigin(facts).origin == BannerOrigin::Refused, why);
    };
    BannerOriginFacts facts = retained;
    facts.identifier = "foreign";
    refuses(facts, "an identifier outside the permission's namespace");
    facts.identifier = retainedBannerPrefix("H", "P");
    refuses(facts, "a namespace with no event in it");
    facts = retained;
    facts.permission = "Q";
    refuses(facts, "a permission the identifier does not name");
    facts = retained;
    facts.session = 0;
    refuses(facts, "no session");
    facts = retained;
    facts.host.clear();
    refuses(facts, "no host");
    facts = retained;
    facts.occasion = 3;
    refuses(facts, "an occasion Core does not define is not Always");
    facts = retained;
    facts.occasion = 256;
    refuses(facts, "an occasion past a byte does not wrap to Always");
    facts = live;
    facts.permission = "P";
    refuses(facts, "two producers at once");
    facts = live;
    facts.daemon = 0;
    refuses(facts, "a live attachment without its daemon");
    facts = live;
    facts.incarnation = "N";
    refuses(facts, "another incarnation under this one's namespace");
    facts = retained;
    facts.permission.clear();
    refuses(facts, "no producer");
}

TEST(aBannerHandedBackShowsOnlyForItsCurrentProducer)
{
    using Ubiquitty::NotificationOccasion;
    using Ubiquitty::OSC::BannerAudience;
    using Ubiquitty::OSC::BannerOrigin;
    using Ubiquitty::OSC::BannerOriginDecision;
    using Ubiquitty::OSC::isBannerPresented;
    const BannerOriginDecision live { .origin = BannerOrigin::Live, .occasion = NotificationOccasion::Unfocused };
    const BannerAudience looking { .isAppActive = true, .isWindowKey = true, .isPaneFocused = true, .isPaneVisible = true };
    BannerAudience besideTheFocus = looking;
    besideTheFocus.isPaneFocused = false;
    CHECK(isBannerPresented(live, true, besideTheFocus));
    CHECK_MSG(!isBannerPresented(live, true, looking), "the person moved to the pane before it was shown");
    CHECK_MSG(!isBannerPresented(live, false, besideTheFocus), "a retired attachment");
    CHECK_MSG(!isBannerPresented(BannerOriginDecision {}, true, BannerAudience {}), "a refused request");
    const BannerOriginDecision retained { .origin = BannerOrigin::Retained, .occasion = NotificationOccasion::Always };
    CHECK(isBannerPresented(retained, true, BannerAudience {}));
    CHECK_MSG(!isBannerPresented(retained, false, BannerAudience {}), "a renewed or withdrawn permission");
}

UBIQUITTY_TEST_MAIN()
