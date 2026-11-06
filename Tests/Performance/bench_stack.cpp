// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_stack.cpp — the portable hot-path benchmarks for the automated perf
// harness, emitting one JSON object via BenchHarness.
//
// P1 coverage: VT parser throughput on representative streams (plain text, an
// SGR-heavy stream, and a scroll-heavy stream). Pure CPU through
// Terminal::processBytes — no rendering, no PTY — so the numbers are stable and
// comparable across platforms. Follow-ups add the screen plane's checkpoint and update, the transport
// channels, and RemoteSessionCore under this same harness.
//
//   bench_stack [--platform NAME] [--commit SHA] [--mib N] [--iters N]

#include "BenchHarness.h"
#include "Core/CellPen.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/LogicalPosition.h"
#include "Core/VT/SoftFont.h"
#include "SearchMaintenanceBench.h"
#include <memory>
#include <span>

#include "Core/CellColor.h"
#include "Core/CellGrid.h"
#include "Core/Codec/ZlibDeflate.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Input/PointerShape.h"
#include "Core/Keys/KeyEvent.h"
#include "Core/Keys/Keymap.h"
#include "Core/Keys/KittyKeyboard.h"
#include "Core/Kitty/KeyboardFlags.h"
#include "Core/LineStore.h"
#include "Core/Modes/SyncOutput.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Search/SearchController.h"
#include "Core/Selection/Selection.h"
#include "Core/Selection/StyledClipboard.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Themes/Themes.h"
#include "PAL/PAL.h"

#include "Core/CellColor.h"
#include "Core/Rendering/GlyphSlotPolicy.h"
#include "Core/Rendering/RenderPlan.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"
#include "Core/TextSizeTable.h"
#include "Core/Transport/SubprocessChannel.h"
#include "StrictAtlas.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// Repeat `unit` until the buffer reaches ~targetBytes.
std::vector<char> repeatTo(const std::string& unit, std::size_t targetBytes)
{
    std::vector<char> out;
    out.reserve(targetBytes + unit.size());
    while (out.size() < targetBytes)
        out.insert(out.end(), unit.begin(), unit.end());
    return out;
}

std::vector<char> plainStream(std::size_t bytes) { return repeatTo("The quick brown fox jumps over the lazy dog 0123456789\r\n", bytes); }
std::vector<char> sgrStream(std::size_t bytes)
{
    // Heavy SGR churn: bold/colour set + reset around short runs.
    return repeatTo("\x1b[1;31mERR\x1b[0m \x1b[32mok\x1b[0m \x1b[4;33mwarn\x1b[0m \x1b[7mrev\x1b[0m\r\n", bytes);
}
// G-set designation + translated text. Every parse lane feeds US-ASCII, and
// resolveGLByte returns the byte unchanged for that set before reaching any
// table — so the whole character-set path compiled cold under -fprofile-use
// while the corpus reported full parse coverage.
//
// One designation per line rather than a block of each: an application switches
// sets around short runs (a box-drawing frame, an accented word, a technical
// symbol), so the branch is taken and untaken at that rhythm rather than
// predicted straight through a thousand identical cells.
//
// The five arms of resolveGLByte are all here, because they are separate
// tables and a stream that reached one would leave the others exactly as cold:
//   ESC ( 0  DEC Special Graphics  -> decSpecialToUnicode  (0x60..0x7E only)
//   ESC ( >  DEC Technical         -> decTechnicalToUnicode
//   ESC ( <  DEC Supplemental      -> set96Translate, GL folded up into GR
//   ESC ( R  French NRCS           -> nrcsTranslate
//   ESC ( A  UK NRCS               -> the '#' special case, which is its own arm
// ESC ( B restores US-ASCII so each line pays the designation twice.
std::vector<char> characterSetStream(std::size_t bytes)
{
    return repeatTo("\x1b(0lqwqk mno \x1b(Btext \x1b(>abgd \x1b(Bmore \x1b(<def \x1b(Bplain "
                    "\x1b(R\x40\x5b\x5c \x1b(Bend \x1b(A#42 \x1b(B\r\n",
        bytes);
}

std::vector<char> scrollStream(std::size_t bytes)
{
    // Newline-dense: forces scroll/line management every few bytes.
    return repeatTo("x\r\n", bytes);
}

// REP expands a small input into many cell writes. Bound the encoded corpus
// independently of --mib so the default parse battery stays practical. Every
// REP follows a fresh graphic, preventing REP-after-REP multiplication. The
// throughput counts encoded input bytes, as in the other parse lanes.
constexpr std::size_t maxRepeatStreamBytes = 64 * 1024;

std::vector<char> repeatedASCIIStream(std::size_t bytes)
{
    // Column 79 of the 80-column grid leaves one cell after the seed: the
    // repeat crosses two margins, reaching both fill batches and wrap fallbacks.
    return repeatTo("\x1b[79Ga\x1b[160b\r\n\x1b[79GZ\x1b[160b\r\n",
        std::min(bytes, maxRepeatStreamBytes));
}

std::vector<char> repeatedWideStream(std::size_t bytes)
{
    // Each seed fills the last wide-cell pair, leaving a pending wrap. REP
    // then emits 80 wide graphics through the scalar Unicode path across rows.
    return repeatTo("\x1b[79G\xe6\x97\xa5\x1b[80b\r\n"
                    "\x1b[79G\xe8\xaa\x9e\x1b[80b\r\n",
        std::min(bytes, maxRepeatStreamBytes));
}

// Wide text: CJK, where every cell is two columns and every character costs a
// UTF-8 decode before it reaches a width table.
//
// The same blindness the character-set lane above exists for, one layer
// out. `plainStream` is US-ASCII, so the batched text path's non-ASCII branch
// and the whole non-ASCII put path never execute during training — inside
// functions whose other blocks run constantly, which is why naming the symbol
// in the profile says nothing about the branch.
//
// Mixed with ASCII rather than a solid block of CJK: a run of one width
// predicts straight through, and the interesting work is the transition — a
// wide cell landing at the last column, a run that has to be cut where the
// width changes. Real CJK text carries ASCII punctuation and digits at exactly
// this rhythm.
std::vector<char> wideTextStream(std::size_t bytes)
{
    return repeatTo("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e text 123 "
                    "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95 mixed "
                    "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 end\r\n",
        bytes);
}

// Emoji, which are wide, multi-byte, and carry presentation state the ASCII
// paths never ask about.
//
// Separate from the CJK lane because the questions differ: a BMP wide
// character resolves its width from a table, while an emoji reaches the
// presentation test that decides whether it draws wide at all — and a ZWJ
// sequence and a variation selector both make several code points into one
// cluster, which is the cluster path rather than the batched one.
//
// The three shapes are here because they take different branches: a bare
// pictograph with default-emoji presentation, one forced wide by U+FE0F, and a
// ZWJ family that has to be held together across five code points.
std::vector<char> emojiStream(std::size_t bytes)
{
    return repeatTo("ok \xf0\x9f\x98\x80 done \xe2\x9c\x8b\xef\xb8\x8f wave "
                    "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
                    "\xf0\x9f\x91\xa7 family \xe2\x98\x85 star\r\n",
        bytes);
}

// Hyperlink-dense rows, ls --hyperlink-shaped: every entry wraps its name in
// an OSC 8 open/close pair. Two sub-patterns real listings produce: the same
// URI re-opened across adjacent spans (icon + name share the target), and a
// DISTINCT URI per entry (each file its own target). linkIntern runs once
// per open — this is its lane.
std::vector<char> hyperlinkStream(std::size_t bytes)
{
    std::string unit;
    for (int entry = 0; entry < 24; ++entry) {
        const std::string uri = "file:///Users/user/dev/project/Source/Core/File" + std::to_string(entry) + ".cpp";
        // icon span + name span share the target: a repeat-open per entry
        unit += "\x1b]8;;" + uri + "\x1b\\";
        unit += "\xef\x84\x9b "; // a Nerd Font icon glyph
        unit += "\x1b]8;;\x1b\\";
        unit += "\x1b]8;;" + uri + "\x1b\\";
        unit += "File" + std::to_string(entry) + ".cpp";
        unit += "\x1b]8;;\x1b\\";
        unit += "\r\n";
    }
    std::vector<char> out;
    out.reserve(bytes + unit.size());
    while (out.size() < bytes)
        out.insert(out.end(), unit.begin(), unit.end());
    return out;
}

std::vector<char> oscStream(std::size_t bytes)
{
    // OSC/APC payload-heavy: a large OSC 52 clipboard set, an OSC 0 title, and a
    // Kitty APC graphics chunk — exercises the appendOSCPayloadRun /
    // appendAPCPayloadRun bulk batchers (each terminated by ST = ESC \). Payloads
    // are pure base64 (no control bytes), so the batcher scans a long clean run to
    // the break — the path the CAN/SUB break handling lives on.
    std::string unit = "\x1b]52;c;";
    for (int k = 0; k < 96; ++k)
        unit += "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVowMTIzNDU2Nzg5"; // ~4.6 KiB clipboard payload
    unit += "\x1b\\";
    unit += "\x1b]0;ubiquitty bench.cpp:42 build green ~/dev/ubiquitty\x1b\\";
    unit += "\x1b_Ga=T,f=32,s=16,v=16,m=1;";
    for (int k = 0; k < 96; ++k)
        unit += "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAA"; // ~4.6 KiB APC image data
    unit += "\x1b\\";
    return repeatTo(unit, bytes);
}

std::vector<char> oscDigitRunStream(std::size_t bytes)
{
    // An OSC whose payload is nothing but digits. Which ceiling an OSC gets is
    // decided by re-reading its leading digits from the accumulator, and that read
    // happens on every appended byte — so a payload that is all digits has no
    // separator to stop it, and `ps * 10 + 0` stays at zero, which never trips the
    // overflow early-out either. The scan length is then the payload length, per
    // byte. Nothing legitimate looks like this; the lane exists to keep the read
    // bounded rather than to represent a workload.
    std::string unit = "\x1b]";
    unit.append(4096, '0');
    unit += "\x1b\\";
    return repeatTo(unit, bytes);
}

// Standard base64 (RFC 4648) — the Kitty APC payload encoding. Kept local to the
// bench: the core ships only the decoder (kittyDecodeBase64Var), and the bench
// needs the inverse to synthesize o=z transmits.
std::string base64Encode(const std::uint8_t* data, std::size_t n)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out.push_back(table[(v >> 6) & 63]);
        out.push_back(table[v & 63]);
    }
    if (i < n) {
        const bool haveTwo = (i + 1 < n);
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (haveTwo)
            v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        out.push_back(haveTwo ? table[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// A DOS/emulator-style RGB frame: flat horizontal palette bands plus a sparse
// moving-sprite pattern — representative of a framebuffer-video source (large
// flat regions, a small palette) that o=z compresses hugely, so the bench
// measures the DECODE side (inflate + parse), not a pathological worst case.
std::vector<std::uint8_t> dosStyleFrame(int width, int height)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t band = static_cast<std::uint8_t>((y / 8) & 7);
        const std::uint8_t r = static_cast<std::uint8_t>(band * 32);
        const std::uint8_t g = static_cast<std::uint8_t>(255 - band * 24);
        const std::uint8_t b = static_cast<std::uint8_t>((y * 3) & 0xFF);
        for (int x = 0; x < width; ++x) {
            std::uint8_t* p = &pixels[(static_cast<std::size_t>(y) * width + x) * 3];
            const bool isSprite = ((x ^ y) & 0x3F) < 4; // a few lit texels per row
            p[0] = isSprite ? 255 : r;
            p[1] = isSprite ? 255 : g;
            p[2] = isSprite ? 0 : b;
        }
    }
    return pixels;
}

// One Kitty o=z direct-pixel transmit-and-display of a width x height RGB frame,
// re-using image id 1 — the framebuffer-video pattern the item targets (a re-used
// id avoids per-frame image alloc/free churn; see the same-id texture-cache fix).
std::string kittyOzDirectFrame(int width, int height)
{
    const std::vector<std::uint8_t> pixels = dosStyleFrame(width, height);
    const std::vector<std::uint8_t> compressed = Ubiquitty::Codec::Zlib::compress({ pixels.data(), pixels.size() });
    std::string apc = "\x1b_Ga=T,f=24,o=z,i=1,s=";
    apc += std::to_string(width);
    apc += ",v=";
    apc += std::to_string(height);
    apc += ";";
    apc += base64Encode(compressed.data(), compressed.size());
    apc += "\x1b\\";
    return apc;
}

// Repeated-frame o=z decode throughput — the framebuffer-video workload (a client
// streaming full frames at 30-60 fps). Reported over DECODED pixel bytes (what a
// streamer's frame budget is spent on), plus frames/sec, so the throughput floor
// (decode stays ahead of the client) is directly readable. The o=z inflate runs
// on the kittyInflateQueue TaskQueue at submit, so processBytes captures the real
// per-frame inflate + parse cost.
void kittyOzStreamBenches(std::vector<Ubiquitty::Bench::Result>& out, int width, int height, int framesPerIter, int warmup, int iters, const std::string& suffix = "")
{
    const std::string frame = kittyOzDirectFrame(width, height);
    std::string stream;
    stream.reserve(frame.size() * static_cast<std::size_t>(framesPerIter));
    for (int i = 0; i < framesPerIter; ++i)
        stream += frame;

    // Correctness gate: a broken payload would make the lane measure nothing.
    {
        auto probe = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*probe, 48, 160);
        Ubiquitty::processBytes(probe.get(), frame);
        if (Ubiquitty::Image::placementCount(probe.get()) == 0)
            std::fprintf(stderr, "[bench] WARN kitty_oz_decode%s: frame did not decode/place\n", suffix.c_str());
        probe.reset();
    }

    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, 48, 160);
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] { Ubiquitty::processBytes(term.get(), std::string_view(stream)); });
    term.reset();

    const double decodedBytes = static_cast<double>(framesPerIter) * static_cast<double>(width) * static_cast<double>(height) * 3.0;
    out.push_back(Ubiquitty::Bench::throughput("kitty_oz_decode" + suffix, decodedBytes, timing));

    Ubiquitty::Bench::Result fps;
    fps.name = "kitty_oz_fps" + suffix;
    fps.unit = "fps";
    fps.value = timing.medianSeconds > 0.0 ? framesPerIter / timing.medianSeconds : 0.0;
    fps.p95 = timing.p95Seconds > 0.0 ? framesPerIter / timing.p95Seconds : 0.0;
    out.push_back(fps);
}

// CPU floor of the streamed same-id texture upload: the per-frame RGB(24)->RGBA8
// widen (Image::widenToRGBA8) the upload path runs before the Metal replaceRegion.
// Isolates the CPU-side upload cost from the GPU-side in-flight-texture write
// stall (which needs a live GPU run to observe; this bounds the CPU component
// so a live per-frame enc spike can be attributed to the hazard or the widen).
// Also bounds the headroom a faster widen could recover.
void kittyUploadWidenBenches(std::vector<Ubiquitty::Bench::Result>& out, int width, int height, int warmup, int iters, const std::string& suffix = "")
{
    const std::vector<std::uint8_t> rgb = dosStyleFrame(width, height); // width*height*3 (format 24)
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    std::size_t sink = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        Ubiquitty::Image::widenToRGBA8(rgb.data(), width, height, /*format=*/24, rgba.data());
        sink += rgba[0];
    });
    // Throughput over the OUTPUT RGBA bytes (the per-frame widen the upload pays);
    // higher-is-better, so compare-bench guards a regression. ms/frame = outBytes /
    // (value*2^20) * 1000 for the frame-budget read (16.7ms@60 / 8.3ms@120).
    const double outBytes = static_cast<double>(width) * static_cast<double>(height) * 4.0;
    out.push_back(Ubiquitty::Bench::throughput("kitty_upload_widen" + suffix, outBytes, timing));
    if (sink == 0xFFFFFFFFFFFFFFFFull) // defeat dead-store elimination
        std::fprintf(stderr, " ");
}

// An uncompressed f=32 image sent the way kitty's own clients send one: base64
// in 4096-character chunks, the id reused so each transfer replaces the last.
// The decoded payload becomes the stored image, so a transfer's peak holds the
// accumulated payload, the decoded pixels and the image being replaced; run the
// lane alone under a peak-memory reader to see it. Reported over decoded pixel
// bytes. q=2, because nothing here drains the replies.
void kittyRawTransmitBenches(std::vector<Ubiquitty::Bench::Result>& out, int width, int height, int warmup, int iters)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = static_cast<std::uint8_t>(index * 131 + (index >> 12));
    const std::string payload = base64Encode(pixels.data(), pixels.size());
    constexpr std::size_t chunkCharacters = 4096;
    std::string stream;
    for (std::size_t offset = 0; offset < payload.size(); offset += chunkCharacters) {
        if (offset == 0)
            stream += "\x1b_Ga=t,f=32,q=2,i=1,s=" + std::to_string(width) + ",v=" + std::to_string(height);
        else
            stream += "\x1b_Gq=2";
        stream += payload.size() - offset > chunkCharacters ? ",m=1;" : ",m=0;";
        stream.append(payload, offset, chunkCharacters);
        stream += "\x1b\\";
    }
    const std::string_view bytes(stream);

    // A transfer that stored nothing, or stored other bytes, would time a
    // refusal under this lane's name.
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, 48, 160);
    Ubiquitty::processBytes(term.get(), bytes);
    const auto stored = std::ranges::find_if(term->images.stored, [](const Ubiquitty::Image::TerminalImage& image) { return image.used && image.imageID == 1; });
    if (stored == term->images.stored.end() || !stored->pixels || stored->pixelLength != pixels.size()
        || std::memcmp(stored->pixels, pixels.data(), pixels.size()) != 0) {
        std::fprintf(stderr, "bench_stack: lane 'kitty_raw_transmit' did not store the image it sends.\n");
        std::exit(2);
    }
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] { Ubiquitty::processBytes(term.get(), bytes); });
    term.reset();
    out.push_back(Ubiquitty::Bench::throughput("kitty_raw_transmit", static_cast<double>(pixels.size()), timing));
}

// --scroll-lane: enable the deferred-state scroll lane on the parse lanes
// (the lanes the lane targets); other benches keep the shipping default.
static bool isBenchScrollLaneEnabled = false;

// Sync-burst lanes: K complete BSU/ESU alt-screen frames arrive in one
// processBytes chunk — the backlog shape a fast producer creates. Every frame
// carries distinct colored content (the write-side idempotency probes must not
// elide it), so a parser without elision pays full price per frame and the
// per-byte rate should be FLAT across K; a dead-frame elision would lift K>=2
// toward the cost of one full frame + (K-1) light scans. Frames are
// btop-shaped: home, 24 colored rows, no scroll (alt screen, no history
// capture).
static std::string syncBurstFrame(int rows, int columns, int frameIndex)
{
    std::string frame = "\x1b[?2026h\x1b[H";
    for (int r = 0; r < rows; ++r) {
        frame += "\x1b[38;5;" + std::to_string((frameIndex * 31 + r * 7) % 256) + "m";
        for (int c = 0; c < columns; ++c)
            frame += static_cast<char>('a' + (frameIndex + r + c) % 26);
        if (r != rows - 1)
            frame += "\r\n";
    }
    frame += "\x1b[?2026l";
    return frame;
}

