// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_known_hosts.cpp — host-key pinning policy.
//
// The cases are about refusal: a changed key is Mismatched, an absent pin is
// Unknown rather than a quiet pass, and a fingerprint that failed to compute
// never compares equal to a pin. The fingerprint form is checked against a
// digest OpenSSH prints for a known input, so a frontend can show ours beside
// `ssh-keygen -lf` output and compare them character by character.

#include "Core/Transport/KnownHosts.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Transport;

namespace {

std::span<const std::uint8_t> bytesOf(const std::string& text)
{
    return { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() };
}

} // namespace

// ---- The fingerprint is the form OpenSSH prints ----
TEST(test_known_hosts_fingerprint_form)
{
    // SHA-256 of the empty input is a fixed, published value; base64 of that
    // digest with the padding stripped is what OpenSSH would print.
    const std::string fingerprint = hostKeyFingerprint({});
    CHECK_MSG(fingerprint == "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU", fingerprint.c_str());
    CHECK_MSG(fingerprint.rfind("SHA256:", 0) == 0, "prefixed SHA256:");
    CHECK_MSG(fingerprint.back() != '=', "OpenSSH prints the base64 unpadded");
    CHECK_MSG(fingerprint.size() == 7 + 43, "32 digest bytes base64 without padding");
}

// ---- Different keys fingerprint differently ----
TEST(test_known_hosts_fingerprint_distinguishes_keys)
{
    const std::string first("ssh-ed25519 AAAA-first");
    const std::string second("ssh-ed25519 AAAA-second");
    CHECK_MSG(hostKeyFingerprint(bytesOf(first)) != hostKeyFingerprint(bytesOf(second)), "distinct keys, distinct fingerprints");
    CHECK_MSG(hostKeyFingerprint(bytesOf(first)) == hostKeyFingerprint(bytesOf(first)), "the same key fingerprints the same way");
}

// ---- Host and port pin independently ----
TEST(test_known_hosts_endpoint)
{
    CHECK_MSG(hostKeyEndpoint("build.example", 22) == "build.example:22", "endpoint spelling");
    CHECK_MSG(hostKeyEndpoint("build.example", 22) != hostKeyEndpoint("build.example", 2222), "a second port is a second endpoint");
    // An IPv6 literal keeps its brackets, so the trailing :port stays the
    // separator and the address is not confused for one.
    CHECK_MSG(hostKeyEndpoint("[::1]", 22) == "[::1]:22", "IPv6 literal endpoint");
}

// ---- The verdicts ----
TEST(test_known_hosts_verdicts)
{
    const std::string pinned = hostKeyFingerprint(bytesOf(std::string("the-host-key")));
    const std::string impostor = hostKeyFingerprint(bytesOf(std::string("a-different-key")));

    CHECK_MSG(verifyHostKeyFingerprint(pinned, pinned) == HostKeyVerdict::Matched, "the pinned key matches");
    CHECK_MSG(verifyHostKeyFingerprint(pinned, impostor) == HostKeyVerdict::Mismatched, "a changed key is a mismatch");
    CHECK_MSG(verifyHostKeyFingerprint(std::string(), impostor) == HostKeyVerdict::Unknown, "no pin is Unknown, not a pass");
}

// ---- A fingerprint that failed to compute never matches ----
TEST(test_known_hosts_empty_presented_never_matches)
{
    // Both empty would compare equal under a plain string test, which would
    // turn a failed digest into a silent pass.
    CHECK_MSG(verifyHostKeyFingerprint("SHA256:whatever", std::string()) == HostKeyVerdict::Mismatched, "an empty presented fingerprint is refused");
}

UBIQUITTY_TEST_MAIN()
