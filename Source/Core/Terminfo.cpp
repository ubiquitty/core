// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Terminfo.cpp — see Terminfo.h.
//
// XTGETTCAP answers from the terminfo entry the app installs, parsed once from
// the same source text `tic` compiles. There is no second table: a capability
// added to the entry is answered over DCS + q the moment it lands, and one the
// entry does not carry is a miss on both roads. The parse is the small subset
// of the terminfo source grammar the entry uses — booleans, `#` numerics, `=`
// strings with the `\E ^X \ddd \, \s` escapes — kept as bytes the way the
// compiled entry stores them, `%` parameter code and `$<>` padding included.
//
// Termcap probes name a capability by its two-letter name; the entry carries
// only terminfo names, so the aliases below map the termcap spelling of every
// capability a startup probe asks for onto the long name.

#include "Core/Terminfo.h"

#include "Core/TerminfoSource.h"

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Terminfo {

namespace {

// The entry: xterm-256color's capability set trimmed to what the VT engine
// implements (256 + truecolor, italics, strikethrough, titles, styled and
// coloured underlines, OSC 52, bracketed paste, synchronized output, focus
// events, SGR mouse, DECSCUSR, XTGETTCAP, xterm's modified keys).
//
// The ncurses RGB cap is absent because RGB promises setaf/setab consume
// a packed 24-bit value; ours are the indexed 38;5;N form, so claiming RGB makes
// an application (Emacs, for one) emit 38;5;<packed-rgb>, an out-of-range index
// that renders as no colour. SGR 38;2 / 48;2 still round-trip through the pen.
// Graphics (sixel, kitty), OSC 8 and OSC 7 are reached out of band, not through
// this entry.
const char source[] = "xterm-ubiquitty|ubiquitty|ubiquitty-256color|Ubiquitty with xterm 256-color, truecolor and extras,\n"
                      "\tam, bce, ccc, km, mc5i, mir, msgr, npc, xenl, AX, Tc, XT,\n"
                      "\tcolors#0x100, cols#80, it#8, lines#24, pairs#0x10000,\n"
                      "\tacsc=``aaffggiijjkkllmmnnooppqqrrssttuuvvwwxxyyzz{{||}}~~,\n"
                      "\tbel=^G, blink=\\E[5m, bold=\\E[1m, cbt=\\E[Z, civis=\\E[?25l,\n"
                      "\tclear=\\E[H\\E[2J, cnorm=\\E[?12l\\E[?25h, cr=\\r,\n"
                      "\tcsr=\\E[%i%p1%d;%p2%dr, cub=\\E[%p1%dD, cub1=^H,\n"
                      "\tcud=\\E[%p1%dB, cud1=\\n, cuf=\\E[%p1%dC, cuf1=\\E[C,\n"
                      "\tcup=\\E[%i%p1%d;%p2%dH, cuu=\\E[%p1%dA, cuu1=\\E[A,\n"
                      "\tcvvis=\\E[?12;25h, dch=\\E[%p1%dP, dch1=\\E[P, dim=\\E[2m,\n"
                      "\tdl=\\E[%p1%dM, dl1=\\E[M, ech=\\E[%p1%dX, ed=\\E[J, E3=\\E[3J, el=\\E[K,\n"
                      "\tel1=\\E[1K, flash=\\E[?5h$<100/>\\E[?5l, home=\\E[H, ht=^I,\n"
                      "\thts=\\EH, ich=\\E[%p1%d@, il=\\E[%p1%dL, il1=\\E[L, ind=\\n,\n"
                      "\tindn=\\E[%p1%dS, invis=\\E[8m,\n"
                      "\tkbs=^?, kcbt=\\E[Z, kcub1=\\EOD, kcud1=\\EOB, kcuf1=\\EOC,\n"
                      "\tkcuu1=\\EOA, kdch1=\\E[3~, kend=\\EOF, kent=\\EOM, khome=\\EOH,\n"
                      "\tkich1=\\E[2~, kmous=\\E[<, knp=\\E[6~, kpp=\\E[5~,\n"
                      "\tkf1=\\EOP, kf2=\\EOQ, kf3=\\EOR, kf4=\\EOS, kf5=\\E[15~,\n"
                      "\tkf6=\\E[17~, kf7=\\E[18~, kf8=\\E[19~, kf9=\\E[20~,\n"
                      "\tkf10=\\E[21~, kf11=\\E[23~, kf12=\\E[24~,\n"
                      "\tmc0=\\E[i, mc4=\\E[4i, mc5=\\E[5i, nel=\\r\\n,\n"
                      "\top=\\E[39;49m, rc=\\E8, rep=%p1%c\\E[%p2%{1}%-%db,\n"
                      "\trev=\\E[7m, ri=\\EM, rin=\\E[%p1%dT, rmacs=\\E(B,\n"
                      "\trmcup=\\E[?1049l\\E[23;0;0t, rmir=\\E[4l, rmkx=\\E[?1l\\E>,\n"
                      "\trmso=\\E[27m, rmul=\\E[24m,\n"
                      "\trs1=\\Ec\\E]104\\007, sc=\\E7,\n"
                      // ccc says the palette can be redefined; without initc a
                      // curses app that asks can_change_color() is told yes and
                      // then init_color() returns ERR, so it changes nothing and
                      // reports nothing. The emulator answers OSC 4 and OSC 104
                      // — rs1 above uses 104 — and initc is the entry telling
                      // applications how to ask. The scaling is curses'
                      // 0..1000 per channel onto 0..255.
                      "\tinitc=\\E]4;%p1%d;rgb\\:%p2%{255}%*%{1000}%/%2.2X/%p3%{255}%*%{1000}%/%2.2X/%p4%{255}%*%{1000}%/%2.2X\\E\\\\,\n"
                      "\toc=\\E]104\\007,\n"
                      "\tsitm=\\E[3m, ritm=\\E[23m, smxx=\\E[9m, rmxx=\\E[29m,\n"
                      "\ths, tsl=\\E]2;, fsl=^G, dsl=\\E]2;\\007,\n"
                      "\tsetab=\\E[%?%p1%{8}%<%t4%p1%d%e%p1%{16}%<%t10%p1%{8}%-%d%e48;5;%p1%d%;m,\n"
                      "\tsetaf=\\E[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;m,\n"
                      "\tsgr=%?%p9%t\\E(0%e\\E(B%;\\E[0%?%p6%t;1%;%?%p5%t;2%;%?%p2%t;4%;"
                      "%?%p1%p3%|%t;7%;%?%p4%t;5%;%?%p7%t;8%;m,\n"
                      "\tsgr0=\\E(B\\E[m, smacs=\\E(0, smcup=\\E[?1049h\\E[22;0;0t,\n"
                      "\tsmir=\\E[4h, smkx=\\E[?1h\\E=, smso=\\E[7m, smul=\\E[4m,\n"
                      "\ttbc=\\E[3g, u6=\\E[%i%d;%dR, u7=\\E[6n,\n"
                      "\tu8=\\E[?%[;0123456789]c, u9=\\E[c,\n"
                      "\thpa=\\E[%i%p1%dG, vpa=\\E[%i%p1%dd,\n"
                      "# Ubiquitty extensions: styled+colored underline, clipboard,\n"
                      "# bracketed paste, synchronized output, focus events, cursor style.\n"
                      "\tSmulx=\\E[4:%p1%dm, Setulc=\\E[58:2::%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%dm,\n"
                      "\tMs=\\E]52;%p1%s;%p2%s\\007,\n"
                      "\tBD=\\E[?2004l, BE=\\E[?2004h,\n"
                      "\tPS=\\E[200~, PE=\\E[201~,\n"
                      "\tPst=\\E]7501;%p1%s\\E\\\\,\n"
                      "\tSync=\\E[?2026%?%p1%{1}%-%tl%eh%;,\n"
                      "\tfe=\\E[?1004h, fd=\\E[?1004l,\n"
                      "\tXM=\\E[?1006;1000%?%p1%{1}%=%th%el%;,\n"
                      "\txm=\\E[<%i%p3%d;%p1%d;%p2%d;%?%p4%tM%em%;,\n"
                      "\tSs=\\E[%p1%d\\sq, Se=\\E[7\\sq,\n"
                      "\tCr=\\E]112\\007, Cs=\\E]12;%p1%s\\007,\n"
                      "\tkDC=\\E[3;2~, kEND=\\E[1;2F, kHOM=\\E[1;2H, kIC=\\E[2;2~,\n"
                      "\tkLFT=\\E[1;2D, kNXT=\\E[6;2~, kPRV=\\E[5;2~, kRIT=\\E[1;2C,\n"
                      "\tkb2=\\EOE, kind=\\E[1;2B, kri=\\E[1;2A,\n"
                      // Auto-margin (DECAWM), the XTVERSION query and the shape
                      // of its answer, and the two-string soft reset: DECSTR,
                      // insert mode off, numeric keypad. Without DECCOLM's
                      // 80-column half: DECCOLM is ignored until a program
                      // allows it with mode 40, so it would do nothing here.
                      "\tsmam=\\E[?7h, rmam=\\E[?7l,\n"
                      "\tXR=\\E[>0q, xr=\\EP>\\|[ -~]+\\E\\\\,\n"
                      "\tis2=\\E[!p\\E[4l\\E>, rs2=\\E[!p\\E[4l\\E>,\n"
                      // xterm's modified keys (its +pcfkeys block): the encoder
                      // emits CSI 1;<mod> for F1-F4 and the cursor keys and
                      // CSI n;<mod>~ for the rest, mod = 1 + shift 1, alt 2,
                      // ctrl 4. kf13-24 shift, 25-36 ctrl, 37-48 ctrl+shift,
                      // 49-60 alt, 61-63 alt+shift; the navigation keys carry
                      // the modifier as a suffix, 3 alt through 7 ctrl+alt.
                      "\tkf13=\\E[1;2P, kf14=\\E[1;2Q, kf15=\\E[1;2R, kf16=\\E[1;2S, kf17=\\E[15;2~,\n"
                      "\tkf18=\\E[17;2~, kf19=\\E[18;2~, kf20=\\E[19;2~, kf21=\\E[20;2~, kf22=\\E[21;2~,\n"
                      "\tkf23=\\E[23;2~, kf24=\\E[24;2~, kf25=\\E[1;5P, kf26=\\E[1;5Q, kf27=\\E[1;5R,\n"
                      "\tkf28=\\E[1;5S, kf29=\\E[15;5~, kf30=\\E[17;5~, kf31=\\E[18;5~, kf32=\\E[19;5~,\n"
                      "\tkf33=\\E[20;5~, kf34=\\E[21;5~, kf35=\\E[23;5~, kf36=\\E[24;5~, kf37=\\E[1;6P,\n"
                      "\tkf38=\\E[1;6Q, kf39=\\E[1;6R, kf40=\\E[1;6S, kf41=\\E[15;6~, kf42=\\E[17;6~,\n"
                      "\tkf43=\\E[18;6~, kf44=\\E[19;6~, kf45=\\E[20;6~, kf46=\\E[21;6~, kf47=\\E[23;6~,\n"
                      "\tkf48=\\E[24;6~, kf49=\\E[1;3P, kf50=\\E[1;3Q, kf51=\\E[1;3R, kf52=\\E[1;3S,\n"
                      "\tkf53=\\E[15;3~, kf54=\\E[17;3~, kf55=\\E[18;3~, kf56=\\E[19;3~, kf57=\\E[20;3~,\n"
                      "\tkf58=\\E[21;3~, kf59=\\E[23;3~, kf60=\\E[24;3~, kf61=\\E[1;4P, kf62=\\E[1;4Q,\n"
                      "\tkf63=\\E[1;4R, kUP=\\E[1;2A, kDN=\\E[1;2B, kUP3=\\E[1;3A, kUP4=\\E[1;4A,\n"
                      "\tkUP5=\\E[1;5A, kUP6=\\E[1;6A, kUP7=\\E[1;7A, kDN3=\\E[1;3B, kDN4=\\E[1;4B,\n"
                      "\tkDN5=\\E[1;5B, kDN6=\\E[1;6B, kDN7=\\E[1;7B, kLFT3=\\E[1;3D, kLFT4=\\E[1;4D,\n"
                      "\tkLFT5=\\E[1;5D, kLFT6=\\E[1;6D, kLFT7=\\E[1;7D, kRIT3=\\E[1;3C, kRIT4=\\E[1;4C,\n"
                      "\tkRIT5=\\E[1;5C, kRIT6=\\E[1;6C, kRIT7=\\E[1;7C, kHOM3=\\E[1;3H, kHOM4=\\E[1;4H,\n"
                      "\tkHOM5=\\E[1;5H, kHOM6=\\E[1;6H, kHOM7=\\E[1;7H, kEND3=\\E[1;3F, kEND4=\\E[1;4F,\n"
                      "\tkEND5=\\E[1;5F, kEND6=\\E[1;6F, kEND7=\\E[1;7F, kDC3=\\E[3;3~, kDC4=\\E[3;4~,\n"
                      "\tkDC5=\\E[3;5~, kDC6=\\E[3;6~, kDC7=\\E[3;7~, kIC3=\\E[2;3~, kIC4=\\E[2;4~,\n"
                      "\tkIC5=\\E[2;5~, kIC6=\\E[2;6~, kIC7=\\E[2;7~, kNXT3=\\E[6;3~, kNXT4=\\E[6;4~,\n"
                      "\tkNXT5=\\E[6;5~, kNXT6=\\E[6;6~, kNXT7=\\E[6;7~, kPRV3=\\E[5;3~, kPRV4=\\E[5;4~,\n"
                      "\tkPRV5=\\E[5;5~, kPRV6=\\E[5;6~, kPRV7=\\E[5;7~,\n";

struct Capability {
    std::string name;
    std::string value; // empty for a boolean; the decimal digits for a numeric
};

struct TermcapAlias {
    const char* termcap;
    const char* terminfo;
};

// The termcap spellings neovim, tmux, less and ncurses probe at startup.
constexpr TermcapAlias termcapAliases[] = {
    { "TN", "name" },
    { "Co", "colors" },
    { "pa", "pairs" },
    { "co", "cols" },
    { "li", "lines" },
    { "am", "am" },
    { "bw", "bw" },
    { "km", "km" },
    { "mi", "mir" },
    { "ms", "msgr" },
    { "xn", "xenl" },
    { "ut", "bce" },
    { "hs", "hs" },
    { "ce", "el" },
    { "cd", "ed" },
    { "cl", "clear" },
    { "ho", "home" },
    { "cr", "cr" },
    { "nw", "nel" },
    { "bl", "bel" },
    { "le", "cub1" },
    { "k1", "kf1" },
    { "k2", "kf2" },
    { "k3", "kf3" },
    { "k4", "kf4" },
    { "k5", "kf5" },
    { "k6", "kf6" },
    { "k7", "kf7" },
    { "k8", "kf8" },
    { "k9", "kf9" },
    { "k;", "kf10" },
    { "F1", "kf11" },
    { "F2", "kf12" },
    { "ku", "kcuu1" },
    { "kd", "kcud1" },
    { "kr", "kcuf1" },
    { "kl", "kcub1" },
    { "kh", "khome" },
    { "@7", "kend" },
    { "kI", "kich1" },
    { "kD", "kdch1" },
    { "kP", "kpp" },
    { "kN", "knp" },
    { "Km", "kmous" },
};

/// Splits the source into capabilities once, on first use.
///
/// The first field is the name list; its first alias is answered as `name`.
/// Comment lines are skipped, a `,` inside a value is escaped as `\,`, and the
/// separator's trailing whitespace is not part of the next name.
const std::vector<Capability>& capabilities()
{
    static const std::vector<Capability> parsed = [] {
        std::vector<Capability> out;
        std::string_view text = sourceText();
        // Comment lines go before the split on commas: a comment may hold one.
        std::string body;
        body.reserve(text.size());
        while (!text.empty()) {
            const std::size_t end = text.find('\n');
            const std::string_view line = text.substr(0, end);
            if (!line.starts_with('#'))
                body.append(line).push_back('\n');
            if (end == std::string_view::npos)
                break;
            text.remove_prefix(end + 1);
        }
        std::vector<std::string> fields;
        std::string current;
        for (std::size_t i = 0; i < body.size(); ++i) {
            const char c = body[i];
            if (c == '\\' && i + 1 < body.size()) {
                current.push_back(c);
                current.push_back(body[++i]);
                continue;
            }
            if (c == ',') {
                fields.push_back(current);
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        if (!current.empty())
            fields.push_back(current);
        bool first = true;
        for (const std::string& field : fields) {
            std::string_view view = field;
            while (!view.empty() && (view.front() == ' ' || view.front() == '\t' || view.front() == '\n'))
                view.remove_prefix(1);
            while (!view.empty() && (view.back() == ' ' || view.back() == '\t' || view.back() == '\n'))
                view.remove_suffix(1);
            if (view.empty())
                continue;
            if (first) {
                first = false;
                out.push_back({ "name", std::string(view.substr(0, view.find('|'))) });
                continue;
            }
            if (const std::size_t equals = view.find('='); equals != std::string_view::npos)
                out.push_back({ std::string(view.substr(0, equals)), Source::decodeValue(view.substr(equals + 1)) });
            else if (const std::size_t hash = view.find('#'); hash != std::string_view::npos)
                out.push_back({ std::string(view.substr(0, hash)), Source::decodeNumber(view.substr(hash + 1)) });
            else if (!view.ends_with('@'))
                out.push_back({ std::string(view), std::string() });
        }
        return out;
    }();
    return parsed;
}

inline int hexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

} // namespace

const char* sourceText() { return source; }

bool isEntryName(std::string_view terminalName)
{
    // The name list is the source's first field: names joined by `|`, the
    // last of them the description rather than a name.
    std::string_view names = sourceText();
    names = names.substr(0, names.find(','));
    names = names.substr(0, names.rfind('|'));
    while (!terminalName.empty()) {
        const std::size_t bar = names.find('|');
        if (names.substr(0, bar) == terminalName)
            return true;
        if (bar == std::string_view::npos)
            return false;
        names.remove_prefix(bar + 1);
    }
    return false;
}

std::string_view canonicalName(std::string_view capabilityName)
{
    for (const TermcapAlias& alias : termcapAliases) {
        if (capabilityName == alias.termcap)
            return alias.terminfo;
    }
    return capabilityName;
}

std::optional<std::string_view> lookup(std::string_view capabilityName)
{
    if (capabilityName.empty())
        return std::nullopt;
    const std::string_view name = canonicalName(capabilityName);
    for (const Capability& capability : capabilities()) {
        if (capability.name == name)
            return std::string_view(capability.value);
    }
    return std::nullopt;
}

int hexDecode(std::string_view hex, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    // A view's length cannot be negative, so the only malformed shape is an
    // odd one.
    if ((hex.size() & 1) != 0)
        return -1;
    int needed = static_cast<int>(hex.size() / 2);
    if (needed > outputCapacity)
        return -1;
    for (int i = 0; i < needed; i++) {
        int hi = hexNibble(hex[2 * i]);
        int lo = hexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        outputBuffer[i] = char((hi << 4) | lo);
    }
    return needed;
}

int hexEncode(std::string_view input, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    static const char hex[] = "0123456789abcdef";
    const int inputLength = static_cast<int>(input.size());
    // Compared as a division rather than as `inputLength * 2`: the product is
    // what overflows, and it overflows before the comparison that would have
    // rejected the input.
    if (inputLength > outputCapacity / 2)
        return -1;
    for (int i = 0; i < inputLength; i++) {
        unsigned char b = static_cast<unsigned char>(input[i]);
        outputBuffer[2 * i] = hex[(b >> 4) & 0xF];
        outputBuffer[2 * i + 1] = hex[b & 0xF];
    }
    return inputLength * 2;
}

} // namespace Ubiquitty::Terminfo
