// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_terminfo — the xterm-ubiquitty terminfo source must advertise exactly the
// capabilities the Source/Core VT100 engine backs. An overclaimed cap is worse
// than a missing one: an app that trusts it emits a sequence the engine drops or
// misreads, and the damage shows up as wrong output with nothing pointing here.
// Each substring asserted below is paired (in UbiquittyTerminfo.cpp's header)
// with the Core handler that proves it real. The test compiles against the
// portable Terminfo TU only; it does not spawn `tic`.

#include "Core/PaletteRGB.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Terminfo.h"
#include "Core/Terminfo/UbiquittyTerminfo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace Ubiquitty::Terminfo;

// Has(cap) — the source text contains the literal capability fragment.
static bool has(const char* needle) { return std::strstr(sourceText(), needle) != nullptr; }
#define HAS(s) CHECK(has(s))
#define HASNOT(s) CHECK(!has(s))

TEST(terminfoCases)
{

    const char* src = sourceText();
    CHECK(src != nullptr && src[0] != 0);

    // Canonical name + aliases. xterm-ubiquitty is the gating name so apps
    // keying on the "xterm-*" family recognise it.
    HAS("xterm-ubiquitty|ubiquitty|ubiquitty-256color");

    // 256-color indexed, plus 24-bit truecolor advertised via Tc and signalled
    // at runtime by COLORTERM=truecolor. We do not claim the
    // ncurses RGB cap: RGB promises setaf/setab themselves consume a packed
    // 24-bit value (a direct-color setaf paired with colors#0x1000000), but
    // ours are the indexed 38;5;N form — so claiming RGB makes ncurses apps
    // (e.g. Emacs) emit 38;5;<packed-rgb>, an out-of-range index that renders
    // as no color. See the entry's comment in Core/Terminfo.cpp.
    HAS("colors#0x100");
    HAS("pairs#0x10000");
    HAS("Tc,");
    HASNOT("RGB,");
    HAS("setaf=");
    HAS("setab=");

    // Italics (SGR 3/23 -> Attribute::Italic) and strikethrough (SGR 9/29 ->
    // Attribute::Strike), both in Source/Core/SGR.cpp.
    HAS("sitm=\\E[3m");
    HAS("ritm=\\E[23m");
    HAS("smxx=\\E[9m");
    HAS("rmxx=\\E[29m");

    // Dim (SGR 2 -> Attribute::Dim).
    HAS("dim=\\E[2m");

    // Styled + colored underline: Smulx (SGR 4:n) and Setulc (SGR 58/59).
    HAS("Smulx=");
    HAS("Setulc=");

    // OSC 52 clipboard (handleClipboard).
    HAS("Ms=\\E]52;");

    // Bracketed paste: 2004 enable/disable pair + the paste delimiters.
    HAS("BD=\\E[?2004l");
    HAS("BE=\\E[?2004h");
    HAS("PS=\\E[200~");
    HAS("PE=\\E[201~");

    // Synchronized output (DECSET 2026).
    HAS("Sync=\\E[?2026");

    // Focus events (DECSET 1004).
    HAS("fe=\\E[?1004h");
    HAS("fd=\\E[?1004l");

    // SGR mouse (1006) with cell-motion (1000) — enable string + the
    // mouse report key prefix.
    HAS("XM=\\E[?1006;1000");
    HAS("xm=\\E[<");
    HAS("kmous=\\E[<");

    // DECSCUSR cursor styles (csiH_decscusr) + cursor color (OSC 12 / 112).
    HAS("Ss=\\E[%p1%d\\sq");
    HAS("Se=\\E[7\\sq");
    HAS("Cr=\\E]112");
    HAS("Cs=\\E]12;");

    // Window/icon title: hs boolean + tsl/fsl/dsl driving OSC 2
    // (oscH_title handles OSC 0/1/2).
    HAS("hs,");
    HAS("tsl=\\E]2;");
    HAS("fsl=^G");
    HAS("dsl=\\E]2;\\007");

    // Accurate key caps. Home/End in SS3 form (the engine emits \EOH/\EOF
    // under DECCKM, which smkx enables); modified forms in CSI 1;mod form.
    HAS("khome=\\EOH");
    HAS("kend=\\EOF");
    HAS("kHOM=\\E[1;2H");
    HAS("kEND=\\E[1;2F");
    HAS("kind=\\E[1;2B");
    HAS("kri=\\E[1;2A");
    HAS("kcuu1=\\EOA");
    HAS("kbs=^?");
    HAS("smkx=\\E[?1h\\E=");
    HAS("rmkx=\\E[?1l\\E>");

    // Honesty guard: capabilities terminfo has no portable string for must
    // not appear as invented caps even though the engine backs the feature
    // out-of-band (sixel via DA1, OSC 8 hyperlinks, OSC 7 currentDirectory). A bogus
    // "Sixel" / "sixel" cap or an OSC 8 string cap would be over-promising.
    HASNOT("sixel");
    HASNOT("Sixel");

    // exitCodeFor contract: only Success maps to 0.
    CHECK(exitCodeFor(InstallResult::Success) == 0);
    CHECK(exitCodeFor(InstallResult::TicMissing) != 0);
    CHECK(exitCodeFor(InstallResult::WriteFailed) != 0);
    CHECK(exitCodeFor(InstallResult::TicFailed) != 0);
    CHECK(exitCodeFor(InstallResult::SpawnFailed) != 0);
}

