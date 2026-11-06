// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_preferences_configure.cpp — the one door configuration enters through.
//
// Pins:
//   - an empty document configures the default table
//   - a document stating every key at its default configures exactly what the
//     empty one does — every key-addressed read, the renderer fold, and the
//     terminal state a seed writes, cnorm included
//   - a strict document with a diagnostic is refused whole and names each line
//   - a permissive document applies what it could read and names the rest
//   - a key on a refused line keeps the value it had rather than its default:
//     a wrong kind, a word outside its vocabulary, a value that does not
//     parse, an array left open, a key under a header that cannot be read;
//     a number past its range is clamped, and an unknown key is a warning
//   - a settings write while a line is refused edits the text that was saved
//   - a by-key edit rewrites the live document in place and comes back through
//     the door; a refused edit changes nothing
//   - each replacement is heard once by each live subscription, and a
//     destroyed subscription is never called

#include "Harness.h"

#include "Core/Cursor/Blink.h"
#include "Core/Cursor/RenderState.h"
#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/RenderConfigurationFold.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Preferences = Ubiquitty::Preferences;
namespace Effective = Ubiquitty::Preferences::Effective;
using Preferences::configure;
using Preferences::ConfigureResult;
using Preferences::effectiveValue;
using Preferences::Key;
using Preferences::ParseMode;
using Preferences::SerializeKind;
using Preferences::SkipReason;
using Preferences::Value;
using Preferences::Values;
using Preferences::writeDescriptorCount;
using Preferences::writeDescriptors;

namespace {

std::string literalFor(const Value& value)
{
    switch (value.kind) {
    case SerializeKind::Bool:
    case SerializeKind::LegacyBoolOrToken:
        return Preferences::tomlBool(value.boolean);
    case SerializeKind::Int:
        return Preferences::tomlInt(value.integer);
    case SerializeKind::Double:
        return Preferences::tomlDouble(value.number);
    case SerializeKind::EnumToken:
    case SerializeKind::String:
        return Preferences::tomlString(value.text);
    }
    return {};
}

bool sameValue(const Value& left, const Value& right)
{
    return left.kind == right.kind && left.boolean == right.boolean && left.integer == right.integer && left.number == right.number
        && left.text == right.text;
}

// What a seed writes onto a terminal, read back after the traffic that
// tells an unset key from a stated default apart.
struct SeededTerminal {
    std::uint8_t shape = 0;
    bool isBlinkPreferred = false;
    bool isBlinkingAfterCnorm = false;
    std::uint32_t holdFrames = 0;
    std::uint32_t blinkPeriod = 0;
    Ubiquitty::BellMode bellMode {};
    int ambiguousWidth = 0;
    bool isPasswordDetecting = false;

    bool operator==(const SeededTerminal&) const = default;
};

SeededTerminal seed(const Values& values)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 6, 20);
    (void)Ubiquitty::Runtime::seedTerminalConfiguration(terminal, values, Ubiquitty::Runtime::LaunchThemePolicy::ConfiguredOnly);
    SeededTerminal seeded;
    seeded.shape = terminal.cursorShapePref;
    seeded.isBlinkPreferred = terminal.cursorBlinkPref;
    seeded.holdFrames = terminal.cursorHoldFrames;
    seeded.blinkPeriod = terminal.cursorBlinkPeriodMilliseconds;
    seeded.bellMode = terminal.bellMode;
    seeded.ambiguousWidth = static_cast<int>(terminal.ambiguousWidth);
    seeded.isPasswordDetecting = terminal.passwordDetect;
    Ubiquitty::processBytes(&terminal, std::string_view("\033[?12l\033[?25h"));
    seeded.isBlinkingAfterCnorm = Ubiquitty::Cursor::isBlinking(Ubiquitty::Cursor::effectiveStyleForUserPreference(terminal));
    return seeded;
}

// Every key the write table names, stated at the value it reads as with
// nothing set.
std::string documentStatingEveryDefault()
{
    const Values defaulted;
    std::string document;
    for (int index = 0; index < writeDescriptorCount; ++index) {
        const auto& descriptor = writeDescriptors[index];
        const std::optional<Value> value = effectiveValue(descriptor.id, defaulted);
        if (!value.has_value())
            continue;
        document += "[" + std::string(descriptor.section) + "]\n";
        document += std::string(descriptor.key) + " = " + literalFor(*value) + "\n";
    }
    return document;
}

} // namespace

