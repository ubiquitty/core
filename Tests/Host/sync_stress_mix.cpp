// SPDX-FileCopyrightText: 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Assertions.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "LoopbackPaneLink.h"
#include "Platform/StreamDouble.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Session = std::remove_reference_t<decltype(Screen::makeSession(std::declval<Daemon&>(), 24, 80))>;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

struct Options {
    std::uint64_t seed = 1;
    unsigned seconds = 60;
    unsigned clients = 4;
    unsigned rssMiB = 64;
};

struct TemporaryHome {
    std::string path;
    std::optional<std::string> previous;

    TemporaryHome()
    {
        if (const char* value = std::getenv("UBIQUITTY_HOME"))
            previous = value;
        char pattern[] = "/tmp/ubq-sync-mix.XXXXXX";
        if (const char* made = ::mkdtemp(pattern)) {
            path = made;
            if (::setenv("UBIQUITTY_HOME", path.c_str(), 1) != 0) {
                ::rmdir(path.c_str());
                path.clear();
            }
        }
    }

    ~TemporaryHome()
    {
        if (previous)
            ::setenv("UBIQUITTY_HOME", previous->c_str(), 1);
        else
            ::unsetenv("UBIQUITTY_HOME");
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};

std::uint64_t peakResidentBytes()
{
    rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

bool parseOptions(int argc, char** argv, Options& options)
{
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 == argc)
            return false;
        const std::string_view key(argv[index]);
        const std::string_view argument(argv[index + 1]);
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
        if (parsed.ec != std::errc {} || parsed.ptr != argument.data() + argument.size())
            return false;
        if (key == "--seed")
            options.seed = value;
        else if (key == "--seconds" && value >= 1 && value <= 300)
            options.seconds = static_cast<unsigned>(value);
        else if (key == "--clients" && (value == 3 || value == 4))
            options.clients = static_cast<unsigned>(value);
        else if (key == "--rss-mib" && value >= 1 && value <= 1024)
            options.rssMiB = static_cast<unsigned>(value);
        else
            return false;
    }
    return true;
}

const Ubiquitty::Image::TerminalImagePlacement* placement(const Ubiquitty::Terminal* terminal)
{
    if (terminal) {
        for (const auto& item : terminal->images.placements) {
            if (item.used && item.imageID == 77 && item.hasPlacementID && item.placementID == 9)
                return &item;
        }
    }
    return nullptr;
}

const Ubiquitty::Image::TerminalImage* image(const Ubiquitty::Terminal* terminal)
{
    if (terminal) {
        for (const auto& item : terminal->images.stored) {
            if (item.used && item.imageID == 77 && item.pixels)
                return &item;
        }
    }
    return nullptr;
}

struct Client {
    std::unique_ptr<Sync::ReplicaSet> replicas = std::make_unique<Sync::ReplicaSet>();
    std::unique_ptr<UbiquittyTest::ScreenClient> screen;
    Sync::ID128 clientInstance {};
    Session* session = nullptr;
    std::size_t index = 0;
    bool hasSeenPlacement = false;
    std::uint64_t witnessedFrames = 0;
    std::uint64_t lastInputSequence = 0;
};

class Stress {
public:
    explicit Stress(Options options)
        : m_options(options)
        , m_random(options.seed ? options.seed : 0x9e3779b97f4a7c15ull)
    {
    }