void syncBurstBenches(std::vector<Ubiquitty::Bench::Result>& out, int warmup, int iters)
{
    for (int framesPerChunk : { 1, 2, 4, 8 }) {
        auto term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*term, 24, 80);
        static const char enterAlternateScreen[] = "\x1b[?1049h";
        Ubiquitty::processBytes(term.get(), std::string_view(enterAlternateScreen, sizeof(enterAlternateScreen) - 1));
        // One chunk = K distinct complete frames; successive iterations vary
        // the frame set so no iteration repaints identical cells.
        std::vector<std::string> chunks;
        for (int chunkIndex = 0; chunkIndex < 16; ++chunkIndex) {
            std::string chunk;
            for (int f = 0; f < framesPerChunk; ++f)
                chunk += syncBurstFrame(24, 80, chunkIndex * framesPerChunk + f);
            chunks.push_back(chunk);
        }
        std::size_t chunkBytes = chunks[0].size();
        int cursor = 0;
        const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
            Ubiquitty::processBytes(term.get(), chunks[cursor]);
            cursor = (cursor + 1) & 15;
        });
        term.reset();
        out.push_back(Ubiquitty::Bench::throughput("sync_burst_k" + std::to_string(framesPerChunk), static_cast<double>(chunkBytes), timing));
    }
}

// The seams a frontend drives between bytes, which the parse lanes above never
// reach.
//
// A body with no counts compiles cold under -fprofile-use rather than being
// left alone, and the parse lanes only feed bytes: they never move a pointer,
// never ask whether to withhold a frame, never drag a selection. Without these
// lanes `Input::`, `Modes::SyncOutput::` and `Selection::` — the paths a user
// exercises on every mouse move — would ship placed as if never executed.
//
// Rate, not saturation, is the point: a lane that hovers in a tight loop
// trains a distribution no user produces, and a profile that confidently
// mis-places code is worse than one that failed to help. Each lane runs a
// plausible count of events against a grid of realistic content rather than as
// many as the clock allows; the throughput figure in the JSON is a by-product
// so the harness has something to print, not the reason the lane exists. Judge
// them by whether a profile trained with them beats one trained without, on a
// workload neither was trained on.
// A lane whose body was optimised away reports a plausible number and passes
// every guard here: the trainer's required-lane list greps the lane's NAME out
// of the JSON, so a lane emitting nothing reads as present. A lane whose work
// is a pure function of constants over a local struct folds that way.
//
// So a lane asserts its own work: a sink the run moved, and a median the clock
// saw.
//
// That is a floor against a lane that plainly did nothing, not proof against
// folding: a folded body can still leave a non-zero sink and a small positive
// median. The anchor each lane holds — an opaque pointer, state read from the
// grid — is what actually keeps the body alive. Read a rate that changed by
// orders of magnitude as the folding signal; this function will not raise it
// for you.
//
// Exit 2 rather than 1: this is "the lane could not be measured", not "the
// measurement is bad", and it fails the trainer as well as the bench, which
// would otherwise merge counters from a lane that emitted none.
void requireLaneRan(const char* name, std::uint64_t sink, double medianSeconds)
{
    if (sink != 0 && medianSeconds > 0.0)
        return;
    std::fprintf(stderr,
        "bench_stack: lane '%s' produced no work (sink=%llu, median=%gs).\n"
        "  Its body was optimised away or ran below the clock. A profile trained\n"
        "  on this lane gains nothing from it. Anchor the work to something the\n"
        "  optimiser cannot fold — an opaque pointer, or state that escapes.\n",
        name, static_cast<unsigned long long>(sink), medianSeconds);
    std::exit(2);
}

Ubiquitty::Bench::Result pointerHoverRate(const char* name, int rows, int columns, int warmup, int iters)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);
    const std::vector<char> content = plainStream(static_cast<std::size_t>(rows) * columns * 4);
    Ubiquitty::processBytes(term.get(), std::string_view(content));

    // One sweep across the grid per iteration: what a pointer crossing a
    // terminal window actually produces, an event per cell rather than a
    // saturating loop.
    const int events = rows * columns;
    std::uint64_t sink = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                Ubiquitty::Input::PointerContext context;
                // A drag arms rectangular select on some fraction of moves; a
                // pointer past the last glyph on its row is over void. Both
                // ranks stay live rather than one branch carrying every event.
                context.isRectangularSelectArmed = ((row + column) & 7) == 0;
                context.isOverTextVoid = column >= columns - 2;
                sink += static_cast<std::uint64_t>(Ubiquitty::Input::pointerShape(term.get(), context));
            }
        }
    });
    term.reset();
    requireLaneRan(name, sink, timing.medianSeconds);
    return Ubiquitty::Bench::throughput(name, static_cast<double>(events), timing);
}

// The frame gate, asked once per frame by every frontend. A program inside a
// DECSET 2026 block withholds; outside it presents. Both answers matter — the
// withheld one is the branch a frontend takes while an application is
// mid-composition, which is when frames are most expensive to get wrong.
Ubiquitty::Bench::Result frameGateRate(const char* name, int rows, int columns, int warmup, int iters)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);

    // A run covering a couple of seconds at a 60 Hz frontend's cadence, the
    // unit a frame gate is actually asked in.
    const int frames = 120;
    std::uint64_t sink = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        for (int frame = 0; frame < frames; ++frame) {
            // A block opens and closes across the run rather than staying in
            // one state, so neither answer is the only one trained.
            const char* mode = (frame % 20 == 0) ? "\x1b[?2026h" : ((frame % 20 == 10) ? "\x1b[?2026l" : nullptr);
            if (mode)
                Ubiquitty::processBytes(term.get(), std::string_view(mode));
            sink += Ubiquitty::Modes::SyncOutput::shouldWithholdFrame(*term, Ubiquitty::Ticks(static_cast<std::uint32_t>(frame))) ? 1u : 0u;
        }
    });
    term.reset();
    requireLaneRan(name, sink, timing.medianSeconds);
    return Ubiquitty::Bench::throughput(name, static_cast<double>(frames), timing);
}

// A selection drag: begin, a run of updates as the pointer moves, then the
// bounds read a frontend needs to paint it. Without this lane `Selection::`
// has no profile entries, and a drag is one of the few paths that runs at
// pointer-motion rate while touching the grid.
Ubiquitty::Bench::Result selectionDragRate(const char* name, int rows, int columns, int warmup, int iters)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);
    const std::vector<char> content = plainStream(static_cast<std::size_t>(rows) * columns * 4);
    Ubiquitty::processBytes(term.get(), std::string_view(content));

    // Many drags per iteration, each the diagonal shape a user makes, so the
    // lane's total work clears the clock's resolution. Repeating a realistic
    // gesture is the fix for that; making one gesture span the whole grid
    // would train a drag nobody performs.
    const int dragsPerIteration = 512;
    const int steps = rows * dragsPerIteration;

    // The state is heap-allocated and the anchor is read from the grid rather
    // than a literal. A drag over a local struct at literal coordinates is a
    // pure function of constants, and ThinLTO folds the whole lane away; the
    // other two lanes above survive because each passes an opaque Terminal*,
    // and this one has no such anchor without the grid read. A lane that
    // trains nothing while still appearing in the output is the failure this
    // corpus exists to fix.
    auto state = std::make_unique<Ubiquitty::SelectionState>();
    Ubiquitty::Selection::init(state.get());
    std::uint64_t sink = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        for (int drag = 0; drag < dragsPerIteration; ++drag) {
            const Ubiquitty::Cell* seed = Ubiquitty::Screen::visibleCell(term.get(), 0, drag % rows, 0);
            const int startRow = seed ? static_cast<int>(seed->codePoint % static_cast<std::uint32_t>(rows)) : 0;
            Ubiquitty::Selection::begin(state.get(), rows, columns, startRow, 0, Ubiquitty::SelectionMode::Character);
            for (int step = 1; step < rows; ++step)
                Ubiquitty::Selection::update(state.get(), rows, columns, step, (step * columns) / rows);
            sink += static_cast<std::uint64_t>(state->headRow + state->headColumn);
            Ubiquitty::Selection::clear(state.get());
        }
    });
    state.reset();
    term.reset();
    requireLaneRan(name, sink, timing.medianSeconds);
    return Ubiquitty::Bench::throughput(name, static_cast<double>(steps), timing);
}

Ubiquitty::Bench::Result parseThroughput(const char* name, const std::vector<char>& stream, int rows, int columns, int warmup, int iters)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    term->isScrollLaneEnabled = isBenchScrollLaneEnabled;
    Ubiquitty::resize(*term, rows, columns);
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] { Ubiquitty::processBytes(term.get(), std::string_view(stream)); });
    term.reset();
    return Ubiquitty::Bench::throughput(name, static_cast<double>(stream.size()), timing);
}

// Feeds all of `stream` through processBytesUpToSyncClear, the flood pipeline's
// entry, resuming after each early return.
void feedUpToSyncClear(Ubiquitty::Terminal* term, const std::vector<char>& stream)
{
    int offset = 0;
    const int length = static_cast<int>(stream.size());
    while (offset < length) {
        const int consumed = Ubiquitty::processBytesUpToSyncClear(term, std::string_view(stream.data() + offset, static_cast<std::size_t>(length - offset)));
        offset += (consumed > 0) ? consumed : (length - offset); // guard: never spin
    }
}

// Same measurement through processBytesUpToSyncClear — the flood pipeline's
// entry, which instantiates processBytesImpl<true>, a SEPARATE copy from
// processBytes' <false>. Without a lane through it the PGO training corpus
// never executes the instantiation the production flood runs, and that copy
// ships unprofiled.
Ubiquitty::Bench::Result parseThroughputSyncFeed(const char* name, const std::vector<char>& stream, int rows, int columns, int warmup, int iters)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    term->isScrollLaneEnabled = isBenchScrollLaneEnabled;
    Ubiquitty::resize(*term, rows, columns);
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] { feedUpToSyncClear(term.get(), stream); });
    term.reset();
    return Ubiquitty::Bench::throughput(name, static_cast<double>(stream.size()), timing);
}

// ---- SGR while a scroll run is open ----------------------------------------
//
// The deferred scroll lanes ship enabled (`RuntimeInitParameters::
// scrollLaneEnabled`), while the parse lanes above run with them off unless
// `--scroll-lane` is given, and never with the band lane. So by default no lane
// above opens a run, and the parser's handling of a control sequence that
// arrives while one is open runs nowhere. These lanes enable both lanes and run
// the same text through three shapes of the primary screen:
//
//   full    The whole screen scrolls. Each LF at the bottom opens or joins the
//           full-width run, `Terminal::scrollLaneRun`.
//   band    A DECSTBM region narrowed by DECSLRM margins, a pane beside
//           another. Each LF at the region's bottom opens or joins the band
//           run, `Terminal::bandScrollRun`.
//   region  A DECSTBM region at full width. Neither lane owns that shape, so no
//           run opens and every scroll takes the immediate path: the control
//           for the other two, on the same bytes through the same parser.
//
// The text is a man page and a coloured log. An attribute or a reset leaves
// the erase blank alone, so a run stays open across it; a colour changes the
// blank, so the run commits at the line's first colour and reopens at its LF.
// Both kinds reach the parser with a run open. Every line fits the band, so no
// wrap moves the cursor off the region's bottom.
constexpr std::string_view scrollingSGRLines = "\x1b[1mNAME\x1b[0m\r\n"
                                               "  \x1b[1mubq\x1b[22m \x1b[2m-\x1b[22m a \x1b[4mterminal\x1b[24m\r\n"
                                               "\x1b[1;4mSYNOPSIS\x1b[0m\r\n"
                                               "  \x1b[1mubq\x1b[22m [\x1b[4mOPTION\x1b[24m]... [\x1b[3mFILE\x1b[23m]\r\n"
                                               "\x1b[38;5;244m09:14:07\x1b[39m \x1b[32mINFO\x1b[39m read 4096 B\r\n"
                                               "\x1b[38;2;220;50;47mERROR\x1b[39m \x1b[1mparse\x1b[22m: bad \x1b[4mfinal\x1b[24m\r\n"
                                               "\x1b[33mWARN\x1b[39m  \x1b[2mretry in 250 ms\x1b[22m\r\n";

std::vector<char> scrollingSGRStream(std::size_t bytes) { return repeatTo(std::string(scrollingSGRLines), bytes); }

enum class ScrollShape : std::uint8_t {
    Full,
    Band,
    Region,
};

enum class ParseEntry : std::uint8_t {
    ProcessBytes,
    UpToSyncClear,
};

// The bytes that shape the scrolling area and leave the cursor on its bottom
// row, so the stream's first LF already scrolls. The band is the left half of
// the screen; both regions stop a row short of the bottom, a status line's row.
std::string scrollShapeSetup(ScrollShape shape, int rows, int columns)
{
    const std::string regionBottom = std::to_string(rows - 1);
    switch (shape) {
    case ScrollShape::Full:
        return "\x1b[" + std::to_string(rows) + ";1H";
    case ScrollShape::Band:
        return "\x1b[1;" + regionBottom + "r\x1b[?69h\x1b[1;" + std::to_string(columns / 2) + "s\x1b[" + regionBottom + ";1H";
    case ScrollShape::Region:
        return "\x1b[1;" + regionBottom + "r\x1b[" + regionBottom + ";1H";
    }
    return {};
}

// Whether the terminal holds exactly the run its shape opens: the full-width
// run, the band run, or none.
bool hasShapeRun(const Ubiquitty::Terminal& term, ScrollShape shape)
{
    switch (shape) {
    case ScrollShape::Full:
        return term.scrollLaneRun.active && !term.bandScrollRun.active;
    case ScrollShape::Band:
        return term.bandScrollRun.active && !term.scrollLaneRun.active;
    case ScrollShape::Region:
        return !term.scrollLaneRun.active && !term.bandScrollRun.active;
    }
    return false;
}

// A lane that silently fell back to the immediate path would time the control
// under the subject's name, so each one proves its shape first. The flood
// entry keeps a run open across an input-exhausted return, which is what lets
// the run be read after one pass of the text.
Ubiquitty::Bench::Result scrollingSGRThroughput(const std::string& name, const std::vector<char>& stream, ScrollShape shape, ParseEntry entry, int rows, int columns, int warmup, int iters)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    term->isScrollLaneEnabled = true;
    term->isBandScrollLaneEnabled = true;
    Ubiquitty::resize(*term, rows, columns);
    Ubiquitty::processBytes(term.get(), scrollShapeSetup(shape, rows, columns));
    const int consumed = Ubiquitty::processBytesUpToSyncClear(term.get(), scrollingSGRLines);
    if (consumed != static_cast<int>(scrollingSGRLines.size()) || !hasShapeRun(*term, shape)) {
        std::fprintf(stderr,
            "bench_stack: lane '%s' does not hold the scroll run its shape opens.\n"
            "  It would time another path under this lane's name.\n",
            name.c_str());
        std::exit(2);
    }
    Ubiquitty::termFlushScrollLane(*term);
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        if (entry == ParseEntry::UpToSyncClear)
            feedUpToSyncClear(term.get(), stream);
        else
            Ubiquitty::processBytes(term.get(), std::string_view(stream.data(), stream.size()));
    });
    term.reset();
    return Ubiquitty::Bench::throughput(name, static_cast<double>(stream.size()), timing);
}

std::unique_ptr<Ubiquitty::Terminal> makeFilledTerm(int rows, int columns)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);
    // Fill the screen with mixed text + a little SGR so a checkpoint carries a
    // representative payload (not a blank grid).
    const std::vector<char> fill = repeatTo("\x1b[1;36mfile\x1b[0m.cpp:42  const auto value = compute(x, y);  // note\r\n", static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns) * 4);
    Ubiquitty::processBytes(term.get(), std::string_view(fill));
    return term;
}

// ---- the screen plane's checkpoint ------------------------------------------
// What an attach or a resync costs: the host encodes a checkpoint of the frame
// it captured, and the client applies it. Reported as throughput over the
// body, so both halves compare. The encode lane times the encode alone; the
// capture that precedes it is the per-frame lane below.
void checkpointBenchesOf(std::vector<Ubiquitty::Bench::Result>& out, const Ubiquitty::Terminal& host, const std::string& encodeName, const std::string& applyName, int warmup, int iters)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    Sync::ScreenPublisher publisher;
    Sync::AttachmentBaseline attachment;
    std::vector<std::uint8_t> body;
    if (!publisher.capture(host) || !publisher.checkpoint(host, 1, body, attachment)) {
        std::fprintf(stderr, "bench: %s could not encode a checkpoint of its terminal\n", encodeName.c_str());
        std::exit(1);
    }
    const double bodyBytes = static_cast<double>(body.size());

    // Cleared, not reconstructed, as the host's scratch is: a lane building a
    // fresh vector each iteration pays the geometric growth every time.
    std::vector<std::uint8_t> encoded;
    const auto encodeTiming = Ubiquitty::Bench::measure(warmup, iters, [&] {
        encoded.clear();
        Sync::AttachmentBaseline scratch;
        (void)publisher.checkpoint(host, 1, encoded, scratch);
    });
    out.push_back(Ubiquitty::Bench::throughput(encodeName, bodyBytes, encodeTiming));
    if (applyName.empty())
        return;

    // An apply that refused would time the refusal, so the outcome is read
    // once before the clock runs.
    auto client = std::make_unique<Ubiquitty::Terminal>();
    Sync::ScreenReplica replica;
    if (Sync::applyCheckpoint(*client, body, replica) != Sync::ApplyOutcome::Applied) {
        std::fprintf(stderr, "bench: %s refused the checkpoint it is timing\n", applyName.c_str());
        std::exit(1);
    }
    const auto applyTiming = Ubiquitty::Bench::measure(warmup, iters, [&] { (void)Sync::applyCheckpoint(*client, body, replica); });
    out.push_back(Ubiquitty::Bench::throughput(applyName, bodyBytes, applyTiming));
}

void checkpointBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters, const std::string& suffix = "")
{
    auto host = makeFilledTerm(rows, columns);
    checkpointBenchesOf(out, *host, "checkpoint_encode" + suffix, "checkpoint_apply" + suffix, warmup, iters);
}

// A checkpoint taken while the alternate screen is up, which is what an attach
// to a session running vim, less or tmux is. The encode then writes the frozen
// primary grid as well as the live alternate one.
void alternateCheckpointBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters)
{
    auto host = makeFilledTerm(rows, columns);
    static const char enterAlternateScreen[] = "\x1b[?1049h";
    Ubiquitty::processBytes(host.get(), std::string_view(enterAlternateScreen, sizeof(enterAlternateScreen) - 1));
    // Fill the alternate grid too, so neither screen is a page of blanks.
    for (int r = 0; r < rows; ++r) {
        std::string line;
        for (int c = 0; c < columns; ++c)
            line += static_cast<char>('!' + ((r * 7 + c) % 94));
        Ubiquitty::processBytes(host.get(), line);
        if (r + 1 < rows)
            Ubiquitty::processBytes(host.get(), std::string_view("\r\n", 2));
    }
    checkpointBenchesOf(out, *host, "checkpoint_encode_alt", "", warmup, iters);
}

