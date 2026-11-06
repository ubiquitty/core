// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/AttachmentLink.h"
#include "Harness.h"
#include "Helpers/TempDir.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Session/SealedResumeFile.h"
#include "PAL/BoundedFile.h"
#include "PAL/DurableFile.h"
#include "PAL/PAL.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

using namespace Ubiquitty::PAL;
using namespace Ubiquitty::Session;
using namespace Ubiquitty::Protocol::Sync;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

constexpr std::size_t budget = 4096;

// A reversible test envelope with a scope/content checksum, not cryptography.
// It exercises real disk/codec/ownership and refuses damage deterministically.
// test_resume_sealer separately exercises actual ChaChaPoly with owned keys.
struct FileSealer {
    bool hasKey = true;
    bool shouldRefuse = false;
    unsigned sealCalls = 0;
    unsigned openCalls = 0;
    unsigned forgetCalls = 0;
    std::uint64_t keyGeneration = 1;

    static FileSealer& current()
    {
        static FileSealer state;
        return state;
    }

    static std::uint64_t checksum(std::span<const std::uint8_t> scope, std::span<const std::uint8_t> bytes)
    {
        std::uint64_t value = current().keyGeneration;
        for (const auto part : { scope, bytes }) {
            for (const auto byte : part)
                value = (value ^ byte) * 1099511628211ULL;
        }
        return value;
    }

    static std::ptrdiff_t seal(const std::uint8_t* scope, std::size_t scopeSize, const std::uint8_t* input, std::size_t inputSize, std::uint8_t* output, std::size_t capacity)
    {
        auto& state = current();
        ++state.sealCalls;
        if (state.shouldRefuse || capacity != inputSize + SealedStore::overhead)
            return -1;
        if (!state.hasKey) {
            state.hasKey = true;
            ++state.keyGeneration;
        }
        std::fill_n(output, capacity, 0);
        output[0] = 0xa1;
        for (std::size_t index = 0; index < inputSize; ++index)
            output[13 + index] = input[index] ^ 0x5a;
        const auto tag = checksum({ scope, scopeSize }, { output, capacity - 16 });
        for (std::size_t index = 0; index < 16; ++index)
            output[capacity - 16 + index] = static_cast<std::uint8_t>(tag >> ((index % 8) * 8));
        return static_cast<std::ptrdiff_t>(capacity);
    }

    static std::ptrdiff_t open(const std::uint8_t* scope, std::size_t scopeSize, const std::uint8_t* input, std::size_t inputSize, std::uint8_t* output, std::size_t capacity)
    {
        auto& state = current();
        ++state.openCalls;
        if (state.shouldRefuse || !state.hasKey || inputSize != capacity + SealedStore::overhead || input[0] != 0xa1)
            return -1;
        const auto tag = checksum({ scope, scopeSize }, { input, inputSize - 16 });
        for (std::size_t index = 0; index < 16; ++index) {
            if (input[inputSize - 16 + index] != static_cast<std::uint8_t>(tag >> ((index % 8) * 8)))
                return -1;
        }
        for (std::size_t index = 0; index < capacity; ++index)
            output[index] = input[13 + index] ^ 0x5a;
        return static_cast<std::ptrdiff_t>(capacity);
    }

    static bool forget(const std::uint8_t*, std::size_t)
    {
        ++current().forgetCalls;
        if (current().shouldRefuse)
            return false;
        current().hasKey = false;
        return true;
    }
};

// Protected authorization survives reconstruction of the adapter, independently
// of its encryption key. Failures here do not touch the real user keychain.
struct TestAuthorization {
    RestoreAuthorization::Record record { RestoreAuthorization::State::Missing, {} };
    bool isAvailable = true;
    bool shouldRefuseBlock = false;
    bool shouldRefuseActivation = false;
    unsigned blocks = 0;
    unsigned activations = 0;
    static TestAuthorization& current()
    {
        static TestAuthorization state;
        return state;
    }
    static std::uint8_t read(const std::uint8_t*, std::size_t, std::uint8_t* digest, std::size_t size)
    {
        const auto& state = current();
        if (!state.isAvailable || size != state.record.digest.size())
            return 0;
        std::ranges::copy(state.record.digest, digest);
        return static_cast<std::uint8_t>(state.record.state);
    }
    static bool write(const std::uint8_t*, std::size_t, std::uint8_t active, const std::uint8_t* digest, std::size_t size)
    {
        auto& state = current();
        if (!state.isAvailable || size != state.record.digest.size() || (active == 3 ? state.shouldRefuseActivation : state.shouldRefuseBlock))
            return false;
        if (active == 3)
            ++state.activations;
        else
            ++state.blocks;
        state.record.state = static_cast<RestoreAuthorization::State>(active);
        std::copy_n(digest, size, state.record.digest.begin());
        return true;
    }
};

RestoreAuthorization authorization()
{
    return RestoreAuthorization({ .read = TestAuthorization::read, .write = TestAuthorization::write });
}

