// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Attributes/Hyperlinks.h"
#include "Core/Attributes/LinkRun.h"
#include "Core/CellGrid.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"
#include "Harness.h"

#include <cstdio>
#include <cstring>
#include <string>

// The OSC 8 URI registry. A cell holds a 16-bit id, not a URI, so ids are
// interned by content — a shell emitting the same link on every prompt must not
// grow the table.

// Interning is by content: the same pair returns the same id, so a shell emitting
// one link per prompt cannot grow the table without bound.
TEST(testHyperlinksInternDedups)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    const uint16_t a = Ubiquitty::Attributes::Hyperlinks::intern(&t, "", "https://example.com/x");
    const uint16_t b = Ubiquitty::Attributes::Hyperlinks::intern(&t, "", "https://example.com/x");
    const uint16_t c = Ubiquitty::Attributes::Hyperlinks::intern(&t, "", "https://example.com/y");
    CHECK(a != 0);
    CHECK(a == b);
    CHECK(c != a);

    CHECK(std::strcmp(Ubiquitty::Attributes::Hyperlinks::uri(&t, a), "https://example.com/x") == 0);
    CHECK(Ubiquitty::Attributes::Hyperlinks::uri(&t, 0) == nullptr);

    // An empty target closes the current link rather than interning one.
    CHECK(Ubiquitty::Attributes::Hyperlinks::intern(&t, "", "") == 0);
    CHECK(t.currentLinkID == 0);
}

// kindForLinkID — the same classification reached through the terminal's link
// ring, which is what a frontend rendering from a live Terminal has. macOS reads
// a captured copy of the kinds because its renderer runs off a snapshot; every
// other frontend holds the ring itself, and walking it in each of them would be
// a second copy of both the walk and the opinion about which scheme is which.
// An id never resolves differently for a cell that still exists.
//
// A cell carries a 16-bit id in place of a URI, and cells outlive the table: the
// ring holds 64 entries, the scrollback holds up to a million rows. So a cell
// drawn long ago still names an id that `uri()` resolves when the user clicks it,
// against whichever entry holds that number now.
//
// A counter that recycled at 0xFFFF blindly would break that. Interning is what
// OSC 8 does and it prints nothing, so a program can open 65535 links with
// distinct targets — no cells, no scrollback, nothing on screen — and then open
// one more. If that one took back an id a visible cell holds, the link text
// names one destination and the click goes to another.
//
// So reuse is gated on a census: an id returns to the mintable pool only when
// no cell on either screen, no scrollback line, no ring entry and no open link
// carries it. This drives ~200000 mints — three generations of the id space —
// past one cell the user is looking at, and checks per mint that the cell's id
// is never handed out again while the cell exists.
//
// Fails-without-it: a census that misses a population (a screen, the
// scrollback, the ring) reclaims the visible cell's id, a later mint returns
// it, and the CHECK inside the loop names the collision.
TEST(testALiveCellsLinkIDIsNeverReminted)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 6, 20);
    using namespace Ubiquitty::Attributes::Hyperlinks;

    // The link the user is looking at: opened, and PRINTED THROUGH, so a grid
    // cell carries the id. An id with no cell is legitimately reclaimable, and
    // asserting on one would pin the census to a stronger contract than any
    // consumer holds — clicks resolve through cells.
    const uint16_t visible = intern(&t, "", "https://example.com/the-one-they-see");
    CHECK(visible != 0);
    Ubiquitty::processBytes(&t, "X");
    CHECK_MSG(t.grid && t.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(0)) != nullptr
            && t.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(0))->linkID() == visible,
        "the fixture's cell carries the id the test asserts on");

    // Spend the sequential space and keep going through the census: distinct
    // targets, nothing further printed. Minting does not stop — the census
    // refills the pool from ids nothing references — so the bound is the loop,
    // and the property is per-mint.
    int minted = 0;
    for (int i = 0; i < 0x30000; ++i) {
        char scratch[64];
        std::snprintf(scratch, sizeof(scratch), "https://filler.example/%d", i);
        const uint16_t id = intern(&t, "", scratch);
        if (id != 0) {
            ++minted;
            CHECK_MSG(id != visible, "a live cell's id is never handed out again");
        }
    }
    // More mints than the 16-bit space proves the census fed the counter past
    // exhaustion — without that, the loop above asserts nothing new.
    printf("testALiveCellsLinkIDIsNeverReminted: minted %d across the space %d times\n",
        minted, minted / 0xFFFF);
    CHECK_MSG(minted > 0x10000, "reclaim kept minting past the sequential space");

    // The visible cell's ring entry was recycled long ago, so its id resolves
    // to nothing — dead, never elsewhere.
    const char* resolved = uri(&t, visible);
    CHECK_MSG(resolved == nullptr || std::strcmp(resolved, "https://example.com/the-one-they-see") == 0,
        "a resolvable id resolves to its own URI and no other");
}