    void run()
    {
        auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        m_stream = stream.get();
        m_session = &Screen::makeSession(m_daemon, 24, 80, std::move(stream));
        m_dormant = &Screen::makeSession(m_daemon, 24, 80);
        m_session->term->drawsSizedText = true;
        m_session->term->drawsProportionalText = true;
        Screen::publish(m_daemon, *m_session, Ubiquitty::Runtime::FrameClass::Structural);
        Screen::publish(m_daemon, *m_dormant, Ubiquitty::Runtime::FrameClass::Structural);
        replaceImage();
        for (unsigned index = 0; index < m_options.clients && !failures; ++index)
            attach(index, *m_session, std::nullopt);
        quietPoint();
        // Warm each operation and its buffers before measuring retained growth.
        for (unsigned tick = 0; tick < 6 && !failures; ++tick)
            step(tick);
        quietPoint();
        const std::uint64_t residentBefore = peakResidentBytes();
        REQUIRE_MSG(residentBefore != 0, "the RSS sampler failed");
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::seconds(m_options.seconds + 20);
        for (m_tick = 0; m_tick < m_options.seconds * 10 && !failures; ++m_tick) {
            std::this_thread::sleep_until(started + std::chrono::milliseconds(m_tick * 100));
            step(m_tick);
            if (m_tick % 10 == 9)
                quietPoint();
            const std::uint64_t resident = peakResidentBytes();
            CHECK_MSG(resident != 0 && resident <= residentBefore + static_cast<std::uint64_t>(m_options.rssMiB) * 1024 * 1024,
                context("RSS growth exceeded " + std::to_string(m_options.rssMiB) + " MiB").c_str());
            CHECK_MSG(std::chrono::steady_clock::now() < deadline, context("the seeded schedule exceeded its time budget").c_str());
        }
        quietPoint();
        std::uint64_t frames = 0;
        for (unsigned index = 0; index < m_options.clients; ++index) {
            frames += m_clients[index].witnessedFrames;
            CHECK_MSG(m_clients[index].witnessedFrames != 0, context("no frame witness for client " + std::to_string(index)).c_str());
        }
        std::printf("sync_stress_mix: seed=%llu ticks=%u clients=%u frames=%llu RSS-growth=%llu KiB quiet-points=%u\n",
            static_cast<unsigned long long>(m_options.seed), m_tick, m_options.clients,
            static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>((peakResidentBytes() - residentBefore) / 1024), m_quietPoints);
    }

private:
    std::string context(std::string message) const
    {
        return "seed=" + std::to_string(m_options.seed) + " tick=" + std::to_string(m_tick) + " " + message;
    }

    std::uint64_t next()
    {
        m_random ^= m_random >> 12;
        m_random ^= m_random << 25;
        m_random ^= m_random >> 27;
        return m_random * 0x2545f4914f6cdd1dull;
    }

    void feed(Session& session, std::string_view bytes)
    {
        Ubiquitty::processBytes(session.term.get(), bytes);
        session.isFrameDue = true;
        Screen::publish(m_daemon, session);
    }

    void replaceImage()
    {
        const std::string_view pixels = next() & 1 ? "/wAAgAAA//8=" : "AAD//xEAAP8=";
        feed(*m_session, std::string("\0337\033[1;1H\033_Ga=T,t=d,f=32,s=2,v=1,c=2,r=1,i=77,p=9,q=2;") + std::string(pixels) + "\033\\\0338");
        CHECK_MSG(placement(m_session->term.get()) && image(m_session->term.get()), context("host lost the replacement image").c_str());
    }

    void checkConnection(const Client& client)
    {
        CHECK_MSG(client.screen->connection().byeReason() != Sync::ByeReason::Malformed, context("client received Bye{Malformed}").c_str());
        const auto* connection = Screen::connection(m_daemon, client.index);
        CHECK_MSG(connection && connection->byeReason() != Sync::ByeReason::Malformed, context("host recorded Bye{Malformed}").c_str());
    }

    void attach(unsigned slot, Session& session, std::optional<Sync::ResumeRequest> resume)
    {
        Client& client = m_clients[slot];
        if (!resume)
            client.clientInstance = { .low = m_nextClientInstance++, .high = 0x737472657373 };
        client.session = &session;
        client.index = Screen::attach(m_daemon, session.id);
        Screen::grantScope(m_daemon, client.index, { Sync::Scope::Observe, Sync::Scope::Drive });
        Screen::setTokenID(m_daemon, client.index, "stress-client-" + std::to_string(slot));
        Screen::markInteractiveUser(m_daemon, client.index);
        const auto paneID = session.screenSource->paneID();
        if (!client.replicas->find(paneID)) {
            auto terminal = std::make_unique<Ubiquitty::Terminal>();
            terminal->drawsSizedText = true;
            terminal->drawsProportionalText = true;
            Ubiquitty::resize(*terminal, session.term->rows, session.term->columns);
            client.replicas->hold(paneID, std::move(terminal));
        }
        client.screen = std::make_unique<UbiquittyTest::ScreenClient>(session.term->rows, session.term->columns, *client.replicas,
            std::move(resume), client.clientInstance);
        client.hasSeenPlacement = false;
        client.screen->observeAppliedFrames([this, slot](const Sync::ClientConnection& connection) {
            Client& observed = m_clients[slot];
            ++observed.witnessedFrames;
            CHECK_MSG(connection.byeReason() != Sync::ByeReason::Malformed, context("frame observer saw Bye{Malformed}").c_str());
            if (observed.session != m_session || !connection.paneGeneration(1))
                return;
            const bool hasPlacement = placement(connection.paneTerminal(1)) && image(connection.paneTerminal(1));
            CHECK_MSG(!observed.hasSeenPlacement || hasPlacement,
                context("client " + std::to_string(slot) + " lost image 77 / placement 9 between frames").c_str());
            observed.hasSeenPlacement |= hasPlacement;
        });
        Screen::greet(m_daemon, client.index, *client.screen);
        checkConnection(client);
        CHECK_MSG(client.screen->connection().state() == Sync::ConnectionState::Attached, context("client did not attach").c_str());
        CHECK_MSG(client.screen->boundPane(1) == std::optional { paneID }, context("handle 1 does not name the selected pane").c_str());
        if (session.isFrameDue || !session.screenSource->isFrameHeld())
            Screen::publish(m_daemon, session, Ubiquitty::Runtime::FrameClass::Structural);
        CHECK_MSG(session.screenSource->isFrameHeld() && session.screenSource->generation() != 0,
            context("the selected Live pane has no published frame").c_str());
    }

