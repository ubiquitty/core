#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Write the type specimen plates into Tests/Snapshots/cases.json.

The specimen is a folio of plates, one snapshot case each, that puts the
renderer's typography on one page: faces and display mathematics, styles and
colour, proportional runs, scripts, the glyph plate, and texture healing.
Each plate is composed here from the escape sequences it needs, so a plate
is regenerated from its source rather than edited in the JSON, and a change
to a plate is a change to this file.

The cases are written in place: every case whose name starts with
``specimen_``, and the ``proportional_cursor_claim`` case beside them, is
replaced by the plate generated here; every other case is untouched. Their
goldens are minted by the snapshot runner as usual::

    python3 Tests/Snapshots/gen_folio.py            # write the cases
    python3 Tests/Snapshots/gen_folio.py --check    # fail when cases.json is stale
    python3 Tests/Snapshots/run.py --backend macos --update specimen_

Stdlib only.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases.json")

E = "\x1b"
BEL = "\x07"

SGR = re.compile("\x1b(\\[[0-9;:]*m|\\][^\x07]*\x07|[#(][0-9B])")

IT = E + "[3m"
RM = E + "[23m"
SUP = E + "[73m"
SUB = E + "[74m"
SOFF = E + "[75m"
BOLD = E + "[1m"
BOFF = E + "[22m"


def visible(text):
    return len(SGR.sub("", text))


def place(line, column, text):
    pad = column - visible(line)
    return line + " " * max(pad, 1) + text


def rule(n):
    return "─" * n


def masthead(title, folio, width=96):
    line = place("   " + title, width - 1 - len(folio), folio)
    return ["", line, "   " + rule(width - 4), ""]


# The plates' face and settings: the specimen profile, and the theme the plates
# are set on. Every arm renders the plates in the unifont tier as well.
NATIVE = "jetbrains_mono_13_symbols"
SETTINGS = {"appearance": {"theme": "Noir Clair"}}

pages = []

# ---- Plate I — faces, sizes, mathematics ---------------------------------
L = masthead("U B I Q U I T T Y   ·   T Y P E   S P E C I M E N", "P L A T E   I")
L.append(E + "#3   Aa Bb Gg Qq Rr Ss 0123456789 &")
L.append(E + "#4   Aa Bb Gg Qq Rr Ss 0123456789 &")
L.append(E + "#6   double width, the DEC way")
L.append("")
L.append(f"   sized      {E}]66;s=3;Aa{BEL}      {E}]66;s=2;Bb{BEL}     scale three and two, one glyph box each")
L.append("")
L.append("")
L.append(f"   weight     Regular   {BOLD}Bold{BOFF}   {E}[2mFaint{BOFF}   {IT}Italic{RM}   {BOLD}{IT}Bold Italic{E}[0m")
L.append("")
r1, r2, r3 = "", "   display", ""
r1 = place(r1, 14, f"⎛ {IT}n{RM} ⎞")
r2 = place(r2, 14, "⎜   ⎟  =  ────────")
r3 = place(r3, 14, f"⎝ {IT}k{RM} ⎠")
r1 = place(r1, 27, f"{IT}n{RM}!")
r3 = place(r3, 24, f"{IT}k{RM}!({IT}n{RM}−{IT}k{RM})!")
r1 = place(r1, 41, f"{IT}n{RM}")
r2 = place(r2, 41, "∑")
r2 = place(r2, 44, f"{IT}i{RM}  =  ½ {IT}n{RM}({IT}n{RM}+1)")
r3 = place(r3, 40, f"{IT}i{RM}=1")
r1 = place(r1, 68, "⌠")
r2 = place(r2, 68, "⎮")
r3 = place(r3, 68, "⌡")
r1 = place(r1, 70, "∞")
r2 = place(r2, 71, f"{IT}e{RM}{SUP}−{IT}x{RM}²{SOFF} d{IT}x{RM}  =  √π ⁄ 2")
r3 = place(r3, 70, "0")
L += [r1, r2, r3, ""]
r1, r2, r3 = "", "   matrix", ""
r1 = place(r1, 14, f"⎡ {IT}a{RM}  {IT}b{RM} ⎤")
r2 = place(r2, 14, "⎢      ⎥")
r3 = place(r3, 14, f"⎣ {IT}c{RM}  {IT}d{RM} ⎦")
r1 = place(r1, 31, "d")
r2 = place(r2, 30, f"─── {IT}f{RM}({IT}x{RM})  =  {IT}f{RM}′({IT}x{RM})")
r3 = place(r3, 30, f"d{IT}x{RM}")
r1 = place(r1, 55, f"⎧ {IT}x{RM},   {IT}x{RM} > 0")
r2 = place(r2, 49, f"|{IT}x{RM}| = ⎨")
r3 = place(r3, 55, f"⎩ −{IT}x{RM},  {IT}x{RM} ≤ 0")
L += [r1, r2, r3, ""]
L.append(f"   science    E = {IT}mc{RM}{SUP}2{SOFF}   H{SUB}2{SOFF}O   {IT}x{RM}{SUP}2{SUB}1{SOFF}   {IT}a{RM}{SUP}{IT}n{RM}+1{SOFF} = {IT}a{RM}{SUB}{IT}n{RM}{SOFF} + {IT}d{RM}   ∆{IT}t{RM} → 0   {IT}λ{RM} = {IT}c{RM} ⁄ {IT}ν{RM}")
L.append("   sorts      ∀ ∃ ∈ ∉ ∩ ∪ ⊂ ⊃   ≈ ≠ ≡ ≤ ≥ ± × ÷ ∝   ⊕ ⊗ ⋅   ⌈x⌉ ⌊x⌋")
L.append("   ligatures  => -> === <= >= :: |> <| ++ -- ... ///")
L.append("")
pages.append({
    "name": "specimen_faces",
    "columns": 96,
    "rows": len(L) + 1,
    "settings": SETTINGS,
    "native": NATIVE,
    "args": ["--draws-sized-text"],
    "feed": E + "[?25l" + "\r\n".join(L),
    "doc": ("Plate I of the specimen folio: the faces, the sizes, and mathematics set as mathematics. The "
        "DEC double-height pair and double-width line, OSC 66 sized text at scales three and two, the SGR "
        "weights, then display settings a foundry would show: a binomial in stacked parentheses over a "
        "ruled fraction, a sum carrying its limits above and below, an integral assembled from its top, "
        "extension, and bottom pieces with raised bounds and a superscripted exponent, a bracketed matrix "
        "beside a derivative, and a piecewise absolute value in a three-piece brace — all with italic "
        "variables and roman operators. The science line carries the inline forms, the sorts line the "
        "operator repertoire, the ligature line the coding fusions."),
})

