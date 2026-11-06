// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_quarantinepolicy.cpp — how a file fetched from somewhere else lands
// (Session/QuarantinePolicy.h), so every frontend tags and names a fetched
// file the same way.
//
// Three claims, each one a place a frontend could be the loose one:
//
//   the xattr value is a browser download's four fields, in Safari's shape, so
//   Gatekeeper treats a fetched file like one;
//
//   the name a file is written under is the suggestion's last component and
//   nothing else, so a name carrying `..` or a leading slash cannot place the
//   write outside the fresh directory;
//
//   a relative candidate names a file on the HOST, joined to the host's own
//   reported directory and never to anything on this machine.
//
// Header-only policy, links standalone.

#include "Core/Session/FileLink.h"
#include "Core/Session/QuarantinePolicy.h"

#include <cstdio>
#include <optional>
#include <string>

using Ubiquitty::Session::hostAbsolutePath;
using Ubiquitty::Session::quarantineAttributeName;
using Ubiquitty::Session::quarantineValue;
using Ubiquitty::Session::safeDownloadName;

TEST(fileLinksResolveOnlyOnTheAlreadySelectedHost)
{
    using namespace Ubiquitty::Session;
    CHECK(resolveFileLink("notes/report.txt", FileLinkSyntax::Path, "/srv/project", "build.example") == "/srv/project/notes/report.txt");
    CHECK(resolveFileLink("/srv/report.txt", FileLinkSyntax::Path, {}, "build.example") == "/srv/report.txt");
    CHECK(resolveFileLink("file://BUILD.example/srv/a%20b%2520.txt", FileLinkSyntax::URI, {}, "build.example") == "/srv/a b%20.txt");
    CHECK(resolveFileLink("file:///srv/report.txt", FileLinkSyntax::URI, {}, "build.example") == "/srv/report.txt");
    CHECK(resolveFileLink("file://localhost/srv/report.txt", FileLinkSyntax::URI, {}, "build.example") == "/srv/report.txt");
    CHECK_MSG(resolveFileLink("file:/srv/report.txt", FileLinkSyntax::URI, {}, "build.example") == "/srv/report.txt",
        "file:/path, as Java's File.toURI() writes it, has no authority and names this machine");
    CHECK(resolveFileLink("file:/srv/a%20b.txt", FileLinkSyntax::URI, {}, "build.example") == "/srv/a b.txt");
    CHECK(resolveFileLink("file://[2001:db8::1]/srv/report.txt", FileLinkSyntax::URI, {}, "2001:db8::1") == "/srv/report.txt");
    CHECK(resolveFileLink("file://[::1]/srv/report.txt", FileLinkSyntax::URI, {}, "[::1]") == "/srv/report.txt");
    CHECK(resolveFileLink("file://host/tmp/%252e%3f%23", FileLinkSyntax::URI, {}, "host") == "/tmp/%2e?#");
    CHECK(resolveFileLink(std::string(4096, '/'), FileLinkSyntax::Path, {}, "host"));
    const auto foreign = resolveFileLink("file://other.example/srv/report.txt", FileLinkSyntax::URI, {}, "build.example");
    CHECK(!foreign && foreign.error() == FileLinkError::ForeignHost);
    CHECK(!resolveFileLink("relative.txt", FileLinkSyntax::Path, {}, "build.example"));
    CHECK(!resolveFileLink("relative.txt", FileLinkSyntax::Path, "relative-directory", "build.example"));
    CHECK(isLocalFileLinkURI("file:///tmp/local.txt", "this-machine"));
    CHECK(isLocalFileLinkURI("file://this-machine/tmp/local.txt", "this-machine"));
    CHECK(!isLocalFileLinkURI("file://other-machine/tmp/local.txt", "this-machine"));
    CHECK(!isFileLinkURI("https://example.test/file.txt"));
    CHECK(isFileLinkURI("FiLe:invalid"));
}

TEST(fileLinksRejectAmbiguousAndControlBearingURIs)
{
    using namespace Ubiquitty::Session;
    for (const auto value : { "file:", "file:tmp/file", "file://host", "file://user@host/tmp/file", "file://host:22/tmp/file",
             "file://host/tmp/file?query", "file://host/tmp/file#fragment", "file://host/tmp/%", "file://host/tmp/%zz",
             "file://host/tmp/%00", "file://host/tmp/%0a", "file://host/tmp/%7f", "file://host/tmp/%ff",
             "file://[::1]:22/tmp/file", "file://[::1/tmp/file" }) {
        CHECK(!resolveFileLink(value, FileLinkSyntax::URI, {}, "host"));
    }
    CHECK(!resolveFileLink(std::string("/tmp/a\0b", 8), FileLinkSyntax::Path, {}, "host"));
    CHECK(!resolveFileLink(std::string(4097, 'a'), FileLinkSyntax::Path, "/tmp", "host"));
    CHECK(!resolveFileLink("file", FileLinkSyntax::Path, std::string(4096, '/'), "host"));
    // Shell syntax is inert path data, never a command or a second decoding pass.
    CHECK(resolveFileLink("/tmp/$(touch nope);file", FileLinkSyntax::Path, {}, "host") == "/tmp/$(touch nope);file");
}

