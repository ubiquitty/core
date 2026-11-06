// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_linkrun — the portable hovered-link-run computation: OSC 8
// contiguous-id spans and regex URL spans, both returned as a column
// region with the resolved URI. The macOS underline paint reuses this
// verbatim; a future Linux frontend will too.

#include "Core/Attributes/LinkRun.h"
#include "Core/VT/Constants.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty;

// A fixed-width row of (codePoint, linkID) cells driving the probes.
struct Row {
    static constexpr int rowColumns = 64;
    uint32_t codePoint[rowColumns];
    uint16_t link[rowColumns];
    int columns;

    Row()
        : columns(0)
    {
        std::memset(codePoint, 0, sizeof(codePoint));
        std::memset(link, 0, sizeof(link));
    }

    // Lay ASCII text starting at column 0; cells default to space.
    void setText(const char* s)
    {
        int i = 0;
        for (; s[i] != 0 && i < rowColumns; ++i)
            codePoint[i] = static_cast<uint32_t>(s[i]);
        columns = (i > columns) ? i : columns;
    }

    void stampLink(int c0, int c1, uint16_t id)
    {
        for (int c = c0; c <= c1 && c < rowColumns; ++c)
            link[c] = id;
        if (c1 + 1 > columns)
            columns = c1 + 1;
    }
};

static uint32_t probeCodePoint(void* context, int column)
{
    Row* r = static_cast<Row*>(context);
    if (column < 0 || column >= Row::rowColumns)
        return ' ';
    return r->codePoint[column];
}

static uint16_t probeLink(void* context, int column)
{
    Row* r = static_cast<Row*>(context);
    if (column < 0 || column >= Row::rowColumns)
        return 0;
    return r->link[column];
}

// A tiny link table: ids 1..N → URI strings.
struct LinkTable {
    static constexpr int max = 16;
    const char* uri[max];
    LinkTable()
    {
        for (int i = 0; i < max; ++i)
            uri[i] = nullptr;
    }
};

static const char* resolveURI(void* context, uint16_t id)
{
    LinkTable* t = static_cast<LinkTable*>(context);
    if (id == 0 || id > LinkTable::max)
        return nullptr;
    return t->uri[id - 1];
}

TEST(testRegexSpanBasic)
{
    Row r;
    r.setText("see https://example.com/p now");
    //          0123456789...
    // "https://example.com/p" starts at column 4, length 21 → ends column 24.
    Attributes::LinkRun::Run run = Attributes::LinkRun::runAtColumn(r.columns, 10, probeCodePoint, probeLink, &r, resolveURI, nullptr);
    CHECK(run.valid);
    CHECK(run.column0 == 4);
    CHECK(run.column1 == 24);
    CHECK(std::strcmp(run.uri, "https://example.com/p") == 0);
}

TEST(testRegexHoverOutsideRunFails)
{
    Row r;
    r.setText("see https://example.com now");
    // Column 0 ("s" of "see") is not in the URL run.
    Attributes::LinkRun::Run run = Attributes::LinkRun::runAtColumn(r.columns, 0, probeCodePoint, probeLink, &r, resolveURI, nullptr);
    CHECK(!run.valid);
}

TEST(testRegexTrailingPunctTrimmed)
{
    Row r;
    r.setText("go http://a.b/x.");
    // The trailing '.' is trimmed; run is "http://a.b/x".
    Attributes::LinkRun::Run run = Attributes::LinkRun::runAtColumn(r.columns, 5, probeCodePoint, probeLink, &r, resolveURI, nullptr);
    CHECK(run.valid);
    CHECK(std::strcmp(run.uri, "http://a.b/x") == 0);
    // The trimmed '.' column is not part of the span.
    int dotColumn = static_cast<int>(std::strlen("go ")) + static_cast<int>(std::strlen("http://a.b/x"));
    CHECK(run.column1 == dotColumn - 1);
}

TEST(testBareWwwGetsHttpsPrefix)
{
    Row r;
    r.setText("www.foo.com");
    Attributes::LinkRun::Run run = Attributes::LinkRun::runAtColumn(r.columns, 2, probeCodePoint, probeLink, &r, resolveURI, nullptr);
    CHECK(run.valid);
    CHECK(std::strcmp(run.uri, "https://www.foo.com") == 0);
    // The span still covers the raw on-screen columns, not the prefix.
    CHECK(run.column0 == 0);
    CHECK(run.column1 == 10);
}

// A scheme with no host after it is not a link. `https://` and `www.` are
// prefixes, and text that stops there names nothing to open — a terminal that
// underlines them offers the reader a click that can only fail.
TEST(testASchemeWithNoHostIsNotALink)
{
    struct Case {
        const char* text;
        int hoverColumn;
    };
    // Each is the bare prefix, alone and inside a sentence, hovered on the
    // prefix itself.
    const Case cases[] = {
        { "https://", 3 },
        { "http://", 3 },
        { "www.", 1 },
        { "see https:// here", 8 },
        { "see www. here", 5 },
        // A host that is only punctuation names nothing either.
        { "https://.", 3 },
        { "www..", 1 },
    };
    for (const Case& oneCase : cases) {
        Row r;
        r.setText(oneCase.text);
        Attributes::LinkRun::Run run = Attributes::LinkRun::runAtColumn(r.columns, oneCase.hoverColumn, probeCodePoint, probeLink, &r, resolveURI, nullptr);
        CHECK_MSG(!run.valid, oneCase.text);
    }
}

// The shortest thing that is a link: one host character after the scheme. The
// case above must not be passing by rejecting everything short.
TEST(testOneHostCharacterAfterTheSchemeIsALink)
{
    Row r;
    r.setText("https://a");
    Attributes::LinkRun::Run run = Attributes::LinkRun::runAtColumn(r.columns, 3, probeCodePoint, probeLink, &r, resolveURI, nullptr);
    CHECK(run.valid);
    CHECK(std::strcmp(run.uri, "https://a") == 0);

    Row w;
    w.setText("www.a");
    run = Attributes::LinkRun::runAtColumn(w.columns, 1, probeCodePoint, probeLink, &w, resolveURI, nullptr);
    CHECK(run.valid);
    CHECK(std::strcmp(run.uri, "https://www.a") == 0);
}

TEST(testOSC8ContiguousSpan)
{
    Row r;
    r.setText("  Click here  ");
    // Stamp link id 3 across "Click here" (columns 2..11).
    r.stampLink(2, 11, 3);
    LinkTable t;
    t.uri[2] = "https://link.example/8";

    // Hover mid-run resolves the whole contiguous id span and its URI.
    Attributes::LinkRun::Run run = Attributes::LinkRun::runAtColumn(r.columns, 6, probeCodePoint, probeLink, &r, resolveURI, &t);
    CHECK(run.valid);
    CHECK(run.column0 == 2);
    CHECK(run.column1 == 11);
    CHECK(std::strcmp(run.uri, "https://link.example/8") == 0);
}

TEST(testOSC8SpanStopsAtIdChange)
{
    Row r;
    r.setText("ABCDEFGH");
    r.stampLink(0, 2, 5); // ids: 5 5 5
    r.stampLink(3, 7, 9); // ids:       9 9 9 9 9
    LinkTable t;
    t.uri[4] = "scheme://five";
    t.uri[8] = "scheme://nine";

    Attributes::LinkRun::Run a = Attributes::LinkRun::runAtColumn(r.columns, 1, probeCodePoint, probeLink, &r, resolveURI, &t);
    CHECK(a.valid);
    CHECK(a.column0 == 0 && a.column1 == 2);
    CHECK(std::strcmp(a.uri, "scheme://five") == 0);

    Attributes::LinkRun::Run b = Attributes::LinkRun::runAtColumn(r.columns, 5, probeCodePoint, probeLink, &r, resolveURI, &t);
    CHECK(b.valid);
    CHECK(b.column0 == 3 && b.column1 == 7);
    CHECK(std::strcmp(b.uri, "scheme://nine") == 0);
}

TEST(testOSC8UnknownIdFallsBackToRegex)
{
    Row r;
    r.setText("x https://q.example y");
    // Stamp a link id whose entry is absent from the table; the id path
    // must not claim the run — regex detection takes over.
    r.stampLink(2, 18, 7);
    LinkTable t; // id 7 → nullptr

    Attributes::LinkRun::Run run = Attributes::LinkRun::runAtColumn(r.columns, 5, probeCodePoint, probeLink, &r, resolveURI, &t);
    CHECK(run.valid);
    CHECK(std::strcmp(run.uri, "https://q.example") == 0);
}

TEST(testNoLinkNoMatch)
{
    Row r;
    r.setText("just plain words here");
    Attributes::LinkRun::Run run = Attributes::LinkRun::runAtColumn(r.columns, 4, probeCodePoint, probeLink, &r, resolveURI, nullptr);
    CHECK(!run.valid);
}

TEST(testColumnBounds)
{
    Row r;
    r.setText("https://a.b");
    // Out-of-range columns must be rejected without UB.
    Attributes::LinkRun::Run lo = Attributes::LinkRun::runAtColumn(r.columns, -1, probeCodePoint, probeLink, &r, resolveURI, nullptr);
    CHECK(!lo.valid);
    Attributes::LinkRun::Run hi = Attributes::LinkRun::runAtColumn(r.columns, r.columns, probeCodePoint, probeLink, &r, resolveURI, nullptr);
    CHECK(!hi.valid);
}

