// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Typed pane.capture extraction: whole rows, command marks, clamping and the
// explicit incomplete result at the text cap. Requests cross the real inner
// Call/Result transport; no legacy region frame can satisfy these assertions.

#include "Assertions.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/Control/CaptureReply.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace JSON = Ubiquitty::Codec::JSON;
namespace Control = Ubiquitty::Protocol::Control;

namespace {

struct RegionReadAccess {
    static auto& addSession(Daemon& daemon, std::uint32_t, int rows, int columns)
    {
        return Screen::makeSession(daemon, rows, columns);
    }

    static std::size_t addAgent(Daemon& daemon, std::uint32_t sessionID, int*)
    {
        const auto index = Screen::attach(daemon, sessionID);
        Screen::grantScope(daemon, index, { Sync::Scope::Observe, Sync::Scope::RegionRead });
        // No person is attached to approve it, so the agent holds the standing
        // grant a person's allow-for-this-session answer gives.
        Screen::setTokenID(daemon, index, "region-reader");
        Screen::grantAgentSession(daemon, *Screen::findSession(daemon, sessionID), index);
        return index;
    }
};

struct CaptureRange {
    int status = 0;
    std::int64_t absoluteRowStart = 0;
    std::int64_t absoluteRowEnd = 0;
    bool isComplete = false;
    std::string text;
};

// A new reader per extraction keeps each assertion independent of cached
// screen/history contents. Only the typed Result supplies the returned text.
bool readRegion(Daemon& daemon, std::size_t index, int, Control::CaptureSource source,
    std::int64_t start, std::int64_t end, CaptureRange& out)
{
    UbiquittyTest::ScreenClient client(24, 80, 0);
    Screen::greet(daemon, index, client);
    JSON::Value params = JSON::Value::object();
    params.set("pane", JSON::Value::number(1));
    params.set("source", JSON::Value::string(std::string(Control::captureSourceName(source))));
    if (source == Control::CaptureSource::Rows) {
        const auto narrow = [](std::int64_t row) { return std::clamp<std::int64_t>(row, INT32_MIN, INT32_MAX); };
        params.set("start", JSON::Value::number(static_cast<double>(narrow(start))));
        params.set("end", JSON::Value::number(static_cast<double>(narrow(end))));
    }
    JSON::Value request = JSON::Value::object();
    request.set("jsonrpc", JSON::Value::string("2.0"));
    request.set("id", JSON::Value::number(77));
    request.set("method", JSON::Value::string("pane.capture"));
    request.set("params", std::move(params));
    if (!client.call(JSON::serialize(request)))
        return false;
    Screen::receive(daemon, index, client.takeStream(Screen::now()));
    Screen::service(daemon);
    Screen::deliver(daemon, index, client);
    const auto results = client.takeResults();
    if (results.size() != 1)
        return false;
    bool isParsed = false;
    const auto response = JSON::parse(results[0], isParsed);
    if (!isParsed || response.numberOr("id") != 77)
        return false;
    if (const auto* error = response.find("error")) {
        out.status = static_cast<int>(error->numberOr("code"));
        return true;
    }
    const auto* result = response.find("result");
    if (!result || result->numberOr("pane") != 1 || result->stringOr("source") != Control::captureSourceName(source))
        return false;
    out.absoluteRowStart = static_cast<std::int64_t>(result->numberOr("first"));
    out.absoluteRowEnd = static_cast<std::int64_t>(result->numberOr("last"));
    out.isComplete = result->boolOr("complete");
    out.text = result->stringOr("text");
    return true;
}

// The rows of a reply's text, split on the newline the daemon writes per row.
std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> rows;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            rows.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        rows.push_back(current);
    return rows;
}

int liveRows(const Ubiquitty::Terminal& t) { return Ubiquitty::Screen::scrollbackCount(&t) + t.rows; }

} // namespace