// ---- the daemon's per-frame publish, end to end ----------------------------
// The frame shape is the one a session spends its life in: one keystroke
// lands, one row is dirty, and the capture still walks every row to find that
// out before the update for an attachment is built. The lane is sized by the
// cells it walks rather than the cells it ships, because the walk is the part
// that does not shrink when the change does.
void broadcastFrameBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters, const std::string& suffix = "")
{
    namespace Sync = Ubiquitty::Protocol::Sync;

    auto host = makeFilledTerm(rows, columns);
    Ubiquitty::Terminal& t = *host;

    Sync::ScreenPublisher publisher;
    Sync::AttachmentBaseline attachment;
    std::vector<std::uint8_t> body;
    if (!publisher.capture(t) || !publisher.checkpoint(t, 1, body, attachment)) {
        std::fprintf(stderr, "bench: broadcast_frame%s could not encode its first checkpoint\n", suffix.c_str());
        std::exit(1);
    }

    int typedColumn = 0;
    int framesCarryingUpdate = 0;
    const auto oneFrame = [&] {
        // One cell changes, as a keystroke would, rotating so the grid neither
        // grows nor settles into a frame identical to the last.
        typedColumn = (typedColumn + 1) % columns;
        Ubiquitty::Cell typed = *t.grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(typedColumn));
        typed.codePoint = static_cast<std::uint32_t>('a' + (typedColumn % 26));
        t.grid->setCell(Ubiquitty::Row(0), Ubiquitty::Column(typedColumn), typed);
        if (!publisher.capture(t))
            return;
        body.clear();
        if (publisher.rowUpdate(t, attachment, body) == Sync::UpdateOutcome::Encoded && !body.empty())
            ++framesCarryingUpdate;
    };

    // The first captures fill the row caches, which is the attach path rather
    // than the one a session spends its life on.
    for (int prime = 0; prime < 16; ++prime)
        oneFrame();
    framesCarryingUpdate = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, oneFrame);
    // A lane that ships no update measures the publisher refusing, not the
    // broadcast. Counted over the measured window rather than assumed.
    if (framesCarryingUpdate == 0) {
        std::fprintf(stderr, "bench: broadcast_frame%s shipped no update in %d frames — the lane is measuring a refusal, not the broadcast\n",
            suffix.c_str(), iters);
        std::exit(1);
    }

    const double cells = static_cast<double>(rows) * static_cast<double>(columns);
    Ubiquitty::Bench::Result r;
    r.name = "broadcast_frame" + suffix;
    r.unit = "Mcell/s";
    r.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
    r.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
    out.push_back(r);
    host.reset();
}

// ---- the client's update apply ----------------------------------------------
// The steady-state remote hot path on the client: an update of two dirty rows
// applied over the checkpoint it extends. Each iteration starts from the
// replica the checkpoint left, so every apply is of the same update; the copy
// of that replica — its pens and both screens' row hashes — is inside the lane.
void updateBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    auto host = makeFilledTerm(rows, columns);

    Sync::ScreenPublisher publisher;
    Sync::AttachmentBaseline attachment;
    std::vector<std::uint8_t> checkpoint;
    auto client = std::make_unique<Ubiquitty::Terminal>();
    Sync::ScreenReplica base;
    if (!publisher.capture(*host) || !publisher.checkpoint(*host, 1, checkpoint, attachment)
        || Sync::applyCheckpoint(*client, checkpoint, base) != Sync::ApplyOutcome::Applied) {
        std::fprintf(stderr, "bench: update_apply could not seed its client with a checkpoint\n");
        std::exit(1);
    }

    const std::string change = "\x1b[" + std::to_string(rows / 2 + 1) + ";1Hchanged row\x1b[" + std::to_string(rows) + ";1Hanother changed row";
    Ubiquitty::processBytes(host.get(), change);
    std::vector<std::uint8_t> update;
    if (!publisher.capture(*host) || publisher.rowUpdate(*host, attachment, update) != Sync::UpdateOutcome::Encoded || update.empty()) {
        std::fprintf(stderr, "bench: update_apply could not encode the update it is timing\n");
        std::exit(1);
    }

    int refused = 0;
    Sync::ScreenReplica replica;
    const auto applyTiming = Ubiquitty::Bench::measure(warmup, iters, [&] {
        replica = base;
        if (Sync::applyRowUpdate(*client, update, replica) != Sync::ApplyOutcome::Applied)
            ++refused;
    });
    if (refused != 0) {
        std::fprintf(stderr, "bench: update_apply refused %d of its applies — the lane is timing a refusal\n", refused);
        std::exit(1);
    }
    out.push_back(Ubiquitty::Bench::throughput("update_apply", static_cast<double>(update.size()), applyTiming));
}

// Counts inbound bytes off a Channel's I/O thread; the bench thread waits for a
// target count (one round-trip through the loopback transport).
class CountingObserver final : public Ubiquitty::Transport::ChannelObserver {
public:
    void onConnect() override
    {
        std::scoped_lock lock(m_mutex);
        m_connected = true;
        m_cv.notify_all();
    }
    void onData(std::span<const std::uint8_t> data) override
    {
        std::scoped_lock lock(m_mutex);
        m_received += data.size();
        m_cv.notify_all();
    }
    void onClose(const std::string&) override
    {
        std::scoped_lock lock(m_mutex);
        m_closed = true;
        m_cv.notify_all();
    }
    void waitConnected()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_connected || m_closed; });
    }
    // Wait for the CUMULATIVE received count to reach `target` (never reset — a
    // per-op reset would race the streaming pipe and satisfy the wait instantly).
    void waitFor(std::size_t target)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_received >= target || m_closed; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::size_t m_received = 0;
    bool m_connected = false;
    bool m_closed = false;
};

// End-to-end loopback throughput through the portable SubprocessChannel: pipe →
// `cat` → pipe → the poll thread. Each timed op sends `payloadBytes` and waits
// for them to echo back (real syscalls + threads, so noisier than the pure-CPU
// benches — a handful of iterations + the median absorbs it).
void channelLoopbackBench(std::vector<Ubiquitty::Bench::Result>& out, std::size_t payloadBytes, int warmup, int iters)
{
    Ubiquitty::Transport::SubprocessChannel channel("cat", { Ubiquitty::Transport::OriginKind::Daemon, "" });
    CountingObserver observer;
    if (!channel.start(&observer))
        return; // cat unavailable — skip rather than emit a bogus number
    observer.waitConnected();

    const std::vector<std::uint8_t> payload(payloadBytes, 'x');
    // Cumulative round-trips: each iteration sends one payload and waits for the
    // running total to come back, so iteration timing is one clean round trip.
    std::size_t cumulative = 0;
    std::vector<double> samples;
    for (int i = 0; i < warmup + iters; ++i) {
        const auto start = std::chrono::steady_clock::now();
        channel.send(payload);
        cumulative += payloadBytes;
        observer.waitFor(cumulative);
        const auto end = std::chrono::steady_clock::now();
        if (i >= warmup)
            samples.push_back(std::chrono::duration<double>(end - start).count());
    }
    channel.stop();
    if (samples.empty())
        return;
    std::ranges::sort(samples);
    Ubiquitty::Bench::Timing timing;
    timing.medianSeconds = samples[samples.size() / 2];
    timing.p95Seconds = samples[std::min(samples.size() - 1, static_cast<std::size_t>(samples.size() * 0.95))];
    out.push_back(Ubiquitty::Bench::throughput("channel_loopback", static_cast<double>(payloadBytes), timing));
}

// Scrollback (LineStore) — the history-ingest + history-scan hot paths, distinct
// from the VT parser. Reported as throughput over the cell bytes moved, so both
// numbers compare against the rest of the stack (higher is better).
//
// scrollback_push: append `batch` rows into a store already filled to capacity, so
// each append evicts the oldest line and reuses a slab — the steady-state cost a
// flooding shell pays as rows age into history.
// scrollback_scan: linearly read every retained line's cells (the per-pass cost a
// scrollback substring search pays), summing into a sink the optimiser can't drop.
void scrollbackBenches(std::vector<Ubiquitty::Bench::Result>& out, int columns, int warmup, int iters)
{
    const int capacity = Ubiquitty::VT::fallbackScrollbackLines;
    const int rowCells = columns;
    const auto cellBytes = static_cast<double>(sizeof(Ubiquitty::Cell));

    // One representative row of content (mixed printable codePoints).
    std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(rowCells));
    for (int i = 0; i < rowCells; ++i) {
        row[static_cast<std::size_t>(i)] = Ubiquitty::Cell {};
        row[static_cast<std::size_t>(i)].codePoint = static_cast<uint32_t>('a' + (i % 26));
    }

    Ubiquitty::LineStore store;
    store.init(capacity);
    for (int i = 0; i < capacity; ++i)
        store.append(row.data(), rowCells, 0, 0, false, false);

    // Push: each timed op appends `batch` rows (each evicting the oldest).
    const int batch = 5000;
    uint32_t tag = 0;
    const auto pushTiming = Ubiquitty::Bench::measure(warmup, iters, [&] {
        for (int i = 0; i < batch; ++i) {
            row[0].codePoint = 0x100u + (++tag & 0xffffu); // keep each append distinct
            store.append(row.data(), rowCells, 0, 0, false, false);
        }
    });
    const double pushBytes = static_cast<double>(batch) * rowCells * cellBytes;
    out.push_back(Ubiquitty::Bench::throughput("scrollback_push", pushBytes, pushTiming));

    // Scan: read every retained line's cells once per op (a find pass).
    volatile uint32_t sink = 0;
    const auto scanTiming = Ubiquitty::Bench::measure(warmup, iters * 4, [&] {
        uint32_t acc = 0;
        const int count = store.count();
        for (int line = 0; line < count; ++line) {
            const Ubiquitty::StoredLine* stored = store.at(line);
            for (int i = 0; i < stored->length; ++i)
                acc += stored->cells[i].codePoint;
        }
        sink = acc;
    });
    const double scanBytes = static_cast<double>(store.count()) * rowCells * cellBytes;
    out.push_back(Ubiquitty::Bench::throughput("scrollback_scan", scanBytes, scanTiming));

    store.dispose();
}

// Wire-byte SIZE of each frame kind for representative edits — quantifies the
// sub-row span and size-gated compression bandwidth wins. These
// are exact sizes, not timed; reported in bytes (lower is better) so the trend
// shows a single keystroke's cost collapse from a full keyframe to a few bytes.
// ---- styled clipboard -------------------------------------------------
// A macOS copy builds three flavours in one pass — plain, RTF and HTML — and
// only the plain walk had a lane. The rich ones emit per-cell markup, so what
// they add to a copy is a different question from what the walk costs, and the
// answer decides whether Cmd-C is the plain number or a multiple of it.
//
// The callbacks here are the bench's own because the shipping ones live in each
// frontend. They do what those do — resolve the cell, resolve its pen through
// the palette — so the builder is measured against representative input rather
// than against a stub that hands it constants.
struct StyledBenchContext {
    const Ubiquitty::Terminal* term;
    Ubiquitty::CellPalette palette;
    // The row last resolved, as every frontend's styled callback keeps. A
    // callback here without one would measure a shape no frontend ships.
    mutable int memoRow = -1;
    mutable const Ubiquitty::Cell* memoBase = nullptr;
    mutable int memoLength = 0;
};

const Ubiquitty::Cell* styledBenchCell(const StyledBenchContext* ctx, int row, int column)
{
    if (row != ctx->memoRow) {
        ctx->memoRow = row;
        ctx->memoBase = Ubiquitty::Screen::absoluteRowCells(ctx->term, row, &ctx->memoLength);
    }
    return (ctx->memoBase && column >= 0 && column < ctx->memoLength) ? &ctx->memoBase[column] : nullptr;
}

uint32_t styledBenchCellAt(void* context, int row, int column)
{
    const auto* ctx = static_cast<const StyledBenchContext*>(context);
    const Ubiquitty::Cell* cell = styledBenchCell(ctx, row, column);
    return cell ? cell->codePoint : 0;
}

int styledBenchRowExtent(void* context, int row)
{
    const auto* ctx = static_cast<const StyledBenchContext*>(context);
    return Ubiquitty::Screen::absoluteRowExtent(ctx->term, row);
}

int styledBenchRowWrapped(void* context, int row)
{
    const auto* ctx = static_cast<const StyledBenchContext*>(context);
    return Ubiquitty::Screen::absoluteRowWrapped(ctx->term, row) ? 1 : 0;
}

Ubiquitty::Selection::TextCell styledBenchTextCellAt(void* context, int row, int column)
{
    const auto& reader = *static_cast<const StyledBenchContext*>(context);
    const Ubiquitty::Cell* cell = styledBenchCell(&reader, row, column);
    Ubiquitty::Selection::TextCell text;
    text.codePoint = cell ? cell->codePoint : 0;
    text.isContinuation = cell && Ubiquitty::isTextContinuation(*cell);
    text.codePointCount = cell ? Ubiquitty::cellClusterCodePoints(reader.term, cell, text.codePoints.data(), static_cast<int>(text.codePoints.size())) : 0;
    return text;
}

Ubiquitty::Selection::StyledCell styledBenchStyledCellAt(void* context, int row, int column)
{
    const auto* ctx = static_cast<const StyledBenchContext*>(context);
    const Ubiquitty::Cell* cell = styledBenchCell(ctx, row, column);
    if (!cell)
        return Ubiquitty::Selection::StyledCell {};
    const Ubiquitty::Pen& pen = Ubiquitty::cellPen(*cell);
    uint8_t foreground[3];
    uint8_t background[3];
    ctx->palette.resolve(pen, Ubiquitty::ReverseMode::Normal, foreground, background);
    Ubiquitty::Selection::StyledCell styled;
    styled.codePoint = cell->codePoint;
    styled.isContinuation = Ubiquitty::isTextContinuation(*cell);
    for (int i = 0; i < 3; ++i) {
        styled.foreground[i] = foreground[i];
        styled.background[i] = background[i];
    }
    styled.bold = pen.attributes.contains(Ubiquitty::VT::Attribute::Bold);
    styled.italic = pen.attributes.contains(Ubiquitty::VT::Attribute::Italic);
    styled.underline = pen.attributes.contains(Ubiquitty::VT::Attribute::Underline);
    styled.codePointCount = Ubiquitty::cellClusterCodePoints(ctx->term, cell, styled.codePoints.data(), static_cast<int>(styled.codePoints.size()));
    return styled;
}

void styledClipboardBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int lines, int warmup, int iters, const std::string& suffix)
{
    namespace Sel = Ubiquitty::Selection;
    auto host = std::make_unique<Ubiquitty::Terminal>();
    host->scrollbackLimit = lines + rows;
    Ubiquitty::resize(*host, rows, columns);

    // Coloured, because the rich flavours emit a span per colour change and a
    // monochrome buffer would measure the case they are cheapest on.
    std::string feed;
    feed.reserve(static_cast<std::size_t>(lines) * 90);
    for (int line = 0; line < lines; ++line) {
        feed += "\x1b[38;5;" + std::to_string(line % 256) + "m2026-08-14 19:42:01 \x1b[1mINFO\x1b[0m  worker processed 128 records\r\n";
    }
    Ubiquitty::processBytes(host.get(), std::string_view(feed));

    StyledBenchContext ctx { host.get(), {} };
    ctx.palette.seedDefaults();
    ctx.palette.fillExtended();
    const int total = Ubiquitty::Screen::scrollbackCount(host.get()) + rows;
    Ubiquitty::SelectionBounds bounds { 0, 0, total - 1, columns - 1 };
    const Sel::StyledFont font { "SF Mono", 13.0 };
    const std::array<std::uint8_t, 3> defaultForeground { 200, 200, 200 };
    const std::array<std::uint8_t, 3> defaultBackground { 0, 0, 0 };

    std::size_t produced = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        const Sel::StyledClipboard built = Sel::buildStyledClipboard(&bounds, Ubiquitty::SelectionMode::Character, columns,
            styledBenchCellAt, &ctx, styledBenchTextCellAt, &ctx, styledBenchStyledCellAt, &ctx, Sel::SoftWrapNewline::Join, styledBenchRowWrapped, &ctx,
            Sel::StyleColorMode::Full, font, defaultForeground, defaultBackground, styledBenchRowExtent, &ctx);
        produced = built.plain.size() + built.rtf.size() + built.html.size();
    });
    Ubiquitty::Bench::doNotOptimize(produced);

    const double cells = static_cast<double>(lines) * static_cast<double>(columns);
    Ubiquitty::Bench::Result r;
    r.name = "styled_clipboard_all" + suffix;
    r.unit = "Mcell/s";
    r.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
    r.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
    out.push_back(r);

    // What a copy costs once the rich flavors are promised rather than written: the walk
    // alone, which is what has to happen while the grid still is the one selected from.
    // The gap to the lane above is what a paste wanting only the string stops paying.
    std::size_t walkedSize = 0;
    const auto walkTiming = Ubiquitty::Bench::measure(warmup, iters, [&] {
        const Sel::StyledRuns walked = Sel::buildStyledRuns(&bounds, Ubiquitty::SelectionMode::Character, columns,
            styledBenchCellAt, &ctx, styledBenchTextCellAt, &ctx, styledBenchStyledCellAt, &ctx, Sel::SoftWrapNewline::Join, styledBenchRowWrapped, &ctx,
            Sel::StyleColorMode::Full, font, defaultForeground, defaultBackground, styledBenchRowExtent, &ctx);
        walkedSize = walked.plain.size() + walked.runs.size();
    });
    Ubiquitty::Bench::doNotOptimize(walkedSize);
    Ubiquitty::Bench::Result walkResult;
    walkResult.name = "styled_walk_all" + suffix;
    walkResult.unit = "Mcell/s";
    walkResult.value = walkTiming.medianSeconds > 0.0 ? (cells / walkTiming.medianSeconds) / 1.0e6 : 0.0;
    walkResult.p95 = walkTiming.p95Seconds > 0.0 ? (cells / walkTiming.p95Seconds) / 1.0e6 : 0.0;
    out.push_back(walkResult);
    host.reset();
}

// ---- select-all and copy ---------------------------------------------
// Cmd-A then Cmd-C over a session's worth of scrollback. selectedText walks the
// selection cell by cell through the cluster rules and the soft-wrap newline
// rule, so it scales with the buffer rather than with the screen, and a user
// reaches for it on exactly the buffers that are large.
void selectionBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int lines, int warmup, int iters, const std::string& suffix)
{
    namespace Selection = Ubiquitty::Selection;
    auto host = std::make_unique<Ubiquitty::Terminal>();
    host->scrollbackLimit = lines + rows;
    Ubiquitty::resize(*host, rows, columns);

    std::string feed;
    feed.reserve(static_cast<std::size_t>(lines) * 80);
    for (int line = 0; line < lines; ++line)
        feed += "2026-08-14 19:42:01 INFO  worker[3] processed batch of 128 records in 42ms\r\n";
    Ubiquitty::processBytes(host.get(), std::string_view(feed));

    Selection::Session session;
    Selection::selectAllText(session, *host);

    const double cells = static_cast<double>(lines) * static_cast<double>(columns);
    std::size_t produced = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        produced = Selection::selectedText(session, *host).size();
    });
    Ubiquitty::Bench::doNotOptimize(produced);
    Ubiquitty::Bench::Result r;
    r.name = "selection_copy_all" + suffix;
    r.unit = "Mcell/s";
    r.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
    r.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
    out.push_back(r);

    host.reset();
}

