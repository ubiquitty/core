// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// What a host remembers of its workspaces so that a restart of the host does
// not reduce one to an empty window: each workspace's windows, tabs and
// split trees, each pane's directory, launch, title, labels and grid. The host
// writes the records for one socket into one file and reads them back when it
// starts; a record is a recipe for fresh shells, never a picture of a screen.
//
// The file says which version it was written as, and each record says how
// long it is. A reader reads every version up to its own. One it does not know
// it refuses by name: `readHostLayoutFile` answers `NewerVersion` with the
// number, the host logs the file and the two versions and sets the file
// aside untouched, and nothing is restored from it. It is never read as a
// file that holds no record, because the next write would then replace it
// and the host would have forgotten every workspace it was recording. A file
// that does not read whole is refused the same way, which costs the
// arrangement and never a shell started from a misread field.

#include "Core/Session/Workspace.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::Session {

/// What a host records about one pane beyond its place in the arrangement.
///
/// The arrangement's leaf carries the pane's directory, its title and how it
/// was launched; these are the facts a leaf of a portable arrangement does
/// not hold.
struct HostLayoutPane {
    /// The pane's grid when it was recorded.
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    /// The pane's label, empty when it has none.
    std::string label;
    /// The label of the session behind the pane, empty when it has none.
    std::string sessionLabel;
    /// The tags a person put on the pane, in the order they are drawn.
    std::vector<std::string> tags;
    /// The tags a person put on the session behind the pane.
    std::vector<std::string> sessionTags;

    friend bool operator==(const HostLayoutPane&, const HostLayoutPane&) = default;
};

/// A daemon instance that held a recorded workspace live, and the id the
/// workspace had there: what a client attached to it then saved.
struct HostLayoutHolder {
    std::uint64_t daemonInstanceID = 0;
    /// Zero when the record that named the instance kept no id.
    std::uint32_t workspaceID = 0;

    friend bool operator==(const HostLayoutHolder&, const HostLayoutHolder&) = default;
};

/// One workspace as its host records it, named or not.
struct HostLayoutRecord {
    /// The windows, tabs and split trees, under the workspace's name. A leaf's
    /// origin is SSH with the `[user@]host` of a pane launched as a remote
    /// session, and Local for a pane that runs the host's shell.
    Workspace arrangement;
    /// The workspace's label, empty when it has none. Its name is in the
    /// arrangement: the name is what a HELLO reaches it by and is unique on
    /// the host, the label is what a person or an agent calls it.
    std::string workspaceLabel;
    /// The tags a person put on the workspace, in the order they are drawn.
    std::vector<std::string> workspaceTags;
    /// The id the host that wrote the record listed the workspace under,
    /// unique in its file. A host that starts lists the record under this id
    /// when no other takes it, so a workspace its writer held live comes back
    /// under the identity its clients saved.
    std::uint32_t workspaceID = 0;
    /// The daemon instances that held the workspace live, newest first, at
    /// most `maxHostLayoutHolders`, each with the id it held the workspace
    /// under. A host that took the record in under another id lists it there
    /// without being one of them, so the ids a client saved stay here; a
    /// client's saved reference names one of these pairs, see
    /// ``isRecordOfReference``.
    std::vector<HostLayoutHolder> holders;
    /// When a host last held the workspace live, in Unix seconds: what
    /// decides which record goes when a host holds more than it keeps.
    std::uint64_t heldUnixSeconds = 0;
    /// One label per window, in the arrangement's order; empty when unset.
    std::vector<std::string> windowLabels;
    /// One label per tab: windows in order, their tabs in order; empty when
    /// unset. A tab past the end has none.
    std::vector<std::string> tabLabels;
    /// The tags a person put on each window, in the arrangement's order. A
    /// window past the end has none.
    std::vector<std::vector<std::string>> windowTags;
    /// The tags a person put on each tab: windows in order, their tabs in
    /// order, as `tabLabels`. A tab past the end has none.
    std::vector<std::vector<std::string>> tabTags;
    /// One entry per leaf: windows in order, their tabs in order, and each
    /// tab's leaves first child first — the order `restoreFromSnapshot`
    /// reports them in.
    std::vector<HostLayoutPane> panes;
};