struct HandoverAuthorization {
    std::map<std::vector<std::uint8_t>, RestoreAuthorization::Record> records;
    unsigned writeCalls = 0;
    unsigned refusedWrite = 0;
    bool shouldRefuseActivation = false;

    static HandoverAuthorization& current()
    {
        static HandoverAuthorization state;
        return state;
    }

    static std::uint8_t read(const std::uint8_t* scope, std::size_t scopeSize, std::uint8_t* digest, std::size_t size)
    {
        if (size != 32)
            return 0;
        const auto& records = current().records;
        const auto found = records.find({ scope, scope + scopeSize });
        if (found == records.end())
            return static_cast<std::uint8_t>(RestoreAuthorization::State::Missing);
        std::ranges::copy(found->second.digest, digest);
        return static_cast<std::uint8_t>(found->second.state);
    }

    static bool write(const std::uint8_t* scope, std::size_t scopeSize, std::uint8_t active, const std::uint8_t* digest, std::size_t size)
    {
        auto& state = current();
        if (++state.writeCalls == state.refusedWrite || size != 32
            || (active == static_cast<std::uint8_t>(RestoreAuthorization::State::Active) && state.shouldRefuseActivation))
            return false;
        auto& record = state.records[{ scope, scope + scopeSize }];
        record.state = static_cast<RestoreAuthorization::State>(active);
        std::copy_n(digest, size, record.digest.begin());
        return true;
    }
};

RestoreAuthorization handoverAuthorization()
{
    HandoverAuthorization::current() = {};
    return RestoreAuthorization({ .read = HandoverAuthorization::read, .write = HandoverAuthorization::write });
}

ResumeRecord record()
{
    return { .hostInstanceID = { 1, 2 }, .clientInstance = { 3, 4 }, .attachmentID = { 5, 6 }, .resumeSecret = { 7, 8 }, .certificatePin = {}, .panes = { { .paneID = { 9, 10 }, .lastInputSeq = 37, .view = ViewState::Dormant, .checkpoint = {}, .checkpointDigest = {}, .replica = {} } }, .drafts = { { .paneID = { 9, 10 }, .intent = RawInput { .bytes = { 'd', 'r', 'a', 'f', 't' } } } } };
}

SealScope scope()
{
    return resumeSealScope({ 1, 2 }, { 11 }, { 3, 4 });
}

std::array<std::uint8_t, 32> principalDigest(std::string_view principal)
{
    std::array<std::uint8_t, 32> result {};
    Ubiquitty::Crypto::sha256({}, { reinterpret_cast<const std::uint8_t*>(principal.data()), principal.size() }, result);
    return result;
}

SealedStore sealer()
{
    FileSealer::current() = {};
    TestAuthorization::current() = {};
    return SealedStore({ .seal = FileSealer::seal, .open = FileSealer::open, .forget = FileSealer::forget });
}

} // namespace

TEST(sealedStoreBindsOnlyItsExactAuthenticatedWelcome)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto driver = sealer();
    constexpr ID128 host { 41, 42 };
    constexpr ID128 client { 7, 8 };
    std::array<std::uint8_t, 32> pin {};
    pin[0] = 19;
    SealedResumeRecordStore store(directory.path() + "/resume", resumeSealScope(host, principalDigest("alice"), client), pin, driver, authorization(), budget, budget);
    Welcome welcome;
    welcome.hostInstanceID = host;
    welcome.principal = "alice";
    CHECK(store.bindWelcome(welcome, client, pin) == ResumeBindingOutcome::Bound);
    auto changed = welcome;
    changed.hostInstanceID.low++;
    CHECK(store.bindWelcome(changed, client, pin) == ResumeBindingOutcome::Refused);
    changed = welcome;
    changed.principal = "bob";
    CHECK(store.bindWelcome(changed, client, pin) == ResumeBindingOutcome::Refused);
    changed.principal.clear();
    CHECK(store.bindWelcome(changed, client, pin) == ResumeBindingOutcome::Refused);
    CHECK(store.bindWelcome(welcome, { 9, 8 }, pin) == ResumeBindingOutcome::Refused);
    pin[31] ^= 1;
    CHECK(store.bindWelcome(welcome, client, pin) == ResumeBindingOutcome::Refused);
    CHECK_EQ(FileSealer::current().sealCalls, 0u);
    CHECK_EQ(FileSealer::current().openCalls, 0u);
}