// The rendering half of the same design: with the sequential space spent and
// nothing referencing the spent ids, the next link must get an id — a 0 here
// draws every link after the 65535th as plain text for the rest of the
// session, as a benchmark whose first tick mints the whole budget shows.
//
// Fails-without-it: intern returns 0 the moment linkNextID parks, and the
// witness link renders as ordinary text.
TEST(testLinksKeepMintingPastTheSequentialSpace)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 6, 20);
    using namespace Ubiquitty::Attributes::Hyperlinks;

    // Spend the sequential space without stamping a single cell.
    uint16_t last = 1;
    for (int i = 0; i < 0x10000 && last != 0; ++i) {
        char scratch[64];
        std::snprintf(scratch, sizeof(scratch), "https://spend.example/%d", i);
        last = intern(&t, "", scratch);
    }
    CHECK_MSG(t.linkNextID == 0, "the sequential counter is parked");

    // The witness: a fresh target past the budget still gets an id, the id
    // resolves to its own URI, and printing through it stamps the cell.
    const uint16_t witness = intern(&t, "", "file:///home/user/final");
    CHECK_MSG(witness != 0, "a link past the sequential space still mints");
    if (witness == 0)
        return; // the checks below dereference the resolution
    CHECK(t.currentLinkID == witness);
    CHECK(std::strcmp(uri(&t, witness), "file:///home/user/final") == 0);
    Ubiquitty::processBytes(&t, "L");
    CHECK(t.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(0))->linkID() == witness);
}

TEST(testKindForLinkIDReadsTheRing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    using Ubiquitty::Attributes::LinkRun::Kind;
    using Ubiquitty::Attributes::LinkRun::kindForLinkID;

    const uint16_t web = Ubiquitty::Attributes::Hyperlinks::intern(&t, "", "https://example.com");
    const uint16_t file = Ubiquitty::Attributes::Hyperlinks::intern(&t, "", "file:///etc/hosts");
    const uint16_t mail = Ubiquitty::Attributes::Hyperlinks::intern(&t, "", "mailto:a@b.c");

    CHECK(kindForLinkID(&t, web) == Kind::URL);
    CHECK(kindForLinkID(&t, file) == Kind::File);
    CHECK(kindForLinkID(&t, mail) == Kind::Mailto);

    // An id the ring does not hold is Other rather than a failure: a cell can
    // outlive its entry, and a caller styling the link cannot act on the
    // difference between that and a scheme it does not recognise.
    CHECK(kindForLinkID(&t, 0xFFFF) == Kind::Other);
    CHECK(kindForLinkID(nullptr, web) == Kind::Other);

    // Id 0 means "no link" and the ring cannot hold it: intern skips zero when it
    // mints (`mintID` in Hyperlinks.cpp), so the early return is unreachable for
    // a well-formed table and no injection into it can be observed here. Asserted
    // as the contract a caller reads rather than as covered behaviour — a cell
    // with no link asks this question on every frame.
    CHECK(kindForLinkID(&t, 0) == Kind::Other);
}

