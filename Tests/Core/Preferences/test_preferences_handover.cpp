// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Preferences/HandoverValues.h"
#include "Core/Preferences/HandoverValuesSchema.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Harness.h"

#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Preferences;
namespace Detail = Ubiquitty::Preferences::HandoverDetail;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

constexpr HandoverValuesLimits limits { 4 * 1024 * 1024, 65536, 1024, 8 * 1024 * 1024 };

template<typename T> struct OptionalValue : std::false_type { };
template<typename T> struct OptionalValue<std::optional<T>> : std::true_type {
    using Value = T;
};
template<typename T> struct VectorValue : std::false_type { };
template<typename T> struct VectorValue<std::vector<T>> : std::true_type {
    using Value = T;
};

template<typename T>
void fill(T& value, unsigned& serial)
{
    if constexpr (OptionalValue<T>::value)
        fill(value.emplace(), serial);
    else if constexpr (VectorValue<T>::value) {
        value.resize(2);
        for (auto& entry : value)
            fill(entry, serial);
    } else if constexpr (std::is_array_v<T>) {
        for (auto& entry : value)
            fill(entry, serial);
    } else if constexpr (std::is_same_v<T, std::string>)
        value = "value-" + std::to_string(++serial) + std::string("\0\xff", 2);
    else if constexpr (std::is_same_v<T, bool>)
        value = (++serial % 2) != 0;
    else if constexpr (std::is_enum_v<T>)
        value = static_cast<T>(1);
    else if constexpr (std::is_same_v<T, double>)
        value = static_cast<double>(++serial) + 0.125;
    else if constexpr (std::is_arithmetic_v<T>)
        value = static_cast<T>(++serial);
    else
        std::apply([&](const auto&... field) { (fill(value.*field.member, serial), ...); }, Detail::Schema<T>::fields);
}

template<typename T>
bool exact(const T& first, const T& second)
{
    if constexpr (OptionalValue<T>::value)
        return first.has_value() == second.has_value() && (!first || exact(*first, *second));
    else if constexpr (VectorValue<T>::value) {
        if (first.size() != second.size())
            return false;
        for (std::size_t i = 0; i < first.size(); ++i) {
            if (!exact(first[i], second[i]))
                return false;
        }
        return true;
    } else if constexpr (std::is_array_v<T>) {
        for (std::size_t i = 0; i < std::extent_v<T>; ++i) {
            if (!exact(first[i], second[i]))
                return false;
        }
        return true;
    } else if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<std::uint64_t>(first) == std::bit_cast<std::uint64_t>(second);
    else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>)
        return first == second;
    else
        return std::apply([&](const auto&... field) { return (exact(first.*field.member, second.*field.member) && ...); }, Detail::Schema<T>::fields);
}

/// Writes `value` in the codec's grammar, independently of the codec.
template<typename T>
void writeIndependently(Sync::WireWriter& writer, const T& value)
{
    if constexpr (OptionalValue<T>::value) {
        writer.u8(value.has_value());
        if (value)
            writeIndependently(writer, *value);
    } else if constexpr (VectorValue<T>::value) {
        writer.u64(value.size());
        for (const auto& entry : value)
            writeIndependently(writer, entry);
    } else if constexpr (std::is_array_v<T>) {
        for (const auto& entry : value)
            writeIndependently(writer, entry);
    } else if constexpr (std::is_same_v<T, std::string>) {
        writer.u64(value.size());
        for (const char character : value)
            writer.u8(static_cast<std::uint8_t>(character));
    } else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::uint8_t>)
        writer.u8(static_cast<std::uint8_t>(value));
    else if constexpr (std::is_enum_v<T>)
        writer.u32(static_cast<std::uint32_t>(value));
    else if constexpr (std::is_same_v<T, double>)
        writer.u64(std::bit_cast<std::uint64_t>(value));
    else if constexpr (std::is_same_v<T, int>)
        writer.u32(std::bit_cast<std::uint32_t>(value));
    else if constexpr (std::is_same_v<T, std::uint32_t>)
        writer.u32(value);
    else
        std::apply([&](const auto&... field) { (writeIndependently(writer, value.*field.member), ...); }, Detail::Schema<T>::fields);
}