    void drop(unsigned slot)
    {
        const std::size_t removed = m_clients[slot].index;
        Screen::reap(m_daemon, removed);
        m_clients[slot].screen.reset();
        for (unsigned index = 0; index < m_options.clients; ++index) {
            if (index != slot && m_clients[index].screen && m_clients[index].index > removed)
                --m_clients[index].index;
        }
    }

    void fresh(unsigned slot, Session& target)
    {
        Client& client = m_clients[slot];
        Screen::deliver(m_daemon, client.index, *client.screen);
        client.screen->detach();
        Screen::receive(m_daemon, client.index, client.screen->takeStream(Screen::now()));
        checkConnection(client);
        drop(slot);
        client.replicas = std::make_unique<Sync::ReplicaSet>();
        client.lastInputSequence = 0;
        attach(slot, target, std::nullopt);
    }

    void resume(unsigned slot)
    {
        Client& client = m_clients[slot];
        Screen::deliver(m_daemon, client.index, *client.screen);
        const auto welcome = client.screen->welcome();
        REQUIRE_MSG(welcome.has_value(), context("resume has no Welcome").c_str());
        const auto paneID = client.session->screenSource->paneID();
        client.screen->storeReplicas();
        const auto* held = client.replicas->find(paneID);
        REQUIRE_MSG(held, context("resume has no retained replica").c_str());
        Sync::ResumeRequest request { .attachmentID = welcome->attachmentID, .resumeSecret = welcome->resumeSecret, .panes = {} };
        request.panes.push_back({ .paneID = paneID, .ackedGeneration = held->replica.generation, .ackedRowHashesDigest = Sync::replicaDigest(held->replica), .historyEpoch = held->replica.historyEpoch, .cachedRanges = {}, .lastInputSeqSent = client.lastInputSequence, .view = Sync::ViewState::Live });
        client.screen->suspend();
        Screen::receive(m_daemon, client.index, client.screen->takeStream(Screen::now()));
        checkConnection(client);
        drop(slot);
        feed(*m_session, "\033[12;1H\033[2Koffline " + std::to_string(m_tick));
        attach(slot, *m_session, std::move(request));
        const auto resumed = client.screen->welcome();
        CHECK_MSG(resumed && resumed->attachmentID == welcome->attachmentID && !resumed->resumed.empty(),
            context("a resume silently became a fresh attachment").c_str());
    }

    bool hasCurrentPixels(const Client& client) const
    {
        const auto* expected = image(m_session->term.get());
        const auto* actual = image(client.screen->pane());
        return expected && actual && expected->width == actual->width && expected->height == actual->height
            && expected->pixelLength == actual->pixelLength && std::memcmp(expected->pixels, actual->pixels, expected->pixelLength) == 0;
    }

    bool hasCurrentPlacement(const Client& client) const
    {
        const auto* hostPlacement = placement(m_session->term.get());
        const auto* replicaPlacement = placement(client.screen->pane());
        return hostPlacement && replicaPlacement && hostPlacement->cursorX == replicaPlacement->cursorX
            && hostPlacement->cursorY == replicaPlacement->cursorY && hostPlacement->cellColumns == replicaPlacement->cellColumns
            && hostPlacement->cellRows == replicaPlacement->cellRows;
    }

