// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Clipboard/StagedClient.h"

#include <utility>

namespace Ubiquitty::Clipboard {

void StagedClient::stageItems(std::span<const ClipboardItem> items)
{
    Snapshot snapshot;
    for (const auto& item : items) {
        snapshot.add(std::string(item.mime), std::vector<char>(item.bytes.begin(), item.bytes.end()));
    }
    provideSnapshot(std::move(snapshot));
}

void StagedClient::provideSnapshot(Snapshot&& snapshot)
{
    m_readStage.stageTyped(std::move(snapshot));
}

} // namespace Ubiquitty::Clipboard
