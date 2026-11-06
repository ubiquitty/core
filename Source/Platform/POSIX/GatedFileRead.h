// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace Ubiquitty {
struct Terminal;
}

// Reading a file an escape sequence named, and deleting it afterwards, with the
// hardening the kitty graphics file transport (t=f regular, t=t temp) needs.
//
// This is an attacker-controlled path: the terminal program chooses the name, so
// every step assumes the name is hostile and the file behind it can change under
// the reader. Both platforms enforce the same rules:
//
//   - refuse a name not spelled inside a temporary directory before opening
//     it, because opening has effects of its own — a privacy prompt, an
//     automount — that a refusal afterwards cannot take back;
//   - open O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC — the final component cannot be
//     swapped for a symlink, and a FIFO cannot hang the caller;
//   - fstat the DESCRIPTOR and require S_ISREG, which subsumes FIFO, device and
//     symlink-to-special without a second name lookup to race against;
//   - gate the descriptor's own canonical path rather than the caller's string,
//     because the string can stop naming this file between the two, and admit
//     it only inside a temporary directory;
//   - read bounded by the caller's buffer, never by st_size, which the writer
//     can grow after it is read;
//   - read a file the caller will delete, and delete it, only when its
//     canonical path is inside a temporary directory and carries the
//     `tty-graphics-protocol` marker the kitty protocol requires of t=t;
//   - delete through a parent directory descriptor opened
//     O_DIRECTORY|O_NOFOLLOW, and only when the name still has the device and
//     inode that were read.
//
// It lives here, in the POSIX code both frontends already compile, rather than
// in either frontend: a second arm implementing these rules would be a second
// answer to each of them, and the ones that matter are the ones nobody notices
// being answered differently. The macOS and GTK arms both call it.
//
// The scheduling around it stays with the caller. macOS defers the unlink onto a
// serial queue to keep a ~72ms syscall trio off the ingest thread; the
// verification it defers is this file's, so moving the work cannot weaken it.

namespace Ubiquitty::POSIX {

/// What a gated read actually opened, for a caller that will delete it.
///
/// Captured while the descriptor is open, so a later unlink verifies against the
/// file that was read rather than whatever holds the name by then. Value-only on
/// purpose: a deferred unlink outlives the read and must not reach back into a
/// client or a Terminal that may be gone.
struct GatedFileIdentity {
    std::string canonicalPath;
    std::string parentDirectoryPath; // canonicalPath up to the last '/'
    std::string baseName; // canonicalPath after the last '/'
    dev_t device {};
    ino_t inode {};