// ---- scrollback search ----------------------------------------------
// A complete sliced search over retained history and the live screen.
void searchBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int lines, int warmup, int iters, const std::string& suffix)
{
    auto host = std::make_unique<Ubiquitty::Terminal>();
    host->scrollbackLimit = lines + rows; // set before the store is created lazily
    Ubiquitty::resize(*host, rows, columns);

    // Log-shaped text, because that is what a scrollback holds when someone
    // reaches for find. One line in a thousand carries the needle, so the scan
    // reports matches without the match count becoming the thing being timed.
    std::string feed;
    feed.reserve(static_cast<std::size_t>(lines) * 80);
    for (int line = 0; line < lines; ++line) {
        feed += (line % 1000 == 999)
            ? "2026-08-14 19:42:01 ERROR worker[3] needle: batch rejected after 42ms\r\n"
            : "2026-08-14 19:42:01 INFO  worker[3] processed batch of 128 records in 42ms\r\n";
    }
    Ubiquitty::processBytes(host.get(), std::string_view(feed));

    std::mutex modelLock;
    Ubiquitty::Search::SearchController search(*host, modelLock);
    const Ubiquitty::Search::Request request { .pattern = "needle", .sensitivity = Ubiquitty::CaseSensitivity::Sensitive, .normalization = Ubiquitty::Screen::ScrollSearch::normalizationForm() };
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        auto status = search.searchAndWait(request, std::chrono::seconds(30));
        if (!status.isComplete || status.isPatternInvalid) {
            std::fputs("search benchmark did not complete\n", stderr);
            std::exit(2);
        }
        Ubiquitty::Bench::doNotOptimize(status.foundCount);
    });
    Ubiquitty::Bench::Result result;
    result.name = "search_full" + suffix;
    result.unit = "MB/s";
    result.value = timing.medianSeconds > 0.0 ? static_cast<double>(feed.size()) / timing.medianSeconds / 1.0e6 : 0.0;
    result.p95 = timing.p95Seconds > 0.0 ? static_cast<double>(feed.size()) / timing.p95Seconds / 1.0e6 : 0.0;
    out.push_back(result);
    search.detach();

    host.reset();
}

// The bytes a screen costs on the wire: a checkpoint, then the update for a
// one-cell edit, a cursor move, and a repaint of every row, each over the
// frame before it.
void bandwidthBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    (void)warmup;
    (void)iters;
    const auto bytesResult = [](const std::string& name, std::size_t bytes) {
        Ubiquitty::Bench::Result r;
        r.name = name;
        r.unit = "bytes";
        r.value = static_cast<double>(bytes);
        r.p95 = static_cast<double>(bytes);
        return r;
    };

    auto host = makeFilledTerm(rows, columns);
    Sync::ScreenPublisher publisher;
    Sync::AttachmentBaseline attachment;
    std::vector<std::uint8_t> body;
    if (!publisher.capture(*host) || !publisher.checkpoint(*host, 1, body, attachment)) {
        std::fprintf(stderr, "bench: bytes_checkpoint could not encode its checkpoint\n");
        std::exit(1);
    }
    // A full checkpoint: the cost a client pays on attach or resync.
    out.push_back(bytesResult("bytes_checkpoint", body.size()));

    const auto updateBytes = [&](const char* name, const std::string& change) {
        Ubiquitty::processBytes(host.get(), change);
        body.clear();
        if (!publisher.capture(*host) || publisher.rowUpdate(*host, attachment, body) != Sync::UpdateOutcome::Encoded) {
            std::fprintf(stderr, "bench: %s could not encode its update\n", name);
            std::exit(1);
        }
        out.push_back(bytesResult(name, body.size()));
    };

    const int editRow = rows / 2 + 1;
    updateBytes("bytes_update_1edit", "\x1b[" + std::to_string(editRow) + ";4Hx");
    updateBytes("bytes_update_cursoronly", "\x1b[" + std::to_string(editRow) + ";9H");
    std::string repaint;
    for (int r = 0; r < rows; ++r) {
        repaint += "\x1b[" + std::to_string(r + 1) + ";1H";
        repaint.append(static_cast<std::size_t>(columns), static_cast<char>('A' + (r % 26)));
    }
    updateBytes("bytes_update_repaint", repaint);

    host.reset();
}

// ---- render-build microbench ---------------------------------------
// The per-frame cost the renderer pays turning a Cell grid into GPU instances:
// the portable buildCellInstances() pass (color resolve + glyph-slot lookup +
// decoration geometry). Driven over a representative screen against a
// deterministic fake atlas — no GPU, no platform — so it's stable + comparable
// across runners like every other harness bench.
//
// What these numbers do not cover. buildCellInstances writes eight instance
// streams and these lanes wire two, or three in the subpixel one: cells,
// decorations, and glyphs. Overlays, wide glyphs, colour glyphs, ligature
// glyphs and box fills are left null, and the builder skips a stream whose
// buffer is absent — so a pass costing nothing here may cost plenty shipped.
//
// Overlays is the sharpest of those: every shipping frontend wires it, and no
// lane here does. The scene compounds it — no cursor (Rendering::CursorRenderState::row
// defaults to -1) and no selection spans, so the overlay pass would emit
// nothing even if the buffer were supplied.
//
// So a delta measured here is a delta on the subset below, not on the builder a
// frontend runs. Say which when quoting it.
using namespace Ubiquitty::Rendering;

struct RenderFakeAtlas {

    int nextSlot = 0;
    // A real cache, because the bench measures the builder's mono path and a
    // fake reporting none would time it skipping that path entirely.
    static constexpr int atlasColumns = 32;
    static constexpr int atlasCapacity = atlasColumns * 32;
    GlyphSlotCache cache { atlasCapacity, recommendedHashBuckets(atlasCapacity), atlasColumns };
    GlyphSlotGeometry slotGeometry = [] {
        GlyphSlotGeometry geometry {};
        geometry.columns = atlasColumns;
        geometry.slotWidthInPixels = 8;
        geometry.slotHeightInPixels = 16;
        geometry.glyphWidthInPixels = 6;
        geometry.glyphMarginInPixels.width = 1;
        geometry.glyphMarginInPixels.height = 1;
        return geometry;
    }();
    MonoAtlasBinding bindMonoAtlas()
    {
        MonoAtlasBinding binding;
        binding.cache = &cache;
        binding.geometry = slotGeometry;
        binding.slotCapacity = atlasCapacity;
        binding.weight = 0u;
        return binding;
    }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }
    void beginFrame() { cache.beginFrame(); }
    AtlasGlyph monoGlyph(std::uint32_t codePoint, FontFace, std::uint32_t, std::uint32_t) { return assign(codePoint); }
    AtlasGlyph wideGlyph(std::uint32_t codePoint, FontFace) { return assign(codePoint); }
    AtlasGlyph doubleGlyph(std::uint32_t codePoint, FontFace, DoubleRegion) { return assign(codePoint); }
    AtlasGlyph assign(std::uint32_t codePoint)
    {
        if (codePoint == 0x20u)
            return {}; // space: no coverage (blank-glyph path)
        const float u = static_cast<float>(nextSlot++ & 0x3FFF) * 0.00001f;
        return { { u, u }, { u + 0.02f, u + 0.02f }, true };
    }
};

// A deterministic, representative screen: printable ASCII with a sprinkle of
// bold/underline/reverse and ANSI colors, plus every ~17th cell a wide CJK pair.
std::vector<Ubiquitty::Cell> makeRenderGrid(int rows, int columns)
{
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    Ubiquitty::Cell blank = Ubiquitty::defaultBlankCell;
    for (auto& cell : grid)
        cell = blank;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns;) {
            const int idx = r * columns + c;
            Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(idx)];
            if ((idx % 17) == 0 && c + 1 < columns) {
                cell.codePoint = 0x4E00u + static_cast<std::uint32_t>(idx % 0x100);
                Ubiquitty::Pen widePen = Ubiquitty::cellPen(cell);
                widePen.attributes = Ubiquitty::VT::Attribute::Wide;
                Ubiquitty::cellSetPen(cell, widePen);
                Ubiquitty::Cell& cont = grid[static_cast<std::size_t>(idx) + 1];
                cont.codePoint = Ubiquitty::wideContinuationSentinelCodePoint;
                Ubiquitty::Pen contPen = Ubiquitty::cellPen(cont);
                contPen.attributes = Ubiquitty::VT::Attribute::WideContinuation;
                contPen.underlineIndex = Ubiquitty::colorDefault;
                Ubiquitty::cellSetPen(cont, contPen);
                c += 2;
                continue;
            }
            cell.codePoint = static_cast<std::uint32_t>('!' + (idx % 94));
            Ubiquitty::VT::AttributeSet attributes;
            if ((idx % 7) == 0)
                attributes.add(Ubiquitty::VT::Attribute::Bold);
            if ((idx % 11) == 0)
                attributes.add(Ubiquitty::VT::Attribute::Underline);
            if ((idx % 23) == 0)
                attributes.add(Ubiquitty::VT::Attribute::Reverse);
            Ubiquitty::Pen glyphPen = Ubiquitty::cellPen(cell);
            glyphPen.attributes = attributes;
            glyphPen.foregroundIndex = static_cast<std::uint16_t>(idx % 8);
            glyphPen.backgroundIndex = ((idx % 5) == 0) ? static_cast<std::uint16_t>(0) : Ubiquitty::colorDefault;
            Ubiquitty::cellSetPen(cell, glyphPen);
            ++c;
        }
    }
    return grid;
}

// A run-heavy screen: long spans of one pen broken by occasional styled runs,
// the shape real terminal output takes — a prompt, a diff, ls output. The
// companion to makeRenderGrid, which changes pen on nearly every cell. The two
// bracket the range a renderer sees, and a per-run memo ranks oppositely on
// them, so measuring only one of the pair mismeasures the whole class.
std::vector<Ubiquitty::Cell> makeRenderGridRuns(int rows, int columns)
{
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    Ubiquitty::Cell blank = Ubiquitty::defaultBlankCell;
    for (auto& cell : grid)
        cell = blank;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            const int idx = r * columns + c;
            Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(idx)];
            cell.codePoint = static_cast<std::uint32_t>('!' + (idx % 94));
            Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
            // A 12-cell styled span every 40 columns: one pen across the span,
            // one across the plain text between.
            if ((c % 40) < 12) {
                pen.attributes = Ubiquitty::VT::Attribute::Bold;
                pen.foregroundIndex = static_cast<std::uint16_t>(2 + (c / 40) % 6);
            } else {
                pen.attributes = Ubiquitty::VT::AttributeSet {};
                pen.foregroundIndex = Ubiquitty::colorDefault;
            }
            pen.backgroundIndex = Ubiquitty::colorDefault;
            Ubiquitty::cellSetPen(cell, pen);
        }
    }
    return grid;
}

// A screen with ragged right margins — the shape every other render grid here
// lacks, and the one a terminal actually shows. `makeRenderGrid` and
// `makeRenderGridRuns` put a printable character in every cell of every row, so
// the lanes built on them measure a screen that is 100% dense: no lane in this
// battery can see a change whose subject is the blank part of a frame, and a
// blank-region optimisation reads as a flat no-op against all of them.
//
// Line lengths cycle over a fixed set rather than a random draw, so the scene is
// reproducible and its blank fraction is a property of the generator rather than
// of a seed. The tail of each row is the reset blank, which is what a terminal
// leaves past the last character it wrote.
std::vector<Ubiquitty::Cell> makeRenderGridSparse(int rows, int columns)
{
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
    Ubiquitty::Cell blank = Ubiquitty::defaultBlankCell;
    for (auto& cell : grid)
        cell = blank;
    // Prompt-ish, prose-ish, path-ish, and two long-output lengths: a spread
    // that leaves the mean line about half the width, which is what `ls`,
    // a build log and an editor session look like.
    static const int lineLengths[] = { 14, 62, 38, 9, 120, 47, 71, 26 };
    for (int r = 0; r < rows; ++r) {
        const int written = std::min(lineLengths[r % 8], columns);
        for (int c = 0; c < written; ++c) {
            const int idx = r * columns + c;
            Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(idx)];
            cell.codePoint = static_cast<std::uint32_t>('!' + (idx % 94));
            Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
            // One styled span per line, as a prompt or a diff marker leaves.
            if (c < 8) {
                pen.attributes = Ubiquitty::VT::Attribute::Bold;
                pen.foregroundIndex = static_cast<std::uint16_t>(2 + (r % 6));
            } else {
                pen.attributes = Ubiquitty::VT::AttributeSet {};
                pen.foregroundIndex = Ubiquitty::colorDefault;
            }
            pen.backgroundIndex = Ubiquitty::colorDefault;
            Ubiquitty::cellSetPen(cell, pen);
        }
    }
    return grid;
}

// Text whose colours never change while its underline does, which is what SGR 58
// under a fixed foreground produces: a diagnostic line that recolours the squiggle
// per severity, a diff that underlines in one hue over unchanged text.
//
// The distribution exists because no other grid here varies a pen field that the
// colour resolver does not read. Every other lane changes `attributes` or
// `foregroundIndex`, so the branches keyed on the underline group train to zero
// counts and compile cold under -fprofile-use — the shape `render_build_full`
// closes for the box and cursor streams.
std::vector<Ubiquitty::Cell> makeRenderGridUnderlineVaried(int rows, int columns)
{
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            const int idx = r * columns + c;
            Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(idx)];
            cell.codePoint = static_cast<std::uint32_t>('!' + (idx % 94));
            Ubiquitty::Pen pen = Ubiquitty::cellPen(cell);
            // Held constant, so the resolver's answer is the same for every cell
            // in the grid and only the fields it never reads move.
            pen.attributes = Ubiquitty::VT::Attribute::Underline;
            pen.foregroundIndex = Ubiquitty::colorDefault;
            pen.backgroundIndex = Ubiquitty::colorDefault;
            // The one thing that varies. Cycled per cell rather than per span:
            // a memo keyed on the whole pen misses on every one of these, and a
            // memo keyed on the resolver's inputs hits on all of them, so the
            // lane separates the two by construction.
            pen.underlineIndex = static_cast<std::uint16_t>(1 + (idx % 6));
            Ubiquitty::cellSetPen(cell, pen);
        }
    }
    return grid;
}

// Which pen distribution a render lane drives.
enum class PenVariety : std::uint8_t {
    // A different pen on nearly every cell.
    PerCell,
    // Long spans of one pen, broken by occasional styled runs.
    Runs,
    // One resolved colour pair throughout, with the underline colour moving under
    // it. Separates a memo keyed on the whole pen from one keyed on the fields
    // the resolver reads.
    UnderlineVaried,
    // Ragged right margins: written text on the left of each row, reset blanks
    // to the edge. The only distribution here whose rows are not full.
    Sparse,
    // Pens indexed into the 216-colour cube and the greyscale ramp — the range a
    // 256-colour theme emits. Every other distribution here indexes below 16,
    // so this is the only lane that reaches the palette slots above them.
    Indexed256,
};

/// Builds the cell grid a pen distribution describes.
///
/// Switched in one place rather than at each lane, because a lane that picks its
/// grid with a two-way test silently falls back to the per-cell distribution when
/// a third arrives — it compiles, it runs, and it reports the wrong distribution's
/// number under the new lane's name.
///
/// The per-cell grid with every palette index moved above 15.
///
/// Derived from ``makeRenderGrid`` rather than written beside it, so the only
/// difference between this lane and `render_build` is the index range. A grid
/// written independently would differ in glyphs, widths and attributes too, and
/// the lane would report the scene rather than the path.
///
/// `Palette8` narrows every slot once a frame; this lane keeps the slots above
/// the first sixteen under measurement.
///
/// - Parameters:
///   - rows: Grid height in cells.
///   - columns: Grid width in cells.
/// - Returns: The filled grid.
std::vector<Ubiquitty::Cell> makeRenderGridIndexed256(int rows, int columns)
{
    std::vector<Ubiquitty::Cell> grid = makeRenderGrid(rows, columns);
    for (std::size_t i = 0; i < grid.size(); ++i) {
        Ubiquitty::Pen pen = Ubiquitty::cellPen(grid[i]);
        // colorDefault and colorRGB are not indices into the palette and must
        // stay themselves; anything the resolver would have found in the cached
        // sixteen moves into the cube instead.
        if (pen.foregroundIndex < 16)
            pen.foregroundIndex = static_cast<std::uint16_t>(16 + ((pen.foregroundIndex * 29 + i) % 240));
        if (pen.backgroundIndex < 16)
            pen.backgroundIndex = static_cast<std::uint16_t>(16 + ((pen.backgroundIndex * 31 + i * 7) % 240));
        Ubiquitty::cellSetPen(grid[i], pen);
    }
    return grid;
}

/// - Parameters:
///   - variety: Which pen distribution to fill.
///   - rows: Grid height in cells.
///   - columns: Grid width in cells.
/// - Returns: The filled grid.
std::vector<Ubiquitty::Cell> makeRenderGridFor(PenVariety variety, int rows, int columns)
{
    switch (variety) {
    case PenVariety::Runs:
        return makeRenderGridRuns(rows, columns);
    case PenVariety::UnderlineVaried:
        return makeRenderGridUnderlineVaried(rows, columns);
    case PenVariety::Sparse:
        return makeRenderGridSparse(rows, columns);
    case PenVariety::Indexed256:
        return makeRenderGridIndexed256(rows, columns);
    case PenVariety::PerCell:
        break;
    }
    return makeRenderGrid(rows, columns);
}

/// Names the lane a pen distribution produces.
///
/// - Parameter variety: Which pen distribution the lane drives.
/// - Returns: The suffix appended to the lane's base name, empty for the per-cell
///   distribution, whose lanes carry the base name alone.
std::string renderLaneSuffix(PenVariety variety)
{
    switch (variety) {
    case PenVariety::Runs:
        return "_runs";
    case PenVariety::UnderlineVaried:
        return "_underline";
    case PenVariety::Sparse:
        return "_sparse";
    case PenVariety::Indexed256:
        return "_indexed256";
    case PenVariety::PerCell:
        break;
    }
    return "";
}

void renderBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters, PenVariety variety, const std::string& suffix = "")
{
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    std::vector<Ubiquitty::Cell> grid = makeRenderGridFor(variety, rows, columns);
    RenderFakeAtlas atlas;

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 1.0f;
    in.metrics.strikeThickness = 1.0f;
    in.metrics.xHeight = 10.0f;

    std::vector<CellInstance> cellsOut(static_cast<std::size_t>(rows) * columns);
    std::vector<DecorationInstance> decorationsOut(static_cast<std::size_t>(rows) * columns * 8);
    InstanceBuffers buffers;
    buffers.cells = cellsOut.data();
    buffers.cellCapacity = cellsOut.size();
    buffers.decorations = decorationsOut.data();
    buffers.decorationCapacity = decorationsOut.size();

    std::size_t sink = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
        sink += counts.cells + counts.decorations;
    });

    const double cells = static_cast<double>(rows) * static_cast<double>(columns);
    Ubiquitty::Bench::Result result;
    result.name = std::string("render_build") + renderLaneSuffix(variety) + suffix;
    result.unit = "Mcell/s";
    result.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
    result.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
    out.push_back(result);
    if (sink == 0xFFFFFFFFFFFFFFFFull) // defeat dead-store elimination of the build
        std::fprintf(stderr, " ");
}