/// A whole record, opened with `version` and closed with its digest.
std::vector<std::uint8_t> independentRecord(const Values& values, std::uint16_t version)
{
    std::vector<std::uint8_t> bytes;
    Sync::WireWriter writer(bytes);
    writer.u16(version);
    writeIndependently(writer, values);
    const auto digest = Sync::digestOf(bytes);
    writer.u64(digest.low);
    writer.u64(digest.high);
    return bytes;
}

void redigest(std::vector<std::uint8_t>& bytes)
{
    bytes.resize(bytes.size() - 16);
    const auto digest = Sync::digestOf(bytes);
    Sync::WireWriter writer(bytes);
    writer.u64(digest.low);
    writer.u64(digest.high);
}

#if defined(__clang__) && __has_builtin(__builtin_dump_struct)
struct MemberNames {
    std::vector<std::string> names;
    template<typename... Arguments> void operator()(const char*, Arguments... arguments) { take(arguments...); }
    void take(...) { }
    template<typename Value> void take(const char* indent, const char*, const char* name, Value)
    {
        if (std::strcmp(indent, "  ") == 0)
            names.emplace_back(name);
    }
    void take(const char* indent, const char*, const char* name)
    {
        if (std::strcmp(indent, "  ") == 0)
            names.emplace_back(name);
    }
};

template<typename T>
void checkMembers()
{
    T value;
    MemberNames observed;
    __builtin_dump_struct(&value, observed);
    CHECK_EQ(observed.names.size(), std::tuple_size_v<decltype(Detail::Schema<T>::fields)>);
    for (const auto& name : observed.names) {
        const auto isListed = std::apply([&](const auto&... field) { return ((field.name == name) || ...); }, Detail::Schema<T>::fields);
        CHECK_MSG(isListed, name.c_str());
    }
}
#endif

} // namespace

TEST(everyNestedMemberIsNamedIncludingPaddingHoles)
{
#if defined(__clang__) && __has_builtin(__builtin_dump_struct)
    checkMembers<SkippedKey>();
    checkMembers<ColorEntry>();
    checkMembers<Themes::ThemeAppearance>();
    checkMembers<ThemeEntry>();
    checkMembers<KeyBindingEntry>();
    checkMembers<LinkHandlerEntry>();
    checkMembers<TagEntry>();
    checkMembers<TriggerEntry>();
    checkMembers<MacroEntry>();
    checkMembers<CharacterClassEntry>();
    checkMembers<LanguageFallbackEntry>();
    checkMembers<RangeMapEntry>();
    checkMembers<Profile>();
    checkMembers<PlatformOverride>();
    checkMembers<CapabilityOverrides>();
    checkMembers<Values>();
#else
    UbiquittyTest::skip("this compiler has no __builtin_dump_struct; the compile-time layout tiling still applies");
#endif
}

TEST(everyStoredFieldArrivesWithoutDefaultResolution)
{
    Values source;
    unsigned serial = 0;
    fill(source, serial);
    REQUIRE(serial > 400);
    auto bytes = encodeHandoverValues(source, limits);
    REQUIRE(bytes);
    auto restored = decodeHandoverValues(*bytes, limits);
    REQUIRE(restored);
    CHECK(exact(source, *restored));
    REQUIRE(restored->profiles.size() == 2);
    CHECK(restored->profiles[0].processExit == source.profiles[0].processExit);
    CHECK(restored->profiles[0].parentProfile == source.profiles[0].parentProfile);
    CHECK(restored->platformOverrides[1].profile.startupHost == source.platformOverrides[1].profile.startupHost);
    CHECK(restored->controlToken == source.controlToken);
    CHECK(restored->scriptingEnable == source.scriptingEnable);
    CHECK(restored->remoteCapabilities.readExfiltration == source.remoteCapabilities.readExfiltration);
    CHECK(restored->skippedKeys[1].key == source.skippedKeys[1].key);
    restored->profiles[0].name = "replacement";
    restored->themes[0].ansi[0].red ^= 1;
    CHECK(restored->profiles[0].name != source.profiles[0].name);
    CHECK(restored->themes[0].ansi[0].red != source.themes[0].ansi[0].red);
}