TEST(testAnEmptyDocumentIsTheDefaultTable)
{
    const ConfigureResult result = configure("", ParseMode::Strict);
    CHECK_MSG(result.isApplied, "an empty document is a configuration, not an error");
    CHECK(result.diagnostics.empty());

    const Values defaulted;
    int checked = 0;
    for (int index = 0; index < writeDescriptorCount; ++index) {
        const Key key = writeDescriptors[index].id;
        const std::optional<Value> live = effectiveValue(key, Preferences::values());
        const std::optional<Value> table = effectiveValue(key, defaulted);
        CHECK(live.has_value() && table.has_value() && sameValue(*live, *table));
        ++checked;
    }
    CHECK_EQ(checked, writeDescriptorCount);
    CHECK_MSG(Effective::liveRenderConfiguration() == Effective::renderConfiguration(defaulted),
        "the renderer is configured from the default table");
}

TEST(testEveryKeyStatedAtItsDefaultConfiguresIdentically)
{
    const std::string document = documentStatingEveryDefault();
    const ConfigureResult result = configure(document, ParseMode::Strict);
    CHECK_MSG(result.isApplied, "a document of defaults validates");
    for (const auto& diagnostic : result.diagnostics)
        std::printf("unexpected diagnostic: line %d [%s] %s\n", diagnostic.line, diagnostic.section.c_str(), diagnostic.key.c_str());
    CHECK(result.diagnostics.empty());

    const Values stated = Preferences::values();
    const Values unset;
    for (int index = 0; index < writeDescriptorCount; ++index) {
        const auto& descriptor = writeDescriptors[index];
        const std::optional<Value> fromStated = effectiveValue(descriptor.id, stated);
        const std::optional<Value> fromUnset = effectiveValue(descriptor.id, unset);
        const bool isSame = fromStated.has_value() && fromUnset.has_value() && sameValue(*fromStated, *fromUnset);
        if (!isSame) {
            std::printf("FAIL [%s] %s reads differently stated at its default\n", descriptor.section, descriptor.key);
            failures++;
        }
    }
    CHECK_MSG(Effective::renderConfiguration(stated) == Effective::renderConfiguration(unset),
        "the renderer cannot tell a stated default from an unset key");
    CHECK_MSG(seed(stated) == seed(unset), "the terminal cannot tell a stated default from an unset key");
    CHECK_MSG(seed(unset).isBlinkingAfterCnorm, "cnorm does not stop the default blink");
}

TEST(testAStrictDocumentWithADiagnosticIsRefusedWhole)
{
    (void)configure("[font]\nsize = 15.0\n", ParseMode::Strict);
    const std::uint64_t before = Preferences::configurationEpoch();

    const ConfigureResult result = configure("[font]\n"
                                             "size = 19.0\n"
                                             "shpae = 1\n"
                                             "[cursor]\n"
                                             "blink = \"yes\"\n"
                                             "this line has no equals sign\n",
        ParseMode::Strict);
    CHECK_MSG(!result.isApplied, "a strict document with a typo is refused");
    CHECK_EQ(result.epoch, before);
    CHECK_EQ(Preferences::configurationEpoch(), before);
    CHECK_MSG(Effective::fontSize(Preferences::values()) == 15.0, "the live configuration is untouched by a refusal");
    CHECK_MSG(Preferences::liveDocument() == "[font]\nsize = 15.0\n", "and so is the live document");

    CHECK_EQ(static_cast<int>(result.diagnostics.size()), 3);
    if (result.diagnostics.size() == 3) {
        CHECK(result.diagnostics[0].reason == SkipReason::UnknownKey);
        CHECK(result.diagnostics[0].key == "shpae");
        CHECK_EQ(result.diagnostics[0].line, 3);
        CHECK(result.diagnostics[1].reason == SkipReason::WrongValueKind);
        CHECK(result.diagnostics[1].section == "cursor");
        CHECK_EQ(result.diagnostics[1].line, 5);
        CHECK(result.diagnostics[2].reason == SkipReason::MalformedLine);
        CHECK(result.diagnostics[2].key == "this line has no equals sign");
        CHECK_EQ(result.diagnostics[2].line, 6);
    }
}

