// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Platform/test_gated_file_read.cpp
//
// The hardening rules of the kitty graphics file transport, asserted against the
// shared implementation rather than one frontend's copy of it. test_macos_file_read
// covers the same ground through UBFileReadClient and its deferred-unlink service;
// this covers the layer underneath, where the rules themselves live, so a second
// arm adopting it inherits assertions rather than writing its own.
//
// The rules, each with a case below: a name not spelled inside a temporary
// directory is refused before it is opened; the name is opened O_NOFOLLOW so its
// final component cannot become a symlink; the DESCRIPTOR is required to be a
// regular file, which subsumes FIFO and device without a second name lookup to
// race; the gate is asked about the descriptor's own canonical path, not the
// caller's string, and admits only temporary directories whatever directory the
// pane reports; the read is bounded by the caller's buffer and never by st_size;
// and a file to be deleted is read, and deleted, only inside a temporary
// directory with the kitty t=t marker in its path, only when the name still has
// the device and inode that were read.
//
// The policy here is a local stand-in rather than a frontend's: what is under
// test is that the gate is CONSULTED and obeyed, and a shipped policy would tie
// these cases to one frontend's answer.

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "KittyGraphicsFixture.h"
#include "PAL/TerminalClients.h"
#include "Platform/POSIX/GatedFileRead.h"
#include "TerminalFixture.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Ubiquitty::POSIX::GatedFileDisposition;

// Allow or Deny for every capability, so a case says which it is by construction.
class FixedPolicy final : public Ubiquitty::CapabilityPolicy {
public:
    explicit FixedPolicy(Ubiquitty::CapabilityDecision decision)
        : m_decision(decision)
    {
    }
    [[nodiscard]] Ubiquitty::CapabilityDecision check(Ubiquitty::Capability, Ubiquitty::CapabilityOrigin, std::string_view) override
    {
        ++m_calls;
        return m_decision;
    }
    [[nodiscard]] int calls() const { return m_calls; }

private:
    Ubiquitty::CapabilityDecision m_decision;
    int m_calls = 0;
};

std::string temporaryDirectory()
{
    const char* directory = ::getenv("TMPDIR");
    std::string result = (directory && directory[0]) ? directory : "/tmp";
    if (result.back() != '/')
        result += '/';
    return result;
}

// The real path of `directory`, or empty where it does not resolve.
std::string resolved(const char* directory)
{
    if (!directory || !directory[0])
        return {};
    std::array<char, PATH_MAX> buffer {};
    if (!::realpath(directory, buffer.data()) || buffer[0] == '\0')
        return {};
    std::string result(buffer.data());
    while (result.size() > 1 && result.back() == '/')
        result.pop_back();
    return result;
}

// A writable directory outside every readable root, or empty where the machine
// offers none.
//
// The roots are the temporary directories and nothing else — and whether a
// directory is one of those is a question the gate already answers. Asking it rather than
// re-deriving the boundary is what keeps a fixture from disagreeing with the
// rule it exists to probe: a second copy of "is this under a temp root" would be
// free to be right about a case the gate gets wrong, and to agree with itself.
//
// $HOME is not that answer on its own: it is outside the roots on a developer's
// machine and is itself a temporary directory in the container this suite also
// runs in, which is why the path is chosen by the predicate, not by its name.
//
// The predicate is asked about the file, which is the subject the gate itself
// is asked about, rather than about the directory holding it. Containment here means
// strictly under a root, so a directory that is itself a root answers false —
// `/tmp` is not in a temporary directory, and everything written there is.
// Asking about the directory admitted `/tmp` as somewhere outside the roots and
// then wrote the probe into one.
std::string probePathOutsideTheReadableRoots()
{
    const std::array<const char*, 2> candidates { ::getenv("HOME"), "/var/tmp" };
    for (const char* candidate : candidates) {
        const std::string real = resolved(candidate);
        if (real.empty() || ::access(real.c_str(), W_OK) != 0)
            continue;
        const std::string probe = real + "/.ubiquitty-roots-probe";
        if (Ubiquitty::POSIX::pathInTemporaryDirectory(probe))
            continue;
        return probe;
    }
    return {};
}

