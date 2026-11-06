// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// FuzzDriver — runs a libFuzzer harness over a fixed corpus, without libFuzzer.
//
// The harnesses build only under --with-fuzz, which defaults off, and the entry
// that reaches them is shell_fuzz_harnesses_compile — syntax-only by its own
// header. So without this driver a harness is compiled and never executed, and
// an assertion inside one never runs: fuzz_sync carries sixteen, which make it
// an oracle rather than a crash detector, and none of them would be evaluated.
//
// This is not fuzzing and does not replace it. A fixed corpus finds what a fixed
// corpus finds. What it does is make the harness a test: the decoder is entered,
// the oracle's assertions are evaluated, and a change that breaks either is
// reported by ctest rather than waiting for someone to run a fuzzer.
//
// The corpus is empty input, one byte, a truncated header, and bytes belonging
// to another format. Those are the shapes that reach a decoder's length checks
// and early returns, which is where an off-by-one lives.
//
// It reaches a small part of a harness's own lines and of the protocol source
// it drives, and what it misses is most of the oracle. An assertion like
// `assert(decode(payload, again))` sits behind a decode that succeeded, and
// malformed bytes never get there — reaching those needs well-formed seeds, one
// set per harness, which is knowledge each harness's subject carries and this
// driver does not.
//
// So the claim is bounded: the harnesses execute, their entry and refusal paths
// run, and a change that breaks either is reported by ctest. The success-guarded
// half stays for a real fuzzer with a seeded corpus.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

/// The inputs every harness is driven over.
///
/// Shared rather than per-harness because the point is the shapes a decoder
/// mishandles, and those do not depend on which decoder it is: nothing, almost
/// nothing, a header with no body, and bytes belonging to another format.
///
/// - Returns: The corpus, in a fixed order.
std::vector<std::vector<std::uint8_t>> corpus()
{
    std::vector<std::vector<std::uint8_t>> inputs;
    inputs.emplace_back();
    inputs.push_back({ 0x00 });
    inputs.push_back({ 0xFF });
    inputs.push_back({ 0x1B }); // a lone escape
    inputs.push_back({ 0x1B, '[' }); // a CSI with nothing after it
    inputs.push_back({ 0x1B, '_', 'G' }); // an APC introducer, truncated
    inputs.push_back({ 0x78, 0x9C }); // a zlib header, no body
    inputs.push_back({ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A }); // PNG magic, no chunks
    inputs.emplace_back(4096, 0x00);
    inputs.emplace_back(4096, 0xFF);

    // Patterned rather than random: a seeded generator would make a failure
    // depend on the library's implementation, and this has to reproduce
    // anywhere the suite runs.
    for (int step = 1; step <= 24; ++step) {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(257);
        for (int index = 0; index < 257; ++index)
            bytes.push_back(static_cast<std::uint8_t>((index * step + step * 7) & 0xFF));
        inputs.push_back(std::move(bytes));
    }
    return inputs;
}

} // namespace

int main()
{
    const std::vector<std::vector<std::uint8_t>> inputs = corpus();
    for (const std::vector<std::uint8_t>& input : inputs)
        (void)LLVMFuzzerTestOneInput(input.data(), input.size());

    // The count is the verdict's denominator. A driver that silently drove
    // nothing exits 0 exactly like one that drove everything, and this tier has
    // met that shape often enough to print the number.
    std::printf("=== Results: ALL PASSED (%zu input(s)) ===\n", inputs.size());
    return 0;
}