    bool hasConverged(const Client& client) const
    {
        return client.session->screenSource->isFrameHeld() && client.session->screenSource->generation() != 0
            && client.screen->generation() == client.session->screenSource->generation()
            && UbiquittyTest::firstDisagreement(*client.session->term, client.screen->pane()).empty()
            && (client.session != m_session || (hasCurrentPixels(client) && hasCurrentPlacement(client) && client.hasSeenPlacement));
    }

    /// Carries every configured fixture client to its complete published state.
    ///
    /// At most 4,096 service rounds run within five seconds. Each round waits
    /// no longer than one millisecond or the earliest host/client deadline,
    /// so paced transfers remain pending work during an otherwise silent round.
    ///
    /// - Returns: Whether every replica converged before either bound.
    [[nodiscard]] bool waitForConvergence()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (unsigned round = 0; round < 4096 && std::chrono::steady_clock::now() < deadline; ++round) {
            const auto at = Screen::now();
            Screen::service(m_daemon, at);
            for (unsigned index = 0; index < m_options.clients; ++index) {
                Client& client = m_clients[index];
                if (!client.screen)
                    return false;
                client.screen->read(Screen::takeOutput(m_daemon, client.index), at);
                const auto up = client.screen->takeStream(at);
                if (!up.empty())
                    Screen::receive(m_daemon, client.index, up);
                checkConnection(client);
            }
            if (failures)
                return false;
            const auto clients = std::span(m_clients).first(m_options.clients);
            if (std::ranges::all_of(clients, [this](const Client& client) { return hasConverged(client); }))
                return true;
            auto nextWake = at + 1'000;
            for (const Client& client : clients) {
                if (const auto* host = Screen::connection(m_daemon, client.index)) {
                    if (const auto due = host->nextServiceMicroseconds(at))
                        nextWake = std::min(nextWake, *due);
                }
                if (const auto due = client.screen->connection().nextTickMicroseconds())
                    nextWake = std::min(nextWake, *due);
            }
            if (const auto clock = Screen::now(); nextWake > clock) {
                const auto wake = std::chrono::steady_clock::now() + std::chrono::microseconds(nextWake - clock);
                std::this_thread::sleep_until(std::min(deadline, wake));
            }
        }
        return false;
    }

    void compare(unsigned slot)
    {
        Client& client = m_clients[slot];
        CHECK_MSG(waitForConvergence(), context("quiet-point convergence exceeded five seconds or 4096 service rounds").c_str());
        checkConnection(client);
        const std::string difference = UbiquittyTest::firstDisagreement(*client.session->term, client.screen->pane());
        CHECK_MSG(difference.empty(), context("client " + std::to_string(slot) + ": " + difference).c_str());
        CHECK_MSG(client.screen->generation() == client.session->screenSource->generation(), context("replica generation did not converge").c_str());
        if (client.session != m_session)
            return;
        CHECK_MSG(hasCurrentPixels(client), context("replica image pixels did not converge").c_str());
        CHECK_MSG(hasCurrentPlacement(client), context("replica image placement did not converge").c_str());
        CHECK_MSG(client.hasSeenPlacement, context("image frame witness never armed").c_str());
    }

    void quietPoint()
    {
        if (failures)
            return;
        ++m_quietPoints;
        for (unsigned index = 0; index < m_options.clients && !failures; ++index)
            compare(index);
        if (failures)
            return;
        // This pane receives no input or subscriptions between quiet points.
        const unsigned probe = static_cast<unsigned>(next() % m_options.clients);
        fresh(probe, *m_dormant);
        compare(probe);
        fresh(probe, *m_session);
        compare(probe);
    }

    void step(unsigned tick)
    {
        replaceImage();
        const unsigned slot = static_cast<unsigned>(next() % m_options.clients);
        const unsigned action = static_cast<unsigned>((tick + m_options.seed % 6) % 6);
        const unsigned value = static_cast<unsigned>(next() % 16);
        std::printf("sync_stress_mix: seed=%llu tick=%u action=%u client=%u value=%u\n",
            static_cast<unsigned long long>(m_options.seed), tick, action, slot, value);
        Client& client = m_clients[slot];
        switch (action) {
        case 0:
            fresh(slot, *m_session);
            break;
        case 1:
            resume(slot);
            break;
        case 2:
            CHECK(client.screen->sendViewport(65 + static_cast<int>(value), 24 + static_cast<int>(value % 4)));
            Screen::receive(m_daemon, client.index, client.screen->takeStream(Screen::now()));
            Screen::endReadPass(m_daemon);
            Screen::publish(m_daemon, *m_session, Ubiquitty::Runtime::FrameClass::Structural);
            break;
        case 3: {
            feed(*m_session, "\033[6;1H\033[2K\033]66;p=1:c=20;run " + std::to_string(tick) + " \xe6\x97\xa5\033\\");
            m_stream->written.clear();
            m_stream->setInbound(std::vector<char> {});
            m_stream->pushInbound("\033]66;p=1:q=1:i=42;measure\007");
            Screen::servicePTY(m_daemon, *m_session);
            CHECK_MSG(m_stream->written.find("\033]66;i=42:c=") != std::string::npos, context("OSC 66 query was not answered to the program").c_str());
            Screen::publish(m_daemon, *m_session);
            break;
        }
        case 4: {
            const unsigned color = 32 + (++m_paletteChanges % 16) * 8;
            std::array<char, 2> channel {};
            const auto encoded = std::to_chars(channel.data(), channel.data() + channel.size(), color, 16);
            CHECK(encoded.ec == std::errc {} && encoded.ptr == channel.data() + channel.size());
            const auto before = m_dormant->term->palette;
            feed(*m_dormant, "\033]4;1;rgb:" + std::string(channel.data(), channel.size()) + "/20/40\007");
            CHECK_MSG(m_dormant->term->palette != before, context("dormant pane palette did not change").c_str());
            break;
        }
        case 5: {
            m_stream->written.clear();
            const std::string typed = "typed-" + std::to_string(tick);
            const auto* before = Screen::connection(m_daemon, client.index);
            const auto duplicatesBefore = before ? before->counters().inputDuplicates : 0;
            const auto writtenBefore = m_session->screenSource->inputLedger().lastWritten(client.clientInstance).value_or(0);
            const auto sequence = client.lastInputSequence + 1;
            const bool didEncode = client.screen->sendRawNumbered(1, sequence, { reinterpret_cast<const std::uint8_t*>(typed.data()), typed.size() });
            CHECK_MSG(didEncode, context("typed input was not encoded").c_str());
            if (didEncode) {
                CHECK_MSG(sequence > writtenBefore, context("Raw input sequence " + std::to_string(sequence) + " reuses ledger sequence " + std::to_string(writtenBefore)).c_str());
                client.lastInputSequence = sequence;
            }
            Screen::receive(m_daemon, client.index, client.screen->takeStream(Screen::now()));
            const auto* after = Screen::connection(m_daemon, client.index);
            CHECK_MSG(after && after->counters().inputDuplicates == duplicatesBefore, context("fresh Raw input was counted as a duplicate").c_str());
            CHECK_MSG(m_stream->written == typed, context("typed input did not reach the PTY exactly once").c_str());
            m_stream->setInbound(std::vector<char> {});
            m_stream->pushInbound("\033[10;1H\033[2K" + typed);
            Screen::servicePTY(m_daemon, *m_session);
            Screen::publish(m_daemon, *m_session);
            break;
        }
        default:
            CHECK_MSG(false, context("unknown seeded action").c_str());
            break;
        }
        for (unsigned index = 0; index < m_options.clients && !failures; ++index) {
            Screen::deliver(m_daemon, m_clients[index].index, *m_clients[index].screen);
            checkConnection(m_clients[index]);
        }
    }

    Options m_options;
    std::uint64_t m_random;
    std::uint64_t m_nextClientInstance = 1;
    Daemon m_daemon;
    Session* m_session = nullptr;
    Session* m_dormant = nullptr;
    Ubiquitty::TestDoubles::StreamDouble* m_stream = nullptr;
    std::array<Client, 4> m_clients;
    unsigned m_tick = 0;
    unsigned m_quietPoints = 0;
    unsigned m_paletteChanges = 0;
};

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parseOptions(argc, argv, options)) {
        std::fprintf(stderr, "usage: sync_stress_mix [--seconds 1..300] [--seed unsigned] [--clients 3|4] [--rss-mib 1..1024]\n");
        return 2;
    }
    TemporaryHome home;
    CHECK_MSG(!home.path.empty(), "a private UBIQUITTY_HOME could not be created");
    if (!failures) {
        Stress stress(options);
        stress.run();
    }
    return UBIQUITTY_TEST_SUMMARY();
}
