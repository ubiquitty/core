// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "ConsentFuzz.h"
#include "Harness.h"

#include <array>
#include <cstdint>

TEST(canonicalConsentFuzzSeedsReachEveryCurrentShape)
{
    // Coprime family/enum moduli reach every family and each enum value. The
    // helper independently requires each unmodified producer seed to decode.
    for (unsigned selector = 0; selector <= UINT8_MAX; ++selector) {
        const std::array<std::uint8_t, 4> input { static_cast<std::uint8_t>(selector), 0xFF, 17, 0x80 };
        UbiquittyTest::ConsentFuzz::seeded(input);
    }
}

TEST(canonicalConsentFuzzIngressKeepsRefusalLatchedAndAllocationBounded)
{
    // Unknown kind, noncanonical channel/length, wrong Call scope and an
    // oversized declared body all take production header-first ingress.
    constexpr std::array<std::array<std::uint8_t, 7>, 5> inputs { {
        { 0xFF, 0, 0, 13, 0, 0, 0 },
        { 13, 0x80, 0, 0, 13, 0, 0 },
        { 13, 0, 0x80, 0, 13, 0, 0 },
        { 13, 1, 0, 13, 0, 0, 0 },
        { 13, 0, 0x80, 0x80, 4, 13, 0 },
    } };
    for (const auto& input : inputs)
        CHECK(UbiquittyTest::ConsentFuzz::stream(input) == Ubiquitty::Protocol::Sync::FrameIngress::State::Refused);
}

UBIQUITTY_TEST_MAIN()