// The registry answers rather than dereferences when it is handed nothing. Both
// entry points are reached from OSC 8 parsing and from a frontend holding a
// Terminal it did not create.
TEST(testNothingToInternAndNothingToLookUp)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    CHECK(Ubiquitty::Attributes::Hyperlinks::uri(nullptr, 1) == nullptr);
    CHECK(Ubiquitty::Attributes::Hyperlinks::intern(nullptr, "k", "https://example.com") == 0);

    // An empty URI closes the current link: OSC 8 with no target is the close
    // sequence. There is no second spelling of that state to agree with — a view
    // deletes its nullptr_t conversion, so "absent" reaches intern one way.
    const uint16_t open = Ubiquitty::Attributes::Hyperlinks::intern(&t, "", "https://example.com/open");
    CHECK(open != 0);
    CHECK(t.currentLinkID == open);
    CHECK(Ubiquitty::Attributes::Hyperlinks::intern(&t, "", "") == 0);
    CHECK(t.currentLinkID == 0);

    // An absent key is the no-key case, not a key of its own: it interns by URI
    // and dedups against the link opened above.
    CHECK(Ubiquitty::Attributes::Hyperlinks::intern(&t, {}, "https://example.com/open") == open);
}

// The ring compares hashes to skip work and strings to decide. A case that only
// ever feeds distinct targets cannot tell those two apart, because every hash
// miss is also a string miss — so these feed a hash hit that must not become a
// match.
TEST(testAHashHitIsNotAMatch)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Two distinct keys with the same 32-bit hash, found by search rather than
    // by hope. If the hash ever changes they stop colliding and this case stops
    // covering the arm it exists for, so it asserts the collision it needs.
    const char* const first = "kdwsjz";
    const char* const second = "khihpz";

    const uint16_t keyed = Ubiquitty::Attributes::Hyperlinks::intern(&t, first, "https://example.com/a");
    // Something else in between, so the one-slot cache does not answer before
    // the ring is reached.
    Ubiquitty::Attributes::Hyperlinks::intern(&t, "unrelated", "https://example.com/z");
    const uint16_t collided = Ubiquitty::Attributes::Hyperlinks::intern(&t, second, "https://example.com/b");

    CHECK(keyed != 0);
    CHECK(collided != 0);
    CHECK_MSG(collided != keyed, "two keys that hash alike are still two links");
    CHECK(std::strcmp(Ubiquitty::Attributes::Hyperlinks::uri(&t, keyed), "https://example.com/a") == 0);
    CHECK(std::strcmp(Ubiquitty::Attributes::Hyperlinks::uri(&t, collided), "https://example.com/b") == 0);

    // A keyed target and an unkeyed one hash the same when the key equals the
    // other's URI, and the ring must not let a keyed open adopt an unkeyed
    // link's id: they dedup by different fields.
    Ubiquitty::Terminal u;
    Ubiquitty::hardReset(u);
    const uint16_t byURI = Ubiquitty::Attributes::Hyperlinks::intern(&u, "", "shared-target");
    const uint16_t byKey = Ubiquitty::Attributes::Hyperlinks::intern(&u, "shared-target", "https://example.com/c");
    CHECK(byURI != 0);
    CHECK(byKey != 0);
    CHECK_MSG(byKey != byURI, "a keyed open does not adopt an unkeyed link's id");
}

// An over-long key is truncated into the ring, and a truncated copy never
// compares equal to the whole target — so each open mints a fresh id rather than
// every long key collapsing onto whichever one arrived first.
TEST(testAKeyTooLongForTheRingNeverMatchesItself)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    std::string longKey(static_cast<std::size_t>(Ubiquitty::VT::maxLinkKeyLength) + 8, 'k');
    const uint16_t first = Ubiquitty::Attributes::Hyperlinks::intern(&t, longKey.c_str(), "https://example.com/1");
    Ubiquitty::Attributes::Hyperlinks::intern(&t, "unrelated", "https://example.com/z");
    const uint16_t second = Ubiquitty::Attributes::Hyperlinks::intern(&t, longKey.c_str(), "https://example.com/2");

    CHECK(first != 0);
    CHECK(second != 0);
    CHECK_MSG(first != second, "an over-long key cannot match its own truncated copy");

    // Which is what makes the fit test on the scan-hit path unnecessary: a hit
    // implies the whole target compared equal, so it fit. A key one shorter than
    // the field does fit, does match, and does dedup — the control that separates
    // "too long" from "keys never dedup at all".
    std::string fitting(static_cast<std::size_t>(Ubiquitty::VT::maxLinkKeyLength) - 1, 'f');
    const uint16_t fits = Ubiquitty::Attributes::Hyperlinks::intern(&t, fitting.c_str(), "https://example.com/3");
    Ubiquitty::Attributes::Hyperlinks::intern(&t, "unrelated-too", "https://example.com/z");
    CHECK_MSG(Ubiquitty::Attributes::Hyperlinks::intern(&t, fitting.c_str(), "https://example.com/4") == fits,
        "a key that fits the ring dedups against itself");
}