// A URL that soft-wraps across visual rows is detected by scanning the
// rows CONCATENATED into one flat logical line (what the macOS frontend
// builds from visibleRowWrapped). urlMatchSpanAtColumn must find the whole
// span — across the would-be row boundaries — and materialize the full URL
// without the per-row maxLinkURILength truncation. Mirrors the OAuth-link case.
TEST(testWrappedURLSpansConcatenatedRows)
{
    // 80-column rows; a 300-char URL spilling across 4 rows with no separators
    // (a soft wrap joins cells directly), preceded by a prompt on row 0.
    const int columns = 80;
    const int rows = (Ubiquitty::VT::maxLinkURILength + 256) / columns;
    const int n = columns * rows;
    std::vector<uint32_t> lineStorage(static_cast<size_t>(n));
    uint32_t* line = lineStorage.data();
    for (int i = 0; i < n; ++i)
        line[i] = ' ';

    std::string url = "https://example.com/oauth/authorize?";
    // Longer than maxLinkURILength on purpose: what this pins is that the
    // CALLER's buffer governs the answer, not the global constant, so the
    // fixture has to exceed the constant for the distinction to exist. It grew
    // when the constant did.
    while (url.size() < static_cast<size_t>(Ubiquitty::VT::maxLinkURILength) + 64)
        url += "abcdEFGH0123-_"; // long query
    const int start = 5; // after a short prompt
    for (size_t k = 0; k < url.size(); ++k)
        line[start + (int)k] = static_cast<uint32_t>(url[k]);
    const int end = start + (int)url.size() - 1;
    CHECK(url.size() > Ubiquitty::VT::maxLinkURILength); // must exceed per-row cap
    CHECK(end >= columns); // genuinely wraps

    // Hover a column on the THIRD row (well past the first wrap boundary).
    char outputBuffer[Ubiquitty::VT::maxLinkURILength + 128];
    const auto span = Attributes::LinkRun::urlMatchSpanAtColumn(line, n, columns * 2 + 10, outputBuffer);
    CHECK(span.has_value());
    CHECK(span && span->first == start);
    CHECK(span && span->last == end);
    CHECK(std::strcmp(outputBuffer, url.c_str()) == 0); // full URL, not truncated
}

// Build a flat codePoint row from an ASCII string (space-padded to n).
static void layRow(const char* s, uint32_t* row, int n)
{
    int i = 0;
    for (; s[i] && i < n; ++i)
        row[i] = (uint32_t)s[i];
    for (; i < n; ++i)
        row[i] = ' ';
}

// pathMatchSpanAtColumn — TEXTUAL path-candidate detection (the frontend's
// filesystem-existence check is the real gate; this only excludes obvious
// non-paths). Spans cover the path proper, stopping at ':' / whitespace.
TEST(testPathSpans)
{
    const int N = 80;
    uint32_t row[N];
    char outputBuffer[256];

    // Absolute path; hover mid-run.
    layRow("  /Users/user/Source/VT.cpp:42:10  ", row, N);
    const auto absolute = Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 12, outputBuffer);
    CHECK(absolute.has_value());
    CHECK(std::strcmp(outputBuffer, "/Users/user/Source/VT.cpp") == 0); // ':' bounds it
    CHECK(absolute && absolute->first == 2);
    CHECK(absolute && absolute->last == 2 + (int)std::strlen("/Users/user/Source/VT.cpp") - 1);

    // Home-relative (leading '~', no slash needed beyond it).
    layRow("edit ~/notes.md please", row, N);
    CHECK(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 6, outputBuffer));
    CHECK(std::strcmp(outputBuffer, "~/notes.md") == 0);

    // Relative with slash.
    layRow("open ./src/main.cpp now", row, N);
    CHECK(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 8, outputBuffer));
    CHECK(std::strcmp(outputBuffer, "./src/main.cpp") == 0);

    // Trailing sentence period trimmed.
    layRow("see /tmp/x.log.", row, N);
    CHECK(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 6, outputBuffer));
    CHECK(std::strcmp(outputBuffer, "/tmp/x.log") == 0);

    // Slash-less bare word is not a candidate.
    layRow("README and stuff", row, N);
    CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 2, outputBuffer));

    // Bare dotted version is not a candidate (no slash, no leading ~).
    layRow("v1.2.3 shipped", row, N);
    CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 2, outputBuffer));

    // Hover on whitespace: no candidate.
    layRow("  /tmp/x  ", row, N);
    CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 0, outputBuffer));

    // "n/a" does pass the textual gate (slash + alphanumerics) — documents that the
    // frontend's existence check, not this matcher, rejects prose like this.
    layRow("n/a", row, N);
    CHECK(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 0, outputBuffer));
}

// isLaunchableURLScheme — the launch-scheme allowlist gating an OS open. Safe
// network / mail / file schemes pass (macOS + Linux share this one list);
// dangerous schemes (javascript, data) and schemeless / relative refs reject.
TEST(testLaunchableScheme)
{
    CHECK(Attributes::LinkRun::isLaunchableURLScheme("https://example.com"));
    CHECK(Attributes::LinkRun::isLaunchableURLScheme("http://example.com"));
    CHECK(Attributes::LinkRun::isLaunchableURLScheme("mailto:a@b.c"));
    CHECK(Attributes::LinkRun::isLaunchableURLScheme("ftp://host/file"));
    CHECK(Attributes::LinkRun::isLaunchableURLScheme("ftps://host/file"));
    CHECK(Attributes::LinkRun::isLaunchableURLScheme("ssh://host"));
    CHECK(Attributes::LinkRun::isLaunchableURLScheme("sftp://host"));
    CHECK(Attributes::LinkRun::isLaunchableURLScheme("HTTPS://x")); // scheme is case-insensitive

    CHECK(Attributes::LinkRun::isLaunchableURLScheme("file:///etc/passwd")); // file:// launches (macOS + Linux parity)
    CHECK(!Attributes::LinkRun::isLaunchableURLScheme("javascript:alert(1)"));
    CHECK(!Attributes::LinkRun::isLaunchableURLScheme("data:text/html,<x>"));
    CHECK(!Attributes::LinkRun::isLaunchableURLScheme("/etc/passwd")); // schemeless / relative
    CHECK(!Attributes::LinkRun::isLaunchableURLScheme("example.com")); // no scheme
    CHECK(!Attributes::LinkRun::isLaunchableURLScheme(":nohost")); // empty scheme
    CHECK(!Attributes::LinkRun::isLaunchableURLScheme("")); // empty
}

// requiresLinkModifier — the schemes a plain click leaves alone even when the
// program declared them, because each hands a connection to a host it named.
TEST(testAConnectionNeedsTheLinkModifier)
{
    using Ubiquitty::Attributes::LinkRun::requiresLinkModifier;
    for (const char* connecting : { "ssh://host", "sftp://host/file", "ftp://host/file", "ftps://host/file", "SSH://host", "Ftp://host" })
        CHECK_MSG(requiresLinkModifier(connecting), connecting);
    for (const char* plain : { "https://example.com", "http://example.com", "mailto:a@b.c", "file:///tmp/notes.txt",
             "ubiquitty:action/connect", "jira:ABC-1", "sshx://host", "ssh", ":ssh", "" })
        CHECK_MSG(!requiresLinkModifier(plain), plain);
}

// kindForURI — scheme → link category, so a frontend paints url/file/mailto
// distinctly. mailto: → Mailto, file: → File, every other absolute scheme →
// URL, schemeless / empty → Other. Case-insensitive.
TEST(testKindForURI)
{
    CHECK(Attributes::LinkRun::kindForURI("https://example.com") == Attributes::LinkRun::Kind::URL);
    CHECK(Attributes::LinkRun::kindForURI("http://example.com") == Attributes::LinkRun::Kind::URL);
    CHECK(Attributes::LinkRun::kindForURI("ftp://host/file") == Attributes::LinkRun::Kind::URL);
    CHECK(Attributes::LinkRun::kindForURI("ssh://host") == Attributes::LinkRun::Kind::URL);
    CHECK(Attributes::LinkRun::kindForURI("jira://ABC-1") == Attributes::LinkRun::Kind::URL); // unknown scheme still a URL
    CHECK(Attributes::LinkRun::kindForURI("file:///etc/hosts") == Attributes::LinkRun::Kind::File);
    CHECK(Attributes::LinkRun::kindForURI("FILE:///etc/hosts") == Attributes::LinkRun::Kind::File); // case-insensitive
    CHECK(Attributes::LinkRun::kindForURI("mailto:a@b.c") == Attributes::LinkRun::Kind::Mailto);
    CHECK(Attributes::LinkRun::kindForURI("MailTo:a@b.c") == Attributes::LinkRun::Kind::Mailto); // case-insensitive
    CHECK(Attributes::LinkRun::kindForURI("ubiquitty:action/connect") == Attributes::LinkRun::Kind::Action); // the app's own control
    CHECK(Attributes::LinkRun::kindForURI("UBIQUITTY:action/connect") == Attributes::LinkRun::Kind::Action);
    CHECK(Attributes::LinkRun::kindForURI("ubiquittyx:action") == Attributes::LinkRun::Kind::URL); // another scheme, not a prefix match
    CHECK(Attributes::LinkRun::kindForURI("example.com") == Attributes::LinkRun::Kind::Other); // no scheme
    CHECK(Attributes::LinkRun::kindForURI(":nohost") == Attributes::LinkRun::Kind::Other); // empty scheme
    CHECK(Attributes::LinkRun::kindForURI("") == Attributes::LinkRun::Kind::Other); // empty

    // The Run carries the classified kind so the frontend doesn't re-parse.
    Row r;
    r.setText("see https://x.example here");
    Attributes::LinkRun::Run url = Attributes::LinkRun::runAtColumn(r.columns, 6, probeCodePoint, probeLink, &r, resolveURI, nullptr);
    CHECK(url.valid && url.kind == Attributes::LinkRun::Kind::URL);
    Row m;
    m.setText("mail mailto:a@b.c now");
    Attributes::LinkRun::Run mail = Attributes::LinkRun::runAtColumn(m.columns, 6, probeCodePoint, probeLink, &m, resolveURI, nullptr);
    CHECK(mail.valid && mail.kind == Attributes::LinkRun::Kind::Mailto);
}

