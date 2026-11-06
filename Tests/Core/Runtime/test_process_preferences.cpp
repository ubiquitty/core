// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/ProcessPreferences.h"
#include "Harness.h"

// test_process_preferences.cpp — pins Runtime::applyProcessPreferences: the
// preferences with no terminal to ride reach the process-wide state every
// window reads. The macOS shells and the Linux shell each resolved
// `[unicode] normalization` themselves and only the retiring one did it on a
// reload, so the resolution and the apply are pinned here, once.

using Ubiquitty::Preferences::Values;
using Ubiquitty::Runtime::applyProcessPreferences;
using Ubiquitty::Runtime::searchNormalizationForm;
using Ubiquitty::Unicode::NormalizationForm;
namespace ScrollSearch = Ubiquitty::Screen::ScrollSearch;

namespace {

Values normalizing(const char* name)
{
    Values config;
    config.normalization = std::string(name);
    return config;
}

} // namespace

// Each form the key names resolves to that form, and the two ways of naming
// none — leaving the key out, or a name no form answers to — both mean search
// over the text as stored.
TEST(testTheKeyNamesTheSearchForm)
{
    CHECK(searchNormalizationForm(Values {}) == NormalizationForm::None);
    CHECK(searchNormalizationForm(normalizing("none")) == NormalizationForm::None);
    CHECK(searchNormalizationForm(normalizing("nfc")) == NormalizationForm::NFC);
    CHECK(searchNormalizationForm(normalizing("nfd")) == NormalizationForm::NFD);
    CHECK_MSG(searchNormalizationForm(normalizing("not-a-form")) == NormalizationForm::None, "an unknown name falls back to none");
}

// A reload that names a form sets the normalizer every find reads, and one that
// drops the key sets it back: the apply writes the whole answer each time
// rather than only the forms it recognises.
TEST(testApplyingSetsAndClearsTheProcessWideNormalizer)
{
    applyProcessPreferences(normalizing("nfd"));
    CHECK(ScrollSearch::normalizationForm() == NormalizationForm::NFD);
    applyProcessPreferences(normalizing("nfc"));
    CHECK(ScrollSearch::normalizationForm() == NormalizationForm::NFC);
    applyProcessPreferences(Values {});
    CHECK_MSG(ScrollSearch::normalizationForm() == NormalizationForm::None, "a reload without the key takes the form away again");
    applyProcessPreferences(normalizing("nfc"));
    applyProcessPreferences(normalizing("not-a-form"));
    CHECK_MSG(ScrollSearch::normalizationForm() == NormalizationForm::None, "an unknown name does not leave the last form standing");
}

UBIQUITTY_TEST_MAIN()