# ---- Plate II — styles, rules, colour ------------------------------------
L = masthead("S T Y L E S ,   R U L E S   &   C O L O U R", "P L A T E   I I")
L.append(f"   manner     {E}[7m Inverse {E}[27m   {E}[9mStruck{E}[29m   {E}[53mOverlined{E}[55m   {E}[8mConcealed{E}[28m ← concealed   {E}[2;3mfaint italic{E}[0m")
L.append("")
L.append(f"   underline  {E}[4msingle{E}[24m   {E}[21mdouble{E}[24m   {E}[4:3mcurly{E}[4:0m   {E}[4:4mdotted{E}[4:0m   {E}[4:5mdashed{E}[4:0m   {E}[4;58;5;160mcarmine{E}[59;24m   {E}[4:3;58;5;27mazure curl{E}[59;4:0m")
L.append("")
L.append(f"   ideogram   {E}[60m下線{E}[65m under   {E}[61m二重{E}[65m double   {E}[62m上線{E}[65m over   {E}[63m二上{E}[65m double-over   {E}[64m強調{E}[65m stress")
L.append("")
L.append(f"   link       {E}]8;;https://example.invalid/plate-ii{BEL}a hyperlink, quietly ruled{E}]8;;{BEL}")
L.append("")
ansi = "   ansi       "
for i in range(8):
    ansi += f"{E}[4{i}m   {E}[49m"
ansi += "  "
for i in range(8):
    ansi += f"{E}[10{i}m   {E}[49m"
L.append(ansi)
cube = "   spectrum   "
for i in range(72):
    cube += f"{E}[48;5;{16 + i}m {E}[49m"
L.append(cube)
grey = "   greys      "
for i in range(24):
    grey += f"{E}[48;5;{232 + i}m   {E}[49m"
L.append(grey)
ramp = "   truecolor  "
for i in range(72):
    r = 30 + i * 3
    g = 50 + i * 2
    b = 235 - i * 3
    ramp += f"{E}[48;2;{r};{g};{b}m {E}[49m"
