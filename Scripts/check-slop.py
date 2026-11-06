#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-slop.py — prose that performs understanding instead of carrying it.

Six rules. Each names a sentence shape that reads as explanation and holds none.
They are separate because they fail differently, are fixed differently, and a
per-rule count says which habit is growing.

    vouching    asserts a quality whose only evidence is the author claiming it
    metaphor    a figure of speech standing where a reason belongs
    hedging     announces significance instead of stating it
    rhetoric    a question or reveal staged for an audience that is not present
    marketing   adjectives from a product page
    deferral    work recorded as postponed instead of done
    shouting    capitals doing the work a sentence should do

Every rule blocks at zero. A budget of permitted violations is unfinished work
handed to the next reader, and the count it settles at is the count it keeps: a
rule held as written-down guidance with no check accumulates sites under it.

DEFERRAL is the rule that matters most, because it is how giving up gets
recorded. "for now", "we could", "a follow-up change will" — each reads as a
plan and commits no one to anything. There is no reader for whom the sentence is
actionable. Either the work belongs in this change, and the sentence is replaced
by the work, or it does not, and the sentence is deleted.

    // Only US QWERTY for now; other layouts later.
    // US QWERTY only. A non-US layout needs the kernel's keymap ioctl, which
    // this driver does not open.

The two are the same length. One of them tells a reader what would have to be
true.

WHY A LIST OF WORDS AND NOT A JUDGEMENT. The defect is a sentence shape and no
word list closes it. What a list does is hold the shapes that were counted, so a
habit cannot regrow silently while everyone agrees it is bad.

TWO WORDS ARE SPELLED THE SAME AND ARE NOT THE SAME WORD, and each is
distinguished by the phrase around it, never by dropping the rule:

    a C++ object "goes out of scope"        a lifetime, not a plan
    a caller "may pass a temporary"         a value category, not a stopgap

That is the only kind of distinction this file makes. A word is NOT excused for
being frequent. Frequency is what a habit looks like, and reading a large count
as evidence that the usage is this codebase's vocabulary is how every one of
these rules would exempt itself.

WHAT IS NOT PROSE. Code is not addressed to a reader, so only comments,
Markdown, and Python docstrings are scanned; a string literal is the program's
payload. Contents are blanked rather than dropped so a finding still reports the
line it is on. The pattern list below sits in code and is blanked with it, and
this file's module docstring is blanked too — a check has to name what it
rejects, and exempting the path by name would exempt every line added to it
afterwards.

    python3 Scripts/check-slop.py
    python3 Scripts/check-slop.py --self-test        the scan's own scope
    python3 Scripts/check-slop.py --staged           the staged files only
    python3 Scripts/check-slop.py --rule deferral    one rule, for a sweep
    python3 Scripts/check-slop.py --message FILE     a commit message

A COMMIT MESSAGE IS WHERE DEFERRAL ACTUALLY GETS WRITTEN, so --message reads one
and applies the deferral rule plus the promises that only appear there: work
parked in a branch, a follow-up nobody is assigned, a next PR that does not
exist. A comment saying `for now` at least sits beside the code it defers; a
commit message saying it addresses nothing at all, because the message is read
once and then only by `git log`.

Scoping is NOT deferral and is not rejected. "This covers the CSI table; the C0
rows are keyed on a code position and are not compared" states a boundary of
what was built, which is what an honest message owes a reader. What the rule
rejects is a FUTURE-TENSE PROMISE about work with no owner and no change behind
it — the two are told apart by tense, and a sentence that names no future does
not match.
"""

import ast
import io
import os
import re
import subprocess
import sys
import tempfile
import tokenize

# Where prose lives. Root-level .md files are added separately: the repository
# root also holds build trees and vendored checkouts, so walking it would trade a
# handful of files for a skip list that has to track every directory added since.
# The tracked tree, not a list of directories. Five named roots left sixteen
# prose-carrying files outside the walk — every .github document, the shell
# integration this ships to users, both Samples, and the root CMakeLists — while
# `--staged` gated them on the way in, because that mode reads the diff rather
# than this list. A file could be checked at commit and never audited in bulk.
SKIP_ROOTS = ("build", ".git")

SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".m", ".mm", ".swift",
            ".js", ".mjs", ".jsx", ".ts", ".tsx", ".py", ".sh", ".cmake",
            ".md", ".toml", ".metal", ".vert", ".frag", ".css", ".html", ".el")
NAMES = ("CMakeLists.txt",)

# A script is a script because of the line at the top, not because of a dot in
# the name. Every git hook in this repository is extensionless, so a population
# keyed on suffix excludes exactly the six files that enforce every other rule —
# and the one that runs this check was among them.
SHEBANG = re.compile(rb"^#!.*\b(?:ba|z|da)?sh\b|^#!.*\bpython3?\b")
# A fuzz corpus is bytes, and a corpus entry beginning 0x23 0x21 reads as a
# shebang. Nothing under a corpus directory is source.
BINARY_DIRECTORIES = ("fuzz-corpus", "tiff-corpus", "sixel-corpus", "png-corpus")


def hasShebang(path):
    try:
        with open(path, "rb") as handle:
            return SHEBANG.match(handle.readline(256)) is not None
    except OSError:
        return False

# Vendored sources carry their upstream's prose and are not ours to rewrite;
# a licence text is a legal instrument whose wording is fixed by its author.
# Audits holds audit reports that quote findings verbatim; it never ships.
SKIP_DIRECTORIES = {"ThirdParty", "third-party-licenses", "__pycache__",
                    "node_modules", ".git", "Audits"}
# A code of conduct is the Contributor Covenant adopted verbatim — rewording it
# makes it a different document that looks like the one people recognise.
SKIP_PATHS = {os.path.join(".github", "code_of_conduct.md")}

C_FAMILY = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".m", ".mm", ".swift",
            ".js", ".mjs", ".jsx", ".ts", ".tsx", ".metal", ".vert", ".frag",
            ".css", ".html"}
RAW_STRING_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".m", ".mm"}
BACKTICK_SUFFIXES = {".js", ".mjs", ".jsx", ".ts", ".tsx", ".html"}
HASH_COMMENT_SUFFIXES = {".py", ".sh", ".cmake", ".toml"}

# Where a Python string literal is prose rather than data.
#
# For code, a literal is a value and blanking it is right: a rule firing inside
# a parser table says nothing. The showcase inverts that: its literals are
# themselves the product's prose — every "Look for:" a reader checks the screen against, and
# every "Expected:" that states what correct looks like — and the reader is
# the person the rules exist for. So they are scanned as prose: blanked as code,
# they would let the tree-wide run report the showcase clean whatever it says.
#
# Examples/ is excluded on purpose: those files impersonate other systems (a
# VMS shell, a BBS), and their literals are that system's output, not this
# repository addressing anyone.
PROSE_STRING_ROOTS = (os.path.join("Tools", "showcase", "sections"),)
PROSE_STRING_EXCLUDED = (os.path.join("Tools", "showcase", "Examples"),)


def carries_prose_strings(path):
    """Whether this file's string literals are prose addressed to a reader."""
    normalized = os.path.normpath(os.path.abspath(path))
    for excluded in PROSE_STRING_EXCLUDED:
        if os.sep + excluded + os.sep in normalized + os.sep:
            return False
    for root in PROSE_STRING_ROOTS:
        if os.sep + root + os.sep in normalized + os.sep:
            return True
    return False