TEST(sealedStoreRetiresThePredecessorBeforeCommittingTheSuccessorsSecret)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    const auto authority = handoverAuthorization();
    const auto original = record();
    constexpr ID128 successor { 20, 21 };
    const auto predecessorScope = resumeSealScope(original.hostInstanceID, principalDigest("alice"), original.clientInstance);
    const auto successorScope = resumeSealScope(successor, principalDigest("alice"), original.clientInstance);
    SealedResumeRecordStore store(path, predecessorScope, {}, driver, authority, budget, budget);
    REQUIRE(store.write(original) == ResumeWriteOutcome::Committed);
    const auto copied = storageReadBounded(path.c_str(), budget + SealedStore::overhead);
    REQUIRE(copied);
    Welcome welcome;
    welcome.hostInstanceID = successor;
    welcome.predecessorHostInstanceID = original.hostInstanceID;
    welcome.principal = "alice";
    REQUIRE(store.bindWelcome(welcome, original.clientInstance, {}) == ResumeBindingOutcome::Bound);
    CHECK(authority.read(predecessorScope).state == RestoreAuthorization::State::Blocked);
    CHECK(authority.read(successorScope).state == RestoreAuthorization::State::Blocked);
    CHECK(!store.read());
    CHECK(store.write(original) == ResumeWriteOutcome::Unchanged);
    auto next = original;
    next.hostInstanceID = successor;
    ++next.resumeSecret.low;
    REQUIRE(store.write(next) == ResumeWriteOutcome::Committed);
    CHECK(authority.read(predecessorScope).state == RestoreAuthorization::State::Blocked);
    CHECK(authority.read(successorScope).state == RestoreAuthorization::State::Active);
    SealedResumeRecordStore oldOwner(path, predecessorScope, {}, driver, authority, budget, budget);
    SealedResumeRecordStore newOwner(path, successorScope, {}, driver, authority, budget, budget);
    CHECK(!oldOwner.read());
    REQUIRE(newOwner.read());
    CHECK(newOwner.read()->resumeSecret == next.resumeSecret);
    REQUIRE(storageWriteDurable(path.c_str(), std::as_bytes(copied->view())) == DurableFileResult::Durable);
    CHECK(!oldOwner.read());
    CHECK(!newOwner.read());
}

TEST(aHandoverNeverChangesTheResumePrincipalClientOrCertificate)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    const auto authority = handoverAuthorization();
    const auto original = record();
    const auto identity = resumeSealScope(original.hostInstanceID, principalDigest("alice"), original.clientInstance);
    SealedResumeRecordStore store(path, identity, {}, driver, authority, budget, budget);
    REQUIRE(store.write(original) == ResumeWriteOutcome::Committed);
    const auto writes = HandoverAuthorization::current().writeCalls;
    const auto forgets = FileSealer::current().forgetCalls;
    for (unsigned fault = 0; fault < 7; ++fault) {
        Welcome welcome;
        welcome.hostInstanceID = { 20, 21 };
        welcome.predecessorHostInstanceID = original.hostInstanceID;
        welcome.principal = "alice";
        auto client = original.clientInstance;
        std::array<std::uint8_t, 32> pin {};
        switch (fault) {
        case 0:
            welcome.predecessorHostInstanceID.reset();
            break;
        case 1:
            welcome.predecessorHostInstanceID = ID128 { 98, 99 };
            break;
        case 2:
            welcome.predecessorHostInstanceID = ID128 {};
            break;
        case 3:
            welcome.hostInstanceID = {};
            break;
        case 4:
            welcome.principal = "bob";
            break;
        case 5:
            ++client.low;
            break;
        case 6:
            pin[0] = 1;
            break;
        }
        CHECK(store.bindWelcome(welcome, client, pin) == ResumeBindingOutcome::Refused);
        CHECK_EQ(HandoverAuthorization::current().writeCalls, writes);
        CHECK_EQ(FileSealer::current().forgetCalls, forgets);
        REQUIRE(store.read());
        CHECK(store.read()->resumeSecret == original.resumeSecret);
    }
}

TEST(handoverIntentRefusalCannotRetireThePredecessor)
{
    for (unsigned failedIntent : { 1u, 2u }) {
        Ubiquitty::Test::TempDir directory;
        REQUIRE(directory.valid());
        const auto path = directory.path() + "/resume";
        const auto driver = sealer();
        const auto authority = handoverAuthorization();
        const auto original = record();
        const auto identity = resumeSealScope(original.hostInstanceID, principalDigest("alice"), original.clientInstance);
        SealedResumeRecordStore store(path, identity, {}, driver, authority, budget, budget);
        REQUIRE(store.write(original) == ResumeWriteOutcome::Committed);
        const auto before = storageReadBounded(path.c_str(), budget + SealedStore::overhead);
        REQUIRE(before);
        HandoverAuthorization::current().refusedWrite = HandoverAuthorization::current().writeCalls + failedIntent;
        Welcome welcome;
        welcome.hostInstanceID = { 20, 21 };
        welcome.predecessorHostInstanceID = original.hostInstanceID;
        welcome.principal = "alice";
        const auto forgets = FileSealer::current().forgetCalls;
        CHECK(store.bindWelcome(welcome, original.clientInstance, {}) == ResumeBindingOutcome::Refused);
        CHECK_EQ(FileSealer::current().forgetCalls, forgets);
        CHECK(authority.read(identity).state == RestoreAuthorization::State::Active);
        const auto after = storageReadBounded(path.c_str(), budget + SealedStore::overhead);
        REQUIRE(after);
        CHECK(std::ranges::equal(before->view(), after->view()));
        SealedResumeRecordStore restarted(path, identity, {}, driver, authority, budget, budget);
        REQUIRE(restarted.read());
        CHECK(restarted.read()->resumeSecret == original.resumeSecret);
    }
}

