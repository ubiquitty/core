# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Accessibility: the grid as text a screen reader reads.

The grid reaches the macOS Accessibility API as text; no escape sequence
starts it. The entry prints a known paragraph so a VoiceOver pass can be
compared against it by ear.
"""

from . import Entry

TITLE = "Accessibility"
ORDER = 290

# ASCII only so the reader's pronunciation is unambiguous.
_PARAGRAPH = (
    "This paragraph is the text under review. A correct pass reads it back in "
    "the order it is written, by line, then by word, then by character, and "
    "speaks nothing that is not on the screen."
)


def _demo_voiceover(tui):
    row = tui.page(TITLE, "With VoiceOver on (Cmd-F5), VO-A reads the paragraph below word for word.")
    width = min(tui.cols - 8, 72)
    line = ""
    for word in _PARAGRAPH.split():
        if line and len(line) + 1 + len(word) > width:
            tui.text(row, 4, line, tui.fg_default())
            row += 1
            line = word
        else:
            line = f"{line} {word}".strip()
    if line:
        tui.text(row, 4, line, tui.fg_default())
    return tui.footer()


ENTRIES = [
    Entry(
        title="Screen reader (VoiceOver)",
        explanation=(
            "The grid reaches the macOS Accessibility API as text. VoiceOver "
            "reads the printed paragraph by line, word and character, in the "
            "order it is written, and speaks nothing that is not on screen."),
        run=_demo_voiceover,
        sequence=_PARAGRAPH + "\r\n",
        golden="a11y_voiceover_paragraph",
        descriptive=True,
    ),
]