    [[nodiscard]] bool isValid() const { return !canonicalPath.empty() && !baseName.empty(); }
};

/// Called once the opened file has been identified and gated, before it is read.
///
/// The seam macOS needs for its deferred unlink and nothing else does: a client
/// reusing one fixed path with O_TRUNC lets a previous frame's pending delete
/// target the bytes this read is about to return, and device and inode cannot
/// separate them because the inode is the same one. Cancelling the pending
/// delete here — after the path is known, before the read — is what closes that.
/// A caller that deletes inline has no pending queue and passes null.
using GatedFileReadHook = void (*)(void* context, const char* canonicalPath);

/// What the caller of a gated read will do with the file afterwards.
enum class GatedFileDisposition : std::uint8_t {
    /// Leave it where it is: a kitty t=f, or a t=t that is only being asked about.
    Keep,
    /// Delete it once read: a kitty t=t transmission. The read then also needs
    /// the temporary-file name the delete needs, so a file that could not be
    /// deleted is not read either.
    DeleteAfterReading,
};

/// Reads a file named by an escape sequence, gating on the descriptor opened.
///
/// Only files inside a temporary directory are read. The program naming the
/// file is not the user: a Kitty `t=f` carries the session's origin, and a
/// session is Local whether the bytes came from the user's editor or from the
/// far end of an ssh. Neither directory a pane knows can tell those apart.
/// What a shell reported with OSC 7, 633 or 1337 is program output, which a
/// remote host writes as easily as a local shell, and the foreground's kernel
/// cwd under ssh is the local ssh client's, usually the home directory. So
/// neither is a root, and the temporary directories, where every image
/// transfer client stages its files, are the whole grant.
///
/// - Parameters:
///   - terminal: Whose capability policy adjudicates the read. Asked through
///     ``effectGranted``, so no policy means refused rather than allowed.
///   - path: The name the sequence gave, absolute. It says what to open; what
///     the gate adjudicates is the descriptor's own canonical path, after the
///     name itself has been checked to lie inside a temporary directory.
///   - outputBuffer: Receives the bytes; its size bounds the read.
///   - disposition: Whether the caller will delete the file once read. A
///     file to be deleted must also pass ``pathNamesTemporaryTransportFile``.
///   - outIdentity: Receives what was opened, when non-null and the read
///     succeeded. A caller that will not delete may pass null.
///   - beforeRead: Invoked after the gate passes and before the first read.
///   - hookContext: Passed to `beforeRead`.
/// - Returns: The byte count read, or -1 when the file was refused at any step.
[[nodiscard]] int readGatedFile(Ubiquitty::Terminal& terminal, const char* path,
    std::span<unsigned char> outputBuffer, GatedFileDisposition disposition,
    GatedFileIdentity* outIdentity = nullptr, GatedFileReadHook beforeRead = nullptr, void* hookContext = nullptr);

/// What to do with a shared-memory object once it has been read.
enum class SharedMemoryDisposition : std::uint8_t {
    Keep,
    UnlinkAfterReading, // kitty t=s with the delete flag
};

/// Reads a POSIX shared-memory object an escape sequence named.
///
/// The kitty graphics shared-memory transport (t=s). Gated, bounded and
/// name-validated like ``readGatedFile``, and different from it in the one way
/// that matters: an shm object has no path, so there is no canonical path to
/// re-gate the opened descriptor against. The NAME is what the policy sees,
/// which is a weaker guarantee than the file transport's and is stated here
/// rather than left to be discovered — a name can be unlinked and recreated by
/// another process between the check and the open.
///
/// The two platforms read it differently, and each would be wrong the other way
/// round. Both facts were measured rather than assumed:
///
///   - Darwin: `read(2)` on an shm descriptor fails (ENXIO/ESPIPE), so mapping
///     is the only way in. That is safe there because `ftruncate` on an shm
///     object is one-shot — once a size has been observed the object cannot
///     shrink, so a mapped copy cannot touch a page that stops being backed.
///   - Linux: `read(2)` works, and mapping is unsafe. A second `ftruncate`
///     succeeds and shrinks the object, so a writer can pull the pages out from
///     under a mapped copy between the size being read and the bytes being
///     copied. Touching an unbacked page of a shared mapping raises SIGBUS, and
///     the writer here is whoever sent the escape sequence — a terminal that
///     mapped would hand any program a way to kill it. `read(2)` returns short
///     instead, which is a truncated image and not a crash.
///
/// - Parameters:
///   - terminal: Whose capability policy adjudicates the read.
///   - name: The object's name, as the sequence gave it. Validated to the POSIX
///     shape (a leading slash, no other, 2 to 31 characters) before anything
///     else, so a hostile name is refused rather than passed to the system.
///   - outputBuffer: Receives the bytes; its size bounds the read, never the
///     object's own size.
///   - disposition: Whether to unlink the name after a successful read.
/// - Returns: The byte count read, or -1 when refused at any step. Zero is a
///   valid answer for an object that was created and never sized.
[[nodiscard]] int readGatedSharedMemory(Ubiquitty::Terminal& terminal, const char* name,
    std::span<unsigned char> outputBuffer, SharedMemoryDisposition disposition);

/// Whether a canonical path sits inside a temporary directory of this process.
///
/// The containment boundary for every gated read and for a t=t delete, so a
/// hostile name cannot turn "show the image you staged" into reading a file
/// the user cares about, or "remove the temp file you just read" into removing
/// one. A proper descendant only: the '/' test is what stops `/tmpfoo` passing
/// as a child of `/tmp`.
[[nodiscard]] bool pathInTemporaryDirectory(std::string_view canonicalPath);

/// Whether a name, as written, lies inside a temporary directory.
///
/// The check made before a name is opened, so a name outside every temporary
/// directory is refused without the filesystem being touched. Lexical: the
/// name must be absolute, hold no `..` component, and start with a temporary
/// directory as this process spells it (`$TMPDIR`, `/tmp`) or as it resolves
/// (`/private/tmp`). It decides only what may be opened. The canonical path of
/// what was opened is still checked with ``pathInTemporaryDirectory``, which
/// is what a symlink inside a temporary directory cannot get past.
[[nodiscard]] bool pathSpelledInTemporaryDirectory(std::string_view path);

/// Whether a canonical path may be read and then deleted as a kitty t=t file.
///
/// Inside a temporary directory, and carrying `tty-graphics-protocol`
/// somewhere in the path, which is the name the kitty protocol tells a client
/// to give a temporary file and a terminal to require before deleting one.
/// The marker is what keeps a t=t from deleting any other file of the user's
/// that happens to sit in `/tmp`.
[[nodiscard]] bool pathNamesTemporaryTransportFile(std::string_view canonicalPath);

/// Deletes `baseName` under an open directory descriptor, if it is still the
/// same file.
///
/// The verification, in one place, for callers that already hold the parent
/// descriptor.
///
/// - Parameters:
///   - directoryDescriptor: The open parent, which must have been opened
///     `O_DIRECTORY|O_NOFOLLOW` so the delete cannot be redirected through a
///     symlink swapped in since.
///   - baseName: The name to delete within it.
///   - device: The device the read descriptor reported.
///   - inode: The inode it reported. Together with `device` this is what makes
///     the delete about a file rather than about a name.
void verifyAndUnlinkAt(int directoryDescriptor, const char* baseName, dev_t device, ino_t inode);

/// Deletes the file a gated read opened, verifying containment and identity.
///
/// Opens its own parent directory descriptor `O_DIRECTORY|O_NOFOLLOW`, so a
/// symlink swapped in between the read and the delete cannot redirect it. The
/// entry for a caller that deletes inline; one that defers holds the parent
/// descriptor itself and calls ``verifyAndUnlinkAt``.
///
/// - Parameter identity: What ``readGatedFile`` reported opening. Does nothing
///   when it is invalid or fails ``pathNamesTemporaryTransportFile``.
void verifiedUnlink(const GatedFileIdentity& identity);

/// Splits a canonical path into the parts a verified unlink needs.
///
/// - Returns: False when the path has no directory part or no base name, which
///   is a path no unlink should be attempted from.
[[nodiscard]] bool buildFileIdentity(const char* canonicalPath, dev_t device, ino_t inode, GatedFileIdentity& out);

} // namespace Ubiquitty::POSIX
