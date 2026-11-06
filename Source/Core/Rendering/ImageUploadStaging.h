// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace Ubiquitty::Rendering {

// A snapshot pins one pool slot until its image quads are uploaded or replaced.
// The cache entry can stage a newer ingest snapshot into another slot without
// overwriting the exact source version the current render snapshot references.
struct ImageUploadStaging {
    std::uint64_t cacheKey = 0;
    std::uint64_t stamp = 0;
    int width = 0;
    int height = 0;
    int format = 0;
    std::vector<std::uint8_t> sourcePixels;
};

inline std::shared_ptr<ImageUploadStaging> acquireImageUploadStaging(std::vector<std::shared_ptr<ImageUploadStaging>>& pool)
{
    for (const auto& candidate : pool) {
        if (candidate.use_count() == 1) {
            return candidate;
        }
    }
    auto staging = std::make_shared<ImageUploadStaging>();
    pool.push_back(staging);
    return staging;
}

} // namespace Ubiquitty::Rendering