TEST(testAPermissiveDocumentAppliesWhatItCouldRead)
{
    const ConfigureResult result = configure("[font]\nsize = 19.0\nshpae = 1\n[[unclosed\n", ParseMode::Permissive);
    CHECK(result.isApplied);
    CHECK_MSG(Effective::fontSize(Preferences::values()) == 19.0, "the key it could read landed");
    CHECK_EQ(static_cast<int>(result.diagnostics.size()), 2);
    if (result.diagnostics.size() == 2)
        CHECK(result.diagnostics[1].reason == SkipReason::MalformedLine);
}

TEST(testAMalformedLineQuoteIsBounded)
{
    const std::string longLine(4096, 'x');
    const ConfigureResult result = configure(longLine + "\n", ParseMode::Permissive);
    CHECK_EQ(static_cast<int>(result.diagnostics.size()), 1);
    if (result.diagnostics.size() == 1)
        CHECK(result.diagnostics[0].key.size() == Preferences::malformedLineQuoteLimit);
}

// A file saved with an error. The refused line keeps the value its key had
// under the document before, not the default, so one typo moves nothing but
// itself; the lines beside it apply; and the live document is the saved text.
TEST(testAValueOfTheWrongKindKeepsTheValueItsKeyHad)
{
    REQUIRE(configure("[font]\nsize = 15.0\n[cursor]\nblink = false\n", ParseMode::Strict).isApplied);

    const std::string saved = "[font]\nsize = \"large\"\n[cursor]\nblink = true\n";
    const ConfigureResult result = configure(saved, ParseMode::Permissive);
    CHECK(result.isApplied);
    CHECK_MSG(Effective::fontSize(Preferences::values()) == 15.0, "the refused line keeps the size it had, not the default");
    CHECK_MSG(Effective::cursorBlink(Preferences::values()), "the line beside it applies");
    CHECK_MSG(Preferences::liveDocument() == saved, "the live document is the text that was saved");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].reason == SkipReason::WrongValueKind);
    CHECK(result.diagnostics[0].section == "font");
    CHECK(result.diagnostics[0].key == "size");
    CHECK_EQ(result.diagnostics[0].line, 2);
    const std::string sentence = Preferences::diagnosticSentence(result.diagnostics[0]);
    CHECK_MSG(sentence == "line 2: font.size takes a number \xE2\x80\x94 ignored", sentence.c_str());

    // Saved again with the line still wrong, the key still has what it had.
    (void)configure("[font]\nsize = \"large\"\nfamily = \"Menlo\"\n", ParseMode::Permissive);
    CHECK(Effective::fontSize(Preferences::values()) == 15.0);
    CHECK(Preferences::values().fontFamily.value_or("") == "Menlo");

    // A line the person deletes is gone, so its key reads as its default.
    (void)configure("[font]\nfamily = \"Menlo\"\n", ParseMode::Permissive);
    CHECK(Effective::fontSize(Preferences::values()) == Effective::fontSize(Values {}));

    // With nothing applied before it, a refused key has only its default.
    (void)configure("", ParseMode::Permissive);
    (void)configure("[font]\nsize = \"large\"\n", ParseMode::Permissive);
    CHECK(Effective::fontSize(Preferences::values()) == Effective::fontSize(Values {}));
    (void)configure("", ParseMode::Permissive);
}

// Out of range has two meanings. A word outside a key's vocabulary is not a
// value the key takes, and is refused like any other; a number past a key's
// range is clamped to the range, which is a value the key takes.
TEST(testAValueOutOfRangeIsRefusedOrClamped)
{
    REQUIRE(configure("[cursor]\nshape = \"block\"\n[terminal]\nambiguous_width = 1\n", ParseMode::Strict).isApplied);

    const ConfigureResult result = configure("[cursor]\nshape = \"triangle\"\n[terminal]\nambiguous_width = 7\n", ParseMode::Permissive);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].reason == SkipReason::WrongValueKind);
    CHECK(result.diagnostics[0].key == "shape");
    CHECK_EQ(result.diagnostics[0].line, 2);
    CHECK_MSG(Preferences::values().cursorShape == Preferences::CursorShape::Block, "the refused word keeps the shape the key had");
    CHECK_EQ(Effective::ambiguousWidth(Preferences::values()), 2);
    (void)configure("", ParseMode::Permissive);
}

