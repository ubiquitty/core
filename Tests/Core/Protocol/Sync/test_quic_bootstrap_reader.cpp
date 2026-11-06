// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_quic_bootstrap_reader.cpp — reading the bootstrap line off a carrier
// that delivers it in pieces.
//
// The cases that matter are the ones a read-to-end-of-file frontend never sees:
// a line split across deliveries, a banner ahead of it, and a carrier that
// closes without one. A half-supplied line is refused rather than skipped,
// since dialling QUIC on a token with no cert pin is the failure this guards.

#include "Core/Protocol/Sync/QUICBootstrapReader.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using namespace Ubiquitty::Protocol::Sync;
using State = QUICBootstrapReader::State;

namespace {

std::span<const std::uint8_t> bytesOf(std::string_view text)
{
    return { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() };
}

// A well-formed line: port, a token, and a 32-byte cert pin, all lowercase hex.
std::string wellFormedLine()
{
    const std::string token(16, 'a'); // 8 decoded bytes
    const std::string certificate(64, 'b'); // 32 decoded bytes
    return "quic_port=44300 quic_token=" + token + " quic_cert_sha256=" + certificate + "\n";
}

} // namespace

// ---- One delivery carrying the whole line ----
TEST(test_quic_bootstrap_reader_single_delivery)
{
    QUICBootstrapReader reader;
    CHECK_MSG(reader.feed(bytesOf(wellFormedLine())) == State::Ready, "ready after one delivery");
    CHECK_MSG(reader.info().port == 44300, "port decoded");
    CHECK_MSG(reader.info().token.size() == 8, "token decoded to bytes");
    CHECK_MSG(reader.info().certificateSHA256.size() == 32, "cert pin decoded to 32 bytes");
}

// ---- A line split across deliveries at every offset ----
//
// The case a subprocess frontend cannot hit and an SSH exec channel hits
// constantly: the split lands mid-field, mid-hex-digit, mid-newline.
TEST(test_quic_bootstrap_reader_split_at_every_offset)
{
    const std::string line = wellFormedLine();
    for (std::size_t split = 0; split <= line.size(); ++split) {
        QUICBootstrapReader reader;
        const State afterFirst = reader.feed(bytesOf(std::string_view(line).substr(0, split)));
        if (afterFirst == State::Ready) {
            // Only a split at or past the newline can complete the line early.
            CHECK_MSG(split >= line.size(), "completed before the line ended");
            continue;
        }
        CHECK_MSG(afterFirst == State::NeedMoreData, "a partial line is held, not refused");
        const State afterSecond = reader.feed(bytesOf(std::string_view(line).substr(split)));
        CHECK_MSG(afterSecond == State::Ready, "ready once the remainder arrives");
        CHECK_MSG(reader.info().port == 44300, "port survives the split");
        CHECK_MSG(reader.info().certificateSHA256.size() == 32, "cert pin survives the split");
    }
}

// ---- One byte at a time ----
TEST(test_quic_bootstrap_reader_byte_at_a_time)
{
    const std::string line = wellFormedLine();
    QUICBootstrapReader reader;
    State state = State::NeedMoreData;
    for (char c : line)
        state = reader.feed(bytesOf(std::string_view(&c, 1)));
    CHECK_MSG(state == State::Ready, "ready after the last byte");
    CHECK_MSG(reader.info().port == 44300, "port decoded byte-wise");
}

// ---- A login banner ahead of the line is noise, not an answer ----
TEST(test_quic_bootstrap_reader_ignores_a_banner)
{
    QUICBootstrapReader reader;
    CHECK_MSG(reader.feed(bytesOf("Welcome to Ubuntu 24.04 LTS\n")) == State::NeedMoreData, "a banner line is not an answer");
    CHECK_MSG(reader.feed(bytesOf("Last login: Tue Aug 18\n")) == State::NeedMoreData, "still waiting");
    CHECK_MSG(reader.feed(bytesOf(wellFormedLine())) == State::Ready, "the real line still lands");
    CHECK_MSG(reader.info().port == 44300, "port decoded past the banner");
}

// ---- A carrier that closes without a line is refused ----
TEST(test_quic_bootstrap_reader_close_without_a_line)
{
    QUICBootstrapReader reader;
    reader.feed(bytesOf("Welcome\n"));
    CHECK_MSG(reader.finish() == State::Refused, "no line means refused");
    CHECK_MSG(!reader.refusalReason().empty(), "refusal carries a reason");
}

// ---- A command that exits without a trailing newline still answered ----
TEST(test_quic_bootstrap_reader_unterminated_line_on_close)
{
    std::string line = wellFormedLine();
    line.pop_back(); // drop the newline
    QUICBootstrapReader reader;
    CHECK_MSG(reader.feed(bytesOf(line)) == State::NeedMoreData, "held while unterminated");
    CHECK_MSG(reader.finish() == State::Ready, "parsed at close");
    CHECK_MSG(reader.info().port == 44300, "port decoded from the unterminated line");
}

// ---- A line missing the cert pin is refused, never skipped ----
//
// The dangerous shape: a token arrives, a pin does not. Skipping the line in
// the hope of a better one would leave the caller dialling unpinned.
TEST(test_quic_bootstrap_reader_missing_cert_is_refused)
{
    QUICBootstrapReader reader;
    const std::string token(16, 'a');
    CHECK_MSG(reader.feed(bytesOf("quic_port=44300 quic_token=" + token + "\n")) == State::Refused, "a pinless line is refused");
    CHECK_MSG(!reader.refusalReason().empty(), "refusal carries a reason");
}

