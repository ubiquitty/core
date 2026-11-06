// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/App/WelcomeScreen.cpp — see Core/App/WelcomeScreen.h.

#include "Core/App/WelcomeScreen.h"

#include "Core/App/WelcomeIconData.h"
#include "Core/Codec/Base64.h"
#include "Core/Codec/PNGDecoder.h"
#include "Core/Codec/ZlibInflate.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Terminal.h"
#include "PAL/Capability.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::App::Welcome {

namespace {

constexpr std::string_view actionPrefix = "ubiquitty:action/";

struct ActionName {
    Action action;
    std::string_view name;
};

// The whole vocabulary. A click is matched against these names and the screen
// builds its targets from them, so a button cannot carry a name no click
// resolves.
constexpr std::array<ActionName, 4> actionNames { {
    { Action::ShellIntegration, "shell-integration" },
    { Action::Connect, "connect" },
    { Action::Settings, "settings" }, // preferences-vocabulary-ok: the button's own name, for the macOS window
    { Action::Accessibility, "accessibility" },
} };

std::string_view nameOfAction(Action action)
{
    for (const ActionName& entry : actionNames) {
        if (entry.action == action)
            return entry.name;
    }
    return {};
}

// The screen's palette, in truecolor SGR 38/48 alone: every terminal that
// renders this screen understands them, and the first thing a new reader sees
// does not depend on a sequence only this terminal draws. The periwinkle
// (136,136,190) is the accent everything clickable and the tagline wear; the
// buttons' plates and labels are derived from it and the pane's ground (see
// ``buttonColours``).
constexpr ColorRGBA accent { 136, 136, 190 };
constexpr std::string_view accentForeground = "\x1b[38;2;136;136;190m";
constexpr std::string_view reset = "\x1b[0m";

// The widest grid the screen lays out for; a narrower pane uses its own width.
constexpr int screenColumns = 80;

// The rows left above the screen when the grid has at least two to spare,
// so a row still follows the prompt: at 80 by 24 the screen and the prompt
// take 21 rows, and one goes above, two below.
constexpr int topMarginRows = 1;

// The logo's box: 16 cells by 8 rows, centred on the screen's 80 columns, with
// the blank row below it and the name after that.
constexpr int logoColumns = 16;
constexpr int logoRowCount = 8;

// The smallest icon worth drawing, in device pixels. Below it a squircle
// reads as a smudge, and the screen leaves the logo's rows out instead.
constexpr double smallestIconPixels = 32;

// The nominal cell of the default face, in ems: its advance, and its line
// before the configured spacing, whole points rounded up — 7.2 pt by 16 + 2 pt
// at 12 pt, 8.4 by 19 + 2 at 14, as the face measures. It sizes the icon only
// for a pane that has not published its own cell.
constexpr double nominalAdvanceEms = 0.6;
constexpr double nominalLineEms = 1.32;

// Moves the cursor to the column that centres `width` cells on the row.
void appendCentering(std::string& output, int columns, int width)
{
    const int column = std::max(0, (columns - width) / 2) + 1;
    output += "\x1b[";
    output += std::to_string(column);
    output += 'G';
}

// A local program's image is in the rendering tier, which Local allows and
// no configuration overrides, and the kitty graphics transfer the icon rides
// has no sequence gate: what can withhold the image is the arm, not a policy.
static_assert(defaultDecision(tierOf(Capability::ImageRender), CapabilityOrigin::Local) == CapabilityDecision::Allow
    && !isConfigurableTier(tierOf(Capability::ImageRender)));

// The decoder refuses an image past these, and the screen would then show no
// logo at all; the assets are fixed, so this is answered once, here.
constexpr bool everyIconFitsTheDecoder()
{
    for (const auto* table : { &IconData::darkGroundIcons, &IconData::lightGroundIcons }) {
        for (const IconData::IconAsset& asset : *table) {
            // The composite the dark-ground layers make is the same size,
            // straight RGBA, as the transfer that carries it.
            const std::uint64_t bytes = static_cast<std::uint64_t>(asset.canvasPixelSize) * static_cast<std::uint64_t>(asset.canvasPixelSize) * 4;
            if (bytes > static_cast<std::uint64_t>(Image::maxDecodedImageBytes) || bytes > Image::terminalImageBytesBudgetDefault)
                return false;
        }
    }
    return true;
}
static_assert(everyIconFitsTheDecoder());

// Where the icon's canvas goes, in the units the placement states them in.
struct IconPlacement {
    // The rows the logo takes before the blank row and the name.
    int logoRows = logoRowCount;
    const IconData::IconAsset* asset = nullptr;
    // The canvas's width in cells, its height following its square aspect.
    int canvasColumns = 0;
    // The zero-based cell the canvas's top-left corner is placed from.
    int column = 0;
    int row = 0;
    // The canvas's offset within that cell, in device pixels.
    int offsetX = 0;
    int offsetY = 0;
    // A layered render composited for the pane's ground, straight RGBA, the
    // canvas's side square; empty for a picture sent as its PNG.
    std::vector<std::uint8_t> composite;
};

// sRGB's transfer, one channel, gamma-encoded 0-1 to linear and back.
double linearFromEncoded(double value) { return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4); }
double encodedFromLinear(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - 0.055;
}