NOT_NEWLINE = re.compile(r'[^\n]')

# Shouting is caught on the CLOSED CLASS — function words, auxiliaries,
# pronouns, prepositions — and never on content words. That distinction is what
# separates a raised voice from a name: this is a terminal emulator, so OSC,
# `HELLO`, `SNAPSHOT`, `CUP` and `BEL` are all-caps because that is their
# spelling, and a
# dictionary cannot tell them from a shouted noun. A closed class can, because
# English does not coin new prepositions. No token outside this list is judged,
# which leaves a lone shouted noun uncaught, and that boundary is stated
# rather than implied: the
# alternative needs a list of every acronym in the tree, and 1,271 of them are
# in prose today.
#
# Two spellings here are not English at all:
#
#   `YES` / `NO` in .m, .mm   the Objective-C boolean literals
#   `OR'd`, `AND-gate`        the bitwise operator as a verb or a compound
#   `OR of`, `OR over`        the bitwise operator folded across a range
#   `AT-SPI`, `NO-OP`         a closed-class word inside a hyphenated acronym
#   `ESC CAN`, `CAN / SUB`    the ASCII control character 0x18, not the modal verb
#   `0x18 (CAN)`              the same character named after its own byte value
#
# `CAN` is the same collision as `AT`, told apart by what sits beside it. Its C0
# siblings are unaffected because none of them is also an English word: NUL, BEL,
# SUB, ENQ, ETX and SOH appear in capitals throughout the tree. Judging `CAN`
# alone would make this one control character the only member of that set a
# comment has to spell in lowercase, so the gate would be editing the vocabulary
# rather than the voice.
#
# The boundary, stated: a genuinely shouted `CAN` immediately after `ESC ` or
# `0x18 (`, or immediately before `/ SUB`, goes uncaught. None of the three is a
# sentence.
#
# The last one is why every entry carries a `(?!['\-])` lookahead rather than
# only the operators. `AT` is a preposition and the first half of the Linux
# accessibility protocol's name, and a rule that judges the token without
# looking at what follows it rewrites the protocol.
#
# The cost is stated rather than hidden: a genuinely shouted hyphenated phrase
# goes uncaught. Telling `BYTE-FOR-BYTE` from `AT-SPI` needs to know which
# halves are English, and that needs a word list — which the Linux container
# has no copy of, so the gate would pass there for the wrong reason.
#
# `XOR` is absent from the list entirely: it is not an English word in any
# spelling, so every occurrence is the operator.
#
# `SO` is absent for the opposite reason. It is an English word, and in this
# codebase it is overwhelmingly not one. Shift Out is C0 0x0E, and of the 19
# occurrences the first sweep touched, 18 named the control and 1 was a shouted
# conjunction — which still trips on `THE`, `IS` and `IT` in the same phrase, so
# the rule loses nothing by dropping it. A word whose domain sense outnumbers
# its English sense 18 to 1 rewrites vocabulary rather than voice.
#
# Both are the language quoted rather than the voice raised, and both are told
# apart by what sits beside them, not by a path.
SHOUTING = re.compile(r"""\b(?:AND|OR)\b(?!['\-]|\s+(?:of|over|fold|mask|together|with)\b)
|\b(?:THE|BUT|NOT|IS|ARE|WAS|WERE|BE|BEEN|BEING
|THIS|THAT|THESE|THOSE|ALL|ANY|EACH|EVERY|BOTH|SOME|NONE|ONLY|EVEN|ALSO|VERY
|MUCH|MORE|MOST|LESS|SAME|OTHER|SUCH|THAN|IF|THEN|ELSE|WHEN|WHERE|WHY|HOW|WHAT
|WHICH|WHO|WHOSE|BEFORE|AFTER|DURING|WITHOUT|WITHIN|MUST|SHOULD|WOULD|CANNOT
|MIGHT|SHALL|NEVER|ALWAYS|EXACTLY|ALREADY|STILL|EITHER|NEITHER|RATHER|INSTEAD
|TWICE|ANYTHING|NOTHING|EVERYTHING|SOMETHING|ANYONE|EVERYONE|NOBODY|ITSELF
|THEMSELVES|CRITICAL|IMPORTANT|WARNING|CAUTION|BEWARE|REMEMBER|DANGER|IT|ITS
|AS|AT|TO|BY|OF|FOR|FROM|IN|INTO|ONTO|UP|OUT|OVER|UNDER|DO|DOES|DID|DONE|HAS
|HAVE|HAD|COULD|WE|YOU|THEY|THEM|THEIR|OUR|YOUR|YES|NO|NOR|YET|ONCE|HERE
|THERE|NOW)\b(?!['\-])
|(?<!ESC\ )(?<!0x18\ \()\bCAN\b(?!['\-]|\s*/\s*SUB)""", re.VERBOSE)