// isSafeToAutoOpenFileExtension — the gate between a file link the user clicked
// and the OS default handler opening it without asking. The rule it states is
// that only types a handler DISPLAYS may pass; anything a handler would RUN must
// not. Nothing had exercised it, so both halves are asserted here: that every
// listed type passes, and — the half that matters — that the executable types
// the comment names as excluded are actually excluded.
TEST(testOnlyDisplayableFileTypesAutoOpen)
{
    using Ubiquitty::Attributes::LinkRun::isSafeToAutoOpenFileExtension;

    // Every extension the list carries, so an entry deleted by an editing
    // accident stops being covered by a case that never named it.
    for (const char* safe : { "notes.txt", "readme.md", "readme.markdown", "server.log",
             "package.json", "config.yaml", "config.yml", "Cargo.toml", "setup.ini",
             "app.cfg", "nginx.conf", "rows.csv", "rows.tsv", "main.c",
             "main.h", "main.cc", "main.cpp", "main.cxx", "main.hpp", "main.hxx",
             "View.m", "View.mm", "View.swift", "main.rs", "main.go", "Main.java",
             "site.css", "fix.diff", "fix.patch", "build.gitignore", "local.env",
             "shot.png", "photo.jpg", "photo.jpeg", "clip.gif", "photo.heic", "photo.webp",
             "scan.tif", "scan.tiff", "icon.bmp", "photo.avif", "favicon.ico", "report.pdf",
             "clip.mp4", "clip.mov" }) {
        CHECK_MSG(isSafeToAutoOpenFileExtension(safe), safe);
    }

    // The whole names a repository holds with no extension to read.
    for (const char* safe : { "README", "LICENSE", "LICENCE", "COPYING", "NOTICE", "AUTHORS", "CHANGELOG",
             "CONTRIBUTING", "Makefile", "GNUmakefile", "Dockerfile", "Containerfile", ".gitignore",
             ".gitattributes", ".editorconfig", ".env" }) {
        CHECK_MSG(isSafeToAutoOpenFileExtension(std::string("/src/project/") + safe), safe);
    }

    // The refusals. Each of these is a type whose default handler executes
    // rather than displays, which is the whole reason the gate exists: a link
    // the terminal renders is content a remote program chose.
    for (const char* unsafe : { "run.sh", "run.bash", "run.zsh", "run.py", "run.rb",
             "run.pl", "run.js", "run.mjs", "open.command", "Thing.app", "link.webloc",
             "page.html", "page.htm", "disk.dmg", "install.pkg", "bundle.zip",
             "bundle.tar", "bundle.gz", "script.scpt", "script.applescript",
             "session.terminal", "flow.workflow", "shortcut.url", "entry.desktop",
             "lib.so", "lib.dylib", "prog.exe", "prog.bat", "prog.ps1", "image.svg", "pom.xml", "feed.xsl" }) {
        CHECK_MSG(!isSafeToAutoOpenFileExtension(unsafe), unsafe);
    }

    // Audio runs nothing, but a Mac opens it in Music, which imports what it
    // opens into the reader's library.
    for (const char* audio : { "song.mp3", "voice.m4a", "sound.wav", "SONG.MP3" }) {
        CHECK_MSG(!isSafeToAutoOpenFileExtension(audio), audio);
    }
}

TEST(testTheAutoOpenGateReadsTheNameRatherThanThePath)
{
    using Ubiquitty::Attributes::LinkRun::isSafeToAutoOpenFileExtension;

    // A directory carrying a dot must not supply the extension: the run's last
    // component decides, and here it has none.
    CHECK(!isSafeToAutoOpenFileExtension("/home/user/notes.txt/run"));
    CHECK(isSafeToAutoOpenFileExtension("/home/user/notes.txt"));
    CHECK(isSafeToAutoOpenFileExtension("relative/path/notes.txt"));

    // Nothing to read an extension from, and no whole name on the list.
    CHECK(!isSafeToAutoOpenFileExtension(""));
    CHECK(!isSafeToAutoOpenFileExtension("/usr/bin/env"));
    CHECK(!isSafeToAutoOpenFileExtension("notes."));
    CHECK(!isSafeToAutoOpenFileExtension("/a/b/"));

    // A whole name matches in full and folds case, and is the name rather than
    // a directory the path passes through.
    CHECK(isSafeToAutoOpenFileExtension("README"));
    CHECK(isSafeToAutoOpenFileExtension("/src/readme"));
    CHECK(!isSafeToAutoOpenFileExtension("/src/README/run"));
    CHECK(!isSafeToAutoOpenFileExtension("/src/READMEs"));
    CHECK(!isSafeToAutoOpenFileExtension("/src/README.sh"));
    CHECK(isSafeToAutoOpenFileExtension("/src/README.md"));

    // A dotfile has no extension in this sense, so the leading dot is not one,
    // and only a dotfile on the list by its whole name passes.
    CHECK(!isSafeToAutoOpenFileExtension(".bashrc"));
    CHECK(!isSafeToAutoOpenFileExtension("/home/user/.envrc"));
    CHECK(!isSafeToAutoOpenFileExtension("/home/user/.env.sh"));
    CHECK(isSafeToAutoOpenFileExtension(".gitignore"));
    CHECK(isSafeToAutoOpenFileExtension("/home/user/.env"));
    CHECK(isSafeToAutoOpenFileExtension("/home/user/.gitignore"));
    CHECK(isSafeToAutoOpenFileExtension("build.gitignore"));

    // The comparison folds case, so a capitalised name is the same decision —
    // and so is a capitalised refusal, which is the direction that matters.
    CHECK(isSafeToAutoOpenFileExtension("README.MD"));
    CHECK(isSafeToAutoOpenFileExtension("Notes.TxT"));
    CHECK(!isSafeToAutoOpenFileExtension("RUN.SH"));
    CHECK(!isSafeToAutoOpenFileExtension("Thing.APP"));

    // Length is compared before the bytes, so a near-miss must not pass on a
    // prefix. Both directions: longer than an entry and shorter than one.
    CHECK(!isSafeToAutoOpenFileExtension("notes.txtx"));
    CHECK(!isSafeToAutoOpenFileExtension("notes.tx"));
    CHECK(!isSafeToAutoOpenFileExtension("notes.mdd"));
    CHECK(!isSafeToAutoOpenFileExtension("notes.jso"));

    // The extension is the run after the LAST dot, so an archive named to look
    // like a text file is still an archive.
    CHECK(!isSafeToAutoOpenFileExtension("notes.txt.sh"));
    CHECK(isSafeToAutoOpenFileExtension("archive.tar.md"));
}