// The directory component of `path`, for the arm that opens it through OSC 7.
std::string parentDirectory(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

// The marker the kitty protocol requires in a t=t file's path.
constexpr std::string_view temporaryTransportMarker = "tty-graphics-protocol";

// Writes every byte, retrying an interrupted or partial write, so a fixture
// file holds exactly what its case reads back.
bool writeAll(int fileDescriptor, std::string_view bytes)
{
    while (!bytes.empty()) {
        const ssize_t count = ::write(fileDescriptor, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

// A file in the temporary directory holding `contents`, named from `stem`.
// Empty on failure, a short write included; the caller unlinks.
std::string makeFileInTemporaryDirectory(std::string_view stem, std::string_view contents)
{
    std::string templatePath = temporaryDirectory() + std::string(stem) + "-XXXXXX";
    std::vector<char> buffer(templatePath.begin(), templatePath.end());
    buffer.push_back('\0');
    const int fileDescriptor = ::mkstemp(buffer.data());
    if (fileDescriptor < 0)
        return {};
    const bool isWhole = writeAll(fileDescriptor, contents);
    ::close(fileDescriptor);
    if (!isWhole) {
        ::unlink(buffer.data());
        return {};
    }
    return { buffer.data() };
}

// A temporary file a t=f may name: no marker, so no t=t may delete it.
std::string makeTemporaryFile(std::string_view contents)
{
    return makeFileInTemporaryDirectory("ubq-gatedread", contents);
}

// A temporary file a t=t may name, carrying the marker the kitty protocol
// requires before a terminal deletes one.
std::string makeTemporaryTransportFile(std::string_view contents)
{
    return makeFileInTemporaryDirectory("ubq-gatedread-tty-graphics-protocol", contents);
}

bool fileExists(const std::string& path)
{
    struct ::stat status {};
    return ::stat(path.c_str(), &status) == 0;
}

}

TEST(aRegularFileIsReadWhenThePolicyAllows)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    const std::string path = makeTemporaryFile("pixels!");
    CHECK(!path.empty());
    std::array<unsigned char, 64> outputBuffer {};
    Ubiquitty::POSIX::GatedFileIdentity identity;
    const int count = Ubiquitty::POSIX::readGatedFile(terminal, path.c_str(), outputBuffer, GatedFileDisposition::Keep, &identity);
    CHECK(count == 7);
    CHECK(std::memcmp(outputBuffer.data(), "pixels!", 7) == 0);
    // The gate was asked, rather than the read happening to succeed.
    CHECK(policy.calls() >= 1);
    // The identity names a real file, so a caller that will delete has what it needs.
    CHECK(identity.isValid());
    CHECK(!identity.baseName.empty());
    CHECK(identity.inode != 0);

    ::unlink(path.c_str());
}

TEST(anAbsentPolicyRefusesRatherThanAllowing)
{
    // effectGranted, not effectAllowed. This is the only gate on the path, so it
    // cannot take the permissive default a pre-gate elsewhere can afford.
    Ubiquitty::Terminal terminal;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    const std::string path = makeTemporaryFile("pixels!");
    CHECK(!path.empty());
    std::array<unsigned char, 64> outputBuffer {};
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, path.c_str(), outputBuffer, GatedFileDisposition::Keep) < 0);

    ::unlink(path.c_str());
}

TEST(aDenyingPolicyRefuses)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Deny);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;

    const std::string path = makeTemporaryFile("pixels!");
    CHECK(!path.empty());
    std::array<unsigned char, 64> outputBuffer {};
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, path.c_str(), outputBuffer, GatedFileDisposition::Keep) < 0);
    CHECK(policy.calls() >= 1);

    ::unlink(path.c_str());
}

TEST(aSymlinkIsNotFollowed)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    // The target is a perfectly readable file; what is refused is reaching it
    // through a name whose final component is a link, because that link can be
    // repointed between a caller's check and this open.
    const std::string target = makeTemporaryFile("secret");
    CHECK(!target.empty());
    const std::string link = target + "-link";
    CHECK(::symlink(target.c_str(), link.c_str()) == 0);

    std::array<unsigned char, 64> outputBuffer {};
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, link.c_str(), outputBuffer, GatedFileDisposition::Keep) < 0);
    // The target itself still reads, so the refusal is about the link and not
    // about the file being unreadable.
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, target.c_str(), outputBuffer, GatedFileDisposition::Keep) == 6);

    ::unlink(link.c_str());
    ::unlink(target.c_str());
}

