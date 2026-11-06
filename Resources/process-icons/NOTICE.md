<!-- SPDX-FileCopyrightText: 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Process icons

The icons in this directory are the logos of the programs they name, and
are trademarks of their owners. Ubiquitty draws one only to identify the
program running in a pane, before its name in a window, tab or pane title, and
`[appearance] process_icons = false` turns them off. Their use implies no
affiliation with or endorsement by any owner. The repository's Apache-2.0
license does not cover them, and grants no right to use them for anything
else.

`Scripts/import-process-icons.py` fetches each from the source below, as
`catalog.json` records it. An SVG keeps its paths and loses any style sheet,
and a single-colour icon has its fill made black so the title can tint it.
An avatar has its ground removed and is trimmed to its mark.

| Icon | Program | Source | Terms of the source file |
|---|---|---|---|
| `claude-code.svg` | Claude Code | [theSVG](https://thesvg.org/icon/claude-code), `color` | theSVG's collection, MIT; the mark remains its owner's |
| `codex.svg` | Codex | [theSVG](https://thesvg.org/icon/codex), `light`, recoloured with a blue gradient | theSVG's collection, MIT; the mark remains its owner's |
| `opencode.svg` | opencode | [theSVG](https://thesvg.org/icon/opencode), `mono` | theSVG's collection, MIT; the mark remains its owner's |
| `pi.svg` | pi | [theSVG](https://thesvg.org/icon/pi), `default` | theSVG's collection, MIT; the mark remains its owner's |
| `gemini-cli.svg` | Gemini CLI | [theSVG](https://thesvg.org/icon/gemini-cli), `default` | theSVG's collection, MIT; the mark remains its owner's |
| `deepagents.svg` | Deep Agents | [the agent's repository](https://github.com/langchain-ai/deepagents), `.github/images/logo-light.svg`, the mark cut from its lockup | the repository, MIT; the mark remains its owner's |
| `grok-build.png` | Grok Build | the avatar of [xai-org](https://github.com/xai-org), which publishes it | its owner's |
| `prime-agent.png` | Prime Agent | the avatar of [PrimeIntellect-ai](https://github.com/PrimeIntellect-ai), which publishes it | its owner's |
| `kimi-code.png` | Kimi Code | the avatar of [MoonshotAI](https://github.com/MoonshotAI), which publishes it | its owner's |

An owner who would rather their icon were not drawn can ask for its entry to
be removed from `catalog.json`; the program's name is then shown alone.