// hasDeceptiveLinkCharacters — a target a reader cannot read as written never
// opens. Every range in the set is probed at both ends, and the characters just
// outside each range are probed too, so a widened range is caught as surely as
// a narrowed one.
TEST(testATargetThatHidesPartOfItselfIsRefused)
{
    using Ubiquitty::Attributes::LinkRun::hasDeceptiveLinkCharacters;

    CHECK(!hasDeceptiveLinkCharacters("https://example.com/a?b=c#d"));
    CHECK(!hasDeceptiveLinkCharacters("file:///Users/me/caf\xC3\xA9/\xE6\x97\xA5\xE6\x9C\xAC.txt"));
    CHECK_MSG(!hasDeceptiveLinkCharacters(""), "an empty target hides nothing; the opener refuses it on its own");

    struct Probe {
        const char* bytes;
        const char* what;
    };
    for (const Probe deceptive : {
             Probe { "\x01", "U+0001" }, Probe { "\x1F", "U+001F" }, Probe { "\n", "a line feed" }, Probe { "\x7F", "DEL" },
             Probe { "\xC2\x80", "U+0080" }, Probe { "\xC2\x85", "NEL" }, Probe { "\xC2\x9F", "U+009F" },
             Probe { "\xD8\x9C", "U+061C" }, Probe { "\xE2\x80\x8B", "U+200B" }, Probe { "\xE2\x80\x8E", "U+200E" },
             Probe { "\xE2\x80\x8F", "U+200F" },
             Probe { "\xE2\x80\xAA", "U+202A" }, Probe { "\xE2\x80\xAE", "U+202E" }, Probe { "\xE2\x81\xA6", "U+2066" },
             Probe { "\xE2\x81\xA9", "U+2069" }, Probe { "\xE2\x80\xA8", "U+2028" }, Probe { "\xE2\x80\xA9", "U+2029" },
             Probe { "\xE2\x81\xA0", "U+2060" }, Probe { "\xEF\xBB\xBF", "U+FEFF" },
             Probe { "\xFF", "a byte no UTF-8 sequence starts with" }, Probe { "\xC0\xAF", "an overlong slash" },
             Probe { "\xED\xA0\x80", "a surrogate" }, Probe { "\xE2\x80", "a truncated sequence" } }) {
        const std::string uri = std::string("file:///System/Applications/") + deceptive.bytes + "Calculator.app";
        CHECK_MSG(hasDeceptiveLinkCharacters(uri), deceptive.what);
    }
    for (const Probe ordinary : {
             Probe { " ", "a space" }, Probe { "~", "U+007E" }, Probe { "\xC2\xA0", "U+00A0" },
             Probe { "\xD8\x9B", "U+061B" }, Probe { "\xE2\x80\x8A", "U+200A" }, Probe { "\xE2\x80\x8C", "U+200C" },
             Probe { "\xE2\x80\x8D", "U+200D" }, Probe { "\xE2\x80\x90", "U+2010" },
             Probe { "\xE2\x80\xA7", "U+2027" }, Probe { "\xE2\x80\xAF", "U+202F" }, Probe { "\xE2\x81\x9F", "U+205F" },
             Probe { "\xE2\x81\xA1", "U+2061" }, Probe { "\xE2\x81\xAA", "U+206A" }, Probe { "\xEF\xBB\xBE", "U+FEFE" },
             Probe { "\xF0\x9F\x98\x80", "an emoji" } }) {
        const std::string uri = std::string("https://example.com/") + ordinary.bytes;
        CHECK_MSG(!hasDeceptiveLinkCharacters(uri), ordinary.what);
    }
}

// The joiners spell names rather than hide anything, so a target written with
// them, raw as a program prints it into OSC 8, opens, and the plate shows it as
// written.
TEST(testATargetSpelledWithJoinersReadsAsWritten)
{
    using Ubiquitty::Attributes::LinkRun::escapedDeceptiveLinkCharacters;
    using Ubiquitty::Attributes::LinkRun::hasDeceptiveLinkCharacters;

    struct Probe {
        const char* uri;
        const char* what;
    };
    for (const Probe joined : {
             // U+1F468 U+200D U+1F4BB, one emoji.
             Probe { "file:///Users/me/\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB notes.txt", "an emoji ZWJ file name" },
             // U+0DC1 U+0DCA U+200D U+0DBB U+0DD3, the Sinhala word for Sri.
             Probe { "file:///srv/\xE0\xB7\x81\xE0\xB7\x8A\xE2\x80\x8D\xE0\xB6\xBB\xE0\xB7\x93.txt", "a Sinhala file name" },
             Probe { "https://si.wikipedia.org/wiki/\xE0\xB7\x81\xE0\xB7\x8A\xE2\x80\x8D\xE0\xB6\xBB\xE0\xB7\x93", "a Sinhala page" },
             // U+0645 U+06CC U+200C U+062E U+0648 U+0627 U+0647 U+0645, a Persian word.
             Probe { "https://fa.wikipedia.org/wiki/\xD9\x85\xDB\x8C\xE2\x80\x8C\xD8\xAE\xD9\x88\xD8\xA7\xD9\x87\xD9\x85", "a Persian page" },
             Probe { "file:///Users/me/\xD9\x85\xDB\x8C\xE2\x80\x8C\xD8\xAE\xD9\x88\xD8\xA7\xD9\x87\xD9\x85.md", "a Persian file name" } }) {
        CHECK_MSG(!hasDeceptiveLinkCharacters(joined.uri), joined.what);
        CHECK_MSG(escapedDeceptiveLinkCharacters(joined.uri) == joined.uri, joined.what);
    }
}

// escapedDeceptiveLinkCharacters — what the hover plate shows: every character
// the refusal names, spelled out where it stood, and nothing else changed.
TEST(testTheShownTargetSpellsOutWhatItHides)
{
    using Ubiquitty::Attributes::LinkRun::escapedDeceptiveLinkCharacters;

    CHECK(escapedDeceptiveLinkCharacters("/Users/me/caf\xC3\xA9.txt") == "/Users/me/caf\xC3\xA9.txt");
    CHECK(escapedDeceptiveLinkCharacters("/tmp/\xE2\x80\xAEtxt.app") == "/tmp/\\u{202E}txt.app");
    CHECK(escapedDeceptiveLinkCharacters("a\nb\x1F") == "a\\u{0A}b\\u{1F}");
    CHECK(escapedDeceptiveLinkCharacters("\xEF\xBB\xBFx") == "\\u{FEFF}x");
    CHECK_MSG(escapedDeceptiveLinkCharacters("a\xFF\xE2\x80z") == "a\\x{FF}\\x{E2}\\x{80}z",
        "each byte of an ill-formed run is spelled out, and the text after it is kept");
    CHECK_MSG(escapedDeceptiveLinkCharacters("\xE2\x80\xE2\x80\x8B") == "\\x{E2}\\x{80}\\u{200B}",
        "a lead byte that breaks a truncated sequence still starts its own");
    CHECK(escapedDeceptiveLinkCharacters("").empty());

    // Characters that draw as nothing are spelled out too, though a click does
    // not refuse them: a soft hyphen, an invisible plus, a Hangul filler, a
    // tag letter, a musical formatting control and an interlinear annotation.
    CHECK(escapedDeceptiveLinkCharacters("notes\xC2\xAD.txt") == "notes\\u{AD}.txt");
    CHECK(escapedDeceptiveLinkCharacters("a\xE2\x81\xA4z") == "a\\u{2064}z");
    CHECK(escapedDeceptiveLinkCharacters("a\xE3\x85\xA4z") == "a\\u{3164}z");
    CHECK(escapedDeceptiveLinkCharacters("a\xF3\xA0\x81\x81z") == "a\\u{E0041}z");
    CHECK(escapedDeceptiveLinkCharacters("a\xF0\x9D\x85\xB3z") == "a\\u{1D173}z");
    CHECK(escapedDeceptiveLinkCharacters("a\xEF\xBF\xB9z") == "a\\u{FFF9}z");
}

// A joiner or a variation selector between characters it cannot act on joins
// nothing, and draws as nothing: notes<U+200D>.txt is not notes.txt, and the
// plate must not show it as that file. Spelled out there, and kept beside an
// emoji or a letter of a script written with them, which it spells.
TEST(testAJoinerThatJoinsNothingIsSpelledOut)
{
    using Ubiquitty::Attributes::LinkRun::escapedDeceptiveLinkCharacters;

    struct Probe {
        const char* bytes;
        const char* escape;
    };
    for (const Probe selector : { Probe { "\xE2\x80\x8C", "\\u{200C}" }, Probe { "\xE2\x80\x8D", "\\u{200D}" },
             Probe { "\xEF\xB8\x8F", "\\u{FE0F}" }, Probe { "\xF3\xA0\x84\x80", "\\u{E0100}" }, Probe { "\xE1\xA0\x8B", "\\u{180B}" } }) {
        CHECK_MSG(escapedDeceptiveLinkCharacters(std::string("a") + selector.bytes + "b") == std::string("a") + selector.escape + "b", selector.escape);
    }
    CHECK_MSG(escapedDeceptiveLinkCharacters("/Users/me/notes\xE2\x80\x8D.txt") == "/Users/me/notes\\u{200D}.txt", "a file path");
    CHECK_MSG(escapedDeceptiveLinkCharacters("https://github.com/org/re\xE2\x80\x8Dpo") == "https://github.com/org/re\\u{200D}po",
        "a web address");
    CHECK_MSG(escapedDeceptiveLinkCharacters("\xE2\x80\x8Dnotes.txt\xEF\xB8\x8F") == "\\u{200D}notes.txt\\u{FE0F}",
        "at either end, where there is nothing to join");
    CHECK_MSG(escapedDeceptiveLinkCharacters(std::string("a\xEF\xB8\x8F\xE2\x80\x8D") + "b") == "a\\u{FE0F}\\u{200D}b",
        "two side by side cannot vouch for each other");
    CHECK_MSG(escapedDeceptiveLinkCharacters(std::string("a\xE2\x80\x8B\xE2\x80\x8D") + "b") == "a\\u{200B}\\u{200D}b",
        "nor can a character the plate already spells out");
    // U+00E9 U+200D U+00E9, and U+0430 U+200C U+0431: Latin and Cyrillic
    // letters are written without joiners.
    CHECK(escapedDeceptiveLinkCharacters("caf\xC3\xA9\xE2\x80\x8D\xC3\xA9") == "caf\xC3\xA9\\u{200D}\xC3\xA9");
    CHECK(escapedDeceptiveLinkCharacters("\xD0\xB0\xE2\x80\x8C\xD0\xB1") == "\xD0\xB0\\u{200C}\xD0\xB1");

    for (const char* kept : {
             // U+0031 U+FE0F U+20E3, the keycap 1: a digit, but the keycap after it.
             "/tmp/1\xEF\xB8\x8F\xE2\x83\xA3.txt",
             // U+00A9 U+FE0F and U+2764 U+FE0F, pictographs in emoji form.
             "/tmp/\xC2\xA9\xEF\xB8\x8F.txt", "/tmp/\xE2\x9D\xA4\xEF\xB8\x8F.txt",
             // U+845B U+E0100, an ideograph and its variation.
             "/tmp/\xE8\x91\x9B\xF3\xA0\x84\x80.txt",
             // U+1820 U+180B, a Mongolian letter and its free variation.
             "/tmp/\xE1\xA0\xA0\xE1\xA0\x8B.txt",
             // U+0915 U+094D U+200D U+0937, a Devanagari half form.
             "/tmp/\xE0\xA4\x95\xE0\xA5\x8D\xE2\x80\x8D\xE0\xA4\xB7.txt",
             // U+1F3F3 U+FE0F U+200D U+1F308, the rainbow flag, before ASCII.
             "/tmp/\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88.txt" }) {
        CHECK_MSG(escapedDeceptiveLinkCharacters(kept) == kept, kept);
    }
}