TEST(aFifoIsRefusedByTheDescriptorCheck)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    const std::string path = temporaryDirectory() + "ubq-gatedread-fifo";
    ::unlink(path.c_str());
    // Asserted rather than skipped on. A temporary directory that cannot hold a
    // FIFO is an environment this case cannot run in, and saying so is more use
    // than passing without having tried: a skip here would report the descriptor
    // check as covered on exactly the platforms where it was not.
    CHECK(::mkfifo(path.c_str(), 0600) == 0);
    std::array<unsigned char, 64> outputBuffer {};
    // O_NONBLOCK is what stops this hanging on a reader-less FIFO; S_ISREG on the
    // descriptor is what refuses it.
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, path.c_str(), outputBuffer, GatedFileDisposition::Keep) < 0);
    ::unlink(path.c_str());
}

TEST(theReadIsBoundedByTheBufferAndNotBySize)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    const std::string contents(4096, 'x');
    const std::string path = makeTemporaryFile(contents);
    CHECK(!path.empty());
    std::array<unsigned char, 16> outputBuffer {};
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, path.c_str(), outputBuffer, GatedFileDisposition::Keep) == 16);

    ::unlink(path.c_str());
}

TEST(averifiedUnlinkDeletesTheFileThatWasRead)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    const std::string path = makeTemporaryTransportFile("frame");
    CHECK(!path.empty());
    std::array<unsigned char, 64> outputBuffer {};
    Ubiquitty::POSIX::GatedFileIdentity identity;
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, path.c_str(), outputBuffer, GatedFileDisposition::DeleteAfterReading, &identity) == 5);
    CHECK(identity.isValid());
    CHECK(fileExists(path));
    Ubiquitty::POSIX::verifiedUnlink(identity);
    CHECK(!fileExists(path));
}

TEST(averifiedUnlinkRefusesWhenTheNameHoldsADifferentFile)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    const std::string path = makeTemporaryTransportFile("frame");
    CHECK(!path.empty());
    std::array<unsigned char, 64> outputBuffer {};
    Ubiquitty::POSIX::GatedFileIdentity identity;
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, path.c_str(), outputBuffer, GatedFileDisposition::DeleteAfterReading, &identity) == 5);

    // The name now holds a different file. Deleting by name would take the new
    // one; the device/inode check is what refuses.
    //
    // The impostor is created BESIDE the original and renamed over it, rather
    // than created after unlinking it: a filesystem that recycles the just
    // freed inode number (tmpfs and overlayfs both do) hands the replacement
    // the original's identity, and the check under test then rightly matches.
    // Overlapping their lifetimes is what makes the two identities distinct,
    // and the assertion below refuses to test anything until they are.
    const std::string impostorPath = path + ".impostor";
    const int replacement = ::open(impostorPath.c_str(), O_CREAT | O_RDWR, 0600);
    CHECK(replacement >= 0);
    struct ::stat originalStatus {};
    struct ::stat impostorStatus {};
    CHECK(::stat(path.c_str(), &originalStatus) == 0);
    CHECK(::fstat(replacement, &impostorStatus) == 0);
    CHECK(originalStatus.st_ino != impostorStatus.st_ino); // else the case proves nothing
    ::close(replacement);
    CHECK(::unlink(path.c_str()) == 0);
    CHECK(::rename(impostorPath.c_str(), path.c_str()) == 0);

    Ubiquitty::POSIX::verifiedUnlink(identity);
    CHECK(fileExists(path)); // the impostor survives

    ::unlink(path.c_str());
}

TEST(averifiedUnlinkRefusesOutsideATemporaryDirectory)
{
    // Containment: a name outside a temp directory is never deleted, so a hostile
    // t=t cannot turn "remove the temp file you just read" into removing
    // something the user cares about.
    const char* home = ::getenv("HOME");
    CHECK(home != nullptr && home[0] != '\0');
    if (!home || !home[0])
        return;
    // Marked, so the marker is not what refuses: containment is the subject.
    const std::string path = std::string(home) + "/.ubq-gatedread-outside-tty-graphics-protocol";
    const int fileDescriptor = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    CHECK(fileDescriptor >= 0);
    if (fileDescriptor < 0)
        return;
    CHECK(writeAll(fileDescriptor, "x"));
    struct ::stat status {};
    CHECK(::fstat(fileDescriptor, &status) == 0);
    ::close(fileDescriptor);

    Ubiquitty::POSIX::GatedFileIdentity identity;
    CHECK(Ubiquitty::POSIX::buildFileIdentity(path.c_str(), status.st_dev, status.st_ino, identity));
    Ubiquitty::POSIX::verifiedUnlink(identity);

    // Whether HOME is itself inside a temporary directory decides which way this
    // goes, and both ways are asserted rather than one being skipped. The Linux
    // container gives its build user /tmp as a home, so the second arm is the one
    // that runs there and the first is the one that runs on a developer machine —
    // a skip would have left whichever platform took the other branch reporting
    // this rule as covered without having exercised it.
    //
    // Anchored to HOME rather than the working directory because under ctest the
    // working directory is the build directory, whose location says nothing about
    // anything.
    if (Ubiquitty::POSIX::pathInTemporaryDirectory(path)) {
        CHECK(!fileExists(path)); // contained: the delete is permitted and happened
    } else {
        CHECK(fileExists(path)); // outside: refused, which is the rule under test
    }
    ::unlink(path.c_str());
}