# A licence granted inline is a legal instrument, and its capitals are the
# form the licence is granted in. Rewording it changes what was granted, so the
# disclaimer's fixed sentences are skipped wherever they appear rather than by
# listing the one file that carries them today.
LICENCE_TEXT = re.compile(
    r'THE SOFTWARE IS PROVIDED|WITHOUT WARRANTY OF ANY KIND|'
    r'IMPLIED, INCLUDING|MERCHANTABILITY|IN NO EVENT SHALL|'
    r'WHETHER IN AN ACTION|ARISING FROM, OUT OF|DEALINGS IN THE SOFTWARE|'
    r'Permission is hereby granted')

# A capitalised word that names something rather than emphasising it.
#
# Some protocols spell their own commands in capitals, and prose about them has
# to write them as they are: Telnet negotiates with IAC WILL/WONT and DO/DONT,
# a transfer reports status=DONE, and AT&T named a terminal. The shouting rule
# reads those as English words doing a sentence's work, which is what they are
# everywhere else — so the exemption is the surrounding token, not the
# word. Written as the licence exemption is: the shape that makes it a name.
#
# An SPDX licence expression joins licence identifiers with `AND`, `OR` and `WITH`,
# and the identifiers on both sides are what make the capitals operators.
SPDX_IDENTIFIER = r'(?:MIT|ISC|ICU|Zlib|Unlicense|Apache-\d|FSL-\d|BSD-\d|OFL-\d|CC-BY-\d|CC0-\d|L?GPL|MPL-\d|PSF-\d|Unicode-\d|Bitstream-Vera|LicenseRef-)'
NAMED_TOKENS = re.compile(
    r'\bIAC\b|\bWILL/WONT\b|\bDO/DONT\b|\bSB\b\s|AT&T|status=DONE'
    r'|' + SPDX_IDENTIFIER + r'[\w.+-]*\s+(?:AND|OR|WITH)\s+' + SPDX_IDENTIFIER)

OBJC_SUFFIXES = {".m", ".mm"}
OBJC_LITERALS = re.compile(r'\b(?:YES|NO)\b')

# `one could` is absent from the deferral pattern and named here instead. "one"
# is both the indefinite pronoun of a deferral ("one could add X") and an object
# pronoun ("a page that received one could only hand it back"), and nothing in
# the line separates them. "we" is always a subject, so its only false reading is
# a temporal subordinate clause — a closed set of conjunctions, excluded below.
#
# The two lifetime senses, subtracted before the deferral pattern is applied.
# Both are C++ terms of art that share a spelling with a stopgap.
LIFETIME_SENSES = re.compile(
    r'goes? out of scope|going out of scope|went out of scope'
    r'|(?:a|the|any|its|this) temporar(?:y|ies)\b'
    r"|temporary's\b|pass a temporary|bound to a temporary", re.IGNORECASE)

# Promises that appear in a commit message rather than in a comment. Each names
# work with no change behind it and no one carrying it. A boundary statement in
# the past or present tense — what this change covers, what it does not touch —
# matches none of them, because it promises nothing.
MESSAGE_DEFERRAL = re.compile(
    r'\b(?:parked|will (?:follow|land|come|be (?:done|added|fixed|wired))'
    r'|(?:a |the )?(?:later|future|separate|second) (?:commit|change|patch|PR|pass)'
    r'|follow-?up (?:commit|change|patch|pass|PR)'
    r'|next (?:PR|commit|patch|change)|another PR'
    r'|(?:I|we)(?:\'ll| will) (?:do|fix|add|wire|handle|land|get)'
    r'|left (?:for later|to a follow|as an exercise)'
    r'|to be (?:done|added|implemented|decided) (?:later|in|by)'
    r'|deferred to a|punt(?:ed|ing)? (?:on|to)|revisit(?:ed)? later'
    r'|when time permits|coming soon|for a future)\b', re.IGNORECASE)

# The prose-deferral rule, compiled once so --message shares the exact list
# the comment scan blocks on. One rule, one spelling of it.
RULES = (
    ("vouching",
     r'\b(?:deliberat(?:e|ely)|hone(?:st|stly)|carefully|properly|'
     r'intentional(?:ly)?|obviously|of course|naturally|clearly|simply|basically|note that)\b',
     re.IGNORECASE,
     "asserts a quality where a reason belongs"),

    ("marker",
     r'\b(?:XXX|HACK|FIXME|TODO)\s*:',
     0,
     "a deferral marker in shipped code"),

    ("metaphor",
     r'\b(?:load-bearing|the whole point|the crux|at its heart|'
     r'the key insight|the real question|worth noting|keep in mind|'
     r'bear in mind)\b',
     re.IGNORECASE,
     "a figure of speech where a reason belongs"),

    ("hedging",
     r'\b(?:crucial(?:ly)?|fundamental(?:ly)?|arguabl(?:y|e)|in other words|'
     r'that said|in short,|simply put)',
     re.IGNORECASE,
     "announces significance instead of stating it"),

    ("rhetoric",
     r'(?:\bHere\'s the thing\b|\bThe (?:result|problem|catch|answer)\?'
     r'|\bWhy does this matter\b|\bTL;DR\b|\bIn summary\b|\bTo summarize\b'
     r'|\bnot just .*\bbut\b)',
     re.IGNORECASE,
     "staged for an audience that is not reading this"),

    ("marketing",
     r'\b(?:robust|seamless(?:ly)?|elegant(?:ly)?|powerful|comprehensive|'
     r'holistic|streamlined|cutting-edge|state-of-the-art|best-in-class|'
     r'delve|tapestry|testament to|blazingly)\b',
     re.IGNORECASE,
     "an adjective from a product page"),

    # Case-sensitive markers: the convention is capitals, and a lowercase
    # spelling occurs inside a feature's own name.
    ("deferral",
     r'\b(?:TODO|FIXME|XXX|HACK)\b',
     0,
     "a marker recording work as postponed"),

    # An experiment left in, which is a different thing from work postponed: a
    # probe changes what the code does, and it ships looking like a decision.
    # A `// PROBE: no overlay` that zeroes a highlight alpha is caught only by a
    # test asserting the literal it replaced.
    #
    # The labelled form is required for `PROBE` because the bare word has a
    # legitimate use: `TempFileUnlink` in Source/Core/Kitty/Graphics.cpp says
    # `an a=q PROBE Preserves it`, where it is the protocol's own vocabulary.
    # Measured: `REMOVEME`, `DONOTCOMMIT`, `SCAFFOLD` and `WIP` occur nowhere in
    # Source, so those need no colon to be unambiguous.
    ("probe",
     r'\b(?:PROBE|SANITY|SCRATCH|TEMP):'
     r'|\b(?:REMOVEME|DONOTCOMMIT|SCAFFOLD|WIP)\b',
     0,
     "an experiment left in the shipping source"),

    ("deferral",
     r'(?:\bfor now\b|\blater on\b|\bcoming soon\b|\bfuture work\b'
     r'|\bnext step\b|\bfollow-?up (?:commit|change|patch|pass)\b'
     r'|\bwhen time permits\b|(?<!before )(?<!than )(?<!until )\bwe could\b|\bwould need to\b'
     r'|\bideally\b|\beventually\b|\brevisit\b|\bcome back to\b|\bpunt\b'
     r'|\bnot yet (?:implemented|supported|wired|handled|done|covered)\b'
     r'|\bunimplemented\b|\bstubbed\b|\bstill (?:to do|needs? to)\b'
     r'|\bremaining (?:work|cases|sites|tests)\b'
     r'|\bto be (?:added|done|implemented|decided)\b'
     r'|\bout of scope\b)',
     re.IGNORECASE,
     "records work as postponed instead of doing it"),
)