// A key nothing reads is a warning: a newer build may know it, so it is named
// and ignored and every other line applies. It keeps nothing, because the
// name is the key: a misspelled name leaves the setting it meant unstated.
TEST(testAnUnknownKeyIsAWarningAndTheRestApplies)
{
    REQUIRE(configure("[font]\nsize = 15.0\n", ParseMode::Strict).isApplied);

    const ConfigureResult result = configure("[font]\n"
                                             "size = 17.0\n"
                                             "shaping_cache = 4\n"
                                             "[newer_section]\n"
                                             "some_key = true\n",
        ParseMode::Permissive);
    CHECK(result.isApplied);
    CHECK(Effective::fontSize(Preferences::values()) == 17.0);
    REQUIRE(result.diagnostics.size() == 2);
    CHECK(result.diagnostics[0].reason == SkipReason::UnknownKey);
    CHECK_EQ(result.diagnostics[0].line, 3);
    CHECK(result.diagnostics[1].reason == SkipReason::UnknownKey);
    CHECK_EQ(result.diagnostics[1].line, 5);

    (void)configure("[font]\nsise = 19.0\n", ParseMode::Permissive);
    CHECK(Effective::fontSize(Preferences::values()) == Effective::fontSize(Values {}));
    (void)configure("", ParseMode::Permissive);
}

// A TOML syntax error costs the lines it spans and no others, and each key
// it covers keeps its value.
TEST(testASyntaxErrorCostsOnlyTheLinesItSpans)
{
    REQUIRE(configure("[font]\nsize = 15.0\nfeatures = \"calt\"\n[cursor]\nblink = false\n", ParseMode::Strict).isApplied);

    // An array left open before the next key: named at the line it opens, its
    // key keeps its value, and the lines after it are read as written.
    const ConfigureResult open = configure("[font]\n"
                                           "features = [\n"
                                           "  \"liga\",\n"
                                           "size = 16.0\n"
                                           "[cursor]\n"
                                           "blink = true\n",
        ParseMode::Permissive);
    REQUIRE(open.diagnostics.size() == 1);
    CHECK_EQ(open.diagnostics[0].line, 2);
    CHECK(open.diagnostics[0].key == "features");
    CHECK(open.diagnostics[0].reason == SkipReason::WrongValueKind);
    CHECK(Preferences::values().fontFeatures.value_or("") == "calt");
    CHECK(Effective::fontSize(Preferences::values()) == 16.0);
    CHECK(Effective::cursorBlink(Preferences::values()));

    // Left open to the end of the file, it is named where it opens too.
    const ConfigureResult atEnd = configure("[font]\nsize = 16.0\nfeatures = [\n  \"liga\"\n", ParseMode::Permissive);
    REQUIRE(atEnd.diagnostics.size() == 1);
    CHECK_EQ(atEnd.diagnostics[0].line, 3);
    CHECK(Preferences::values().fontFeatures.value_or("") == "calt");
    CHECK(Effective::fontSize(Preferences::values()) == 16.0);

    // Something that is not a value: the key keeps its value, and the
    // sentence says what the key takes rather than quoting the line.
    const ConfigureResult unparsed = configure("[font]\nsize = 12pt\nfeatures = \"calt\"\n", ParseMode::Permissive);
    REQUIRE(unparsed.diagnostics.size() == 1);
    const std::string sentence = Preferences::diagnosticSentence(unparsed.diagnostics[0]);
    CHECK_MSG(sentence == "line 2: font.size takes a number \xE2\x80\x94 ignored", sentence.c_str());
    CHECK(Effective::fontSize(Preferences::values()) == 16.0);

    // A header that cannot be read. The table before it, [sound], has an
    // `audible` of its own; the line meant for [bell] sets neither, and [bell]
    // keeps the value it had.
    REQUIRE(configure("[bell]\naudible = false\n[sound]\naudible = false\n", ParseMode::Strict).isApplied);
    const ConfigureResult header = configure("[sound]\naudible = false\n[bell\naudible = true\n", ParseMode::Permissive);
    REQUIRE(header.diagnostics.size() == 1);
    CHECK(header.diagnostics[0].reason == SkipReason::MalformedLine);
    CHECK_EQ(header.diagnostics[0].line, 3);
    CHECK_MSG(!Preferences::values().soundAudible.value_or(true), "the key under the unreadable header landed in the table before it");
    CHECK_MSG(!Preferences::values().bellAudible.value_or(true), "the key under the unreadable header did not keep its value");
    (void)configure("", ParseMode::Permissive);
}