TEST(aTemporaryPathIsContainedAndItsNeighbourIsNot)
{
    // Resolved first, because the containment prefixes are. On macOS $TMPDIR
    // sits under /var, which is a symlink to /private/var, so an unresolved
    // $TMPDIR prefix matches nothing and this case would report the containment
    // rule broken when what differed was the spelling. Production never hits
    // that: the path being tested comes from the opened descriptor and is
    // already canonical.
    std::array<char, PATH_MAX> resolved {};
    CHECK(::realpath(temporaryDirectory().c_str(), resolved.data()) != nullptr);
    std::string prefix(resolved.data());
    while (prefix.size() > 1 && prefix.back() == '/')
        prefix.pop_back();

    CHECK(Ubiquitty::POSIX::pathInTemporaryDirectory(prefix + "/a-file"));
    // A proper descendant only: the prefix itself is not inside itself, and a
    // sibling whose name merely starts with it is a different directory.
    CHECK(!Ubiquitty::POSIX::pathInTemporaryDirectory(prefix));
    CHECK(!Ubiquitty::POSIX::pathInTemporaryDirectory(prefix + "-elsewhere/a-file"));
}

/// A path outside the readable roots is refused, whatever the policy answers and
/// whatever directory the pane reports.
///
/// The capability decides which principal may read and cannot see which path it
/// named: a Kitty `t=f` carries the SESSION's origin, and a session is Local
/// whether the bytes came from the user's editor or from the far end of an ssh.
/// So the roots are a second question, and this is the case that separates them
/// — the policy allows, the origin is Local, and the read is refused for where
/// the path sits. Neither directory the pane knows widens them: OSC 7 is program
/// output, and the sampled kernel cwd under ssh is the local ssh client's.
TEST(aPathOutsideTheReadableRootsIsRefused)
{
    // The directory is chosen and then checked, rather than named and assumed.
    //
    // $HOME alone is a false assumption where this suite also runs: a container
    // sets HOME to /tmp, so a probe written there sits inside a readable root,
    // the read is allowed correctly, and the case reports that a path outside
    // the roots was admitted. A fixture whose subject is not
    // what it claims fails as the thing it was testing — the loudest possible
    // way to be wrong about a containment rule.
    //
    // So the failure below cannot be that: either the directory is outside every
    // root, or the case says it could not find one and names that instead.
    const std::string outside = probePathOutsideTheReadableRoots();
    CHECK_MSG(!outside.empty(),
        "the fixture found a writable place for the probe that is not inside a readable root");
    if (outside.empty())
        return;
    const std::string outsideDirectory = parentDirectory(outside);
    {
        const int descriptor = ::open(outside.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        CHECK(descriptor >= 0);
        if (descriptor >= 0) {
            CHECK(writeAll(descriptor, "pixels!"));
            ::close(descriptor);
        }
    }

    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    std::array<unsigned char, 64> outputBuffer {};
    Ubiquitty::POSIX::GatedFileIdentity identity;

    // The fixture asserted before the conclusion: this policy allows this
    // capability at this origin, so the refusal below is the roots, not the gate.
    CHECK(policy.check(Ubiquitty::Capability::FileReadAccess, Ubiquitty::CapabilityOrigin::Local, outside) == Ubiquitty::CapabilityDecision::Allow);
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, outside.c_str(), outputBuffer, GatedFileDisposition::Keep, &identity) < 0);

    // A temp file is a root by definition — where t=t lives and where every
    // image-transfer client stages — so it still reads.
    const std::string temporary = makeTemporaryFile("pixels!");
    CHECK(!temporary.empty());
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, temporary.c_str(), outputBuffer, GatedFileDisposition::Keep, &identity) == 7);
    ::unlink(temporary.c_str());

    // A report naming the probe's directory changes nothing. A remote host
    // behind ssh sends OSC 7 as easily as the local shell, so a root taken from
    // it is a root the sender picks — the daemon's file.read refuses one for
    // the same reason. The fixture is asserted first: the pane really is there.
    Ubiquitty::setReportedDirectoryString(terminal, outsideDirectory);
    CHECK_CSTR_EQ(terminal.currentDirectory.c_str(), outsideDirectory.c_str());
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, outside.c_str(), outputBuffer, GatedFileDisposition::Keep, &identity) < 0);

    // Nor does the sampled directory, which stands in once no report does.
    // Under ssh it is the local ssh client's own, usually the home directory,
    // and the far end needs no OSC 7 at all to have it.
    Ubiquitty::setReportedDirectoryString(terminal, "");
    Ubiquitty::setSampledDirectoryString(terminal, outsideDirectory);
    CHECK_CSTR_EQ(terminal.currentDirectory.c_str(), outsideDirectory.c_str());
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, outside.c_str(), outputBuffer, GatedFileDisposition::Keep, &identity) < 0);

    ::unlink(outside.c_str());
}