TEST(optionalAbsenceAndEveryFloatingPointBitRemainExact)
{
    Values source;
    source.fontFamily = "";
    source.ligatures = false;
    source.fontWeight = 0;
    source.fontSize = -0.0;
    source.baselineOffset = std::bit_cast<double>(std::uint64_t { 0x7ff8000000000123 });
    source.proportionalFontSize = std::numeric_limits<double>::infinity();
    source.boldFontWeight = std::numeric_limits<int>::min();
    source.profiles.emplace_back("temporary");
    source.profiles.back().scope = ProfileScope::Ephemeral;
    source.profiles.back().fontSize = 0;
    source.platformOverrides[0].present = true;
    auto bytes = encodeHandoverValues(source, limits);
    REQUIRE(bytes);
    auto restored = decodeHandoverValues(*bytes, limits);
    REQUIRE(restored);
    CHECK(exact(source, *restored));
    CHECK(restored->fontFamily && restored->fontFamily->empty());
    CHECK(restored->ligatures && !*restored->ligatures);
    CHECK(!restored->fallbackFont);
    CHECK(!restored->profiles[0].parentProfile);
    CHECK(restored->platformOverrides[0].present);
    CHECK(!restored->platformOverrides[1].present);
}

TEST(binaryAndRepeatedListEntriesAreNotParsedOrNormalized)
{
    Values source;
    source.fontFamily = std::string("A\0B", 3);
    source.triggers = { { ".*", "run_command:literal" }, { ".*", "copy:second" } };
    source.macros = { { "same", std::string("\xff\0", 2) }, { "same", "" } };
    source.tags = { { .name = "production", .color = "#cc3300", .matchHost = "prod-*", .matchProfile = "", .prominence = "loud", .icon = "flame", .iconWeight = "regular", .showsName = false, .showsInTitleBar = true, .showsInTabs = false, .showsInPaneHeaders = false } };
    source.rangeMap = { { 0x10ffff, 0, "original spelling", "custom", "family" } };
    source.profiles.emplace_back("Default");
    source.profiles.back().parentProfile = "Default";
    auto bytes = encodeHandoverValues(source, limits);
    REQUIRE(bytes);
    const std::array<std::uint8_t, 14> prefix { 7, 0, 1, 3, 0, 0, 0, 0, 0, 0, 0, 'A', 0, 'B' };
    CHECK(std::equal(prefix.begin(), prefix.end(), bytes->begin()));
    auto restored = decodeHandoverValues(*bytes, limits);
    REQUIRE(restored);
    CHECK(exact(source, *restored));
    CHECK_EQ(restored->triggers.size(), 2u);
    REQUIRE(restored->tags.size() == 1u);
    CHECK(restored->tags[0].icon == "flame" && restored->tags[0].iconWeight == "regular");
    CHECK(!restored->tags[0].showsName && restored->tags[0].showsInTitleBar && !restored->tags[0].showsInTabs && !restored->tags[0].showsInPaneHeaders);
    CHECK(restored->rangeMap[0].firstCodePoint > restored->rangeMap[0].lastCodePoint);
}

TEST(emptyValuesHaveAStableVersionedEncoding)
{
    auto first = encodeHandoverValues(Values {}, limits);
    REQUIRE(first);
    auto restored = decodeHandoverValues(*first, limits);
    REQUIRE(restored);
    CHECK(exact(Values {}, *restored));
    auto second = encodeHandoverValues(*restored, limits);
    REQUIRE(second);
    CHECK(*first == *second);
    CHECK_EQ((*first)[0], 7);
    CHECK_EQ((*first)[1], 0);
}

TEST(everyTruncationCorruptionAndExtraByteIsRefused)
{
    Values source;
    source.fontFamily = "retained";
    auto bytes = encodeHandoverValues(source, limits);
    REQUIRE(bytes);
    for (std::size_t length = 0; length < bytes->size(); ++length)
        CHECK(!decodeHandoverValues(std::span(*bytes).first(length), limits));
    auto corrupt = *bytes;
    corrupt[5] ^= 0x80;
    CHECK(decodeHandoverValues(corrupt, limits).error() == HandoverValuesError::DigestMismatch);
    auto longer = *bytes;
    longer.insert(longer.end() - 16, 0);
    redigest(longer);
    CHECK(decodeHandoverValues(longer, limits).error() == HandoverValuesError::Malformed);
    (*bytes)[0] = 3;
    redigest(*bytes);
    CHECK(decodeHandoverValues(*bytes, limits).error() == HandoverValuesError::Malformed);
    (*bytes)[0] = 8;
    CHECK(decodeHandoverValues(*bytes, limits).error() == HandoverValuesError::NewerVersion);
}

