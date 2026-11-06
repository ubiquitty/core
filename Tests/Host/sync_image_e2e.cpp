// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/sync_image_e2e.cpp — an inline image a program prints on the host
// is held, decoded and placed, by a thin client's passive Terminal, in a REAL
// daemon driving a REAL shell.
//
// A thin client draws only what the wire gives it: the host's Terminal decodes
// the picture, and the pixels and placement table cross as typed ImageHead,
// BulkChunk and ImagePlacements frames. Their codecs are pinned by the Sync
// tests; this pins everything between the program and them — the escape
// surviving the PTY and the parse, the host's decode landing in the session's
// store, and the daemon sending the set beside the client's screen plane.
//
// The iTerm2 OSC 1337 `File=inline=1` form, Kitty transmit-and-place, and
// Sixel all reach the client. Placement-only changes retain their crop and
// pixel offsets, and RGBA pixels retain their alpha.
//
// Before either, the pixels. A program that places pictures sizes them from the
// winsize's pixel fields and refuses a terminal that reports none ("Terminal does
// not support reporting screen sizes in pixels"), and the host has no window to
// measure: the only pixels a session has are the ones its client sends in Viewport.
// The program here reads TIOCGWINSZ itself, so the assertion is what a real
// image tool would be told. Only the pane's geometry owner's pixels are the
// session's (Docs/sync.md §6.10), and only a person owns: the client reaches
// the daemon through its `--attach` bridge, as a person's client does over
// SSH, so the daemon verifies it as one.
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Image/TerminalImage.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "HostConnection.h"
#include "Platform/Host/HostSocketName.h"
#include "ScreenClient.h"

#include <csignal>
#include <ctime>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace Ubiquitty::HostTest;

namespace {

// A 24x24 RGB PNG in four flat quadrants, the snapshot tier's own feed.
constexpr const char* quadrantsPNG = "iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAIAAABvFaqvAAAAL0lEQVR42mO4Y6RPELXOZCCIGEYNGjVo1CC8Bql1XyKItnYyEESjBo0aNGoQXgQAtVDf3/jjW+IAAAAASUVORK5CYII=";

bool fail(const char* what)
{
    std::fprintf(stderr, "sync_image_e2e: FAIL — %s\n", what);
    return false;
}

std::uint64_t nowMilliseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

// Queries the terminal ScreenClient owns. Its canonical connection installs
// image owners and placements; this fixture never runs a second image decoder.
struct MirrorClient {
    explicit MirrorClient(const UbiquittyTest::ScreenClient& screen)
        : m_screen(screen)
    {
    }

    // The stored image with these pixel dimensions that a placement draws.
    [[nodiscard]] bool holdsPlacedImage(int width, int height) const
    {
        const auto* terminal = m_screen.pane();
        if (!terminal)
            return false;
        for (const Ubiquitty::Image::TerminalImage& image : terminal->images.stored) {
            if (!image.used || !image.pixels || image.width != width || image.height != height)
                continue;
            for (const Ubiquitty::Image::TerminalImagePlacement& placement : terminal->images.placements) {
                if (placement.used && placement.imageID == image.imageID)
                    return true;
            }
        }
        return false;
    }

    [[nodiscard]] int placedImageCount() const
    {
        const auto* terminal = m_screen.pane();
        if (!terminal)
            return 0;
        int placed = 0;
        for (const Ubiquitty::Image::TerminalImagePlacement& placement : terminal->images.placements)
            placed += placement.used ? 1 : 0;
        return placed;
    }

    [[nodiscard]] const Ubiquitty::Image::TerminalImagePlacement* placementForImage(std::uint32_t imageID) const
    {
        const auto* terminal = m_screen.pane();
        if (!terminal)
            return nullptr;
        for (const auto& placement : terminal->images.placements) {
            if (placement.used && placement.imageID == imageID)
                return &placement;
        }
        return nullptr;
    }

