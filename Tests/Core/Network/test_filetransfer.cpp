// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_filetransfer.cpp — portable in-band file-transfer detection
// (Network/FileTransferDetect.{h,cpp}) + native remote-launch argv assembly
// (Network/RemoteLaunch.h). Pins the ZMODEM "**\x18B" preamble + direction
// inference (sz→receive vs rz→send), the cross-burst carry matcher, the Kermit
// send-init, the no-false-positive floor, the ssh/mosh argv (agent-fwd,
// SOCKS/HTTP ProxyCommand, host-injection guard), and the ZMODEM agent argv.

#include "Core/Network/FileTransferDetect.h"
#include "Core/Network/FileTransferLaunch.h"
#include "Core/Network/RemoteLaunch.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;

// A full ZMODEM hex-header line as a sender (`sz`) emits it: it primes the
// receiver with "rz\r" then sends ZRQINIT as a hex header "**\x18B00...".
static std::string zsendPreamble()
{
    std::string s = "rz\r"; // sz tells the remote rz to start
    s += "**\x18";
    s += 'B'; // ZHEX format
    s += "00000000000000"; // hex digits of the header (content irrelevant to detect)
    return s;
}

// A receiver (`rz`) waiting: it emits ZRINIT as a hex header with no "rz\r".
static std::string zrecvPreamble()
{
    std::string s = "**\x18";
    s += 'B';
    s += "01000000000000";
    return s;
}

