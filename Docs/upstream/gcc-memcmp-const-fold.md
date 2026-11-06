<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Constant-array comparison guard

The build probes whether the selected compiler preserves a runtime offset in
a comparison against compile-time-readable bytes. This reference explains the
probe and the source restriction; it does not infer compiler behavior from a
version number.

## Required behavior

Both functions below must return 2. The searched ranges are inside their
arrays, and `memcmp` compares four bytes at the specified offset.

```c
#include <stdio.h>
#include <string.h>

static const unsigned char constantBytes[8] = { 'x', 'x', 'I', 'D', 'A', 'T', 'y', 'y' };
static unsigned char mutableBytes[8] = { 'x', 'x', 'I', 'D', 'A', 'T', 'y', 'y' };
static const unsigned char needle[4] = { 'I', 'D', 'A', 'T' };

int findConstant(void)
{
    for (unsigned index = 0; index + 4 <= sizeof constantBytes; ++index) {
        if (memcmp(&constantBytes[index], needle, 4) == 0)
            return (int)index;
    }
    return -1;
}

int findMutable(void)
{
    for (unsigned index = 0; index + 4 <= sizeof mutableBytes; ++index) {
        if (memcmp(&mutableBytes[index], needle, 4) == 0)
            return (int)index;
    }
    return -1;
}

int main(void)
{
    int constantResult = findConstant();
    int mutableResult = findMutable();
    printf("constant=%d mutable=%d\\n", constantResult, mutableResult);
    return constantResult != 2 || mutableResult != 2;
}
```

Save the reproducer as `repro.c` and compile it with the compiler and flags
being investigated. Preserve the exact compiler version, command line,
diagnostics, output, and generated assembly in any upstream report.

The failure the guard detects is a comparison folded as though the runtime
index were zero. Binding the candidate address to a pointer before comparing,
or comparing the required bytes explicitly, avoids the source shape rejected
by the repository lint.

## Configure-time probe

`Scripts/cmake/CheckMemcmpFold.cmake` defines
`ubiquitty_check_memcmp_fold`. Its `try_run` compiles and executes a small
constant-array search and records `UBIQUITTY_MEMCMP_FOLD_BROKEN`:

| Result | Meaning |
| --- | --- |
| `OFF` | The probe found the needle at its required offset |
| `ON` | The compiled probe did not produce the required result |
| `unknown` | Cross-compilation prevented execution, or the probe did not build |

An unexecuted probe is not a passing probe. The result is cached by the build;
a toolchain change requires a fresh applicable configure result.

## Source rule

`Scripts/check-memcmp-fold.py` rejects inline runtime-offset
`memcmp` / `strncmp` calls when the first operand is an initialized
constant array and the second is a literal or another constant array.
A buffer supplied at runtime is not that shape. `__builtin_memcmp` is not
an exemption.

Do not disable builtin comparison optimization globally to bypass this rule.
If a translation unit genuinely requires `-fno-builtin-memcmp`, scope the
option to that source through `set_source_files_properties` and retain
behavioral coverage. Keep compiler investigation and patch-submission records
separate from the product's source contract.