TEST(handoverKeyRetirementRefusalStaysBlockedAcrossRestart)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    const auto authority = handoverAuthorization();
    const auto original = record();
    const auto identity = resumeSealScope(original.hostInstanceID, principalDigest("alice"), original.clientInstance);
    const auto successor = resumeSealScope({ 20, 21 }, principalDigest("alice"), original.clientInstance);
    SealedResumeRecordStore store(path, identity, {}, driver, authority, budget, budget);
    REQUIRE(store.write(original) == ResumeWriteOutcome::Committed);
    const auto copied = storageReadBounded(path.c_str(), budget + SealedStore::overhead);
    REQUIRE(copied);
    Welcome welcome;
    welcome.hostInstanceID = { 20, 21 };
    welcome.predecessorHostInstanceID = original.hostInstanceID;
    welcome.principal = "alice";
    FileSealer::current().shouldRefuse = true;
    CHECK(store.bindWelcome(welcome, original.clientInstance, {}) == ResumeBindingOutcome::Refused);
    CHECK(authority.read(identity).state == RestoreAuthorization::State::Blocked);
    CHECK(authority.read(successor).state == RestoreAuthorization::State::Blocked);
    FileSealer::current().shouldRefuse = false;
    REQUIRE(storageWriteDurable(path.c_str(), std::as_bytes(copied->view())) == DurableFileResult::Durable);
    SealedResumeRecordStore oldOwner(path, identity, {}, driver, authority, budget, budget);
    SealedResumeRecordStore newOwner(path, successor, {}, driver, authority, budget, budget);
    const auto opens = FileSealer::current().openCalls;
    CHECK(!oldOwner.read());
    CHECK(!newOwner.read());
    CHECK(!store.read());
    CHECK_EQ(FileSealer::current().openCalls, opens);
    REQUIRE(store.bindWelcome(welcome, original.clientInstance, {}) == ResumeBindingOutcome::Bound);
    auto next = original;
    next.hostInstanceID = welcome.hostInstanceID;
    ++next.resumeSecret.low;
    REQUIRE(store.write(next) == ResumeWriteOutcome::Committed);
    REQUIRE(newOwner.read());
    CHECK(newOwner.read()->resumeSecret == next.resumeSecret);
}