TEST(filetransfer)
{

    using namespace Network::Transfer;

    // ---- ZMODEM: sender preamble → we RECEIVE (download) ----
    {
        Detection d = detectPreamble(zsendPreamble());
        CHECK(d);
        CHECK(d.protocol == Protocol::ZModem);
        CHECK(d.direction == Direction::Receive);
    }

    // ---- ZMODEM: receiver preamble (no "rz\r") → we SEND (upload) ----
    {
        Detection d = detectPreamble(zrecvPreamble());
        CHECK(d);
        CHECK(d.protocol == Protocol::ZModem);
        CHECK(d.direction == Direction::Send);
    }

    // ---- binary header format 'A' / CRC-32 'C' also recognised ----
    {
        std::string a = "**\x18";
        a += 'A';
        CHECK(detectPreamble(a).protocol == Protocol::ZModem);
        std::string c = "**\x18";
        c += 'C';
        CHECK(detectPreamble(c).protocol == Protocol::ZModem);
    }

    // ---- "**\x18" not followed by a valid format byte is not zmodem ----
    {
        std::string bad = "**\x18";
        bad += 'Z'; // not A/B/C
        CHECK(!detectPreamble(bad));
    }

    // ---- no false positive on ordinary terminal output ----
    {
        CHECK(!detectPreamble("ls -la /home/user\r\n"));
        CHECK(!detectPreamble("the answer is **bold** text"));
        CHECK(!detectPreamble("")); // empty
        CHECK(!detectPreamble("\x18\x18\x18")); // bare CANs (ZMODEM abort), no header
    }

    // ---- Kermit send-init: SOH <len> <seq> 'S' ----
    {
        std::string k;
        k += '\x01'; // SOH
        k += ' '; // len (char+32)
        k += ' '; // seq
        k += 'S'; // send-init
        Detection d = detectPreamble(k);
        CHECK(d);
        CHECK(d.protocol == Protocol::Kermit);
        CHECK(d.direction == Direction::Receive);
    }

    // ---- streaming Detector: handshake split across two read() bursts ----
    {
        Detector det;
        std::string full = zsendPreamble();
        // Split mid-header: first burst ends inside "**\x18", second has the rest.
        std::string_view sv(full);
        std::size_t headerAt = full.find("**\x18");
        std::size_t cut = headerAt + 2; // between the two '*' ... actually inside "**"
        Detection first = det.feed(sv.substr(0, cut));
        CHECK(!first); // not yet — header incomplete
        Detection second = det.feed(sv.substr(cut));
        CHECK(second);
        CHECK(second.protocol == Protocol::ZModem);
        CHECK(second.direction == Direction::Receive);
    }

    // ---- Detector carry is bounded (a long no-match stream never grows carry) ----
    {
        Detector det;
        std::string noise(10000, 'x');
        for (int i = 0; i < 5; ++i)
            CHECK(!det.feed(noise));
        // (No direct carry accessor; reaching here without a false positive +
        // without unbounded growth is the contract. reset clears it.)
        det.reset();
    }

    // ---- protocolName ----
    CHECK(protocolName(Protocol::ZModem) == "ZMODEM");
    CHECK(protocolName(Protocol::Kermit) == "Kermit");
    CHECK(protocolName(Protocol::None) == "none");

    // ============ RemoteLaunch ssh/mosh argv ============
    using namespace Network;

    // ---- plain ssh ----
    {
        auto argv = buildSSHArguments("/usr/bin/ssh", "user@host");
        CHECK(argv.size() == 3);
        CHECK(argv[0] == "/usr/bin/ssh");
        CHECK(argv[1] == "-t");
        CHECK(argv[2] == "user@host");
    }

    // ---- ssh with agent forwarding (-A) ----
    {
        RemoteOptions opt;
        opt.forwardAgent = true;
        auto argv = buildSSHArguments("/usr/bin/ssh", "host", opt);
        CHECK(argv.size() == 4);
        CHECK(argv[0] == "/usr/bin/ssh");
        CHECK(argv[1] == "-A");
        CHECK(argv[2] == "-t");
        CHECK(argv[3] == "host");
    }

    // ---- ssh with a SOCKS5 proxy → -o ProxyCommand ----
    {
        RemoteOptions opt;
        opt.proxy = {
            ProxyKind::SOCKS5,
            "proxy.example",
            1080,
        };
        auto argv = buildSSHArguments("/usr/bin/ssh", "host", opt);
        // ssh, -o, ProxyCommand=..., -t, host
        CHECK(argv.size() == 5);
        CHECK(argv[1] == "-o");
        CHECK(argv[2] == "ProxyCommand=nc -X 5 -x proxy.example:1080 %h %p");
        CHECK(argv[3] == "-t");
        CHECK(argv[4] == "host");
    }

    // ---- ssh with an HTTP proxy ----
    {
        RemoteOptions opt;
        opt.proxy = {
            ProxyKind::HTTP,
            "10.0.0.1",
            3128,
        };
        auto argv = buildSSHArguments("/usr/bin/ssh", "host", opt);
        CHECK(argv[2] == "ProxyCommand=nc -X connect -x 10.0.0.1:3128 %h %p");
    }

    // ---- host-injection guard: a '-' host yields no argv ----
    {
        CHECK(buildSSHArguments("/usr/bin/ssh", "-oProxyCommand=evil").empty());
        CHECK(buildSSHArguments("/usr/bin/ssh", "user@-evil").empty());
        CHECK(buildSSHArguments("", "host").empty()); // no binary
    }

    // ---- a malformed proxy host is dropped (no -o) but the session still launches ----
    {
        RemoteOptions opt;
        opt.proxy = {
            ProxyKind::SOCKS5,
            "-evil",
            1080,
        };
        auto argv = buildSSHArguments("/usr/bin/ssh", "host", opt);
        CHECK(argv.size() == 3); // ssh -t host — proxy silently skipped
        CHECK(argv[1] == "-t");
    }

    // ---- plain mosh ----
    {
        auto argv = buildMoshArguments("/usr/local/bin/mosh", "user@host");
        CHECK(argv.size() == 2);
        CHECK(argv[0] == "/usr/local/bin/mosh");
        CHECK(argv[1] == "user@host");
    }

    // ---- mosh with agent forwarding → --ssh override ----
    {
        RemoteOptions opt;
        opt.forwardAgent = true;
        auto argv = buildMoshArguments("/usr/local/bin/mosh", "host", opt);
        CHECK(argv.size() == 3);
        CHECK(argv[1] == "--ssh=ssh -A");
        CHECK(argv[2] == "host");
    }

    // ---- mosh with agent forwarding + proxy ----
    {
        RemoteOptions opt;
        opt.forwardAgent = true;
        opt.proxy = {
            ProxyKind::SOCKS5,
            "proxy",
            1080,
        };
        auto argv = buildMoshArguments("/usr/local/bin/mosh", "host", opt);
        CHECK(argv.size() == 3);
        CHECK(argv[1] == "--ssh=ssh -A -o \"ProxyCommand=nc -X 5 -x proxy:1080 %h %p\"");
    }

    // ---- mosh host-injection guard ----
    CHECK(buildMoshArguments("/usr/local/bin/mosh", "-evil").empty());
    CHECK(buildMoshArguments("", "host").empty());
}