// The path a clicked file link hands the opener: this machine's, decoded once,
// and nothing for a link that names another machine or carries a query or a
// fragment, which no filesystem object has and a handler may read its own way.
TEST(aClickedFileLinkNamesOnlyAPathOnThisMachine)
{
    using Ubiquitty::Session::localFileLinkPath;
    constexpr auto utf8 = Ubiquitty::Session::FilePathEncoding::UTF8;
    CHECK(localFileLinkPath("file:///System/Applications/Calculator.app", "this-mac", utf8) == "/System/Applications/Calculator.app");
    CHECK(localFileLinkPath("file://LOCALHOST/tmp/a%20b.txt", "this-mac", utf8) == "/tmp/a b.txt");
    CHECK_MSG(localFileLinkPath("file://this-mac/tmp/listing", "this-mac", utf8) == "/tmp/listing",
        "ls --hyperlink names the machine it ran on");
    CHECK(localFileLinkPath("file://other-mac/tmp/listing", "this-mac", utf8).empty());
    CHECK_MSG(localFileLinkPath("file://other-mac/tmp/listing", "", utf8).empty(), "an unknown host name admits only an empty authority and localhost");
    CHECK(localFileLinkPath("file:///tmp/notes.txt?open", "this-mac", utf8).empty());
    CHECK(localFileLinkPath("file:///tmp/notes.txt#run", "this-mac", utf8).empty());
    CHECK(localFileLinkPath("https://example.test/notes.txt", "this-mac", utf8).empty());
}

// A fragment that names a line is how rg --hyperlink-format and delta point at
// a match, and the file is the same whatever line it opens at, so the path is
// read with the line set aside. Any other fragment is still refused: no file
// has one, and a program could mean it for whichever handler reads it.
TEST(aFileLinkThatNamesALineNamesTheFile)
{
    using Ubiquitty::Session::FileLinkLine;
    using Ubiquitty::Session::fileLinkLine;
    using Ubiquitty::Session::localFileLinkPath;
    constexpr auto utf8 = Ubiquitty::Session::FilePathEncoding::UTF8;

    for (const char* uri : { "file:///tmp/main.swift#12", "file:///tmp/main.swift#L12", "file:///tmp/main.swift#l12",
             "file:///tmp/main.swift#L12-L20", "file:///tmp/main.swift#12:5", "file:///tmp/main.swift#12,14",
             "file:///tmp/main.swift#12:5-14:3", "file://this-mac/tmp/main.swift#123456789" }) {
        CHECK_MSG(localFileLinkPath(uri, "this-mac", utf8) == "/tmp/main.swift", uri);
    }
    for (const char* uri : { "file:///tmp/main.swift#", "file:///tmp/main.swift#run", "file:///tmp/main.swift#0",
             "file:///tmp/main.swift#L", "file:///tmp/main.swift#12a", "file:///tmp/main.swift#12-",
             "file:///tmp/main.swift#-12", "file:///tmp/main.swift#12--14", "file:///tmp/main.swift#1234567890",
             "file:///tmp/main.swift#12#13", "file:///tmp/main.swift?open#12", "file:///Users/me/C#/notes.txt",
             "file://other-mac/tmp/main.swift#12" }) {
        CHECK_MSG(localFileLinkPath(uri, "this-mac", utf8).empty(), uri);
    }

    const auto lineOf = [](const char* uri) {
        const FileLinkLine reference = fileLinkLine(uri);
        return std::to_string(reference.line) + ":" + std::to_string(reference.column);
    };
    CHECK(lineOf("file:///tmp/main.swift#12") == "12:0");
    CHECK(lineOf("file:///tmp/main.swift#L12-L20") == "12:0");
    CHECK_MSG(lineOf("file:///tmp/main.swift#12:5") == "12:5", "a position after a colon is the column");
    CHECK(lineOf("file:///tmp/main.swift#12:5-14:3") == "12:5");
    CHECK_MSG(lineOf("file:///tmp/main.swift#12-14:3") == "12:0", "a column belongs to the line it directly follows");
    CHECK(lineOf("file:///tmp/main.swift#12,14") == "12:0");
    CHECK(lineOf("file:///tmp/main.swift") == "0:0");
    CHECK(lineOf("file:///tmp/main.swift#run") == "0:0");
    CHECK(lineOf("file:///tmp/main.swift#0") == "0:0");
}

