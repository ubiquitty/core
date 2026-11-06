// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <vector>

namespace Ubiquitty::Diagnostics::RenderProfile {

/// Supplies parser input for complete and incrementally damaged render benchmarks.
struct Scene {
    const char* name;
    std::string feed;
    std::string damageUnit;
    int viewOffset = 0;
    bool hasSelection = false;
    bool hasBackgroundImage = false;
};

/// Repeats a nonempty fixture until it reaches the requested byte count.
///
/// - Parameters:
///   - unit: A nonempty parser-input fixture.
///   - targetBytes: The minimum output size, including complete repetitions.
/// - Returns: Repeated input without truncating escape sequences or UTF-8.
inline std::string repeatTo(const std::string& unit, std::size_t targetBytes)
{
    std::string out;
    out.reserve(targetBytes + unit.size());
    while (out.size() < targetBytes)
        out += unit;
    return out;
}

/// Builds text with changing foreground colors and rendition attributes.
///
/// - Parameter cells: The number of printable cells to generate.
/// - Returns: Parser input terminated with a rendition reset.
inline std::string variedPenFeed(std::size_t cells)
{
    static const char* const styles[] = { "0", "0;1", "0;2", "0;7", "0;1;2" };
    std::string out;
    out.reserve(cells * 18);
    for (std::size_t i = 0; i < cells; ++i) {
        out += "\x1b[";
        out += styles[i % 5];
        out += ";38;5;";
        out += std::to_string(1 + (i % 254));
        out += 'm';
        out += static_cast<char>('a' + (i % 26));
    }
    out += "\x1b[0m";
    return out;
}

/// Builds distinguishable history rows for scrollback rendering.
///
/// - Parameter lines: The nonnegative number of rows to generate.
/// - Returns: Colored, numbered lines terminated with carriage return and line feed.
inline std::string numberedHistoryFeed(int lines)
{
    std::string out;
    out.reserve(static_cast<std::size_t>(lines) * 64);
    for (int i = 0; i < lines; ++i) {
        out += "\x1b[38;5;";
        out += std::to_string(1 + (i % 254));
        out += "mline ";
        out += std::to_string(i);
        out += " history marker ";
        out += std::to_string((i * 7) % 997);
        out += " the quick brown fox\x1b[0m\r\n";
    }
    return out;
}

/// Builds a Tektronix scene containing vectors, points, and text.
///
/// - Returns: Parser input that enters Tektronix mode and draws the scene.
inline std::string tekFeed()
{
    auto vec = [](int x, int y) {
        std::string out;
        out += static_cast<char>(0x20 | ((y >> 7) & 0x1F));
        out += static_cast<char>(0x60 | ((y >> 2) & 0x1F));
        out += static_cast<char>(0x20 | ((x >> 7) & 0x1F));
        out += static_cast<char>(0x40 | ((x >> 2) & 0x1F));
        return out;
    };
    std::string out = "\x1b[?38h";
    out += "\x1b"
           "`"
           "\x1d";
    out += vec(200, 200) + vec(3800, 200) + vec(3800, 2800) + vec(200, 2800) + vec(200, 200);
    out += "\x1d" + vec(200, 200) + vec(3800, 2800);
    out += "\x1b"
           "a"
           "\x1d"
        + vec(200, 600) + vec(3800, 600);
    out += "\x1b"
           "b"
           "\x1d"
        + vec(200, 900) + vec(3800, 900);
    out += "\x1b"
           "c"
           "\x1d"
        + vec(200, 1200) + vec(3800, 1200);
    out += "\x1b"
           "d"
           "\x1d"
        + vec(200, 1500) + vec(3800, 1500);
    out += "\x1c" + vec(1000, 2000) + vec(1200, 2000) + vec(1400, 2000);
    out += "\x1d" + vec(400, 2400) + "\x1f"
                                     "TEK ALPHA 4014 STORAGE TUBE";
    out += "\x1b"
           "8"
           "\x1d"
        + vec(400, 2600) + "\x1f"
                           "LARGE";
    return out;
}

/// Builds rendering fixtures sized for the benchmark grid.
///
/// - Parameters:
///   - rows: The positive grid height.
///   - columns: The positive grid width.
/// - Returns: Scenes with full input and optional incremental damage input.
inline std::vector<Scene> buildScenes(int rows, int columns)
{
    const std::size_t fillBytes = static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns) * 3;
    const std::size_t rowBytes = static_cast<std::size_t>(columns) * 3;
    return {
        { "variedpen", variedPenFeed(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns) * 3),
            variedPenFeed(static_cast<std::size_t>(columns)) },
        { "text", repeatTo("The quick brown fox jumps over the lazy dog 0123456789 ", fillBytes),
            repeatTo("The quick brown fox jumps over the lazy dog 0123456789 ", rowBytes) },
        { "ligated",
            repeatTo("-> => != !== === <= >= :: := <=> |> ++ -- << >> && || ?? "
                     "\x1b[1m-> != === <=>\x1b[0m --> <!-- ... =~ ",
                fillBytes),
            repeatTo("-> => != !== === <= >= :: := <=> |> ++ -- << >> && || ?? "
                     "\x1b[1m-> != === <=>\x1b[0m --> <!-- ... =~ ",
                rowBytes) },
        { "chunked", repeatTo("a.", fillBytes), repeatTo("a.", rowBytes) },
        { "sgr",
            repeatTo("\x1b[1;36mfile\x1b[0m.cpp:\x1b[33m42\x1b[0m  \x1b[1;32mconst\x1b[0m auto v = "
                     "\x1b[35mcompute\x1b[0m(x, y); \x1b[2m// note\x1b[0m ",
                fillBytes),
            repeatTo("\x1b[1;36mfile\x1b[0m.cpp:\x1b[33m42\x1b[0m  \x1b[1;32mconst\x1b[0m auto v = "
                     "\x1b[35mcompute\x1b[0m(x, y); \x1b[2m// note\x1b[0m ",
                rowBytes) },
        { "doublerows",
            repeatTo("\x1b#6double width\r\n"
                     "\x1b#3double height\r\n"
                     "\x1b#4double height\r\n"
                     "single width\r\n",
                fillBytes),
            repeatTo("\x1b#3double height ", rowBytes) },
        { "box",
            repeatTo("┌─┬─┐ ├─┼─┤ │ │ │ └─┴─┘ ╔═╦═╗ ╠═╬═╣ ║ ║ ║ ╚═╩═╝ "
                     "█▓▒░ ▀▄▌▐ ▖▗▘▙▚▛▜▝▞▟ 🬀🬁🬂🬃🬄 ",
                fillBytes),
            repeatTo("┌─┬─┐ ├─┼─┤ │ │ │ └─┴─┘ ╔═╦═╗ ╠═╬═╣ ║ ║ ║ ╚═╩═╝ "
                     "█▓▒░ ▀▄▌▐ ▖▗▘▙▚▛▜▝▞▟ 🬀🬁🬂🬃🬄 ",
                rowBytes) },
        { "unicode",
            repeatTo("日本語 αβγδ ñörmàl grüße \U0001f389\U0001f680 "
                     "\U0001f468‍\U0001f469‍\U0001f467 \U0001f1ef\U0001f1f5 \U0001f44d\U0001f3fd "
                     "العربية ",
                fillBytes),
            repeatTo("日本語 αβγδ ñörmàl grüße \U0001f389\U0001f680 "
                     "\U0001f468‍\U0001f469‍\U0001f467 \U0001f1ef\U0001f1f5 \U0001f44d\U0001f3fd "
                     "العربية ",
                rowBytes) },
        { "decorated",
            repeatTo("\x1b[4munder\x1b[0m \x1b[4:3mcurly\x1b[0m \x1b[4:4mdotted\x1b[0m "
                     "\x1b[4:5mdashed\x1b[0m \x1b[21mdouble\x1b[0m \x1b[9mstrike\x1b[0m "
                     "\x1b[53mover\x1b[0m \x1b[4;58;5;196mcolored\x1b[0m \x1b[51mframed\x1b[0m "
                     "\x1b[52mcircled\x1b[0m \x1b[4;9;53mcombined\x1b[0m "
                     "\x1b[60m右\x1b[61m右\x1b[62m左\x1b[63m左\x1b[64m点\x1b[65m ",
                fillBytes * 3),
            repeatTo("\x1b[4munder\x1b[0m \x1b[4:3mcurly\x1b[0m \x1b[4:4mdotted\x1b[0m "
                     "\x1b[4:5mdashed\x1b[0m \x1b[21mdouble\x1b[0m \x1b[9mstrike\x1b[0m "
                     "\x1b[53mover\x1b[0m \x1b[4;58;5;196mcolored\x1b[0m \x1b[51mframed\x1b[0m "
                     "\x1b[52mcircled\x1b[0m \x1b[4;9;53mcombined\x1b[0m "
                     "\x1b[60m右\x1b[61m右\x1b[62m左\x1b[63m左\x1b[64m点\x1b[65m ",
                rowBytes * 3) },
        { "blink",
            repeatTo("\x1b[5mblink\x1b[0m \x1b[6mfast\x1b[0m \x1b[5;4munder\x1b[0m steady ", fillBytes),
            repeatTo("\x1b[5mblink\x1b[0m \x1b[6mfast\x1b[0m steady ", rowBytes) },
        { "cursor", repeatTo("cursor ", fillBytes) + "\x1b[4 q",
            repeatTo("cursor ", rowBytes) },
        { "promptmarks", repeatTo("\x1b]133;A\x1b\\cursor \x1b]133;C\x1b\\cursor\r\n", fillBytes),
            repeatTo("\x1b]133;A\x1b\\cursor \x1b]133;C\x1b\\cursor ", rowBytes) },
        { "clusters", repeatTo("e\xcc\x81 o\xcc\xa3\xcc\x82 a ", fillBytes),
            repeatTo("e\xcc\x81 o\xcc\xa3\xcc\x82 a ", rowBytes) },
        { "sixelimage",
            repeatTo("under ", fillBytes) + "\x1bPq\"1;1;16;16#0;2;100;20;20#0~~~~~~~~-~~~~~~~~\x1b\\",
            "\x1bPq\"1;1;16;16#0;2;100;20;20#0~~~~~~~~\x1b\\" },
        { "softfont",
            std::string("\x1bP1;1;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\x1b\\\x1b(@") + repeatTo("!!!! ", fillBytes),
            repeatTo("!!!! ", rowBytes) },
        { "backdrop",
            repeatTo("backdrop cells    ", fillBytes / 3),
            repeatTo("backdrop cells    ", rowBytes),
            0, false, true },
        { "selected",
            repeatTo("\x1b[1;35mselect\x1b[0m me \x1b[32mplease\x1b[0m now ", fillBytes),
            repeatTo("\x1b[1;35mselect\x1b[0m me \x1b[32mplease\x1b[0m now ", rowBytes),
            0, true },
        { "scrolledback", numberedHistoryFeed(rows + 40), std::string(), 15 },
        { "tek", tekFeed(), std::string() },
    };
}