// A ZMODEM receive is driven by the remote end: the sender names the file. lrzsz's
// default refuses an absolute path, a parent reference and an overwrite; a leading
// dot it allows, and -R is what refuses that. Measured against lrzsz 0.12.20 with
// a real sz and rz: a file sent as `.hidden` is WRITTEN without the flag and
// refused with it ("rz: Security Violation"). So the flag is the difference
// between a remote sender being able to drop a .zshrc into the download directory
// and not — the directory is user-confirmed, but the file that arrives is not the
// file the user agreed to receive.
TEST(testZModemArgumentsAreRestricted)
{
    using namespace Ubiquitty::Network;

    const std::vector<std::string> receive = buildZModemArguments("/usr/local/bin/rz", TransferDirection::Receive);
    CHECK(!receive.empty());
    CHECK(receive[0] == "/usr/local/bin/rz");
    CHECK(std::ranges::find(receive, "-R") != receive.end());
    CHECK(std::ranges::find(receive, "-b") != receive.end());
    CHECK(std::ranges::find(receive, "-e") != receive.end());
    CHECK(receive.size() == 4); // no stray file argument on a receive

    // A send carries the file and the same policy.
    const std::vector<std::string> send = buildZModemArguments("/usr/local/bin/sz", TransferDirection::Send, "/tmp/report.pdf");
    CHECK(send.size() == 5);
    CHECK(send.back() == "/tmp/report.pdf");
    CHECK(std::ranges::find(send, "-R") != send.end());

    // Empty means do not spawn, rather than spawning something bare.
    CHECK(buildZModemArguments("", TransferDirection::Receive).empty());
    CHECK(buildZModemArguments("/usr/local/bin/sz", TransferDirection::Send).empty()); // send with no file
}

// Detection is driven by PTY output, so anything that can print decides when an
// app-modal prompt appears. Without a limit on the answer, a stream that repeats
// the preamble holds the app in a dialog loop the user cannot outrun: each
// prompt blocks everything until dismissed, and dismissing it arms the next.
// Declining is the signal that bounds it — say no twice and this stream stops
// being able to ask.
TEST(testTransferPromptBudgetStopsARepeatingStream)
{
    using namespace Ubiquitty::Network;

    PromptBudget budget;
    CHECK(budget.mayPrompt(AutoReceive::Enabled)); // a first detection always reaches the user

    budget.recordDecline();
    CHECK(budget.mayPrompt(AutoReceive::Enabled)); // one decline is a mis-click, not an answer

    budget.recordDecline();
    CHECK(!budget.mayPrompt(AutoReceive::Enabled)); // twice is an answer — the stream is done asking

    // Further declines cannot un-stick it, and a hostile stream gets no benefit
    // from repeating: the budget is already spent.
    budget.recordDecline();
    CHECK(!budget.mayPrompt(AutoReceive::Enabled));

    // Accepting is what the user wanting the feature looks like, so the count
    // starts over rather than leaking a decline from an earlier transfer.
    PromptBudget accepted;
    accepted.recordDecline();
    accepted.recordAccept();
    CHECK(accepted.consecutiveDeclines() == 0);
    accepted.recordDecline();
    CHECK(accepted.mayPrompt(AutoReceive::Enabled)); // the accept really cleared it, not just decremented

    // And an explicit re-enable restores prompting.
    budget.reset();
    CHECK(budget.mayPrompt(AutoReceive::Enabled));
}