// A colour in OKLab: perceptual lightness and the two opponent axes.
struct OKLab {
    double lightness;
    double a;
    double b;
};

OKLab oklabFrom(ColorRGBA colour)
{
    const double red = linearFromEncoded(colour.red / 255.0);
    const double green = linearFromEncoded(colour.green / 255.0);
    const double blue = linearFromEncoded(colour.blue / 255.0);
    const double l = std::cbrt(0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue);
    const double m = std::cbrt(0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue);
    const double s = std::cbrt(0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue);
    return { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s };
}

ColorRGBA colourFrom(OKLab lab)
{
    const double l = std::pow(lab.lightness + 0.3963377774 * lab.a + 0.2158037573 * lab.b, 3);
    const double m = std::pow(lab.lightness - 0.1055613458 * lab.a - 0.0638541728 * lab.b, 3);
    const double s = std::pow(lab.lightness - 0.0894841775 * lab.a - 1.2914855480 * lab.b, 3);
    const auto channel = [](double linear) { return static_cast<std::uint8_t>(std::lround(encodedFromLinear(linear) * 255)); };
    return { channel(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s), channel(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
        channel(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s) };
}

// The ground moved `lightnessStep` in OKLab lightness, its hue kept and its
// chroma cut to `chromaShare`.
ColorRGBA shadeOf(ColorRGBA ground, double lightnessStep, double chromaShare)
{
    const OKLab lab = oklabFrom(ground);
    return colourFrom({ lab.lightness + lightnessStep, lab.a * chromaShare, lab.b * chromaShare });
}

} // namespace

TileTone tileTone(ColorRGBA ground)
{
    return { shadeOf(ground, tileLightnessStep, tileChromaShare), shadeOf(ground, tileLightnessStep - tileGradientDepth, tileChromaShare) };
}

namespace {

// One PNG of the icon decoded to RGBA through the decoder a program's image
// goes through, or empty when it does not decode to the side it should.
std::vector<std::uint8_t> decodedLayer(std::string_view png, int side)
{
    const std::size_t pixelBytes = static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4;
    // The pixels and the decoder's filter workspace, which is about a row per
    // row of the image again.
    std::vector<std::uint8_t> scratch(pixelBytes * 3 + 65536);
    Codec::Zlib::Inflater inflater;
    const Codec::PNG::DecodeResult result = Codec::PNG::decode(
        { reinterpret_cast<const std::uint8_t*>(png.data()), png.size() }, scratch, inflater);
    if (result.status != Codec::PNG::DecodeStatus::Success || result.width != side || result.height != side || result.bytesPerPixel != 4)
        return {};
    return { result.pixels.begin(), result.pixels.begin() + static_cast<std::ptrdiff_t>(pixelBytes) };
}

// A layered render composited for `ground`: the tile in the ground's tone,
// lighter at the top as the document's fill is, then the rim, highlights and
// mark the layers carry above it, then the shadow underneath, as straight
// RGBA. Empty when a layer does not decode.
std::vector<std::uint8_t> compositeIcon(const IconData::IconAsset& asset, ColorRGBA ground)
{
    const int side = asset.canvasPixelSize;
    const std::vector<std::uint8_t> above = decodedLayer(asset.png, side);
    const std::vector<std::uint8_t> coverage = decodedLayer(asset.coverage, side);
    if (above.empty() || coverage.empty())
        return {};
    const TileTone tone = tileTone(ground);
    const int inset = (side - asset.pixelSize) / 2;
    std::vector<std::uint8_t> composite(above.size());
    for (int y = 0; y < side; ++y) {
        // The fill's gradient runs over the top 70% of the icon, as the
        // document's does.
        const double along = std::clamp((y - inset + 0.5) / (asset.pixelSize * 0.7), 0.0, 1.0);
        const std::array<double, 3> fill { tone.top.red + (tone.bottom.red - tone.top.red) * along,
            tone.top.green + (tone.bottom.green - tone.top.green) * along, tone.top.blue + (tone.bottom.blue - tone.top.blue) * along };
        for (int x = 0; x < side; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x)) * 4;
            const double shape = coverage[index] / 255.0;
            const double shadow = coverage[index + 1] / 255.0;
            const double share = above[index + 3] / 255.0;
            const double alpha = shape + shadow * (1 - shape);
            std::uint8_t* out = composite.data() + index;
            out[3] = static_cast<std::uint8_t>(std::lround(alpha * 255));
            for (std::size_t channel = 0; channel < 3; ++channel) {
                // Premultiplied: what sits above the fill, plus the fill's
                // share; the shadow beneath is black and adds no colour.
                const double premultiplied = above[index + channel] + fill[channel] * share;
                out[channel] = alpha > 0 ? static_cast<std::uint8_t>(std::lround(std::clamp(premultiplied / alpha, 0.0, 255.0))) : 0;
            }
        }
    }
    return composite;
}