/// A line of ligature-heavy source code, the unit the scroll benchmark feeds.
///
/// The operators are the ones a programming face ligates (`->`, `=>`, `!=`,
/// `==`, `<=`, `&&`), mixed with plain identifiers and a stacked-accent
/// comment every eighth line, so a scroll of these lines moves strips, plain
/// glyphs and the occasional ink that does reach past its row.
///
/// - Parameter index: The line's index, which picks the text.
/// - Returns: The line's parser input, without a line ending.
inline std::string ligatureCodeLine(int index)
{
    static const char* const lines[] = {
        "fn parse(input: &str) -> Result<Token, Error> {",
        "    if lhs != rhs && depth >= limit { return Err(e); }",
        "    let next = tokens.iter().map(|t| t => t.kind).collect();",
        "    while cursor <= end || flag == !ready { cursor += 1; }",
        "    match state { State::Open => close(), _ => {} }",
        "    let ptr = node->next; assert!(ptr !== null);",
        "    total = total << 2 | mask >> 1; // shift === pack",
        "    // r\xc3\xa9sum\xc3\xa9 e\xcc\x81\xcc\x82\xcc\x83 <=> keep",
    };
    std::string line = lines[static_cast<std::size_t>(index) % (sizeof(lines) / sizeof(lines[0]))];
    line += "  // ";
    line += std::to_string(index);
    return line;
}

}
