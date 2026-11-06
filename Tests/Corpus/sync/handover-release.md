<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Published handover fixture

The v0.3.2 tag (`9217daa81984d477403cf994b93d86196eb04c89`) writes
handover body version 35. Its `theEncoderWritesTheFrozenCurrentPaneRecord`
case mints this record:

- Corpus file: `fuzz_sync_handover/6b76710f4379439cbb9afaaef26fa3a59a1701d8`
- Length: 15,777 bytes
- SHA-256: `cec03aed4101db87253f015f083749f06cf48802194b9ce546b4b4fd5beac0fa`

It contains pane 17/18, session 17, title `frozen pane`, scrolled text,
attributes, a hyperlink, combining and wide characters, and principal 21/22's
input receipt through sequence 1. The current reader must restore that state.
The published fixture stays pinned independently of the current encoder's
fixture, even when the two records are identical.

To mint it from a clean v0.3.2 checkout, with `UBQ_BUILD_ROOT` outside it:

```sh
cmake -S . -B "$UBQ_BUILD_ROOT/core" -G Ninja \
  -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$UBQ_BUILD_ROOT/core" --target test_sync_handover_body -j 4
UBQ_MINT_HANDOVER_FIXTURE="$UBQ_BUILD_ROOT/handover-v35.bin" \
  "$UBQ_BUILD_ROOT/core/Tests/test_sync_handover_body" \
  theEncoderWritesTheFrozenCurrentPaneRecord
sha256sum "$UBQ_BUILD_ROOT/handover-v35.bin"
cmp "$UBQ_BUILD_ROOT/handover-v35.bin" \
  Tests/Corpus/sync/fuzz_sync_handover/6b76710f4379439cbb9afaaef26fa3a59a1701d8
```

Mint mode exits 1 after writing. Run the reader tests without that environment
variable. A clean Linux Debug build at the tag reproduced these bytes exactly.

At publication, update `lastPublishedHandoverBodyVersion` in
`Source/Core/Protocol/Sync/HandoverBody.h`, the independent published fixture
pin in `test_sync_handover_body.cpp`, and this provenance. Preserve the old
corpus file. Do not move the release floor merely to accommodate a development
writer change: its reader must continue to accept the published fixture.