// Link-styled cells through the builder, which no other lane reaches: the
// corpus emits OSC 8 in its parse lanes, so a grep for the escape reports the
// path covered while the render half — `CellPalette::linkColor()` and the
// hyperlink colour path — never runs. This lane executes it: the theme-contrast
// resolve that picks the link blue, and the builder's link branches — resting
// tint, hovered tint, and the always-on link underline — over cells
// `linkCells` marks. Counters are the point; the throughput number is a
// baseline like any other lane's.
void renderLinkBench(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters)
{
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    std::vector<Ubiquitty::Cell> grid = makeRenderGrid(rows, columns);
    RenderFakeAtlas atlas;

    // The theme the contrast resolve reads. Mid-grey background, so the
    // WCAG comparison between the two blues does real arithmetic rather than
    // short-circuiting on black.
    Ubiquitty::Themes::Theme theme {};
    theme.name = "bench";
    for (int i = 0; i < 16; ++i)
        theme.ansi[i] = Ubiquitty::ColorRGBA { static_cast<uint8_t>(0x20 + i), static_cast<uint8_t>(0x30 + i), static_cast<uint8_t>(0xA0 + i), 255 };
    theme.defaultForeground = Ubiquitty::ColorRGBA { 0xE0, 0xE0, 0xE0, 255 };
    theme.defaultBackground = Ubiquitty::ColorRGBA { 0x70, 0x70, 0x70, 255 };

    // One link run per row, and every eighth row hovered: both tints and the
    // membership boundary execute, and most cells stay plain so the lane's
    // number remains a builder number rather than a link number.
    std::vector<std::uint8_t> linkCells(static_cast<std::size_t>(rows) * columns, 0);
    for (int row = 0; row < rows; ++row) {
        const std::uint8_t state = (row % 8 == 0)
            ? static_cast<std::uint8_t>(Ubiquitty::Rendering::LinkState::Hovered)
            : static_cast<std::uint8_t>(Ubiquitty::Rendering::LinkState::Resting);
        for (int column = 8; column < 24 && column < columns; ++column)
            linkCells[static_cast<std::size_t>(row) * columns + column] = state;
    }

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 1.0f;
    in.metrics.strikeThickness = 1.0f;
    in.metrics.xHeight = 10.0f;
    in.linkCells = linkCells.data();

    std::vector<CellInstance> cellsOut(static_cast<std::size_t>(rows) * columns);
    std::vector<DecorationInstance> decorationsOut(static_cast<std::size_t>(rows) * columns * 8);
    InstanceBuffers buffers;
    buffers.cells = cellsOut.data();
    buffers.cellCapacity = cellsOut.size();
    buffers.decorations = decorationsOut.data();
    buffers.decorationCapacity = decorationsOut.size();

    std::size_t sink = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        // The frontends' own call order: the theme-contrast resolve runs when
        // a palette is rebuilt, and the tint it picks feeds the frame. Inside
        // the lambda so the resolve trains with the builder it feeds.
        const Ubiquitty::CellPalette cellPalette = Ubiquitty::CellPalette::fromTheme(theme);
        const Ubiquitty::ColorRGBA link = cellPalette.linkColor();
        in.linkColor = { link.red / 255.0f, link.green / 255.0f, link.blue / 255.0f, 1.0f };
        in.linkHoverColor = { theme.defaultForeground.red / 255.0f, theme.defaultForeground.green / 255.0f, theme.defaultForeground.blue / 255.0f, 1.0f };
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
        sink += counts.cells + counts.decorations;
    });

    const double cells = static_cast<double>(rows) * static_cast<double>(columns);
    Ubiquitty::Bench::Result result;
    result.name = "render_build_links";
    result.unit = "Mcell/s";
    result.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
    result.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
    out.push_back(result);
    if (sink == 0xFFFFFFFFFFFFFFFFull) // defeat dead-store elimination of the build
        std::fprintf(stderr, " ");
}

// The subpixel glyph pass, which the render_build lanes above never reach: they
// leave subpixelGlyphs false and glyphs unwired, so every cell takes the
// cell-baked UV path.
//
// That is not a defect in those lanes — it is the shipping SoftwareRenderer
// configuration. Unifont has no overhang, so computeGlyphSlot returns zero
// margins and the backends switch the pass off (`VulkanRenderer::renderGrid`,
// and `TerminalFrameInputs::captureSource` for the rest). A real text face does
// overhang its cell box, and then every plain mono cell emits a GlyphInstance
// and takes the margin-expanded UV instead. Both configurations ship; only the
// first was measured.
//
// The slot geometry comes from computeGlyphSlot over a representative text-face
// overhang, not from hand-picked constants, so the lane tracks the policy the
// backends actually call rather than drifting from it.
// `isSelected` draws a selection behind the text over every other row, the
// way the macOS arm draws one: in the cells, so the walk tests it per cell.
void subpixelRenderBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters, PenVariety variety, bool isSelected = false) // bool-ok: a lane variant, named at its two call sites
{
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    std::vector<Ubiquitty::Cell> grid = makeRenderGridFor(variety, rows, columns);
    RenderFakeAtlas atlas;

    // A monospace text face on an 8x16 cell whose ink runs half a pixel past the
    // box on every side — the ordinary case that turns the pass on.
    Ubiquitty::Rendering::GlyphOverhangInputs overhang;
    overhang.advanceWidth = 8.0;
    overhang.lineHeight = 16.0;
    overhang.ascent = 13.0;
    overhang.descent = 3.0;
    overhang.primaryBboxX = -0.5;
    overhang.primaryBboxY = -3.5;
    overhang.primaryBboxWidth = 9.0;
    overhang.primaryBboxHeight = 17.0;
    const Ubiquitty::Rendering::GlyphSlot slot = Ubiquitty::Rendering::computeGlyphSlot(overhang);

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 1.0f;
    in.metrics.strikeThickness = 1.0f;
    in.metrics.xHeight = 10.0f;
    // Mirrors `VulkanRenderer::renderGrid` — the pass is on exactly when the
    // slot carries a margin, and the slot is the cell plus that margin each side.
    in.subpixelGlyphs = slot.marginInPixels.width > 0 || slot.marginInPixels.height > 0;
    in.glyphMarginInPixels.width = static_cast<float>(slot.marginInPixels.width);
    in.glyphMarginInPixels.height = static_cast<float>(slot.marginInPixels.height);
    in.glyphSlotWidthInPixels = static_cast<float>(8 + 2 * slot.marginInPixels.width);
    in.glyphSlotHeightInPixels = static_cast<float>(16 + 2 * slot.marginInPixels.height);
    std::vector<SelectionRowSpan> selection;
    if (isSelected) {
        for (int r = 0; r < rows; r += 2)
            selection.push_back(SelectionRowSpan { r, 0, columns - 1 });
        in.selectionSpans = selection.data();
        in.selectionSpanCount = static_cast<int>(selection.size());
        in.selectionColor = { 0.2f, 0.4f, 0.9f, 0.35f };
        in.isSelectionBehindText = true;
    }

    const std::size_t cellCapacity = static_cast<std::size_t>(rows) * columns;
    std::vector<CellInstance> cellsOut(cellCapacity);
    std::vector<DecorationInstance> decorationsOut(cellCapacity * 8);
    std::vector<GlyphInstance> glyphsOut(cellCapacity); // up to one overflow glyph per cell
    InstanceBuffers buffers;
    buffers.cells = cellsOut.data();
    buffers.cellCapacity = cellsOut.size();
    buffers.decorations = decorationsOut.data();
    buffers.decorationCapacity = decorationsOut.size();
    buffers.glyphs = glyphsOut.data();
    buffers.glyphCapacity = glyphsOut.size();

    std::size_t sink = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
        sink += counts.cells + counts.glyphs;
    });

    const double cells = static_cast<double>(rows) * static_cast<double>(columns);
    Ubiquitty::Bench::Result result;
    result.name = std::string("render_build_subpixel") + (isSelected ? "_selected" : "") + renderLaneSuffix(variety);
    result.unit = "Mcell/s";
    result.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
    result.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
    out.push_back(result);
    if (sink == 0xFFFFFFFFFFFFFFFFull) // defeat dead-store elimination
        std::fprintf(stderr, " ");
}

// ---- sized text (OSC 66) ----------------------------------------------
// A screen of s=7 letters, every window of every band its own slot, built and
// composited through the SoftwareRenderer's own sheets: the lane that times the
// sized-window probes and the per-block admission (CellInstanceBuilder.cpp,
// `SizedBlockWindows`) where they are largest. The face is synthetic —
// its 1x glyph blank, its sized master solid — so the time is the builder's and
// the cache's rather than a rasteriser's. The first frame faults every window;
// the timed frames find them resident, which is a static screen's steady state.
//
// `_cycle` turns through three such screens of different letters frame by
// frame. The sheet is cut for two screens, so each frame evicts windows the
// frames before it drew from and faults its own back in: the eviction a screen
// of sized text scrolling past costs, one LRU step per window missed. The
// 15x71 lane fits a 1024-slot sheet, the one size every revision draws sharp,
// so it is the lane to compare across them.
// The composite is checked for coverage, so a lane that drew every block soft
// fails rather than timing the cheaper path.
struct SolidSizedRasterizer final : Ubiquitty::Rendering::GlyphRasterizer {
    [[nodiscard]] Ubiquitty::Rendering::CellMetrics cellMetrics() const override
    {
        Ubiquitty::Rendering::CellMetrics metrics {};
        metrics.cellWidth = 8.0f;
        metrics.cellHeight = 16.0f;
        metrics.baselineFromTop = 13.0f;
        return metrics;
    }
    bool rasterizeGlyph(std::uint32_t, Ubiquitty::Rendering::FontFace, std::uint32_t, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * static_cast<std::size_t>(slotHeight), std::uint8_t { 0 });
        return true;
    }
    [[nodiscard]] bool rasterisesScaledGlyphsSharply() const override { return true; }
    bool rasterizeSizedMaster(std::uint32_t, Ubiquitty::Rendering::FontFace, std::uint32_t, const Ubiquitty::Rendering::SizedMaster&, int slotWidth, int slotHeight, std::uint8_t* coverageOut) override
    {
        std::fill_n(coverageOut, static_cast<std::size_t>(slotWidth) * static_cast<std::size_t>(slotHeight), std::uint8_t { 255 });
        return true;
    }
};

// Blocks of `scale` rows by `scale` columns from the top-left, one letter a
// block, letters counting up from `firstCodePoint`.
std::vector<Ubiquitty::Cell> makeSizedLetterGrid(int rows, int columns, int scale, unsigned specID, std::uint32_t firstCodePoint)
{
    std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns), Ubiquitty::defaultBlankCell);
    std::uint32_t codePoint = firstCodePoint;
    for (int top = 0; top + scale <= rows; top += scale) {
        for (int left = 0; left + scale <= columns; left += scale, ++codePoint) {
            for (int band = 0; band < scale; ++band) {
                for (int offset = 0; offset < scale; ++offset) {
                    Ubiquitty::Cell& cell = grid[static_cast<std::size_t>(top + band) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(left + offset)];
                    cell.codePoint = offset == 0 ? codePoint : 0u;
                    Ubiquitty::Pen pen = cell.pen;
                    pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
                    pen.extendedAttributes.setTextSizeSpecID(specID);
                    Ubiquitty::penSetTextBand(&pen, static_cast<std::uint8_t>(band));
                    Ubiquitty::cellSetPen(cell, pen);
                }
            }
        }
    }
    return grid;
}

enum class SizedScreens : std::uint8_t {
    One,
    Cycling,
};

void sizedTextBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters, SizedScreens screens)
{
    constexpr int scale = 7;
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    Ubiquitty::TextSizeTable table;
    const unsigned id = table.intern(Ubiquitty::TextSizeSpec { /*scale=*/scale, /*widthCells=*/1 });
    // Latin Extended-A and B, Latin Extended Additional, and Glagolitic with
    // Coptic: narrow letters, none a combining mark, distinct per block and
    // between the screens.
    const std::array<std::vector<Ubiquitty::Cell>, 3> screenGrids = {
        makeSizedLetterGrid(rows, columns, scale, id, 0x100u),
        makeSizedLetterGrid(rows, columns, scale, id, 0x1E00u),
        makeSizedLetterGrid(rows, columns, scale, id, 0x2C00u),
    };

    BuildInputs in;
    in.cells = screenGrids[0].data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.textSizeTable = &table;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;

    SolidSizedRasterizer rasterizer;
    Ubiquitty::Rendering::SoftwareRenderer renderer(8, 16, rasterizer);
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    renderer.render(in, rgba, width, height);
    const char* const suffix = screens == SizedScreens::Cycling ? "_cycle" : "";
    // A frame that drew every block soft would be cheaper and blank: the 1x
    // glyph is empty, so the ink counted here is the sharp windows' alone. The
    // last pixel is a plain cell past the blocks, so every geometry here
    // leaves a row and a column the blocks do not reach.
    std::size_t inked = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
        inked += rgba[i] != rgba[rgba.size() - 4] ? 1 : 0;
    const std::size_t blockPixels = static_cast<std::size_t>(rows / scale * scale) * 16u * static_cast<std::size_t>(columns / scale * scale) * 8u;
    if (inked != blockPixels) {
        std::fprintf(stderr, "render_sized%s: %zu of %zu block pixels drawn sharp — the lane is not timing the sharp path\n", suffix, inked, blockPixels);
        return;
    }

    const GlyphBackend backend = renderer.glyphBackend();
    const std::size_t cellCapacity = static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns);
    std::vector<CellInstance> cellsOut(cellCapacity + static_cast<std::size_t>(rows));
    std::vector<DecorationInstance> decorationsOut(cellCapacity * 2);
    InstanceBuffers buffers;
    buffers.cells = cellsOut.data();
    buffers.cellCapacity = cellsOut.size();
    buffers.decorations = decorationsOut.data();
    buffers.decorationCapacity = decorationsOut.size();

    std::size_t frame = 0;
    const auto nextScreen = [&] {
        if (screens == SizedScreens::Cycling)
            in.cells = screenGrids[++frame % screenGrids.size()].data();
    };
    std::size_t sink = 0;
    const auto build = Ubiquitty::Bench::measure(warmup, iters, [&] {
        nextScreen();
        const InstanceCounts counts = buildCellInstances(in, backend, buffers);
        sink += counts.cells;
    });
    const auto composite = Ubiquitty::Bench::measure(warmup, iters, [&] {
        nextScreen();
        renderer.render(in, rgba, width, height);
        sink += static_cast<std::size_t>(width);
    });

    const std::string geometry = "_" + std::to_string(rows) + "x" + std::to_string(columns);
    Ubiquitty::Bench::Result built;
    built.name = std::string("render_sized_build_s7") + geometry + suffix;
    built.unit = "us";
    built.value = build.medianSeconds * 1e6;
    built.p95 = build.p95Seconds * 1e6;
    out.push_back(built);
    Ubiquitty::Bench::Result composited;
    composited.name = std::string("render_sized_composite_s7") + geometry + suffix;
    composited.unit = "us";
    composited.value = composite.medianSeconds * 1e6;
    composited.p95 = composite.p95Seconds * 1e6;
    out.push_back(composited);
    if (sink == 0xFFFFFFFFFFFFFFFFull) // defeat dead-store elimination
        std::fprintf(stderr, " ");
}

// ---- the macOS stream set -------------------------------------------
// A null output stream is not an empty one: each stream the caller omits
// switches a whole pass off inside the builder, so which streams a lane wires
// decides which code it times. The frontends do not agree on the set.
//
//     stream            macOS   Web   Linux   lanes above
//     cells               *      *      *        *
//     decorations         *      *      *        *
//     overlays            *      *      *        -
//     glyphs              *      -      *        subpixel lane only
//     wideGlyphs          *      -      -        -
//     colorGlyphs         *      -      -        -
//     ligatureGlyphs      *      -      -        -
//     boxFills            *      -      -        -
//
// The Swift surface's render profile wires every stream; the web terminal and
// the Vulkan renderer wire overlays. So the lanes above are the
// Linux/Web shape, and the configuration macOS runs — the one the renderer is
// judged on — is measured by nothing.
//
// Wiring a stream is only half of reaching its pass: the pass also needs cells
// that take it. An overlay buffer with no cursor and no selection emits
// nothing, a colour sheet with no emoji cell is never consulted, and a box
// sheet only matters where a code point decomposes. This lane carries the
// content for each alongside the buffer.
//
// What it does not carry is the platform's shaping and rasterisation. The
// shaper below returns a fixed decomposition for planted pairs, which is enough
// to put the builder on its run-slicing path and nothing like what CoreText
// costs; a fake sized to imitate that would be timing the fake. The figure here
// is the BUILDER's share of a macOS-shaped frame and is not a frame budget.
//
// The cluster read stays unreached: it needs cells carrying combining marks or
// a pool id, which this scene does not build.
struct FullRenderFakeAtlas : RenderFakeAtlas {
    // Ligature-forming pairs the scene plants, as (first, second) code points.
    static bool isRunStart(std::uint32_t first, std::uint32_t second)
    {
        return (first == '-' && second == '>') || (first == '=' && second == '>')
            || (first == '!' && second == '=') || (first == '=' && second == '=');
    }

    // A DECDLD cell rasterises in Core and asks the backend only to stage the
    // finished plane. Without this entry `declineMonoCoverage` answers, every
    // soft-font cell blanks, and the lane profiles the FAILURE path while its
    // counts read as coverage.
    std::size_t stagedPlanes = 0;
    bool stageMonoCoverage(int, std::span<const std::uint8_t> coverage)
    {
        stagedPlanes += coverage.empty() ? 0 : 1;
        return true;
    }

    AtlasGlyph colorGlyph(std::span<const std::uint32_t> codePoints, std::uint16_t)
    {
        return !codePoints.empty() ? assign(codePoints[0] | 0x1000000u) : AtlasGlyph {};
    }

    // A run per planted pair, every other column left on the mono path. The
    // strip spans a full slot because the builder maps a covered cell's
    // atlas-pixel range onto it linearly and needs a non-zero denominator.
    void shapeLigatureRuns(std::span<const LigatureCell> rowCells, float, float, GlyphPass, std::span<LigatureRun> runs)
    {
        const int columns = static_cast<int>(rowCells.size());
        for (int column = 0; column < columns; ++column)
            runs[column].length = 0;
        for (int column = 0; column + 1 < columns; ++column) {
            if (!isRunStart(rowCells[column].codePoint, rowCells[column + 1].codePoint))
                continue;
            LigatureRun run;
            run.strip = assign(rowCells[column].codePoint | 0x2000000u);
            run.usedWidthInAtlasPixels = 12.0f;
            run.slotWidthInAtlasPixels = 16.0f;
            run.startColumn = column;
            run.length = 2;
            runs[column] = run;
            runs[column + 1] = run;
            ++column; // a cell belongs to one run
        }
    }
};