// The unkeyed half of the same scan. A re-open of the same URI normally answers
// from the one-slot cache and never reaches the ring at all, so every case here
// interns something else in between — without that the ring's own comparison is
// unexercised and the cache is all that is being measured.
TEST(testTheUnkeyedScanComparesURIsRatherThanHashes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    using namespace Ubiquitty::Attributes::Hyperlinks;

    const uint16_t plain = intern(&t, "", "https://example.com/x");
    intern(&t, "", "https://example.com/between");
    CHECK_MSG(intern(&t, "", "https://example.com/x") == plain, "the ring dedups a URI the cache has forgotten");

    // A keyed slot is not an answer for an unkeyed open even when the two hash
    // alike, because they dedup by different fields: one by its id= key, the
    // other by its URI.
    const uint16_t keyed = intern(&t, "shared", "https://example.com/keyed");
    intern(&t, "", "https://example.com/between2");
    const uint16_t unkeyed = intern(&t, "", "shared");
    CHECK(keyed != 0);
    CHECK(unkeyed != 0);
    CHECK_MSG(unkeyed != keyed, "an unkeyed open does not adopt a keyed link's id");

    // And two distinct URIs that hash alike stay two links, which is what makes
    // the hash a filter rather than the decision.
    const uint16_t first = intern(&t, "", "kdwsjz");
    intern(&t, "", "https://example.com/between3");
    const uint16_t second = intern(&t, "", "khihpz");
    CHECK_MSG(first != second, "two URIs that hash alike are still two links");
    CHECK(std::strcmp(uri(&t, first), "kdwsjz") == 0);
    CHECK(std::strcmp(uri(&t, second), "khihpz") == 0);
}

// A URI too long for its ring field is truncated on the way in, so it cannot
// match itself later — the same rule the keys follow, and the reason the fit is
// tested where a link is inserted rather than where one is found.
TEST(testAURITooLongForTheRingIsTruncatedAndNeverMatchesItself)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    using namespace Ubiquitty::Attributes::Hyperlinks;

    const std::string longURI = "https://example.com/"
        + std::string(static_cast<std::size_t>(Ubiquitty::VT::maxLinkURILength), 'u');
    const uint16_t first = intern(&t, "", longURI.c_str());
    intern(&t, "", "https://example.com/between");
    const uint16_t second = intern(&t, "", longURI.c_str());

    CHECK(first != 0);
    CHECK(second != 0);
    CHECK_MSG(first != second, "an over-long URI cannot match its own truncated copy");

    // Truncated rather than overrun: what comes back is a prefix of what went
    // in, and it ends where the field does.
    const char* stored = uri(&t, first);
    CHECK(stored != nullptr);
    CHECK(std::strlen(stored) == static_cast<std::size_t>(Ubiquitty::VT::maxLinkURILength) - 1);
    CHECK(std::strncmp(stored, longURI.c_str(), std::strlen(stored)) == 0);
}

