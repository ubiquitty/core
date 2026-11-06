// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"

// The values a row mark can carry (OSC 133 shell integration). A row holds one of
// these, so prompt navigation and clear-to-mark can tell a prompt row from the
// command output below it, and a failed command from a successful one.

// OSC 133 prompt-mark protocol — per-row marker bits (ESC ] 133 ; <letter> ST).
// The parser records the bit on the cursor row; bits travel with the row into
// and out of scrollback.

namespace Ubiquitty::Screen {
constexpr uint8_t osc133MarkPromptStart = 0x01;
constexpr uint8_t osc133MarkCmdStart = 0x02;
constexpr uint8_t osc133MarkOutputStart = 0x04;
constexpr uint8_t osc133MarkCmdEnd = 0x08;
/// Promoted onto a prompt's row only once that prompt actually runs a command
/// (the A→C transition): PROMPT_START is set the instant the prompt prints, but
/// the visible gutter indicator and Cmd-L target must mark only prompts whose
/// command executed — a bare prompt the user never submits stays unmarked.
constexpr uint8_t osc133MarkCmdExecuted = 0x10;
/// commandDurationTicksRing slot for a completion whose start was never seen.
constexpr uint32_t commandDurationUnknown = 0xFFFFFFFFu;
/// Set on the prompt's row when its command finished with a NON-ZERO exit status
/// (OSC 133;D;<n>, n>0). The renderer tints that prompt's bracket as an error.
constexpr uint8_t osc133MarkCmdFailed = 0x20;

} // namespace Ubiquitty::Screen