/// The most workspaces one file records.
inline constexpr std::size_t maxHostLayoutRecords = 64;

/// The most panes one record holds, which is the most sessions one host runs.
inline constexpr std::size_t maxHostLayoutPanes = 64;

/// The longest directory recorded, in bytes.
inline constexpr std::size_t maxHostLayoutDirectoryLength = 4096;

/// The longest pane or tab title recorded, in bytes.
inline constexpr std::size_t maxHostLayoutTitleLength = 1024;

/// The longest remote host string recorded, in bytes.
inline constexpr std::size_t maxHostLayoutRemoteHostLength = 255;

/// The largest file a host reads its records from, in bytes.
inline constexpr std::size_t maxHostLayoutFileBytes = 1024 * 1024;

/// The most daemon instances one record names as having held its workspace:
/// a client that slept through more host restarts than this restores by the
/// workspace's name instead.
inline constexpr std::size_t maxHostLayoutHolders = 4;

/// Whether a client's saved reference names the workspace a record was
/// written for.
///
/// A reference is the daemon instance that served the workspace and the id
/// the workspace had there, and one holder must name both: an id alone is
/// reused by every host that starts, an instance alone held every workspace
/// it served, and the id a record is listed under may be one a later host
/// gave it, which the instance never knew it by.
///
/// - Parameters:
///   - record: The record.
///   - daemonInstanceID: The instance the client saved, nonzero.
///   - workspaceID: The workspace id the client saved, nonzero.
/// - Returns: Whether restoring the record answers the reference.
[[nodiscard]] bool isRecordOfReference(const HostLayoutRecord& record, std::uint64_t daemonInstanceID, std::uint32_t workspaceID);

/// The holders a record names once `holder` holds its workspace.
///
/// - Parameters:
///   - former: The instances that held it before, newest first.
///   - holder: The instance that holds it now, with the id it holds it under.
/// - Returns: `holder`, then `former` without its instance, cut to
///   `maxHostLayoutHolders`.
[[nodiscard]] std::vector<HostLayoutHolder> holdersAfter(std::span<const HostLayoutHolder> former, HostLayoutHolder holder);

/// The finest a record's held time is kept to, in seconds. Coarser than a
/// second so a record whose workspace is merely in use is not rewritten for
/// the time alone; fine enough to rank which workspace was held longest ago.
inline constexpr std::uint64_t hostLayoutHeldResolutionSeconds = 3600;

/// The held time a record names once a host holds its workspace at `now`.
///
/// - Parameters:
///   - previous: The held time recorded before, zero for none.
///   - now: The current time, in Unix seconds.
/// - Returns: `now` once it is a resolution past `previous` (or before it,
///   after the clock moved back), else `previous`.
[[nodiscard]] constexpr std::uint64_t heldTimeAfter(std::uint64_t previous, std::uint64_t now)
{
    return !previous || now < previous || now - previous >= hostLayoutHeldResolutionSeconds ? now : previous;
}

/// Whether two records name the same workspace: the same id, or the same
/// name when both have one. A file, and a host's list, hold each at most once.
///
/// - Parameters:
///   - a: One record.
///   - b: The other.
/// - Returns: Whether keeping both would offer one workspace twice.
[[nodiscard]] bool isSameRecordedWorkspace(const HostLayoutRecord& a, const HostLayoutRecord& b);

/// Orders records the way a host keeps them when it holds more than
/// `maxHostLayoutRecords`: the most recently held first, so cutting the
/// list's tail drops the least recently held. Records held at the same
/// second keep their order.
///
/// - Parameter records: The records, reordered in place.
void orderByRecentHold(std::span<HostLayoutRecord> records);