/// A file the caller will delete is refused unread without the t=t marker.
///
/// The kitty protocol lets a terminal delete a t=t file only when its path
/// carries `tty-graphics-protocol`, and without that rule t=t deletes any file
/// of the user's the sender can name in /tmp. Refusing the read as well as the
/// delete keeps a transfer from showing a file and leaving it behind, and the
/// before-read hook not running is what says nothing was read.
TEST(aFileToBeDeletedWithoutTheMarkerIsRefusedBeforeItIsRead)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    const std::string path = makeTemporaryFile("pixels!");
    CHECK(!path.empty());
    CHECK(path.find(temporaryTransportMarker) == std::string::npos);
    int hookCalls = 0;
    std::array<unsigned char, 64> outputBuffer {};
    Ubiquitty::POSIX::GatedFileIdentity identity;
    const Ubiquitty::POSIX::GatedFileReadHook countRead = [](void* context, const char*) { ++*static_cast<int*>(context); };
    const int count = Ubiquitty::POSIX::readGatedFile(terminal, path.c_str(), outputBuffer,
        GatedFileDisposition::DeleteAfterReading, &identity, countRead, &hookCalls);
    CHECK(count < 0);
    CHECK(hookCalls == 0);
    CHECK(!identity.isValid());
    CHECK(outputBuffer[0] == 0);

    // The backstop: handed the file's own identity, the delete refuses too.
    std::array<char, PATH_MAX> canonical {};
    struct ::stat status {};
    CHECK(::realpath(path.c_str(), canonical.data()) != nullptr);
    CHECK(::stat(path.c_str(), &status) == 0);
    Ubiquitty::POSIX::GatedFileIdentity sameFile;
    CHECK(Ubiquitty::POSIX::buildFileIdentity(canonical.data(), status.st_dev, status.st_ino, sameFile));
    Ubiquitty::POSIX::verifiedUnlink(sameFile);
    CHECK(fileExists(path));

    // Kept rather than deleted, the same temp file reads: it is a t=f.
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, path.c_str(), outputBuffer, GatedFileDisposition::Keep) == 7);
    ::unlink(path.c_str());
}

/// Whether a name lies in a temporary directory is answered on the name, before
/// anything opens it, and agrees with the containment asked of the descriptor.
TEST(aNameIsCheckedAgainstTheTemporaryDirectoriesBeforeItIsOpened)
{
    std::string spelled = temporaryDirectory();
    while (spelled.size() > 1 && spelled.back() == '/')
        spelled.pop_back();
    const std::string real = resolved(spelled.c_str());
    CHECK(!real.empty());

    // As the environment spells it, and as it resolves.
    CHECK(Ubiquitty::POSIX::pathSpelledInTemporaryDirectory(spelled + "/a-file"));
    CHECK(Ubiquitty::POSIX::pathSpelledInTemporaryDirectory(real + "/a-file"));
    CHECK(Ubiquitty::POSIX::pathSpelledInTemporaryDirectory(spelled + "//./a-file"));

    // Relative, climbing, the directory itself, and a sibling sharing its prefix.
    CHECK(!Ubiquitty::POSIX::pathSpelledInTemporaryDirectory("a-file"));
    CHECK(!Ubiquitty::POSIX::pathSpelledInTemporaryDirectory(""));
    CHECK(!Ubiquitty::POSIX::pathSpelledInTemporaryDirectory(spelled + "/../etc/passwd"));
    CHECK(!Ubiquitty::POSIX::pathSpelledInTemporaryDirectory(spelled));
    CHECK(!Ubiquitty::POSIX::pathSpelledInTemporaryDirectory(spelled + "-elsewhere/a-file"));
    const std::string outside = probePathOutsideTheReadableRoots();
    if (!outside.empty())
        CHECK(!Ubiquitty::POSIX::pathSpelledInTemporaryDirectory(outside));
}

