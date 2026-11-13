// SPDX-FileCopyrightText: 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace UbiquittyTest {

/// Removes inherited process background throttling from the presentation test.
///
/// - Returns: Whether the platform accepted the request.
bool preparePresentationProcess();

/// Places the simulated frontend in the native presentation thread's band.
///
/// - Returns: Whether the platform accepted the request.
bool preparePresentationThread();

}