L.append(ramp)
L.append("")
pages.append({
    "name": "specimen_styles",
    "columns": 96,
    "rows": len(L) + 1,
    "settings": SETTINGS,
    "native": NATIVE,
    "feed": E + "[?25l" + "\r\n".join(L),
    "doc": ("Plate II: every manner of dressing a glyph. Inverse, strike, overline, and conceal beside a "
        "faint italic, the five underline shapes with two coloured rules (SGR 58), the five ideogram marks "
        "on CJK text, an OSC 8 hyperlink in the configured link ruling, then the colour system: the sixteen "
        "ANSI swatches, seventy-two steps of the 256-colour cube, the twenty-four greys, and a truecolour "
        "ramp whose neighbouring cells differ by a few units per channel — banding in any colour lane shows "
        "here first."),
})

# ---- Plate III — proportional text ---------------------------------------
L = masthead("P R O P O R T I O N A L   R U N S", "P L A T E   I I I")
L.append(f"   {BOLD}{E}]66;p=1:c=13;Install updates?{BEL}{BOFF}")
L.append("")
L.append(f"   {E}]66;p=1:c=23;A fresh build is ready, and its{BEL}")
L.append(f"   {E}]66;p=1:c=17;signature checks out.{BEL}")
L.append("")
# The Install label takes a cube white rather than SGR 97: the house light
# theme sets its bright white to the deepest ink, so 97 on a blue ground is
# the theme's answer and not a label.
L.append(f"   {E}[7m {E}]66;p=1:c=8:h=2;Cancel{BEL} {E}[0m   {E}[44;38;5;231m {E}]66;p=1:c=8:h=2;Install{BEL} {E}[0m      two proportional buttons, centred (h=2)")
L.append("")
L.append(f"   ladder     {E}]66;p=1:c=4;iiii{BEL}│ {E}]66;p=1:c=8;WWWW{BEL}│ four narrow letters fit four cells; four wide ones need eight")
L.append(f"   align      {E}]66;p=1:c=16:h=0;left (h=0){BEL}│{E}]66;p=1:c=16:h=1;right (h=1){BEL}│{E}]66;p=1:c=16:h=2;centred (h=2){BEL}│ one budget, three ways")
L.append(f"   justified  {E}]66;p=1:c=16:h=3;the quick brown{BEL}│{E}]66;p=1:c=16:h=3;fox jumps over{BEL}│{E}]66;p=1:c=16:h=3;the lazy dog.{BEL}│ word gaps open to the measure")
L.append("")
L.append(f"   styled     {IT}{E}]66;p=1:c=9;an italic run{BEL}{RM}  {BOLD}{IT}{E}]66;p=1:c=12;bold italic too{BEL}{E}[0m  {E}[4:3;58;5;27m{E}]66;p=1:c=13;curly underline{BEL}{E}[59;4:0m  {E}[38;5;160m{E}]66;p=1:c=8;in carmine{BEL}{E}[0m")
L.append("")
L.append(f"   mixed      grid text, then {E}]66;p=1:c=10;a quiet aside{BEL}, then grid again — one baseline")
L.append("")
pages.append({
    "name": "specimen_proportional",
    "columns": 96,
    "rows": len(L) + 1,
    "settings": SETTINGS,
    "native": NATIVE,
    "args": ["--draws-sized-text"],
    "feed": E + "[?25l" + "\r\n".join(L),
    "doc": ("Plate III: proportional text inside the grid, the OSC 66 p= protocol in the system face. A "
        "bold headline and a body set one line to a run, each claiming the columns its text measures, dialog buttons centred in their "
        "cells (h=2) under inverse and coloured grounds, the ladder that shows four narrow letters in four "
        "cells beside four wide ones in eight, one sixteen-column budget set left, right, and centred "
        "(h=0, 1, 2), three justified runs whose word gaps open to the measure (h=3), runs dressed in "
        "italic, bold italic, a coloured curly underline, and a colour of their own, and a proportional "
        "aside spliced into grid text on a shared baseline. Every run declares its column budget in the "
        "sequence, so a layout drift is a protocol regression, not a styling choice."),
})

# ---- The cursor inside a claim ------------------------------------------
L = []
L.append("cursor on a proportional run:")
L.append(f"  {E}]66;p=1:c=16;Remind me later{BEL}  and grid text after")
L.append("")
L.append("cursor on a wide pair:")
L.append("  漢字 and grid text after")
pages.append({
    "name": "proportional_cursor_claim",
    "columns": 48,
    "rows": 6,
    "settings": SETTINGS,
    "native": NATIVE,
    "args": ["--draws-sized-text"],
    "feed": "\r\n".join(L) + E + "[2;9H",
    "doc": ("The cursor addressed to a column inside a proportional run's claim. A run is one glyph "
        "across its cells, so the focused block cursor takes the whole run: the anchor bakes the cursor "
        "colour behind the strip and the text inverts across every claimed cell, where a one-cell bake "
        "on the continuation column drew nothing at all. The wide pair below is the same claim at two "
        "cells, pinned so the rule stays one rule."),
})