// `[transfer] auto_receive` was parsed into Values and read by nothing: the
// template advertised it, the field held the user's answer, and every detected
// handshake prompted regardless. The setting lives on this predicate rather than
// in a frontend `if` for the reason the budget does — it is the same question on
// every surface, and the signature makes the next frontend supply an answer
// instead of forgetting to ask.
TEST(testAutoReceiveOffStopsADetectionFromPrompting)
{
    using namespace Ubiquitty::Network;

    // A full budget is not enough on its own: with auto-receive off, a
    // detection does not prompt whatever the budget holds.
    PromptBudget fresh;
    CHECK(!fresh.mayPrompt(AutoReceive::Disabled));
    CHECK(fresh.mayPrompt(AutoReceive::Enabled)); // and the budget really is intact

    // Turning it off costs no budget, so a user who re-enables it mid-session
    // gets the same two chances they started with rather than a spent counter.
    CHECK(fresh.consecutiveDeclines() == 0);

    // The two reasons are independent, and either one alone refuses.
    PromptBudget spent;
    spent.recordDecline();
    spent.recordDecline();
    CHECK(!spent.mayPrompt(AutoReceive::Enabled));
    CHECK(!spent.mayPrompt(AutoReceive::Disabled));

    // Off outranks a reset: an explicit return to automatic prompting restores
    // the budget and still asks the setting.
    spent.reset();
    CHECK(spent.mayPrompt(AutoReceive::Enabled));
    CHECK(!spent.mayPrompt(AutoReceive::Disabled));
}

TEST(testMatchOffsetSeparatesOutputFromFrame)
{
    using namespace Network::Transfer;

    // The offset is what lets the caller keep the output that precedes the
    // preamble — the command echo, an rz banner — while the frame stays off
    // the screen. Each case pins where the boundary falls in the firing feed.

    // Before any fire, the accessor answers its sentinel.
    {
        Detector detector;
        CHECK(detector.lastMatchOffsetInFeed() == -1);
        CHECK(!detector.feed("plain output\r\n"));
        CHECK(detector.lastMatchOffsetInFeed() == -1);
    }

    // A preamble opening the feed fences at its first ZPAD: the "rz\r" ahead
    // of it is the sender's lead-in text, which terminals display.
    {
        Detector detector;
        CHECK(detector.feed(zsendPreamble()));
        CHECK(detector.lastMatchOffsetInFeed() == 3);
    }

    // Output ahead of the preamble is exactly what the offset preserves. The
    // "rz\r" lead-in belongs to the preamble's own text and parses with the
    // echo — the fence is the first ZPAD.
    {
        Detector detector;
        const std::string echo = "sz notes.txt\r\n";
        CHECK(detector.feed(echo + zsendPreamble()));
        CHECK(detector.lastMatchOffsetInFeed() == static_cast<int>(echo.size() + 3)); // echo + "rz\r"
    }

    // A Kermit packet's fence is its SOH.
    {
        Detector detector;
        std::string k = "kermit -r\r\n";
        const int sohAt = static_cast<int>(k.size());
        k += '\x01';
        k += ' ';
        k += ' ';
        k += 'S';
        CHECK(detector.feed(k));
        CHECK(detector.lastMatchOffsetInFeed() == sohAt);
    }

    // A preamble split across feeds fires on the completing feed with offset
    // zero: its head arrived earlier, so none of the completing feed is output.
    {
        Detector detector;
        const std::string whole = zsendPreamble();
        CHECK(!detector.feed(whole.substr(0, 4))); // "rz\r*" — a candidate begins
        CHECK(detector.feed(whole.substr(4)));
        CHECK(detector.lastMatchOffsetInFeed() == 0);
    }

    // With a carry in front, the offset is still feed-relative: the carry's
    // bytes were an earlier feed's and already parsed.
    {
        Detector detector;
        CHECK(!detector.feed("earlier output"));
        const std::string echo = "$ sz a.bin\r\n";
        CHECK(detector.feed(echo + zsendPreamble()));
        CHECK(detector.lastMatchOffsetInFeed() == static_cast<int>(echo.size() + 3));
    }

    // A completing feed that begins no candidate of its own takes the carry
    // route — the bounded straddle window — and answers zero the same way.
    {
        Detector detector;
        CHECK(!detector.feed("rz\r**\x18")); // header split before its format byte
        CHECK(detector.feed("B0000000"));
        CHECK(detector.lastMatchOffsetInFeed() == 0);
    }

    // Reset forgets the fire, so a stale offset cannot leak into the next
    // session's first detection.
    {
        Detector detector;
        CHECK(detector.feed(zsendPreamble()));
        detector.reset();
        CHECK(detector.lastMatchOffsetInFeed() == -1);
    }
}

UBIQUITTY_TEST_MAIN()
