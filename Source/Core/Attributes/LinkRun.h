// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "Base/Types.h"
#include "Core/VT/Constants.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// LinkRun — portable computation of the hovered link run: the contiguous span of cells sharing a hovered
// cell's OSC 8 link id, or (when it has none) the regex-detected URL run covering the column. Both return
// the same region so a frontend paints one underline. Grid-layout-agnostic: reads cells through two
// caller probes (codePoint + link id by column). Host-tested.

namespace Ubiquitty {
struct Terminal;

namespace Attributes::LinkRun {

// Per-column probes over a row (`context` opaque). codepointAt returns the base codePoint (0/space blank);
// linkIDAt returns the OSC 8 link id (0 == no link).
/// Reads one column's code point, so the run walk needs no cell storage.
using CodepointProbe = uint32_t (*)(void* context, int column);
/// Reads one column's OSC 8 link id, 0 where the cell carries none.
using LinkIDProbe = uint16_t (*)(void* context, int column);

// Link category from a resolved URI's scheme, for distinct frontend decoration: Mailto (mailto:),
// File (file:// + resolved local paths), URL (any other absolute scheme incl. www), Other (schemeless/empty).
/// What a resolved link points at, which decides how it is styled and gated.
enum class Kind : uint8_t {
    Other = 0,
    URL,
    File,
    Mailto,
    /// The terminal's own `ubiquitty:` scheme — a control the app draws, such
    /// as a button on its Welcome screen, rather than a link to somewhere.
    /// Drawn as the text it is, with no link tint or underline, and never
    /// handed to an opener.
    Action,
};

// Classify a resolved URI into a Kind from its scheme. Pure bytes (no platform URL type), case-insensitive.
/// Categorises a resolved URI so the frontend can style or gate it.
///
/// - Parameter uri: The resolved link target.
/// - Returns: Which kind of link it is.
Kind kindForURI(std::string_view uri);

/// Whether a code point can sit inside a bare URL run.
///
/// Exposed so the row-join heuristic asks the same question the matcher does:
/// a row that ends in these bytes and a row that begins in them read as one
/// run, and a definition kept in two places would let the two drift.
///
/// - Parameter ch: The code point.
/// - Returns: True for the bytes a URL may contain.
[[nodiscard]] bool isURLRunByte(uint32_t ch);

// The computed run. `valid` false when no link covers the column. When valid, [column0, column1] is the
// inclusive span and `uri` the resolved NUL-terminated URI (interned OSC 8 URI or materialized regex match;
// `www.` hosts gain an https:// prefix). `kind` categorizes it.
/// One hovered link: the cells it covers, its resolved URI, and its kind.
struct Run {
    bool valid;
    int column0;
    int column1;
    Kind kind;
    char uri[Ubiquitty::VT::maxLinkURILength];
};

// Compute the link run covering `column` over a row of `columns` cells. `linkURIForID` resolves an OSC 8
// id to its interned URI (nullptr/empty for unknown). A non-empty link id spans every contiguous cell
// sharing it; otherwise the row text is scanned for a URL run covering `column`.
/// Resolves an OSC 8 link id to its interned URI, or null when unknown.
using LinkURIResolver = const char* (*)(void* uriContext, uint16_t id);

/// Returns the contiguous run of cells sharing the link under one column.
///
/// Probes rather than cells, so the same walk serves a live grid, a snapshot
/// and a test without any of them agreeing on a cell layout.
///
/// - Parameters:
///   - columns: How wide the row is.
///   - column: The column being hovered.
///   - codepointAt: Reads a column's code point.
///   - linkIDAt: Reads a column's OSC 8 link id.
///   - cellContext: Passed to both cell probes.
///   - linkURIForID: Resolves an id to its URI.
///   - uriContext: Passed to the resolver.
/// - Returns: The run, with an empty URI when the column carries no link.
Run runAtColumn(int columns, int column, CodepointProbe codepointAt, LinkIDProbe linkIDAt, void* cellContext, LinkURIResolver linkURIForID, void* uriContext);

// Regex-only span: scan codePoints for a URL run covering `column`. Writes the NUL-terminated run to `out`
// and the span to outColumn0/1. True on a hit. Shared by runAtColumn and exposed for the row-text path.
/// The columns a match covers, both inclusive.
///
/// One value rather than two out-parameters: the ends are what a frontend
/// underlines between, and a caller holding one of them has nothing to draw.
struct ColumnSpan {
    /// The span's first column.
    int first;
    /// The span's last column, inclusive.
    int last;
};

/// Finds a bare URL around a column by pattern, for text carrying no OSC 8.
///
/// - Parameters:
///   - row: The row's code points.
///   - n: How many entries `row` holds.
///   - column: The column being hovered.
///   - output: Receives the NUL-terminated URL, and carries its own capacity.
/// - Returns: The columns the URL covers, or nothing when none does.
[[nodiscard]] std::optional<ColumnSpan> urlMatchSpanAtColumn(const uint32_t* row, int n, int column, std::span<char> output);

// File-path span: a maximal path-byte run ([A-Za-z0-9] plus / . _ - ~ +) covering `column` that contains a
// '/' or starts with '~' (a slash-less word is too noisy). Trailing period trimmed. A TEXTUAL candidate
// only — does not touch the filesystem; the frontend confirms existence before underlining, so "n/a" etc.
// never underlines.
/// Finds a bare filesystem path around a column, for text carrying no OSC 8.
///
/// - Parameters:
///   - row: The row's code points.
///   - n: How many entries `row` holds.
///   - column: The column being hovered.
///   - output: Receives the NUL-terminated path, and carries its own capacity.
/// - Returns: The columns the path covers, or nothing when none does.
[[nodiscard]] std::optional<ColumnSpan> pathMatchSpanAtColumn(const uint32_t* row, int n, int column, std::span<char> output);

// Scheme-allowlist gate for opening a resolved URL through the OS handler. A clickable link is
// attacker-controllable (the program writes the escape), so only launch a scheme on this list, else a
// hostile sequence could open e.g. javascript: or data:. True only for an absolute scheme in
// {http, https, file, mailto, ftp, ftps, ssh, sftp} (case-insensitive); schemeless and every other
// scheme → false.
//
// `file` is on the list for a frontend with an OS handler to hand it to, and passing the list is not
// permission to open one: a file target still goes through `localLinkAction`, which decides from what
// is on disk whether it opens, is revealed in the file manager, or is refused. A frontend without an
// OS handler — a browser, where file:// is both unopenable and a path disclosure — narrows this
// further rather than treating the list as its whole policy.
/// Whether a URI's scheme may be handed to the system opener.
///
/// An allowlist rather than a denylist: a scheme nobody has considered is one
/// the terminal must not launch, because the opener will hand it to whichever
/// application claims it.
///
/// - Parameter uri: The resolved link target.
/// - Returns: `true` when the scheme is safe to open.
bool isLaunchableURLScheme(std::string_view uri);

/// Whether a link opens only with the link modifier held (Command on macOS, Ctrl on Linux), even one
/// the program declared.
///
/// `ssh`, `sftp`, `ftp` and `ftps` hand a connection to another program, to a host the program named:
/// Terminal or whichever client claims the scheme opens a session there, and Finder mounts an FTP
/// server's files. A plain click is how a reader focuses and selects, so the program's declaration is
/// not enough for these: the reader asks for the connection by holding the modifier. `http`, `https`,
/// `mailto` and `file` still open on a plain click, which `osc8_click_opens` governs.
///
/// - Parameter uri: The resolved link target.
/// - Returns: Whether a plain click leaves it alone.
[[nodiscard]] bool requiresLinkModifier(std::string_view uri);

/// The absolute path a bare file-path candidate names.
///
/// `pathMatchSpanAtColumn` finds text that looks like a path; this says what
/// path, which takes two things the text does not carry. A relative candidate
/// is relative to the pane's own directory — the reader is looking at output a
/// command printed, and the base a command prints paths against is the one it
/// is running in — and `~` is relative to a home directory only the host
/// knows. Both arrive as arguments so the rule can be read and tested without
/// either.
///
/// Lexical only: `.` and `..` collapse and repeated separators fold, and
/// nothing is looked up. The filesystem is the caller's, both because Core has
/// no business touching it and because a stat belongs off the model lock and
/// behind a modifier — never on the path every printed line takes.
///
/// A `~user` form is left alone rather than guessed at: expanding it needs the
/// password database. An unresolved path fails the caller's existence check
/// and opens nothing, which is the direction to fail in for a string an escape
/// sequence chose.
///
/// - Parameters:
///   - candidate: The text under the pointer.
///   - baseDirectory: The pane's directory, for a relative candidate.
///   - homeDirectory: The home directory, for a leading `~`.
/// - Returns: The absolute path, or empty when the candidate names none.
[[nodiscard]] std::string resolveFilePathCandidate(std::string_view candidate, std::string_view baseDirectory, std::string_view homeDirectory);

/// A URI's scheme, lowercased, or empty when it names none.
///
/// Lowercased because a scheme comparison is case-insensitive (RFC 3986),
/// which is what makes a `[link_handlers]` key match the link it was written
/// for however either was typed.
///
/// - Parameter uri: The resolved link target.
/// - Returns: The scheme without its colon, or empty.
[[nodiscard]] std::string schemeOfURI(std::string_view uri);

/// The argv of a launched link handler: the program, then its arguments.
///
/// Named because a frontend holds one, and a Swift caller can only hold a
/// container type that has a name.
using HandlerArgv = std::vector<std::string>;

/// The argv a user's `[link_handlers]` command runs for one URI.
///
/// The template is the reader's; the URI is whatever a program printed, so
/// it is attacker-controllable. The two are never merged into one string:
/// only the template is split into words, and each `%s` or `%u` word becomes
/// the entire URI as one argument. The URI is never split on whitespace and
/// never concatenated, so a link reading `x; rm -rf ~` arrives at the handler
/// as one argument that nothing parses.
///
/// A template naming no placeholder gets the URI appended, so a bare
/// `open -a Jira` still receives the link — still as one argument. `%%`
/// collapses to a literal `%`.
///
/// The caller runs argv[0] as a program directly, never through a shell.
///
/// - Parameters:
///   - commandTemplate: The reader's configured command.
///   - uri: The resolved link target.
/// - Returns: The argv, empty when the template names no program.
[[nodiscard]] HandlerArgv linkHandlerArgv(std::string_view commandTemplate, std::string_view uri);

/// The argv the reader's `[links] line_handler` runs to open one file at a line.
///
/// The template is split into words the way `linkHandlerArgv` splits one, and
/// `{path}`, `{line}` and `{column}` are replaced wherever they stand inside a
/// word, so `code --goto {path}:{line}:{column}` reaches the program as three
/// words, the last of them one argument. Nothing is ever handed to a shell.
///
/// What is substituted is never what a program printed. The path is the
/// canonical one the click judged, which is absolute, so a word it begins
/// cannot start with `-` and be read as an option; the line and column are
/// numbers. A link that names no column fills `{column}` with 1, so a
/// template written for both reads one either way.
///
/// A template naming no `{path}` gets the path appended as one argument, as
/// `linkHandlerArgv` appends its URI.
///
/// - Parameters:
///   - commandTemplate: The reader's configured command.
///   - canonicalPath: The file, its symbolic links resolved.
///   - line: The line, counted from 1.
///   - column: The column, counted from 1, or 0 when the link names none.
/// - Returns: The argv, empty when the template names no program or the path
///   or the line is missing.
[[nodiscard]] HandlerArgv lineHandlerArgv(std::string_view commandTemplate, std::string_view canonicalPath, std::uint32_t line, std::uint32_t column);

/// Whether a file's name is one the OS handler only displays or plays.
///
/// The name half of two decisions: what a click on a link to a local file may hand the opener
/// (`localLinkAction`, which also reads the file's type and permissions), and whether a file fetched
/// from a remote session opens or is revealed. The path is attacker-controllable, so the allowlist is of extensions the handler merely displays or
/// plays -- plain text, source, config, images, video, PDF -- and never of ones it can execute:
/// scripts, executables, `.app`, `.webloc`, archives, and HTML, SVG and XML, which a browser runs. A few
/// whole names a repository holds pass too, `README`, `LICENSE`, `Makefile`, `Dockerfile`,
/// `.gitignore` and `.env` among them, because they carry no extension to read. A caller reveals
/// anything else in Finder instead of opening it.
///
/// Audio (`mp3`, `m4a`, `wav`) is revealed too. Nothing runs it, but the Mac's handler for it is
/// Music, which imports what it opens into the reader's library, and a click is not consent to that.
///
/// The match is case-insensitive and exact, so `readme.txt.sh` matches `.sh` and falls through, and
/// so does a `README` with `.sh` after its name.
///
/// - Parameter path: The fetched file's path or name.
/// - Returns: Whether the name is one the handler only displays.
bool isSafeToAutoOpenFileExtension(std::string_view path);

/// Whether a link target carries a character that hides, reorders or breaks the text a reader checks.
///
/// A program writes both a link's visible text and its target, so reading the
/// target is the reader's one check on where a click goes. These characters
/// defeat that reading without changing what an opener receives:
///
/// - C0 and C1 controls and DEL, which a text view draws as nothing or as a
///   line break.
/// - The Arabic letter mark (U+061C), and the left-to-right and
///   right-to-left marks (U+200E, U+200F).
/// - The bidirectional embeddings and overrides (U+202A–U+202E) and isolates
///   (U+2066–U+2069), which reorder the text after them.
/// - The line and paragraph separators (U+2028, U+2029), which start a second
///   line.
/// - The zero-width space (U+200B), the word joiner (U+2060) and the
///   byte-order mark (U+FEFF), which pad invisibly.
///
/// Bytes that are not well-formed UTF-8 count too: no reader can be shown
/// what they say.
///
/// The zero-width non-joiner and joiner (U+200C, U+200D) are not among them.
/// They join or part the letters beside them and move nothing: an emoji
/// sequence is spelled with the joiner, and so are Sinhala and the Indic
/// conjuncts, and Persian writes the non-joiner inside words. A file target is
/// judged by what is on disk and a host by its own rules, so refusing either
/// would only refuse those names.
///
/// Asked of the target as the program wrote it, before any URL parser sees
/// it. A parser percent-encodes these characters, and the encoded target no
/// longer contains what was there to hide something.
///
/// - Parameter uri: The link target.
/// - Returns: Whether a click on it must open nothing.
[[nodiscard]] bool hasDeceptiveLinkCharacters(std::string_view uri);

/// A link target as a reader is shown it, with every deceptive character spelled out.
///
/// Each character `hasDeceptiveLinkCharacters` names becomes `\u{XXXX}` and
/// each byte that is not well-formed UTF-8 becomes `\x{XX}`, so a target
/// carrying one can neither hide it nor draw a second line. So does every
/// other character that draws as nothing (a Default_Ignorable_Code_Point,
/// such as the soft hyphen, the invisible operators U+2061-U+2064, the Hangul
/// fillers and the tag characters, and the interlinear annotation characters
/// U+FFF9-U+FFFB), which a click does not refuse but the reader must still be
/// able to see.
///
/// The joiners (U+200C, U+200D) and the variation selectors are kept where they
/// can join or select something, because the emoji and scripts spelled with
/// them would come apart: beside an emoji, or beside a letter of any script
/// but the few written without them. Where neither side can be joined, as
/// between two ASCII characters, they join nothing and are spelled out too,
/// so `notes<U+200D>.txt` cannot pass for `notes.txt`, the file it is not. A
/// run of them is judged by the characters around the whole run. Everything
/// else is kept as written.
///
/// - Parameter text: The target, or the path it names.
/// - Returns: The text to show.
[[nodiscard]] std::string escapedDeceptiveLinkCharacters(std::string_view text);

/// What a click on a link to a local file may do with it.
enum class LocalLinkAction : uint8_t {
    /// Hand the file to the system opener.
    Open UBIQUITTY_SWIFT_NAME(open),
    /// Select the file in the file manager and open nothing.
    Reveal UBIQUITTY_SWIFT_NAME(reveal),
    /// Do nothing.
    Refuse UBIQUITTY_SWIFT_NAME(refuse),
};

/// What a frontend found on disk where a file link points.
///
/// Read from the path after the frontend has resolved its symbolic links, so
/// the facts describe the object at the end of that path rather than the
/// spelling a program printed. Resolving a path does not see the two ways a
/// file can send the opener somewhere else, so those are facts of their own:
/// `isAlias` and `hasOwnOpener`. A path where nothing exists leaves every
/// field false.
struct LocalFileFacts {
    /// Whether it is a directory.
    bool isDirectory = false;
    /// Whether the platform opens it as one document, as macOS does a bundle.
    bool isPackage = false;
    /// Whether it is a regular file.
    bool isRegularFile = false;
    /// Whether this user may execute it.
    bool isExecutable = false;
    /// Whether the platform's type system calls it an application, an executable or a script, or a
    /// page a browser runs: HTML, XHTML or SVG.
    bool isLaunchableType = false;
    /// Whether the platform's type system calls it text, an image, audio or video, or a PDF:
    /// kinds the opener hands to a viewer whatever its permissions say.
    bool isDisplayOnlyType = false;
    /// Whether it sits on a volume that keeps no permissions and reports every file as executable,
    /// as exFAT, FAT, NTFS and SMB do.
    bool isOnVolumeWithoutPermissions = false;
    /// Whether the opener follows it to another file and opens that, as macOS does a Finder alias.
    bool isAlias = false;
    /// Whether it names the program that opens it in place of its type's, as a macOS Open With choice does.
    bool hasOwnOpener = false;
    /// Whether the opener hands this directory to a program other than the file manager, as macOS
    /// hands a folder named like a mailbox to Mail.
    bool isOpenedOutsideFileManager = false;
};

/// What a click on a link to a local file may do with it.
///
/// The system opener runs what it is handed if the file is a program: an
/// application launches, and a script or an executable opens in a terminal
/// and runs. A program chose the link's target, so the opener is handed only
/// what it can merely display.
///
/// - A directory that is not a package opens when the opener hands it to the
///   file manager, which shows its contents and runs nothing. One whose name
///   gives it a type another program claims (a `.mbox` folder opens in Mail)
///   is revealed: the name is the program's choice.
/// - A regular file opens only when it has no launchable type, a name
///   `isSafeToAutoOpenFileExtension` passes, and no execute permission that
///   counts. Each of the three misses what another sees: a name says nothing
///   about the permission, the permission nothing about a script the opener
///   hands to an interpreter, and a type nothing about a name the opener
///   reads first. The permission does not count on a display-only type,
///   which the opener hands to a viewer whatever the bit says, nor on a
///   volume without permissions, where every file reads as executable; a
///   program there still has its launchable type.
/// - A Finder alias, and a file that names its own opener, is revealed
///   whatever its name and type say. An alias opens its target, which nothing
///   here classified, and an Open With choice can name an interpreter, which
///   runs the text it is handed. Both travel in extended attributes, which an
///   unpacked archive keeps and nothing quarantines.
/// - Any other regular file, and any package, is revealed. The reader still
///   reaches it, and opening it is a decision they make in the file manager.
/// - Anything else is refused: a path where nothing exists, a device, a
///   socket or a pipe.
///
/// - Parameters:
///   - canonicalPath: The path with its symbolic links resolved, whose name the extension rule reads.
///   - facts: What the frontend found at that path.
/// - Returns: What the click may do.
[[nodiscard]] LocalLinkAction localLinkAction(std::string_view canonicalPath, const LocalFileFacts& facts);

/// Whether a link that names a line in a file goes to the reader's line handler.
///
/// A regular file does, whatever `localLinkAction` would have the system
/// opener do with it. The handler is the program the reader named for opening
/// a file at a line, an editor, and an editor shows a script or an executable
/// as text rather than running it, so the source file a search printed opens
/// at its match even where the system opener would only reveal it. A
/// directory, a package and anything that is not a file take
/// `localLinkAction`'s answer instead: a line names nothing in them.
///
/// That holds only while the handler is an editor. One that opens a file the
/// system's way (`open`, `xdg-open`, `gio open`) would run the script, so the
/// guide and the configuration template tell the reader to name an editor.
///
/// - Parameter facts: What the frontend found at the link's canonical path.
/// - Returns: Whether the line handler opens it.
[[nodiscard]] bool lineHandlerTakes(const LocalFileFacts& facts);

/// Classifies the link an OSC 8 id names, reading the terminal's own link ring.
///
/// The ring is a fixed table of live ids; an id absent from it — recycled, or
/// never minted — is `Kind::Other` rather than a lookup failure, because a
/// caller styling a link has nothing to do with the difference and the id came
/// off a cell that may outlive its entry.
///
/// A frontend rendering from a live `Terminal` calls this; one rendering from a
/// captured snapshot carries its own copy of the kinds, because the ring belongs
/// to the terminal and a capture cannot borrow it.
///
/// - Parameters:
///   - t: The terminal whose link ring resolves the id. Null yields `Other`.
///   - linkID: The id an OSC 8 cell carries. Zero means no link.
/// - Returns: The kind the id's URI classifies as, or `Other`.
[[nodiscard]] Kind kindForLinkID(const Ubiquitty::Terminal* t, std::uint16_t linkID);

} // namespace Attributes::LinkRun

} // namespace Ubiquitty