// XTGETTCAP is the entry, not a list beside it. Every capability the source
// names answers over DCS + q with the bytes the compiled entry holds, the
// termcap spellings a startup probe uses reach the same values, and a name
// the entry does not carry is a miss — including the ones a terminal is
// tempted to invent.
TEST(terminfoAnswersXTGETTCAPFromTheEntry)
{
    using Ubiquitty::Terminfo::lookup;

    // The population is the source text itself, split by a second reading of
    // it: a name is whatever precedes `=`, `#` or the separator on a line
    // that is not a comment. A capability this walk finds and lookup misses
    // is a parser gap.
    std::string_view text = sourceText();
    int named = 0;
    int answered = 0;
    bool hasSeenNames = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (end != std::string_view::npos)
            text.remove_prefix(end + 1);
        else
            text = {};
        if (line.starts_with('#'))
            continue;
        std::size_t position = 0;
        while (position < line.size()) {
            std::size_t comma = position;
            while (comma < line.size() && !(line[comma] == ',' && (comma == 0 || line[comma - 1] != '\\')))
                ++comma;
            std::string_view field = line.substr(position, comma - position);
            position = comma + 1;
            while (!field.empty() && (field.front() == ' ' || field.front() == '\t'))
                field.remove_prefix(1);
            if (field.empty())
                continue;
            if (!hasSeenNames) {
                hasSeenNames = true;
                continue;
            }
            const std::string_view name = field.substr(0, field.find_first_of("=#"));
            ++named;
            if (lookup(name))
                ++answered;
            else
                std::fprintf(stderr, "terminfo: the entry names %.*s and XTGETTCAP misses it\n", static_cast<int>(name.size()), name.data());
        }
    }
    CHECK(named > 100);
    CHECK_EQ(answered, named);

    // The bytes, decoded the way tic stores them.
    CHECK(lookup("name") == "xterm-ubiquitty");
    CHECK(lookup("colors") == "256");
    CHECK(lookup("pairs") == "65536");
    CHECK(lookup("Tc") == "");
    CHECK(lookup("Smulx") == "\033[4:%p1%dm");
    CHECK(lookup("Ms") == "\033]52;%p1%s;%p2%s\007");
    CHECK(lookup("E3") == "\033[3J");
    CHECK(lookup("kmous") == "\033[<");
    CHECK(lookup("kbs") == "\177");
    CHECK(lookup("Ss") == "\033[%p1%d q");
    CHECK(lookup("flash") == "\033[?5h$<100/>\033[?5l");
    CHECK(lookup("Sync") == "\033[?2026%?%p1%{1}%-%tl%eh%;");
    CHECK(lookup("kf13") == "\033[1;2P");
    CHECK(lookup("kf63") == "\033[1;4R");
    CHECK(lookup("kUP5") == "\033[1;5A");
    CHECK(lookup("kDC7") == "\033[3;7~");
    CHECK(lookup("XR") == "\033[>0q");
    CHECK(lookup("rs2") == "\033[!p\033[4l\033>");

    // Termcap spellings reach the same capability.
    CHECK(lookup("TN") == lookup("name"));
    CHECK(lookup("Co") == "256");
    CHECK(lookup("ku") == "\033OA");
    CHECK(lookup("kh") == lookup("khome"));
    CHECK(lookup("Km") == lookup("kmous"));

    // Misses: the empty name, a capability the entry has no line for, an
    // invented one, and a name that is only a prefix of a real one.
    CHECK(!lookup(""));
    CHECK(!lookup("kf64"));
    CHECK(!lookup("Sixel"));
    CHECK(!lookup("RGB"));
    CHECK(!lookup("Smul"));
    CHECK(!lookup("k"));
}

