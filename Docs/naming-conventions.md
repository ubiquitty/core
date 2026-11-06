<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Naming conventions

This supplement defines bounded-value and abbreviation naming. The general
rules are in [Coding conventions](coding-conventions.md); enforcement is in
`Scripts/check-acronyms.py`.

## Bounds

Use `max` and `min` as prefixes for value limits. Name the quantity being
bounded: `maxLinkKeyLength`, `maxTerminalImageCount`, `minColumns`.
Do not append `Max`, `Maximum`, `Min`, or `Minimum` to a project-owned
limit name.

| Meaning | Pattern | Example |
| --- | --- | --- |
| Maximum or minimum allowed value | `maxQuantity`, `minQuantity` | `maxDecodedImageBytes` |
| Allocated container storage | `quantityCapacity` | `glyphCapacity` |
| Maximum allocation | `maxQuantityCapacity` | `maxSixelAccumulatorCapacity` |
| Configured limit or region edge | `quantityLimit` | `scrollbackLimit`, `topLimit` |
| Point where behavior changes | `quantityThreshold` | `confirmMultilinePasteThreshold` |
| Allowance consumed by work | `quantityBudget` | `drainQuietSpinBudget` |

Geometric minimum and maximum coordinates are not capacities. Established
graphics names such as `uvMax` and `inkMin` retain their coordinate meaning.
Do not rename vendor fields or API symbols.

## Abbreviations

Use a type to express representation and a name to express meaning.
`Uint` is not a project word: write `uint32_t` as the type and choose a
semantic name such as `sessionCount`. Spell `Variable` out rather than
using `Var` in project-owned identifiers.

Acronyms retain their acronym spelling: `ID`, `IDs`, `UTF8`, `SHA256`.
A leading acronym in a lower-camel-case name is lowercase, as in `csiHandlers`;
a non-leading acronym remains uppercase, as in `inputUTF8`.

Vendor symbols retain their original spelling, including `FT_MM_Var`,
`FT_Var_Axis`, and `NSMaximumRange`. Language keywords and external wire or
configuration strings are not semantic-rename targets.

## Semantic renames

Use `Scripts/refactor.py` for C++ symbol renames. Review its dry-run edits
before applying them; identically spelled members and locals can be distinct
symbols. A rename must update references and compile at each affected boundary.
Do not replace identifier substrings with a text-editing command.

## Workspace and session

The interface says **workspace** for the named or unnamed set of panes a person
attaches to, opens, creates, names, lists on a host or is asked to choose
between. Core's attach question, the host list, the Connect and picker buttons,
the web page, and the iOS navigation use that word: "New Workspace", "Choose
Workspace", "Workspaces on this host", "Workspace Name".

**Session** stays where it means a terminal session in the technical sense: the
PTY and terminal behind one pane, its log, inspector, clipboard scope and
presence badge, session-granted consent, the host's terminate and handoff
operations, tmux and SSH sessions, the saved-session settings, and the `ubq`
and MCP `-s session` targets. Code identifiers, the Sync wire's names, JSON
fields and preference keys keep their spelling; this rule changes interface
text only. Menu titles and accessibility labels follow the same split, and a
UI scenario or smoke asserts the string the interface shows.

## Writing

