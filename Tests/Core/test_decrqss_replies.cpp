// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// DECRQSS reports a setting back to the application, and the bytes it reports
// are the contract. This pins them across the state space the SGR report ranges
// over — every attribute, every colour form on both halves — plus the other
// selectors, so a change to how the reply is assembled has to produce the same
// bytes or fail here.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <string>

using UbiquittyTest::feed;

namespace {

// The reply to one DECRQSS request, with the DCS framing left in: the framing
// is part of what the assembly produces.
std::string requestSetting(Ubiquitty::Terminal& t, const std::string& selector)
{
    t.outputLength = 0;
    feed(t, "\x1bP$q" + selector + "\x1b\\");
    return std::string(t.outputBuffer.data(), static_cast<std::size_t>(t.outputLength));
}

// A reply with the DCS frame stripped, which is what the cases below read: the
// frame is asserted once, on its own.
std::string settingBody(Ubiquitty::Terminal& t, const std::string& selector)
{
    const std::string whole = requestSetting(t, selector);
    const std::string opening = "\x1bP1$r";
    const std::string closing = "\x1b\\";
    if (whole.size() < opening.size() + closing.size())
        return whole;
    if (!whole.starts_with(opening))
        return whole;
    return whole.substr(opening.size(), whole.size() - opening.size() - closing.size());
}

Ubiquitty::Terminal& freshTerminal(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.outputLength = 0;
    return t;
}

} // namespace

TEST(theSGRReportCarriesEveryAttribute)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    CHECK(settingBody(t, "m") == "0m");

    feed(t, "\x1b[1m");
    CHECK(settingBody(t, "m") == "0;1m");
    feed(t, "\x1b[0;2m");
    CHECK(settingBody(t, "m") == "0;2m");
    feed(t, "\x1b[0;3m");
    CHECK(settingBody(t, "m") == "0;3m");
    feed(t, "\x1b[0;4m");
    CHECK(settingBody(t, "m") == "0;4m");
    feed(t, "\x1b[0;5m");
    CHECK(settingBody(t, "m") == "0;5m");
    feed(t, "\x1b[0;6m");
    CHECK(settingBody(t, "m") == "0;6m");
    feed(t, "\x1b[0;7m");
    CHECK(settingBody(t, "m") == "0;7m");
    feed(t, "\x1b[0;8m");
    CHECK(settingBody(t, "m") == "0;8m");
    feed(t, "\x1b[0;9m");
    CHECK(settingBody(t, "m") == "0;9m");
}

TEST(theSGRReportCarriesAttributesTogetherInOrder)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    feed(t, "\x1b[1;3;4;7m");
    CHECK(settingBody(t, "m") == "0;1;4;7;3m");

    feed(t, "\x1b[0;1;2;3;4;5;7;8;9m");
    CHECK(settingBody(t, "m") == "0;1;4;5;7;8;2;3;9m");
}

TEST(theSGRReportCarriesEveryForegroundForm)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    feed(t, "\x1b[0;31m");
    CHECK(settingBody(t, "m") == "0;31m");
    feed(t, "\x1b[0;37m");
    CHECK(settingBody(t, "m") == "0;37m");
    feed(t, "\x1b[0;90m");
    CHECK(settingBody(t, "m") == "0;90m");
    feed(t, "\x1b[0;97m");
    CHECK(settingBody(t, "m") == "0;97m");
    feed(t, "\x1b[0;38;5;200m");
    CHECK(settingBody(t, "m") == "0;38:5:200m");
    feed(t, "\x1b[0;38;2;12;34;56m");
    CHECK(settingBody(t, "m") == "0;38:2::12:34:56m");
}

TEST(theSGRReportCarriesEveryBackgroundForm)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    feed(t, "\x1b[0;41m");
    CHECK(settingBody(t, "m") == "0;41m");
    feed(t, "\x1b[0;47m");
    CHECK(settingBody(t, "m") == "0;47m");
    feed(t, "\x1b[0;100m");
    CHECK(settingBody(t, "m") == "0;100m");
    feed(t, "\x1b[0;107m");
    CHECK(settingBody(t, "m") == "0;107m");
    feed(t, "\x1b[0;48;5;200m");
    CHECK(settingBody(t, "m") == "0;48:5:200m");
    feed(t, "\x1b[0;48;2;12;34;56m");
    CHECK(settingBody(t, "m") == "0;48:2::12:34:56m");
}

TEST(theSGRReportCarriesBothHalvesAtOnce)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    feed(t, "\x1b[0;1;4;38;2;255;0;127;48;5;17m");
    CHECK(settingBody(t, "m") == "0;1;4;38:2::255:0:127;48:5:17m");

    // An indexed colour inside 0..7 reports in the 30-37 form, not the form it
    // was set with: the report states the setting, not the request.
    feed(t, "\x1b[0;38;5;0;48;2;0;0;0m");
    CHECK(settingBody(t, "m") == "0;30;48:2::0:0:0m");
}

TEST(theOtherSelectorsReportTheirSettings)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    CHECK(settingBody(t, "r") == "1;24r");
    feed(t, "\x1b[3;20r");
    CHECK(settingBody(t, "r") == "3;20r");

    feed(t, "\x1b[4 q");
    CHECK(settingBody(t, " q") == "4 q");

    CHECK(settingBody(t, "\"q") == "0\"q");
    feed(t, "\x1b[1\"q");
    CHECK(settingBody(t, "\"q") == "1\"q");

    CHECK(settingBody(t, "s") == "1;80s");

    // DECSCL — conformance level and the C1 transmission select.
    CHECK(settingBody(t, "\"p") == "65;1\"p");
}

TEST(anOverlongSelectorCannotRunPastTheBuffer)
{
    // The appends are bounded, so a selector far longer than any reply still
    // produces a well-formed frame rather than a partial write.
    Ubiquitty::Terminal t;
    freshTerminal(t);

    const std::string huge(4096, 'q');
    const std::string reply = requestSetting(t, huge);
    CHECK(reply == "\x1bP0$r\x1b\\");
}

TEST(anUnknownSelectorReportsTheMiss)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    // DCS 0 $ r ST — the reply that says the setting was not recognised, with
    // no body and no selector echoed back.
    CHECK(requestSetting(t, "zz") == "\x1bP0$r\x1b\\");
}

TEST(theReplyIsFramedAsADeviceControlString)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    CHECK(requestSetting(t, "m") == "\x1bP1$r0m\x1b\\");
}

TEST(theWidestPenStillFitsTheReplyFrame)
{
    // Every attribute set at once with truecolor on both halves is the longest
    // answer the SGR report assembles. It is refused rather than sent short if
    // it outgrows its frame, so a reply that comes back as a miss here means the
    // frame ran out — which is what this asserts it has not.
    Ubiquitty::Terminal t;
    freshTerminal(t);

    feed(t, "\x1b[0;1;2;3;4;5;7;8;9;38;2;255;255;255;48;2;255;255;255m");
    const std::string widest = settingBody(t, "m");
    CHECK(widest == "0;1;4;5;7;8;2;3;9;38:2::255:255:255;48:2::255:255:255m");
    CHECK(requestSetting(t, "m") != "\x1bP0$r\x1b\\");
}

UBIQUITTY_TEST_MAIN()