// The render whose icon side is nearest at or above `side` device pixels, or
// the largest when none is: drawn at its own size it is one texel a pixel,
// and otherwise the renderer filters it down rather than up.
const IconData::IconAsset& iconAsset(const decltype(IconData::darkGroundIcons)& table, double side)
{
    for (const IconData::IconAsset& asset : table) {
        if (static_cast<double>(asset.pixelSize) + 0.5 >= side)
            return asset;
    }
    return table.back();
}

// The canvas's side over the icon's: what the shadow adds.
double canvasRatio(const IconData::IconAsset& asset)
{
    return static_cast<double>(asset.canvasPixelSize) / static_cast<double>(asset.pixelSize);
}

// The icon for `pane`, or nothing when the screen leaves the logo's rows out.
// Rec. 709 luma below half is a dark ground, the rule a theme's own darkness
// is computed by.
//
// The icon is as large as its box allows — 16 cells by 8 rows, or the space a
// smaller grid has: the screen's width and the rows above the name — at whole
// columns of its canvas, so the placement states its size exactly. Across, it
// is centred on the screen; down, in its rows and the blank row under them, so
// the space above it equals the gap to the name. An arm that draws no images,
// or a space too small for a recognisable icon, gets no logo rows at all.
std::optional<IconPlacement> iconPlacement(const Pane& pane)
{
    if (pane.imageDrawing != ImageDrawing::Drawn)
        return std::nullopt;
    if (pane.cellPixelWidth <= 0 || pane.cellPixelHeight <= 0 || pane.deviceScale <= 0)
        return std::nullopt;
    const double cellWidth = pane.cellPixelWidth;
    const double cellHeight = pane.cellPixelHeight;
    // The blank row and the name stay on screen below the logo.
    const int logoRows = std::min(logoRowCount, pane.rows - 2);
    const int layoutColumns = std::min(pane.columns, screenColumns);
    if (logoRows <= 0 || layoutColumns <= 0)
        return std::nullopt;
    const auto& table = perceivedLightness(pane.ground) < 128 ? IconData::darkGroundIcons : IconData::lightGroundIcons;
    const double ratio = canvasRatio(table[1]);
    // The canvas must fit the screen's width, and down to the name: a canvas
    // taller than the rows above the name would start above the first row,
    // which no placement can, and the icon would sit off centre.
    const double side = std::min({ cellWidth * logoColumns, cellHeight * logoRows, cellWidth * layoutColumns / ratio, cellHeight * (logoRows + 1) / ratio });

    IconPlacement placement;
    placement.logoRows = logoRows;
    placement.canvasColumns = std::min(layoutColumns, static_cast<int>(std::floor((side + 0.5) * ratio / cellWidth)));
    const double canvasWidth = cellWidth * placement.canvasColumns;
    if (placement.canvasColumns <= 0 || canvasWidth / ratio < smallestIconPixels)
        return std::nullopt;
    placement.asset = &iconAsset(table, canvasWidth / ratio);
    if (!placement.asset->coverage.empty()) {
        placement.composite = compositeIcon(*placement.asset, pane.ground);
        if (placement.composite.empty())
            return std::nullopt;
    }
    const double iconSide = canvasWidth / canvasRatio(*placement.asset);

    const double canvasLeft = (cellWidth * layoutColumns - canvasWidth) / 2;
    placement.column = std::max(0, static_cast<int>(std::floor(canvasLeft / cellWidth)));
    const double nameTop = cellHeight * (logoRows + 1);
    const double canvasTop = std::max(0.0, (nameTop - iconSide) / 2 - (canvasWidth - iconSide) / 2);
    placement.row = static_cast<int>(std::floor(canvasTop / cellHeight));
    placement.offsetX = static_cast<int>(std::lround(std::max(0.0, canvasLeft - placement.column * cellWidth)));
    placement.offsetY = static_cast<int>(std::lround(canvasTop - placement.row * cellHeight));
    return placement;
}