TEST(aSuccessorWelcomeHoldsInputUntilItsProtectedRecordActivates)
{
    using namespace UbiquittyTest::AttachmentLink;
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    const auto authority = handoverAuthorization();
    constexpr std::size_t recordBudget = 1 << 20;
    auto original = record();
    original.clientInstance = clientInstance;
    constexpr ID128 successor { 41, 42 };
    const auto oldScope = resumeSealScope(original.hostInstanceID, principalDigest("alice"), clientInstance);
    const auto newScope = resumeSealScope(successor, principalDigest("alice"), clientInstance);
    SealedResumeRecordStore store(path, oldScope, {}, driver, authority, recordBudget, recordBudget);
    REQUIRE(store.write(original) == ResumeWriteOutcome::Committed);
    const auto copied = storageReadBounded(path.c_str(), recordBudget + SealedStore::overhead);
    REQUIRE(copied);
    Host host(successor);
    host.connection = std::make_unique<HostConnection>(UbiquittyTest::syncHostPolicy(0), HostIdentity { .predecessorHostInstanceID = original.hostInstanceID, .selection = UbiquittyTest::syncDefaultSelection() });
    host.connection->serveFromTable(host.table, owner, ++host.connectionNumber, [&host](ID128 id) { return id == paneID ? host.pane.source.get() : nullptr; });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    HandoverAuthorization::current().shouldRefuseActivation = true;
    (void)client.connect(now);
    pump(host, client, now);
    pump(host, client, now);
    REQUIRE(host.connection->bindPane(*host.pane.source));
    pumpFor(host, client, now, 100'000);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(client.connection()->welcome());
    CHECK(client.connection()->welcome()->predecessorHostInstanceID == original.hostInstanceID);
    REQUIRE(client.send(paneID, raw("x"), now));
    pumpFor(host, client, now, 50'000);
    CHECK(host.pane.writer.bytes().empty());
    CHECK(authority.read(oldScope).state == RestoreAuthorization::State::Blocked);
    CHECK(authority.read(newScope).state == RestoreAuthorization::State::Blocked);
    SealedResumeRecordStore oldOwner(path, oldScope, {}, driver, authority, recordBudget, recordBudget);
    SealedResumeRecordStore newOwner(path, newScope, {}, driver, authority, recordBudget, recordBudget);
    const auto opens = FileSealer::current().openCalls;
    CHECK(!store.read());
    CHECK(!oldOwner.read());
    CHECK(!newOwner.read());
    REQUIRE(storageWriteDurable(path.c_str(), std::as_bytes(copied->view())) == DurableFileResult::Durable);
    CHECK(!oldOwner.read());
    CHECK(!newOwner.read());
    CHECK_EQ(FileSealer::current().openCalls, opens);
    HandoverAuthorization::current().shouldRefuseActivation = false;
    client.persist();
    REQUIRE(newOwner.read());
    REQUIRE(client.held());
    CHECK(newOwner.read()->hostInstanceID == successor);
    CHECK(newOwner.read()->resumeSecret == client.held()->resumeSecret);
    CHECK(!oldOwner.read());
    pumpFor(host, client, now, 50'000);
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("x"));
}

TEST(sealedStoreRestoresTheExactFrontendBeforeResumingItsRealAttachment)
{
    using namespace UbiquittyTest::AttachmentLink;
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    constexpr ID128 instance { 41, 42 };
    const auto identity = resumeSealScope(instance, principalDigest("alice"), clientInstance);
    constexpr std::size_t recordBudget = 1 << 20;
    Host host(instance);
    std::uint64_t now = 1;
    {
        SealedResumeRecordStore store(path, identity, {}, driver, authorization(), recordBudget, recordBudget);
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        UbiquittyTest::feed(host.pane.terminal, "\x1b[32mcold picture\x1b[0m");
        host.pane.source->publish(FrameClass::Settled, now);
        pumpFor(host, first, now, 100'000);
        first.persist();
        REQUIRE(store.read());
        host.lose(now);
    }
    struct Frontend final : ClientPaneDelegate {
        Ubiquitty::Terminal terminal;
        Ubiquitty::Terminal* terminalForPane(ID128 id) override { return id == paneID ? &terminal : nullptr; }
    } frontend;
    // A new adapter has no in-memory record or attachment owner from the first.
    SealedResumeRecordStore store(path, identity, {}, driver, authorization(), recordBudget, recordBudget);
    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &frontend, &store);
    REQUIRE(second.restoreFromStore());
    CHECK(second.connection() == nullptr);
    CHECK(second.paneTerminal(paneID) == &frontend.terminal);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, frontend.terminal), "");
    const auto before = store.read();
    REQUIRE(before);
    reconnect(host, second, now);
    REQUIRE(second.connection()->welcome());
    REQUIRE(second.connection()->welcome()->resumed.size() == 1);
    CHECK(second.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK(second.connection()->counters().checkpointsApplied == 0);
    const auto after = store.read();
    REQUIRE(after);
    CHECK(after->resumeSecret != before->resumeSecret);
    REQUIRE(second.send(paneID, raw("x"), now));
    pumpFor(host, second, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "x");
}

TEST(encryptedHistoryRestoresBeforeNetworkAndClearRetiresCopiedCiphertext)
{
    using namespace UbiquittyTest::AttachmentLink;
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    constexpr ID128 instance { 41, 42 };
    const auto identity = resumeSealScope(instance, principalDigest("alice"), clientInstance);
    constexpr std::size_t recordBudget = 1 << 20;
    struct OptedInStore final : ResumeRecordStore {
        SealedResumeRecordStore& store;
        explicit OptedInStore(SealedResumeRecordStore& value)
            : store(value)
        {
        }
        HistoryPersistence historyPersistence() const override { return HistoryPersistence::Enabled; }
        ResumeBindingOutcome bindWelcome(const Welcome& welcome, ID128 client, const std::array<std::uint8_t, 32>& pin) override { return store.bindWelcome(welcome, client, pin); }
        ResumeWriteOutcome write(const ResumeRecord& value) override { return store.write(value); }
        std::optional<ResumeRecord> read() override { return store.read(); }
        ResumePurgeOutcome purge() override { return store.purge(); }
    };
    Host host(instance);
    std::uint64_t now = 1;
    int historyRows = 0;
    {
        SealedResumeRecordStore sealed(path, identity, {}, driver, authorization(), recordBudget, recordBudget);
        OptedInStore store(sealed);
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        for (int line = 0; line < 80; ++line)
            UbiquittyTest::feed(host.pane.terminal, "old encrypted history " + std::to_string(line) + "\r\n");
        host.pane.source->publish(FrameClass::Settled, now);
        pumpFor(host, first, now, 200'000);
        historyRows = Ubiquitty::Screen::scrollbackCount(first.paneTerminal(paneID));
        REQUIRE(historyRows > 0);
        drop(host, first, now);
    }
    const auto copied = storageReadBounded(path.c_str(), recordBudget + SealedStore::overhead);
    REQUIRE(copied);
    SealedResumeRecordStore sealed(path, identity, {}, driver, authorization(), recordBudget, recordBudget);
    OptedInStore store(sealed);
    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    REQUIRE(second.restoreFromStore());
    CHECK(second.connection() == nullptr);
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(second.paneTerminal(paneID)), historyRows);
    reconnect(host, second, now);
    REQUIRE(second.connection()->welcome());
    CHECK(second.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    const auto oldKey = FileSealer::current().keyGeneration;
    UbiquittyTest::feed(host.pane.terminal, "\x1b"
                                            "c");
    host.pane.source->publish(FrameClass::Settled, now);
    pumpFor(host, second, now, 100'000);
    second.persist();
    REQUIRE(store.read());
    CHECK_EQ(Ubiquitty::Screen::scrollbackCount(second.paneTerminal(paneID)), 0);
    CHECK(FileSealer::current().keyGeneration > oldKey);
    REQUIRE(storageWriteDurable(path.c_str(), std::as_bytes(copied->view())) == DurableFileResult::Durable);
    SealedResumeRecordStore restarted(path, identity, {}, driver, authorization(), recordBudget, recordBudget);
    CHECK(!restarted.read());
}

TEST(sealedStoreRejectsDifferentPinsAndLatchesFailedKeyRetirement)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    SealedResumeRecordStore store(path, scope(), {}, driver, authorization(), budget, budget);
    const auto original = record();
    REQUIRE(store.write(original) == ResumeWriteOutcome::Committed);
    const auto cipher = storageReadBounded(path.c_str(), budget + SealedStore::overhead);
    REQUIRE(cipher);
    auto changed = original;
    changed.certificatePin[0] = 1;
    CHECK(store.write(changed) == ResumeWriteOutcome::Unchanged);
    SealedResumeRecordStore foreign(path, scope(), changed.certificatePin, driver, authorization(), budget, budget);
    CHECK(!foreign.read());
    REQUIRE(store.read());
    SealedResumeRecordStore tooSmall(path, scope(), {}, driver, authorization(), budget, 1);
    const auto seals = FileSealer::current().sealCalls;
    CHECK(tooSmall.write(original) == ResumeWriteOutcome::Unchanged);
    CHECK(FileSealer::current().sealCalls == seals);
    REQUIRE(store.read());
    FileSealer::current().shouldRefuse = true;
    CHECK(store.purge() == ResumePurgeOutcome::Refused);
    FileSealer::current().shouldRefuse = false;
    // Restoring copied ciphertext cannot bypass the adapter's pending purge.
    REQUIRE(storageWriteDurable(path.c_str(), std::as_bytes(cipher->view())) == DurableFileResult::Durable);
    CHECK(!store.read());
    FileSealer::current().shouldRefuse = true;
    CHECK(store.write(original) == ResumeWriteOutcome::Unchanged);
    FileSealer::current().shouldRefuse = false;
    CHECK(store.purge() == ResumePurgeOutcome::Purged);
    REQUIRE(storageWriteDurable(path.c_str(), std::as_bytes(cipher->view())) == DurableFileResult::Durable);
    CHECK(!store.read());
}

TEST(sealedStoreDiskRefusalKeepsRealInputHeldUntilTheSecretCommits)
{
    using namespace UbiquittyTest::AttachmentLink;
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto missingParent = directory.path() + "/private";
    const auto path = missingParent + "/resume";
    const auto driver = sealer();
    constexpr ID128 instance { 41, 42 };
    constexpr std::size_t recordBudget = 1 << 20;
    SealedResumeRecordStore store(path, resumeSealScope(instance, principalDigest("alice"), clientInstance), {}, driver, authorization(), recordBudget, recordBudget);
    Host host(instance);
    std::uint64_t now = 1;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    attachFresh(host, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(client.send(paneID, raw("x"), now));
    pumpFor(host, client, now, 50'000);
    CHECK(host.pane.writer.bytes().empty());
    CHECK(!store.read());
    REQUIRE(::mkdir(missingParent.c_str(), 0700) == 0);
    client.persist();
    const auto committed = store.read();
    REQUIRE(committed);
    REQUIRE(client.held());
    CHECK(committed->resumeSecret == client.held()->resumeSecret);
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "x");
}

TEST(restartedStoreRefusesCopiedCiphertextAfterFailedRetirement)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    const auto original = record();
    SealedResumeRecordStore first(path, scope(), {}, driver, authorization(), budget, budget);
    REQUIRE(first.write(original) == ResumeWriteOutcome::Committed);
    const auto copied = storageReadBounded(path.c_str(), budget + SealedStore::overhead);
    REQUIRE(copied);
    FileSealer::current().shouldRefuse = true;
    CHECK(first.purge() == ResumePurgeOutcome::Refused);
    FileSealer::current().shouldRefuse = false;
    REQUIRE(storageWriteDurable(path.c_str(), std::as_bytes(copied->view())) == DurableFileResult::Durable);
    SealedResumeRecordStore restarted(path, scope(), {}, driver, authorization(), budget, budget);
    const auto opens = FileSealer::current().openCalls;
    CHECK(!restarted.read());
    CHECK(FileSealer::current().openCalls == opens);
    const auto generation = FileSealer::current().keyGeneration;
    REQUIRE(restarted.write(original) == ResumeWriteOutcome::Committed);
    CHECK(FileSealer::current().keyGeneration > generation);
    REQUIRE(restarted.read());
    REQUIRE(storageWriteDurable(path.c_str(), std::as_bytes(copied->view())) == DurableFileResult::Durable);
    CHECK(!restarted.read());
}

TEST(authorizationIntentRefusalPrecedesAnyKeyOrFileMutation)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    SealedResumeRecordStore first(path, scope(), {}, driver, authorization(), budget, budget);
    REQUIRE(first.write(record()) == ResumeWriteOutcome::Committed);
    const auto before = storageReadBounded(path.c_str(), budget + SealedStore::overhead);
    REQUIRE(before);
    TestAuthorization::current().shouldRefuseBlock = true;
    const auto seals = FileSealer::current().sealCalls;
    auto changed = record();
    ++changed.resumeSecret.low;
    CHECK(first.write(changed) == ResumeWriteOutcome::Unchanged);
    CHECK(first.purge() == ResumePurgeOutcome::Refused);
    CHECK(FileSealer::current().hasKey);
    CHECK(FileSealer::current().sealCalls == seals);
    const auto after = storageReadBounded(path.c_str(), budget + SealedStore::overhead);
    REQUIRE(after);
    CHECK(std::ranges::equal(before->view(), after->view()));
    // No durable deny intent was possible; refusal did not claim retirement.
    SealedResumeRecordStore restarted(path, scope(), {}, driver, authorization(), budget, budget);
    REQUIRE(restarted.read());
    CHECK(restarted.read()->resumeSecret == record().resumeSecret);
}

TEST(unconfirmedActivationKeepsAReplacementUnreadableAcrossRestart)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    SealedResumeRecordStore first(path, scope(), {}, driver, authorization(), budget, budget);
    REQUIRE(first.write(record()) == ResumeWriteOutcome::Committed);
    TestAuthorization::current().shouldRefuseActivation = true;
    auto changed = record();
    ++changed.resumeSecret.low;
    CHECK(first.write(changed) == ResumeWriteOutcome::DurabilityUnknown);
    REQUIRE(readSealedResumeFile(path.c_str(), scope(), driver, budget));
    SealedResumeRecordStore restarted(path, scope(), {}, driver, authorization(), budget, budget);
    CHECK(!restarted.read());
    TestAuthorization::current().shouldRefuseActivation = false;
    REQUIRE(restarted.write(changed) == ResumeWriteOutcome::Committed);
    REQUIRE(restarted.read());
    CHECK(restarted.read()->resumeSecret == changed.resumeSecret);
}