# ---- Plate IV — the world's text ----------------------------------------
L = masthead("T H E   W O R L D ' S   T E X T", "P L A T E   I V")
L.append("   cjk        漢字は幅二列  简体字  繁體字  한글  ひらがな  カタカナ")
L.append("")
L.append(f"   sized cjk  {E}]66;s=2;漢字{BEL}   {E}]66;s=2;😀{BEL}    wide glyphs and emoji at scale two")
L.append("")
L.append("")
L.append("   marks      é è ê ë  õ̂ ǹ ą̈  ậ ệ ị̂  (combining, stacked)")
L.append("")
L.append("   emoji      😀 🚀 🎨 🌊 🦉   presentation: ☂️ emoji ☂︎ text   zwj: 👩‍💻 👨‍👩‍👧")
L.append("")
L.append("   arrows     ← ↑ → ↓ ↔ ↕ ⇐ ⇒ ⇔ ↩ ↪ ⤴ ⤵")
L.append("")
L.append("   geometry   ■ □ ▲ △ ● ○ ◆ ◇ ★ ☆ ◢ ◣ ◤ ◥")
L.append("")
pages.append({
    "name": "specimen_scripts",
    "columns": 96,
    "rows": len(L) + 1,
    "settings": SETTINGS,
    "native": NATIVE,
    "args": ["--draws-sized-text"],
    "feed": E + "[?25l" + "\r\n".join(L),
    "doc": ("Plate IV: the text beyond ASCII. Wide CJK across three scripts and two syllabaries, wide "
        "glyphs and an emoji at OSC 66 scale two, combining marks single and stacked, emoji with both "
        "presentation selectors and ZWJ sequences, then the arrow and geometric ranges. Width is the claim "
        "under test on every line: a wide cell that measures one column, a mark that advances the pen, or "
        "a ZWJ sequence that splits shifts every glyph after it."),
})

# ---- Plate V — rules, boxes and separators -------------------------------
L = masthead("R U L E S ,   B O X E S   &   S E P A R A T O R S", "P L A T E   V")
L.append("   ┌─────┬─────┐   ┏━━━━━┳━━━━━┓   ╔═════╦═════╗   ╭─────╮   ╌╌╌╌ ┄┄┄┄ ┈┈┈┈")
L.append("   │ pty │ tty │   ┃ raw ┃ cmd ┃   ║ ssh ║ mux ║   │ tab │   ╍╍╍╍ ┅┅┅┅ ┉┉┉┉")
L.append("   ├─────┼─────┤   ┣━━━━━╋━━━━━┫   ╠═════╬═════╣   ╰─────╯   ╱╲╱╲ ╳╳ ╲╱╲╱")
L.append("   └─────┴─────┘   ┗━━━━━┻━━━━━┛   ╚═════╩═════╝")
L.append("")
L.append("   mixed      ┍━┑ ┎─┒ ┏┭┓ ┗┵┛  ╒═╕ ╓─╖ ╞═╡ ╟─╢  ┽ ╀ ╁ ╂ ┿ ╊ ╉")
L.append("")
L.append(f"   linedraw   {E}(0lqqwqqk{E}(B  SCS G0 line drawing  {E}(0x{E}(B pipes {E}(0x{E}(B  {E}(0mqqvqqj{E}(B")
L.append("")
L.append("   scan       ⎺⎺⎻⎻⎼⎼⎽⎽   elbows ⎡ ⎤ ⎣ ⎦   dentistry ⎴ ⎵")
L.append("")
L.append("   blocks     ▁▂▃▄▅▆▇█   ▏▎▍▌▋▊▉   ▀▄ ▖▗▘▝   ▙▚▛▜   ░░░▒▒▒▓▓▓")
L.append("   sextants   \U0001fb00\U0001fb01\U0001fb02\U0001fb07\U0001fb0e\U0001fb1e\U0001fb3a\U0001fb3b   quadrant fills tile with no seam between cells")
L.append("")
sep = "   separators "
for cp in range(0xE0B0, 0xE0C0):
    sep += chr(cp)
    if cp % 4 == 3:
        sep += " "