// The reason a reload stays permissive. A settings write edits the live
// document and saves it over the file, so the live document has to be the
// text the person saved: their refused line survives the write, the write
// applies, and the refused line still keeps its value.
TEST(testASettingsWriteWhileALineIsRefusedEditsTheSavedText)
{
    REQUIRE(configure("[font]\nsize = 15.0\n", ParseMode::Strict).isApplied);
    (void)configure("[font]\nsize = \"large\"\n[cursor]\nblink = true\n", ParseMode::Permissive);

    const std::optional<std::string> written = Preferences::configureKey(Key::CursorBlink, Value::ofBool(SerializeKind::Bool, false));
    REQUIRE(written.has_value());
    CHECK_MSG(written->find("size = \"large\"") != std::string::npos, "the write replaced the person's line with older text");
    CHECK(written->find("blink = false") != std::string::npos);
    CHECK(!Effective::cursorBlink(Preferences::values()));
    CHECK(Effective::fontSize(Preferences::values()) == 15.0);
    const std::string summary = Preferences::liveDiagnosticSummary();
    CHECK_MSG(summary.find("line 2: font.size") == 0, summary.c_str());
    (void)configure("", ParseMode::Permissive);
}

TEST(testAKeyEditRewritesTheLiveDocumentInPlace)
{
    const std::string original = "# my settings\n"
                                 "[font]\n"
                                 "family = \"Menlo\"  # the one I like\n"
                                 "size = 12.0\n"
                                 "\n"
                                 "[cursor]\n"
                                 "# blink = true\n";
    CHECK(configure(original, ParseMode::Strict).isApplied);
    const std::uint64_t before = Preferences::configurationEpoch();

    const std::optional<std::string> edited = Preferences::configureKey(Key::FontSize, Value::ofNumber(SerializeKind::Double, 16.5));
    CHECK(edited.has_value());
    if (!edited.has_value())
        return;
    CHECK(*edited
        == std::string("# my settings\n"
                       "[font]\n"
                       "family = \"Menlo\"  # the one I like\n"
                       "size = 16.5\n"
                       "\n"
                       "[cursor]\n"
                       "# blink = true\n"));
    CHECK_MSG(Preferences::liveDocument() == *edited, "the live document is the rewritten one");
    CHECK_MSG(Effective::fontSize(Preferences::values()) == 16.5, "and the values came from it");
    CHECK_EQ(Preferences::configurationEpoch(), before + 1);

    // A commented-out assignment is the one rewritten, so the edit lands where
    // the reader's file already put the key.
    const std::optional<std::string> blink = Preferences::configureKey(Key::CursorBlink, Value::ofBool(SerializeKind::Bool, false));
    CHECK(blink.has_value() && blink->find("[cursor]\nblink = false\n") != std::string::npos);
    CHECK(!Effective::cursorBlink(Preferences::values()));

    // A value of the wrong kind changes nothing at all.
    const std::uint64_t beforeRefusal = Preferences::configurationEpoch();
    const std::string documentBeforeRefusal = Preferences::liveDocument();
    CHECK(!Preferences::configureKey(Key::FontSize, Value::ofBool(SerializeKind::Bool, true)).has_value());
    CHECK_EQ(Preferences::configurationEpoch(), beforeRefusal);
    CHECK(Preferences::liveDocument() == documentBeforeRefusal);
}

