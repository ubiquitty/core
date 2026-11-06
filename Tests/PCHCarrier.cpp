// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

/// Anchors the target that owns the shared test precompiled header.
///
/// `target_precompile_headers` builds one precompiled header per target, which
/// across this many test targets costs more than it saves. The tests
/// `REUSE_FROM` this one instead, so the header set is parsed once for the whole
/// suite. The target needs a translation unit to exist; it contributes no
/// symbols.
