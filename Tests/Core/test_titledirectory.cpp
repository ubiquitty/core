// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_titledirectory.cpp — pins Title::titleDirectorySegment, the working-
// directory title segment policy shared by the macOS and Linux frontends.
//
// The home/~ collapse, the basename-vs-full-~-path choice, the exactly-home → ~
// rule, the trailing-slash normalization, and the empty-input case all live in
// the one header; this test keeps them from drifting apart. Header-
// only (pure std::string), so it links standalone.

#include "Core/Title/Directory.h"

#include <cstdio>
#include <string>

using Ubiquitty::Title::PathFormat;
using Ubiquitty::Title::titleDirectorySegment;
static void expectSegment(const std::string& cwd, const std::string& home, bool fullPath, const std::string& want, const char* label)
{
    const std::string got = titleDirectorySegment(cwd, home, fullPath ? PathFormat::FullPath : PathFormat::BasenameOnly);
    if (got != want) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), R"(%s: titleDirectorySegment("%s", "%s", %d) = "%s", want "%s")", label, cwd.c_str(), home.c_str(), fullPath ? 1 : 0, got.c_str(), want.c_str());
        CHECK_MSG(false, ubqMessage);
    }
}

TEST(titledirectory)
{

    const std::string home = "/home/user";

    // basename mode (fullPath = false)
    expectSegment("/home/user/Projects/ubiquitty", home, false, "ubiquitty", "basename");
    expectSegment("/home/user", home, false, "~", "exactly-home → ~");
    expectSegment("/var/log/", home, false, "log", "trailing-slash trimmed");
    expectSegment("/", home, false, "/", "root basename collapses to /");
    expectSegment("", home, false, "", "no directory → empty");
    expectSegment("/usr/local/bin", home, false, "bin", "non-home basename");

    // full ~-path mode (fullPath = true)
    expectSegment("/home/user/Projects", home, true, "~/Projects", "full ~-path");
    expectSegment("/home/user", home, true, "~", "exactly-home full → ~");
    expectSegment("/home/user/", home, true, "~", "exactly-home trailing slash → ~");
    expectSegment("/etc/ssh", home, true, "/etc/ssh", "non-home full path unchanged");
    expectSegment("", home, true, "", "no directory full → empty");

    // Home prefix must be a full path component, not a substring: "/home/usertwo"
    // is not under "/home/user" and must not collapse.
    expectSegment("/home/usertwo", home, true, "/home/usertwo", "home is not a string prefix");
    expectSegment("/home/usertwo", home, false, "usertwo", "home-prefix basename unaffected");

    // Empty home: nothing collapses to ~.
    expectSegment("/home/user", "", true, "/home/user", "empty home → no ~ collapse");
    expectSegment("/home/user", "", false, "user", "empty home → plain basename");

    // macOS-shaped home, exercising the same policy under /Users.
    expectSegment("/Users/user/", "/Users/user", false, "~", "macOS exactly-home → ~");
    expectSegment("/Users/user/Code", "/Users/user", true, "~/Code", "macOS full ~-path");

    // Basename mode collapses the home prefix before taking the last component,
    // so a path under home reads from its ~-relative form.
    expectSegment("/Users/user/Projects/ubiquitty", "/Users/user", false, "ubiquitty", "basename under home");
    expectSegment("/Users/user/Projects", "/Users/user", true, "~/Projects", "full path keeps the ~ anchor");
    // Outside home nothing collapses; the basename stands alone.
    expectSegment("/opt/homebrew/bin", "/Users/user", false, "bin", "outside home, basename");
    expectSegment("/opt/homebrew/bin", "/Users/user", true, "/opt/homebrew/bin", "outside home, full path");
    // Root has no basename; both modes keep "/" rather than going empty.
    expectSegment("/", "/Users/user", true, "/", "root full path stays /");
    // A directory whose basename equals the home basename must not collapse:
    // only a real home-anchored path becomes ~.
    expectSegment("/srv/user", "/Users/user", false, "user", "same basename, different path");
    expectSegment("/srv/user", "/Users/user", true, "/srv/user", "same basename, no collapse");
    // Repeated trailing slashes normalize the same as one.
    expectSegment("/Users/user///", "/Users/user", true, "~", "repeated trailing slashes");
    expectSegment("/var/log///", "/Users/user", false, "log", "repeated slashes, basename");
    // A home with a trailing slash is not the same string as the cwd, so the
    // collapse must not depend on the caller normalizing home for us.
    expectSegment("/Users/user/Code", "/Users/user", false, "Code", "home without trailing slash");
}