TEST(testEachReplacementIsHeardOnceByEachLiveSubscription)
{
    int firstCount = 0;
    int secondCount = 0;
    double heardSize = 0;
    auto second = std::make_unique<Preferences::ConfigurationSubscription>([&](const Values&) { ++secondCount; });
    Preferences::ConfigurationSubscription first([&](const Values& values) {
        ++firstCount;
        heardSize = Effective::fontSize(values);
        // A subscriber may end another subscription from inside the call.
        second.reset();
    });

    (void)configure("[font]\nsize = 21.0\n", ParseMode::Permissive);
    CHECK_EQ(firstCount, 1);
    CHECK_MSG(heardSize == 21.0, "the callback sees the replacement, not the values before it");
    CHECK_MSG(secondCount <= 1, "a subscription destroyed mid-notification is called at most the once already under way");
    const int secondAfterFirst = secondCount;

    (void)configure("", ParseMode::Permissive);
    CHECK_EQ(firstCount, 2);
    CHECK_MSG(secondCount == secondAfterFirst, "a destroyed subscription is never called again");

    // A refused document is not a replacement and is not announced.
    (void)configure("nonsense\n", ParseMode::Strict);
    CHECK_EQ(firstCount, 2);
}

TEST(testAFileThatDoesNotExistIsTheDefaultTable)
{
    (void)configure("[font]\nsize = 30.0\n", ParseMode::Permissive);
    const ConfigureResult result = Preferences::configureFromFile("/definitely/not/a/config.toml", ParseMode::Strict);
    CHECK(result.isApplied);
    CHECK(result.diagnostics.empty());
    CHECK(Effective::fontSize(Preferences::values()) == Effective::fontSize(Values {}));
    CHECK(Preferences::liveDocument().empty());
}

TEST(testAMovedSubscriptionIsStillHeardAndANotifyingOneReadsTheLiveValues)
{
    int heard = 0;
    double heardSize = 0;
    Preferences::ConfigurationSubscription moved = Preferences::ConfigurationSubscription::notifying([&] {
        ++heard;
        heardSize = Effective::fontSize(Preferences::values());
    });
    Preferences::ConfigurationSubscription holder = std::move(moved);
    (void)configure("[font]\nsize = 23.0\n", ParseMode::Permissive);
    CHECK_EQ(heard, 1);
    CHECK(heardSize == 23.0);

    // A subscription that ends itself from inside its own call is safe.
    auto self = std::make_unique<Preferences::ConfigurationSubscription>([](const Values&) { });
    int selfHeard = 0;
    self = std::make_unique<Preferences::ConfigurationSubscription>([&](const Values&) {
        ++selfHeard;
        self.reset();
    });
    (void)configure("", ParseMode::Permissive);
    (void)configure("", ParseMode::Permissive);
    CHECK_EQ(selfHeard, 1);
    CHECK_EQ(heard, 3);
}

TEST(testTabShortcutHintsAreWrittenAsTokens)
{
    (void)configure("", ParseMode::Permissive);
    const std::optional<std::string> document = Preferences::configureKey(Key::TabsShowShortcuts, Value::ofText(SerializeKind::EnumToken, "command"));
    CHECK(document.has_value());
    CHECK(document->find("show_shortcuts = \"command\"") != std::string::npos);
    CHECK(Preferences::values().tabShortcutHints == Preferences::TabShortcutHints::OnCommand);
    (void)Preferences::configureKey(Key::TabsShowShortcuts, Value::ofText(SerializeKind::EnumToken, "off"));
    CHECK(Preferences::values().tabShortcutHints == Preferences::TabShortcutHints::Off);
    // A bool is not the key's kind, so it is refused before anything is written.
    CHECK(!Preferences::configureKey(Key::TabsShowShortcuts, Value::ofBool(SerializeKind::LegacyBoolOrToken, true)).has_value());
    CHECK(Preferences::values().tabShortcutHints == Preferences::TabShortcutHints::Off);
}