L.append(sep)
L.append("   segments   "
    + f"{E}[48;5;214;38;5;235m{chr(0xE0B0)}{E}[38;5;231m SRC {E}[48;5;235;38;5;214m{chr(0xE0B0)}"
    + f"{E}[38;5;245m {chr(0xE0B1)} BUILD {E}[49;38;5;235m{chr(0xE0B0)}{E}[0m   "
    + f"{E}[38;5;235m{chr(0xE0B2)}{E}[48;5;235;38;5;245m LOG {chr(0xE0B3)} RUN {E}[38;5;214m{chr(0xE0B2)}"
    + f"{E}[48;5;214;38;5;231m OUT {E}[0m")
art = "   artwork    " + " ".join(chr(c) for c in [0xE0A0, 0xE0A1, 0xE0A2, 0xE0A3]) \
    + "   " + "".join(chr(c) for c in [0xE0C0, 0xE0C2, 0xE0C8, 0xE0CA]) \
    + "   " + "".join(chr(c) for c in [0xE0CC, 0xE0CD, 0xE0CE, 0xE0CF]) \
    + "   " + "".join(chr(c) for c in [0xE0D0, 0xE0D1, 0xE0D2, 0xE0D4]) \
    + "   " + chr(0xE0D6) + chr(0xE0D7)
L.append(art)
L.append("")
pages.append({
    "name": "specimen_glyph_plate",
    "columns": 96,
    "rows": len(L) + 1,
    "settings": SETTINGS,
    "native": NATIVE,
    "feed": E + "[?25l" + "\r\n".join(L),
    "doc": ("Plate V: the graphics lane entire. The light, heavy and double box families with their mixed "
        "junctions, SCS G0 line drawing through the DEC special graphics set, scan lines, elbows and the "
        "dentistry pair from the terminal-graphics family, block elements, eighth steps, shades and "
        "sextants, then every analytic Powerline separator, the two-direction segment bar at full "
        "contrast, and the font-routed artwork from the bundled symbols face. Every table on the plate "
        "tiles by construction, so a seam, a weight drift, or a junction that misses its rail breaks a "
        "visible figure."),
})

# ---- Plate VI — texture healing (Monaspace) ------------------------------
L = masthead("T E X T U R E   H E A L I N G", "P L A T E   V I", width=72)
L.append("   witmilmw witmilmw witmilmw witmilmw witmilmw witmilmw")
L.append("   mm wi tm il mw  mm wi tm il mw  mm wi tm il mw  mm wi tm")
L.append("")
L.append("   The quick brown fox jumps over the lazy dog — illmw mmiw wmli")
L.append(f"   {BOLD}The quick brown fox jumps over the lazy dog — illmw mmiw wmli{E}[0m")
L.append("")
L.append("   tmux tmux tmux   mux mux mux   miniature illuminated manuscripts")
L.append("")
L.append("   fusions    => -> != == === ... :: |> <| ## && ||")
L.append("   healed      mill will wilt — letters drawn by their neighbours")
L.append("")
pages.append({
    "name": "specimen_healing",
    "columns": 72,
    "rows": len(L) + 1,
    "native": "monaspace_neon_nf_14",
    "feed": E + "[?25l" + "\r\n".join(L),
    "doc": ("Plate VI, set in Monaspace: texture healing and its fusions. The witmilmw runs and the spaced "
        "pairs draw the healed letterforms whose shapes depend on their neighbours, at regular and bold "
        "weight, the words that made the feature famous, and the coding fusions the same face carries. A "
        "shaping change that splits a run or drops a healed variant changes letterforms across whole lines "
        "of this plate at once."),
})



def generated_names():
    return {page["name"] for page in pages}


def merged_cases():
    """The cases file with the folio's plates in place of the ones it holds."""
    cases = json.load(open(CASES))
    names = generated_names()
    cases = [c for c in cases if c["name"] not in names]
    idx = next(i for i, c in enumerate(cases) if c["name"] == "powerline_segment_bar_line_spacing_10")
    for offset, page in enumerate(pages):
        cases.insert(idx + 1 + offset, page)
    return json.dumps(cases, indent=2, ensure_ascii=True) + "\n"


def main(argv):
    text = merged_cases()
    if "--check" in argv:
        if open(CASES).read() != text:
            print("gen_folio: cases.json is stale; run python3 Tests/Snapshots/gen_folio.py", file=sys.stderr)
            return 1
        print("gen_folio: cases.json is current")
        return 0
    open(CASES, "w").write(text)
    for page in pages:
        print(page["name"], page["columns"], "x", page["rows"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
