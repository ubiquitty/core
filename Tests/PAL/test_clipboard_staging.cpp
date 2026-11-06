// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_clipboard_staging.cpp — the typed capture staging on the clipboard
// seam (PAL/TerminalClients.h): what a single-threaded asynchronous client
// stages at approval and serves Core's type and item reads from. The web and
// Linux clients both answer through this one implementation.

#include "PAL/TerminalClients.h"

#include <string>
#include <string_view>
#include <vector>

namespace {

Ubiquitty::ClipboardItem item(std::string_view mime, std::string_view bytes)
{
    return { mime, { bytes.data(), bytes.size() } };
}

std::string types(const Ubiquitty::ClipboardReadItemStaging& staging)
{
    char buffer[256];
    const int n = staging.serveTypes(buffer);
    if (n < 0)
        return "<unstaged>";
    return std::string(buffer, static_cast<std::size_t>(n));
}

std::string bytes(const Ubiquitty::ClipboardReadItemStaging& staging, std::string_view mime)
{
    char buffer[256];
    const int n = staging.serveItem(mime, buffer);
    if (n < 0)
        return "<unserved>";
    return std::string(buffer, static_cast<std::size_t>(n));
}

} // namespace

TEST(testStagingServesTypesAndItemsUntilReplaced)
{
    Ubiquitty::ClipboardReadItemStaging staging;
    CHECK(types(staging) == "<unstaged>");
    CHECK(bytes(staging, "text/plain") == "<unserved>");

    const std::vector<Ubiquitty::ClipboardItem> capture = {
        item("text/plain", "Hi"),
        item("image/png", "PNG"),
    };
    staging.stage(capture);
    CHECK(types(staging) == "text/plain image/png");
    CHECK(bytes(staging, "text/plain") == "Hi");
    CHECK(bytes(staging, "image/png") == "PNG");
    // The capture persists across serves: one approval answers the type
    // list and every item read of its completion.
    CHECK(bytes(staging, "image/png") == "PNG");
    CHECK(bytes(staging, "text/html") == "<unserved>");

    const std::vector<Ubiquitty::ClipboardItem> replacement = { item("text/html", "<p>") };
    staging.stage(replacement);
    CHECK(types(staging) == "text/html");
    CHECK(bytes(staging, "text/plain") == "<unserved>");
}

TEST(testStagingMatchesCaselesslyByBareType)
{
    Ubiquitty::ClipboardReadItemStaging staging;
    const std::vector<Ubiquitty::ClipboardItem> capture = { item("Image/PNG", "P") };
    staging.stage(capture);
    // Both sides fold, parameters ignored — the stored spelling rides the
    // list verbatim and the match reads through it.
    CHECK(types(staging) == "Image/PNG");
    CHECK(bytes(staging, "image/png") == "P");
    CHECK(bytes(staging, "IMAGE/png;whatever=1") == "P");
}

TEST(testStagingAnEmptySetClears)
{
    Ubiquitty::ClipboardReadItemStaging staging;
    const std::vector<Ubiquitty::ClipboardItem> capture = { item("text/plain", "soon gone") };
    staging.stage(capture);
    CHECK(types(staging) == "text/plain");
    staging.stage({});
    // Cleared, not empty-typed: -1 has Core fall back to the untyped bytes.
    CHECK(types(staging) == "<unstaged>");
    CHECK(bytes(staging, "text/plain") == "<unserved>");
}

TEST(testStagingTypeListNamesOnlyWhatFits)
{
    Ubiquitty::ClipboardReadItemStaging staging;
    const std::vector<Ubiquitty::ClipboardItem> capture = {
        item("text/plain", "a"),
        item("image/png", "b"),
    };
    staging.stage(capture);
    char tiny[12];
    const int n = staging.serveTypes(tiny);
    // A list longer than the span names what fits, never a torn name.
    CHECK(n == 10 && std::string_view(tiny, 10) == "text/plain");
}

TEST(testStagingBoundsAnItemByTheSpan)
{
    Ubiquitty::ClipboardReadItemStaging staging;
    const std::vector<Ubiquitty::ClipboardItem> capture = { item("text/plain", "0123456789") };
    staging.stage(capture);
    char four[4];
    CHECK(staging.serveItem("text/plain", four) == 4);
    CHECK(std::string_view(four, 4) == "0123");
}

UBIQUITTY_TEST_MAIN()