DEFERRAL_PROSE = re.compile(
    next(pattern for name, pattern, _, _ in RULES
         if name == "deferral" and "for now" in pattern),
    re.IGNORECASE)

ADVICE = {
    "probe": "Delete the experiment, or land what it proved. A probe changes\n"
             "             what the code does and ships looking like a decision —\n"
             "             one shipped into Core, and the only thing that caught it\n"
             "             was a test asserting the literal it replaced.",
    "vouching": "Say WHAT the word stood in for. If nothing is left after the\n"
                "             rewrite, the sentence was carrying nothing — delete it.",
    "marker": "Finish it or revert it. A marker survives review, the gates and a\n"
              "             regenerated golden — one reached main as an alpha of 0.0 on\n"
              "             a shipped Core colour, caught by a unit case pinning the\n"
              "             literal beside a pairing check.",
    "metaphor": "Replace the figure with the mechanism it stands for.",
    "hedging": "State the thing. A reader decides whether it is significant.",
    "rhetoric": "Answer the question rather than posing it.",
    "marketing": "Name the property and how it is measured.",
    "shouting": "Write the sentence so the emphasis is unnecessary. Capitals\n"
                "             carry no meaning a reader can act on, and a page of them\n"
                "             makes the one that mattered invisible.",
    "deferral": "Do the work, or delete the sentence. A note that some later\n"
                "             change will handle it names no change and no owner, and\n"
                "             the reader who needs it cannot act on it either way.",
}


def blanked(segment):
    """The segment with every character but a newline replaced by a space."""
    return NOT_NEWLINE.sub(' ', segment)


def emit_literal(text, out, start, body_start, terminator, escapes, one_line):
    """Appends one literal with its body blanked; returns the index past it.

    `one_line` closes the literal at a newline as well as at its terminator: a
    quote left open by a line that never closes it must not blank the rest of the
    file. An unterminated multi-line literal is the file's own defect, and
    blanking to the end is the reading of it that cannot report a phantom.
    """
    length = len(text)
    index = body_start
    while index < length:
        if escapes and text[index] == '\\':
            index += 2
            continue
        if text.startswith(terminator, index):
            out.append(blanked(text[start:body_start]))
            out.append(blanked(text[body_start:index]))
            out.append(blanked(terminator))
            return index + len(terminator)
        if one_line and text[index] == '\n':
            break
        index += 1
    end = min(index, length)
    out.append(blanked(text[start:body_start]))
    out.append(blanked(text[body_start:end]))
    return end


def identifier_char(text, index):
    return 0 <= index < len(text) and (text[index].isalnum() or text[index] == '_')


# One pass in C instead of a character loop in Python. The alternation order is
# the scanner's precedence and carries the same rule the loop did: a comment is
# recognised before a quote, so an apostrophe in `// don\'t` opens no character
# literal and a `"` inside a comment closes nothing.
C_TOKENS = re.compile(r"""
      //[^\n]*                      # line comment
    | /\*.*?(?:\*/|\Z)              # block comment, unterminated runs to EOF
    | R"([^()\\ ]{0,16})\(.*?\)\1"  # raw string
    | \"\"\".*?(?:\"\"\"|\Z)         # Swift multi-line string
    | "(?:\\.|[^"\\\n])*(?:"|$)     # string, closed by a newline if unterminated
    | '(?:\\.|[^'\\\n])*(?:'|$)     # character literal
    | `(?:\\.|[^`\\])*(?:`|\Z)      # template literal
""", re.DOTALL | re.VERBOSE)


def c_family_comments(text, raw, backtick):
    """C-family source reduced to its comment bodies, everything else blanked.

    A comment is recognised before a quote is: an apostrophe in `// don't` opens
    no character literal, and a `"` inside a comment closes nothing. Code is
    blanked rather than dropped so an identifier can never be read as prose — a
    function named for a phrase is not the phrase.
    """
    out = []
    position = 0
    for match in C_TOKENS.finditer(text):
        token = match.group(0)
        if token.startswith('`') and not backtick:
            continue
        if token.startswith('R"') and not raw:
            continue
        out.append(blanked(text[position:match.start()]))
        out.append(token if token.startswith('//') or token.startswith('/*')
                   else blanked(token))
        position = match.end()
    out.append(blanked(text[position:]))
    return ''.join(out)