/// A remote session's title, composed from the parts the host sends.
///
/// The host sends the inputs rather than a finished title so each frontend can
/// spell it in its own voice. Both macOS shells ask this, so a case here is a
/// case for both — and the two cannot drift into an em dash on one and something else on the
/// other for the same session.
TEST(testComposeRemoteTitleJoinsTheHostsPartsOnce)
{
    using Ubiquitty::Title::composeRemoteTitle;
    const std::string home = "/Users/user";

    CHECK(composeRemoteTitle("/Users/user/Code", home, "vim", Ubiquitty::PAL::Process::TitleSeparator::Spaced) == "Code — vim");
    // The directory collapses the same way it does alone: exactly home is ~.
    CHECK(composeRemoteTitle("/Users/user", home, "zsh", Ubiquitty::PAL::Process::TitleSeparator::Spaced) == "~ — zsh");

    // Either half alone stands as the whole title rather than carrying a
    // dangling separator.
    CHECK(composeRemoteTitle("/Users/user/Code", home, "", Ubiquitty::PAL::Process::TitleSeparator::Spaced) == "Code");
    CHECK(composeRemoteTitle("", home, "vim", Ubiquitty::PAL::Process::TitleSeparator::Spaced) == "vim");
    CHECK(composeRemoteTitle("", home, "", Ubiquitty::PAL::Process::TitleSeparator::Spaced).empty());

    // A process that reports as the folder it sits in — a make in a directory
    // named make — would read "make — make", which is two facts carrying one.
    CHECK(composeRemoteTitle("/Users/user/make", home, "make", Ubiquitty::PAL::Process::TitleSeparator::Spaced) == "make");
    // Only an exact repeat folds. A process whose name merely contains the
    // folder is a different fact and both are kept.
    CHECK(composeRemoteTitle("/Users/user/make", home, "make install", Ubiquitty::PAL::Process::TitleSeparator::Spaced) == "make — make install");
}

// OSC 6 names a document; the proxy icon should stand for it rather than for
// the folder it sits in — but only when it is a document on the local machine.
TEST(testALocalDocumentURLIsTakenAndARemoteOneIsNot)
{
    using Ubiquitty::Title::isLocalDocumentURL;

    CHECK_MSG(isLocalDocumentURL("file:///Users/user/notes.md"), "an empty authority is this machine");
    CHECK_MSG(isLocalDocumentURL("file://localhost/Users/user/notes.md"), "localhost is this machine");
    CHECK_MSG(isLocalDocumentURL("file://LOCALHOST/tmp/x"), "the authority is compared without case");
    CHECK_MSG(isLocalDocumentURL("file://"), "a bare scheme has no authority, so it is local");

    // The case the rule exists for: a shell on the far side of an ssh session
    // reports the document visible to the remote program. Representing that would point the
    // Finder at a path that is not here.
    CHECK_MSG(!isLocalDocumentURL("file://build-host/home/user/notes.md"), "another host is not this machine");
    CHECK_MSG(!isLocalDocumentURL("file://localhost.example.com/x"), "a longer name that starts with localhost is not it");
    CHECK_MSG(!isLocalDocumentURL("file://localhos/x"), "a shorter near-miss is not it");

    // Not a file URL at all, and the empty case that means "nothing reported".
    CHECK_MSG(!isLocalDocumentURL("https://example.com/x"), "only file URLs name documents");
    CHECK_MSG(!isLocalDocumentURL(""), "no report is not a document");
    CHECK_MSG(!isLocalDocumentURL("/Users/user/notes.md"), "a bare path carries no scheme");
}

TEST(localURLAuthorityUsesTheCurrentHostName)
{
    using Ubiquitty::Title::isLocalURLAuthority;
    CHECK(isLocalURLAuthority("WorkStation.local", "workstation.LOCAL"));
    CHECK(!isLocalURLAuthority("another.local", "workstation.local"));
    CHECK(!isLocalURLAuthority("workstation.local", "renamed.local"));
    CHECK(!isLocalURLAuthority("workstation.local", ""));
    CHECK(isLocalURLAuthority("LOCALHOST", "workstation.local"));
    CHECK(isLocalURLAuthority("", "workstation.local"));
    const std::string current = Ubiquitty::PAL::systemHostName();
    if (!current.empty()) {
        CHECK(isLocalURLAuthority(current));
        CHECK(Ubiquitty::Title::isLocalDocumentURL("file://" + current + "/tmp/file"));
    }
}

UBIQUITTY_TEST_MAIN()

TEST(remoteDirectoriesHaveNoLocalFolderProxy)
{
    using namespace Ubiquitty;
    PAL::Process::RemoteTarget local;
    CHECK(Title::isLocalDirectory(local, ""));
    CHECK(Title::isLocalDirectory(local, "localhost"));
    CHECK(!Title::isLocalDirectory(local, "remote.invalid"));
    for (const auto kind : { PAL::Process::RemoteKind::Host, PAL::Process::RemoteKind::Container,
             PAL::Process::RemoteKind::VirtualMachine, PAL::Process::RemoteKind::Serial }) {
        PAL::Process::RemoteTarget remote;
        remote.kind = kind;
        CHECK(!Title::isLocalDirectory(remote, ""));
        CHECK(!Title::isLocalDirectory(remote, "localhost"));
    }
}

TEST(aTitleUnderAConnectionNamesOnlyWhatWasReported)
{
    using namespace Ubiquitty;
    // Here, the pane's directory is the title's, reported or sampled.
    CHECK(Title::titleDirectory({ .reportedDirectory = "", .paneDirectory = "/Users/user/project" }) == "/Users/user/project");
    CHECK(Title::titleDirectory({ .reportedDirectory = "/Users/user/project", .paneDirectory = "/Users/user/project" }) == "/Users/user/project");
    for (const auto kind : { PAL::Process::RemoteKind::Host, PAL::Process::RemoteKind::Container,
             PAL::Process::RemoteKind::VirtualMachine, PAL::Process::RemoteKind::Serial }) {
        // The client's own folder, sampled, is not where the session is.
        CHECK(Title::titleDirectory({ .reportedDirectory = "", .paneDirectory = "/Users/user/project", .foregroundLeadsTo = kind }).empty());
        // What the far side reported is.
        CHECK(Title::titleDirectory({ .reportedDirectory = "/home/user", .paneDirectory = "/home/user", .foregroundLeadsTo = kind }) == "/home/user");
    }
}