/// A name outside every temporary directory is never opened.
///
/// Opening is itself an effect: a name under a protected folder raises a
/// privacy prompt naming this application, one under an automount point mounts
/// it, and a FIFO's writer is released. The FIFO is the one a test can watch. A
/// writer blocked opening it stays blocked unless something opens the read end,
/// so a writer still waiting after the gate has answered means the gate never
/// opened the name.
TEST(aNameOutsideTheTemporaryDirectoriesIsNeverOpened)
{
    const std::string outside = probePathOutsideTheReadableRoots();
    CHECK_MSG(!outside.empty(),
        "the fixture found a writable place for the probe that is not inside a readable root");
    if (outside.empty())
        return;
    const std::string fifo = outside + "-fifo";
    ::unlink(fifo.c_str());
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);

    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    std::atomic<bool> didWriterOpen { false };
    std::thread writer([&fifo, &didWriterOpen] {
        const int descriptor = ::open(fifo.c_str(), O_WRONLY | O_CLOEXEC);
        if (descriptor >= 0) {
            didWriterOpen.store(true);
            ::close(descriptor);
        }
    });
    // Time for the writer to block in its open, so a read end opened by the
    // gate would be one it sees.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::array<unsigned char, 64> outputBuffer {};
    CHECK(Ubiquitty::POSIX::readGatedFile(terminal, fifo.c_str(), outputBuffer, GatedFileDisposition::Keep) < 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(!didWriterOpen.load());

    // Released here, by this test's own read end, so the writer can be joined.
    const int release = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    writer.join();
    if (release >= 0)
        ::close(release);
    ::unlink(fifo.c_str());
}

