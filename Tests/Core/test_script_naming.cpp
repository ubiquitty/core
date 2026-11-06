// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// An interpreter running a script is named by the SCRIPT.
//
// `python3 /srv/deploy.py` tells a reader nothing if it reads as `Python`, and
// nothing again if it reads as the interpreter plus a path too long for a tab.
// The script's own name is what they called it.
//
// Four places hold the rule where a narrower one would not, each a row below:
// the Arguments option does not gate it, a bare filename counts as a script,
// `-m` names its module, and the set has heard of node.

#include "PAL/Process.h"

#include <cstdio>
#include <string_view>

using Ubiquitty::PAL::Process::scriptDisplayName;

namespace {

struct Row {
    std::string_view name; // what the kernel calls the process
    std::string_view commandLine; // argv joined by spaces
    std::string_view shown;
};

bool check(const Row& row)
{
    const std::string_view got = scriptDisplayName(row.name, row.commandLine);
    if (got == row.shown)
        return true;
    std::printf("    name=%.*s cmd=%.*s\n      wanted %.*s, got %.*s\n",
        static_cast<int>(row.name.size()), row.name.data(),
        static_cast<int>(row.commandLine.size()), row.commandLine.data(),
        static_cast<int>(row.shown.size()), row.shown.data(),
        static_cast<int>(got.size()), got.data());
    return false;
}

bool checkAll(std::span<const Row> rows)
{
    bool didAllMatch = true;
    for (const Row& row : rows)
        didAllMatch = check(row) && didAllMatch;
    return didAllMatch;
}

} // namespace

// The interpreter steps aside for the script, however the path is spelled.
TEST(testAnInterpreterIsNamedByTheScriptItRuns)
{
    static constexpr Row rows[] {
        { "python3", "python3 /srv/deploy.py", "deploy.py" },
        { "python3", "python3 ./deploy.py", "deploy.py" },
        { "python3", "python3 ../srv/deploy.py", "deploy.py" },
        { "ruby", "ruby /srv/task.rb", "task.rb" },
        { "perl", "perl /srv/task.pl", "task.pl" },
        { "bash", "bash /srv/build.sh", "build.sh" },
        { "zsh", "zsh /srv/build.sh", "build.sh" },
        // An option before the script does not hide it.
        { "python3", "python3 -u /srv/deploy.py", "deploy.py" },
        { "ruby", "ruby -w task.rb", "task.rb" },
    };
    CHECK(checkAll(rows));
}

// The script in `python3 deploy.py` is named even without a path prefix.
TEST(testABareFilenameIsAScriptToo)
{
    static constexpr Row rows[] {
        { "python3", "python3 deploy.py", "deploy.py" },
        { "bash", "bash build.sh", "build.sh" },
    };
    CHECK(checkAll(rows));
}

// A module is what `-m` names, and it is what the reader typed.
TEST(testAModuleIsNamedByItsOwnName)
{
    static constexpr Row rows[] {
        { "python3", "python3 -m http.server", "http.server" },
        { "python3", "python3 -m pytest", "pytest" },
    };
    CHECK(checkAll(rows));
}

// macOS names the framework executable, so a python process reports `Python`.
// A set matched case-sensitively misses every one of them.
TEST(testTheSetMatchesWhateverCaseTheProcessReports)
{
    static constexpr Row rows[] {
        { "Python", "Python /srv/deploy.py", "deploy.py" },
        { "Python", "/Library/Frameworks/Python.framework/Versions/3.13/Resources/Python.app/Contents/MacOS/Python /srv/deploy.py", "deploy.py" },
    };
    CHECK(checkAll(rows));
}