// The blink curve every frontend's cursor reads moves with the configuration,
// whichever door the document came through, with no frontend copying it.
// The history depth is seeded with the rest of a terminal's configuration, so
// every arm that mints a terminal — a window's session, a daemon's — keeps the
// depth the document names without reading the key itself.
// A fallback or range rule the router cannot use is dropped and named where it
// was written; a strict document holding one is refused.
TEST(testAnInvalidFontRoutingRuleIsNamed)
{
    const std::string document = "[[font.language_fallbacks]]\nscript = \"han\"\nmode = \"custom\"\nfamily = \"Noto\"\n"
                                 "[[font.language_fallbacks]]\nscript = \"klingon\"\nmode = \"custom\"\nfamily = \"Noto\"\n"
                                 "[[font.range_map]]\nrange = \"not a range\"\nmode = \"custom\"\nfamily = \"Noto\"\n";
    const ConfigureResult permissive = configure(document, ParseMode::Permissive);
    CHECK(permissive.isApplied);
    CHECK_EQ(Preferences::values().languageFallbacks.size(), std::size_t { 1 });
    CHECK(Preferences::values().rangeMap.empty());
    REQUIRE(permissive.diagnostics.size() == 2);
    CHECK_EQ(permissive.diagnostics[0].line, 5);
    CHECK(permissive.diagnostics[0].key == "[[font.language_fallbacks]]");
    CHECK(permissive.diagnostics[0].reason == SkipReason::WrongValueKind);
    CHECK_EQ(permissive.diagnostics[1].line, 9);
    CHECK(permissive.diagnostics[1].key == "[[font.range_map]]");
    CHECK_MSG(Preferences::diagnosticSentence(permissive.diagnostics[1]).find("line 9: [[font.range_map]] takes") == 0,
        Preferences::diagnosticSentence(permissive.diagnostics[1]).c_str());

    CHECK_MSG(!configure(document, ParseMode::Strict).isApplied, "a strict document with an unusable rule is refused");
    (void)configure("", ParseMode::Permissive);
}

TEST(testAnInvalidEnumTokenNamesTheAcceptedValues)
{
    const ConfigureResult result = configure("[tabs]\nshow_shortcuts = \"sometimes\"\n[cursor]\nshape = 3\n", ParseMode::Permissive);
    REQUIRE(result.diagnostics.size() == 2);
    const std::string tabs = Preferences::diagnosticSentence(result.diagnostics[0]);
    CHECK_MSG(tabs == "line 2: tabs.show_shortcuts takes \"off\", \"always\" or \"command\" \xE2\x80\x94 ignored", tabs.c_str());
    const std::string cursor = Preferences::diagnosticSentence(result.diagnostics[1]);
    CHECK_MSG(cursor.find("cursor.shape takes \"") != std::string::npos, cursor.c_str());
    (void)configure("", ParseMode::Permissive);
}

TEST(testADragModifierWarningNamesTheChordForm)
{
    const std::string expected = " takes modifiers joined by \"+\", such as \"control+command\", \"shift\" or \"none\" \xE2\x80\x94 ignored";
    const ConfigureResult result = configure("[window]\ndrag_modifier = 4\n", ParseMode::Permissive);
    REQUIRE(result.diagnostics.size() == 1);
    const std::string number = Preferences::diagnosticSentence(result.diagnostics[0]);
    CHECK_MSG(number == "line 2: window.drag_modifier" + expected, number.c_str());
    const ConfigureResult unknown = configure("[window]\ndrag_modifier = \"control+hyper\"\n", ParseMode::Permissive);
    REQUIRE(unknown.diagnostics.size() == 1);
    CHECK_MSG(Preferences::diagnosticSentence(unknown.diagnostics[0]) == "line 2: window.drag_modifier" + expected, "unknown modifier");
    const ConfigureResult good = configure("[window]\ndrag_modifier = \"Shift+Opt\"\n", ParseMode::Permissive);
    CHECK(good.diagnostics.empty());
    (void)configure("", ParseMode::Permissive);
}

TEST(testTheSeedCarriesTheScrollbackDepth)
{
    (void)configure("[terminal]\nscrollback_lines = 1500\n", ParseMode::Strict);
    Ubiquitty::Terminal configured;
    (void)Ubiquitty::Runtime::seedTerminalConfiguration(configured, Preferences::values());
    CHECK_EQ(configured.scrollbackLimit, 1500);

    (void)configure("", ParseMode::Strict);
    Ubiquitty::Terminal defaulted;
    (void)Ubiquitty::Runtime::seedTerminalConfiguration(defaulted, Preferences::values());
    CHECK_EQ(defaulted.scrollbackLimit, Effective::scrollbackLines(Values {}));
}