// The icon as a kitty graphics transfer of the PNG, displayed at once: sized
// by its canvas's columns so it keeps its aspect, offset to its place, the
// cursor left where it was, and no reply. The PNG goes as base64 in blocks of
// the protocol's 4096 characters, 3072 bytes each. Then the cursor goes to the
// start of the row below the logo's rows. `topRows` is the rows left above
// the logo's box.
void appendLogoImage(std::string& output, const IconPlacement& placement, int topRows)
{
    constexpr std::size_t blockBytes = 3072;
    output += "\x1b[";
    output += std::to_string(topRows + placement.row + 1);
    output += ';';
    output += std::to_string(placement.column + 1);
    output += 'H';
    // A composite goes as its RGBA, which needs its size; a picture as its PNG.
    const bool isComposite = !placement.composite.empty();
    const std::string_view payload = isComposite
        ? std::string_view(reinterpret_cast<const char*>(placement.composite.data()), placement.composite.size())
        : placement.asset->png;
    for (std::size_t start = 0; start < payload.size(); start += blockBytes) {
        const bool isFirst = start == 0;
        const bool isLast = start + blockBytes >= payload.size();
        output += "\x1b_G";
        if (isFirst) {
            if (isComposite) {
                output += "a=T,f=32,s=";
                output += std::to_string(placement.asset->canvasPixelSize);
                output += ",v=";
                output += std::to_string(placement.asset->canvasPixelSize);
                output += ",q=2,C=1,c=";
            } else {
                output += "a=T,f=100,q=2,C=1,c=";
            }
            output += std::to_string(placement.canvasColumns);
            if (placement.offsetX > 0) {
                output += ",X=";
                output += std::to_string(placement.offsetX);
            }
            if (placement.offsetY > 0) {
                output += ",Y=";
                output += std::to_string(placement.offsetY);
            }
            output += ',';
        }
        output += isLast ? "m=0;" : "m=1;";
        const std::string_view block = payload.substr(start, blockBytes);
        const std::size_t at = output.size();
        output.resize(at + Codec::base64EncodedLength(block.size()));
        const auto written = Codec::base64Encode(block, { output.data() + at, output.size() - at });
        output.resize(at + (written ? written->size() : 0));
        output += "\x1b\\";
    }
    output += "\x1b[";
    output += std::to_string(topRows + placement.logoRows + 1);
    output += ";1H";
}

// Where a run's text sits in its claim: `OSC 66`'s `h`.
enum class RunAlignment : std::uint8_t {
    Left = 0,
    Right = 1,
    Centre = 2,
};

// A proportional run in a claim of `cells` cells (`OSC 66`, `p=1`), set in
// the proportional face the frontend draws runs with. A renderer without
// sized text prints the text plainly at the cursor.
void appendProportional(std::string& output, int cells, std::string_view text, RunAlignment alignment = RunAlignment::Centre)
{
    output += "\x1b]66;p=1:c=";
    output += std::to_string(cells);
    output += ":h=";
    output += std::to_string(static_cast<int>(alignment));
    output += ';';
    output += text;
    output += "\x1b\\";
}

