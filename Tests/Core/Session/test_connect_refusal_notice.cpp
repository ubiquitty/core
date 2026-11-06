// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Session/ConnectRemedy.h"

TEST(typedConnectionRefusalsHaveReadableNoticesWithoutInventingAReplacement)
{
    using Ubiquitty::Protocol::Sync::ByeReason;
    using Ubiquitty::Session::connectRefusalNotice;
    CHECK(connectRefusalNotice(ByeReason::SessionGone) == "This workspace is no longer available on the host. Choose another workspace, or create a new one.");
    CHECK(connectRefusalNotice(ByeReason::SessionGone).find("new session opens") == std::string_view::npos);
    CHECK(connectRefusalNotice(ByeReason::TokenMissing) == "This host requires an access token.");
    CHECK(connectRefusalNotice(ByeReason::NoConsent).find("allow this connection") != std::string_view::npos);
    for (unsigned value = 0; value <= std::to_underlying(Ubiquitty::Protocol::Sync::lastByeReasonOnWire); ++value) {
        const auto notice = connectRefusalNotice(static_cast<ByeReason>(value));
        CHECK(!notice.empty());
        CHECK(notice.find("refused:") == std::string_view::npos);
    }
}

UBIQUITTY_TEST_MAIN()
