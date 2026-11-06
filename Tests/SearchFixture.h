// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Search/SearchController.h"
#include "Harness.h"

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>

namespace UbiquittyTest {

/// Drives the production sliced search over a test's exclusively owned terminal.
class SearchFixture {
public:
    /// Binds a search to the terminal, which outlives the fixture.
    explicit SearchFixture(Ubiquitty::Terminal& terminal)
        : m_controller(terminal, m_mutex)
    {
    }

    /// Detaches the controller before releasing its model lock.
    ~SearchFixture() { m_controller.detach(); }

    /// Completes a regex query and publishes its render projection.
    /// - Parameters:
    ///   - pattern: The RE2 pattern.
    ///   - sensitivity: Whether case distinguishes.
    /// - Returns: The complete match count.
    int find(std::string_view pattern, Ubiquitty::CaseSensitivity sensitivity = Ubiquitty::CaseSensitivity::Sensitive)
    {
        const auto status = searchScrollbackAndWait({ .pattern = std::string(pattern), .form = Ubiquitty::Search::PatternForm::Regex, .sensitivity = sensitivity, .normalization = Ubiquitty::Screen::ScrollSearch::normalizationForm() });
        CHECK(!status.isPatternInvalid);
        return static_cast<int>(status.foundCount);
    }

    /// Completes a request with a bounded wait, asserting that it completed.
    ///
    /// - Parameter request: The query and its matching policy.
    /// - Returns: The search publication.
    Ubiquitty::Search::Status searchScrollbackAndWait(const Ubiquitty::Search::Request& request)
    {
        const auto status = m_controller.searchAndWait(request, std::chrono::seconds(30));
        CHECK(status.isComplete);
        return status;
    }

    /// Moves the active hit in the named direction, wrapping at either end.
    ///
    /// - Parameter direction: The direction through the stored hits.
    /// - Returns: Whether an active hit was available to move.
    bool step(Ubiquitty::Search::StepDirection direction)
    {
        const std::scoped_lock hold(m_mutex);
        return m_controller.step(direction);
    }

    /// Reads the current bounded labels as independent values.
    ///
    /// - Returns: The labels copied under the model lock.
    std::vector<Ubiquitty::Search::Label> labels()
    {
        const std::scoped_lock hold(m_mutex);
        return m_controller.labels();
    }

    /// Copies the logical hits for storage and mapping assertions.
    ///
    /// - Returns: The stored hits, newest first.
    std::vector<Ubiquitty::Search::Hit> hits()
    {
        const std::scoped_lock hold(m_mutex);
        const auto hits = m_controller.hits();
        return { hits.begin(), hits.end() };
    }

private:
    std::mutex m_mutex;
    Ubiquitty::Search::SearchController m_controller;
};

} // namespace UbiquittyTest
