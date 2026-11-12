// SPDX-FileCopyrightText: 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace UbiquittyTest {

/// Places the simulated frontend in the native presentation thread's band.
///
/// - Returns: Whether the platform accepted the request.
bool preparePresentationThread();

}