namespace {

/// The status of the newest kitty graphics reply, or empty when there is none.
std::string kittyReplyOutcome(Ubiquitty::Terminal& terminal)
{
    std::array<char, 512> outputBuffer {};
    const int written = UbiquittyTest::drain(terminal, outputBuffer);
    const std::string all(outputBuffer.data(), static_cast<std::size_t>(written > 0 ? written : 0));
    const std::size_t start = all.rfind("\033_G");
    if (start == std::string::npos)
        return {};
    const std::size_t semicolon = all.find(';', start);
    const std::size_t end = all.find("\033\\", start);
    if (semicolon == std::string::npos || end == std::string::npos || semicolon > end)
        return {};
    return all.substr(semicolon + 1, end - semicolon - 1);
}

std::string base64Text(const std::string& text)
{
    return UbiquittyTest::Kitty::base64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

/// The GTK arm's reader, over the shared gate: read, and delete a t=t inline.
class GatedFileReader final : public Ubiquitty::FileReadClient {
public:
    explicit GatedFileReader(Ubiquitty::Terminal& terminal)
        : m_terminal(terminal)
    {
    }
    int calls = 0;

    [[nodiscard]] int readFile(const char* path, bool shouldDelete, unsigned char* outputBuffer, int capacity) override // bool-ok: FileReadClient's signature
    {
        ++calls;
        Ubiquitty::POSIX::GatedFileIdentity identity;
        const int count = Ubiquitty::POSIX::readGatedFile(m_terminal, path,
            std::span<unsigned char>(outputBuffer, static_cast<std::size_t>(capacity)),
            shouldDelete ? GatedFileDisposition::DeleteAfterReading : GatedFileDisposition::Keep, &identity);
        if (count >= 0 && shouldDelete && identity.isValid())
            Ubiquitty::POSIX::verifiedUnlink(identity);
        return count;
    }
    [[nodiscard]] bool supportsTemporaryFileTransport() const override { return true; }

private:
    Ubiquitty::Terminal& m_terminal;
};

}

/// The kitty file transport end to end, through the parser: a directory OSC 7
/// names is not one a t=f may read from, a relative t=f is refused before any
/// reader sees it, and a t=t names a file the terminal deletes only when it
/// carries the marker — even after OSC 7 names "/".
TEST(aReportedDirectoryOpensNothingToTheKittyFileTransport)
{
    const std::string outside = probePathOutsideTheReadableRoots();
    CHECK_MSG(!outside.empty(),
        "the fixture found a writable place for the probe that is not inside a readable root");
    if (outside.empty())
        return;
    const std::string outsideDirectory = parentDirectory(outside);
    const std::string pixel("\xff\x00\x00", 3);
    {
        const int descriptor = ::open(outside.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        CHECK(descriptor >= 0);
        if (descriptor >= 0) {
            CHECK(writeAll(descriptor, pixel));
            ::close(descriptor);
        }
    }

    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(terminal);
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;
    GatedFileReader reader(terminal);
    terminal.fileReadClient = &reader;

    // The report stands — the fixture asserted before the conclusion.
    UbiquittyTest::Kitty::feed(terminal, "\033]7;file://remote.example" + outsideDirectory + "\033\\");
    CHECK_CSTR_EQ(terminal.currentDirectory.c_str(), outsideDirectory.c_str());

    // Absolute, in the reported directory: the reader is asked and the gate
    // refuses, with the one answer every failed read gets.
    UbiquittyTest::Kitty::feed(terminal, UbiquittyTest::Kitty::apc("a=q,f=24,s=1,v=1,i=1,t=f", base64Text(outside)));
    CHECK_STR_EQ(kittyReplyOutcome(terminal), "EBADF:file transport failed");
    CHECK(reader.calls == 1);

    // Relative to it: refused before any reader is asked.
    const std::string baseName = outside.substr(outside.rfind('/') + 1);
    UbiquittyTest::Kitty::feed(terminal, UbiquittyTest::Kitty::apc("a=q,f=24,s=1,v=1,i=1,t=f", base64Text(baseName)));
    CHECK_STR_EQ(kittyReplyOutcome(terminal), "EPERM:outside confinement");
    CHECK(reader.calls == 1);

    // A staged file in a temporary directory reads, so the refusals above are
    // about where the file is.
    const std::string staged = makeTemporaryFile(pixel);
    UbiquittyTest::Kitty::feed(terminal, UbiquittyTest::Kitty::apc("a=q,f=24,s=1,v=1,i=1,t=f", base64Text(staged)));
    CHECK_STR_EQ(kittyReplyOutcome(terminal), "OK");

    // OSC 7 naming "/" made every path a descendant of the root. A t=t on an
    // unmarked temp file is refused unread and the file survives; a marked one
    // is read and deleted.
    UbiquittyTest::Kitty::feed(terminal, "\033]7;file:///\033\\");
    const int callsBefore = reader.calls;
    UbiquittyTest::Kitty::feed(terminal, UbiquittyTest::Kitty::apc("a=T,f=24,s=1,v=1,i=2,t=t", base64Text(staged)));
    CHECK_STR_EQ(kittyReplyOutcome(terminal), "EPERM:outside confinement");
    CHECK(reader.calls == callsBefore);
    CHECK(fileExists(staged));
    const std::string marked = makeTemporaryTransportFile(pixel);
    UbiquittyTest::Kitty::feed(terminal, UbiquittyTest::Kitty::apc("a=T,f=24,s=1,v=1,i=3,t=t", base64Text(marked)));
    CHECK_STR_EQ(kittyReplyOutcome(terminal), "OK");
    CHECK(!fileExists(marked));

    terminal.fileReadClient = nullptr;
    ::unlink(staged.c_str());
    ::unlink(marked.c_str());
    ::unlink(outside.c_str());
}

namespace {

// Creates a shared-memory object holding `contents`, or an empty name on
// failure. The caller unlinks.
std::string makeSharedMemoryObject(std::string_view contents)
{
    const std::string name = "/ubq-gatedshm";
    ::shm_unlink(name.c_str());
    const int descriptor = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor < 0)
        return {};
    if (::ftruncate(descriptor, static_cast<off_t>(contents.size())) != 0) {
        ::close(descriptor);
        ::shm_unlink(name.c_str());
        return {};
    }
    if (!contents.empty()) {
        void* mapping = ::mmap(nullptr, contents.size(), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        if (mapping != MAP_FAILED) {
            std::memcpy(mapping, contents.data(), contents.size());
            ::munmap(mapping, contents.size());
        }
    }
    ::close(descriptor);
    return name;
}

bool sharedMemoryObjectExists(const std::string& name)
{
    const int descriptor = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (descriptor < 0)
        return false;
    ::close(descriptor);
    return true;
}

}

TEST(aSharedMemoryObjectIsReadWhenThePolicyAllows)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    const std::string name = makeSharedMemoryObject("SHMPIXELS");
    CHECK(!name.empty());
    if (name.empty())
        return;
    // Sized to the payload, which is how the transport is actually called: the
    // kitty sequence says how many bytes the image is, and that is the buffer.
    // An oversized buffer would not compare across platforms — Darwin reports an
    // shm object's st_size rounded up to a page (ftruncate(9) then reads back
    // 16384), while Linux tmpfs reports 9, so the count would differ by arm for
    // one object. Bounded by the caller, the two agree.
    std::array<unsigned char, 9> outputBuffer {};
    const int count = Ubiquitty::POSIX::readGatedSharedMemory(terminal, name.c_str(), outputBuffer,
        Ubiquitty::POSIX::SharedMemoryDisposition::Keep);
    CHECK(count == 9);
    CHECK(std::memcmp(outputBuffer.data(), "SHMPIXELS", 9) == 0);
    CHECK(policy.calls() >= 1);
    // Keep means keep: the object is still openable.
    CHECK(sharedMemoryObjectExists(name));

    ::shm_unlink(name.c_str());
}

TEST(aSharedMemoryReadIsBoundedByTheBuffer)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    // Never st_size: the writer can grow the object after it is measured, and on
    // Linux can shrink it too, which is why that arm reads rather than maps.
    const std::string name = makeSharedMemoryObject(std::string(4096, 'z'));
    CHECK(!name.empty());
    if (name.empty())
        return;
    std::array<unsigned char, 16> outputBuffer {};
    CHECK(Ubiquitty::POSIX::readGatedSharedMemory(terminal, name.c_str(), outputBuffer,
              Ubiquitty::POSIX::SharedMemoryDisposition::Keep)
        == 16);