def c_family_comments_reference(text, raw, backtick):
    """The character loop the scanner replaced, kept for the equivalence test."""
    out = []
    index, length = 0, len(text)
    while index < length:
        char = text[index]
        if char == '/' and text.startswith('//', index):
            end = text.find('\n', index)
            end = length if end < 0 else end
            out.append(text[index:end])
            index = end
        elif char == '/' and text.startswith('/*', index):
            end = text.find('*/', index + 2)
            end = length if end < 0 else end + 2
            out.append(text[index:end])
            index = end
        elif raw and char == 'R' and text.startswith('R"', index) \
                and not identifier_char(text, index - 1):
            # R"delim( … )delim" — the delimiter is bounded at 16 characters by
            # the standard, so a `(` further out than that is not one.
            paren = text.find('(', index + 2)
            if 0 < paren <= index + 18:
                index = emit_literal(text, out, index, paren + 1,
                                     ')%s"' % text[index + 2:paren],
                                     escapes=False, one_line=False)
            else:
                out.append(' ')
                index += 1
        elif text.startswith('"""', index):
            index = emit_literal(text, out, index, index + 3, '"""',
                                 escapes=True, one_line=False)
        elif char in '"\'' or (backtick and char == '`'):
            index = emit_literal(text, out, index, index + 1, char,
                                 escapes=True, one_line=(char != '`'))
        else:
            out.append('\n' if char == '\n' else ' ')
            index += 1
    return ''.join(out)


def python_comments(text):
    """Keeps Python comments and actual docstrings at their source offsets.

    Assigned multiline strings are data, even when a closing delimiter is the
    first token on its line. Syntax identifies docstrings; tokenization keeps
    comment markers inside strings from becoming prose.
    """
    try:
        tree = ast.parse(text)
        comments = [token for token in tokenize.generate_tokens(io.StringIO(text).readline)
                    if token.type == tokenize.COMMENT]
    except (SyntaxError, tokenize.TokenError):
        # Unparseable source stays visible to the gate rather than acquiring an
        # exemption for every reference or rule violation it might contain.
        return text

    lines = text.split("\n")
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line) + 1)
    result = list(blanked(text))

    def keep(start, end):
        first = offsets[start[0] - 1] + start[1]
        last = offsets[end[0] - 1] + end[1]
        result[first:last] = text[first:last]

    def syntax_position(line, column):
        # AST columns count UTF-8 bytes; token columns and string slices count
        # Unicode characters. Preserve the source offsets in either spelling.
        return line, len(lines[line - 1].encode("utf-8")[:column].decode("utf-8"))

    for token in comments:
        keep(token.start, token.end)
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        if ast.get_docstring(node) is None:
            continue
        docstring = node.body[0]
        keep(syntax_position(docstring.lineno, docstring.col_offset),
             syntax_position(docstring.end_lineno, docstring.end_col_offset))
    return ''.join(result)


def hash_comments(text, docstrings):
    """Lines reduced to their `#` comment, optionally keeping docstrings.

    A `#` inside a quoted value opens no comment, so quotes are tracked across
    the line. Docstrings are this repository's primary prose — every check in
    Scripts documents its own defect there — so for Python they are kept.
    """
    if docstrings:
        return python_comments(text)
    out = []
    for line in text.split("\n"):
        cut, quote, index = None, None, 0
        while index < len(line):
            char = line[index]
            if quote:
                if char == '\\':
                    index += 2
                    continue
                if char == quote:
                    quote = None
            elif char in '"\'':
                quote = char
            elif char == '#':
                cut = index
                break
            index += 1
        out.append(blanked(line[:cut]) + line[cut:] if cut is not None else blanked(line))
    return "\n".join(out)


def blank_module_docstring(text):
    """Blanks the leading triple-quoted block, keeping line numbers.

    Only the module docstring, so a rule word in this file's own comments is
    still reported. Line count is preserved because a finding names a line.
    """
    opener = text.find('"""')
    if opener < 0:
        return text
    closer = text.find('"""', opener + 3)
    if closer < 0:
        return text
    return text[:opener] + '\n' * text[opener:closer + 3].count('\n') + text[closer + 3:]


# A term inside backticks is being MENTIONED, not used: a rules document has to
# name what it forbids, and this check's own docstring is the same case. Blanking
# code spans is what lets those files be written without a path exemption, which
# would go on excusing every line added to them afterwards. It also settles the
# typography — a word under discussion is a term, and a term is set as code.
#
# Three delimiters mark a mention, and each is bounded so it cannot swallow a
# paragraph: a code span, a short quotation (a cited rule, a word under
# discussion), and a short bold span (a defined term — the five product pillars
# are named in bold, and a pillar's name is the document's subject rather than a
# claim inside it). A quotation or a bold run longer than that is prose making an
# argument, and it is read as prose.
MENTIONS = re.compile(
    r'`+[^`\n]*`+'
    r'|"[^"\n]{1,60}"'
    r'|“[^”\n]{1,60}”'
    r'|\*\*[A-Za-z][A-Za-z -]{0,24}\*\*')


def blank_mentions(text):
    """Terms and quotations blanked: a word named is not a word used.

    A rules document has to name what it forbids, and this check's own docstring
    is the same case. Without this they would need a path exemption, which goes
    on excusing every line added to those files afterwards.
    """
    return MENTIONS.sub(lambda match: blanked(match.group(0)), text)


def prose(path, text):
    """The part of a file addressed to a reader, with everything else blanked."""
    suffix = os.path.splitext(path)[1]
    if suffix == ".md":
        return blank_mentions(text)
    if suffix in C_FAMILY:
        return c_family_comments(text, suffix in RAW_STRING_SUFFIXES,
                                 suffix in BACKTICK_SUFFIXES)
    # A shebang script has no suffix to dispatch on, and falling through to the
    # bottom returns the file whole — so every heredoc banner and echoed message
    # is read as prose. The rules then fire on a gate's own printed output.
    if (suffix in HASH_COMMENT_SUFFIXES or os.path.basename(path) in NAMES
            or (not suffix and text.startswith("#!"))):
        if suffix == ".py" and carries_prose_strings(path):
            # The literals are the prose here, so the file is read whole: the
            # code around them is identifiers and punctuation, which the
            # sentence-shaped rules do not match.
            return text
        reduced = hash_comments(text, docstrings=(suffix == ".py"))
        if os.path.abspath(path) == os.path.abspath(__file__):
            return blank_module_docstring(reduced)
        return reduced
    return text