// A host reads only the configuration record it writes. The same fields
// under any earlier version are refused rather than read in another layout.
TEST(aRecordOfAnEarlierVersionIsRefused)
{
    Values source;
    unsigned serial = 0;
    fill(source, serial);
    const auto newest = independentRecord(source, 7);
    CHECK(encodeHandoverValues(source, limits).value() == newest);
    const auto restored = decodeHandoverValues(newest, limits);
    REQUIRE(restored);
    CHECK(exact(source, *restored));
    for (std::uint16_t earlier = 0; earlier < 7; ++earlier) {
        const auto refused = decodeHandoverValues(independentRecord(source, earlier), limits);
        REQUIRE(!refused);
        CHECK(refused.error() == HandoverValuesError::Malformed);
    }
}

TEST(recomputedDigestsDoNotAdmitInvalidPresenceLengthsOrEnums)
{
    auto empty = encodeHandoverValues(Values {}, limits);
    REQUIRE(empty);
    auto presence = *empty;
    presence[2] = 2;
    redigest(presence);
    CHECK(decodeHandoverValues(presence, limits).error() == HandoverValuesError::Malformed);
    Values source;
    source.fontFamily = "x";
    auto bytes = encodeHandoverValues(source, limits);
    REQUIRE(bytes);
    for (std::size_t i = 3; i < 11; ++i)
        (*bytes)[i] = 0xff;
    redigest(*bytes);
    CHECK(decodeHandoverValues(*bytes, limits).error() == HandoverValuesError::OverCapacity);
    source.fontFamily.reset();
    source.titlebarStyle = TitlebarStyle::Themed;
    auto themed = encodeHandoverValues(source, limits);
    source.titlebarStyle = TitlebarStyle::Native;
    auto native = encodeHandoverValues(source, limits);
    REQUIRE(themed && native);
    std::size_t enumOffset = 0;
    for (std::size_t i = 2; i < themed->size() - 16; ++i) {
        if ((*themed)[i] != (*native)[i]) {
            REQUIRE(!enumOffset);
            enumOffset = i;
        }
    }
    REQUIRE(enumOffset);
    (*themed)[enumOffset] = 2;
    redigest(*themed);
    CHECK(decodeHandoverValues(*themed, limits).error() == HandoverValuesError::Malformed);
    source.titlebarStyle = static_cast<TitlebarStyle>(2);
    CHECK(encodeHandoverValues(source, limits).error() == HandoverValuesError::Malformed);
}

TEST(independentReceiverBoundsRefuseBeforeConstructingAResult)
{
    Values source;
    unsigned serial = 0;
    fill(source, serial);
    auto bytes = encodeHandoverValues(source, limits);
    REQUIRE(bytes);
    for (std::size_t HandoverValuesLimits::* field : { &HandoverValuesLimits::maxSectionBytes,
             &HandoverValuesLimits::maxStringBytes, &HandoverValuesLimits::maxCollectionEntries,
             &HandoverValuesLimits::maxDecodedBytes }) {
        auto small = limits;
        small.*field = 1;
        CHECK(encodeHandoverValues(source, small).error() == HandoverValuesError::OverCapacity);
        CHECK(decodeHandoverValues(*bytes, small).error() == HandoverValuesError::OverCapacity);
    }
    CHECK(encodeHandoverValues(source, limits).value() == *bytes);
}

TEST(decodedStorageBoundaryIncludesNestedOwnersAndStrings)
{
    Values source;
    source.profiles.emplace_back("abc");
    source.profiles.back().fontFamily = "four";
    source.platformOverrides[1].keyBindings.push_back({ "a", "b" });
    auto exactLimits = limits;
    exactLimits.maxDecodedBytes = sizeof(Values) + sizeof(Profile) + 3 + 4 + sizeof(KeyBindingEntry) + 2;
    auto bytes = encodeHandoverValues(source, exactLimits);
    REQUIRE(bytes);
    CHECK(decodeHandoverValues(*bytes, exactLimits));
    --exactLimits.maxDecodedBytes;
    CHECK(encodeHandoverValues(source, exactLimits).error() == HandoverValuesError::OverCapacity);
    CHECK(decodeHandoverValues(*bytes, exactLimits).error() == HandoverValuesError::OverCapacity);
}

UBIQUITTY_TEST_MAIN()