// On a thin client a file link names the host's file, so the plate reads it
// against the host's name and with nothing on this machine, the way the click
// asks the host for it: the host's own name or none is that host, a third
// machine is refused, and so is a fragment that names no line.
TEST(aThinClientsFileLinkNamesAPathOnTheHost)
{
    using Ubiquitty::Session::FileLinkSyntax;
    using Ubiquitty::Session::hostFileLinkPath;
    using Ubiquitty::Session::resolveHostFileLink;
    CHECK_MSG(hostFileLinkPath("file://build-box/home/u/notes.txt", "build-box") == "/home/u/notes.txt",
        "ls --hyperlink on the host names the host");
    CHECK(hostFileLinkPath("file:///home/u/a%20b.txt", "build-box") == "/home/u/a b.txt");
    CHECK(hostFileLinkPath("file://localhost/home/u/notes.txt", "build-box") == "/home/u/notes.txt");
    CHECK(hostFileLinkPath("file://this-mac/home/u/notes.txt", "build-box").empty());
    CHECK_MSG(hostFileLinkPath("file:///home/u/main.swift#L12", "build-box") == "/home/u/main.swift", "a line link names the file");
    CHECK(hostFileLinkPath("file:///home/u/main.swift#run", "build-box").empty());
    CHECK(hostFileLinkPath("file:///home/u/notes.txt?open", "build-box").empty());
    CHECK_MSG(hostFileLinkPath("file:///home/u/notes.txt", "").empty(), "a connection with no host name asks for nothing");
    for (const char* uri : { "file://build-box/home/u/notes.txt", "file:///home/u/main.swift#L12", "file://this-mac/x", "file:///x#run" }) {
        const auto asked = resolveHostFileLink(uri, FileLinkSyntax::URI, "/home/u", "build-box");
        CHECK_MSG(hostFileLinkPath(uri, "build-box") == (asked ? asked->path : std::string {}), uri);
    }
}

// A line link clicked on a thin client asks the host for the file it names and
// keeps the line for the copy that lands, as the same link opens a file here
// at its line. A path candidate is literal, so a `#` in it is a name's.
TEST(aThinClientsLineLinkAsksForTheFileAndKeepsTheLine)
{
    using Ubiquitty::Session::FileLinkError;
    using Ubiquitty::Session::FileLinkSyntax;
    using Ubiquitty::Session::resolveHostFileLink;
    const auto ranged = resolveHostFileLink("file://build-box/srv/main.swift#L12-L20", FileLinkSyntax::URI, "/srv", "build-box");
    REQUIRE(ranged.has_value());
    CHECK(ranged->path == "/srv/main.swift");
    CHECK(ranged->line.line == 12 && ranged->line.column == 0);
    const auto columned = resolveHostFileLink("file:///srv/a%20b.swift#12:5", FileLinkSyntax::URI, "/srv", "build-box");
    REQUIRE(columned.has_value());
    CHECK(columned->path == "/srv/a b.swift");
    CHECK(columned->line.line == 12 && columned->line.column == 5);
    const auto plain = resolveHostFileLink("file:///srv/notes.txt", FileLinkSyntax::URI, "/srv", "build-box");
    REQUIRE(plain.has_value());
    CHECK_MSG(plain->line.line == 0, "a link that names no line keeps none");
    const auto refusal = [](const char* uri) {
        const auto target = resolveHostFileLink(uri, FileLinkSyntax::URI, "/srv", "build-box");
        return target ? std::optional<FileLinkError> {} : std::optional { target.error() };
    };
    CHECK_MSG(refusal("file:///srv/main.swift#run") == FileLinkError::Invalid, "a fragment that names no line");
    CHECK_MSG(refusal("file:///srv/main.swift?x#L1") == FileLinkError::Invalid, "a query before a line");
    CHECK_MSG(refusal("file://elsewhere/srv/main.swift#L1") == FileLinkError::ForeignHost, "a third machine's line link");
    const auto literal = resolveHostFileLink("logs/run#12.log", FileLinkSyntax::Path, "/srv", "build-box");
    REQUIRE(literal.has_value());
    CHECK(literal->path == "/srv/logs/run#12.log");
    CHECK(literal->line.line == 0);
}

