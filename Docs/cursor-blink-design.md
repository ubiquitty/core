<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Cursor blink

Cursor shape, visibility, opacity, and activity timing are resolved in Core.
Frontends paint the resulting `Cursor::RenderState`; they must not run an
independent cursor animation.

## Shape and visibility

An explicit DECSCUSR style selects block, underline, or bar. The
`FollowUserPreference` sentinel resolves through the user's cursor settings.
The master blink preference can force a blinking style to remain steady.
DECTCEM-hidden cursors and terminals without an active screen are invisible.

A focused blinking cursor uses the opacity curve below. An unfocused cursor is
steady; its frontend presentation is a hollow outline. The discrete visibility
gate is `blinkAlpha >= 0.5` for consumers that cannot use fractional opacity.

## Cadence

The default period is 60 monotonic ticks, or one second. The Fade animation uses
five opacity levels; the ramps are stepped, not continuously interpolated.

| Ticks in the 60-tick cycle | Opacity |
| --- | --- |
| 0–31 | 1.00 |
| 32–33 | 0.75 |
| 34–35 | 0.50 |
| 36–37 | 0.25 |
| 38–53 | 0.00 |
| 54–55 | 0.25 |
| 56–57 | 0.50 |
| 58–59 | 0.75 |

The OnOff animation uses the same phase and thresholds these values at 0.5.
The configured period scales the cycle while retaining its proportions. Zero
milliseconds selects the default; conversion to ticks has a minimum of one.

## Activity and scheduling

Keyboard and mouse input call `Cursor::noteUserActivity`. The cursor remains
solid for 40 ticks by default, approximately 0.67 seconds. A nonzero configured
hold overrides that value. After the hold, blinking resumes at the fade-out,
because the hold already provided the solid portion of the cycle. A fresh
terminal follows the same rule from tick zero.

The frontend schedules its next cursor update with
`Cursor::ticksUntilAlphaChange`. A zero result means no opacity transition
within the next period. This avoids repainting every display frame while the
cursor stays at one level. Output damage can still cause independent repaints.

DECRQSS reports the resolved configured style, not a momentary activity hold or
blink phase.

## References

- `Source/Core/Cursor/Blink.h`: shared curve and animation selection.
- `Source/Core/Cursor/RenderState.h`: shape, activity hold, period, and scheduling.
