// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/DragDrop/Queue.h"

#include <optional>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// Owns a host command fenced by the pane's native-owner epoch.
struct NativeDragCommand {
    std::uint64_t epoch = 0;
    DragDrop::CommandRecord command {};
};

/// Owns native input correlated with a bound pane and owner epoch.
struct NativeDragEvent {
    std::uint32_t handle = 0;
    std::uint64_t epoch = 0;
    DragDrop::NativeEvent event {};
    FallibleVector<char> bytes {};
};

/// Each frame belongs to one native owner epoch and one bound pane channel.
[[nodiscard]] bool encodeNativeDragCommand(std::vector<std::uint8_t>&, std::uint64_t epoch, const DragDrop::Command&);
[[nodiscard]] std::optional<NativeDragCommand> decodeNativeDragCommand(std::span<const std::uint8_t>);
[[nodiscard]] bool encodeNativeDragEvent(std::vector<std::uint8_t>&, std::uint64_t epoch, const DragDrop::NativeEvent&, std::span<const char>);
[[nodiscard]] std::optional<NativeDragEvent> decodeNativeDragEvent(std::span<const std::uint8_t>);

} // namespace Ubiquitty::Protocol::Sync