// localLinkAction — what a click may do with the local file a link names. The
// target an escape sequence chose is handed to the opener only when the opener
// can merely display it; an application or anything that runs is revealed,
// and a path where nothing ordinary exists is refused.
TEST(testALocalLinkOpensOnlyWhatCannotRun)
{
    using Ubiquitty::Attributes::LinkRun::LocalFileFacts;
    using Ubiquitty::Attributes::LinkRun::LocalLinkAction;
    using Ubiquitty::Attributes::LinkRun::localLinkAction;

    const LocalFileFacts directory { .isDirectory = true, .isExecutable = true };
    const LocalFileFacts bundle { .isDirectory = true, .isPackage = true, .isExecutable = true, .isLaunchableType = true };
    const LocalFileFacts document { .isRegularFile = true };
    const LocalFileFacts executable { .isRegularFile = true, .isExecutable = true };
    const LocalFileFacts script { .isRegularFile = true, .isLaunchableType = true };
    const LocalFileFacts nothing {};

    CHECK_MSG(localLinkAction("/Users/me/Projects", directory) == LocalLinkAction::Open,
        "a directory opens in the file manager, whose search permission is not an execute bit");
    CHECK_MSG(localLinkAction("/System/Applications/Calculator.app", bundle) == LocalLinkAction::Reveal,
        "an application is shown, never launched");
    CHECK_MSG(localLinkAction("/Users/me/Archive.bundle", { .isDirectory = true, .isPackage = true }) == LocalLinkAction::Reveal,
        "any package is one document the opener hands to its owner");
    CHECK_MSG(localLinkAction("/Users/me/Things.app", { .isDirectory = true, .isLaunchableType = true }) == LocalLinkAction::Reveal,
        "a directory whose type is an application is not a plain directory");
    CHECK_MSG(localLinkAction("/tmp/inbox.mbox", { .isDirectory = true, .isOpenedOutsideFileManager = true }) == LocalLinkAction::Reveal,
        "a folder the opener hands to Mail, not to the file manager, is shown");

    CHECK(localLinkAction("/Users/me/notes.txt", document) == LocalLinkAction::Open);
    CHECK(localLinkAction("/Users/me/README.md", document) == LocalLinkAction::Open);
    CHECK_MSG(localLinkAction("/Users/me/notes.txt", executable) == LocalLinkAction::Reveal,
        "an execute bit outranks a name that reads as text");
    CHECK_MSG(localLinkAction("/Users/me/notes.txt", script) == LocalLinkAction::Reveal,
        "so does a type the system reads as a script");
    CHECK_MSG(localLinkAction("/Users/me/Desktop/run.command", document) == LocalLinkAction::Reveal,
        "a .command runs in a terminal even with no execute bit to show for it");
    CHECK_MSG(localLinkAction("/usr/bin/env", executable) == LocalLinkAction::Reveal,
        "an executable with no extension at all");
    CHECK_MSG(localLinkAction("/Users/me/Downloads/page.html", document) == LocalLinkAction::Reveal,
        "a name off the display-only list is shown rather than guessed at");
    CHECK_MSG(localLinkAction("/Users/me/Downloads/report.pdf", document) == LocalLinkAction::Open,
        "a document the handler only displays opens");

    // The execute bit counts only where it can mean something. A display-only
    // type goes to a viewer whatever the bit says, and a volume that keeps no
    // permissions reports every file as executable; a program on either still
    // has its launchable type.
    CHECK_MSG(localLinkAction("/Users/me/run.txt", { .isRegularFile = true, .isExecutable = true, .isDisplayOnlyType = true })
            == LocalLinkAction::Open,
        "a stray execute bit on text does not make it reveal-only");
    CHECK_MSG(localLinkAction("/Volumes/CARD/DCIM/photo.jpg",
                  { .isRegularFile = true, .isExecutable = true, .isOnVolumeWithoutPermissions = true })
            == LocalLinkAction::Open,
        "a photo on an SD card, where every file reads as executable, opens");
    CHECK_MSG(localLinkAction("/Volumes/CARD/run",
                  { .isRegularFile = true, .isExecutable = true, .isLaunchableType = true, .isOnVolumeWithoutPermissions = true })
            == LocalLinkAction::Reveal,
        "a program on such a volume is still shown, by its type");
    CHECK_MSG(localLinkAction("/Users/me/notes.txt",
                  { .isRegularFile = true, .isExecutable = true, .isLaunchableType = true, .isDisplayOnlyType = true })
            == LocalLinkAction::Reveal,
        "a script is text to the type system too, and its launchable type still reveals it");
    CHECK_MSG(localLinkAction("/Users/me/page.html", { .isRegularFile = true, .isDisplayOnlyType = true }) == LocalLinkAction::Reveal,
        "the name rule still applies to a display-only type");
    CHECK_MSG(localLinkAction("/Users/me/project/Makefile", document) == LocalLinkAction::Open, "a whole name on the list opens");

    // What a path resolution cannot see: a file that sends the opener to
    // another file or program. Each is otherwise a document that would open.
    CHECK_MSG(localLinkAction("/Users/me/notes.txt", { .isRegularFile = true, .isAlias = true }) == LocalLinkAction::Reveal,
        "a Finder alias opens its target, which nothing classified");
    CHECK_MSG(localLinkAction("/Users/me/notes.txt", { .isRegularFile = true, .hasOwnOpener = true }) == LocalLinkAction::Reveal,
        "a file that names its own opener can name an interpreter");
    CHECK_MSG(localLinkAction("/Users/me/Projects", { .isDirectory = true, .hasOwnOpener = true }) == LocalLinkAction::Reveal,
        "so can a directory");
    CHECK_MSG(localLinkAction("/Users/me/notes.txt", { .isAlias = true }) == LocalLinkAction::Refuse,
        "a dangling link the platform calls an alias is still nothing");

    CHECK_MSG(localLinkAction("/Users/me/missing.txt", nothing) == LocalLinkAction::Refuse, "nothing exists there");
    CHECK_MSG(localLinkAction("/dev/null", nothing) == LocalLinkAction::Refuse, "a device is neither a file nor a directory");
    CHECK_MSG(localLinkAction("/tmp/socket.txt", { .isExecutable = true }) == LocalLinkAction::Refuse,
        "nor is a socket, whatever its name and permissions");
}

// Every scheme the detector claims to know, driven through the detector rather
// than read off the comment: a shell printing an ftp:// or ssh:// URL, or a
// mailto: address, is ordinary output.
TEST(testEverySchemeTheDetectorClaimsIsDetected)
{
    const int N = 80;
    uint32_t row[N];
    char outputBuffer[256];

    for (const char* url : { "https://example.com/a", "http://example.com/a",
             "file:///etc/hosts", "ftp://host/file", "ssh://host/path",
             "mailto:someone@example.com", "www.example.com" }) {
        std::string text = "see ";
        text += url;
        text += " ok";
        layRow(text.c_str(), row, N);
        const int hover = 4 + static_cast<int>(std::strlen(url)) / 2;
        const auto span = Attributes::LinkRun::urlMatchSpanAtColumn(row, N, hover,
            std::span<char>(outputBuffer, sizeof(outputBuffer)));
        CHECK_MSG(span.has_value(), url);
        CHECK_MSG(span && span->first == 4, url);
        CHECK_MSG(span && span->last == 4 + static_cast<int>(std::strlen(url)) - 1, url);
    }

    // A scheme-shaped run that is not one of the seven is not a URL, so the
    // agreement above is about the list rather than about anything with a colon
    // matching.
    layRow("see gopher://host/x ok", row, N);
    CHECK(!Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 10,
        std::span<char>(outputBuffer, sizeof(outputBuffer))));
}

