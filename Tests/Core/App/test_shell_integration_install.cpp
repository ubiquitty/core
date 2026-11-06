// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_shell_integration_install.cpp — what installing shell integration does
// to each shell startup file, and what the question before it says.
//
// The file is the reader's, so every case here is about what is kept as much
// as what is written: an install appends to a file without the block, rewrites
// the block in place in a file carrying an older one with every byte around it
// untouched, and leaves alone a file that already has it, a file that loads the
// script the way the guide shows, and a block whose closing marker is missing.
// An absent file is created only where no other file stands in for it.

#include "Harness.h"

#include "Core/App/ShellIntegrationInstall.h"

#include <string>
#include <string_view>

namespace {

using Ubiquitty::App::ShellIntegrationInstall::consentDetail;
using Ubiquitty::App::ShellIntegrationInstall::Edit;
using Ubiquitty::App::ShellIntegrationInstall::endMarker;
using Ubiquitty::App::ShellIntegrationInstall::managedBlock;
using Ubiquitty::App::ShellIntegrationInstall::planAbsentFile;
using Ubiquitty::App::ShellIntegrationInstall::planEdit;
using Ubiquitty::App::ShellIntegrationInstall::PlannedChanges;
using Ubiquitty::App::ShellIntegrationInstall::PlannedEdit;
using Ubiquitty::App::ShellIntegrationInstall::scriptName;
using Ubiquitty::App::ShellIntegrationInstall::Shell;
using Ubiquitty::App::ShellIntegrationInstall::startMarker;
using Ubiquitty::App::ShellIntegrationInstall::StartupFile;
using Ubiquitty::App::ShellIntegrationInstall::startupFiles;

const StartupFile zshrc { ".zshrc", Shell::Zsh, true };
const StartupFile bashProfile { ".bash_profile", Shell::Bash, false };

int occurrences(std::string_view haystack, std::string_view needle)
{
    int count = 0;
    for (std::size_t position = haystack.find(needle); position != std::string_view::npos;
        position = haystack.find(needle, position + needle.size()))
        ++count;
    return count;
}

// A managed block as an earlier build wrote it: the markers, and a body that
// is not this build's.
std::string olderBlock()
{
    std::string block(startMarker);
    block += "\n[ -n \"$UBIQUITTY_INTEGRATION_DIR\" ] && . \"$UBIQUITTY_INTEGRATION_DIR/zsh.sh\"\n";
    block += endMarker;
    return block;
}

} // namespace

TEST(eachBlockSourcesItsShellsScriptOnlyWhenUbiquittyStartedTheShell)
{
    for (const Shell shell : { Shell::Zsh, Shell::Bash, Shell::Fish }) {
        const std::string block = managedBlock(shell);
        CHECK(block.starts_with(startMarker));
        CHECK(block.ends_with(endMarker));
        CHECK_CONTAINS(block, "source \"$UBIQUITTY_INTEGRATION_DIR/" + scriptName(shell) + "\"\n");
        // The block only loads the script: what the integration does, the
        // `claude` wrapper included, is the script's, so a later release
        // changes it without editing the reader's files again.
        CHECK_NOT_CONTAINS(block, "claude");
        CHECK_NOT_CONTAINS(block, "export");
    }
    CHECK_CONTAINS(managedBlock(Shell::Zsh), "if [[ -n \"${UBIQUITTY_INTEGRATION_DIR-}\" && -r ");
    CHECK_CONTAINS(managedBlock(Shell::Fish), "if set -q UBIQUITTY_INTEGRATION_DIR; and test -r ");
    CHECK_STR_EQ(scriptName(Shell::Zsh), "zsh.sh");
    CHECK_STR_EQ(scriptName(Shell::Bash), "bash.sh");
    CHECK_STR_EQ(scriptName(Shell::Fish), "fish.fish");
}

TEST(theInstallVisitsTheStartupFilesInOrderAndCreatesOnlyTheSafeOnes)
{
    const auto files = startupFiles();
    REQUIRE(files.size() == 4);
    CHECK_STR_EQ(files[0].relativePath, ".zshrc");
    CHECK(files[0].shell == Shell::Zsh && files[0].isCreatedWhenAbsent);
    CHECK_STR_EQ(files[1].relativePath, ".bashrc");
    CHECK(files[1].shell == Shell::Bash && files[1].isCreatedWhenAbsent);
    // A new .bash_profile would stop a login bash from reading .profile.
    CHECK_STR_EQ(files[2].relativePath, ".bash_profile");
    CHECK(files[2].shell == Shell::Bash && !files[2].isCreatedWhenAbsent);
    CHECK_STR_EQ(files[3].relativePath, ".config/fish/config.fish");
    CHECK(files[3].shell == Shell::Fish && !files[3].isCreatedWhenAbsent);
    for (const StartupFile& file : files)
        CHECK_MSG(file.relativePath != ".profile", "a POSIX .profile is never handed a bash script");
}

TEST(anAbsentFileIsCreatedHoldingTheBlockOnlyWhereThatIsSafe)
{
    const PlannedEdit created = planAbsentFile(zshrc);
    CHECK(created.edit == Edit::Create);
    CHECK_STR_EQ(created.contents, managedBlock(Shell::Zsh) + "\n");

    const PlannedEdit absent = planAbsentFile(bashProfile);
    CHECK(absent.edit == Edit::LeftAbsent);
    CHECK(absent.contents.empty());
}