TEST(activeAuthorizationNamesOnlyTheLatestExactPlaintext)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    SealedResumeRecordStore first(path, scope(), {}, driver, authorization(), budget, budget);
    REQUIRE(first.write(record()) == ResumeWriteOutcome::Committed);
    const auto copied = storageReadBounded(path.c_str(), budget + SealedStore::overhead);
    REQUIRE(copied);
    auto changed = record();
    ++changed.resumeSecret.low;
    REQUIRE(first.write(changed) == ResumeWriteOutcome::Committed);
    REQUIRE(storageWriteDurable(path.c_str(), std::as_bytes(copied->view())) == DurableFileResult::Durable);
    SealedResumeRecordStore restarted(path, scope(), {}, driver, authorization(), budget, budget);
    CHECK(!restarted.read());
    const auto active = TestAuthorization::current().record;
    for (auto state : { RestoreAuthorization::State::Unavailable, RestoreAuthorization::State::Missing, RestoreAuthorization::State::Blocked }) {
        TestAuthorization::current().record.state = state;
        const auto opens = FileSealer::current().openCalls;
        CHECK(!restarted.read());
        CHECK(FileSealer::current().openCalls == opens);
    }
    TestAuthorization::current().record = active;
}

TEST(failedSealLeavesPersistentDenyIntentBeforeAnyReplacement)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    SealedResumeRecordStore first(path, scope(), {}, driver, authorization(), budget, budget);
    REQUIRE(first.write(record()) == ResumeWriteOutcome::Committed);
    FileSealer::current().shouldRefuse = true;
    CHECK(first.write(record()) == ResumeWriteOutcome::Unchanged);
    FileSealer::current().shouldRefuse = false;
    SealedResumeRecordStore restarted(path, scope(), {}, driver, authorization(), budget, budget);
    CHECK(!restarted.read());
    REQUIRE(restarted.write(record()) == ResumeWriteOutcome::Committed);
    REQUIRE(restarted.read());
}