// Where a URL stops. Each of these characters ends the run, and a URL that
// swallowed one would hand the opener bytes the surrounding text owns — a quote
// or a backslash reaching a shell-adjacent handler is the reason the set is
// this wide.
TEST(testAURLRunStopsAtEveryByteItExcludes)
{
    const int N = 80;
    uint32_t row[N];
    char outputBuffer[256];

    for (const char* stop : { "\"", "'", "<", ">", "`", "{", "}", "|", "\\", "^", " " }) {
        std::string text = "https://example.com/a";
        text += stop;
        text += "tail";
        layRow(text.c_str(), row, N);
        const auto span = Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 8,
            std::span<char>(outputBuffer, sizeof(outputBuffer)));
        CHECK_MSG(span.has_value(), stop);
        CHECK_MSG(std::strcmp(outputBuffer, "https://example.com/a") == 0, stop);
        CHECK_MSG(span && span->last == 20, stop);
    }

    // A byte outside printable ASCII ends it too, which is the guard before the
    // switch rather than one of its labels.
    layRow("https://example.com/a", row, N);
    row[21] = 0x00E9; // é
    for (int k = 22; k < N; ++k)
        row[k] = ' ';
    const auto nonASCIIBounded = Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 8,
        std::span<char>(outputBuffer, sizeof(outputBuffer)));
    CHECK(nonASCIIBounded.has_value());
    CHECK(nonASCIIBounded && nonASCIIBounded->last == 20);
}

// And what it gives back. Sentence punctuation after a URL belongs to the
// sentence, so it is trimmed — but only from the end, and never into the scheme.
TEST(testEveryTrailingPunctuationMarkIsTrimmed)
{
    const int N = 80;
    uint32_t row[N];
    char outputBuffer[256];

    for (const char* trailing : { ".", ",", ";", ":", "!", "?", ")", "]" }) {
        std::string text = "https://example.com/a";
        text += trailing;
        layRow(text.c_str(), row, N);
        CHECK_MSG(Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 8,
                      std::span<char>(outputBuffer, sizeof(outputBuffer))),
            trailing);
        CHECK_MSG(std::strcmp(outputBuffer, "https://example.com/a") == 0, trailing);
    }

    // A run of them comes off together, and the same characters inside the URL
    // stay — the trim reads from the end rather than deleting a character class.
    layRow("https://example.com/a?b=1,2!x).", row, N);
    CHECK(Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 8,
        std::span<char>(outputBuffer, sizeof(outputBuffer))));
    CHECK_STR_EQ(outputBuffer, "https://example.com/a?b=1,2!x");
}

// The two span entry points answer rather than read when they are handed
// nothing to read. A frontend calls these on every hover, with a column that
// came from a pointer position.
TEST(testTheSpanEntryPointsRefuseWhatTheyCannotRead)
{
    const int N = 40;
    uint32_t row[N];
    layRow("see https://example.com/a and /tmp/x.txt", row, N);
    char outputBuffer[256];
    const std::span<char> out(outputBuffer, sizeof(outputBuffer));

    for (int column : { -1, N, N + 100 }) {
        CHECK(!Attributes::LinkRun::urlMatchSpanAtColumn(row, N, column, out));
        CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(row, N, column, out));
    }

    CHECK(!Attributes::LinkRun::urlMatchSpanAtColumn(nullptr, N, 6, out));
    CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(nullptr, N, 32, out));

    const std::span<char> noRoom(outputBuffer, 0);
    CHECK(!Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 6, noRoom));
    CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 32, noRoom));

    // The control: the same row and columns do match when there is somewhere to
    // put the answer, so the refusals above are about the arguments.
    CHECK(Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 6, out));
    CHECK(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 32, out));

    // A hit-test that wants no column just discards the span; the signature has no
    // null-pointer parameter for it.
    CHECK(Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 6, out).has_value());
}

namespace {

// A row wider than the detector's fixed scan buffer, holding one URL at its
// start. The probe answers for any column so the width is the only thing under
// test.
uint32_t wideRowCodePoint(void*, int column)
{
    static const char* const text = "https://example.com/a";
    const int length = static_cast<int>(std::strlen(text));
    if (column >= 0 && column < length)
        return static_cast<uint32_t>(text[column]);
    return ' ';
}

uint16_t noLinkIDs(void*, int) { return 0; }

const char* noURIs(void*, uint16_t) { return nullptr; }

} // namespace

// A row wider than the scan buffer is read up to the buffer and no further. The
// column count comes from the grid, and a terminal can be resized past 2048.
TEST(testARowWiderThanTheScanBufferIsClamped)
{
    Attributes::LinkRun::Run inside = Attributes::LinkRun::runAtColumn(4000, 8,
        wideRowCodePoint, noLinkIDs, nullptr, noURIs, nullptr);
    CHECK_MSG(inside.valid, "a URL inside the clamped window is still found");
    CHECK_STR_EQ(inside.uri, "https://example.com/a");

    // A column past the clamp answers nothing rather than reading past the
    // buffer's end.
    Attributes::LinkRun::Run outside = Attributes::LinkRun::runAtColumn(4000, 3000,
        wideRowCodePoint, noLinkIDs, nullptr, noURIs, nullptr);
    CHECK_MSG(!outside.valid, "a column past the scan window has no run");

    // And the control: the same column in a row that is not over-wide is inside
    // the window, so the answer above is about the clamp.
    Attributes::LinkRun::Run narrow = Attributes::LinkRun::runAtColumn(40, 8,
        wideRowCodePoint, noLinkIDs, nullptr, noURIs, nullptr);
    CHECK(narrow.valid);
}

// Only `www.` arrives without a scheme, and it is the only one four bytes long
// — which is what lets the length alone decide that a run needs the https://
// prefix. Asserted through the detector so a scheme added at that length stops
// this passing rather than silently acquiring a prefix.
TEST(testWwwIsTheOnlyFourLongScheme)
{
    const int N = 80;
    uint32_t row[N];
    char outputBuffer[256];
    const std::span<char> out(outputBuffer, sizeof(outputBuffer));

    struct Expected {
        const char* url;
        const char* materialized;
    };
    for (const Expected e : { Expected { "https://a.b", "https://a.b" },
             Expected { "http://a.b", "http://a.b" }, Expected { "file:///x", "file:///x" },
             Expected { "ftp://a.b", "ftp://a.b" }, Expected { "ssh://a.b", "ssh://a.b" },
             Expected { "mailto:a@b.c", "mailto:a@b.c" },
             Expected { "www.a.b", "https://www.a.b" } }) {
        layRow(e.url, row, N);
        const auto span = Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 2, out);
        CHECK_MSG(span.has_value(), e.url);
        CHECK_MSG(std::strcmp(outputBuffer, e.materialized) == 0, e.url);
        // The span is the on-screen columns, so a prefix the user cannot see is
        // not counted into it.
        CHECK_MSG(span && span->first == 0 && span->last == static_cast<int>(std::strlen(e.url)) - 1, e.url);
    }
}

// What each entry point writes when there is less room than there is answer.
//
// The property that matters is not overrunning. Truncating and refusing both
// satisfy that; they differ in what the caller then does with the
// result. A shortened URL still parses and still opens, so a click lands on an
// address that was never on screen -- a 450-character authorize URL cut at
// byte 255 navigates somewhere else. A shortened path names a real directory
// for the same reason.
//
// So a run that does not fit is reported as no run. The absence is visible; the
// wrong answer is not.
TEST(testARunLongerThanTheBufferIsRefusedRatherThanOverrun)
{
    const int N = 80;
    uint32_t row[N];

    // A www. match, so the https:// prefix alone already fills the buffer.
    char tiny[5];
    layRow("www.example.com", row, N);
    const auto refused = Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 2, std::span<char>(tiny, sizeof(tiny)));
    CHECK(!refused.has_value());
    CHECK_EQ(static_cast<int>(tiny[0]), 0);

    // And one where the prefix fits and the URL is what would be cut.
    char small[12];
    CHECK(!Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 2, std::span<char>(small, sizeof(small))).has_value());

    // Given room, the same row still answers -- the refusal is about capacity,
    // not about the run.
    char roomy[64];
    CHECK(Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 2, std::span<char>(roomy, sizeof(roomy))));
    CHECK_STR_EQ(roomy, "https://www.example.com");

    char cramped[6];
    layRow("cd /tmp/some/deep/path", row, N);
    CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 8, std::span<char>(cramped, sizeof(cramped))).has_value());
    CHECK_EQ(static_cast<int>(cramped[0]), 0);
}

TEST(testAURLTrimmedBackToItsSchemeIsNotARun)
{
    const int N = 40;
    uint32_t row[N];
    char outputBuffer[64];
    const std::span<char> out(outputBuffer, sizeof(outputBuffer));

    // Nothing but punctuation after the scheme, so the run names nothing: no
    // host, no path, nothing a click could reach. A prefix on its own is not a
    // link, however well-formed the prefix is.
    layRow("https://.", row, N);
    const auto schemeOnly = Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 2, out);
    CHECK(!schemeOnly.has_value());

    // A URL earlier in the row does not answer for a column after it, which is
    // the other side of the span test — a hover on the following word is not a
    // hover on the link.
    layRow("https://a.b   word", row, N);
    CHECK(!Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 15, out));
    CHECK(Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 5, out));
}