// One of what the terminal is, as a line of the Welcome screen: the name set
// bold and ranged right against the gap, the clause set plain from the gap,
// so the four lines read as one list. Two runs, each aligned in its own
// claim, which is the alignment the protocol offers put to use.
void appendPillar(std::string& output, int columns, std::string_view name, std::string_view clause)
{
    constexpr int nameCells = 13;
    constexpr int gapCells = 2;
    const int totalCells = std::min(columns, 76);
    const int clauseCells = totalCells - nameCells - gapCells;
    appendCentering(output, columns, totalCells);
    output += "\x1b[1m";
    output += accentForeground;
    appendProportional(output, nameCells, name, RunAlignment::Right);
    output += reset;
    output += "  ";
    appendProportional(output, clauseCells, clause, RunAlignment::Left);
    output += "\r\n";
}

// The name across a double-height pair: the same run on two consecutive rows,
// the first tagged top half (ESC # 3) and the second bottom half (ESC # 4),
// which the renderer draws as one line twice the size. A double-height row
// has half the grid's columns, so a claim of half the width is the whole row.
// Bold in the pane's default foreground, the body text's colour: no colour is
// set, so the name follows the theme the pane is drawn in, re-themed or not.
void appendDoubleHeightName(std::string& output, int columns, std::string_view text)
{
    for (const std::string_view tag : { std::string_view { "\x1b#3" }, std::string_view { "\x1b#4" } }) {
        output += tag;
        output += "\x1b[1m";
        appendProportional(output, std::max(1, columns / 2), text);
        output += reset;
        output += "\r\n";
    }
}

// An `OSC 8` link set as a button: a proportional run centred on a plate the
// width of its claim, the target the action's name under the prefix. The
// plate is the run's background, so the button is its whole claim and a click
// anywhere on it is the action. The renderer knows the `ubiquitty:` scheme as
// a control: the label keeps these colours with no link underline, and the
// plate lightens under the pointer.
enum class ButtonWeight : std::uint8_t {
    Plain,
    // The one action a first-launch reader is asked to take, set bold.
    Primary,
};