TEST(sealedResumeFilePersistsOnlyEnvelopeAndRetainsBorrowedViewThroughMove)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    const auto original = record();
    REQUIRE(writeSealedResumeFile(path.c_str(), scope(), driver, original, budget));
    const auto disk = storageReadBounded(path.c_str(), budget);
    REQUIRE(disk);
    CHECK(disk->view()[0] == 0xa1);
    struct stat status {};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
    auto opened = readSealedResumeFile(path.c_str(), scope(), driver, budget);
    REQUIRE(opened);
    CHECK(opened->record.resumeSecret == original.resumeSecret);
    CHECK(opened->record.panes[0].lastInputSeq == 37);
    REQUIRE(opened->record.draftCount == 1);
    const auto* draft = opened->record.drafts.data();
    auto moved = std::move(*opened);
    CHECK(moved.record.drafts.data() == draft);
    CHECK(draft >= moved.plaintext.bytes.get());
    CHECK(draft + moved.record.drafts.size() <= moved.plaintext.bytes.get() + moved.plaintext.size);
}

TEST(sealedResumeWriteRefusalPreservesPreviousCiphertext)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    REQUIRE(writeSealedResumeFile(path.c_str(), scope(), driver, record(), budget));
    const auto previous = storageReadBounded(path.c_str(), budget);
    REQUIRE(previous);
    auto changed = record();
    changed.resumeSecret.low += 1;
    FileSealer::current().shouldRefuse = true;
    CHECK(!writeSealedResumeFile(path.c_str(), scope(), driver, changed, budget));
    CHECK(!writeSealedResumeFile(path.c_str(), scope(), SealedStore {}, changed, budget));
    FileSealer::current().shouldRefuse = false;
    CHECK(!writeSealedResumeFile(path.c_str(), scope(), driver, changed, 1));
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
        CHECK(!writeSealedResumeFile(path.c_str(), scope(), driver, changed, budget));
    }
    const auto after = storageReadBounded(path.c_str(), budget);
    REQUIRE(after);
    CHECK(std::ranges::equal(previous->view(), after->view()));
    CHECK(!writeSealedResumeFile(directory.path().c_str(), scope(), driver, changed, budget));
}