// The path-byte set, driven rather than read. A path holding an underscore, a
// hyphen or a plus is ordinary, and each is a separate label in the classifier.
TEST(testEveryPathByteHoldsTheRunTogether)
{
    const int N = 80;
    uint32_t row[N];
    char outputBuffer[128];
    const std::span<char> out(outputBuffer, sizeof(outputBuffer));

    for (const char* path : { "/tmp/a_b/c", "/tmp/a-b/c", "/tmp/a+b/c", "/tmp/a.b/c",
             "/tmp/a~b/c", "~/src/main.cpp" }) {
        std::string text = "cd ";
        text += path;
        layRow(text.c_str(), row, N);
        CHECK_MSG(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 6, out), path);
        CHECK_MSG(std::strcmp(outputBuffer, path) == 0, path);
    }

    // A home-relative run with no slash at all is still a path, which is the
    // only way the leading-tilde arm is reached.
    layRow("cd ~home", row, N);
    CHECK(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 5, out));
    CHECK_STR_EQ(outputBuffer, "~home");

    // Punctuation with a slash but no letters or digits is not a path, so the
    // rule above is about the tilde rather than about anything being accepted.
    layRow("cd /-_/ x", row, N);
    CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 4, out));

    // A lone slash is a one-column run: nothing to trim, and nothing that reads
    // as a path.
    layRow("a / b", row, N);
    CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 2, out));

    // A trailing sentence period comes off, but a real ".." tail stays — the
    // trim looks at the character before it for exactly this reason.
    layRow("see /tmp/x.cpp.", row, N);
    CHECK(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 8, out));
    CHECK_STR_EQ(outputBuffer, "/tmp/x.cpp");
    layRow("cd /tmp/sub/..", row, N);
    CHECK(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 8, out));
    CHECK_STR_EQ(outputBuffer, "/tmp/sub/..");

    // The span may be discarded, the same as for URLs.
    layRow("cd /tmp/x", row, N);
    CHECK(Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 5, out).has_value());
}

// A span with nowhere to write is not the same as a span with no room: the
// first has no pointer at all, which is what a caller passing a default-built
// span hands over.
TEST(testASpanWithNoStorageIsRefused)
{
    const int N = 40;
    uint32_t row[N];
    layRow("see https://example.com and /tmp/x.txt", row, N);
    const std::span<char> nowhere;

    CHECK(!Attributes::LinkRun::urlMatchSpanAtColumn(row, N, 6, nowhere));
    CHECK(!Attributes::LinkRun::pathMatchSpanAtColumn(row, N, 30, nowhere));
}

// Case folding runs over bytes that are not letters, and an extension or scheme
// carrying a digit is ordinary — the fold must leave those alone rather than
// shifting them.
TEST(testCaseFoldingLeavesNonLettersAlone)
{
    using Ubiquitty::Attributes::LinkRun::isSafeToAutoOpenFileExtension;

    // Lengths chosen to match list entries, so the comparison actually runs
    // rather than being skipped by the length test.
    CHECK(!isSafeToAutoOpenFileExtension("song.m4"));
    CHECK(!isSafeToAutoOpenFileExtension("page.h1"));
    CHECK(!isSafeToAutoOpenFileExtension("data.js0"));
    CHECK(!isSafeToAutoOpenFileExtension("clip.mp5"));

    // The same over the scheme readers, which fold the same way.
    CHECK(!Attributes::LinkRun::isLaunchableURLScheme("s3://bucket/key"));
    CHECK(!Attributes::LinkRun::isLaunchableURLScheme("http2://host"));
    CHECK(Attributes::LinkRun::kindForURI("s3://bucket/key") == Attributes::LinkRun::Kind::URL);

    // The scheme reader compares only against names of its own length, so a
    // digit is reached only when it sits inside one that matches: these are the
    // same length as "file" and "mailto" and differ by one character.
    CHECK(Attributes::LinkRun::kindForURI("f1le:///etc/hosts") == Attributes::LinkRun::Kind::URL);
    CHECK(Attributes::LinkRun::kindForURI("1ile:///etc/hosts") == Attributes::LinkRun::Kind::URL);
    CHECK(Attributes::LinkRun::kindForURI("ma1lto:a@b.c") == Attributes::LinkRun::Kind::URL);

    // The control: the spellings they are one character away from do classify,
    // so the answers above are about the difference rather than about this
    // reader never recognising anything.
    CHECK(Attributes::LinkRun::kindForURI("file:///etc/hosts") == Attributes::LinkRun::Kind::File);
    CHECK(Attributes::LinkRun::kindForURI("mailto:a@b.c") == Attributes::LinkRun::Kind::Mailto);
}

namespace {

uint32_t blankProbe(void*, int) { return 0; }

uint16_t alwaysLinked(void*, int) { return 4; }

const char* emptyURI(void*, uint16_t) { return ""; }

const char* overLongURI(void*, uint16_t)
{
    static std::string held = "https://example.com/"
        + std::string(static_cast<std::size_t>(Ubiquitty::VT::maxLinkURILength), 'q');
    return held.c_str();
}

} // namespace

// runAtColumn is called on every pointer move, with a column and a width that
// came from the frontend and probes that came from whichever backend built the
// call. It answers rather than reads when any of them is missing.
TEST(testTheRunEntryPointRefusesAnIncompleteCall)
{
    Attributes::LinkRun::Run run;

    run = Attributes::LinkRun::runAtColumn(0, 0, wideRowCodePoint, noLinkIDs, nullptr, noURIs, nullptr);
    CHECK_MSG(!run.valid, "a grid with no columns has no run");
    run = Attributes::LinkRun::runAtColumn(40, 8, nullptr, noLinkIDs, nullptr, noURIs, nullptr);
    CHECK_MSG(!run.valid, "no way to read a cell means no run");
    run = Attributes::LinkRun::runAtColumn(40, 8, wideRowCodePoint, nullptr, nullptr, noURIs, nullptr);
    CHECK_MSG(!run.valid, "no way to read a link id means no run");

    // A cell holding an id but no way to resolve it falls through to pattern
    // detection rather than claiming the run, and so does one whose id resolves
    // to nothing.
    run = Attributes::LinkRun::runAtColumn(40, 8, wideRowCodePoint, alwaysLinked, nullptr, nullptr, nullptr);
    CHECK_MSG(run.valid, "an unresolvable id falls back to the pattern");
    CHECK_STR_EQ(run.uri, "https://example.com/a");
    run = Attributes::LinkRun::runAtColumn(40, 8, wideRowCodePoint, alwaysLinked, nullptr, emptyURI, nullptr);
    CHECK_MSG(run.valid, "an id resolving to nothing falls back too");

    // A cell reading as zero is a blank, not a null: the scan substitutes a
    // space so a sparse row does not terminate it early.
    run = Attributes::LinkRun::runAtColumn(40, 8, blankProbe, noLinkIDs, nullptr, noURIs, nullptr);
    CHECK_MSG(!run.valid, "a row of blanks has no run");

    // A resolved URI longer than the run's own field is truncated into it
    // rather than overrunning.
    run = Attributes::LinkRun::runAtColumn(40, 8, wideRowCodePoint, alwaysLinked, nullptr, overLongURI, nullptr);
    CHECK(run.valid);
    CHECK(std::strlen(run.uri) == static_cast<std::size_t>(Ubiquitty::VT::maxLinkURILength) - 1);
}

// A 450-character OAuth authorize URL survives whole.
//
// A 256-byte buffer stops the copy at byte 255 -- inside a percent-escape, at
// "...user%3As" -- and the shortened string still parses as a URL, so a click
// opens a DIFFERENT address, one that is not on screen. Wherever the click lands
// makes no difference: the run is materialized from its start every time.
//
// The length is the subject, so it is spelled out rather than generated: a
// synthesized string of the same size would not pin the escapes, and the
// escapes are what make a truncation land mid-token instead of visibly
// mid-word.
TEST(testALongOAuthURLIsNotTruncated)
{
    static const char kURL[] = "https://claude.com/cai/oauth/authorize?code=true&client_id=9d1c250a-e61b-44d9-88ed-5944d1962f5e&response_type=code&redirect_uri=https%3A%2F%2Fplatform.claude.com%2Foauth%2Fcode%2Fcallback&scope=org%3Acreate_api_key+user%3Aprofile+user%3Ainference+user%3Asessions%3Aclaude_code+user%3Amcp_servers+user%3Afile_upload&code_challenge=aQzIQj1m4zZ2swH14PwaS4DkOiu8rdCgifJpDVTPhII&code_challenge_method=S256&state=sgf1cflVjvYzjtjgTnZFQcQSj-7nruEdDAX4-XxD6us";
    const int length = static_cast<int>(sizeof(kURL) - 1);
    CHECK_EQ(length, 450);

    std::vector<uint32_t> row(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i)
        row[static_cast<size_t>(i)] = static_cast<uint32_t>(kURL[i]);

    char uri[Ubiquitty::VT::maxLinkURILength] = {};
    const auto span = Ubiquitty::Attributes::LinkRun::urlMatchSpanAtColumn(
        row.data(), length, 10, std::span<char> { uri, sizeof(uri) });
    CHECK(span.has_value());
    CHECK_STR_EQ(uri, kURL);

    // From the tail too: the same run, not a fresh match starting mid-string.
    char fromTail[Ubiquitty::VT::maxLinkURILength] = {};
    const auto tail = Ubiquitty::Attributes::LinkRun::urlMatchSpanAtColumn(
        row.data(), length, length - 3, std::span<char> { fromTail, sizeof(fromTail) });
    CHECK(tail.has_value());
    CHECK_STR_EQ(fromTail, kURL);
}