// The install path: findTic, the temp source write, the tic spawn, and the
// result mapping. Every assertion goes through an EXPLICIT destination
// directory — install(nullptr) resolves the real ~/.terminfo, which a test
// must never touch. That alone is not enough: when `tic` cannot write its
// -o destination it FALLS BACK to $HOME/.terminfo silently (and still exits
// zero on macOS), so the whole case also runs under a sandbox HOME — measured
// here the fallback overwrote the developer's real installed entry.
//
// A host without `tic` reports TicMissing before anything is written; the
// arms below that need a real spawn are gated on that first verdict rather
// than on a duplicate of findTic's probe order. TicMissing and SpawnFailed
// themselves have no seam to force on a host whose /usr/bin/tic exists, so
// those arms are asserted only through exitCodeFor above.
// ccc tells a curses application the palette can be redefined; init_color()
// then emits whatever initc says, and returns ERR when the entry has no initc.
// Advertising one without the other is the state where can_change_color()
// answers yes and every attempt to use the answer changes nothing, silently —
// which is what this entry did while the emulator answered OSC 4 the whole
// time.
TEST(terminfoPaletteRedefinitionIsDeliverable)
{
    const char* src = sourceText();
    CHECK(src != nullptr && src[0] != 0);

    const bool canChangeColor = std::strstr(src, " ccc,") != nullptr;
    CHECK(canChangeColor);
    if (canChangeColor) {
        CHECK_MSG(std::strstr(src, "initc=") != nullptr,
            "ccc without initc: can_change_color() says yes and init_color() returns ERR");
        CHECK_MSG(std::strstr(src, "oc=") != nullptr,
            "ccc without oc: nothing tells an application how to put the palette back");
    }

    // What initc emits has to be a sequence this emulator answers, or the entry
    // is telling applications to do something that does nothing. The bytes here
    // are what tparm produces from the entry's initc for colour 3 at full red
    // in curses' 0..1000 scale, and the reset is the entry's own oc.
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 20);
    const Ubiquitty::PaletteRGB before = terminal.palette.indexed[3];

    Ubiquitty::processBytes(&terminal, "\x1b]4;3;rgb:FF/00/00\x1b\\");
    const Ubiquitty::PaletteRGB redefined = terminal.palette.indexed[3];
    // Channels are 16-bit here; an 8-bit 0xFF arrives as 0xFFFF.
    CHECK_MSG(redefined.red == 0xFFFF && redefined.green == 0 && redefined.blue == 0,
        "OSC 4 did not redefine the indexed colour initc targets");

    Ubiquitty::processBytes(&terminal, "\x1b]104\x07");
    const Ubiquitty::PaletteRGB restored = terminal.palette.indexed[3];
    CHECK_MSG(restored.red == before.red && restored.green == before.green && restored.blue == before.blue,
        "OSC 104 did not restore the colour oc resets");
}