// A DECDLD soft font with the first eight positions defined, so the scene can
// carry DRCS cells. Without one the soft-font fault is never reached and the
// profile leaves `softFontGlyph` cold.
Ubiquitty::VT::SoftFont makeSoftFont()
{
    Ubiquitty::VT::SoftFont font {};
    font.valid = true;
    font.serial = 1;
    font.matrixWidth = 8;
    font.matrixHeight = 8;
    for (int cell = 0; cell < 8; ++cell) {
        font.glyphSet[cell] = true;
        for (int row = 0; row < 8; ++row)
            font.glyph[cell][row] = static_cast<std::uint16_t>(0xFFu >> (cell % 4));
    }
    return font;
}

// The per-cell scene of makeRenderGrid, with the cells the macOS stream set
// needs: box-drawing shapes that decompose rather than reaching a font, an
// emoji carrying the presentation bit that routes it to the colour sheet, and
// the character pairs the shaper above joins.
std::vector<Ubiquitty::Cell> makeRenderGridFull(int rows, int columns)
{
    std::vector<Ubiquitty::Cell> grid = makeRenderGrid(rows, columns);
    // Box shapes that covers() accepts: rules, a corner, a block and a shade.
    // Dashes are excluded there by design, so none appear here.
    static const std::uint32_t boxShapes[] = { 0x2500u, 0x2502u, 0x250Cu, 0x2588u, 0x2591u };
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            const std::size_t idx = static_cast<std::size_t>(r) * columns + c;
            Ubiquitty::Cell& cell = grid[idx];
            const Ubiquitty::Pen existing = Ubiquitty::cellPen(cell);
            // Leave the wide pairs makeRenderGrid planted intact: overwriting a
            // lead or its continuation half separates them.
            if (existing.attributes.contains(Ubiquitty::VT::Attribute::Wide) || existing.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                continue;
            if ((c % 13) == 0) {
                cell.codePoint = boxShapes[(r + c / 13) % 5];
                continue;
            }
            if ((c % 31) == 7) {
                cell.codePoint = 0x1F600u;
                Ubiquitty::Pen emojiPen = existing;
                emojiPen.attributes.add(Ubiquitty::VT::Attribute::Emoji);
                Ubiquitty::cellSetPen(cell, emojiPen);
                continue;
            }
            // A DECDLD cell every 23 columns. Its code point is the private
            // sentinel the parser writes, so the builder takes the soft-font
            // fault rather than the font. The stride is coprime with the box (13)
            // and ligature (19) plants, so no cell is claimed twice.
            if ((c % 23) == 7) {
                cell.codePoint = Ubiquitty::VT::drcsCodePointBase + static_cast<std::uint32_t>((r + c) % 8);
                continue;
            }
            // A ligature pair every 19 columns, which the shaper joins.
            if ((c % 19) == 3 && c + 1 < columns) {
                cell.codePoint = '=';
                grid[idx + 1].codePoint = '>';
                ++c;
            }
        }
    }
    return grid;
}

void fullRenderBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters)
{
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    std::vector<Ubiquitty::Cell> grid = makeRenderGridFull(rows, columns);
    FullRenderFakeAtlas atlas;

    BuildInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.metrics.cellWidth = 8.0f;
    in.metrics.cellHeight = 16.0f;
    in.metrics.baselineFromTop = 13.0f;
    in.metrics.underlineThickness = 1.0f;
    in.metrics.underlineDrop = 1.0f;
    in.metrics.strikeThickness = 1.0f;
    in.metrics.xHeight = 10.0f;
    // The bold rule variants and the skip-ink cut are part of what the macOS
    // capture fills in, and each is a branch the defaults never take.
    in.metrics.underlineThicknessBold = 2.0f;
    in.metrics.underlineDropBold = 1.0f;
    in.metrics.strikeThicknessBold = 2.0f;
    in.metrics.skipInk = true;
    in.metrics.skipInkPadding = 1.0f;
    in.viewportSizeInPixels = { static_cast<float>(columns) * 8.0f, static_cast<float>(rows) * 16.0f };
    // A real text face overhangs its cell, which is what turns the subpixel
    // pass on and puts every plain mono cell on the margin-expanded UV. The
    // slot comes from the policy the backends call rather than from constants.
    Ubiquitty::Rendering::GlyphOverhangInputs overhang;
    overhang.advanceWidth = 8.0;
    overhang.lineHeight = 16.0;
    overhang.ascent = 13.0;
    overhang.descent = 3.0;
    overhang.primaryBboxX = -0.5;
    overhang.primaryBboxY = -3.5;
    overhang.primaryBboxWidth = 9.0;
    overhang.primaryBboxHeight = 17.0;
    const Ubiquitty::Rendering::GlyphSlot fullSlot = Ubiquitty::Rendering::computeGlyphSlot(overhang);
    in.subpixelGlyphs = fullSlot.marginInPixels.width > 0 || fullSlot.marginInPixels.height > 0;
    in.glyphMarginInPixels.width = static_cast<float>(fullSlot.marginInPixels.width);
    in.glyphMarginInPixels.height = static_cast<float>(fullSlot.marginInPixels.height);
    in.glyphSlotWidthInPixels = static_cast<float>(8 + 2 * fullSlot.marginInPixels.width);
    in.glyphSlotHeightInPixels = static_cast<float>(16 + 2 * fullSlot.marginInPixels.height);
    // The two-cell slot a wide glyph scales against. Left zero, a wide glyph
    // stays on its cell however many streams the caller wires — the stream is
    // necessary and not sufficient, and the coverage check below is what makes
    // the difference visible rather than a silently smaller lane.
    in.glyphWideSlotWidthInPixels = static_cast<float>(16 + 2 * fullSlot.marginInPixels.width);
    in.glyphWideSlotHeightInPixels = static_cast<float>(16 + 2 * fullSlot.marginInPixels.height);
    in.glyphBoxWidthInPixels = 8.0f;
    // The 2x slot a DECDWL/DECDHL row's glyph scales against. Sized like the
    // wide slot above: both are two cells across, and only the sheet differs.
    in.glyphDoubleSlotWidthInPixels = static_cast<float>(16 + 2 * fullSlot.marginInPixels.width);
    in.glyphDoubleSlotHeightInPixels = static_cast<float>(16 + 2 * fullSlot.marginInPixels.height);

    // A cursor and one selected span, the two things that make the overlay pass
    // emit. Without either the buffer is wired and the pass still writes
    // nothing, which is the shape a lane can pass while measuring none of it.
    in.cursor.row = rows / 2;
    in.cursor.column = columns / 3;
    in.cursor.mixA = 1.0f;
    in.cursor.rgb = { 1.0f, 1.0f, 1.0f, 1.0f };
    in.cursor.shape = Ubiquitty::Cursor::Shape::Bar;
    in.cursor.focused = true;
    in.cellHeightPoints = 16.0;
    std::vector<SelectionRowSpan> selection;
    for (int r = 2; r < rows; r += 8)
        selection.push_back(SelectionRowSpan { r, 4, columns - 4 });
    in.selectionSpans = selection.data();
    in.selectionSpanCount = static_cast<int>(selection.size());
    in.selectionColor = { 0.2f, 0.4f, 0.9f, 0.35f };

    // The soft font the DRCS cells above resolve against. Borrowed for the
    // build, like every other pointer in BuildInputs.
    const Ubiquitty::VT::SoftFont softFont = makeSoftFont();
    in.softFont = &softFont;

    // A DECDWL row and a DECDHL pair every 25 rows, the pair adjacent because a
    // double-height top row draws the same text as the bottom one under it.
    //
    // Sparse on purpose, unlike the column plants above. A doubled row halves
    // the cells it draws, so its share sets how much of this lane is the doubled
    // path rather than the single-width one every other stream measures — a
    // banner's worth keeps the pass trained without the lane's figure becoming
    // mostly a report on DECDHL.
    std::vector<std::uint8_t> lineAttributes(static_cast<std::size_t>(rows), Ubiquitty::lineAttributeNormal);
    for (int r = 3; r + 2 < rows; r += 25) {
        lineAttributes[static_cast<std::size_t>(r)] = Ubiquitty::lineAttributeDoubleWidth;
        lineAttributes[static_cast<std::size_t>(r) + 1] = Ubiquitty::lineAttributeDoubleHeightTop;
        lineAttributes[static_cast<std::size_t>(r) + 2] = Ubiquitty::lineAttributeDoubleHeightBottom;
    }
    in.lineAttributes = lineAttributes.data();

    const std::size_t cellCapacity = static_cast<std::size_t>(rows) * columns;
    std::vector<CellInstance> cellsOut(cellCapacity);
    std::vector<DecorationInstance> decorationsOut(cellCapacity * 8);
    std::vector<GlyphInstance> glyphsOut(cellCapacity);
    std::vector<GlyphInstance> wideGlyphsOut(cellCapacity);
    std::vector<GlyphInstance> proportionalGlyphsOut(cellCapacity);
    std::vector<GlyphInstance> proportionalStripsOut(cellCapacity);
    std::vector<GlyphInstance> colorGlyphsOut(cellCapacity);
    std::vector<GlyphInstance> ligatureGlyphsOut(cellCapacity);
    std::vector<GlyphInstance> doubleGlyphsOut(cellCapacity);
    // A box cell decomposes into several quads, and the overlay stream also
    // carries the cursor and every selection span.
    std::vector<OverlayInstance> boxFillsOut(cellCapacity * 8);
    std::vector<OverlayInstance> overlaysOut(cellCapacity + 64);

    InstanceBuffers buffers;
    buffers.cells = cellsOut.data();
    buffers.cellCapacity = cellsOut.size();
    buffers.decorations = decorationsOut.data();
    buffers.decorationCapacity = decorationsOut.size();
    buffers.glyphs = glyphsOut.data();
    buffers.glyphCapacity = glyphsOut.size();
    buffers.wideGlyphs = wideGlyphsOut.data();
    buffers.wideGlyphCapacity = wideGlyphsOut.size();
    buffers.proportionalGlyphs = proportionalGlyphsOut.data();
    buffers.proportionalGlyphCapacity = proportionalGlyphsOut.size();
    buffers.proportionalStrips = proportionalStripsOut.data();
    buffers.proportionalStripCapacity = proportionalStripsOut.size();
    buffers.colorGlyphs = colorGlyphsOut.data();
    buffers.colorGlyphCapacity = colorGlyphsOut.size();
    buffers.ligatureGlyphs = ligatureGlyphsOut.data();
    buffers.ligatureGlyphCapacity = ligatureGlyphsOut.size();
    buffers.doubleGlyphs = doubleGlyphsOut.data();
    buffers.doubleGlyphCapacity = doubleGlyphsOut.size();
    buffers.boxFills = boxFillsOut.data();
    buffers.boxFillCapacity = boxFillsOut.size();
    buffers.overlays = overlaysOut.data();
    buffers.overlayCapacity = overlaysOut.size();

    // A stream that is wired but never written times exactly what a null one
    // times, while reading as covered — the shape a lane keeps passing after the
    // scene drifts out from under it. Each is checked once against the built
    // frame, so losing a pass stops the lane instead of quietly shrinking it.
    const InstanceCounts covered = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
    const struct {
        const char* stream;
        std::size_t count;
    } coverage[] = {
        { "cells", covered.cells },
        { "decorations", covered.decorations },
        { "overlays", covered.overlays },
        { "glyphs", covered.glyphs },
        { "wideGlyphs", covered.wideGlyphs },
        { "colorGlyphs", covered.colorGlyphs },
        { "ligatureGlyphs", covered.ligatureGlyphs },
        { "boxFills", covered.boxFills },
        { "softFontGlyphs", covered.softFontGlyphs },
        { "doubleGlyphs", covered.doubleGlyphs },
    };
    std::fprintf(stderr, "render_build_full covers:");
    for (const auto& entry : coverage)
        std::fprintf(stderr, " %s=%zu", entry.stream, entry.count);
    std::fprintf(stderr, "\n");
    for (const auto& entry : coverage) {
        if (entry.count > 0)
            continue;
        std::fprintf(stderr, "render_build_full: %s is wired and stayed empty — the scene no longer\n"
                             "  reaches that pass, so the lane would report a figure for code it did not run.\n",
            entry.stream);
        std::abort();
    }

    std::size_t sink = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), buffers);
        sink += counts.cells + counts.decorations + counts.overlays + counts.boxFills;
    });

    const double cells = static_cast<double>(rows) * static_cast<double>(columns);
    Ubiquitty::Bench::Result result;
    result.name = "render_build_full";
    result.unit = "Mcell/s";
    result.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
    result.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
    out.push_back(result);
    if (sink == 0xFFFFFFFFFFFFFFFFull) // defeat dead-store elimination
        std::fprintf(stderr, " ");
}

// TerminalFrameInputs::capture — the per-frame rows -> BuildInputs assembly
// every arm pays before the builder runs; no other lane reaches it. The source
// pair measures a detached frame, including the ownership copy it requires.
// The live lane reads the terminal directly and exposes an accidental second
// ownership copy without conflating it with the builder.
void captureBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters)
{
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    std::vector<Ubiquitty::Cell> grid = makeRenderGridFor(PenVariety::PerCell, rows, columns);
    std::vector<std::uint8_t> lineAttributes(static_cast<std::size_t>(rows), Ubiquitty::lineAttributeNormal);

    FrameSource source;
    source.cells = grid.data();
    source.lineAttributes = lineAttributes.data();
    source.rows = rows;
    source.columns = columns;
    source.palette = &palette;
    source.paletteGeneration = 1;
    source.hasPaletteGeneration = true;

    TerminalFrameInputs frame;
    CellMetrics metrics;
    metrics.cellWidth = 8.0f;
    metrics.cellHeight = 16.0f;
    const GlyphSlotGeometry slotGeometry {};
    const double cells = static_cast<double>(rows) * static_cast<double>(columns);
    std::vector<std::uint8_t> dirtyRows(static_cast<std::size_t>(rows), 0u);
    dirtyRows.front() = 1u;
    dirtyRows.back() = 1u;

    std::size_t sink = 0;
    const std::uint8_t* const masks[] = { nullptr, dirtyRows.data() };
    const char* const names[] = { "render_capture", "render_capture_dirty" };
    for (int lane = 0; lane < 2; ++lane) {
        TerminalSnapshotOptions options;
        // Links on: the flag-plane rebuild is part of what a frame pays.
        options.shouldUnderlineLinks = true;
        options.dirtyRows = masks[lane];
        const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
            sink += frame.capture(source, options, metrics, slotGeometry) ? 1u : 0u;
        });
        Ubiquitty::Bench::Result result;
        result.name = names[lane];
        result.unit = "Mcell/s";
        result.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
        result.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
        out.push_back(result);
    }

    auto terminal = makeFilledTerm(rows, columns);
    TerminalFrameInputs liveFrame;
    TerminalSnapshotOptions liveOptions;
    liveOptions.shouldUnderlineLinks = true;
    const auto liveTiming = Ubiquitty::Bench::measure(warmup, iters, [&] {
        sink += liveFrame.capture(*terminal, liveOptions, metrics, slotGeometry) ? 1u : 0u;
    });
    Ubiquitty::Bench::Result liveResult;
    liveResult.name = "render_capture_live";
    liveResult.unit = "Mcell/s";
    liveResult.value = liveTiming.medianSeconds > 0.0 ? (cells / liveTiming.medianSeconds) / 1.0e6 : 0.0;
    liveResult.p95 = liveTiming.p95Seconds > 0.0 ? (cells / liveTiming.p95Seconds) / 1.0e6 : 0.0;
    out.push_back(liveResult);
    terminal.reset();
    if (sink == 0xFFFFFFFFFFFFFFFFull) // defeat dead-store elimination
        std::fprintf(stderr, " ");
}

// The per-cell colour resolve, with and without the run memo the macOS renderer
// uses in CellInstanceBuilder.cpp: resolve once per
// DISTINCT pen and reuse across the run, keyed on raw pen bytes.
//
// Driven against CellPalette::resolve — the resolver macOS actually calls — not
// the portable resolveCell the render_build lanes drive. The two are independent
// implementations by architecture (Rendering/ColorResolve.h: macOS's per-colour
// opacity "stays backend-specific"), so whether a memo pays is a property of the
// RESOLVER's cost, and a result measured against one does not transfer to the
// other. Both pen regimes run, because a memo ranks oppositely on them.
void resolveMemoBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters, PenVariety variety)
{
    Ubiquitty::CellPalette palette;
    palette.seedDefaults();
    palette.fillExtended();
    const std::vector<Ubiquitty::Cell> grid = makeRenderGridFor(variety, rows, columns);
    const std::size_t cellCount = static_cast<std::size_t>(rows) * columns;
    const std::string suffix = renderLaneSuffix(variety);
    std::uint32_t sink = 0;

    // Every cell resolved: what the portable builder does.
    auto everyCellPass = [&] {
        std::uint8_t foreground[3], background[3];
        for (std::size_t i = 0; i < cellCount; ++i) {
            palette.resolve(Ubiquitty::cellPen(grid[i]), Ubiquitty::ReverseMode::Normal, foreground, background);
            sink += foreground[0] + background[0];
        }
    };

    // Resolved once per distinct pen: what macOS does.
    auto perPenRunPass = [&] {
        Ubiquitty::Pen cachedPen {};
        cachedPen.foregroundIndex = static_cast<std::uint16_t>(-1);
        std::uint8_t cachedForeground[3] = { 0, 0, 0 };
        std::uint8_t cachedBackground[3] = { 0, 0, 0 };
        for (std::size_t i = 0; i < cellCount; ++i) {
            const Ubiquitty::Pen& pen = Ubiquitty::cellPen(grid[i]);
            if (std::memcmp(&pen, &cachedPen, sizeof(Ubiquitty::Pen)) != 0) {
                cachedPen = pen;
                palette.resolve(pen, Ubiquitty::ReverseMode::Normal, cachedForeground, cachedBackground);
            }
            sink += cachedForeground[0] + cachedBackground[0];
        }
    };

    // Interleaved and lead-swapping, via the shared harness. Timing all of A
    // before any of B charges whatever drifts between the blocks to whichever
    // ran second, and that drift is the same size as the effect being measured.
    const Ubiquitty::Bench::PairedTiming paired = Ubiquitty::Bench::measurePaired(
        warmup, iters, everyCellPass, perPenRunPass);
    const Ubiquitty::Bench::Timing everyCell = paired.a;
    const Ubiquitty::Bench::Timing perRun = paired.b;

    const double cells = static_cast<double>(cellCount);
    auto push = [&](const char* name, const Ubiquitty::Bench::Timing& timing) {
        Ubiquitty::Bench::Result result;
        result.name = std::string(name) + suffix;
        result.unit = "Mcell/s";
        result.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
        result.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
        out.push_back(result);
    };
    push("resolve_every_cell", everyCell);
    push("resolve_per_pen_run", perRun);
    if (sink == 0xFFFFFFFFu) // defeat dead-store elimination
        std::fprintf(stderr, " ");
}

