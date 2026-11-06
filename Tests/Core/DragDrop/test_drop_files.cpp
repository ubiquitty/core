// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "PAL/DropFiles.h"
#include "PAL/ReceivedDropFiles.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace Ubiquitty::PAL;

namespace {
struct Tree {
    std::filesystem::path path;
    Tree()
    {
        std::string pattern = (std::filesystem::current_path() / ".drop-files-test-XXXXXX").string();
        auto* directory = mkdtemp(pattern.data());
        CHECK(directory);
        if (directory)
            path = directory;
    }
    ~Tree()
    {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};

std::string contents(DropFiles& files, std::uint64_t token)
{
    std::array<char, 3> bytes;
    std::string result;
    while (true) {
        const auto part = files.read(token, bytes);
        CHECK(part.error == DropFileError::None);
        if (part.error != DropFileError::None || part.isComplete)
            return result;
        result.append(bytes.data(), part.size);
    }
}
}

TEST(droppedFileAndSymlinkReadsNeverFollowTheSymlink)
{
    Tree tree;
    std::ofstream(tree.path / "file") << "hello";
    std::filesystem::create_symlink("file", tree.path / "link");
    DropFiles files;
    CHECK(files.addPath((tree.path / "file").string()) == DropFileError::None);
    CHECK(files.addPath((tree.path / "link").string()) == DropFileError::None);
    const auto file = files.readRoot(1);
    CHECK(file.error == DropFileError::None);
    CHECK(file.kind == 0);
    CHECK(contents(files, file.token) == "hello");
    const auto link = files.readRoot(2);
    CHECK(link.kind == 1);
    CHECK(contents(files, link.token) == "file");
    CHECK(files.readRoot(3).error == DropFileError::NotFound);
}

TEST(directoryHandlesPinTheDirectoryAndClosingRevokesIt)
{
    Tree tree;
    std::filesystem::create_directory(tree.path / "directory");
    std::ofstream(tree.path / "directory" / "child") << "inside";
    DropFiles files;
    CHECK(files.addPath((tree.path / "directory").string()) == DropFileError::None);
    const auto directory = files.readRoot(1);
    CHECK(directory.kind >= 2);
    CHECK(contents(files, directory.token) == "child");
    std::filesystem::rename(tree.path / "directory", tree.path / "moved");
    std::filesystem::create_directory(tree.path / "directory");
    std::ofstream(tree.path / "directory" / "child") << "outside";
    const auto child = files.readChild(directory.kind, 1);
    CHECK(child.error == DropFileError::None);
    CHECK(contents(files, child.token) == "inside");
    files.closeDirectory(directory.kind);
    CHECK(files.readChild(directory.kind, 1).error == DropFileError::Invalid);
}

TEST(replacingAListedChildWithASymlinkReturnsItsTargetOnly)
{
    Tree tree;
    std::filesystem::create_directory(tree.path / "directory");
    std::ofstream(tree.path / "directory" / "child") << "old";
    std::ofstream(tree.path / "private") << "must not be read";
    DropFiles files;
    CHECK(files.addPath((tree.path / "directory").string()) == DropFileError::None);
    const auto directory = files.readRoot(1);
    CHECK(contents(files, directory.token) == "child");
    std::filesystem::remove(tree.path / "directory" / "child");
    std::filesystem::create_symlink("../private", tree.path / "directory" / "child");
    const auto child = files.readChild(directory.kind, 1);
    CHECK(child.kind == 1);
    CHECK(contents(files, child.token) == "../private");
}

TEST(remoteDirectoriesRequireEveryChildBeforeTheyArePublished)
{
    Tree tree;
    ReceivedDropFiles files(tree.path.string());
    CHECK(files.addRoot("result") == DropFileError::None);
    constexpr std::string_view names("file\0link\0empty", 15);
    CHECK(files.writeEntry(1, 0, 0, 9, std::span(names.data(), names.size()), DropEntryCompletion::More) == DropFileError::None);
    CHECK(files.writeEntry(1, 0, 0, 9, {}, DropEntryCompletion::Complete) == DropFileError::None);
    CHECK(!files.isComplete(1));
    constexpr std::string_view content = "payload";
    CHECK(files.writeEntry(1, 9, 1, 0, std::span(content.data(), content.size()), DropEntryCompletion::More) == DropFileError::None);
    CHECK(files.writeEntry(1, 9, 1, 0, {}, DropEntryCompletion::Complete) == DropFileError::None);
    constexpr std::string_view target = "file";
    CHECK(files.writeEntry(1, 9, 2, 1, std::span(target.data(), target.size()), DropEntryCompletion::More) == DropFileError::None);
    CHECK(files.writeEntry(1, 9, 2, 1, {}, DropEntryCompletion::Complete) == DropFileError::None);
    CHECK(files.writeEntry(1, 9, 3, -2, {}, DropEntryCompletion::Complete) == DropFileError::None);
    CHECK(files.isComplete(1));
    const auto root = std::filesystem::path(files.path(1));
    CHECK(std::filesystem::is_regular_file(root / "file"));
    CHECK(std::filesystem::read_symlink(root / "link") == "file");
    CHECK(std::filesystem::is_directory(root / "empty"));
}

TEST(remoteDirectoryNamesCannotEscapeTheirParent)
{
    Tree tree;
    for (const std::string_view name : { "..", ".", "../outside", "/outside", "one/two" }) {
        ReceivedDropFiles files(tree.path.string());
        CHECK(files.addRoot("result") == DropFileError::None);
        CHECK(files.writeEntry(1, 0, 0, 2, std::span(name.data(), name.size()), DropEntryCompletion::More) == DropFileError::None);
        CHECK(files.writeEntry(1, 0, 0, 2, {}, DropEntryCompletion::Complete) == DropFileError::Invalid);
        CHECK(files.path(1).empty());
    }
    CHECK(!std::filesystem::exists(tree.path / "outside"));
}

TEST(remoteFilesCannotUseADirectoryHandleFromAnotherRoot)
{
    Tree tree;
    ReceivedDropFiles files(tree.path.string());
    CHECK(files.addRoot("first") == DropFileError::None);
    CHECK(files.addRoot("second") == DropFileError::None);
    constexpr std::string_view name = "child";
    CHECK(files.writeEntry(1, 0, 0, 2, std::span(name.data(), name.size()), DropEntryCompletion::More) == DropFileError::None);
    CHECK(files.writeEntry(1, 0, 0, 2, {}, DropEntryCompletion::Complete) == DropFileError::None);
    CHECK(files.writeEntry(2, 2, 1, 0, {}, DropEntryCompletion::Complete) == DropFileError::Invalid);
}

TEST(remoteSymlinksCannotBeUsedAsParentDirectories)
{
    Tree tree;
    ReceivedDropFiles files(tree.path.string());
    CHECK(files.addRoot("link") == DropFileError::None);
    constexpr std::string_view target = "../outside";
    CHECK(files.writeEntry(1, 0, 0, 1, std::span(target.data(), target.size()), DropEntryCompletion::More) == DropFileError::None);
    CHECK(files.writeEntry(1, 0, 0, 1, {}, DropEntryCompletion::Complete) == DropFileError::None);
    CHECK(files.writeEntry(1, 1, 1, 0, {}, DropEntryCompletion::Complete) == DropFileError::Invalid);
    CHECK(!std::filesystem::exists(tree.path / "outside"));
}

UBIQUITTY_TEST_MAIN()