TEST(terminfoInstallCases)
{
    char destTemplate[] = "/tmp/ubiquitty-terminfo-test.XXXXXX";
    char* dest = ::mkdtemp(destTemplate);
    CHECK(dest != nullptr);
    if (!dest)
        return;

    const char* oldHome = ::getenv("HOME");
    const std::string savedHome = oldHome ? oldHome : "";
    const std::string sandboxHome = std::string(dest) + "/home";
    std::filesystem::create_directory(sandboxHome);
    ::setenv("HOME", sandboxHome.c_str(), 1);

    const std::string root = std::string(dest) + "/db";
    std::filesystem::create_directory(root);
    const InstallResult first = install(root.c_str());
    if (first == InstallResult::TicMissing) {
        // No tic on this host: nothing may have been written to the destination.
        CHECK(std::filesystem::is_empty(root));
    } else {
        // A host with tic: the entry compiles and lands under the destination —
        // a compiled `xterm-ubiquitty` leaf in one of ncurses' two layouts
        // (single-letter dir on most builds, hex-coded on macOS).
        CHECK(first == InstallResult::Success);
        bool entryFound = false;
        for (const auto& walked : std::filesystem::recursive_directory_iterator(root))
            if (walked.is_regular_file() && walked.path().filename() == "xterm-ubiquitty")
                entryFound = true;
        CHECK(entryFound);

        // Idempotent: a second install overwrites in place and still succeeds.
        CHECK(install(root.c_str()) == InstallResult::Success);

        // A null destination resolves $HOME/.terminfo — the sandbox home here,
        // since PAL::systemHomeDirectory consults the environment first.
        CHECK(install(nullptr) == InstallResult::Success);
        CHECK(std::filesystem::exists(sandboxHome + "/.terminfo"));

        // An unwritable destination with an unwritable home leaves tic nowhere
        // to write — the fallback is the reason home must be pinned too.
        //
        // Skipped under root, which is not subject to the directory permission
        // the case depends on: CAP_DAC_OVERRIDE makes the unwritable
        // destination writable, so tic succeeds and the case would assert the
        // opposite of what it names. Reported rather than dropped silently —
        // a case that did not run must not read as one that passed.
        if (::geteuid() != 0) {
            const std::string roDest = std::string(dest) + "/rodest";
            const std::string roHome = std::string(dest) + "/rohome";
            std::filesystem::create_directory(roDest);
            std::filesystem::create_directory(roHome);
            std::filesystem::permissions(roDest, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
            std::filesystem::permissions(roHome, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
            ::setenv("HOME", roHome.c_str(), 1);
            CHECK(install(roDest.c_str()) == InstallResult::TicFailed);
            ::setenv("HOME", sandboxHome.c_str(), 1);
            std::filesystem::permissions(roDest, std::filesystem::perms::owner_all);
            std::filesystem::permissions(roHome, std::filesystem::perms::owner_all);
        } else {
            UbiquittyTest::skip("the unwritable-destination check needs a user the directory permission binds, not root");
        }

        // A TMPDIR that does not exist starves the temp source write, which is
        // reported before tic is ever spawned.
        const char* oldTemporaryDir = ::getenv("TMPDIR");
        const std::string savedTemporaryDir = oldTemporaryDir ? oldTemporaryDir : "";
        ::setenv("TMPDIR", "/nonexistent-ubiquitty-tmp/none", 1);
        CHECK(install(root.c_str()) == InstallResult::WriteFailed);
        if (oldTemporaryDir)
            ::setenv("TMPDIR", savedTemporaryDir.c_str(), 1);
        else
            ::unsetenv("TMPDIR");
    }

    if (oldHome)
        ::setenv("HOME", savedHome.c_str(), 1);
    else
        ::unsetenv("HOME");
    std::filesystem::remove_all(dest);
}

UBIQUITTY_TEST_MAIN()