TEST(sealedResumeIdentityIsCheckedBeforeWriteAndAfterAuthenticatedRead)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    auto foreign = record();
    ++foreign.clientInstance.low;
    const auto mismatch = writeSealedResumeFile(path.c_str(), scope(), driver, foreign, budget);
    REQUIRE(!mismatch);
    CHECK(mismatch.error() == ResumeFileError::IdentityMismatch);
    CHECK(FileSealer::current().sealCalls == 0);
    // Even a correctly authenticated envelope cannot substitute a different
    // window identity inside its record.
    const auto plaintext = encodeResumeRecord(foreign, budget);
    REQUIRE(plaintext);
    const auto ciphertext = driver.seal(scope(), plaintext->view(), budget);
    REQUIRE(ciphertext);
    REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(ciphertext->view())));
    const auto opened = readSealedResumeFile(path.c_str(), scope(), driver, budget);
    REQUIRE(!opened);
    CHECK(opened.error() == ResumeFileError::IdentityMismatch);
    auto wrongPurpose = scope();
    wrongPurpose.purpose = SealScope::Purpose::History;
    CHECK(!readSealedResumeFile(path.c_str(), wrongPurpose, driver, budget));
}

TEST(sealedResumeReadRefusesDamageWrongPrincipalAndUnframedPlaintext)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    REQUIRE(writeSealedResumeFile(path.c_str(), scope(), driver, record(), budget));
    auto different = scope();
    different.principal[0] += 1;
    CHECK(!readSealedResumeFile(path.c_str(), different, driver, budget));
    auto disk = storageReadBounded(path.c_str(), budget);
    REQUIRE(disk);
    disk->bytes[14] ^= 1;
    REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(disk->view())));
    CHECK(!readSealedResumeFile(path.c_str(), scope(), driver, budget));
    const std::array<std::uint8_t, 4> malformed { 1, 2, 3, 4 };
    const auto sealed = driver.seal(scope(), malformed, budget);
    REQUIRE(sealed);
    REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(sealed->view())));
    const auto refused = readSealedResumeFile(path.c_str(), scope(), driver, budget);
    REQUIRE(!refused);
    CHECK(refused.error() == ResumeFileError::RecordRefused);
}

TEST(sealedResumeReadRejectsOversizeAndNullAllocationBeforeOpening)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    REQUIRE(writeSealedResumeFile(path.c_str(), scope(), driver, record(), budget));
    UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    const auto oversized = readSealedResumeFile(path.c_str(), scope(), driver, 1);
    REQUIRE(!oversized);
    CHECK(oversized.error() == ResumeFileError::TooLarge);
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts);
    const auto unavailable = readSealedResumeFile(path.c_str(), scope(), driver, budget);
    REQUIRE(!unavailable);
    CHECK(unavailable.error() == ResumeFileError::AllocationFailed);
    CHECK(FileSealer::current().openCalls == 0);
}

TEST(sealedResumeForgetReportsKeyFailureButAlwaysRemovesItsFile)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/resume";
    const auto driver = sealer();
    REQUIRE(writeSealedResumeFile(path.c_str(), scope(), driver, record(), budget));
    const auto copy = storageReadBounded(path.c_str(), budget);
    REQUIRE(copy);
    FileSealer::current().shouldRefuse = true;
    CHECK(!forgetSealedResumeFile(path.c_str(), scope(), driver));
    CHECK(::access(path.c_str(), F_OK) != 0);
    FileSealer::current().shouldRefuse = false;
    REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(copy->view())));
    CHECK(forgetSealedResumeFile(path.c_str(), scope(), driver));
    CHECK(::access(path.c_str(), F_OK) != 0);
    REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(copy->view())));
    CHECK(!readSealedResumeFile(path.c_str(), scope(), driver, budget));
}

UBIQUITTY_TEST_MAIN()
