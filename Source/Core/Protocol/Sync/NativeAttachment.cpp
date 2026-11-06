// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/NativeAttachment.h"

#include <utility>

namespace Ubiquitty::Protocol::Sync {

ClipboardDataMessage makeClipboardData(
    uint32_t requestID, ClipboardServeStatus status, std::vector<ClipboardServeItem> items)
{
    ClipboardDataMessage message;
    message.requestID = requestID;
    message.status = status;
    if (status != ClipboardServeStatus::OK)
        return message; // a refusal carries nothing; two answers in one frame is not an answer

    std::size_t total = 0;
    for (const ClipboardServeItem& item : items)
        total += item.data.size();
    if (total > maxClipboardServeBytes) {
        message.status = ClipboardServeStatus::TooLarge;
        return message;
    }
    message.items = std::move(items);
    return message;
}

} // namespace Ubiquitty::Protocol::Sync