// A run that does not fit is no link, not a short one.
//
// The buffer is finite whatever its size, so the behaviour past the end is a
// choice rather than an accident. A truncated URL is a wrong answer wearing the
// right shape: it opens. An absent one is a visible absence.
TEST(testARunTooLongForTheBufferIsRefused)
{
    std::vector<uint32_t> row;
    const char* prefix = "https://example.com/";
    for (const char* p = prefix; *p; ++p)
        row.push_back(static_cast<uint32_t>(*p));
    while (row.size() < static_cast<size_t>(Ubiquitty::VT::maxLinkURILength) + 64)
        row.push_back(static_cast<uint32_t>('a'));

    char uri[Ubiquitty::VT::maxLinkURILength] = {};
    const auto span = Ubiquitty::Attributes::LinkRun::urlMatchSpanAtColumn(
        row.data(), static_cast<int>(row.size()), 10, std::span<char> { uri, sizeof(uri) });
    CHECK(!span.has_value());
    CHECK_EQ(static_cast<int>(uri[0]), 0);
}

// A link handler's argv, which is where a mistake becomes a command injection.
TEST(linkHandlerArgvKeepsTheURIWhole)
{
    using Ubiquitty::Attributes::LinkRun::linkHandlerArgv;
    using Argv = std::vector<std::string>;

    // The placeholder takes the whole URI as one argument.
    CHECK((linkHandlerArgv("open -a Safari %s", "https://example.com/a")
        == Argv { "open", "-a", "Safari", "https://example.com/a" }));
    CHECK((linkHandlerArgv("handler %u", "x:y") == Argv { "handler", "x:y" }));

    // The case this shape exists for: a URI carrying shell metacharacters and
    // spaces stays ONE argument, so nothing it contains can become a command.
    const Argv injected = linkHandlerArgv("open %s", "https://e.com/a; rm -rf ~ && echo $(whoami)");
    CHECK(injected.size() == 2);
    CHECK(injected[1] == "https://e.com/a; rm -rf ~ && echo $(whoami)");

    // A template naming no placeholder still receives the link, appended.
    CHECK((linkHandlerArgv("open -a Jira", "https://j/1") == Argv { "open", "-a", "Jira", "https://j/1" }));

    // Runs of whitespace collapse rather than making empty arguments, and a
    // literal percent survives as one.
    CHECK((linkHandlerArgv("  run   %%   %s ", "u:1") == Argv { "run", "%", "u:1" }));

    // A template naming no program runs nothing: there is no argv[0] to exec,
    // and appending the URI would make the LINK the program.
    CHECK(linkHandlerArgv("", "https://example.com").empty());
    CHECK(linkHandlerArgv("   ", "https://example.com").empty());
}

// The argv a line handler runs: the reader's words, with the checked path, the
// line and the column put where the template names them and nowhere else.
TEST(lineHandlerArgvPutsTheCheckedFileAtItsLine)
{
    using Ubiquitty::Attributes::LinkRun::lineHandlerArgv;
    using Argv = std::vector<std::string>;

    CHECK((lineHandlerArgv("code --goto {path}:{line}:{column}", "/src/main.swift", 12, 5)
        == Argv { "code", "--goto", "/src/main.swift:12:5" }));
    CHECK((lineHandlerArgv("nvim +{line} {path}", "/src/main.swift", 12, 0) == Argv { "nvim", "+12", "/src/main.swift" }));
    CHECK_MSG((lineHandlerArgv("zed {path}:{line}:{column}", "/src/main.swift", 12, 0) == Argv { "zed", "/src/main.swift:12:1" }),
        "a link that names no column fills it with 1");

    // A path carrying spaces and shell syntax is one argument, and a path that
    // spells a placeholder is not read as one: each placeholder is replaced
    // once, where the template put it.
    CHECK((lineHandlerArgv("edit {path}", "/tmp/a b; rm -rf ~ $(x)", 3, 0) == Argv { "edit", "/tmp/a b; rm -rf ~ $(x)" }));
    CHECK((lineHandlerArgv("edit {path} {line}", "/tmp/{line}", 3, 0) == Argv { "edit", "/tmp/{line}", "3" }));

    // A template naming no path still receives it, appended.
    CHECK((lineHandlerArgv("subl", "/src/main.swift", 12, 0) == Argv { "subl", "/src/main.swift" }));
    CHECK((lineHandlerArgv("  open   -t  ", "/src/a.txt", 1, 0) == Argv { "open", "-t", "/src/a.txt" }));

    // Nothing runs without a program, a path and a line.
    CHECK(lineHandlerArgv("", "/src/main.swift", 12, 0).empty());
    CHECK(lineHandlerArgv("   ", "/src/main.swift", 12, 0).empty());
    CHECK(lineHandlerArgv("code {path}", "", 12, 0).empty());
    CHECK(lineHandlerArgv("code {path}", "/src/main.swift", 0, 0).empty());
}

// Which files a line handler takes: any regular file, the ones the opener would
// only reveal among them, and nothing else.
TEST(theLineHandlerTakesEveryRegularFileAndNothingElse)
{
    using Ubiquitty::Attributes::LinkRun::lineHandlerTakes;
    using Ubiquitty::Attributes::LinkRun::LocalFileFacts;

    CHECK(lineHandlerTakes({ .isRegularFile = true }));
    CHECK_MSG(lineHandlerTakes({ .isRegularFile = true, .isExecutable = true, .isLaunchableType = true }),
        "a script a search found opens in the editor at its match");
    CHECK(!lineHandlerTakes({ .isDirectory = true }));
    CHECK(!lineHandlerTakes({ .isDirectory = true, .isPackage = true }));
    CHECK_MSG(!lineHandlerTakes(LocalFileFacts {}), "nothing at the path, or a device, a socket or a pipe");
}

// The scheme a handler lookup matches on.
TEST(schemeOfURIIsLowercasedAndOptional)
{
    using Ubiquitty::Attributes::LinkRun::schemeOfURI;

    CHECK(schemeOfURI("https://example.com") == "https");
    // Case-insensitive per RFC 3986, which is what makes a configured key match
    // the link it was written for however either was typed.
    CHECK(schemeOfURI("HTTPS://example.com") == "https");
    CHECK(schemeOfURI("Jira:1234") == "jira");

    // No scheme: a relative reference, and a colon that starts the string names
    // nothing before it.
    CHECK(schemeOfURI("example.com/path").empty());
    CHECK(schemeOfURI(":nothing").empty());
    CHECK(schemeOfURI("").empty());
}

// What path a bare candidate names, given the two things the text does not say.
TEST(testAFilePathCandidateResolvesAgainstThePanesDirectory)
{
    using Ubiquitty::Attributes::LinkRun::resolveFilePathCandidate;
    const std::string_view base = "/work/project";
    const std::string_view home = "/home/user";

    CHECK_MSG(resolveFilePathCandidate("src/main.cpp", base, home) == "/work/project/src/main.cpp",
        "a relative candidate joins the pane's directory");
    CHECK_MSG(resolveFilePathCandidate("/etc/hosts", base, home) == "/etc/hosts",
        "an absolute one is already the answer");
    CHECK_MSG(resolveFilePathCandidate("~/notes.md", base, home) == "/home/user/notes.md",
        "a leading tilde is the home directory");
    CHECK_MSG(resolveFilePathCandidate("~", base, home) == "/home/user", "bare, too");

    // Lexical folding, so the caller stats one path rather than a walk.
    CHECK(resolveFilePathCandidate("./a/../b/c.txt", base, home) == "/work/project/b/c.txt");
    CHECK(resolveFilePathCandidate("a//b", base, home) == "/work/project/a/b");
    CHECK_MSG(resolveFilePathCandidate("../..", base, home) == "/",
        "a walk past the root stops there rather than underflowing");

    // The cases that resolve to nothing, each for its own reason. A candidate
    // that names no path is one the caller never stats, and an escape sequence
    // chose the string.
    CHECK_MSG(resolveFilePathCandidate("", base, home).empty(), "an empty candidate names nothing");
    CHECK_MSG(resolveFilePathCandidate("~somebody/notes", base, home).empty(),
        "a ~user form needs a password database Core has no business reading");
    CHECK_MSG(resolveFilePathCandidate("~/notes", base, "").empty(),
        "and a tilde with no home to expand against");
    CHECK_MSG(resolveFilePathCandidate("src/main.cpp", "", home).empty(),
        "a relative candidate with no base directory is not a guess");
    CHECK_MSG(resolveFilePathCandidate("src/main.cpp", "relative/base", home).empty(),
        "nor is one whose base is itself relative");
}

UBIQUITTY_TEST_MAIN()