    ::shm_unlink(name.c_str());
}

TEST(aDenyingPolicyRefusesSharedMemory)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Deny);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;

    const std::string name = makeSharedMemoryObject("SHMPIXELS");
    CHECK(!name.empty());
    if (name.empty())
        return;
    std::array<unsigned char, 64> outputBuffer {};
    CHECK(Ubiquitty::POSIX::readGatedSharedMemory(terminal, name.c_str(), outputBuffer,
              Ubiquitty::POSIX::SharedMemoryDisposition::Keep)
        < 0);
    CHECK(policy.calls() >= 1);
    // Refused before opening, so the object is untouched.
    CHECK(sharedMemoryObjectExists(name));

    ::shm_unlink(name.c_str());
}

TEST(aMalformedSharedMemoryNameIsRefusedBeforeTheSystemSeesIt)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    std::array<unsigned char, 64> outputBuffer {};
    const auto refuse = [&](const char* name) {
        return Ubiquitty::POSIX::readGatedSharedMemory(terminal, name, outputBuffer,
                   Ubiquitty::POSIX::SharedMemoryDisposition::Keep)
            < 0;
    };
    CHECK(refuse("no-leading-slash"));
    CHECK(refuse("/has/embedded/slash"));
    CHECK(refuse("/"));
    CHECK(refuse(""));
    CHECK(refuse("/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")); // past the 31-character shape
    // Refused by shape, so the policy is never consulted for a name that could
    // not be opened anyway — the validation is ahead of the gate on purpose.
    CHECK(policy.calls() == 0);
}

TEST(unlinkAfterReadingRemovesTheObject)
{
    FixedPolicy policy(Ubiquitty::CapabilityDecision::Allow);
    Ubiquitty::Terminal terminal;
    terminal.capabilityPolicy = &policy;
    terminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Local;

    const std::string name = makeSharedMemoryObject("FRAME");
    CHECK(!name.empty());
    if (name.empty())
        return;
    std::array<unsigned char, 5> outputBuffer {};
    CHECK(Ubiquitty::POSIX::readGatedSharedMemory(terminal, name.c_str(), outputBuffer,
              Ubiquitty::POSIX::SharedMemoryDisposition::UnlinkAfterReading)
        == 5);
    CHECK(!sharedMemoryObjectExists(name));
}

UBIQUITTY_TEST_MAIN()