TEST(aFileWithoutTheBlockHasItAppendedAfterABlankLine)
{
    const std::string block = managedBlock(Shell::Zsh);

    const PlannedEdit empty = planEdit(zshrc, "");
    CHECK(empty.edit == Edit::Append);
    CHECK_STR_EQ(empty.contents, "\n" + block + "\n");

    const PlannedEdit terminated = planEdit(zshrc, "export EDITOR=vi\n");
    CHECK(terminated.edit == Edit::Append);
    CHECK_STR_EQ(terminated.contents, "export EDITOR=vi\n\n" + block + "\n");

    // A last line with no newline of its own gets one before the blank line,
    // so the block never joins the reader's last command.
    const PlannedEdit unterminated = planEdit(zshrc, "export EDITOR=vi");
    CHECK(unterminated.edit == Edit::Append);
    CHECK_STR_EQ(unterminated.contents, "export EDITOR=vi\n\n" + block + "\n");
}

TEST(anOlderBlockIsRewrittenInPlaceAndEverythingAroundItKept)
{
    const std::string before = "# mine, above\nalias ll='ls -l'\n";
    const std::string after = "\n# mine, below\nexport PATH=\"$HOME/bin:$PATH\"\n";
    const PlannedEdit planned = planEdit(zshrc, before + olderBlock() + after);
    CHECK(planned.edit == Edit::Upgrade);
    CHECK_STR_EQ(planned.contents, before + managedBlock(Shell::Zsh) + after);
    CHECK_EQ(occurrences(planned.contents, startMarker), 1);
}

TEST(theCurrentBlockIsLeftAsItIsAndAnInstallIsIdempotent)
{
    const PlannedEdit first = planEdit(zshrc, "export EDITOR=vi\n");
    const PlannedEdit second = planEdit(zshrc, first.contents);
    CHECK(second.edit == Edit::AlreadyCurrent);
    CHECK_STR_EQ(second.contents, first.contents);
    CHECK_EQ(occurrences(second.contents, startMarker), 1);

    const PlannedEdit afterCreation = planEdit(zshrc, planAbsentFile(zshrc).contents);
    CHECK(afterCreation.edit == Edit::AlreadyCurrent);
}

TEST(aFileThatSourcesTheScriptAsTheGuideShowsIsLeftAlone)
{
    const std::string plain = "[ -n \"$UBIQUITTY_INTEGRATION_DIR\" ] && source \"$UBIQUITTY_INTEGRATION_DIR/zsh.sh\"\n";
    const PlannedEdit planned = planEdit(zshrc, plain);
    CHECK(planned.edit == Edit::ReaderWritten);
    CHECK_STR_EQ(planned.contents, plain);

    const std::string braced = "source \"${UBIQUITTY_INTEGRATION_DIR}/zsh.sh\"\n";
    CHECK(planEdit(zshrc, braced).edit == Edit::ReaderWritten);

    // Another shell's script is not this file's integration.
    const PlannedEdit other = planEdit(zshrc, "source \"$UBIQUITTY_INTEGRATION_DIR/bash.sh\"\n");
    CHECK(other.edit == Edit::Append);
}

TEST(aBlockMissingItsClosingMarkerIsNeverGuessedAt)
{
    const std::string opened = "alias ll='ls -l'\n" + std::string(startMarker) + "\nsource x\nexport KEEP=1\n";
    const PlannedEdit planned = planEdit(zshrc, opened);
    CHECK(planned.edit == Edit::Malformed);
    CHECK_STR_EQ(planned.contents, opened);

    // A closing marker before the opening one closes nothing.
    const std::string reversed = std::string(endMarker) + "\n" + std::string(startMarker) + "\n";
    CHECK(planEdit(zshrc, reversed).edit == Edit::Malformed);
}

TEST(theQuestionNamesEveryFileWrittenAndEveryFileCreated)
{
    const std::string none = consentDetail({});
    CHECK_CONTAINS(none, "None of your shell startup files needs a change.");
    CHECK_CONTAINS(none, "xterm-ubiquitty terminfo entry");

    const PlannedChanges one { { ".zshrc", true } };
    const std::string single = consentDetail(one);
    CHECK_CONTAINS(single, "adds a block to ~/.zshrc that loads");
    CHECK_CONTAINS(single, "prompt marks, and the directory and host each shell is in.");
    CHECK_MSG(single.find("claude") == std::string::npos, "the integration wraps no one program");
    CHECK_CONTAINS(single, " ~/.zshrc does not exist yet and is created.");

    const PlannedChanges two { { ".zshrc", false }, { ".bashrc", false } };
    const std::string pair = consentDetail(two);
    CHECK_CONTAINS(pair, "adds a block to ~/.zshrc and ~/.bashrc that loads");
    CHECK_NOT_CONTAINS(pair, "does not exist");
    CHECK_NOT_CONTAINS(pair, "do not exist");

    const PlannedChanges three { { ".zshrc", true }, { ".bashrc", true }, { ".bash_profile", false } };
    const std::string triple = consentDetail(three);
    CHECK_CONTAINS(triple, "adds a block to ~/.zshrc, ~/.bashrc, and ~/.bash_profile that loads");
    CHECK_CONTAINS(triple, " ~/.zshrc and ~/.bashrc do not exist yet and are created.");
}

UBIQUITTY_TEST_MAIN()