TEST(testTheBlinkCurveFollowsTheConfiguration)
{
    (void)configure("[appearance]\nblink_animation = false\n", ParseMode::Permissive);
    CHECK(Ubiquitty::Cursor::Blink::animation() == Ubiquitty::Cursor::Blink::Animation::OnOff);
    (void)configure("", ParseMode::Permissive);
    CHECK_MSG(Ubiquitty::Cursor::Blink::animation() == Ubiquitty::Cursor::Blink::Animation::Fade, "an unset key fades, as its default does");
}

TEST(testScriptRulesAreKeptBesideTheLiveDocument)
{
    const std::string gated = "[scripting]\nenable = true\nconfig_script = \"/bin/rules\"\n";
    (void)configure(gated, ParseMode::Permissive);
    const std::size_t declared = Preferences::values().triggers.size();

    CHECK_EQ(Preferences::configureScriptOutput("trigger HOOKED-[0-9]+ = copy\n"), 1);
    CHECK_MSG(Preferences::values().triggers.size() == declared + 1, "the printed rule joins the live configuration");

    // A by-key edit re-parses the document; the script's rule is Core's to
    // keep, and a frontend editing one key must not have to run the program again.
    (void)Preferences::configureKey(Key::FontSize, Value::ofNumber(SerializeKind::Double, 17.0));
    CHECK_MSG(Preferences::values().triggers.size() == declared + 1, "a by-key edit keeps the script's rule");
    CHECK(Effective::fontSize(Preferences::values()) == 17.0);

    // A document that shuts the gate takes no rule from the program.
    (void)configure("[font]\nsize = 12.0\n", ParseMode::Permissive);
    CHECK_MSG(Preferences::values().triggers.size() == Values {}.triggers.size(), "a shut gate merges nothing");

    // Empty output forgets the rules, under an open gate too.
    (void)configure(gated, ParseMode::Permissive);
    CHECK_EQ(Preferences::configureScriptOutput(""), 0);
    CHECK_EQ(Preferences::values().triggers.size(), declared);
}

TEST(testTheLiveDiagnosticsReadAsOneLine)
{
    (void)configure("[font]\nsize = 14.0\n", ParseMode::Permissive);
    CHECK_MSG(Preferences::liveDiagnosticSummary().empty(), "a document that applied whole has nothing to say");

    (void)configure("[font]\nsise = 14.0\n[cursor]\nblink = 3\n", ParseMode::Permissive);
    const std::string summary = Preferences::liveDiagnosticSummary();
    CHECK_MSG(summary.find("line 2: no setting named font.sise") == 0, summary.c_str());
    CHECK_MSG(summary.find("(and 1 more)") != std::string::npos, summary.c_str());

    // A strict refusal leaves the live document, and so its diagnostics.
    (void)configure("[font]\nother = 1\n", ParseMode::Strict);
    CHECK(Preferences::liveDiagnosticSummary() == summary);
}

UBIQUITTY_TEST_MAIN()

TEST(testResetPreviewLeavesLiveConfigurationAndSubscribersUntouched)
{
    const std::string original = Preferences::liveDocument();
    const auto configured = configure("[font]\nsize = 22\nfamily = \"Menlo\"\n", ParseMode::Strict);
    CHECK(configured.isApplied);
    int notifications = 0;
    Preferences::ConfigurationSubscription subscription([&notifications](const Values&) { ++notifications; });
    const auto epoch = Preferences::configurationEpoch();
    const std::string live = Preferences::liveDocument();
    const auto edited = Preferences::removeSectionKey(live, "font", "size");
    const auto preview = Preferences::previewConfiguration(edited);
    CHECK(!preview.fontSize.has_value());
    CHECK(preview.fontFamily.value_or("") == "Menlo");
    CHECK(Preferences::values().fontSize.value_or(0) == 22);
    CHECK(Preferences::liveDocument() == live);
    CHECK(Preferences::configurationEpoch() == epoch);
    CHECK(notifications == 0);
    (void)configure(original, ParseMode::Permissive);
}

TEST(testSettingAddressUsesTheWriterVocabulary)
{
    for (const auto& descriptor : writeDescriptors)
        CHECK(Preferences::keyForAddress(std::string(descriptor.section) + "." + descriptor.key) == descriptor.id);
    CHECK(!Preferences::keyForAddress("font.unknown"));
    CHECK(!Preferences::keyForAddress("profiles.font_size"));
}