// getDisplayRow's Arabic-shape + bidi-reorder pass, cached vs forced-reshape. A
// static RTL row (the idle full-screen RTL case) hits the shaped-row cache after
// the first fetch; the gap between the two variants is the O(columns) shape cost
// the cache elides per row per frame.
// Interactive-resize deferral: a live width-drag step rewraps only the screen
// band (O(rows)); a Settled step rewraps the whole scrollback (O(history)).
// Same 10k-line history, same 120<->119 width toggle — the gap is the deferred
// history rewrap; the one-time settle cost is reported alongside.
void reflowBenches(std::vector<Ubiquitty::Bench::Result>& out, int historyLines, int warmup, int iters, const std::string& suffix = "")
{
    auto makeTerm = [&]() {
        auto t = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::setScrollbackLimit(t.get(), historyLines + 64);
        Ubiquitty::resize(*t, 24, 120);
        std::string line;
        for (int i = 0; i < historyLines; i++) {
            line.assign(static_cast<std::size_t>(20 + (i % 80)), static_cast<char>('a' + (i % 26)));
            line += "\r\n";
            Ubiquitty::processBytes(t.get(), line);
        }
        return t;
    };

    auto settled = makeTerm();
    int settledWidth = 120;
    const auto settledTiming = Ubiquitty::Bench::measure(warmup, iters, [&] {
        settledWidth = (settledWidth == 120) ? 119 : 120;
        Ubiquitty::resize(*settled, 24, settledWidth);
    });
    settled.reset();

    auto interactive = makeTerm();
    int interactiveWidth = 120;
    const auto interactiveTiming = Ubiquitty::Bench::measure(warmup, iters, [&] {
        interactiveWidth = (interactiveWidth == 120) ? 119 : 120;
        Ubiquitty::resize(*interactive, 24, interactiveWidth, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    });
    const auto settleTiming = Ubiquitty::Bench::measure(0, 1, [&] { Ubiquitty::settleResize(interactive.get()); });
    interactive.reset();

    Ubiquitty::Bench::Result settledResult;
    settledResult.name = "reflow_step_settled" + suffix;
    settledResult.unit = "ms/step";
    settledResult.value = settledTiming.medianSeconds * 1.0e3;
    settledResult.p95 = settledTiming.p95Seconds * 1.0e3;
    out.push_back(settledResult);
    Ubiquitty::Bench::Result interactiveResult;
    interactiveResult.name = "reflow_step_interactive" + suffix;
    interactiveResult.unit = "ms/step";
    interactiveResult.value = interactiveTiming.medianSeconds * 1.0e3;
    interactiveResult.p95 = interactiveTiming.p95Seconds * 1.0e3;
    out.push_back(interactiveResult);
    Ubiquitty::Bench::Result settleResult;
    settleResult.name = "reflow_settle_once" + suffix;
    settleResult.unit = "ms";
    settleResult.value = settleTiming.medianSeconds * 1.0e3;
    settleResult.p95 = settleTiming.p95Seconds * 1.0e3;
    out.push_back(settleResult);
}

// Logical positions: a capture and a resolution of the same place, spread over
// a long history and the screen, which is what a selection endpoint costs each
// facade call. A screen row's capture finds the pin its earlier capture made.
void positionBenches(std::vector<Ubiquitty::Bench::Result>& out, int historyLines, int warmup, int iters, const std::string& suffix)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(t.get(), historyLines + 64);
    Ubiquitty::resize(*t, 24, 120);
    std::string line;
    for (int i = 0; i < historyLines; i++) {
        line.assign(static_cast<std::size_t>(20 + (i % 180)), static_cast<char>('a' + (i % 26)));
        line += "\r\n";
        Ubiquitty::processBytes(t.get(), line);
    }
    constexpr int places = 64;
    const int total = Ubiquitty::Screen::scrollbackCount(t.get()) + t->rows;
    const auto rowOf = [&](int index) { return static_cast<int>(static_cast<long long>(total - 1) * index / (places - 1)); };
    long long resolved = 0;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        for (int index = 0; index < places; ++index) {
            const auto position = Ubiquitty::Screen::capturePosition(*t, rowOf(index), 10);
            if (!position)
                continue;
            const auto place = Ubiquitty::Screen::resolvePosition(*t, *position);
            resolved += place ? place->absoluteRow : -1;
        }
        Ubiquitty::Bench::doNotOptimize(resolved);
    });
    Ubiquitty::Bench::Result result;
    result.name = "position_capture_resolve" + suffix;
    result.unit = "ns/pair";
    result.value = timing.medianSeconds * 1.0e9 / places;
    result.p95 = timing.p95Seconds * 1.0e9 / places;
    out.push_back(result);
}

// The model lock's share of a search's start: capturing the history for the
// worker, block directory and the newest rows' facts, over a long history.
void searchStartBenches(std::vector<Ubiquitty::Bench::Result>& out, int historyLines, int warmup, int iters, const std::string& suffix)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(t.get(), historyLines + 64);
    Ubiquitty::resize(*t, 24, 120);
    std::string line;
    for (int i = 0; i < historyLines; i++) {
        line.assign(static_cast<std::size_t>(20 + (i % 180)), static_cast<char>('a' + (i % 26)));
        line += "\r\n";
        Ubiquitty::processBytes(t.get(), line);
    }
    while (t->lineStore->foldColdBlocks(64)) {
    }
    Ubiquitty::LineStore::Snapshot snapshot;
    snapshot.spareLock = Ubiquitty::PAL::makePriorityInheritingLock();
    // The rows a scan of the recent history reads before it asks for more.
    constexpr std::size_t startFacts = 4096;
    bool isCaptured = true;
    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] {
        isCaptured = t->lineStore->captureSnapshot(snapshot, startFacts) && isCaptured;
        Ubiquitty::Bench::doNotOptimize(isCaptured);
    });
    Ubiquitty::LineStore::releaseSnapshot(snapshot);
    Ubiquitty::Bench::Result result;
    result.name = "search_start_lock_hold" + suffix;
    result.unit = isCaptured ? "us" : "us (refused)";
    result.value = timing.medianSeconds * 1.0e6;
    result.p95 = timing.p95Seconds * 1.0e6;
    out.push_back(result);
}

void shapedRowBenches(std::vector<Ubiquitty::Bench::Result>& out, int columns, int warmup, int iters, const std::string& suffix = "")
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, columns);
    std::string rtl;
    for (int c = 0; c < columns - 1; ++c)
        rtl += "\xD8\xA8"; // Arabic BEH — cursive, needs shaping
    Ubiquitty::processBytes(&t, rtl);

    std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(columns));
    Ubiquitty::Screen::getDisplayRow(t, 0, 0, row.data(), columns); // warm the cache

    const auto hit = Ubiquitty::Bench::measure(warmup, iters, [&] { Ubiquitty::Screen::getDisplayRow(t, 0, 0, row.data(), columns); });
    const auto reshape = Ubiquitty::Bench::measure(warmup, iters, [&] {
        t.grid->invalidateShapedRowCache();
        Ubiquitty::Screen::getDisplayRow(t, 0, 0, row.data(), columns);
    });

    Ubiquitty::Bench::Result cached;
    cached.name = "shaped_row_cached" + suffix;
    cached.unit = "ns/row";
    cached.value = hit.medianSeconds * 1.0e9;
    cached.p95 = hit.p95Seconds * 1.0e9;
    out.push_back(cached);
    Ubiquitty::Bench::Result rs;
    rs.name = "shaped_row_reshape" + suffix;
    rs.unit = "ns/row";
    rs.value = reshape.medianSeconds * 1.0e9;
    rs.p95 = reshape.p95Seconds * 1.0e9;
    out.push_back(rs);
}

// tmux -CC %output pane delivery: every payload byte routes through the
// primary's paneLookup hook to the Ubiquitty::Terminal backing the active pane. The modeled
// hook pays the production hop's shape (a lock acquire + hash lookup per call —
// the frontend consults a locked registry), so the route memo's once-per-run
// resolution is visible against a per-byte one; the stream is kilobyte
// single-pane frames, the shape %output arrives in.
static std::mutex s_paneRouteLock;
static std::unordered_map<int, Ubiquitty::Terminal*> s_paneRoute;

static Ubiquitty::Terminal* modeledPaneLookup(void*, int paneID)
{
    std::scoped_lock lock(s_paneRouteLock);
    auto it = s_paneRoute.find(paneID);
    return it == s_paneRoute.end() ? nullptr : it->second;
}

void tmuxDeliverBenches(std::vector<Ubiquitty::Bench::Result>& out, int warmup, int iters)
{
    auto primary = std::make_unique<Ubiquitty::Terminal>();
    auto pane = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*primary, 24, 80);
    Ubiquitty::resize(*pane, 24, 80);
    s_paneRoute.clear();
    s_paneRoute[0] = pane.get();
    primary->integration.paneRouter.lookup = modeledPaneLookup;
    primary->integration.paneRouter.context = nullptr;

    Ubiquitty::Protocol::Tmux::configure(&primary->integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    const char* handshake = "%begin 1 1 0\r\n%end 1 1 0\r\n";
    Ubiquitty::processBytes(primary.get(), handshake);

    std::string stream;
    const std::string payload(1024, 'x');
    for (int frame = 0; frame < 64; ++frame)
        stream += "%output %0 " + payload + "\r\n";

    const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] { Ubiquitty::processBytes(primary.get(), stream); });
    out.push_back(Ubiquitty::Bench::throughput("tmux_deliver_pane", static_cast<double>(stream.size()), timing));

    pane.reset();
    primary.reset();
}

// The host daemon's per-frame digest pass (buildFrameDigests): a per-row content
// digest plus, by default, a per-cell SGR-5 blink scan. Run on a no-blink grid
// (the common case) both with the scan on (blinkPresentHint=true) and skipped via
// the sticky hint (false) — the gap is the blink-scan cost the hint elides.
void frameDigestBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters, const std::string& suffix = "")
{
    using namespace Ubiquitty::Rendering;
    std::vector<Ubiquitty::Cell> grid = makeRenderGrid(rows, columns); // no blink cells
    std::vector<RowDigest> digests(static_cast<std::size_t>(rows));

    PlanInputs in;
    in.cells = grid.data();
    in.rows = rows;
    in.columns = columns;
    in.cursorRow = rows / 2;
    in.cursorColumn = columns / 2;
    static const std::uint64_t global = 0xABCDEF0123456789ull;
    in.globalState = &global;
    in.globalStateLength = sizeof(global);
    in.blinkClock = 1;

    const double cells = static_cast<double>(rows) * static_cast<double>(columns);
    auto bench = [&](const char* name, bool blinkScan) {
        in.blinkPresentHint = blinkScan;
        std::uint64_t sink = 0;
        const auto timing = Ubiquitty::Bench::measure(warmup, iters, [&] { sink ^= buildFrameDigests(in, digests.data()); });
        Ubiquitty::Bench::Result r;
        r.name = std::string(name) + suffix;
        r.unit = "Mcell/s";
        r.value = timing.medianSeconds > 0.0 ? (cells / timing.medianSeconds) / 1.0e6 : 0.0;
        r.p95 = timing.p95Seconds > 0.0 ? (cells / timing.p95Seconds) / 1.0e6 : 0.0;
        out.push_back(r);
        if (sink == 0xFFFFFFFFFFFFFFFFull) // defeat dead-store elimination
            std::fprintf(stderr, " ");
    };
    bench("frame_digest_scan", true); // word-wise digest (#3) + per-cell blink scan
    bench("frame_digest_hint", false); // digest only — blink scan skipped (#6)
}

// Our-slice latency (headless): the CPU cost from bytes-at-Core to a
// model update for one line of fresh output — the unit that lands between
// frames. processBytes of a full-width line + CRLF on a Ubiquitty::Terminal already holding a
// screenful, p50/p99 ms. The render/present legs are GPU (P2); this guards the
// parse+model slice we own. (A single keystroke's model update is sub-timer-
// resolution; a line is the smallest stable, meaningful interactive unit.)
void latencyBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, int warmup, int iters)
{
    auto term = makeFilledTerm(rows, columns);
    std::vector<char> line;
    line.reserve(static_cast<std::size_t>(columns) + 2);
    for (int c = 0; c < columns; ++c)
        line.push_back(static_cast<char>('A' + (c % 26)));
    line.push_back('\r');
    line.push_back('\n');
    auto samples = Ubiquitty::Bench::measureSamples(warmup, iters, [&] { Ubiquitty::processBytes(term.get(), std::string_view(line)); });
    term.reset();
    Ubiquitty::Bench::latencyMilliseconds(out, "line_latency", std::move(samples));
}

// Frame-time TAIL under a sustained deterministic flood: a fixed byte
// stream (scroll-heavy + SGR-churn + plain) split into equal frame-sized chunks,
// each fed via processBytes and timed. The worst frame (max) + p99 are the
// "never stutter" guard — a scroll storm or reflow shows up as a tall frame the
// mean hides. Deterministic stream ⇒ reproducible tail.
void frameTailBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns)
{
    std::vector<char> flood;
    const std::vector<char> plain = plainStream(384 * 1024);
    const std::vector<char> sgr = sgrStream(384 * 1024);
    const std::vector<char> scroll = scrollStream(256 * 1024);
    flood.insert(flood.end(), plain.begin(), plain.end());
    flood.insert(flood.end(), sgr.begin(), sgr.end());
    flood.insert(flood.end(), scroll.begin(), scroll.end());

    constexpr std::size_t frameBytes = 4096; // ~one 60 Hz read under flood
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);
    // Warm pass over the whole flood so scrollback/caches are settled.
    Ubiquitty::processBytes(term.get(), std::string_view(flood));

    std::vector<double> frames;
    frames.reserve(flood.size() / frameBytes + 1);
    for (std::size_t off = 0; off < flood.size(); off += frameBytes) {
        const int n = static_cast<int>(std::min(frameBytes, flood.size() - off));
        const auto start = std::chrono::steady_clock::now();
        Ubiquitty::processBytes(term.get(), std::string_view(flood.data() + off, static_cast<std::size_t>(n)));
        const auto end = std::chrono::steady_clock::now();
        frames.push_back(std::chrono::duration<double>(end - start).count());
    }
    term.reset();
    Ubiquitty::Bench::frameTailMilliseconds(out, "flood_frame_tail", std::move(frames));
}

// Lock-acquire wait under flood: the shape of the model-lock starvation class
// (T7's motivating problem). One thread floods `processBytes` in lock-bracketed
// chunks (mirroring the real ingest drain's per-chunk lock/unlock); a second
// thread simulates a 60 Hz render loop trying to acquire the same lock on
// schedule, timing only the WAIT (request → acquired), then holds it just long
// enough to do a representative grid scan (getDisplayRow every row — the same
// per-row work captureInto:'s damage-bounded copy does) before releasing.
// Reports the wait distribution's tail (p99/max) — a rare long stall under
// flood is exactly what "starvation" means; the mean hides it. This is the
// "before" half of T7's before/after — a published-snapshot redesign is
// expected to collapse this toward render_build's uncontended cost.
//
// It mirrors the drain's lock/unlock CADENCE and not its unlock. The shipping
// path goes through `PacedDrainHost::unlockModel`, which donates the lock
// through a fairness doorway whenever the renderer has announced a wait —
// probe first, yield past a band. This lane takes a plain `std::mutex` and
// reacquires immediately, so its number is what the drain would cost with that
// doorway removed, not what a shipping session pays.
//
// The difference is the whole effect. Read this lane as the floor the doorway
// is holding up, and do not quote it as a stall a user sees.
//
// The tail is also two adjacent order statistics rather than two numbers:
// `quantileSeconds` indexes `size * 0.99`, and 2 seconds at 60 Hz is ~120
// samples, so _p99 is the second largest and _max the largest. They tend to
// agree closely because they are neighbours, not because the tail is flat.
void lockStarvationBenches(std::vector<Ubiquitty::Bench::Result>& out, int rows, int columns, double durationSeconds)
{
    std::vector<char> flood;
    const std::vector<char> plain = plainStream(2 * 1024 * 1024);
    const std::vector<char> scroll = scrollStream(1 * 1024 * 1024);
    flood.insert(flood.end(), plain.begin(), plain.end());
    flood.insert(flood.end(), scroll.begin(), scroll.end());
    constexpr std::size_t chunkBytes = 4096; // ~one ingest-drain chunk

    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);
    std::mutex modelLock;
    std::atomic<bool> stop { false };

    std::thread producer([&] {
        std::size_t off = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const std::size_t n = std::min(chunkBytes, flood.size() - off);
            {
                std::scoped_lock lock(modelLock);
                Ubiquitty::processBytes(term.get(), std::string_view(flood.data() + off, static_cast<std::size_t>(n)));
            }
            off += n;
            if (off >= flood.size())
                off = 0; // wrap — a fixed deterministic stream replayed for the run's duration
        }
    });

    std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(columns));
    std::vector<double> waitSeconds;
    const auto tickInterval = std::chrono::microseconds(16667); // 60 Hz cadence
    const auto runUntil = std::chrono::steady_clock::now() + std::chrono::duration<double>(durationSeconds);
    while (std::chrono::steady_clock::now() < runUntil) {
        const auto tickStart = std::chrono::steady_clock::now();
        const auto wantLock = std::chrono::steady_clock::now();
        modelLock.lock();
        const auto gotLock = std::chrono::steady_clock::now();
        for (int r = 0; r < rows; ++r)
            Ubiquitty::Screen::getDisplayRow(*term, 0, r, row.data(), columns);
        modelLock.unlock();
        waitSeconds.push_back(std::chrono::duration<double>(gotLock - wantLock).count());
        std::this_thread::sleep_until(tickStart + tickInterval);
    }

    stop.store(true, std::memory_order_relaxed);
    producer.join();
    term.reset();
    Ubiquitty::Bench::frameTailMilliseconds(out, "lock_wait_under_flood", std::move(waitSeconds));
}

} // namespace

