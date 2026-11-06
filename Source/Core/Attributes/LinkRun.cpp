// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Attributes/LinkRun.h"

#include "Core/Attributes/Hyperlinks.h"
#include <cctype>

#include "Core/Terminal.h"
#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/UTF8Decoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace Ubiquitty::Attributes::LinkRun {

namespace {

// The bytes a command line separates words on.
bool isSpaceByte(char byte) { return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' || byte == '\f'; }

} // namespace

namespace {

// URL-ish byte: the run characters a bare URL may contain. Excludes
// whitespace, controls, and the common trailing punctuation that
// shouldn't be part of the link.
inline bool urlRunByte(uint32_t ch)
{
    if (ch <= 0x20 || ch >= 0x7F)
        return false;
    switch (ch) {
    case '"':
    case '\'':
    case '<':
    case '>':
    case '`':
    case '{':
    case '}':
    case '|':
    case '\\':
    case '^':
        return false;
    default:
        return true;
    }
}

// Trailing bytes trimmed off a matched run (sentence punctuation that
// usually isn't part of the URL). Balanced parens are kept.
inline bool urlTrailingTrim(uint32_t ch)
{
    switch (ch) {
    case '.':
    case ',':
    case ';':
    case ':':
    case '!':
    case '?':
    case ')':
    case ']':
        return true;
    default:
        return false;
    }
}

// Does the byte run starting at `s[i]` begin a URL we recognize? On a
// Whether anything addressable follows the scheme, between `from` and `to`.
//
// One letter or digit is enough, and it need not come first: `file://` takes an
// empty authority, so the run that names a file begins with the path's own
// slash. What it rules out is a run made entirely of the scheme and
// punctuation — `https://`, `www.`, `https://.` — where there is no host, no
// path, and nothing a click could reach.
static bool urlRunNamesSomething(const std::uint32_t* row, int from, int to)
{
    for (int index = from; index < to; ++index) {
        const std::uint32_t c = row[index];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return true;
    }
    return false;
}

// hit, returns the scheme-run length (so the caller can extend the
// match); 0 otherwise. Recognizes https://, http://, file://, ftp://,
// ssh://, mailto:, and a bare www. host.
int urlSchemeLengthAt(const uint32_t* s, int n, int i)
{
    auto starts = [&](const char* lit) -> int {
        int k = 0;
        for (; lit[k] != 0; k++) {
            if (i + k >= n || s[i + k] != static_cast<uint32_t>(lit[k]))
                return 0;
        }
        return k;
    };
    int m;
    if ((m = starts("https://")))
        return m;
    if ((m = starts("http://")))
        return m;
    if ((m = starts("file://")))
        return m;
    if ((m = starts("ftp://")))
        return m;
    if ((m = starts("ssh://")))
        return m;
    if ((m = starts("mailto:")))
        return m;
    if ((m = starts("www.")))
        return m;
    return 0;
}

} // namespace

bool isURLRunByte(uint32_t ch) { return urlRunByte(ch); }

std::optional<ColumnSpan> urlMatchSpanAtColumn(const uint32_t* row, int n, int column, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    if (!row || !outputBuffer || outputCapacity < 1)
        return std::nullopt;
    if (column < 0 || column >= n)
        return std::nullopt;
    // Find every scheme start; for each, extend the run and test whether
    // it covers `column`. Bounded by the row length — O(n) per hover.
    for (int i = 0; i < n;) {
        int schemeLength = urlSchemeLengthAt(row, n, i);
        if (schemeLength == 0) {
            i++;
            continue;
        }
        // Extend through URL-run bytes.
        int j = i + schemeLength;
        while (j < n && urlRunByte(row[j]))
            j++;
        // A scheme names nothing on its own. `https://` and `www.` are
        // prefixes, and text that stops at one — or continues with punctuation
        // — has nothing behind it to open, so underlining it offers the reader
        // a click that can only fail.
        if (!urlRunNamesSomething(row, i + schemeLength, j)) {
            i = (j > i) ? j : i + 1;
            continue;
        }
        // Trim trailing sentence punctuation.
        while (j > i + schemeLength && urlTrailingTrim(row[j - 1]))
            j--;
        if (column >= i && column < j) {
            // Materialize the run. `www.` matches get an https:// prefix
            // so the opener gets an absolute URL.
            int o = 0;
            // `www.` is the only scheme the detector recognises that is four
            // long, and the only one arriving without one — so the length names
            // it, and the cases pin that no other scheme shares the length.
            bool bareWww = (schemeLength == 4);
            if (bareWww) {
                const char* pre = "https://";
                for (int k = 0; pre[k] != 0 && o < outputCapacity - 1; k++)
                    outputBuffer[o++] = pre[k];
            }
            // A run that does not fit is reported as no link at all. Stopping at
            // the capacity produces a shorter string that still looks like a URL
            // and still opens -- so a 450-character authorize URL cut at byte
            // 255 becomes a live click onto a different address. No link
            // is a visible absence; a truncated one is a wrong answer wearing the
            // right shape.
            const int needed = (j - i) + o;
            if (needed > outputCapacity - 1) {
                outputBuffer[0] = 0;
                return std::nullopt;
            }
            for (int k = i; k < j; k++)
                outputBuffer[o++] = static_cast<char>(row[k] & 0x7F);
            outputBuffer[o] = 0;
            // The columns went out before this test when they were
            // out-parameters, so a caller reading them on a miss got the span of
            // a match that produced no text.
            if (o == 0)
                return std::nullopt;
            return ColumnSpan { i, j - 1 };
        }
        i = j;
    }
    return std::nullopt;
}

namespace {

// Path-run byte: the conservative set a bare file path may contain. Letters,
// digits, and the path punctuation / . _ - ~ +. Everything else (whitespace,
// ':' line-suffix separator, quotes, shell metacharacters) bounds the run.
inline bool pathRunByte(uint32_t ch)
{
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
        return true;
    switch (ch) {
    case '/':
    case '.':
    case '_':
    case '-':
    case '~':
    case '+':
        return true;
    default:
        return false;
    }
}

} // namespace

std::optional<ColumnSpan> pathMatchSpanAtColumn(const uint32_t* row, int n, int column, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    if (!row || !outputBuffer || outputCapacity < 1)
        return std::nullopt;
    if (column < 0 || column >= n || !pathRunByte(row[column]))
        return std::nullopt;

    // Maximal path-byte run covering column.
    int i = column;
    while (i > 0 && pathRunByte(row[i - 1]))
        --i;
    int j = column + 1;
    while (j < n && pathRunByte(row[j]))
        ++j;
    // Trim a single trailing sentence period ("see src/foo.cpp.") without
    // eating a real ".." path tail.
    // The run always holds at least the hovered column: `i` never passes it and
    // `j` starts past it, and the trim above only fires when it would leave two
    // columns behind. So the span below is never empty.
    if (j - 1 > i && row[j - 1] == '.' && row[j - 2] != '.')
        --j;

    // Require a '/' (or a leading '~') and at least one alphanumeric, so a
    // slash-less word, a bare version like "1.2.3", or punctuation-only runs
    // don't read as paths. Filesystem existence (checked by the frontend) is
    // the real gate; this just keeps obvious non-paths out.
    bool hasSlash = false, hasAlphanumeric = false;
    for (int k = i; k < j; ++k) {
        uint32_t c = row[k];
        if (c == '/')
            hasSlash = true;
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            hasAlphanumeric = true;
    }
    if (!(hasSlash || row[i] == '~') || !hasAlphanumeric)
        return std::nullopt;

    // Refused rather than shortened, for the reason the URL half is: a truncated
    // path still names something, and /tmp/some/deep/path cut to /tmp/ opens a
    // real directory that is not the one under the pointer.
    if ((j - i) > outputCapacity - 1) {
        outputBuffer[0] = 0;
        return std::nullopt;
    }
    int o = 0;
    for (int k = i; k < j; ++k)
        outputBuffer[o++] = static_cast<char>(row[k] & 0x7F);
    outputBuffer[o] = 0;
    if (o == 0)
        return std::nullopt;
    return ColumnSpan { i, j - 1 };
}

Kind kindForURI(std::string_view uri)
{
    // The scheme is the run before the first ':' (RFC 3986). No ':' means a
    // schemeless / relative reference → Other.
    const std::size_t colon = uri.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return Kind::Other;
    std::string_view scheme = uri.substr(0, colon);
    auto schemeIs = [&](std::string_view name) {
        if (scheme.size() != name.size())
            return false;
        for (std::size_t i = 0; i < scheme.size(); ++i) {
            char c = scheme[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a'); // case-insensitive scheme
            if (c != name[i])
                return false;
        }
        return true;
    };
    if (schemeIs("mailto"))
        return Kind::Mailto;
    if (schemeIs("file"))
        return Kind::File;
    if (schemeIs("ubiquitty"))
        return Kind::Action;
    // Any other absolute scheme (http, https, ftp, ssh, ...) reads as a URL.
    return Kind::URL;
}

Run runAtColumn(int columns, int column, CodepointProbe codepointAt, LinkIDProbe linkIDAt, void* cellContext, LinkURIResolver linkURIForID, void* uriContext)
{
    Run run;
    run.valid = false;
    run.column0 = run.column1 = column;
    run.kind = Kind::Other;
    run.uri[0] = 0;
    if (columns <= 0 || column < 0 || column >= columns || !codepointAt || !linkIDAt)
        return run;

    // OSC 8 link id on the hovered cell wins: span every contiguous cell
    // sharing it, then resolve the id to its interned URI.
    uint16_t id = linkIDAt(cellContext, column);
    if (id != 0 && linkURIForID) {
        const char* uri = linkURIForID(uriContext, id);
        if (uri && uri[0] != 0) {
            int c0 = column;
            while (c0 > 0 && linkIDAt(cellContext, c0 - 1) == id)
                --c0;
            int c1 = column;
            while (c1 + 1 < columns && linkIDAt(cellContext, c1 + 1) == id)
                ++c1;
            int o = 0;
            for (; uri[o] != 0 && o < Ubiquitty::VT::maxLinkURILength - 1; ++o)
                run.uri[o] = uri[o];
            run.uri[o] = 0;
            run.column0 = c0;
            run.column1 = c1;
            run.kind = kindForURI(run.uri);
            run.valid = true;
            return run;
        }
    }

    // Regex detection over the row text. Build the row's codePoint buffer
    // from the cell probe, bounded to the column count.
    uint32_t rowCodePoints[2048];
    int n = columns;
    if (n > 2048)
        n = 2048;
    if (column >= n)
        return run;
    for (int x = 0; x < n; ++x) {
        uint32_t ch = codepointAt(cellContext, x);
        rowCodePoints[x] = (ch != 0) ? ch : ' ';
    }
    if (const std::optional<ColumnSpan> span = urlMatchSpanAtColumn(rowCodePoints, n, column, run.uri)) {
        run.column0 = span->first;
        run.column1 = span->last;
        run.kind = kindForURI(run.uri);
        run.valid = true;
    }
    return run;
}

namespace {

// Collapses `.`, `..` and repeated separators in an absolute path. Lexical, so
// a `..` crossing a symlink resolves the way the text reads rather than the way
// the filesystem would — the caller's existence check is what settles that.
std::string foldAbsolutePath(std::string_view path)
{
    std::vector<std::string_view> parts;
    std::size_t index = 0;
    while (index < path.size()) {
        const std::size_t next = path.find('/', index);
        const std::string_view part = path.substr(index, next == std::string_view::npos ? std::string_view::npos : next - index);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (next == std::string_view::npos)
            break;
        index = next + 1;
    }
    std::string folded;
    for (const std::string_view part : parts) {
        folded.push_back('/');
        folded.append(part);
    }
    return folded.empty() ? std::string { "/" } : folded;
}

} // namespace

std::string resolveFilePathCandidate(std::string_view candidate, std::string_view baseDirectory, std::string_view homeDirectory)
{
    if (candidate.empty())
        return {};
    std::string absolute;
    if (candidate == "~" || candidate.starts_with("~/")) {
        if (homeDirectory.empty())
            return {};
        absolute.append(homeDirectory);
        absolute.append(candidate.substr(1));
    } else if (candidate.starts_with('/')) {
        absolute.append(candidate);
    } else if (candidate.starts_with('~')) {
        // A `~user` form; see the header.
        return {};
    } else {
        if (baseDirectory.empty() || !baseDirectory.starts_with('/'))
            return {};
        absolute.append(baseDirectory);
        if (!absolute.ends_with('/'))
            absolute.push_back('/');
        absolute.append(candidate);
    }
    return foldAbsolutePath(absolute);
}

bool isLaunchableURLScheme(std::string_view uri)
{
    // The scheme is the run before the first ':' (RFC 3986: ALPHA *( ALPHA
    // / DIGIT / "+" / "-" / "." )). No ':' means a schemeless / relative
    // reference, which never launches.
    const std::size_t colon = uri.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    std::string_view scheme = uri.substr(0, colon);

    // The launchable allowlist, shared by every frontend so macOS and Linux
    // agree on one list: safe network / mail handlers plus file:// (a local
    // file or folder handed to the OS default handler). javascript:, data:, and
    // every other scheme fall through to false.
    //
    // This list does not decide whether a click needs a modifier. That is
    // [selection] osc8_click_opens, which both frontends read and which defaults
    // on: a plain click opens an OSC-8-stamped link of any scheme here but the
    // four that connect to a host (requiresLinkModifier), on macOS and Linux
    // alike. A regex-GUESSED link needs Cmd/Ctrl — the program never
    // said that text was a URL, the terminal did — except on macOS under mouse
    // reporting with links_override_reporting, where
    // Input::plainClickOpenableUnderReporting lets a plain click open a strict
    // http(s) guess. Linux opens no link while a program reports the mouse.
    //
    // Membership here means the OS may be asked to launch a scheme at all. It is
    // not a statement about modifiers, and reading it as one is what let the two
    // frontends drift apart while this comment described only one of them. Nor
    // is it permission to open a file: a file: target still goes through
    // localLinkAction.
    static const std::string_view allowedSchemes[] = {
        "http",
        "https",
        "file",
        "mailto",
        "ftp",
        "ftps",
        "ssh",
        "sftp",
    };
    for (std::string_view allowed : allowedSchemes) {
        if (scheme.size() != allowed.size())
            continue;
        bool match = true;
        for (std::size_t i = 0; i < scheme.size(); ++i) {
            char c = scheme[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a'); // case-insensitive scheme
            if (c != allowed[i]) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

namespace {

// Whether two names are equal under ASCII case folding, as a case-insensitive
// volume compares them.
bool namesEqualFoldingCase(std::string_view name, std::string_view lowercase)
{
    if (name.size() != lowercase.size())
        return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != lowercase[i])
            return false;
    }
    return true;
}

} // namespace

bool requiresLinkModifier(std::string_view uri)
{
    const std::size_t colon = uri.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    const std::string_view scheme = uri.substr(0, colon);
    for (const std::string_view connecting : { std::string_view("ssh"), std::string_view("sftp"), std::string_view("ftp"), std::string_view("ftps") }) {
        if (namesEqualFoldingCase(scheme, connecting))
            return true;
    }
    return false;
}

bool isSafeToAutoOpenFileExtension(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    const std::string_view name = (slash == std::string_view::npos) ? path : path.substr(slash + 1);

    // Whole names a repository holds that carry no extension of their own:
    // text a handler displays. Matched in full, so a `README` with `.sh`
    // after its name and `.envrc` are not among them, and the dotfiles match
    // by the name they are written under.
    static const std::string_view allowedNames[] = {
        "readme",
        "license",
        "licence",
        "copying",
        "notice",
        "authors",
        "changelog",
        "contributing",
        "makefile",
        "gnumakefile",
        "dockerfile",
        "containerfile",
        ".gitignore",
        ".gitattributes",
        ".editorconfig",
        ".env",
    };
    for (std::string_view allowed : allowedNames) {
        if (namesEqualFoldingCase(name, allowed))
            return true;
    }

    const std::size_t dot = name.find_last_of('.');
    // No extension, or a dotfile with nothing after the leading dot (".bashrc"
    // has no "extension" in this sense — name.find_last_of('.') == 0).
    if (dot == std::string_view::npos || dot == 0 || dot == name.size() - 1)
        return false;
    const std::string_view extension = name.substr(dot + 1);

    // Plain-text / source / config / log, images, video and PDF — types the
    // OS default handler only ever DISPLAYS or plays, never executes. The list
    // excludes every type with its own execution semantics: scripts (sh, py,
    // rb, pl, js, ...), .command, .app, .webloc, archives, installers, disk
    // images — and HTML/JS, SVG and XML, which a browser default handler would
    // actually RUN rather than display. Safari claims .xml, and runs the
    // XHTML-namespaced script in one under a file: origin.
    //
    // Audio is left off although nothing runs it: macOS hands mp3, m4a and
    // wav to Music, which imports what it opens into the reader's library, so
    // a click would leave the program's file there after the window closed.
    static const std::string_view allowedExtensions[] = {
        "txt",
        "md",
        "markdown",
        "log",
        "json",
        "yaml",
        "yml",
        "toml",
        "ini",
        "cfg",
        "conf",
        "csv",
        "tsv",
        "c",
        "h",
        "cc",
        "cpp",
        "cxx",
        "hpp",
        "hxx",
        "m",
        "mm",
        "swift",
        "rs",
        "go",
        "java",
        "css",
        "diff",
        "patch",
        "gitignore",
        "env",
        "png",
        "jpg",
        "jpeg",
        "gif",
        "heic",
        "webp",
        "tif",
        "tiff",
        "bmp",
        "avif",
        "ico",
        "pdf",
        "mp4",
        "mov",
    };
    for (std::string_view allowed : allowedExtensions) {
        if (namesEqualFoldingCase(extension, allowed))
            return true;
    }
    return false;
}

namespace {

// One step of a walk over link text: a scalar, or the maximal subpart of a
// sequence that is not well-formed UTF-8.
struct LinkTextUnit {
    std::size_t length;
    bool isScalar;
    std::uint32_t scalar;
};

// The unit at the head of a nonempty text. The byte that breaks a sequence is
// left to begin the next unit, so a lead byte after a truncated sequence is
// still read as the start of its own.
LinkTextUnit linkTextUnitAt(std::string_view text)
{
    Unicode::UTF8Decoder decoder;
    Unicode::utf8Reset(&decoder);
    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t codePoint = 0;
        switch (Unicode::utf8Feed(&decoder, static_cast<std::uint8_t>(text[index]), &codePoint)) {
        case Unicode::UTF8Status::Need:
            continue;
        case Unicode::UTF8Status::Emit:
            return { index + 1, true, codePoint };
        case Unicode::UTF8Status::Invalid:
            return { index == 0 ? 1 : index, false, 0 };
        }
    }
    return { text.size(), false, 0 };
}

// Whether a scalar is one `hasDeceptiveLinkCharacters` names.
bool isDeceptiveLinkScalar(std::uint32_t scalar)
{
    if (scalar <= 0x1F || (scalar >= 0x7F && scalar <= 0x9F))
        return true;
    if (scalar == 0x061C || scalar == 0x200B || scalar == 0x200E || scalar == 0x200F)
        return true;
    if ((scalar >= 0x202A && scalar <= 0x202E) || (scalar >= 0x2066 && scalar <= 0x2069))
        return true;
    return scalar == 0x2028 || scalar == 0x2029 || scalar == 0x2060 || scalar == 0xFEFF;
}

// An inclusive run of scalars.
struct ScalarRange {
    std::uint32_t first;
    std::uint32_t last;
};

// Whether a scalar falls in one of the ranges.
bool isInScalarRanges(std::uint32_t scalar, std::span<const ScalarRange> ranges)
{
    return std::ranges::any_of(ranges, [scalar](const ScalarRange range) { return scalar >= range.first && scalar <= range.last; });
}

// Whether a scalar draws as nothing, so a target could carry it unseen: a
// Default_Ignorable_Code_Point, or an interlinear annotation character, which
// hides the text between two of them. The joiners and the variation selectors
// (isJoinerOrSelectorScalar) are left out: they shape the emoji and the
// letters beside them, and spelling them out there would take a name apart.
bool isInvisibleLinkScalar(std::uint32_t scalar)
{
    static constexpr ScalarRange invisible[] = {
        { 0x00AD, 0x00AD },
        { 0x034F, 0x034F },
        { 0x061C, 0x061C },
        { 0x115F, 0x1160 },
        { 0x17B4, 0x17B5 },
        { 0x180E, 0x180E },
        { 0x200B, 0x200B },
        { 0x200E, 0x200F },
        { 0x202A, 0x202E },
        { 0x2060, 0x206F },
        { 0x3164, 0x3164 },
        { 0xFEFF, 0xFEFF },
        { 0xFFA0, 0xFFA0 },
        { 0xFFF0, 0xFFFB },
        { 0x1BCA0, 0x1BCA3 },
        { 0x1D173, 0x1D17A },
        { 0xE0000, 0xE00FF },
        { 0xE01F0, 0xE0FFF },
    };
    return isInScalarRanges(scalar, invisible);
}

// Whether a scalar is a joiner or a variation selector: the zero-width
// non-joiner and joiner (U+200C, U+200D), the Mongolian free variation
// selectors (U+180B-U+180D, U+180F) and the variation selectors
// (U+FE00-U+FE0F, U+E0100-U+E01EF). Each draws as nothing and acts only on
// the characters beside it.
bool isJoinerOrSelectorScalar(std::uint32_t scalar)
{
    static constexpr ScalarRange joinersAndSelectors[] = {
        { 0x180B, 0x180D },
        { 0x180F, 0x180F },
        { 0x200C, 0x200D },
        { 0xFE00, 0xFE0F },
        { 0xE0100, 0xE01EF },
    };
    return isInScalarRanges(scalar, joinersAndSelectors);
}

// Whether a joiner or a variation selector beside this scalar can act on it,
// so that one there may be how a name is spelled: an emoji, or a letter of any
// script but the few written without them.
//
// Those few are ASCII; the Latin, Greek, Cyrillic, Armenian and Georgian
// letters and their combining marks; and the punctuation, spaces and currency
// signs that set them. A pictograph among them (the copyright and registered
// signs, U+203C, U+2049) takes the emoji selector and can. So can every script
// not named, which keeps an unknown one as written. A character the plate
// spells out carries nothing a joiner could act on.
bool canJoinOrSelect(std::uint32_t scalar)
{
    static constexpr ScalarRange writtenWithoutThem[] = {
        { 0x0000, 0x036F }, // ASCII, Latin-1, Latin Extended-A and -B, IPA, modifier letters, combining diacritics
        { 0x0370, 0x058F }, // Greek and Coptic, Cyrillic and its supplement, Armenian
        { 0x10A0, 0x10FF }, // Georgian
        { 0x1C80, 0x1CBF }, // Cyrillic Extended-C, Georgian Extended
        { 0x1D00, 0x1FFF }, // phonetic extensions, combining diacritics supplement, Latin Extended Additional, Greek Extended
        { 0x2000, 0x206F }, // General Punctuation
        { 0x20A0, 0x20CF }, // Currency Symbols
        { 0x2C60, 0x2C7F }, // Latin Extended-C
        { 0x2DE0, 0x2DFF }, // Cyrillic Extended-A
        { 0x2E00, 0x2E7F }, // Supplemental Punctuation
        { 0xA640, 0xA69F }, // Cyrillic Extended-B
        { 0xA720, 0xA7FF }, // Latin Extended-D
        { 0xAB30, 0xAB6F }, // Latin Extended-E
        { 0xFB00, 0xFB06 }, // the Latin ligatures
    };
    if (isDeceptiveLinkScalar(scalar) || isInvisibleLinkScalar(scalar) || isJoinerOrSelectorScalar(scalar))
        return false;
    return Unicode::isExtendedPictographic(scalar) || !isInScalarRanges(scalar, writtenWithoutThem);
}

// Appends `\<marker>{XX…}`: the value in uppercase hex, at least two digits.
void appendEscape(std::string& output, char marker, std::uint32_t value)
{
    static constexpr std::string_view digits = "0123456789ABCDEF";
    std::array<char, 8> hex {};
    std::size_t count = 0;
    do {
        hex[count++] = digits[value & 0xF];
        value >>= 4;
    } while (value != 0 || count < 2);
    output.push_back('\\');
    output.push_back(marker);
    output.push_back('{');
    while (count > 0)
        output.push_back(hex[--count]);
    output.push_back('}');
}

} // namespace

bool hasDeceptiveLinkCharacters(std::string_view uri)
{
    for (std::size_t index = 0; index < uri.size();) {
        const LinkTextUnit unit = linkTextUnitAt(uri.substr(index));
        if (!unit.isScalar || isDeceptiveLinkScalar(unit.scalar))
            return true;
        index += unit.length;
    }
    return false;
}

std::string escapedDeceptiveLinkCharacters(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    // Whether the character before the current one is something a joiner or
    // a selector can act on; the start of the text is not.
    bool isAfterJoinable = false;
    for (std::size_t index = 0; index < text.size();) {
        const LinkTextUnit unit = linkTextUnitAt(text.substr(index));
        if (unit.isScalar && isJoinerOrSelectorScalar(unit.scalar)) {
            // A run of them is judged by the characters on either side of the
            // whole run, so two between ASCII letters cannot vouch for each
            // other. Kept when either side can be joined or selected; spelled
            // out when neither can, where they join nothing and hide.
            std::size_t end = index;
            LinkTextUnit next = unit;
            while (end < text.size()) {
                next = linkTextUnitAt(text.substr(end));
                if (!next.isScalar || !isJoinerOrSelectorScalar(next.scalar))
                    break;
                end += next.length;
            }
            const bool isBeforeJoinable = end < text.size() && next.isScalar && canJoinOrSelect(next.scalar);
            if (isAfterJoinable || isBeforeJoinable) {
                escaped.append(text.substr(index, end - index));
            } else {
                for (std::size_t inner = index; inner < end;) {
                    const LinkTextUnit selector = linkTextUnitAt(text.substr(inner));
                    appendEscape(escaped, 'u', selector.scalar);
                    inner += selector.length;
                }
            }
            index = end;
            continue;
        }
        isAfterJoinable = unit.isScalar && canJoinOrSelect(unit.scalar);
        if (!unit.isScalar) {
            for (std::size_t offset = 0; offset < unit.length; ++offset)
                appendEscape(escaped, 'x', static_cast<std::uint8_t>(text[index + offset]));
        } else if (isDeceptiveLinkScalar(unit.scalar) || isInvisibleLinkScalar(unit.scalar)) {
            appendEscape(escaped, 'u', unit.scalar);
        } else {
            escaped.append(text.substr(index, unit.length));
        }
        index += unit.length;
    }
    return escaped;
}

LocalLinkAction localLinkAction(std::string_view canonicalPath, const LocalFileFacts& facts)
{
    if (!facts.isDirectory && !facts.isPackage && !facts.isRegularFile)
        return LocalLinkAction::Refuse;
    // Each of these hands the opener something other than a file to display:
    // a program, one document whose owner decides, or another file or program
    // the object itself names.
    if (facts.isPackage || facts.isLaunchableType || facts.isAlias || facts.hasOwnOpener)
        return LocalLinkAction::Reveal;
    if (facts.isDirectory)
        return facts.isOpenedOutsideFileManager ? LocalLinkAction::Reveal : LocalLinkAction::Open;
    // The execute bit counts where it can mean something: not on a type the
    // opener only displays or plays, which it hands to a viewer whatever the
    // bit says, and not on a volume that keeps no permissions and reports
    // every file as executable.
    const bool doesExecuteBitCount = !facts.isDisplayOnlyType && !facts.isOnVolumeWithoutPermissions;
    const bool isInert = !(doesExecuteBitCount && facts.isExecutable) && isSafeToAutoOpenFileExtension(canonicalPath);
    return isInert ? LocalLinkAction::Open : LocalLinkAction::Reveal;
}

bool lineHandlerTakes(const LocalFileFacts& facts) { return facts.isRegularFile; }

Kind kindForLinkID(const Ubiquitty::Terminal* t, std::uint16_t linkID)
{
    if (!t || linkID == 0)
        return Kind::Other;
    // The ring is walked from its oldest entry rather than indexed: ids are
    // minted monotonically and slots recycle, so a slot's position says nothing
    // about which id lives in it.
    const Ubiquitty::Attributes::Hyperlinks::LinkTable* links = Ubiquitty::Attributes::Hyperlinks::table(t);
    for (int i = 0; links && i < t->linkCount; ++i) {
        const int slot = (t->linkHead + i) % Ubiquitty::VT::linkTableCapacity;
        if (links->ids[slot] == linkID)
            return kindForURI(links->uris[slot].data());
    }
    // An id whose entry has been recycled out of the ring. The cell carrying it
    // outlived the table, and a caller styling a link has nothing to do with the
    // difference between that and a link whose scheme it does not recognise.
    return Kind::Other;
}

std::string schemeOfURI(std::string_view uri)
{
    const std::size_t colon = uri.find(':');
    // Position zero is a colon with nothing before it, which names no scheme.
    if (colon == std::string_view::npos || colon == 0)
        return {};
    std::string scheme(uri.substr(0, colon));
    for (char& character : scheme)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return scheme;
}

HandlerArgv linkHandlerArgv(std::string_view commandTemplate, std::string_view uri)
{
    HandlerArgv argv;
    bool isURIPlaced = false;
    std::size_t index = 0;
    while (index < commandTemplate.size()) {
        // Runs of whitespace collapse: the template is a command line, and two
        // spaces in it never meant an empty argument.
        while (index < commandTemplate.size() && isSpaceByte(commandTemplate[index]))
            ++index;
        const std::size_t start = index;
        while (index < commandTemplate.size() && !isSpaceByte(commandTemplate[index]))
            ++index;
        if (start == index)
            break;
        const std::string_view word = commandTemplate.substr(start, index - start);
        if (word == "%s" || word == "%u") {
            argv.emplace_back(uri); // the whole URI, one argument
            isURIPlaced = true;
        } else if (word == "%%") {
            argv.emplace_back("%");
        } else {
            argv.emplace_back(word);
        }
    }
    if (argv.empty())
        return argv;
    // No placeholder means the reader wrote a program and expects the link, so
    // it goes on the end — still as one argument.
    if (!isURIPlaced)
        argv.emplace_back(uri);
    return argv;
}

HandlerArgv lineHandlerArgv(std::string_view commandTemplate, std::string_view canonicalPath, std::uint32_t line, std::uint32_t column)
{
    HandlerArgv argv;
    if (canonicalPath.empty() || line == 0)
        return argv;
    const std::string lineText = std::to_string(line);
    const std::string columnText = std::to_string(column == 0 ? 1 : column);
    bool isPathPlaced = false;
    std::size_t index = 0;
    while (index < commandTemplate.size()) {
        while (index < commandTemplate.size() && isSpaceByte(commandTemplate[index]))
            ++index;
        const std::size_t start = index;
        while (index < commandTemplate.size() && !isSpaceByte(commandTemplate[index]))
            ++index;
        if (start == index)
            break;
        const std::string_view word = commandTemplate.substr(start, index - start);
        // Each placeholder is replaced where it stands and the scan resumes
        // after what replaced it, so a path that itself reads `{line}` stays a
        // path.
        std::string argument;
        for (std::size_t position = 0; position < word.size();) {
            const std::string_view rest = word.substr(position);
            if (rest.starts_with("{path}")) {
                argument.append(canonicalPath);
                isPathPlaced = true;
                position += 6;
            } else if (rest.starts_with("{line}")) {
                argument.append(lineText);
                position += 6;
            } else if (rest.starts_with("{column}")) {
                argument.append(columnText);
                position += 8;
            } else {
                argument.push_back(word[position]);
                ++position;
            }
        }
        argv.push_back(std::move(argument));
    }
    if (argv.empty())
        return argv;
    if (!isPathPlaced)
        argv.emplace_back(canonicalPath);
    return argv;
}

} // namespace Ubiquitty::Attributes::LinkRun
