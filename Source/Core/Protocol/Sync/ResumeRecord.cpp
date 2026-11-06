// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/ResumeRecord.h"

#include <numeric>

namespace Ubiquitty::Protocol::Sync {

std::size_t pictureBytes(const ResumeRecord& record)
{
    return std::accumulate(record.panes.begin(), record.panes.end(), std::size_t { 0 }, [](std::size_t bytes, const ResumeRecordPane& pane) { return bytes + pane.checkpoint.size(); });
}

ResumeWriteOutcome MemoryResumeRecordStore::write(const ResumeRecord& record)
{
    m_record = record;
    return ResumeWriteOutcome::Committed;
}

std::optional<ResumeRecord> MemoryResumeRecordStore::read()
{
    return m_record;
}

ResumePurgeOutcome MemoryResumeRecordStore::purge()
{
    m_record.reset();
    return ResumePurgeOutcome::Purged;
}

} // namespace Ubiquitty::Protocol::Sync