// ---- A malformed field is refused rather than treated as a banner ----
TEST(test_quic_bootstrap_reader_malformed_field_is_refused)
{
    QUICBootstrapReader reader;
    const std::string certificate(64, 'b');
    // An odd-length token cannot decode; the parser rejects the line, and the
    // line names a bootstrap field, so it is an answer that failed.
    CHECK_MSG(reader.feed(bytesOf("quic_port=44300 quic_token=abc quic_cert_sha256=" + certificate + "\n")) == State::Refused, "malformed hex is refused");
}

// ---- A decided read stays decided ----
TEST(test_quic_bootstrap_reader_is_terminal)
{
    QUICBootstrapReader reader;
    CHECK_MSG(reader.feed(bytesOf(wellFormedLine())) == State::Ready, "ready");
    // A remote that keeps talking cannot unsettle the answer.
    CHECK_MSG(reader.feed(bytesOf("quic_port=1 quic_token=ff quic_cert_sha256=ff\n")) == State::Ready, "still ready");
    CHECK_MSG(reader.info().port == 44300, "the first answer stands");
    CHECK_MSG(reader.finish() == State::Ready, "finish does not undo it");

    QUICBootstrapReader refused;
    refused.finish();
    CHECK_MSG(refused.state() == State::Refused, "refused");
    CHECK_MSG(refused.feed(bytesOf(wellFormedLine())) == State::Refused, "a refusal is not reversed by later input");
}

// ---- A remote that streams without completing a line is cut off ----
TEST(test_quic_bootstrap_reader_caps_the_buffer)
{
    QUICBootstrapReader reader;
    const std::string chunk(1024, 'x'); // no newline anywhere
    State state = State::NeedMoreData;
    for (int i = 0; i < 16 && state == State::NeedMoreData; ++i)
        state = reader.feed(bytesOf(chunk));
    CHECK_MSG(state == State::Refused, "an unbounded unterminated stream is refused");
    CHECK_MSG(!reader.refusalReason().empty(), "refusal carries a reason");
}

// ---- A CRLF carrier parses the same ----
TEST(test_quic_bootstrap_reader_tolerates_crlf)
{
    std::string line = wellFormedLine();
    line.insert(line.size() - 1, "\r");
    QUICBootstrapReader reader;
    CHECK_MSG(reader.feed(bytesOf(line)) == State::Ready, "CRLF line parses");
    CHECK_MSG(reader.info().certificateSHA256.size() == 32, "cert pin decoded past the CR");
}

TEST(testOversizedCompleteLineIsRefused)
{
    const std::string line = std::string(maxQUICBootstrapBufferSize + 1, 'x') + "\n" + wellFormedLine();
    for (const std::size_t split : { std::size_t { 0 }, maxQUICBootstrapBufferSize, maxQUICBootstrapBufferSize + 1 }) {
        QUICBootstrapReader reader;
        (void)reader.feed(bytesOf(std::string_view(line).substr(0, split)));
        CHECK(reader.feed(bytesOf(std::string_view(line).substr(split))) == State::Refused);
        CHECK(reader.finish() == State::Refused);
        CHECK(reader.info().port == 0);
    }
}

TEST(testExactLimitAndManyShortLinesAreAccepted)
{
    QUICBootstrapReader reader;
    CHECK(reader.feed(bytesOf(std::string(maxQUICBootstrapBufferSize, 'x'))) == State::NeedMoreData);
    CHECK(reader.feed(bytesOf("\n")) == State::NeedMoreData);
    std::string banners;
    for (std::size_t index = 0; index < maxQUICBootstrapBufferSize; ++index)
        banners += "banner\n";
    CHECK(reader.feed(bytesOf(banners + wellFormedLine())) == State::Ready);
}

// A bootstrap pinned to a daemon that is gone says so in place of its line,
// in whatever pieces the carrier delivers. The read ends there, naming the
// socket, and is neither a refusal to fall back from nor noise to read past.
TEST(testAPinnedDaemonGoneEndsTheReadAndNamesItsSocket)
{
    QUICBootstrapReader reader;
    const std::string output = "Last login: today\nUBIQUITTY-INSTANCE-GONE host-c756f263bc841e61.sock\r\n";
    for (std::size_t split = 0; split < output.size(); ++split) {
        QUICBootstrapReader pieces;
        (void)pieces.feed(bytesOf(std::string_view(output).substr(0, split)));
        CHECK(pieces.feed(bytesOf(std::string_view(output).substr(split))) == State::InstanceGone);
        CHECK(pieces.goneSocketName() == "host-c756f263bc841e61.sock");
        CHECK(pieces.refusalReason().empty());
        CHECK(pieces.feed(bytesOf(wellFormedLine())) == State::InstanceGone);
    }
    // A gone line that names no socket is noise, and the read goes on.
    CHECK(reader.feed(bytesOf("UBIQUITTY-INSTANCE-GONE ../host.sock\n")) == State::NeedMoreData);
    CHECK(reader.feed(bytesOf(wellFormedLine())) == State::Ready);
    CHECK(reader.goneSocketName().empty());
}

UBIQUITTY_TEST_MAIN()