    [[nodiscard]] const Ubiquitty::Image::TerminalImage* imageWithID(std::uint32_t imageID) const
    {
        const auto* terminal = m_screen.pane();
        if (!terminal)
            return nullptr;
        for (const auto& image : terminal->images.stored) {
            if (image.used && image.imageID == imageID)
                return &image;
        }
        return nullptr;
    }

private:
    const UbiquittyTest::ScreenClient& m_screen;
};

// Whether a row of the screen the client replicates holds `needle`.
[[nodiscard]] bool screenContains(const UbiquittyTest::ScreenClient& screen, const std::string& needle)
{
    const Ubiquitty::Terminal* terminal = screen.pane();
    if (!terminal)
        return false;
    for (int row = 0; row < terminal->rows; ++row) {
        std::string line;
        for (int column = 0; column < terminal->columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(terminal, 0, row, column);
            const std::uint32_t codePoint = cell && cell->codePoint ? cell->codePoint : ' ';
            line.push_back(codePoint >= 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : '?');
        }
        if (line.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// Carries the screen plane for up to `windowMilliseconds`, applying the image
// frames beside it in arrival order, and stops early once `isDone` holds.
template<typename Predicate>
void pump(Connection& c, UbiquittyTest::ScreenClient& screen, int windowMilliseconds, Predicate isDone)
{
    const std::uint64_t deadline = nowMilliseconds() + static_cast<std::uint64_t>(windowMilliseconds);
    while (nowMilliseconds() < deadline && !isDone()) {
        const std::vector<std::uint8_t> up = screen.takeUpstream(UbiquittyTest::screenClientNow());
        for (std::size_t written = 0; written < up.size();) {
            const ssize_t n = ::write(c.fileDescriptor, up.data() + written, up.size() - written);
            if (n <= 0)
                return;
            written += static_cast<std::size_t>(n);
        }
        pollfd p { c.fileDescriptor, POLLIN, 0 };
        if (::poll(&p, 1, 25) <= 0)
            continue;
        std::uint8_t buffer[65536];
        const ssize_t n = ::read(c.fileDescriptor, buffer, sizeof(buffer));
        if (n <= 0)
            return;
        const std::span<const std::uint8_t> bytes(buffer, static_cast<std::size_t>(n));
        screen.read(bytes, UbiquittyTest::screenClientNow());
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }

    char homeTemplate[] = "/tmp/ubiquitty-syncimage.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(argv[1]);
    ::mkdir(macHome.c_str(), 0700);

    const pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        char* args[] = { argv[1], const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"),
            const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(argv[1], args);
        _exit(127);
    }

    bool ok = false;
    pid_t bridge = -1;
    do {
        if (!waitForDaemonReady(sockPath, 30)) {
            fail("daemon did not come up");
            break;
        }
        Connection c;
        c.fileDescriptor = Ubiquitty::HostTest::connectThroughAttachBridge(argv[1], macHome, bridge);
        if (c.fileDescriptor < 0) {
            fail("connect");
            break;
        }
        UbiquittyTest::ScreenClient screen(24, 80);
        // The authenticated native bridge is a person. Presenting an Agent
        // credential here would surrender the geometry ownership being tested.
        if (!screen.start({ .target = Ubiquitty::Protocol::Sync::AdmissionTarget::Session })) {
            fail("the canonical native session greeting did not start");
            break;
        }
        MirrorClient client(screen);
        pump(c, screen, 8000, [&] { return screen.connection().counters().checkpointsApplied > 0; });
        const auto welcome = screen.welcome();
        if (screen.connection().counters().checkpointsApplied == 0 || !welcome
            || welcome->admission != Ubiquitty::Protocol::Sync::AttachmentAdmission::Granted
            || welcome->grantedRole != Ubiquitty::Protocol::Sync::Role::Owner
            || welcome->selection.target != Ubiquitty::Protocol::Sync::AdmissionTarget::Session
            || !welcome->selection.sessionID || !welcome->selection.daemonInstanceID
            || welcome->selection.socketName != Ubiquitty::Host::socketLeafForBinary(argv[1])) {
            fail("no checkpoint — the session never came up");
            break;
        }
        ::usleep(600 * 1000); // let the shell reach a prompt
        pump(c, screen, 600, [] { return false; });

        // The pixels this client draws in: a 10x20 cell, the 800x480 text area
        // of the 80x24 grid it attached at, inside an 820x500 drawable.
        if (!screen.sendViewport({ .rows = 24, .columns = 80, .cellPixelWidth = 10, .cellPixelHeight = 20, .textAreaPixelWidth = 800, .textAreaPixelHeight = 480, .drawablePixelWidth = 820, .drawablePixelHeight = 500 })) {
            fail("the client's viewport of the pane was not sent");
            break;
        }
        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen, "python3 -c 'import fcntl, struct, termios; print(\"winsize=%d,%d,%d,%d\" % struct.unpack(\"4H\", fcntl.ioctl(0, termios.TIOCGWINSZ, bytes(8))))'\r");
        pump(c, screen, 6000, [&] { return screenContains(screen, "winsize=24,80,800,480"); });
        if (!screenContains(screen, "winsize=24,80,800,480")) {
            fail("the program on the host was not told the pixels the client draws in (expected winsize=24,80,800,480)");
            break;
        }

        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen, std::string("printf '\\033]1337;File=inline=1;width=24px;height=24px;size=104:") + quadrantsPNG + "\\007\\n'\r");
        pump(c, screen, 6000, [&] { return client.holdsPlacedImage(24, 24); });
        if (!client.holdsPlacedImage(24, 24)) {
            std::fprintf(stderr, "sync_image_e2e: after OSC 1337 — %zu ImageHead, %zu ImagePlacements applied, %d placements\n",
                screen.count(Ubiquitty::Protocol::Sync::FrameKind::ImageHead), screen.count(Ubiquitty::Protocol::Sync::FrameKind::ImagePlacements), client.placedImageCount());
            fail("the client holds no placed image after the host printed an OSC 1337 inline image");
            break;
        }

        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen, std::string("printf '\\033_Gf=100,a=T,i=77,q=2;") + quadrantsPNG + "\\033\\\\\\n'\r");
        pump(c, screen, 6000, [&] { return client.placedImageCount() >= 2; });
        const bool isKittyImagePlaced = client.placementForImage(77);
        std::fprintf(stderr, "sync_image_e2e: %zu ImageHead, %zu ImagePlacements applied, %d placements\n",
            screen.count(Ubiquitty::Protocol::Sync::FrameKind::ImageHead), screen.count(Ubiquitty::Protocol::Sync::FrameKind::ImagePlacements), client.placedImageCount());
        if (!isKittyImagePlaced) {
            fail("the client holds no placement of the Kitty image the host transmitted");
            break;
        }

        const auto hasPlacementGeometry = [&](std::uint16_t sourceX, std::uint16_t sourceY, std::uint16_t pixelX, std::uint16_t pixelY) {
            const auto* placement = client.placementForImage(77);
            return placement && placement->placementID == 8 && placement->hasPlacementID
                && placement->cursorX == 4 && placement->cursorY == 4
                && placement->cellColumns == 2 && placement->cellRows == 2
                && placement->sourceWidth == 12 && placement->sourceHeight == 12
                && placement->sourceX == sourceX && placement->sourceY == sourceY
                && placement->pixelOffsetX == pixelX && placement->pixelOffsetY == pixelY;
        };
        const auto dumpPlacementFailure = [&] {
            std::fprintf(stderr, "sync_image_e2e: placement failure after %zu ImageHead, %zu ImagePlacements, %d placements\n",
                screen.count(Ubiquitty::Protocol::Sync::FrameKind::ImageHead), screen.count(Ubiquitty::Protocol::Sync::FrameKind::ImagePlacements), client.placedImageCount());
            const Ubiquitty::Terminal* terminal = screen.pane();
            if (!terminal) {
                std::fprintf(stderr, "sync_image_e2e: no replicated screen\n");
                return;
            }
            for (const auto& placement : terminal->images.placements) {
                if (!placement.used)
                    continue;
                std::fprintf(stderr, "sync_image_e2e: image=%u placement=%u hasID=%u cursor=(%u,%d) cells=(%u,%u) crop=(%u,%u,%u,%u) offset=(%u,%u)\n",
                    static_cast<unsigned>(placement.imageID), static_cast<unsigned>(placement.placementID), static_cast<unsigned>(placement.hasPlacementID),
                    static_cast<unsigned>(placement.cursorX), static_cast<int>(placement.cursorY),
                    static_cast<unsigned>(placement.cellColumns), static_cast<unsigned>(placement.cellRows),
                    static_cast<unsigned>(placement.sourceX), static_cast<unsigned>(placement.sourceY),
                    static_cast<unsigned>(placement.sourceWidth), static_cast<unsigned>(placement.sourceHeight),
                    static_cast<unsigned>(placement.pixelOffsetX), static_cast<unsigned>(placement.pixelOffsetY));
            }
            for (int row = 0; row < terminal->rows; ++row) {
                std::string line;
                for (int column = 0; column < terminal->columns; ++column) {
                    const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(terminal, 0, row, column);
                    const std::uint32_t codePoint = cell && cell->codePoint ? cell->codePoint : ' ';
                    line.push_back(codePoint >= 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : '?');
                }
                std::fprintf(stderr, "sync_image_e2e: screen[%d] |%s|\n", row, line.c_str());
            }
        };
        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen,
            R"(printf '\033[5;5H\033_Ga=d,d=i,i=77,q=2\033\\\033_Ga=p,i=77,p=8,c=2,r=2,x=1,y=2,w=12,h=12,X=1,Y=2,C=1,q=2\033\\')"
            "\r");
        pump(c, screen, 6000, [&] { return hasPlacementGeometry(1, 2, 1, 2); });
        if (!hasPlacementGeometry(1, 2, 1, 2)) {
            dumpPlacementFailure();
            fail("the Kitty placement's crop and pixel offsets did not arrive");
            break;
        }
        // Replacing a placement without retransmitting the image keeps the
        // same identity and extent; its crop and sub-cell offset still publish.
        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen,
            R"(printf '\033[5;5H\033_Ga=d,d=i,i=77,q=2\033\\\033_Ga=p,i=77,p=8,c=2,r=2,x=3,y=4,w=12,h=12,X=3,Y=4,C=1,q=2\033\\')"
            "\r");
        pump(c, screen, 6000, [&] { return hasPlacementGeometry(3, 4, 3, 4); });
        if (!hasPlacementGeometry(3, 4, 3, 4)) {
            dumpPlacementFailure();
            fail("a placement-only crop and offset change was not published");
            break;
        }

        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen,
            R"(printf '\033_Ga=T,t=d,f=32,s=2,v=1,c=2,r=1,i=88,q=2;/wAAgAAA//8=\033\\')"
            "\r");
        pump(c, screen, 6000, [&] { return client.placementForImage(88) && client.imageWithID(88); });
        const auto* transparent = client.imageWithID(88);
        constexpr unsigned char rgba[] { 255, 0, 0, 128, 0, 0, 255, 255 };
        if (!client.placementForImage(88) || !transparent || transparent->format != 32 || transparent->pixelLength != sizeof(rgba)
            || !transparent->pixels || std::memcmp(transparent->pixels, rgba, sizeof(rgba))) {
            fail("the Kitty RGBA pixels lost their alpha or color");
            break;
        }

        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen,
            R"(printf '\033P0;0;0q"1;1;6;6#0;2;100;0;0#0~~~~~~\033\\')"
            "\r");
        pump(c, screen, 6000, [&] { return client.holdsPlacedImage(6, 6); });
        if (!client.holdsPlacedImage(6, 6)) {
            fail("the client holds no placed image after the host printed Sixel");
            break;
        }

        // A dashboard: two charts sent again in place every 300 ms, under the
        // same image and placement IDs, zlib-compressed and chunked as a
        // program writes them, inside synchronized output. The first pixel of
        // each picture spells its frame.
        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen, "clear; python3 - <<'PY'\r");
        for (const char* line : {
                 "import base64, sys, time, zlib",
                 "W = 330",
                 "def chart(i, f, h):",
                 "    p = bytearray(W * h * 4)",
                 "    for x in range(W):",
                 "        y = 2 + ((x // 6 + f) * 37 + i * 11) % (h - 4)",
                 "        p[(y * W + x) * 4:(y * W + x) * 4 + 4] = bytes((57, 135, i % 256, 255))",
                 "    p[0:4] = bytes((f, 0, 1, 255))",
                 "    return bytes(p)",
                 "def place(i, row, rows, f):",
                 "    h = rows * 19",
                 "    data = base64.standard_b64encode(zlib.compress(chart(i, f, h), 6)).decode()",
                 "    parts = [data[n:n + 4096] for n in range(0, len(data), 4096)]",
                 "    out = '\\x1b[%d;9H' % row",
                 "    for n, part in enumerate(parts):",
                 "        m = 1 if n + 1 < len(parts) else 0",
                 "        head = 'a=T,i=%d,p=1,f=32,o=z,s=%d,v=%d,c=55,r=%d,C=1,q=2,m=%d' % (i, W, h, rows, m) if n == 0 else 'm=%d' % m",
                 "        out += '\\x1b_G%s;%s\\x1b\\\\' % (head, part)",
                 "    return out",
                 "for f in range(10):",
                 "    sys.stdout.write('\\x1b[?2026h' + place(7401, 6, 5, f) + place(7402, 14, 4, f) + '\\x1b[?2026l')",
                 "    sys.stdout.flush()",
                 "    time.sleep(0.3)",
                 "sys.stdout.write('\\x1b[22;1Hcharts-done\\n')",
                 "PY" })
            (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, screen, std::string(line) + "\r");
        const auto shownFrame = [&](std::uint32_t imageID) -> int {
            const auto* image = client.imageWithID(imageID);
            if (!client.placementForImage(imageID) || !image || !image->pixels || image->pixelLength < 4 || image->pixels[2] != 1)
                return -1;
            return image->pixels[0];
        };
        bool haveBothArrived = false;
        int gaps = 0;
        int samples = 0;
        pump(c, screen, 15000, [&] {
            const int top = shownFrame(7401);
            const int bottom = shownFrame(7402);
            if (top >= 0 && bottom >= 0)
                haveBothArrived = true;
            else if (haveBothArrived)
                ++gaps;
            ++samples;
            return top == 9 && bottom == 9 && screenContains(screen, "charts-done");
        });
        std::fprintf(stderr, "sync_image_e2e: charts replaced in place — %d samples, %d without both charts, frames shown %d and %d\n",
            samples, gaps, shownFrame(7401), shownFrame(7402));
        if (!haveBothArrived || gaps || shownFrame(7401) != 9 || shownFrame(7402) != 9) {
            dumpPlacementFailure();
            fail("a chart replaced in place left its placement empty, or its last frame did not arrive");
            break;
        }
        ok = true;
    } while (false);

    int status = 0;
    if (bridge > 0) {
        ::kill(bridge, SIGTERM);
        ::waitpid(bridge, &status, 0);
    }
    ::kill(daemon, SIGTERM);
    ::waitpid(daemon, &status, 0);

    if (ok)
        std::fprintf(stderr, "sync_image_e2e: a real daemon delivers inline, Kitty, and Sixel images with alpha and placement updates\n");
    return ok ? 0 : 1;
}