# The PRINTED probe: a diagnostic string an experiment writes per frame or per
# call. The prose scan cannot see it — `prose` blanks string literals for the
# C family, which is correct for the seven sentence-shape rules and exactly
# wrong for this one. Two of them once shipped into the render loop
# spelled `"PROBE drawBackgroundImageOn: ..."`, where the labelled `PROBE:`
# form matches nothing: the colon ends the call-site name, not the marker.
# Matched against the RAW text with only BACKTICK spans blanked — the general
# mention-blanking erases every double-quoted string, which is the very literal
# this hunts. A comment quoting an incident wraps the string in backticks and
# stays a quotation; the word boundary keeps `"PROBE_BAUD"` — an environment
# variable's own name — out.
PRINTED_PROBE = re.compile(r'"PROBE\b')
BACKTICK_SPAN = re.compile(r'`[^`]*`')


# Compiled once. Storing the rules as strings sends every match through
# re.search's pattern cache — a dictionary probe per match over the whole tree,
# for a pattern set that never changes.
COMPILED_RULES = tuple((name, re.compile(pattern, flags), explanation)
                       for name, pattern, flags, explanation in RULES)



def examine(path, wanted):
    """Returns [(line number, rule, explanation, the line as written)]."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    readable = blank_mentions(prose(path, text))

    # Almost every file is clean, so ask the whole file before asking each line.
    # A rule that cannot match anywhere in the text cannot match in any of its
    # lines, and skipping it there turns eight regexes per line into eight per
    # file. The scan below is unchanged for a file that does hit.
    #
    # It asks that question of the comments alone. `prose` blanks code rather
    # than removing it, because the per-line scan below indexes by line number
    # and needs the offsets kept — which leaves most of what a rule scans as
    # spaces and newlines.
    #
    # Dropping the blank lines here cannot lose a finding. This decides only
    # whether the per-line scan runs, and that scan is what emits. Joining
    # comment blocks that code separated can bring a rule alive on a file where
    # no single line matches, and the cost of that is the scan running and
    # finding nothing.
    probe = "\n".join(line for line in readable.split("\n") if line.strip())
    live = [rule for rule in COMPILED_RULES
            if (not wanted or rule[0] == wanted) and rule[1].search(probe)]
    shouts = SHOUTING.search(probe) and (not wanted or wanted == "shouting")
    printedProbes = []
    if ((not wanted or wanted == "probe")
            and os.path.abspath(path) != os.path.abspath(__file__)
            and PRINTED_PROBE.search(text)):
        for number, rawLine in enumerate(text.split("\n"), 1):
            if PRINTED_PROBE.search(BACKTICK_SPAN.sub(" ", rawLine)):
                printedProbes.append((number, "probe",
                                      "a printed probe in the shipping source",
                                      rawLine.strip()))
    if not live and not shouts and not printedProbes:
        return []

    original = text.split("\n")
    objc = os.path.splitext(path)[1] in OBJC_SUFFIXES
    findings = printedProbes
    for number, line in enumerate(readable.split("\n"), 1):
        if LICENCE_TEXT.search(line):
            continue
        if NAMED_TOKENS.search(line):
            continue
        shouted = OBJC_LITERALS.sub(' ', line) if objc else line
        if shouts and SHOUTING.search(shouted):
            findings.append((number, "shouting",
                             "capitals doing the work a sentence should do",
                             original[number - 1].strip()))
            continue
        for name, pattern, explanation in live:
            searched = LIFETIME_SENSES.sub(' ', line) if name == "deferral" else line
            if pattern.search(searched):
                findings.append((number, name, explanation, original[number - 1].strip()))
                break
    return findings


def stagedSources(root):
    """The staged files this check would otherwise walk the tree to reach.

    The tree sits at zero, so a commit can only introduce a violation through a
    file it stages. Scanning those instead of all 1,449 turns a 7.4-second wait
    on every commit into a few milliseconds, and check-style still scans
    everything, which is where a rule gaining a new word gets validated against
    prose nobody is touching.
    """
    listing = subprocess.run(
        ["git", "-C", root, "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        capture_output=True, text=True)
    for path in listing.stdout.split("\n"):
        if not path:
            continue
        full = os.path.join(root, path)
        if not os.path.isfile(full):
            continue
        if any(part in SKIP_DIRECTORIES for part in path.split(os.sep)):
            continue
        if path.endswith(SUFFIXES) or os.path.basename(path) in NAMES:
            yield full


def changedSources(root, reference):
    """The files this branch changed, which are the only ones it can have spoiled.

    The argument stagedSources makes, one scope out. main sits at zero, so a
    branch introduces a violation only through a file it touched, and the push
    gate is asking about the branch. The whole-tree pass is what keeps that
    premise true and it still runs: the CI workflow calls this script with no
    ref on every push, which is where a rule gaining a new word meets prose
    nobody is editing.

    - Parameters:
      - root: the repository root.
      - reference: the branch to compare against, usually origin/main.
    - Returns: yields each changed file this check reads.
    """
    listing = subprocess.run(
        ["git", "-C", root, "diff", "--name-only", "--diff-filter=ACMR",
         "%s...HEAD" % reference],
        capture_output=True, text=True)
    if listing.returncode != 0:
        return
    for path in listing.stdout.split("\n"):
        if not path:
            continue
        full = os.path.join(root, path)
        if not os.path.isfile(full):
            continue
        if any(part in SKIP_DIRECTORIES for part in path.split(os.sep)):
            continue
        if path.endswith(SUFFIXES) or os.path.basename(path) in NAMES:
            yield full


def sources(root):
    """Every tracked file that carries prose, from git rather than from a walk.

    A directory walk has to be told which build trees to avoid, and it is told
    once — so `BuildBench/` and `BuildHostTests/` were scanned as soon as the
    root list went away, reporting generated `.gen.cpp` output as prose. The
    tracked set has no build tree in it by construction, which is the same
    property stated once instead of maintained, and `--exclude-standard` keeps
    that true for the untracked half.

    `--others` is why a file being added is in the set. The pre-commit hook
    judges staged paths, so a new file is already inside the enforcer's subject;
    a tracked-only listing would report clean on the one file that has never
    been read by anything.
    """
    listing = subprocess.run(
        ["git", "-C", root, "ls-files", "--cached", "--others", "--exclude-standard"],
        capture_output=True, text=True)
    for path in sorted(listing.stdout.split("\n")):
        if not path:
            continue
        parts = path.split("/")
        if any(part in SKIP_DIRECTORIES or part in BINARY_DIRECTORIES
               for part in parts[:-1]):
            continue
        name = parts[-1]
        full = os.path.join(root, path)
        if not os.path.lexists(full):
            continue
        if name.endswith(SUFFIXES) or name in NAMES:
            yield full
        elif "." not in name and hasShebang(full):
            yield full


def addedLineNumbers(root, ref):
    """Line numbers this branch added since REF, keyed by relative path.

    A whole-tree scan reports the tree's state, and a push is only when someone
    reads that state — so a finding on a line nobody on this branch touched
    reads as the pusher's, and the only cost of the misread is the minute spent
    ruling out a diff that never contained the line. These are the lines the
    branch is answerable for: `git diff REF...HEAD` resolves to the changes since
    the branch left REF. A finding outside this set is somebody else's, and the
    report says so rather than narrowing what it scans — a scan that stops seeing
    untouched lines stops catching what lands through a path it never ran on.

    It is committed work only, so an uncommitted violation attributes as not the
    branch's — harmless in the push path, where HEAD carries everything by then,
    and the pre-commit `--staged` path takes no ref so it never attributes at all.
    """
    diff = subprocess.run(
        ["git", "-C", root, "diff", "--unified=0", "%s...HEAD" % ref],
        capture_output=True, text=True)
    if diff.returncode != 0:
        return None
    added, current = {}, None
    for line in diff.stdout.split("\n"):
        if line.startswith("+++ b/"):
            current = line[6:]
            added.setdefault(current, set())
        elif line.startswith("@@") and current is not None:
            match = re.search(r"\+(\d+)(?:,(\d+))?", line)
            if match:
                start = int(match.group(1))
                count = int(match.group(2)) if match.group(2) is not None else 1
                added[current].update(range(start, start + count))
    return added


def checkMessage(path):
    """Reads a commit message and reports deferral in it.

    Comment lines (`#`) are git's own template and are not the author's prose.
    A trailer block is machine-read and names no work, so it is skipped too.
    """
    with open(path, encoding="utf-8", errors="replace") as handle:
        lines = handle.read().split("\n")
    findings = []
    for number, line in enumerate(lines, 1):
        if line.startswith("#") or re.match(r'^[A-Z][A-Za-z-]*: ', line):
            continue
        text = blank_mentions(line)
        # A message shouts for the same reason a comment does, and a heading in
        # capitals is the commonest form of it here.
        shout = SHOUTING.search(text)
        if shout:
            findings.append((number, shout.group(0), line.strip()))
            continue
        for pattern in (MESSAGE_DEFERRAL, DEFERRAL_PROSE):
            match = pattern.search(text)
            if match:
                findings.append((number, match.group(0), line.strip()))
                break
    if not findings:
        return 0
    print("check-slop: this commit message defers work, or shouts",
          file=sys.stderr)
    for number, phrase, line in findings:
        print("  line %d: \"%s\"" % (number, phrase), file=sys.stderr)
        print("    %s" % line[:100], file=sys.stderr)
    print("""
  A commit message is where work gets given up on, because the sentence sits
  beside no code and is read once. Naming a later change commits no one: there
  is no such change, no owner, and nothing a reader can act on.

  Do the work in this commit, or drop the sentence.

  A boundary is not a deferral, and none of these match one. "This covers the
  CSI rows; the C0 section is keyed on a code position and is not compared"
  says what was built and what was not, in the present tense, promising
  nothing. Say that instead.""", file=sys.stderr)
    return 1


def self_test():
    """The scan reads each file class as the prose it carries, and no other.

    What a string literal means depends on where it is. In code it is a value,
    and a rule firing inside a parser table says nothing about anyone's writing.
    In the showcase's sections the literals are the product's prose — the
    "Look for:" lines a reader checks the screen against — so blanking them hid
    thirteen sites, on 2026-08-24, from a run that called the tree clean. The
    cases below hold
    both halves of that, because widening the scan is only safe while the
    narrower classes stay narrow.
    """
    status = 0

    def check(label, condition):
        nonlocal status
        if condition:
            print("check-slop self-test: ok — %s" % label)
        else:
            print("check-slop self-test: FAIL — %s" % label)
            status = 1

    shouting_literal = 'text = "the cursor does NOT advance"\n'
    showcase = os.path.join("Tools", "showcase", "sections", "demo.py")
    example = os.path.join("Tools", "showcase", "Examples", "vms-shell.py")
    ordinary = os.path.join("Scripts", "tool.py")

    check("a showcase section's literal is prose",
          bool(SHOUTING.search(prose(showcase, shouting_literal))))
    check("an Examples literal is the impersonated system's output, not prose",
          not SHOUTING.search(prose(example, shouting_literal)))
    check("an ordinary script's literal stays a value",
          not SHOUTING.search(prose(ordinary, shouting_literal)))
    check("a comment is prose in every Python file",
          bool(SHOUTING.search(prose(ordinary, "# the cursor does NOT advance\n"))))

    # The named-token exemption covers a protocol's own spelling and nothing
    # wider: DO is a Telnet command beside IAC, and an ordinary word elsewhere.
    check("a protocol's command name is not shouting",
          bool(NAMED_TOKENS.search("IAC WILL/WONT (offer) and DO/DONT (request)")))
    check("the exemption does not cover a bare emphasis",
          not NAMED_TOKENS.search("the cursor does NOT advance"))
    check("an SPDX licence expression's operators are not shouting",
          bool(NAMED_TOKENS.search("| MIT AND BSD-3-Clause (selects the upstream MIT option) |")))
    check("a data licence expression's operators are not shouting",
          bool(NAMED_TOKENS.search("// SPDX-License-Identifier: Apache-2.0 AND PSF-2.0 AND Unicode-3.0 AND ICU")))
    check("a capital AND between ordinary words is still shouting",
          not NAMED_TOKENS.search("the reader AND the writer"))

    # A rule with no advice is unactionable, which is what main checks before
    # reading a file; the self-test states it so a new rule cannot land without.
    check("every rule names what to do instead",
          not ({name for name, _, _, _ in RULES} - set(ADVICE)))

    # Several paths are several files. Taking the last as a root dropped every
    # file named before it, so a run over a bad file and a good one reported
    # the good one and passed — a scan of one file wearing the verdict of two.
    with tempfile.TemporaryDirectory() as directory:
        bad = os.path.join(directory, "bad.h")
        good = os.path.join(directory, "good.h")
        with open(bad, "w", encoding="utf-8") as handle:
            handle.write("// a comment that IS shouting\n")
        with open(good, "w", encoding="utf-8") as handle:
            handle.write("// a comment stating a constraint\n")
        check("a violation in the first of several paths is still reported",
              main(["check-slop", bad, good]) != 0)
    return status


def main(argv):
    # RULES and ADVICE are two tables that have to agree, and nothing made them.
    # A rule added without advice raised KeyError from the summary — after every
    # file had been read and every finding printed, so the run looked like a
    # crash in the reporting rather than a missing line in a table.
    missing = sorted({name for name, _, _, _ in RULES} - set(ADVICE))
    if missing:
        print("check-slop: rule(s) with no ADVICE entry: %s" % ", ".join(missing),
              file=sys.stderr)
        print("  Every rule names what to do instead; a rule that cannot say so "
              "is not ready to fire.", file=sys.stderr)
        return 2

    if "--self-test" in argv[1:]:
        return self_test()

    root, wanted, staged, attribute, since = ".", None, False, None, None
    paths = []
    arguments = argv[1:]
    while arguments:
        item = arguments.pop(0)
        if item == "--staged":
            staged = True
        elif item == "--since":
            since = arguments.pop(0) if arguments else None
            if not since:
                print("check-slop: --since needs a git ref", file=sys.stderr)
                return 2
        elif item == "--rule":
            wanted = arguments.pop(0) if arguments else None
        elif item == "--message":
            return checkMessage(arguments.pop(0))
        elif item == "--attribute":
            attribute = arguments.pop(0) if arguments else None
        elif not item.startswith("--"):
            paths.append(item)
    # Several paths are several files, not a root that keeps being replaced by
    # the next one. Taking only the last silently drops every file before it:
    # a run naming a file with a violation and a clean file after it reports
    # the clean one and passes, which reads exactly like a run that found
    # nothing.
    root = paths[-1] if len(paths) == 1 else "."
    root = os.path.abspath(root)
    names = {name for name, _, _, _ in RULES}
    if wanted and wanted not in names:
        print("check-slop: no rule named %s (have: %s)"
              % (wanted, ", ".join(sorted(names))), file=sys.stderr)
        return 2

    # A path that names a file is that file. Taken as a root to walk, it scans
    # nothing, and the run passes over a file it never read.
    if staged:
        walk = stagedSources(root)
    elif since:
        walk = changedSources(root, since)
    elif len(paths) > 1:
        walk, root = [os.path.abspath(path) for path in paths], os.getcwd()
    elif os.path.isfile(root):
        walk, root = [root], os.path.dirname(root) or "."
    else:
        walk = sources(root)
    scanned, findings = 0, []
    for path in walk:
        relative = os.path.relpath(path, root)
        if relative in SKIP_PATHS:
            continue
        scanned += 1
        for number, name, explanation, line in examine(path, wanted):
            findings.append((relative, number, name, explanation, line))

    if not scanned:
        if staged:
            # Nothing staged that this check reads is an answer, not a silence.
            print("check-slop: ok (no staged file carries prose)")
            return 0
        if since:
            # And a branch that changed no prose is the same answer one scope
            # out. This is only sound because the whole-tree pass still runs and
            # holds the premise: the CI workflow calls this script with no ref.
            print("check-slop: ok (no file this branch changed carries prose)")
            return 0
        print("check-slop: no files found under %s — the walk matched nothing,"
              % root, file=sys.stderr)
        print("  which is not the same as nothing being wrong.", file=sys.stderr)
        return 2

    # A whole-tree gate reports the tree, and a push is only when someone reads
    # it — so a finding on an untouched line is answered here rather than by
    # narrowing the scan. When a base ref is given, each finding says whether the
    # branch added its line, and the summary leads with how many are the pusher's.
    added = None
    if attribute is not None:
        added = addedLineNumbers(root, attribute)
        if added is None:
            print("check-slop: --attribute %s is not a usable ref; reporting without it"
                  % attribute, file=sys.stderr)

    counts, mine = {}, 0
    for relative, number, name, explanation, line in sorted(findings):
        counts[name] = counts.get(name, 0) + 1
        owned = added is not None and number in added.get(relative, set())
        if owned:
            mine += 1
        print("%s:%d: [%s] %s%s" % (relative, number, name, explanation,
                                    "  <- in your diff" if owned else ""),
              file=sys.stderr)
        print("    %s" % line[:110], file=sys.stderr)

    if findings:
        print("", file=sys.stderr)
        if added is not None:
            print("check-slop: %d site(s), %d in your diff vs %s, %d rule(s):"
                  % (len(findings), mine, attribute, len(counts)), file=sys.stderr)
            if mine == 0:
                print("  None sit on a line this branch changed. The tree carries them "
                      "and the push is where you meet them; the fix is owed, but not by "
                      "your diff.", file=sys.stderr)
        else:
            print("check-slop: %d site(s), %d rule(s):" % (len(findings), len(counts)),
                  file=sys.stderr)
        for name in sorted(counts):
            print("  %-10s %4d   %s" % (name, counts[name], ADVICE[name]), file=sys.stderr)
        return 1

    print("check-slop: ok (%d files, %d rules, every reason is one a reader can check)"
          % (scanned, len(names) + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