// A Linux file name is bytes, and one written in Latin-1 is a file like any
// other, so the opener there reads a percent-encoded byte that is not UTF-8 as
// that byte. A macOS name, and a path sent to a host, stays UTF-8.
TEST(aFileNameThatIsNotUTF8IsReadAsBytesWhereNamesAreBytes)
{
    using namespace Ubiquitty::Session;
    CHECK(localFileLinkPath("file:///tmp/caf%E9.txt", "host", FilePathEncoding::Bytes) == "/tmp/caf\xE9.txt");
    CHECK(localFileLinkPath("file:///tmp/caf%e9.txt#12", "host", FilePathEncoding::Bytes) == "/tmp/caf\xE9.txt");
    CHECK(localFileLinkPath("file:///tmp/caf%C3%A9.txt", "host", FilePathEncoding::Bytes) == "/tmp/caf\xC3\xA9.txt");
    CHECK(localFileLinkPath("file:///tmp/caf%E9.txt", "host", FilePathEncoding::UTF8).empty());
    CHECK(!resolveFileLink("file:///tmp/caf%E9.txt", FileLinkSyntax::URI, {}, "host"));
    // Controls are refused whichever bytes a name may hold.
    for (const auto value : { "file:///tmp/a%00b", "file:///tmp/a%0Ab", "file:///tmp/a%1Fb", "file:///tmp/a%7Fb" })
        CHECK_MSG(localFileLinkPath(value, "host", FilePathEncoding::Bytes).empty(), value);
}

// The value a fetched file carries is the four semicolon-separated fields
// Safari and `curl --xattr` write: the web-download flags, the time as eight
// lower-case hex digits, the agent, and the caller's event id.
TEST(quarantineValueIsABrowserDownloadRecord)
{
    CHECK(quarantineAttributeName == "com.apple.quarantine");

    const std::string value = quarantineValue(0x68C1'2345ULL, "B7C0FE73-1C4C-4E5B-9E1C-2B2C4A2E7D11");
    CHECK(value == "0083;68c12345;Ubiquitty;B7C0FE73-1C4C-4E5B-9E1C-2B2C4A2E7D11");

    // Eight digits, zero-padded and never truncated: a short field is not the
    // shape LaunchServices parses, and a wide one would drop the high digits.
    CHECK(quarantineValue(0, "x") == "0083;00000000;Ubiquitty;x");
    CHECK(quarantineValue(0xFFFF'FFFFULL, "x") == "0083;ffffffff;Ubiquitty;x");
    CHECK(quarantineValue(0x1'0000'0000ULL, "x") == "0083;00000000;Ubiquitty;x");

    // The flags say "downloaded, not yet approved", which is what makes the
    // first open ask rather than silently launching.
    CHECK(value.rfind("0083;", 0) == 0);
}

// The suggested name is the other end's, so only its last component is used
// and a suggestion that names no file at all falls back to a fixed name.
TEST(safeDownloadNameCannotEscapeItsDirectory)
{
    CHECK(safeDownloadName("notes.txt") == "notes.txt");
    CHECK(safeDownloadName("/var/log/system.log") == "system.log");
    CHECK(safeDownloadName("../../../../etc/passwd") == "passwd");
    CHECK(safeDownloadName("a/../b/payload.sh") == "payload.sh");

    // Nothing to write under: a bare separator, a directory, or the two dot
    // names that would climb out of it.
    CHECK(safeDownloadName("") == "download");
    CHECK(safeDownloadName("/") == "download");
    CHECK(safeDownloadName("dir/") == "download");
    CHECK(safeDownloadName("..") == "download");
    CHECK(safeDownloadName("../") == "download");
    CHECK(safeDownloadName(".") == "download");
    CHECK(safeDownloadName("x/.") == "download");

    // A leading dot is a name, not a climb.
    CHECK(safeDownloadName(".bashrc") == ".bashrc");
    CHECK(safeDownloadName("/home/me/.bashrc") == ".bashrc");
}

// A clicked candidate names a path on the host: absolute and `~` paths are
// the host's own, a relative one joins the host's reported directory, and with
// no directory reported there is nothing to fetch.
TEST(hostAbsolutePathJoinsTheHostsOwnDirectory)
{
    CHECK(hostAbsolutePath("/etc/hosts", "/home/me") == "/etc/hosts");
    CHECK(hostAbsolutePath("/etc/hosts", "") == "/etc/hosts");

    // Left for the host's shell: this machine's home is not the host's.
    CHECK(hostAbsolutePath("~/notes.txt", "/home/me") == "~/notes.txt");
    CHECK(hostAbsolutePath("~/notes.txt", "") == "~/notes.txt");

    CHECK(hostAbsolutePath("notes.txt", "/home/me") == "/home/me/notes.txt");
    CHECK(hostAbsolutePath("build/log.txt", "/home/me") == "/home/me/build/log.txt");
    // One separator, whichever way the host reported its directory.
    CHECK(hostAbsolutePath("notes.txt", "/home/me/") == "/home/me/notes.txt");
    CHECK(hostAbsolutePath("notes.txt", "/") == "/notes.txt");

    // Nothing to join against, so nothing is asked for — the same silence a
    // local click on a path that resolves to no file answers with.
    CHECK(hostAbsolutePath("notes.txt", "").empty());
    CHECK(hostAbsolutePath("", "/home/me").empty());
}

UBIQUITTY_TEST_MAIN()