int main()
{
    // ---- 1. The text on the screen is the text that comes back, one line per
    // row, with the blank tail of each row trimmed. Padding every row to the
    // window width would make an agent's line-anchored match fail and would
    // inflate every read by the width of a window the agent cannot see.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "alpha\r\nbeta   \r\n");

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, 0, 2, reply));
        CHECK(reply.status == 0);
        const std::vector<std::string> rows = lines(reply.text);
        CHECK(rows.size() == 3);
        CHECK(rows[0] == "alpha");
        CHECK(rows[1] == "beta"); // the three spaces the shell wrote are gone
        CHECK(rows[2].empty()); // the row the cursor sits on, still untouched
        CHECK(reply.absoluteRowStart == 0 && reply.absoluteRowEnd == 2);
    }

    // ---- 2. A combining mark travels with the character it modifies. The
    // cluster lives in ONE cell with the mark held beside the base, so a reply
    // that emitted only cell code points would hand the agent an unaccented
    // word that no longer matches what the human sees.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "cafe\xcc\x81"); // café, decomposed

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, 0, 0, reply));
        CHECK(reply.status == 0);
        CHECK(lines(reply.text).at(0) == "cafe\xcc\x81");
    }

    // ---- 3. A double-width glyph is emitted once. It occupies two cells, and
    // the second carries no code point of its own — counting it would double
    // every CJK character in the reply.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "\xe4\xb8\x96\xe7\x95\x8c"); // 世界

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, 0, 0, reply));
        CHECK(reply.status == 0);
        CHECK(lines(reply.text).at(0) == "\xe4\xb8\x96\xe7\x95\x8c");
    }

    // ---- 3b. A gap made by cursor addressing survives the read. A read that
    // dropped every cell whose code point was zero would deliver a status bar or
    // a table with its columns closed up while every frontend rendering those
    // rows through Core showed the reader the gaps.
    //
    // The row is compared byte for byte, and separately checked for a NUL: an
    // untouched cell holds code point zero, and a walk that appended it rather
    // than substituting a space would put a zero byte in the middle of what an
    // agent parses as UTF-8 text. Printed in a terminal the two are
    // indistinguishable.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "a\033[1;21Hb"); // column 21, nothing written between

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, 0, 0, reply));
        CHECK(reply.status == 0);
        const std::string row = lines(reply.text).at(0);
        CHECK(row == "a                   b"); // 19 columns of gap, held open
        CHECK(row.find('\0') == std::string::npos);
    }

    // ---- 3c. A sized block reaches the agent once, not once per row it
    // covers. Its lower rows carry the same glyph so the renderer can crop a
    // band out of one master, and a reader that took each row at face value
    // would hand back a three-row block as the character three times.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        s.term->drawsSizedText = true;
        s.term->drawsProportionalText = true;
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "\033[H\033]66;s=3;A\007"); // three rows tall

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, 0, 2, reply));
        CHECK(reply.status == 0);
        const std::vector<std::string> rows = lines(reply.text);
        CHECK(rows.size() == 3);
        CHECK(rows.at(0).find('A') != std::string::npos);
        CHECK(rows.at(1).find('A') == std::string::npos);
        CHECK(rows.at(2).find('A') == std::string::npos);
    }

    // ---- 4. Rows that scrolled off the screen are still readable. The request
    // addresses the buffer absolutely, so an agent reads history it was never
    // attached for — which is the reason the frame exists.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 4, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        for (int i = 0; i < 10; ++i)
            UbiquittyTest::feed(*s.term, ("line" + std::to_string(i) + "\r\n").c_str());
        CHECK(Ubiquitty::Screen::scrollbackCount(s.term.get()) > 0);

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, 0, 1, reply));
        CHECK(reply.status == 0);
        const std::vector<std::string> rows = lines(reply.text);
        CHECK(rows.at(0) == "line0"); // long since off the top of the screen
        CHECK(rows.at(1) == "line1");
    }

    // ---- 5. A range that starts below the buffer is clamped to its first row,
    // and the reply reports the row it actually started at rather than the one
    // it was asked for.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "first\r\nsecond\r\n");

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, -1, 1, reply));
        CHECK(reply.status == 0);
        CHECK(reply.absoluteRowStart == 0);
        CHECK(lines(reply.text).at(0) == "first");
    }

    // ---- 6. A range that runs past the end is clamped to the last live row,
    // and the reply names that row. An agent resuming from the end it ASKED
    // for would skip every row written after this read.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "only\r\n");
        const int total = liveRows(*s.term);

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, 0, 1000000, reply));
        CHECK(reply.status == 0);
        CHECK(reply.absoluteRowEnd == total - 1);
        CHECK(static_cast<int>(lines(reply.text).size()) == total);
    }

    // ---- 7. The reply is capped, and it says where it stopped. A hostile (or
    // merely optimistic) request for a million rows must not make the daemon
    // build a million rows, and the truncated answer has to carry the row the
    // agent should resume from — the cap is silent data loss otherwise.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 1024);
        s.term->scrollbackLimit = 2000;
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        const std::string wide(1024, 'x');
        for (int i = 0; i < 1200; ++i)
            UbiquittyTest::feed(*s.term, wide + "\r\n");
        const int total = liveRows(*s.term);
        CHECK(total > 1024); // enough rows that 1 MiB of text runs out first

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, 0, 1000000, reply));
        CHECK(reply.status == 0);
        constexpr std::size_t oneMebibyte = 1u << 20;
        CHECK(reply.text.size() >= oneMebibyte);
        CHECK(reply.text.size() < oneMebibyte + 1026); // one row past the cap at most
        CHECK(reply.absoluteRowEnd < total - 1); // it stopped short of the end
        CHECK(!reply.isComplete);
        // The named range and the delivered text agree exactly, which is what
        // makes resuming from absoluteRowEnd + 1 lossless.
        CHECK(static_cast<int>(lines(reply.text).size()) == reply.absoluteRowEnd - reply.absoluteRowStart + 1);
    }

    // ---- 8. READ_LAST_COMMAND finds the output of the last command, not the
    // one before it, and stops at the prompt that followed. A range that ran
    // to the bottom would hand back the next prompt as if it were output.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "\033]133;A\007$ echo one\r\n\033]133;C\007one\r\n\033]133;D;0\007");
        UbiquittyTest::feed(*s.term, "\033]133;A\007$ echo two\r\n\033]133;C\007two\r\n\033]133;D;0\007");
        UbiquittyTest::feed(*s.term, "\033]133;A\007$ ");

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::LastCommand, 0, 0, reply));
        CHECK(reply.status == 0);
        const std::vector<std::string> rows = lines(reply.text);
        CHECK(rows.size() == 1);
        CHECK(rows.at(0) == "two"); // the last command's output
        CHECK(reply.absoluteRowEnd == reply.absoluteRowStart); // the trailing prompt is excluded
    }

    // ---- 9. A command still running reads down to the cursor. There is no
    // prompt after it to bound the range, and an agent watching a build needs
    // the output that exists so far rather than nothing until it finishes.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "\033]133;A\007$ make\r\n\033]133;C\007");
        UbiquittyTest::feed(*s.term, "compiling one\r\ncompiling two\r\ncompiling three\r\n");

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::LastCommand, 0, 0, reply));
        CHECK(reply.status == 0);
        const std::vector<std::string> rows = lines(reply.text);
        CHECK(rows.size() == 4); // three emitted lines plus the row the cursor is on
        CHECK(rows.at(0) == "compiling one");
        CHECK(rows.at(2) == "compiling three");
    }

    // ---- 10. The null for the two above: a terminal with no OSC-133 output
    // mark answers no-command rather than guessing a range. A daemon that
    // returned the whole screen here would tell an agent that a shell without
    // integration had just run something.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "plain output, no shell integration\r\n");

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::LastCommand, 0, 0, reply));
        CHECK(reply.status == -32003);
        CHECK(reply.text.empty());
    }

    // ---- 10. Rows past an int are compared before they are narrowed. A
    // request ending at the largest row reads to the last live row; a start
    // past the buffer is refused. Tail captures obtain extent and text in one
    // snapshot instead of extracting extent from an earlier failed read.
    {
        Daemon d;
        auto& s = RegionReadAccess::addSession(d, 1, 24, 80);
        int readEnd = -1;
        const std::size_t agent = RegionReadAccess::addAgent(d, 1, &readEnd);
        UbiquittyTest::feed(*s.term, "only\r\n");
        const int total = liveRows(*s.term);

        CaptureRange reply {};
        CHECK(readRegion(d, agent, readEnd, Control::CaptureSource::Rows, 0, INT64_MAX, reply));
        CHECK(reply.status == 0);
        CHECK(reply.absoluteRowEnd == total - 1);
        CHECK(lines(reply.text).at(0) == "only");

        CaptureRange refused {};
        const auto nextAgent = RegionReadAccess::addAgent(d, 1, nullptr);
        CHECK(readRegion(d, nextAgent, readEnd, Control::CaptureSource::Rows, INT64_MAX - 1, INT64_MAX, refused));
        CHECK(refused.status == -32003);
        CHECK(refused.text.empty());
    }

    if (failures == 0)
        std::printf("region_read_text: the agent's tail returns the text on the screen — combining marks "
                    "with their base, a wide glyph once, trailing blanks trimmed, scrollback addressed "
                    "absolutely — and the range in every reply is the range delivered, including when "
                    "the 1 MiB cap cuts it short\n");
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