Interface text follows the platform's own writing guide. The macOS and iOS
apps, and the Swift packages under `Source/Platform/Apple`, follow the
[Apple Style Guide](https://support.apple.com/guide/applestyleguide/welcome/web)
and the writing guidance of the
[Human Interface Guidelines](https://developer.apple.com/design/human-interface-guidelines/writing).
The Linux frontend follows the
[GNOME Human Interface Guidelines](https://developer.gnome.org/hig/guidelines/writing-style.html).
Core's interface text — host-list words, connect remedies, consent questions,
refusal notices, preference names — is shown by both, so it is one text that
satisfies both guides. They agree on nearly everything; where they differ,
Core carries the sentence and each frontend supplies the frame around it.
`Scripts/check-copy.py` holds the mechanical part of these rules.

### Capitalization

| Element | Apple | GNOME |
| --- | --- | --- |
| Window, sheet, dialog and alert titles; menu items; buttons; tabs; navigation titles; headings | Title case | Header capitalization |
| Body text, descriptions, footers, labels, check boxes, switches, status lines, tooltips | Sentence case | Sentence capitalization |

Title case capitalizes every word except articles (*a*, *an*, *the*),
coordinating conjunctions (*and*, *but*, *or*, *nor*, *for*, *yet*, *so*) and
short prepositions, which stay lowercase unless they are the first or last
word. Apple keeps prepositions of four or fewer letters lowercase (*at*, *by*,
*for*, *from*, *in*, *into*, *of*, *on*, *onto*, *over*, *to*, *with*); GNOME
keeps those of three or fewer, so it capitalizes *From*, *Into*, *Onto*,
*Over* and *With*. A preposition that is part of a verb phrase is capitalized
on both: "Set Up", "Log In", "Turn Off". Core's titles avoid the four-letter
prepositions, or capitalize as GNOME does where Apple's rule would leave them
lowercase only by length.

An alert title that is a complete sentence takes sentence-style capitalization
and its own punctuation ("The arrangement couldn’t be saved.", "Paste multiple
lines into the terminal?"); a fragment takes title case and no period ("Can’t
Reach build-server").

### Sentences

- Body text is complete sentences with terminal punctuation. A fragment is
  acceptable only in a label, a status line or a title.
- An error message says what happened, then what to do, in that order: "Can’t
  reach build-server over SSH. Check that it’s on and connected to the
  network, then try again."
- Buttons and menu items are verbs or verb phrases ("Connect", "Try Again",
  "Copy Install Commands"). An item that opens a window or asks for more input
  ends with an ellipsis ("Open…", "Export Theme…").
- No exclamation marks. No internal names: an enumeration case, a protocol
  message, *transport*, *body*, *frame*, *payload*, *decode*, *handshake* or
  *Sync* as jargon never reaches a person. Say what the person sees: "The
  host sent a reply Ubiquitty doesn’t understand."
- Address the person as *you*; refer to the app by name, not as *we*.

### Typography

- Curly apostrophes and quotes: ’ “ ” (and ‘ ’ inside a quotation).
- The ellipsis character … rather than three periods.
- An en dash for a range ("rows 1–24"), an em dash without spaces for a break.
- US spelling on both platforms: *color*, *behavior*, *canceled*, *license*,
  *organize*.

### Words

One word per concept: **workspace**, **host**, **pane**, **tab** and
**window**, as defined above and in [the architecture](architecture.md).
Products and acronyms are spelled as their owners spell them, at the start of
a sentence too:

| Write | Not |
| --- | --- |
| SSH, QUIC, URL, PTY, GPU, CPU, JSON, UTF-8 | ssh, quic, url, pty, Url |
| Wi-Fi | wifi, WiFi, Wi-fi |
| iPhone, iPad, Mac, macOS, iOS | Iphone, IPad, Macos, MacOS |
| tmux, Tailscale, GitHub | Tmux, tailscale, Github |
| OK | Ok |
| email | e-mail |

A command, file, path, environment variable or option keeps its literal
spelling and is set apart in code style where the surface allows it: `ubq`,
`ssh`, `~/.ubiquitty`, `TERM`. Name the program when the program is the
subject ("`ssh` exited"); name the protocol otherwise ("over SSH").

Where the platforms name a thing differently, each frontend uses its own word:

| Concept | macOS and iOS | Linux (GNOME) |
| --- | --- | --- |
| The app’s options window | Settings | Preferences |
| Leaving the app | Quit | Quit (never Exit) |

### The gate

`Scripts/check-copy.py` scans the Apple frontends with Apple's rules, the
Linux frontend with GNOME's, and the Core files that own interface text with
the rules both share. It checks the lexicon above, British spellings and
internal jargon; straight quotes and apostrophes, three periods, hyphenated
ranges and exclamation marks; title case where the API makes a literal a
title, button or menu item; terminal punctuation where it makes one a body
sentence; and the other platform's words. A site the rule gets wrong is
allowed with `copy-ok: <reason>` on its line or the line above. A Core file
that starts carrying interface text joins `CORE_TEXT_FILES` in the gate.