// --- Key encoding -------------------------------------------------------------
//
// The corpus is this one binary, so a namespace no lane here executes compiles
// COLD under -fprofile-use rather than neutrally. Without this lane that is the
// whole Ubiquitty::Input namespace, with Keyboard, Keymap and Compose — and the
// keyboard path is the one the user feels most directly, because every
// keystroke pays it before anything else can start.
//
// It is not a throughput lane in the sense the parse lanes are: a person cannot
// press keys fast enough for this to matter as a rate. It exists so the encoder
// is TRAINED, and the measurement is the by-product that shows the training
// happened — a lane that stopped executing would show up as a number, not as
// silence.
//
// The sweep is over the shape of the work rather than over every key. What
// changes the branch a key takes is its class (printable, functional, keypad),
// its modifier set, its event type, and the reported-flags mode the application
// asked for; the code point itself only changes the digits printed. So the
// matrix is those four, and it is walked in full rather than sampled.
Ubiquitty::Bench::Result keyEncodeThroughput(const char* name, int warmup, int iterations)
{
    using namespace Ubiquitty;
    using Kitty::KeyFlag;

    // A printable, a functional (cursor Left), and a keypad key: three distinct
    // routes through the encoder, not three spellings of one.
    static const std::uint32_t keys[] = {
        0x61u, // 'a' — printable, takes the text path
        Keys::Keymap::keymapKeyLeft,
        Keys::Keymap::keymapKeyKeypad5,
    };
    static const std::uint32_t modifierSets[] = {
        0u,
        static_cast<std::uint32_t>(Keys::Keymap::keymapModifierShift),
        static_cast<std::uint32_t>(Keys::Keymap::keymapModifierCtrl),
        static_cast<std::uint32_t>(Keys::Keymap::keymapModifierAlternate | Keys::Keymap::keymapModifierCtrl),
        static_cast<std::uint32_t>(Keys::Keymap::keymapModifierSuper | Keys::Keymap::keymapModifierShift),
    };
    static const Keys::KeyEventType events[] = {
        Keys::KeyEventType::Press,
        Keys::KeyEventType::Repeat,
        Keys::KeyEventType::Release,
    };
    // Progressive-enhancement levels an application actually negotiates, from
    // "off" up. A release event is suppressed without ReportEvents, so the sweep
    // covers the suppressed path as well as the emitting one.
    static const Kitty::KeyFlagSet flagSets[] = {
        Kitty::KeyFlagSet {},
        Kitty::KeyFlagSet { KeyFlag::Disambiguate },
        Kitty::KeyFlagSet { KeyFlag::Disambiguate } | KeyFlag::ReportEvents,
        Kitty::KeyFlagSet { KeyFlag::Disambiguate } | KeyFlag::ReportEvents | KeyFlag::ReportAlternateKeys,
        Kitty::KeyFlagSet { KeyFlag::Disambiguate } | KeyFlag::ReportEvents | KeyFlag::ReportAlternateKeys | KeyFlag::ReportAll | KeyFlag::ReportText,
    };
    const std::uint32_t text[] = { 0x61u };

    char outputBuffer[64];
    // Read as well as written. `volatile` alone stops the store being removed
    // and still leaves the variable set-but-not-used, which -Werror rejects —
    // and a lane whose encoder call the optimiser is free to delete trains
    // nothing while reporting a rate.
    static volatile int sink = 0;

    auto once = [&]() {
        long bytes = 0;
        for (std::uint32_t key : keys)
            for (std::uint32_t modifiers : modifierSets)
                for (Keys::KeyEventType event : events)
                    for (const Kitty::KeyFlagSet& flags : flagSets) {
                        const bool isPrintable = key < 0x110000u && key >= 0x20u;
                        bytes += Kitty::encodeKey(key, isPrintable ? key : 0u, isPrintable ? key : 0u,
                            modifiers, event, std::span<const std::uint32_t>(text, isPrintable ? 1u : 0u),
                            flags, outputBuffer);
                        // A bare modifier reaches the same encoder through
                        // Runtime::encodeKey, whose gate this lane does not
                        // model; the key forms above are what it times.
                    }
        sink = static_cast<int>(bytes);
        return bytes + (sink & 0);
    };

    const double encodesPerIteration = static_cast<double>(std::size(keys) * std::size(modifierSets)
        * std::size(events) * std::size(flagSets) * 2);
    const auto timing = Ubiquitty::Bench::measure(warmup, iterations, [&] { once(); });
    Ubiquitty::Bench::Result result;
    result.name = name;
    // Encodes rather than bytes: the output length varies by an order of
    // magnitude across the sweep, so bytes would report the flag set rather than
    // the work.
    result.unit = "Mencode/s";
    result.value = timing.medianSeconds > 0.0 ? (encodesPerIteration / timing.medianSeconds) / 1.0e6 : 0.0;
    result.p95 = timing.p95Seconds > 0.0 ? (encodesPerIteration / timing.p95Seconds) / 1.0e6 : 0.0;
    return result;
}

int main(int argc, char** argv)
{
    // --only <substring>[,<substring>...]: run just the lane groups whose tag
    // contains one of them, so a profiler or a counter run attributes to one
    // lane instead of forty, and an A/B pair runs the groups it compares
    // instead of the battery. The tag is the group function's name minus
    // "Benches" (render, checkpoint, parse...). Scripts/profile-lane.sh and
    // Scripts/bench-ab.py drive this; Docs/measuring-a-core-change.md is the
    // procedure they belong to.
    std::string only;
    const char* platform = "unknown";
    const char* commit = "unknown";
    double mib = 4.0;
    int iters = 20;
    double floodSeconds = 2.0;
    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* f) { return std::strcmp(argv[i], f) == 0; };
        if (eq("--platform") && i + 1 < argc)
            platform = argv[++i];
        else if (eq("--commit") && i + 1 < argc)
            commit = argv[++i];
        else if (eq("--mib") && i + 1 < argc)
            mib = std::atof(argv[++i]);
        else if (eq("--iters") && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else if (eq("--flood-seconds") && i + 1 < argc)
            floodSeconds = std::atof(argv[++i]);
        else if (eq("--only") && i + 1 < argc)
            only = argv[++i];
        else if (eq("--scroll-lane"))
            isBenchScrollLaneEnabled = true;
    }
    const std::size_t bytes = static_cast<std::size_t>(mib * 1024 * 1024);
    const int warmup = 3;

    std::vector<Ubiquitty::Bench::Result> results;
    // The filter splits on commas up front rather than substring-matching the
    // joined string per call: matching against "checkpoint,parse" would also
    // admit any tag containing a comma-spanning fragment, and that no tag
    // matches "t,p" is an accident of the names — the split keeps that from
    // ever mattering.
    std::vector<std::string> onlyTags;
    for (std::size_t start = 0; start <= only.size();) {
        const std::size_t comma = only.find(',', start);
        const std::size_t end = comma == std::string::npos ? only.size() : comma;
        if (end > start)
            onlyTags.emplace_back(only, start, end - start);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    const auto wanted = [&](const char* tag) {
        if (onlyTags.empty())
            return true;
        for (const std::string& wantedTag : onlyTags) {
            if (std::strstr(tag, wantedTag.c_str()) != nullptr)
                return true;
        }
        return false;
    };
    // A lane too long for every run, which runs only when --only names its
    // tag whole: the full-run baselines and the PGO training corpus stay as
    // they are.
    const auto named = [&](const char* tag) { return std::ranges::find(onlyTags, std::string(tag)) != onlyTags.end(); };
    if (wanted("key"))
        results.push_back(keyEncodeThroughput("key_encode", warmup, iters));
    // The frontend seams: see the lane definitions above parseThroughput for
    // why each one exists.
    if (wanted("seams"))
        results.push_back(pointerHoverRate("pointer_hover", 24, 80, warmup, iters));
    if (wanted("seams"))
        results.push_back(frameGateRate("frame_gate", 24, 80, warmup, iters));
    if (wanted("seams"))
        results.push_back(selectionDragRate("selection_drag", 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughput("parse_plain", plainStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughput("parse_sgr", sgrStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughput("parse_scroll", scrollStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughput("parse_charset", characterSetStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughput("parse_wide", wideTextStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughput("parse_emoji", emojiStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse_rep"))
        results.push_back(parseThroughput("parse_rep_ascii", repeatedASCIIStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse_rep"))
        results.push_back(parseThroughput("parse_rep_wide", repeatedWideStream(bytes), 24, 80, warmup, iters));
    // Native input uses processBytesUpToSyncClear, including SGR and Unicode.
    if (wanted("parse"))
        results.push_back(parseThroughputSyncFeed("parse_plain_syncfeed", plainStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughputSyncFeed("parse_scroll_syncfeed", scrollStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughputSyncFeed("parse_sgr_syncfeed", sgrStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughputSyncFeed("parse_wide_syncfeed", wideTextStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughputSyncFeed("parse_emoji_syncfeed", emojiStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughputSyncFeed("parse_charset_syncfeed", characterSetStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse_rep"))
        results.push_back(parseThroughputSyncFeed("parse_rep_ascii_syncfeed", repeatedASCIIStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse_rep"))
        results.push_back(parseThroughputSyncFeed("parse_rep_wide_syncfeed", repeatedWideStream(bytes), 24, 80, warmup, iters));
    // A control sequence inside an open scroll run; see scrollingSGRThroughput.
    // Run by name only, so neither the full-run baselines nor the PGO training
    // corpus include it.
    if (named("sgr_scroll")) {
        const std::vector<char> stream = scrollingSGRStream(bytes);
        for (const ParseEntry entry : { ParseEntry::ProcessBytes, ParseEntry::UpToSyncClear }) {
            const std::string suffix = entry == ParseEntry::UpToSyncClear ? "_syncfeed" : "";
            results.push_back(scrollingSGRThroughput("parse_sgr_scroll_full" + suffix, stream, ScrollShape::Full, entry, 24, 80, warmup, iters));
            results.push_back(scrollingSGRThroughput("parse_sgr_scroll_band" + suffix, stream, ScrollShape::Band, entry, 24, 80, warmup, iters));
            results.push_back(scrollingSGRThroughput("parse_sgr_scroll_region" + suffix, stream, ScrollShape::Region, entry, 24, 80, warmup, iters));
        }
    }
    // OSC/APC payload batcher path (titles, OSC 52 clipboard, Kitty APC graphics) —
    // instruments the bulk appendOSC/APCPayloadRun scan that the CAN/SUB break
    // handling lives on.
    if (wanted("parse"))
        results.push_back(parseThroughput("parse_osc", oscStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughput("parse_osc_digit_run", oscDigitRunStream(bytes), 24, 80, warmup, iters));
    if (wanted("parse"))
        results.push_back(parseThroughput("hyperlinks", hyperlinkStream(bytes), 24, 80, warmup, iters));
    // Multi-frame sync-burst backlog (dead-frame elision sizing).
    if (wanted("syncburst"))
        syncBurstBenches(results, warmup, iters);
    // Streamed Kitty o=z decode — the framebuffer-video workload (a client pushing
    // full RGB frames at 30-60 fps). Two sizes: a classic 320x200 DOS mode and a
    // 640x480 VGA frame (~4x pixels). framesPerIter models ~one second of stream,
    // so kitty_oz_fps reads directly against the client's target rate.
    if (wanted("kittyozstream"))
        kittyOzStreamBenches(results, 320, 200, /*framesPerIter*/ 60, warmup, iters);
    if (wanted("kittyozstream"))
        kittyOzStreamBenches(results, 640, 480, /*framesPerIter*/ 30, warmup, iters, "_640x480");
    // CPU floor of the streamed same-id texture upload (the per-frame RGB->RGBA
    // widen) — isolates the CPU widen from the GPU-side in-flight-texture stall
    // that a live PerfLog run measures.
    if (wanted("kittyuploadwiden"))
        kittyUploadWidenBenches(results, 320, 200, warmup, iters * 20);
    if (wanted("kittyuploadwiden"))
        kittyUploadWidenBenches(results, 640, 480, warmup, iters * 10, "_640x480");
    // An uncompressed transfer of a 4 MiB image, chunked as clients send it.
    // Run by name only, for the same reason as sgr_scroll.
    if (named("kitty_raw"))
        kittyRawTransmitBenches(results, 1024, 1024, warmup, iters);
    // A checkpoint is small at a typical window, so it wants many more
    // iterations than the multi-MiB parse streams to settle.
    if (wanted("checkpoint"))
        checkpointBenches(results, 24, 80, warmup, iters * 50);
    // Larger grids expose how encode/apply scale with area. Fewer iters — each
    // checkpoint is much bigger.
    if (wanted("checkpoint"))
        checkpointBenches(results, 50, 200, warmup, iters * 10, "_50x200");
    if (wanted("checkpoint"))
        alternateCheckpointBenches(results, 50, 200, warmup, iters * 10);
    if (wanted("checkpoint"))
        checkpointBenches(results, 80, 300, warmup, iters * 5, "_80x300");
    // The update apply — the steady-state remote hot path on the client.
    if (wanted("update"))
        updateBenches(results, 50, 200, warmup, iters * 50);
    broadcastFrameBenches(results, 50, 200, warmup, iters * 50, "_50x200");
    broadcastFrameBenches(results, 24, 80, warmup, iters * 50);
    // Wire-byte sizes: what a checkpoint, a keystroke, a cursor move and a
    // repaint cost on a wide grid.
    if (wanted("bandwidth"))
        bandwidthBenches(results, 50, 200, warmup, iters);
    // Scrollback ingest + scan — pure CPU/memory, so plenty of iterations settle it.
    if (wanted("scrollback"))
        scrollbackBenches(results, 80, warmup, iters);
    // Find over a power-user scrollback: 10k lines is a session, not a corner.
    // Two buffer sizes, because the cost is linear in the whole scrollback.
    if (wanted("search")) {
        searchBenches(results, 50, 200, 10000, warmup, iters, "_10k");
        searchBenches(results, 50, 200, 100000, warmup, iters, "_100k");
    }
    // Select-all then copy over the same buffer size the find lanes use.
    // Two sizes, as the find lanes carry: Cmd-A then Cmd-C is linear in the
    // whole buffer, and it is a far more common action than a find.
    if (wanted("selection")) {
        selectionBenches(results, 50, 200, 10000, warmup, iters, "_10k");
        selectionBenches(results, 50, 200, 100000, warmup, iters, "_100k");
    }
    // The rich flavours a macOS copy builds beside the plain one.
    if (wanted("styledclipboard"))
        styledClipboardBenches(results, 50, 200, 10000, warmup, iters, "_10k");
    // Render build — the per-frame Cell-grid → GPU-instance pass. Per-frame and
    // fast, so many iterations like the checkpoint benches; two sizes show area
    // scaling (a typical window vs a large one).
    if (wanted("render"))
        renderBenches(results, 24, 80, warmup, iters * 20, PenVariety::PerCell);
    if (wanted("render"))
        renderBenches(results, 50, 200, warmup, iters * 10, PenVariety::PerCell, "_50x200");
    if (wanted("render"))
        renderBenches(results, 24, 80, warmup, iters * 20, PenVariety::Runs);
    // The underline group, which no other distribution moves: every lane above
    // varies a field the colour resolver reads, so the branches keyed on the
    // underline colour train to zero counts and compile cold.
    if (wanted("render"))
        renderBenches(results, 24, 80, warmup, iters * 20, PenVariety::UnderlineVaried);
    if (wanted("renderlink"))
        renderLinkBench(results, 24, 80, warmup, iters * 20);
    if (wanted("render"))
        renderBenches(results, 50, 200, warmup, iters * 10, PenVariety::Runs, "_50x200");
    if (wanted("render"))
        renderBenches(results, 24, 80, warmup, iters * 20, PenVariety::Sparse);
    if (wanted("render"))
        renderBenches(results, 50, 200, warmup, iters * 10, PenVariety::Sparse, "_50x200");
    if (wanted("render"))
        renderBenches(results, 24, 80, warmup, iters * 20, PenVariety::Indexed256);
    if (wanted("render"))
        renderBenches(results, 50, 200, warmup, iters * 10, PenVariety::Indexed256, "_50x200");
    if (wanted("resolvememo"))
        resolveMemoBenches(results, 50, 200, warmup, iters * 10, PenVariety::PerCell);
    if (wanted("resolvememo"))
        resolveMemoBenches(results, 50, 200, warmup, iters * 10, PenVariety::Runs);
    if (wanted("subpixelrender"))
        subpixelRenderBenches(results, 50, 200, warmup, iters * 40, PenVariety::PerCell);
    if (wanted("subpixelrender"))
        subpixelRenderBenches(results, 50, 200, warmup, iters * 40, PenVariety::Runs);
    if (wanted("subpixelrender"))
        subpixelRenderBenches(results, 50, 200, warmup, iters * 40, PenVariety::Runs, true);
    // A screen of s=7 letters and three such screens in turn, at the geometry
    // the other render lanes call large, and a screen small enough for any
    // revision's sheet.
    if (wanted("sizedtext"))
        sizedTextBenches(results, 50, 200, warmup, iters, SizedScreens::One);
    if (wanted("sizedtext"))
        sizedTextBenches(results, 50, 200, warmup, iters, SizedScreens::Cycling);
    if (wanted("sizedtext"))
        sizedTextBenches(results, 15, 71, warmup, iters * 10, SizedScreens::One);
    // Same geometry as render_build_50x200, so the pair differs only by which
    // streams are wired and what the scene carries.
    if (wanted("fullrender"))
        fullRenderBenches(results, 50, 200, warmup, iters * 10);
    if (wanted("capture"))
        captureBenches(results, 50, 200, warmup, iters * 10);
    // Shaped-row cache — getDisplayRow's per-row Arabic-shape/bidi pass, cached
    // vs reshape. Per-frame + fast, so many iterations; a wide row shows the
    // O(columns) shape cost the cache elides.
    // Interactive-resize deferral — width-drag step cost with 10k history,
    // deferred vs full; few iterations (each settled step is a full rewrap).
    if (wanted("reflow"))
        reflowBenches(results, 10000, /*warmup*/ 2, /*iters*/ 15, "_10k");
    if (named("reflow_100k"))
        reflowBenches(results, 100000, /*warmup*/ 1, /*iters*/ 5, "_100k");
    if (named("position"))
        positionBenches(results, 100000, warmup, iters * 10, "_100k");
    if (named("search_start"))
        searchStartBenches(results, 100000, warmup, iters * 10, "_100k");
    if (named("search_flowing_100k"))
        Ubiquitty::Bench::SearchMaintenance::flowing(results, warmup, iters);
    if (named("typing_while_searching_100k"))
        Ubiquitty::Bench::SearchMaintenance::typing(results, warmup, iters);
    if (wanted("shapedrow"))
        shapedRowBenches(results, 80, warmup, iters * 20);
    if (wanted("shapedrow"))
        shapedRowBenches(results, 200, warmup, iters * 10, "_200col");
    // tmux -CC %output pane delivery — payload MiB/s through the primary's
    // control-stream decode + pane route. The lookup memo's per-run resolution
    // (vs per-byte) is the guarded gap.
    if (wanted("tmuxdeliver"))
        tmuxDeliverBenches(results, warmup, iters * 10);
    // Daemon per-frame digest pass — the only hot caller of buildFrameDigests.
    // Two sizes; each emits a scan vs hint pair so the blink-scan cost is visible.
    if (wanted("framedigest"))
        frameDigestBenches(results, 24, 80, warmup, iters * 20);
    if (wanted("framedigest"))
        frameDigestBenches(results, 50, 200, warmup, iters * 10, "_50x200");
    // End-to-end loopback through the real SubprocessChannel (pipes + cat + poll
    // thread). Each send must stay under the channel's outbound cap (maxPendingOut
    // = 4 MiB), and we wait for each to echo before the next, so 2 MiB/op drains
    // cleanly. Fewer iterations — it's I/O-bound and noisier.
    if (wanted("channelloopback"))
        channelLoopbackBench(results, 2u * 1024u * 1024u, /*warmup*/ 2, /*iters*/ 9);
    // Felt-latency + frame-time tail — the "feels instant" / "never
    // stutter" guards. Latency wants many samples for a stable p99; the flood
    // self-supplies its frame count. ms metrics ⇒ compare-bench gates lower-better.
    if (wanted("latency"))
        latencyBenches(results, 50, 200, warmup, iters * 50);
    if (wanted("frametail"))
        frameTailBenches(results, 50, 200);
    // Model-lock starvation under flood — T7's motivating problem, the "before"
    // half of the one-data-path before/after.
    // This external-mutex model omits shipping drain fairness and
    // feeds bytes until a wall-clock deadline. Its Core work is already trained
    // by parse_plain/parse_scroll, the 4096-byte frame-tail feed, shaped-row reads
    // and live capture. Excluding it avoids load-dependent corpus weights without
    // inventing a replacement ratio of parser calls to display captures.
    if (!Ubiquitty::Bench::isProfileTraining && wanted("lockstarvation"))
        lockStarvationBenches(results, 24, 80, floodSeconds);

    Ubiquitty::Bench::emitJSON(results, platform, commit);
    return 0;
}
