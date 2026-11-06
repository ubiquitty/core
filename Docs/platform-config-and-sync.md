<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Platform configuration and reviewed profile transfer

## `[platform]` override axis

`Source/Core/Preferences/` layers a platform override axis on top of the Profile
fields and the 2-arg `resolve()`.

- `Profile` carries a compile-time `UBIQUITTY_PLATFORM_{MAC,LINUX}` selector
  (PAL-clean — keys off `__linux__`, mac default), a
  `Platform` enum + `currentPlatform()`, and `resolveLayers()`, which left-folds
  `resolve()` across N layers (built-in → … → `[platform]`). `resolve()` inherits
  the base name when an override is unnamed, so a trailing platform layer never
  wipes a profile's identity.
- `Preferences` holds a `PlatformOverride{profile, keyBindings, present}` slot per
  platform. The TOML parser routes `[mac]` / `[linux]` into the
  matching slot (a bare block sets profile fields; `[<plat>.keys]` sets keybinding
  overrides). A normal `[section]` after a platform block stops the routing.
- `applyCurrentPlatformOverride` folds the build's platform profile as the final
  resolver layer; `resolveKeyBindings` merges global `[keys]` with `[<plat>.keys]`
  (a platform spec replaces a matching global one; new specs append).

```toml
font_size = 13

[mac]
font_size = 14        # applied on the macOS build, ignored on linux

[mac.keys]
"cmd+t" = "new_tab"
```

## Profile lifetime and transfer

Local profiles are saved on this device and are excluded from transfer.
`scope = "synced"` opts a profile into explicit export. In Settings → Profiles,
turn on **Allow Export to Other Devices**, then export a `.ubqprofile` file.
Transfer that file using the person's chosen channel. Import opens a comparison
with the current terminal window. **Apply to Window** creates a temporary window
override; **Save on This Mac** creates a local copy with a unique name. Neither
choice replaces an existing profile. Cancel changes nothing.

Temporary overrides belong to the window controller. Closing that window or
restarting the app discards them; disconnecting its transport does not. Reset
Window Overrides restores inheritance. `scope = "ephemeral"` is never emitted
by the persistent serializer, and persistent configuration ignores such entries.

The resolution order, from lowest to highest precedence, is:

| Layer | Owner |
| --- | --- |
| Reviewed shared presentation | Receiving session |
| Explicit local defaults | This device |
| Current platform overrides | This device |
| Parent profiles, farthest to nearest | This device |
| Selected profile | This device |
| Temporary overrides | Current window |

An absent field inherits the next lower layer. Export resolves only parents
that also opted into sharing. It does not export device defaults, platform
blocks, local parents, parent names, startup commands, process-exit policies,
credentials, clipboard permissions, or agent grants. Font and theme names are
bounded display names, not filesystem paths; numeric values must be finite
and inside the setting's schema bounds. Import applies the same projection.
Turning sharing off prevents future exports; it cannot recall copies already
sent to another device.

The CONFIG codec uses the same scope filter and produces review proposals.
It preserves the receiver's global and platform preferences and rejects a
malformed frame atomically. Receiving configuration is not permission to apply it. Cross-device changes
require explicit export, review, and application; a host must not silently
replace a client's settings.

## Local process exit behavior

A named profile may set `process_exit` to `keep_output`,
`close_on_success`, `close_always`, or `restart`. An absent value inherits
from its parent; the default keeps output. The Profiles editor presents the
same choices. A clean exit requires a collected zero exit status without a
signal. Restart waits one second, launches the original command in the same
pane, and stops after three consecutive restarts. A process that runs for a
minute resets that budget. A failed launch keeps the ended terminal available.

The policy applies to locally owned shells. A remote disconnect does not
restart or terminate a hosted process. Explicit command-line session lifetime
continues to close windows opened with `-e`. Process exit behavior
is an execution setting and is excluded from the presentation sync codec.