/// Counts the leaves of an arrangement, every window and tab included.
///
/// - Parameter arrangement: The arrangement to count.
/// - Returns: The number of panes it would restore.
[[nodiscard]] std::size_t hostLayoutPaneCount(const Workspace& arrangement);

/// Encodes a host's records as the bytes of its record file.
///
/// - Parameters:
///   - records: The records; every bound above must already hold, which is
///     what `decodeHostLayoutRecords` checks on the way back in.
///   - out: Receives the bytes, appended.
void encodeHostLayoutRecords(std::span<const HostLayoutRecord> records, std::vector<std::uint8_t>& out);

/// The version `encodeHostLayoutRecords` writes, and the only one a reader of
/// this build reads. A field added to a record takes the next number.
inline constexpr std::uint16_t hostLayoutFileVersion = 2;

/// What reading a record file came to.
enum class HostLayoutFileVerdict : std::uint8_t {
    /// The bytes open with no magic a record file opens with.
    NotARecordFile,
    /// Every record was read.
    Read,
    /// The file names a version above `hostLayoutFileVersion`. Nothing past
    /// the version was read.
    NewerVersion,
    /// The file names a version below `hostLayoutFileVersion`, which this
    /// build does not read in any other layout. Nothing past the version was
    /// read.
    OlderVersion,
    /// A record file of a version this build reads, whose bytes do not read
    /// whole: cut short, past a bound, carrying a byte no record accounts
    /// for, or naming one workspace twice.
    Damaged,
};

/// A record file as read.
struct HostLayoutFile {
    HostLayoutFileVerdict verdict = HostLayoutFileVerdict::NotARecordFile;
    /// The version the file names; zero for a file that names none.
    std::uint16_t version = 0;
    /// The records; empty unless the verdict is `Read`.
    std::vector<HostLayoutRecord> records;
};

/// Reads a host's record file.
///
/// Bounds-safe over bytes a person or another program may have written: every
/// count, length, depth and grid is checked before it sizes anything, each
/// record must carry a name or an id and name a workspace no other record
/// names, each record must use exactly the bytes its
/// length gives it, and every byte of the file must be accounted for.
///
/// - Parameter bytes: The file's contents.
/// - Returns: The records and `Read`, or why there are none. A caller that
///   holds the file must not treat any other verdict as an empty file.
[[nodiscard]] HostLayoutFile readHostLayoutFile(std::span<const std::uint8_t> bytes);

/// How many record files a host could not read are kept set aside beside
/// its own. Each is a build's or a person's to recover from, and a host that
/// meets one at every start must not fill the directory with them.
inline constexpr std::size_t maxUnreadHostLayoutFiles = 4;

/// Where a record file a host could not read is kept.
///
/// - Parameters:
///   - recordPath: The host's record file.
///   - age: How many files were set aside after it: zero for the newest,
///     below `maxUnreadHostLayoutFiles`.
/// - Returns: `<recordPath>.unread` for the newest, then `.unread.1`,
///   `.unread.2` and so on.
[[nodiscard]] inline std::string unreadHostLayoutFilePath(std::string_view recordPath, std::size_t age)
{
    std::string path(recordPath);
    path.append(".unread");
    if (age != 0)
        path.append("." + std::to_string(age));
    return path;
}

/// Says why a file was not read, for a log line that names the file.
///
/// - Parameter file: What `readHostLayoutFile` returned.
/// - Returns: A clause that follows "because", empty for a file that was
///   read.
[[nodiscard]] std::string describeUnreadHostLayoutFile(const HostLayoutFile& file);

/// The records of a file that reads whole.
///
/// - Parameter bytes: The file's contents.
/// - Returns: The records, or nothing for any verdict but `Read`; a caller
///   that must say why reads the file with `readHostLayoutFile`.
[[nodiscard]] std::optional<std::vector<HostLayoutRecord>> decodeHostLayoutRecords(std::span<const std::uint8_t> bytes);

} // namespace Ubiquitty::Session