// Terminal's set stops at 1999, so `node /srv/serve.js` reads there as the
// interpreter and a full path. The set here has heard of the last two decades.
TEST(testTheSetHasHeardOfTheLastTwoDecades)
{
    static constexpr Row rows[] {
        { "node", "node /srv/serve.js", "serve.js" },
        { "bun", "bun /srv/serve.ts", "serve.ts" },
        { "nu", "nu /srv/task.nu", "task.nu" },
        { "swift", "swift /srv/tool.swift", "tool.swift" },
        { "php", "php /srv/site.php", "site.php" },
        { "lua", "lua /srv/task.lua", "task.lua" },
        // A subcommand is not a script: `run` is neither a path nor a filename.
        { "deno", "deno run /srv/serve.ts", "serve.ts" },
    };
    CHECK(checkAll(rows));
}

// A script run directly execs the interpreter named in its shebang, so the
// kernel reports the INTERPRETER while argv[0] holds the script.
TEST(testAScriptRunByItsShebangIsNamedByItself)
{
    static constexpr Row rows[] {
        { "Python", "/srv/deploy.py", "deploy.py" },
        { "Python", "./deploy.py --verbose /etc/hosts", "deploy.py" },
        { "bash", "/srv/build.sh", "build.sh" },
    };
    CHECK(checkAll(rows));
}

// There is no script here, so the interpreter keeps its own name. `-c` and `-e`
// carry program TEXT, and space-splitting it can leave a token that looks like
// a filename — naming that would put a fragment of the reader's code in a tab.
TEST(testAnInterpreterWithNoScriptKeepsItsName)
{
    static constexpr Row rows[] {
        { "Python", "Python -c import time; time.sleep(90)", "Python" },
        { "perl", "perl -e print 1.0", "perl" },
        { "node", "node -e console.log(1.5)", "node" },
        { "python3", "python3", "python3" },
        { "zsh", "zsh", "zsh" },
    };
    CHECK(checkAll(rows));
}

// Everything that is not an interpreter keeps the name it had, including the
// leading dash that marks a login shell and the wrappers a session passes
// through on its way to one.
TEST(testAnythingElseKeepsTheNameItHad)
{
    static constexpr Row rows[] {
        { "-zsh", "-zsh", "-zsh" },
        { "login", "login -pf user", "login" },
        { "ssh", "ssh example-host", "ssh" },
        { "sleep", "sleep 5", "sleep" },
        { "vim", "vim deploy.py", "vim" },
        { "git", "git commit -m fix.me", "git" },
        // No command line at all: the name is all there is.
        { "python3", "", "python3" },
    };
    CHECK(checkAll(rows));
}

// An option that takes a VALUE swallows the token after it, and those values
// look like scripts more often than not: a load path has slashes, a warning
// filter has dots. Naming one puts a flag's argument in the titlebar where the
// script should be.
TEST(testAnOptionsValueIsNotMistakenForTheScript)
{
    static constexpr Row rows[] {
        { "ruby", "ruby -I /usr/lib task.rb", "task.rb" },
        { "python3", "python3 -W ignore::DeprecationWarning deploy.py", "deploy.py" },
        { "node", "node --require ./hook.js app.js", "app.js" },
        { "python3", "python3 -X importtime deploy.py", "deploy.py" },
        { "perl", "perl -I /opt/lib task.pl", "task.pl" },
    };
    CHECK(checkAll(rows));
}

// Program text attaches to its option as often as it follows it, and a split
// fragment of it can carry a slash or a dot like any path.
TEST(testAttachedProgramTextIsStillProgramText)
{
    static constexpr Row rows[] {
        { "perl", "perl -e'print \"a/b\"'", "perl" },
        { "python3", "python3 -c'import os.path'", "python3" },
    };
    CHECK(checkAll(rows));
}

// A version suffix is how these are installed and how a reader invokes them.
TEST(testAVersionedInterpreterIsStillAnInterpreter)
{
    static constexpr Row rows[] {
        { "python3.12", "python3.12 /srv/deploy.py", "deploy.py" },
        { "ruby3.3", "ruby3.3 task.rb", "task.rb" },
        { "php8", "php8 /srv/site.php", "site.php" },
        // A name that merely ends in digits is not a version of anything.
        { "log4j", "log4j /srv/x.conf", "log4j" },
    };
    CHECK(checkAll(rows));
}

UBIQUITTY_TEST_MAIN()