// The ring's 132 KB is not part of a Terminal that has no links.
TEST(testHyperlinksTableIsAbsentUntilInterned)
{
    using namespace Ubiquitty::Attributes::Hyperlinks;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    CHECK_MSG(table(&t) == nullptr, "a terminal that has seen no OSC 8 carries no ring");
    CHECK(t.linkCount == 0);

    CHECK(intern(&t, "", "") == 0);
    CHECK_MSG(table(&t) == nullptr, "an empty target allocates nothing");

    CHECK(uri(&t, 1) == nullptr);
    CHECK(Ubiquitty::Attributes::LinkRun::kindForLinkID(&t, 1) == Ubiquitty::Attributes::LinkRun::Kind::Other);

    const uint16_t id = intern(&t, "", "https://example.com/first");
    CHECK(id != 0);
    CHECK_MSG(table(&t) != nullptr, "the first interned link allocates the ring");
    CHECK(std::strcmp(uri(&t, id), "https://example.com/first") == 0);
}

// An id `intern` returns always resolves: the cache cannot outlive the ring.
TEST(testHyperlinksInternCacheCannotOutliveTheRing)
{
    using namespace Ubiquitty::Attributes::Hyperlinks;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    intern(&t, "", "https://example.com/a");
    const uint16_t b = intern(&t, "", "https://example.com/b");
    CHECK(b != 0);
    CHECK(table(&t) != nullptr);

    reset(t);
    CHECK_MSG(table(&t) == nullptr, "reset releases the ring");
    CHECK(t.linkCount == 0);
    CHECK(uri(&t, b) == nullptr);

    const uint16_t again = intern(&t, "", "https://example.com/b");
    CHECK(again != 0);
    const char* resolved = uri(&t, again);
    CHECK_MSG(resolved != nullptr, "an id intern returned resolves through the ring");
    CHECK(std::strcmp(resolved, "https://example.com/b") == 0);
}

// Targets that agree for a long run and then differ. A comparison that widens
// past one byte a step has to get the tail exactly right, and every way of
// being unequal is a separate way to get it wrong: differing at the end, being
// a strict prefix of what is stored, extending past it, and sitting on the
// field's own boundary.
//
// The shape is the common one rather than a contrived one — links off one host
// share their scheme, host and directory, so the bytes that tell two apart are
// the last few.
TEST(testLinksSharingALongPrefixAreStillToldApart)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    using Ubiquitty::Attributes::Hyperlinks::intern;
    const std::string stem = "file:///home/user/project/build/output/item_";

    // Differ only in the final byte, after 44 identical ones.
    const uint16_t a = intern(&t, "", stem + "0001");
    const uint16_t b = intern(&t, "", stem + "0002");
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a != b);
    CHECK(std::strcmp(Ubiquitty::Attributes::Hyperlinks::uri(&t, a), (stem + "0001").c_str()) == 0);
    CHECK(std::strcmp(Ubiquitty::Attributes::Hyperlinks::uri(&t, b), (stem + "0002").c_str()) == 0);

    // Re-opening either still dedups, so widening the compare did not break the
    // equality it exists for.
    CHECK(intern(&t, "", stem + "0001") == a);
    CHECK(intern(&t, "", stem + "0002") == b);

    // A strict prefix of a stored target is a different target, because the
    // stored side continues where the measured one stops.
    const uint16_t shorter = intern(&t, "", stem);
    CHECK(shorter != a);
    CHECK(shorter != b);

    // ...and extending a stored target is not it either.
    const uint16_t longer = intern(&t, "", stem + "0001x");
    CHECK(longer != a);
    CHECK(intern(&t, "", stem + "0001") == a);

    // The longest target the URI field can hold, and one byte more. The first
    // round-trips; the second is stored clipped and, by that same rule, never
    // matches itself.
    const std::string longest(static_cast<std::size_t>(Ubiquitty::VT::maxLinkURILength) - 1, 'u');
    const uint16_t fits = intern(&t, "", longest);
    CHECK(fits != 0);
    CHECK(intern(&t, "", longest) == fits);

    const std::string overlong(static_cast<std::size_t>(Ubiquitty::VT::maxLinkURILength), 'u');
    CHECK(intern(&t, "", overlong) != intern(&t, "", overlong));
}

UBIQUITTY_TEST_MAIN()