// WCAG 2 relative luminance of a gamma-encoded colour.
double relativeLuminance(ColorRGBA colour)
{
    const auto linear = [](std::uint8_t channel) {
        const double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(colour.red) + 0.7152 * linear(colour.green) + 0.0722 * linear(colour.blue);
}

double contrastRatio(ColorRGBA first, ColorRGBA second)
{
    const double a = relativeLuminance(first);
    const double b = relativeLuminance(second);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

// `from` moved `amount` of the way to `to`, channel by channel.
ColorRGBA mixColour(ColorRGBA from, ColorRGBA to, double amount)
{
    const auto channel = [amount](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(std::lround(a + (b - a) * amount));
    };
    return { channel(from.red, to.red), channel(from.green, to.green), channel(from.blue, to.blue) };
}

// The text contrast WCAG asks of a label: AA for body text.
constexpr double labelContrast = 4.5;

// `ink`, moved toward `pole` just far enough to read on `plate`.
ColorRGBA legibleOn(ColorRGBA ink, ColorRGBA plate, ColorRGBA pole)
{
    for (int step = 0; step <= 64; ++step) {
        const ColorRGBA candidate = mixColour(ink, pole, step / 64.0);
        if (contrastRatio(candidate, plate) >= labelContrast)
            return candidate;
    }
    return pole;
}

// A button's plate and label.
struct ButtonColours {
    ColorRGBA plate;
    ColorRGBA label;
};

// The buttons' colours, from the pane's ground and the accent by one rule for
// either kind of ground: a plain plate is the ground moved toward the far
// pole — lighter on a dark ground, darker on a light one — a primary plate is
// the ground tinted with the accent, and every label is the accent moved
// toward the far pole until it reads on its plate at WCAG AA.
ButtonColours buttonColours(ColorRGBA ground, ButtonWeight weight)
{
    const bool isDarkGround = perceivedLightness(ground) < 128;
    const ColorRGBA pole = isDarkGround ? ColorRGBA { 255, 255, 255 } : ColorRGBA { 0, 0, 0 };
    const ColorRGBA plate = weight == ButtonWeight::Primary
        ? mixColour(ground, accent, isDarkGround ? 0.28 : 0.24)
        : mixColour(ground, pole, isDarkGround ? 0.14 : 0.07);
    return { plate, legibleOn(accent, plate, pole) };
}

void appendColour(std::string& output, std::string_view introducer, ColorRGBA colour)
{
    output += introducer;
    output += std::to_string(colour.red);
    output += ';';
    output += std::to_string(colour.green);
    output += ';';
    output += std::to_string(colour.blue);
    output += 'm';
}

void appendButton(std::string& output, Action action, std::string_view label, int cells, ButtonWeight weight, ColorRGBA ground)
{
    const ButtonColours colours = buttonColours(ground, weight);
    appendColour(output, "\x1b[48;2;", colours.plate);
    appendColour(output, "\x1b[38;2;", colours.label);
    if (weight == ButtonWeight::Primary)
        output += "\x1b[1m";
    output += "\x1b]8;;";
    output += actionPrefix;
    output += nameOfAction(action);
    output += "\x1b\\";
    appendProportional(output, cells, label);
    output += "\x1b]8;;\x1b\\";
    output += reset;
}

} // namespace

bool isActionURI(std::string_view uri) { return uri.starts_with(actionPrefix); }

Action actionForURI(std::string_view uri)
{
    if (!isActionURI(uri))
        return Action::Unrecognized;
    const std::string_view name = uri.substr(actionPrefix.size());
    for (const ActionName& entry : actionNames) {
        if (entry.name == name)
            return entry.action;
    }
    return Action::Unrecognized;
}

Pane paneOf(const Terminal& terminal, const Preferences::Values& config, double deviceScale, ImageDrawing imageDrawing)
{
    Pane pane;
    // The palette's channels are sixteen bits, as OSC carries them.
    pane.ground = { static_cast<std::uint8_t>(terminal.palette.defaultBackground.red >> 8),
        static_cast<std::uint8_t>(terminal.palette.defaultBackground.green >> 8),
        static_cast<std::uint8_t>(terminal.palette.defaultBackground.blue >> 8) };
    pane.imageDrawing = imageDrawing;
    pane.rows = terminal.rows;
    pane.columns = terminal.columns;
    // The text area over the grid rather than the one-cell report: the area
    // is rounded once, so dividing it recovers the fractional advance the
    // grid is drawn at, where the cell report is rounded to a whole pixel.
    if (terminal.columns > 0 && terminal.rows > 0 && terminal.textAreaPixelWidth > 0 && terminal.textAreaPixelHeight > 0) {
        pane.cellPixelWidth = static_cast<double>(terminal.textAreaPixelWidth) / terminal.columns;
        pane.cellPixelHeight = static_cast<double>(terminal.textAreaPixelHeight) / terminal.rows;
        pane.isGeometryPublished = true;
    } else {
        // Nothing published yet: the configured face's nominal cell, so a
        // screen written before the frontend measures still has an icon.
        const double fontSize = Preferences::Effective::fontSize(config);
        pane.cellPixelWidth = fontSize * nominalAdvanceEms * deviceScale;
        pane.cellPixelHeight = (std::ceil(fontSize * nominalLineEms) + Preferences::Effective::lineSpacing(config)) * deviceScale;
    }
    pane.deviceScale = deviceScale;
    return pane;
}

namespace {

// Everything below the logo: the name, the tagline, the four pillars and the
// buttons, ending at the start of the row the prompt takes.
void appendScreenBody(std::string& output, const Pane& pane)
{
    const int columns = std::clamp(pane.columns, 1, screenColumns);
    appendDoubleHeightName(output, columns, "Ubiquitty");

    output += accentForeground;
    appendProportional(output, columns, "the terminal that keeps up");
    output += reset;
    output += "\r\n";
    output += "\r\n";

    // The clauses need room beside their names. On a small pane, keep the
    // actions visible rather than letting four wrapped clauses scroll them away.
    if (columns >= 64) {
        if (columns >= 76) {
            appendPillar(output, columns, "Fast", "parses at the wire's pace and draws every frame on the GPU");
            appendPillar(output, columns, "Typography", "proportional, sized and double-height text, real fonts, ligatures");
            appendPillar(output, columns, "Multiplexer", "workspaces, windows and panes live on the host, past any client");
            appendPillar(output, columns, "Together", "humans and AI agents share one terminal, one screen");
        } else {
            appendPillar(output, columns, "Fast", "parses and draws at the wire's pace");
            appendPillar(output, columns, "Typography", "proportional and sized text, real fonts");
            appendPillar(output, columns, "Multiplexer", "sessions live on the host, past any client");
            appendPillar(output, columns, "Together", "people and agents share one screen");
        }
        output += "\r\n";
    }

    // Four buttons, centred in one row where they fit and split below it.
    constexpr int installCells = 26;
    constexpr int connectCells = 14;
    constexpr int settingsCells = 10;
    constexpr int accessibilityCells = 15;
    constexpr int gapCells = 2;
    const auto button = [&](Action action, std::string_view label, int width, ButtonWeight weight) {
        appendButton(output, action, label, std::min(width, columns), weight, pane.ground);
    };
    if (columns >= installCells + connectCells + settingsCells + accessibilityCells + 3 * gapCells) {
        appendCentering(output, columns, installCells + connectCells + settingsCells + accessibilityCells + 3 * gapCells);
        button(Action::ShellIntegration, "Install shell integration", installCells, ButtonWeight::Primary);
        output += "  ";
        button(Action::Connect, "⌘⇧K  Connect", connectCells, ButtonWeight::Plain);
        output += "  ";
        button(Action::Settings, "Settings", settingsCells, ButtonWeight::Plain); // preferences-vocabulary-ok: the label a reader sees
        output += "  ";
        button(Action::Accessibility, "Accessibility", accessibilityCells, ButtonWeight::Plain);
    } else if (columns >= installCells + connectCells + gapCells) {
        appendCentering(output, columns, installCells + connectCells + gapCells);
        button(Action::ShellIntegration, "Install shell integration", installCells, ButtonWeight::Primary);
        output += "  ";
        button(Action::Connect, "⌘⇧K  Connect", connectCells, ButtonWeight::Plain);
        output += "\r\n";
        appendCentering(output, columns, settingsCells + accessibilityCells + gapCells);
        button(Action::Settings, "Settings", settingsCells, ButtonWeight::Plain); // preferences-vocabulary-ok: the label a reader sees
        output += "  ";
        button(Action::Accessibility, "Accessibility", accessibilityCells, ButtonWeight::Plain);
    } else {
        const auto row = [&](Action action, std::string_view label, int width, ButtonWeight weight) {
            appendCentering(output, columns, std::min(width, columns));
            button(action, label, width, weight);
            output += "\r\n";
        };
        row(Action::ShellIntegration, "Install shell integration", installCells, ButtonWeight::Primary);
        row(Action::Connect, "⌘⇧K  Connect", connectCells, ButtonWeight::Plain);
        row(Action::Settings, "Settings", settingsCells, ButtonWeight::Plain); // preferences-vocabulary-ok: the label a reader sees
        appendCentering(output, columns, std::min(accessibilityCells, columns));
        button(Action::Accessibility, "Accessibility", accessibilityCells, ButtonWeight::Plain);
    }
    output += "\r\n";
    output += "\r\n";
}

// The rows of `text` its line breaks move the cursor down.
int rowsDown(std::string_view text)
{
    int rows = 0;
    for (std::size_t at = text.find("\r\n"); at != std::string_view::npos; at = text.find("\r\n", at + 2))
        ++rows;
    return rows;
}

} // namespace

std::string screenContent(const Pane& pane)
{
    const std::optional<IconPlacement> icon = iconPlacement(pane);
    // The body is written first so its rows can be counted: the logo's rows,
    // the body's and the prompt's are what the screen takes.
    std::string body;
    body.reserve(2048);
    appendScreenBody(body, pane);
    const int screenRows = (icon ? icon->logoRows + 1 : 0) + rowsDown(body) + 1;
    const int topRows = pane.rows - screenRows >= 2 ? topMarginRows : 0;

    std::string output;
    const std::size_t payloadBytes = icon ? (icon->composite.empty() ? icon->asset->png.size() : icon->composite.size()) : 0;
    output.reserve(body.size() + 64 + Codec::base64EncodedLength(payloadBytes) + payloadBytes / 128);
    output += "\x1b[H\x1b[2J";

    // No icon, no logo rows: the name opens the screen.
    if (icon) {
        appendLogoImage(output, *icon, topRows);
        output += "\r\n";
    } else if (topRows > 0) {
        output += "\x1b[";
        output += std::to_string(topRows + 1);
        output += ";1H";
    }
    output += body;
    return output;
}

} // namespace Ubiquitty::App::Welcome
