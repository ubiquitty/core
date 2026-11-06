// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A conforming answer reproduces Xlib's own arithmetic rather than "converting to sRGB":
//   * The "#" form is left-justified, never scaled (libX11 ParseCol.c shifts rather than scaling).
//   * The device-independent forms go through Xcms with the *default* screen characterization
//     libX11 ships when the X server publishes no XDCCC properties: the Tektronix 19" (Sony) CRT
//     primaries and its measured per-gun intensity tables (libX11 src/xcms/LRGB.c). Those tables,
//     not a gamma curve, are the transfer function.
//   * Colors outside the display gamut are compressed with XcmsTekHVCClipC — libX11's default
//     gamut-compression procedure — which holds hue and value and pulls chroma back to the boundary.
//   * The result lands on an 8-bit-per-channel visual, so every channel a client reads back is a
//     replicated byte (0xNN -> 0xNNNN). That quantization is why #fff reads back as f0f0.
//
// Cold path: only OSC color parsing reaches here, so doubles and table searches cost nothing that
// matters. Nothing in this file may be called per cell or per byte.

#include "Core/OSC/XParseColor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <optional>
#include <string_view>

namespace Ubiquitty::OSC::XParseColor {
namespace {

// One row of a gun's measured transfer table: a device value and the light intensity it emits.
struct IntensityRecord {
    uint16_t value;
    double intensity;
};

// libX11 src/xcms/LRGB.c, Default_RGB_{Red,Green,Blue}Tuples — Tektronix 19" (Sony) CRT,
// part 119-2451-00, models Tek4300/Tek4800. Non-decreasing in both columns; the leading run of
// zero-intensity entries is real (the gun emits no light below its cutoff).
inline constexpr IntensityRecord redIntensities[] = {
    { 0x0000, 0.000000 },
    { 0x0909, 0.000000 },
    { 0x0a0a, 0.000936 },
    { 0x0f0f, 0.001481 },
    { 0x1414, 0.002329 },
    { 0x1919, 0.003529 },
    { 0x1e1e, 0.005127 },
    { 0x2323, 0.007169 },
    { 0x2828, 0.009699 },
    { 0x2d2d, 0.012759 },
    { 0x3232, 0.016392 },
    { 0x3737, 0.020637 },
    { 0x3c3c, 0.025533 },
    { 0x4141, 0.031119 },
    { 0x4646, 0.037431 },
    { 0x4b4b, 0.044504 },
    { 0x5050, 0.052373 },
    { 0x5555, 0.061069 },
    { 0x5a5a, 0.070624 },
    { 0x5f5f, 0.081070 },
    { 0x6464, 0.092433 },
    { 0x6969, 0.104744 },
    { 0x6e6e, 0.118026 },
    { 0x7373, 0.132307 },
    { 0x7878, 0.147610 },
    { 0x7d7d, 0.163958 },
    { 0x8282, 0.181371 },
    { 0x8787, 0.199871 },
    { 0x8c8c, 0.219475 },
    { 0x9191, 0.240202 },
    { 0x9696, 0.262069 },
    { 0x9b9b, 0.285089 },
    { 0xa0a0, 0.309278 },
    { 0xa5a5, 0.334647 },
    { 0xaaaa, 0.361208 },
    { 0xafaf, 0.388971 },
    { 0xb4b4, 0.417945 },
    { 0xb9b9, 0.448138 },
    { 0xbebe, 0.479555 },
    { 0xc3c3, 0.512202 },
    { 0xc8c8, 0.546082 },
    { 0xcdcd, 0.581199 },
    { 0xd2d2, 0.617552 },
    { 0xd7d7, 0.655144 },
    { 0xdcdc, std::numbers::ln2 },
    { 0xe1e1, 0.734031 },
    { 0xe6e6, 0.775322 },
    { 0xebeb, 0.817837 },
    { 0xf0f0, 0.861571 },
    { 0xf5f5, 0.906515 },
    { 0xfafa, 0.952662 },
    { 0xffff, 1.000000 },
};

inline constexpr IntensityRecord greenIntensities[] = {
    { 0x0000, 0.000000 },
    { 0x1313, 0.000000 },
    { 0x1414, 0.000832 },
    { 0x1919, 0.001998 },
    { 0x1e1e, 0.003612 },
    { 0x2323, 0.005736 },
    { 0x2828, 0.008428 },
    { 0x2d2d, 0.011745 },
    { 0x3232, 0.015740 },
    { 0x3737, 0.020463 },
    { 0x3c3c, 0.025960 },
    { 0x4141, 0.032275 },
    { 0x4646, 0.039449 },
    { 0x4b4b, 0.047519 },
    { 0x5050, 0.056520 },
    { 0x5555, 0.066484 },
    { 0x5a5a, 0.077439 },
    { 0x5f5f, 0.089409 },
    { 0x6464, 0.102418 },
    { 0x6969, 0.116485 },
    { 0x6e6e, 0.131625 },
    { 0x7373, 0.147853 },
    { 0x7878, 0.165176 },
    { 0x7d7d, 0.183604 },
    { 0x8282, 0.203140 },
    { 0x8787, 0.223783 },
    { 0x8c8c, 0.245533 },
    { 0x9191, 0.268384 },
    { 0x9696, 0.292327 },
    { 0x9b9b, std::numbers::inv_pi },
    { 0xa0a0, 0.343441 },
    { 0xa5a5, 0.370580 },
    { 0xaaaa, 0.398747 },
    { 0xafaf, 0.427919 },
    { 0xb4b4, 0.458068 },
    { 0xb9b9, 0.489165 },
    { 0xbebe, 0.521176 },
    { 0xc3c3, 0.554067 },
    { 0xc8c8, 0.587797 },
    { 0xcdcd, 0.622324 },
    { 0xd2d2, 0.657604 },
    { 0xd7d7, std::numbers::ln2 },
    { 0xdcdc, 0.730225 },
    { 0xe1e1, 0.767459 },
    { 0xe6e6, 0.805235 },
    { 0xebeb, 0.843491 },
    { 0xf0f0, 0.882164 },
    { 0xf5f5, 0.921187 },
    { 0xfafa, 0.960490 },
    { 0xffff, 1.000000 },
};

inline constexpr IntensityRecord blueIntensities[] = {
    { 0x0000, 0.000000 },
    { 0x0e0e, 0.000000 },
    { 0x0f0f, 0.001341 },
    { 0x1414, 0.002080 },
    { 0x1919, 0.003188 },
    { 0x1e1e, 0.004729 },
    { 0x2323, 0.006766 },
    { 0x2828, 0.009357 },
    { 0x2d2d, 0.012559 },
    { 0x3232, 0.016424 },
    { 0x3737, 0.021004 },
    { 0x3c3c, 0.026344 },
    { 0x4141, 0.032489 },
    { 0x4646, 0.039481 },
    { 0x4b4b, 0.047357 },
    { 0x5050, 0.056154 },
    { 0x5555, 0.065903 },
    { 0x5a5a, 0.076634 },
    { 0x5f5f, 0.088373 },
    { 0x6464, 0.101145 },
    { 0x6969, 0.114968 },
    { 0x6e6e, 0.129862 },
    { 0x7373, 0.145841 },
    { 0x7878, 0.162915 },
    { 0x7d7d, 0.181095 },
    { 0x8282, 0.200386 },
    { 0x8787, 0.220791 },
    { 0x8c8c, 0.242309 },
    { 0x9191, 0.264937 },
    { 0x9696, 0.288670 },
    { 0x9b9b, 0.313499 },
    { 0xa0a0, 0.339410 },
    { 0xa5a5, 0.366390 },
    { 0xaaaa, 0.394421 },
    { 0xafaf, 0.423481 },
    { 0xb4b4, 0.453547 },
    { 0xb9b9, 0.484592 },
    { 0xbebe, 0.516587 },
    { 0xc3c3, 0.549498 },
    { 0xc8c8, 0.583291 },
    { 0xcdcd, 0.617925 },
    { 0xd2d2, 0.653361 },
    { 0xd7d7, 0.689553 },
    { 0xdcdc, 0.726454 },
    { 0xe1e1, 0.764013 },
    { 0xe6e6, 0.802178 },
    { 0xebeb, 0.840891 },
    { 0xf0f0, 0.880093 },
    { 0xf5f5, 0.919723 },
    { 0xfafa, 0.959715 },
    { 0xffff, 1.000000 },
};

inline constexpr int redIntensityCount = static_cast<int>(sizeof(redIntensities) / sizeof(redIntensities[0]));
inline constexpr int greenIntensityCount = static_cast<int>(sizeof(greenIntensities) / sizeof(greenIntensities[0]));
inline constexpr int blueIntensityCount = static_cast<int>(sizeof(blueIntensities) / sizeof(blueIntensities[0]));

// libX11 src/xcms/LRGB.c, Default_RGB_SCCData. Row-major; index [row][column].
inline constexpr double xyzToRGBMatrix[3][3] = {
    { 3.48340481253539000, -1.52176374927285200, -0.55923133354049780 },
    { -1.07152751306193600, 1.96593795204372400, 0.03673691339553462 },
    { 0.06351179790497788, -0.20020501000496480, 0.81070942031648220 },
};
inline constexpr double rgbToXYZMatrix[3][3] = {
    { 0.38106149108714790, 0.32025712365352110, 0.24834578525933100 },
    { 0.20729745115140850, 0.68054638776373240, 0.11215616108485920 },
    { 0.02133944350088028, 0.14297193020246480, 1.24172892629665500 },
};

// The screen white point is what RGB intensity (1,1,1) produces, i.e. the RGB->XYZ row sums
// (libX11 _XcmsLRGB_InitScrnDefault). Every device-independent space is referred to it.
inline constexpr double whitePointX = rgbToXYZMatrix[0][0] + rgbToXYZMatrix[0][1] + rgbToXYZMatrix[0][2];
inline constexpr double whitePointY = rgbToXYZMatrix[1][0] + rgbToXYZMatrix[1][1] + rgbToXYZMatrix[1][2];
inline constexpr double whitePointZ = rgbToXYZMatrix[2][0] + rgbToXYZMatrix[2][1] + rgbToXYZMatrix[2][2];
inline constexpr double whitePointDenominator = whitePointX + 15.0 * whitePointY + 3.0 * whitePointZ;
inline constexpr double whitePointU = 4.0 * whitePointX / whitePointDenominator;
inline constexpr double whitePointV = 9.0 * whitePointY / whitePointDenominator;

// TekHVC constants (libX11 src/xcms/HVC.c): the "best red" chromaticity that anchors hue 0, and the
// factor that scales CIELuv metric chroma into TekHVC chroma.
inline constexpr double bestRedU = 0.7127;
inline constexpr double bestRedV = 0.4931;
inline constexpr double chromaScaleFactor = 7.50725;

// libX11 src/xcms/LRGB.c EPS: how far outside 0..1 an RGB-intensity triple may stray before Xcms
// calls it out of gamut. Also HVCMxC.c's convergence window for the max-chroma search.
inline constexpr double gamutEpsilon = 0.001;

// DBL_EPSILON, the nudge Xcms uses when it pulls a TekHVC component back inside its range.
inline constexpr double tinyEpsilon = 2.220446049250313e-16;

// M_PI is not guaranteed under a strict-ANSI C++ dialect; Xcms carries its own constant too.
inline constexpr double piValue = std::numbers::pi;

// The visual these answers assume. Xcms rounds every device value to this many significant bits and
// the X server hands back the replicated byte, so a client can only ever observe 0xNNNN.
inline constexpr int bitsPerChannel = 8;

struct ColorXYZ {
    double x;
    double y;
    double z;
};
struct ColorUVY {
    double uPrime;
    double vPrime;
    double luminance;
};
struct ColorHVC {
    double hue;
    double value;
    double chroma;
};
struct ColorRGBIntensity {
    double red;
    double green;
    double blue;
};

// Which end of a table's leading zero-intensity run a zero request resolves to. Gamut compression
// lands the color exactly on a face of the RGB cube, where one gun's intensity is mathematically
// zero and only round-off decides its sign; Xlib's own arithmetic falls just inside the gamut
// there, so the answer is the highest device value that still emits no light (blue 0e0e, not
// 0000). A color that was never compressed keeps Xcms' documented "zero intensity maps to zero
// value".
enum class ZeroIntensity {
    Lowest,
    Highest,
};

inline double minOf(const ColorRGBIntensity& color) { return std::fmin(color.red, std::fmin(color.green, color.blue)); }
inline double maxOf(const ColorRGBIntensity& color) { return std::fmax(color.red, std::fmax(color.green, color.blue)); }

inline double degreesOf(double radians) { return radians * 180.0 / piValue; }
inline double radiansOf(double degrees) { return degrees * piValue / 180.0; }

inline ColorRGBIntensity xyzToRGBIntensity(const ColorXYZ& color)
{
    return { xyzToRGBMatrix[0][0] * color.x + xyzToRGBMatrix[0][1] * color.y + xyzToRGBMatrix[0][2] * color.z,
        xyzToRGBMatrix[1][0] * color.x + xyzToRGBMatrix[1][1] * color.y + xyzToRGBMatrix[1][2] * color.z,
        xyzToRGBMatrix[2][0] * color.x + xyzToRGBMatrix[2][1] * color.y + xyzToRGBMatrix[2][2] * color.z };
}

inline ColorXYZ rgbIntensityToXYZ(const ColorRGBIntensity& color)
{
    return { rgbToXYZMatrix[0][0] * color.red + rgbToXYZMatrix[0][1] * color.green + rgbToXYZMatrix[0][2] * color.blue,
        rgbToXYZMatrix[1][0] * color.red + rgbToXYZMatrix[1][1] * color.green + rgbToXYZMatrix[1][2] * color.blue,
        rgbToXYZMatrix[2][0] * color.red + rgbToXYZMatrix[2][1] * color.green + rgbToXYZMatrix[2][2] * color.blue };
}

inline ColorUVY xyzToUVY(const ColorXYZ& color)
{
    const double denominator = color.x + 15.0 * color.y + 3.0 * color.z;
    if (denominator == 0.0)
        return { whitePointU, whitePointV, color.y };
    return { 4.0 * color.x / denominator, 9.0 * color.y / denominator, color.y };
}

inline ColorXYZ uvyToXYZ(const ColorUVY& color)
{
    double denominator = (6.0 * color.uPrime) - (16.0 * color.vPrime) + 12.0;
    double x = 0.0;
    double y = 0.0;
    if (denominator == 0.0) {
        // Degenerate chromaticity: Xcms substitutes the white point (libX11 uvY.c).
        denominator = (6.0 * whitePointU) - (16.0 * whitePointV) + 12.0;
        x = 9.0 * whitePointU / denominator;
        y = 4.0 * whitePointV / denominator;
    } else {
        x = 9.0 * color.uPrime / denominator;
        y = 4.0 * color.vPrime / denominator;
    }
    const double z = 1.0 - x - y;
    if (y == 0.0)
        return { x, color.luminance, z };
    return { x * color.luminance / y, color.luminance, z * color.luminance / y };
}

// CIExyY reaches CIEXYZ by way of u'v' in Xcms (libX11 xyY.c), not the textbook X = xY/y.
inline ColorXYZ xyyToXYZ(double chromaticityX, double chromaticityY, double luminance)
{
    double denominator = (-2.0 * chromaticityX) + (12.0 * chromaticityY) + 3.0;
    if (denominator == 0.0)
        return { 0.0, 0.0, 0.0 };
    const double uPrime = (4.0 * chromaticityX) / denominator;
    const double vPrime = (9.0 * chromaticityY) / denominator;
    denominator = (6.0 * uPrime) - (16.0 * vPrime) + 12.0;
    double x = 0.0;
    double y = 0.0;
    if (denominator == 0.0) {
        denominator = (6.0 * whitePointU) - (16.0 * whitePointV) + 12.0;
        if (denominator == 0.0)
            denominator = gamutEpsilon;
        x = 9.0 * whitePointU / denominator;
        // libX11 reads u' for both axes in this degenerate branch; kept for wire-compatible answers.
        y = 4.0 * whitePointU / denominator;
    } else {
        x = 9.0 * uPrime / denominator;
        y = 4.0 * vPrime / denominator;
    }
    const double z = 1.0 - x - y;
    if (y == 0.0)
        y = gamutEpsilon;
    return { x * luminance / y, luminance, z * luminance / y };
}

// Xcms scales a* and b* by 1/100 relative to the textbook CIELab, so its a*/b* run about
// -1.28..1.27 and the divisors below are 5.0 / 2.0 rather than 500 / 200 (libX11 Lab.c).
inline ColorXYZ labToXYZ(double lightness, double aStar, double bStar)
{
    double scaled = (lightness + 16.0) / 116.0;
    const double luminance = scaled * scaled * scaled;
    if (luminance < 0.008856) {
        scaled = lightness / 9.03292;
        return { whitePointX * ((aStar / 3893.5) + scaled), scaled, whitePointZ * (scaled - (bStar / 1557.4)) };
    }
    const double xTerm = scaled + (aStar / 5.0);
    const double zTerm = scaled - (bStar / 2.0);
    return { whitePointX * xTerm * xTerm * xTerm, luminance, whitePointZ * zTerm * zTerm * zTerm };
}

// Same 1/100 scaling for u* and v* (libX11 Luv.c).
inline ColorUVY luvToUVY(double lightness, double uStar, double vStar)
{
    double luminance = 0.0;
    if (lightness < 7.99953624)
        luminance = lightness / 903.29;
    else {
        const double scaled = (lightness + 16.0) / 116.0;
        luminance = scaled * scaled * scaled;
    }
    if (lightness == 0.0)
        return { whitePointU, whitePointV, luminance };
    const double scale = 13.0 * (lightness / 100.0);
    return { uStar / scale + whitePointU, vStar / scale + whitePointV, luminance };
}

// Hue 0 of TekHVC points from the white point toward "best red" (libX11 HVC.c).
inline double tekHVCThetaOffset() { return degreesOf(std::atan((bestRedV - whitePointV) / (bestRedU - whitePointU))); }

inline ColorHVC uvyToHVC(const ColorUVY& color)
{
    const double u = color.uPrime - whitePointU;
    const double v = color.vPrime - whitePointV;
    double theta = (u == 0.0) ? 0.0 : degreesOf(std::atan(v / u));

    // atan collapses the four quadrants onto one; Xcms folds theta back into the quadrant the signs
    // of u,v select instead of using atan2.
    double quadrantLow = 0.0;
    double quadrantHigh = 360.0;
    if (u > 0.0 && v > 0.0) {
        quadrantLow = 0.0;
        quadrantHigh = 90.0;
    } else if (u < 0.0 && v > 0.0) {
        quadrantLow = 90.0;
        quadrantHigh = 180.0;
    } else if (u < 0.0 && v < 0.0) {
        quadrantLow = 180.0;
        quadrantHigh = 270.0;
    } else if (u > 0.0 && v < 0.0) {
        quadrantLow = 270.0;
        quadrantHigh = 360.0;
    }
    while (theta < quadrantLow)
        theta += 90.0;
    while (theta >= quadrantHigh)
        theta -= 90.0;

    const double lightness = (color.luminance < 0.008856) ? (color.luminance * 903.29) : (std::cbrt(color.luminance) * 116.0 - 16.0);
    const double chroma = lightness * chromaScaleFactor * std::sqrt((u * u) + (v * v));
    if (chroma < 0.0)
        theta = 0.0;
    double hue = theta - tekHVCThetaOffset();
    while (hue < -gamutEpsilon)
        hue += 360.0;
    while (hue >= 360.0 + gamutEpsilon)
        hue -= 360.0;
    return { hue, lightness, chroma };
}

inline ColorUVY hvcToUVY(const ColorHVC& color)
{
    if (color.value == 0.0 || color.value == 100.0)
        return { whitePointU, whitePointV, color.value == 100.0 ? 1.0 : 0.0 };

    double angle = color.hue + tekHVCThetaOffset();
    while (angle < 0.0)
        angle += 360.0;
    while (angle >= 360.0)
        angle -= 360.0;
    angle = radiansOf(angle);
    const double u = (std::cos(angle) * color.chroma) / (color.value * chromaScaleFactor);
    const double v = (std::sin(angle) * color.chroma) / (color.value * chromaScaleFactor);
    double luminance = 0.0;
    if (color.value < 7.99953624)
        luminance = color.value / 903.29;
    else {
        const double scaled = (color.value + 16.0) / 116.0;
        luminance = scaled * scaled * scaled;
    }
    return { u + whitePointU, v + whitePointV, luminance };
}

inline ColorHVC xyzToHVC(const ColorXYZ& color) { return uvyToHVC(xyzToUVY(color)); }
inline ColorXYZ hvcToXYZ(const ColorHVC& color) { return uvyToXYZ(hvcToUVY(color)); }

// libX11 _XcmsTekHVC_CheckModify: pull V and C back into range and wrap H into 0..360.
inline ColorHVC checkModifyHVC(ColorHVC color)
{
    if (color.value < 0.0)
        color.value = 0.0 + tinyEpsilon;
    else if (color.value > 100.0)
        color.value = 100.0 - tinyEpsilon;
    if (color.chroma < 0.0)
        color.chroma = 0.0 - tinyEpsilon;
    if (color.hue < 0.0) {
        const double turns = static_cast<double>(static_cast<int>(-color.hue / 360.0));
        color.hue += (turns + 1.0) * 360.0;
        if (color.hue >= 360.0)
            color.hue -= 360.0;
    } else if (color.hue >= 360.0)
        color.hue -= static_cast<double>(static_cast<int>(color.hue / 360.0)) * 360.0;
    return color;
}

// Invert a gun's transfer table: the device value that emits `intensity`. Mirrors libX11's
// _XcmsTableSearch + _XcmsIntensityInterpolation, including its rounding to the visual's bit depth
// — the interpolated target snaps to whichever of the two neighbouring 8-bit codes is closer, which
// is why every answer is a replicated byte.
inline uint16_t intensityToDeviceValue(const IntensityRecord* table, int count, double intensity, ZeroIntensity zeroChoice)
{
    // Xcms masks the answer to the visual's significant bits and the X server hands back the
    // replicated byte; the two steps compose to this.
    const auto deviceValue = [](long raw) { return static_cast<uint16_t>(((raw >> 8) & 0xFF) * 0x0101); };

    if (intensity <= table[0].intensity) {
        int index = 0;
        if (zeroChoice == ZeroIntensity::Highest) {
            while (index + 1 < count && table[index + 1].intensity <= table[0].intensity)
                index++;
        }
        return deviceValue(table[index].value);
    }
    if (intensity >= table[count - 1].intensity)
        return deviceValue(table[count - 1].value);

    int low = 0;
    int high = count - 1;
    int middle = 0;
    int previousMiddle = count - 1;
    while (middle != previousMiddle) {
        previousMiddle = middle;
        middle = low + (high - low) / 2;
        if (intensity == table[middle].intensity)
            return deviceValue(table[middle].value);
        if (intensity < table[middle].intensity)
            high = middle;
        else
            low = middle;
    }
    if (high <= low)
        return deviceValue(table[low].value);

    const double ratio = (intensity - table[low].intensity) / (table[high].intensity - table[low].intensity);
    // Xcms truncates the interpolated product to an integer before adding the base.
    long target = static_cast<long>(static_cast<double>(table[high].value - table[low].value) * ratio) + table[low].value;

    constexpr long maxCode = (1L << bitsPerChannel) - 1;
    constexpr int shift = 16 - bitsPerChannel;
    long up = ((target >> shift) * 0xFFFF) / maxCode;
    long down = 0;
    if (up < target) {
        down = up;
        long code = (down >> shift) + 1;
        if (code > maxCode)
            code = maxCode;
        up = (code * 0xFFFF) / maxCode;
    } else {
        long code = (up >> shift) - 1;
        if (code < 0)
            code = 0;
        down = (code * 0xFFFF) / maxCode;
    }
    return deviceValue((up - target) < (target - down) ? up : down);
}

[[nodiscard]] inline PaletteRGB rgbIntensityToDeviceRGB(const ColorRGBIntensity& color, ZeroIntensity zeroChoice)
{
    return { intensityToDeviceValue(redIntensities, redIntensityCount, color.red, zeroChoice),
        intensityToDeviceValue(greenIntensities, greenIntensityCount, color.green, zeroChoice),
        intensityToDeviceValue(blueIntensities, blueIntensityCount, color.blue, zeroChoice) };
}

// libX11 _XcmsTekHVCQueryMaxVCRGB: the most chromatic color of this hue the display can show, found
// by driving an unreachable HVC through the matrix and normalizing the resulting RGB intensity so
// the smallest gun sits at 0 and the largest at 1 — that is, onto the gamut surface.
// The point on the gamut surface, in both the spaces the search needs it in. One
// value rather than two out-parameters: the RGB is the HVC, and a caller holding
// one without the other has half an answer.
struct GamutSurfacePoint {
    ColorHVC hvc;
    ColorRGBIntensity rgb;
};

[[nodiscard]] inline std::optional<GamutSurfacePoint> queryMaxValueChroma(double hue)
{
    constexpr double startValue = 40.0;
    constexpr double startChroma = 120.0;

    ColorRGBIntensity rgb = xyzToRGBIntensity(hvcToXYZ({ hue, startValue, startChroma }));
    const double smallest = minOf(rgb);
    rgb.red -= smallest;
    rgb.green -= smallest;
    rgb.blue -= smallest;
    const double largest = maxOf(rgb);
    if (!(largest > 0.0))
        return std::nullopt;
    rgb.red /= largest;
    rgb.green /= largest;
    rgb.blue /= largest;

    // libX11 restores the requested hue over the one the HVC round trip lands
    // on, and this reproduces that. No caller reads it: queryMaxChroma takes
    // .value and .chroma from this point and its hue from elsewhere, and a sweep
    // of 1859 specs across the hue circle and the Lab cube answers identically
    // with the line removed. Kept because the contract is Xlib's arithmetic, not
    // the subset of it this file happens to consume.
    ColorHVC hvc = xyzToHVC(rgbIntensityToXYZ(rgb));
    hvc.hue = hue;
    return GamutSurfacePoint { hvc, rgb };
}

// libX11 XcmsTekHVCQueryMaxC: the largest chroma the display can show at this hue and value.
[[nodiscard]] inline std::optional<ColorHVC> queryMaxChroma(double hue, double value)
{
    constexpr int maxSearchSteps = 100;

    ColorHVC current = checkModifyHVC({ hue, value, 100.0 });
    const std::optional<GamutSurfacePoint> surface = queryMaxValueChroma(hue);
    if (!surface)
        return std::nullopt;
    const ColorHVC maxValueChroma = surface->hvc;
    const ColorRGBIntensity surfaceRGB = surface->rgb;
    if (!(maxValueChroma.value > 0.0))
        return std::nullopt;

    // Below the most-chromatic point the gamut boundary is the straight line back to black.
    if (value <= maxValueChroma.value) {
        current.chroma = value * maxValueChroma.chroma / maxValueChroma.value;
        return checkModifyHVC(current);
    }

    // Above it, walk the surface toward white until the value matches the one asked for.
    const double maxDistance = 100.0 - maxValueChroma.value;
    if (!(maxDistance > 0.0))
        return std::nullopt;
    const double savedValue = value;
    double nextValue = value;
    double lastValue = -1.0;
    double lastChroma = -1.0;
    double previousValue = -1.0;
    double relaxation = 1.0;

    for (int step = 0; step < maxSearchSteps; step++) {
        previousValue = lastValue;
        lastValue = current.value;
        lastChroma = current.chroma;
        const double blend = (nextValue - maxValueChroma.value) / maxDistance * relaxation;
        current = xyzToHVC(rgbIntensityToXYZ({ surfaceRGB.red * (1.0 - blend) + blend,
            surfaceRGB.green * (1.0 - blend) + blend,
            surfaceRGB.blue * (1.0 - blend) + blend }));

        if (current.value <= savedValue + gamutEpsilon && current.value >= savedValue - gamutEpsilon) {
            current.hue = hue;
            return checkModifyHVC(current);
        }
        nextValue += savedValue - current.value;
        if (nextValue < maxValueChroma.value) {
            nextValue = maxValueChroma.value;
            relaxation *= 0.5; // the walk overshot; halve the step
        } else if (nextValue > 100.0) {
            current.hue = hue;
            if (std::fabs(lastValue - savedValue) < std::fabs(current.value - savedValue)) {
                current.value = lastValue;
                current.chroma = lastChroma;
            }
            return checkModifyHVC(current);
        } else if (current.value <= previousValue + gamutEpsilon && current.value >= previousValue - gamutEpsilon)
            relaxation *= 0.5; // stalled between two values; halve the step
    }
    if (std::fabs(lastValue - savedValue) < std::fabs(current.value - savedValue)) {
        current.value = lastValue;
        current.chroma = lastChroma;
    }
    current.hue = hue;
    return current;
}

// Resolve a device-independent color to the display's RGB, compressing chroma when it falls outside
// the gamut (libX11's default XcmsTekHVCClipC). Nothing when the color cannot be represented at all,
// which is how Xlib reports a spec that must then be ignored.
[[nodiscard]] inline std::optional<PaletteRGB> xyzToDeviceRGB(const ColorXYZ& color)
{
    ColorRGBIntensity rgb = xyzToRGBIntensity(color);
    ZeroIntensity zeroChoice = ZeroIntensity::Lowest;

    if (minOf(rgb) < -gamutEpsilon || maxOf(rgb) > 1.0 + gamutEpsilon) {
        const ColorHVC hvc = xyzToHVC(color);
        const std::optional<ColorHVC> clipped = queryMaxChroma(hvc.hue, hvc.value);
        if (!clipped)
            return std::nullopt;
        rgb = xyzToRGBIntensity(hvcToXYZ(*clipped));
        if (minOf(rgb) < -gamutEpsilon || maxOf(rgb) > 1.0 + gamutEpsilon)
            return std::nullopt;
        zeroChoice = ZeroIntensity::Highest;
    }

    const auto clampToUnit = [](double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); };
    rgb = { clampToUnit(rgb.red), clampToUnit(rgb.green), clampToUnit(rgb.blue) };
    return rgbIntensityToDeviceRGB(rgb, zeroChoice);
}

// --- specification text ------------------------------------------------------------------------

// Each form decides it has ended by looking one past its last consumed character. A view has no NUL
// waiting there, so an exhausted one answers with the byte a NUL-terminated string would have held,
// and every test below stays spelled the way Xlib spells it.
[[nodiscard]] inline char peek(std::string_view s) { return s.empty() ? '\0' : s.front(); }

[[nodiscard]] inline bool isDecimalDigit(char c) { return c >= '0' && c <= '9'; }

// A spec ends at the payload terminator; OSC payloads pack several fields with ';'. The view's own
// end is the other terminator, which is the one place this differs from the `const char*` it
// replaced: a NUL inside a view is payload, and payload after a colour is a rejection.
[[nodiscard]] inline bool atSpecificationEnd(std::string_view s)
{
    const size_t firstSignificant = s.find_first_not_of(" \t");
    return firstSignificant == std::string_view::npos || s[firstSignificant] == ';';
}

inline int hexValueOf(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

inline char lowerCaseOf(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

// Case-insensitive match of a lower-case `name` followed by ':'. Xlib lower-cases the spec before
// dispatching, so "CIELab:" and "cielab:" name the same space. Consumes the colon on a match, and
// leaves the cursor where it found it otherwise, so the caller can try the next space.
[[nodiscard]] inline bool matchPrefix(std::string_view& cursor, std::string_view name)
{
    if (!std::ranges::equal(cursor.substr(0, name.size()), name, {}, lowerCaseOf))
        return false;
    std::string_view rest = cursor.substr(name.size());
    if (peek(rest) != ':')
        return false;
    rest.remove_prefix(1);
    cursor = rest;
    return true;
}

// Decimal float field, locale-independent: strtod would accept or reject a comma decimal point
// depending on LC_NUMERIC, and a color spec is never localized.
[[nodiscard]] inline std::optional<double> parseDouble(std::string_view& cursor)
{
    std::string_view s = cursor;
    bool isNegative = false;
    if (peek(s) == '+' || peek(s) == '-') {
        isNegative = (peek(s) == '-');
        s.remove_prefix(1);
    }
    constexpr uint64_t maxMantissa = 1000000000000000000ULL; // one more digit still fits a uint64
    uint64_t mantissa = 0;
    int digits = 0;
    int decimalExponent = 0;
    while (isDecimalDigit(peek(s))) {
        if (mantissa < maxMantissa) {
            mantissa = mantissa * 10 + static_cast<uint64_t>(s.front() - '0');
            digits++;
        } else
            decimalExponent++; // past double's precision; only the scale still matters
        s.remove_prefix(1);
    }
    if (peek(s) == '.') {
        s.remove_prefix(1);
        while (isDecimalDigit(peek(s))) {
            if (mantissa < maxMantissa) {
                mantissa = mantissa * 10 + static_cast<uint64_t>(s.front() - '0');
                digits++;
                decimalExponent--;
            }
            s.remove_prefix(1);
        }
    }
    if (!digits)
        return std::nullopt;
    if (peek(s) == 'e' || peek(s) == 'E') {
        std::string_view mark = s.substr(1);
        bool isExponentNegative = false;
        if (peek(mark) == '+' || peek(mark) == '-') {
            isExponentNegative = (peek(mark) == '-');
            mark.remove_prefix(1);
        }
        // An 'e' with no digits after it is not an exponent, and the mantissa keeps the 'e'
        // for whoever reads next — which is a rejection, since no form admits one there.
        if (isDecimalDigit(peek(mark))) {
            int exponent = 0;
            while (isDecimalDigit(peek(mark))) {
                if (exponent < 10000)
                    exponent = exponent * 10 + (mark.front() - '0');
                mark.remove_prefix(1);
            }
            decimalExponent += isExponentNegative ? -exponent : exponent;
            s = mark;
        }
    }

    double value = static_cast<double>(mantissa);
    if (decimalExponent > 0)
        value *= std::pow(10.0, static_cast<double>(decimalExponent));
    else if (decimalExponent < 0)
        value /= std::pow(10.0, static_cast<double>(-decimalExponent));
    cursor = s;
    return isNegative ? -value : value;
}

// Three '/'-separated fields, before any colour space claims them. Returned
// together rather than written through three pointers, so a caller cannot read
// a field the parse never reached — the seven forms below shared one set of
// locals, and a branch that failed mid-triple left the previous branch's values
// in place.
struct ParsedTriple {
    double first;
    double second;
    double third;
};

// Three '/'-separated float fields, the shape every device-independent form uses.
[[nodiscard]] inline std::optional<ParsedTriple> parseTriple(std::string_view cursor)
{
    const std::optional<double> first = parseDouble(cursor);
    if (!first || peek(cursor) != '/')
        return std::nullopt;
    cursor.remove_prefix(1);
    const std::optional<double> second = parseDouble(cursor);
    if (!second || peek(cursor) != '/')
        return std::nullopt;
    cursor.remove_prefix(1);
    const std::optional<double> third = parseDouble(cursor);
    if (!third || !atSpecificationEnd(cursor))
        return std::nullopt;
    return ParsedTriple { *first, *second, *third };
}

// "#" is left-justified into the channel and zero-filled below — libX11 ParseCol.c shifts rather
// than scales, so #fff is 0xf000, not 0xffff. The 8-bit visual then replicates the top byte, which
// is why #fff, #f0f0f0, #f00f00f00 and #f000f000f000 all read back as f0f0.
[[nodiscard]] inline std::optional<PaletteRGB> parseHashForm(std::string_view spec)
{
    size_t length = 0;
    while (length < spec.size() && hexValueOf(spec[length]) >= 0)
        length++;
    if (length != 3 && length != 6 && length != 9 && length != 12)
        return std::nullopt;
    if (!atSpecificationEnd(spec.substr(length)))
        return std::nullopt;

    const size_t perChannel = length / 3;
    const size_t shift = 16 - 4 * perChannel;
    PaletteRGB parsed {};
    uint16_t* const channels[3] = { &parsed.red, &parsed.green, &parsed.blue };
    for (size_t channel = 0; channel < 3; channel++) {
        unsigned value = 0;
        for (const char digit : spec.substr(channel * perChannel, perChannel))
            value = (value << 4) | static_cast<unsigned>(hexValueOf(digit));
        value <<= shift;
        *channels[channel] = static_cast<uint16_t>(((value >> 8) & 0xFF) * 0x0101);
    }
    return parsed;
}

// "rgb:" scales instead: v * 0xFFFF / (16^digits - 1), so rgb:f/0/0 is full-intensity red. The
// 8-bit visual then keeps the top byte of the scaled value, as it does for "#": xterm stores the
// colour as its visual's pixel, each channel shifted down to the visual's width (misc.c:2733,
// AllocOneColor), and answers a query by replicating that byte (misc.c:2774, QueryOneColor), so
// rgb:f/80/abc reads back as ffff/8080/abab and rgb:1234/5678/9abc as 1212/5656/9a9a.
[[nodiscard]] inline std::optional<PaletteRGB> parseRGBForm(std::string_view cursor)
{
    PaletteRGB parsed {};
    uint16_t* const channels[3] = { &parsed.red, &parsed.green, &parsed.blue };
    for (int channel = 0; channel < 3; channel++) {
        unsigned value = 0;
        int digits = 0;
        while (digits < 4) {
            const int digit = hexValueOf(peek(cursor));
            if (digit < 0)
                break;
            value = (value << 4) | static_cast<unsigned>(digit);
            cursor.remove_prefix(1);
            digits++;
        }
        if (!digits)
            return std::nullopt;
        if (digits < 4)
            value = (value * 0xFFFFu) / ((1u << (digits * 4)) - 1u);
        *channels[channel] = static_cast<uint16_t>(((value >> 8) & 0xFF) * 0x0101);
        if (channel < 2) {
            if (peek(cursor) != '/')
                return std::nullopt;
            cursor.remove_prefix(1);
        }
    }
    if (!atSpecificationEnd(cursor))
        return std::nullopt;
    return parsed;
}
// The colour names XParseColor resolves through the X server's colour
// database: X.Org's rgb.txt, every entry, keyed by its name lower-cased with
// the spaces taken out ("light sky blue" and "LightSkyBlue" are one entry
// there twice). Sorted for the binary search below.
struct NamedColor {
    std::string_view name;
    std::array<std::uint8_t, 3> rgb;
};
constexpr NamedColor namedColors[] = {
    { "aliceblue", { 240, 248, 255 } },
    { "antiquewhite", { 250, 235, 215 } },
    { "antiquewhite1", { 255, 239, 219 } },
    { "antiquewhite2", { 238, 223, 204 } },
    { "antiquewhite3", { 205, 192, 176 } },
    { "antiquewhite4", { 139, 131, 120 } },
    { "aqua", { 0, 255, 255 } },
    { "aquamarine", { 127, 255, 212 } },
    { "aquamarine1", { 127, 255, 212 } },
    { "aquamarine2", { 118, 238, 198 } },
    { "aquamarine3", { 102, 205, 170 } },
    { "aquamarine4", { 69, 139, 116 } },
    { "azure", { 240, 255, 255 } },
    { "azure1", { 240, 255, 255 } },
    { "azure2", { 224, 238, 238 } },
    { "azure3", { 193, 205, 205 } },
    { "azure4", { 131, 139, 139 } },
    { "beige", { 245, 245, 220 } },
    { "bisque", { 255, 228, 196 } },
    { "bisque1", { 255, 228, 196 } },
    { "bisque2", { 238, 213, 183 } },
    { "bisque3", { 205, 183, 158 } },
    { "bisque4", { 139, 125, 107 } },
    { "black", { 0, 0, 0 } },
    { "blanchedalmond", { 255, 235, 205 } },
    { "blue", { 0, 0, 255 } },
    { "blue1", { 0, 0, 255 } },
    { "blue2", { 0, 0, 238 } },
    { "blue3", { 0, 0, 205 } },
    { "blue4", { 0, 0, 139 } },
    { "blueviolet", { 138, 43, 226 } },
    { "brown", { 165, 42, 42 } },
    { "brown1", { 255, 64, 64 } },
    { "brown2", { 238, 59, 59 } },
    { "brown3", { 205, 51, 51 } },
    { "brown4", { 139, 35, 35 } },
    { "burlywood", { 222, 184, 135 } },
    { "burlywood1", { 255, 211, 155 } },
    { "burlywood2", { 238, 197, 145 } },
    { "burlywood3", { 205, 170, 125 } },
    { "burlywood4", { 139, 115, 85 } },
    { "cadetblue", { 95, 158, 160 } },
    { "cadetblue1", { 152, 245, 255 } },
    { "cadetblue2", { 142, 229, 238 } },
    { "cadetblue3", { 122, 197, 205 } },
    { "cadetblue4", { 83, 134, 139 } },
    { "chartreuse", { 127, 255, 0 } },
    { "chartreuse1", { 127, 255, 0 } },
    { "chartreuse2", { 118, 238, 0 } },
    { "chartreuse3", { 102, 205, 0 } },
    { "chartreuse4", { 69, 139, 0 } },
    { "chocolate", { 210, 105, 30 } },
    { "chocolate1", { 255, 127, 36 } },
    { "chocolate2", { 238, 118, 33 } },
    { "chocolate3", { 205, 102, 29 } },
    { "chocolate4", { 139, 69, 19 } },
    { "coral", { 255, 127, 80 } },
    { "coral1", { 255, 114, 86 } },
    { "coral2", { 238, 106, 80 } },
    { "coral3", { 205, 91, 69 } },
    { "coral4", { 139, 62, 47 } },
    { "cornflowerblue", { 100, 149, 237 } },
    { "cornsilk", { 255, 248, 220 } },
    { "cornsilk1", { 255, 248, 220 } },
    { "cornsilk2", { 238, 232, 205 } },
    { "cornsilk3", { 205, 200, 177 } },
    { "cornsilk4", { 139, 136, 120 } },
    { "crimson", { 220, 20, 60 } },
    { "cyan", { 0, 255, 255 } },
    { "cyan1", { 0, 255, 255 } },
    { "cyan2", { 0, 238, 238 } },
    { "cyan3", { 0, 205, 205 } },
    { "cyan4", { 0, 139, 139 } },
    { "darkblue", { 0, 0, 139 } },
    { "darkcyan", { 0, 139, 139 } },
    { "darkgoldenrod", { 184, 134, 11 } },
    { "darkgoldenrod1", { 255, 185, 15 } },
    { "darkgoldenrod2", { 238, 173, 14 } },
    { "darkgoldenrod3", { 205, 149, 12 } },
    { "darkgoldenrod4", { 139, 101, 8 } },
    { "darkgray", { 169, 169, 169 } },
    { "darkgreen", { 0, 100, 0 } },
    { "darkgrey", { 169, 169, 169 } },
    { "darkkhaki", { 189, 183, 107 } },
    { "darkmagenta", { 139, 0, 139 } },
    { "darkolivegreen", { 85, 107, 47 } },
    { "darkolivegreen1", { 202, 255, 112 } },
    { "darkolivegreen2", { 188, 238, 104 } },
    { "darkolivegreen3", { 162, 205, 90 } },
    { "darkolivegreen4", { 110, 139, 61 } },
    { "darkorange", { 255, 140, 0 } },
    { "darkorange1", { 255, 127, 0 } },
    { "darkorange2", { 238, 118, 0 } },
    { "darkorange3", { 205, 102, 0 } },
    { "darkorange4", { 139, 69, 0 } },
    { "darkorchid", { 153, 50, 204 } },
    { "darkorchid1", { 191, 62, 255 } },
    { "darkorchid2", { 178, 58, 238 } },
    { "darkorchid3", { 154, 50, 205 } },
    { "darkorchid4", { 104, 34, 139 } },
    { "darkred", { 139, 0, 0 } },
    { "darksalmon", { 233, 150, 122 } },
    { "darkseagreen", { 143, 188, 143 } },
    { "darkseagreen1", { 193, 255, 193 } },
    { "darkseagreen2", { 180, 238, 180 } },
    { "darkseagreen3", { 155, 205, 155 } },
    { "darkseagreen4", { 105, 139, 105 } },
    { "darkslateblue", { 72, 61, 139 } },
    { "darkslategray", { 47, 79, 79 } },
    { "darkslategray1", { 151, 255, 255 } },
    { "darkslategray2", { 141, 238, 238 } },
    { "darkslategray3", { 121, 205, 205 } },
    { "darkslategray4", { 82, 139, 139 } },
    { "darkslategrey", { 47, 79, 79 } },
    { "darkturquoise", { 0, 206, 209 } },
    { "darkviolet", { 148, 0, 211 } },
    { "debianred", { 215, 7, 81 } },
    { "deeppink", { 255, 20, 147 } },
    { "deeppink1", { 255, 20, 147 } },
    { "deeppink2", { 238, 18, 137 } },
    { "deeppink3", { 205, 16, 118 } },
    { "deeppink4", { 139, 10, 80 } },
    { "deepskyblue", { 0, 191, 255 } },
    { "deepskyblue1", { 0, 191, 255 } },
    { "deepskyblue2", { 0, 178, 238 } },
    { "deepskyblue3", { 0, 154, 205 } },
    { "deepskyblue4", { 0, 104, 139 } },
    { "dimgray", { 105, 105, 105 } },
    { "dimgrey", { 105, 105, 105 } },
    { "dodgerblue", { 30, 144, 255 } },
    { "dodgerblue1", { 30, 144, 255 } },
    { "dodgerblue2", { 28, 134, 238 } },
    { "dodgerblue3", { 24, 116, 205 } },
    { "dodgerblue4", { 16, 78, 139 } },
    { "firebrick", { 178, 34, 34 } },
    { "firebrick1", { 255, 48, 48 } },
    { "firebrick2", { 238, 44, 44 } },
    { "firebrick3", { 205, 38, 38 } },
    { "firebrick4", { 139, 26, 26 } },
    { "floralwhite", { 255, 250, 240 } },
    { "forestgreen", { 34, 139, 34 } },
    { "fuchsia", { 255, 0, 255 } },
    { "gainsboro", { 220, 220, 220 } },
    { "ghostwhite", { 248, 248, 255 } },
    { "gold", { 255, 215, 0 } },
    { "gold1", { 255, 215, 0 } },
    { "gold2", { 238, 201, 0 } },
    { "gold3", { 205, 173, 0 } },
    { "gold4", { 139, 117, 0 } },
    { "goldenrod", { 218, 165, 32 } },
    { "goldenrod1", { 255, 193, 37 } },
    { "goldenrod2", { 238, 180, 34 } },
    { "goldenrod3", { 205, 155, 29 } },
    { "goldenrod4", { 139, 105, 20 } },
    { "gray", { 190, 190, 190 } },
    { "gray0", { 0, 0, 0 } },
    { "gray1", { 3, 3, 3 } },
    { "gray10", { 26, 26, 26 } },
    { "gray100", { 255, 255, 255 } },
    { "gray11", { 28, 28, 28 } },
    { "gray12", { 31, 31, 31 } },
    { "gray13", { 33, 33, 33 } },
    { "gray14", { 36, 36, 36 } },
    { "gray15", { 38, 38, 38 } },
    { "gray16", { 41, 41, 41 } },
    { "gray17", { 43, 43, 43 } },
    { "gray18", { 46, 46, 46 } },
    { "gray19", { 48, 48, 48 } },
    { "gray2", { 5, 5, 5 } },
    { "gray20", { 51, 51, 51 } },
    { "gray21", { 54, 54, 54 } },
    { "gray22", { 56, 56, 56 } },
    { "gray23", { 59, 59, 59 } },
    { "gray24", { 61, 61, 61 } },
    { "gray25", { 64, 64, 64 } },
    { "gray26", { 66, 66, 66 } },
    { "gray27", { 69, 69, 69 } },
    { "gray28", { 71, 71, 71 } },
    { "gray29", { 74, 74, 74 } },
    { "gray3", { 8, 8, 8 } },
    { "gray30", { 77, 77, 77 } },
    { "gray31", { 79, 79, 79 } },
    { "gray32", { 82, 82, 82 } },
    { "gray33", { 84, 84, 84 } },
    { "gray34", { 87, 87, 87 } },
    { "gray35", { 89, 89, 89 } },
    { "gray36", { 92, 92, 92 } },
    { "gray37", { 94, 94, 94 } },
    { "gray38", { 97, 97, 97 } },
    { "gray39", { 99, 99, 99 } },
    { "gray4", { 10, 10, 10 } },
    { "gray40", { 102, 102, 102 } },
    { "gray41", { 105, 105, 105 } },
    { "gray42", { 107, 107, 107 } },
    { "gray43", { 110, 110, 110 } },
    { "gray44", { 112, 112, 112 } },
    { "gray45", { 115, 115, 115 } },
    { "gray46", { 117, 117, 117 } },
    { "gray47", { 120, 120, 120 } },
    { "gray48", { 122, 122, 122 } },
    { "gray49", { 125, 125, 125 } },
    { "gray5", { 13, 13, 13 } },
    { "gray50", { 127, 127, 127 } },
    { "gray51", { 130, 130, 130 } },
    { "gray52", { 133, 133, 133 } },
    { "gray53", { 135, 135, 135 } },
    { "gray54", { 138, 138, 138 } },
    { "gray55", { 140, 140, 140 } },
    { "gray56", { 143, 143, 143 } },
    { "gray57", { 145, 145, 145 } },
    { "gray58", { 148, 148, 148 } },
    { "gray59", { 150, 150, 150 } },
    { "gray6", { 15, 15, 15 } },
    { "gray60", { 153, 153, 153 } },
    { "gray61", { 156, 156, 156 } },
    { "gray62", { 158, 158, 158 } },
    { "gray63", { 161, 161, 161 } },
    { "gray64", { 163, 163, 163 } },
    { "gray65", { 166, 166, 166 } },
    { "gray66", { 168, 168, 168 } },
    { "gray67", { 171, 171, 171 } },
    { "gray68", { 173, 173, 173 } },
    { "gray69", { 176, 176, 176 } },
    { "gray7", { 18, 18, 18 } },
    { "gray70", { 179, 179, 179 } },
    { "gray71", { 181, 181, 181 } },
    { "gray72", { 184, 184, 184 } },
    { "gray73", { 186, 186, 186 } },
    { "gray74", { 189, 189, 189 } },
    { "gray75", { 191, 191, 191 } },
    { "gray76", { 194, 194, 194 } },
    { "gray77", { 196, 196, 196 } },
    { "gray78", { 199, 199, 199 } },
    { "gray79", { 201, 201, 201 } },
    { "gray8", { 20, 20, 20 } },
    { "gray80", { 204, 204, 204 } },
    { "gray81", { 207, 207, 207 } },
    { "gray82", { 209, 209, 209 } },
    { "gray83", { 212, 212, 212 } },
    { "gray84", { 214, 214, 214 } },
    { "gray85", { 217, 217, 217 } },
    { "gray86", { 219, 219, 219 } },
    { "gray87", { 222, 222, 222 } },
    { "gray88", { 224, 224, 224 } },
    { "gray89", { 227, 227, 227 } },
    { "gray9", { 23, 23, 23 } },
    { "gray90", { 229, 229, 229 } },
    { "gray91", { 232, 232, 232 } },
    { "gray92", { 235, 235, 235 } },
    { "gray93", { 237, 237, 237 } },
    { "gray94", { 240, 240, 240 } },
    { "gray95", { 242, 242, 242 } },
    { "gray96", { 245, 245, 245 } },
    { "gray97", { 247, 247, 247 } },
    { "gray98", { 250, 250, 250 } },
    { "gray99", { 252, 252, 252 } },
    { "green", { 0, 255, 0 } },
    { "green1", { 0, 255, 0 } },
    { "green2", { 0, 238, 0 } },
    { "green3", { 0, 205, 0 } },
    { "green4", { 0, 139, 0 } },
    { "greenyellow", { 173, 255, 47 } },
    { "grey", { 190, 190, 190 } },
    { "grey0", { 0, 0, 0 } },
    { "grey1", { 3, 3, 3 } },
    { "grey10", { 26, 26, 26 } },
    { "grey100", { 255, 255, 255 } },
    { "grey11", { 28, 28, 28 } },
    { "grey12", { 31, 31, 31 } },
    { "grey13", { 33, 33, 33 } },
    { "grey14", { 36, 36, 36 } },
    { "grey15", { 38, 38, 38 } },
    { "grey16", { 41, 41, 41 } },
    { "grey17", { 43, 43, 43 } },
    { "grey18", { 46, 46, 46 } },
    { "grey19", { 48, 48, 48 } },
    { "grey2", { 5, 5, 5 } },
    { "grey20", { 51, 51, 51 } },
    { "grey21", { 54, 54, 54 } },
    { "grey22", { 56, 56, 56 } },
    { "grey23", { 59, 59, 59 } },
    { "grey24", { 61, 61, 61 } },
    { "grey25", { 64, 64, 64 } },
    { "grey26", { 66, 66, 66 } },
    { "grey27", { 69, 69, 69 } },
    { "grey28", { 71, 71, 71 } },
    { "grey29", { 74, 74, 74 } },
    { "grey3", { 8, 8, 8 } },
    { "grey30", { 77, 77, 77 } },
    { "grey31", { 79, 79, 79 } },
    { "grey32", { 82, 82, 82 } },
    { "grey33", { 84, 84, 84 } },
    { "grey34", { 87, 87, 87 } },
    { "grey35", { 89, 89, 89 } },
    { "grey36", { 92, 92, 92 } },
    { "grey37", { 94, 94, 94 } },
    { "grey38", { 97, 97, 97 } },
    { "grey39", { 99, 99, 99 } },
    { "grey4", { 10, 10, 10 } },
    { "grey40", { 102, 102, 102 } },
    { "grey41", { 105, 105, 105 } },
    { "grey42", { 107, 107, 107 } },
    { "grey43", { 110, 110, 110 } },
    { "grey44", { 112, 112, 112 } },
    { "grey45", { 115, 115, 115 } },
    { "grey46", { 117, 117, 117 } },
    { "grey47", { 120, 120, 120 } },
    { "grey48", { 122, 122, 122 } },
    { "grey49", { 125, 125, 125 } },
    { "grey5", { 13, 13, 13 } },
    { "grey50", { 127, 127, 127 } },
    { "grey51", { 130, 130, 130 } },
    { "grey52", { 133, 133, 133 } },
    { "grey53", { 135, 135, 135 } },
    { "grey54", { 138, 138, 138 } },
    { "grey55", { 140, 140, 140 } },
    { "grey56", { 143, 143, 143 } },
    { "grey57", { 145, 145, 145 } },
    { "grey58", { 148, 148, 148 } },
    { "grey59", { 150, 150, 150 } },
    { "grey6", { 15, 15, 15 } },
    { "grey60", { 153, 153, 153 } },
    { "grey61", { 156, 156, 156 } },
    { "grey62", { 158, 158, 158 } },
    { "grey63", { 161, 161, 161 } },
    { "grey64", { 163, 163, 163 } },
    { "grey65", { 166, 166, 166 } },
    { "grey66", { 168, 168, 168 } },
    { "grey67", { 171, 171, 171 } },
    { "grey68", { 173, 173, 173 } },
    { "grey69", { 176, 176, 176 } },
    { "grey7", { 18, 18, 18 } },
    { "grey70", { 179, 179, 179 } },
    { "grey71", { 181, 181, 181 } },
    { "grey72", { 184, 184, 184 } },
    { "grey73", { 186, 186, 186 } },
    { "grey74", { 189, 189, 189 } },
    { "grey75", { 191, 191, 191 } },
    { "grey76", { 194, 194, 194 } },
    { "grey77", { 196, 196, 196 } },
    { "grey78", { 199, 199, 199 } },
    { "grey79", { 201, 201, 201 } },
    { "grey8", { 20, 20, 20 } },
    { "grey80", { 204, 204, 204 } },
    { "grey81", { 207, 207, 207 } },
    { "grey82", { 209, 209, 209 } },
    { "grey83", { 212, 212, 212 } },
    { "grey84", { 214, 214, 214 } },
    { "grey85", { 217, 217, 217 } },
    { "grey86", { 219, 219, 219 } },
    { "grey87", { 222, 222, 222 } },
    { "grey88", { 224, 224, 224 } },
    { "grey89", { 227, 227, 227 } },
    { "grey9", { 23, 23, 23 } },
    { "grey90", { 229, 229, 229 } },
    { "grey91", { 232, 232, 232 } },
    { "grey92", { 235, 235, 235 } },
    { "grey93", { 237, 237, 237 } },
    { "grey94", { 240, 240, 240 } },
    { "grey95", { 242, 242, 242 } },
    { "grey96", { 245, 245, 245 } },
    { "grey97", { 247, 247, 247 } },
    { "grey98", { 250, 250, 250 } },
    { "grey99", { 252, 252, 252 } },
    { "honeydew", { 240, 255, 240 } },
    { "honeydew1", { 240, 255, 240 } },
    { "honeydew2", { 224, 238, 224 } },
    { "honeydew3", { 193, 205, 193 } },
    { "honeydew4", { 131, 139, 131 } },
    { "hotpink", { 255, 105, 180 } },
    { "hotpink1", { 255, 110, 180 } },
    { "hotpink2", { 238, 106, 167 } },
    { "hotpink3", { 205, 96, 144 } },
    { "hotpink4", { 139, 58, 98 } },
    { "indianred", { 205, 92, 92 } },
    { "indianred1", { 255, 106, 106 } },
    { "indianred2", { 238, 99, 99 } },
    { "indianred3", { 205, 85, 85 } },
    { "indianred4", { 139, 58, 58 } },
    { "indigo", { 75, 0, 130 } },
    { "ivory", { 255, 255, 240 } },
    { "ivory1", { 255, 255, 240 } },
    { "ivory2", { 238, 238, 224 } },
    { "ivory3", { 205, 205, 193 } },
    { "ivory4", { 139, 139, 131 } },
    { "khaki", { 240, 230, 140 } },
    { "khaki1", { 255, 246, 143 } },
    { "khaki2", { 238, 230, 133 } },
    { "khaki3", { 205, 198, 115 } },
    { "khaki4", { 139, 134, 78 } },
    { "lavender", { 230, 230, 250 } },
    { "lavenderblush", { 255, 240, 245 } },
    { "lavenderblush1", { 255, 240, 245 } },
    { "lavenderblush2", { 238, 224, 229 } },
    { "lavenderblush3", { 205, 193, 197 } },
    { "lavenderblush4", { 139, 131, 134 } },
    { "lawngreen", { 124, 252, 0 } },
    { "lemonchiffon", { 255, 250, 205 } },
    { "lemonchiffon1", { 255, 250, 205 } },
    { "lemonchiffon2", { 238, 233, 191 } },
    { "lemonchiffon3", { 205, 201, 165 } },
    { "lemonchiffon4", { 139, 137, 112 } },
    { "lightblue", { 173, 216, 230 } },
    { "lightblue1", { 191, 239, 255 } },
    { "lightblue2", { 178, 223, 238 } },
    { "lightblue3", { 154, 192, 205 } },
    { "lightblue4", { 104, 131, 139 } },
    { "lightcoral", { 240, 128, 128 } },
    { "lightcyan", { 224, 255, 255 } },
    { "lightcyan1", { 224, 255, 255 } },
    { "lightcyan2", { 209, 238, 238 } },
    { "lightcyan3", { 180, 205, 205 } },
    { "lightcyan4", { 122, 139, 139 } },
    { "lightgoldenrod", { 238, 221, 130 } },
    { "lightgoldenrod1", { 255, 236, 139 } },
    { "lightgoldenrod2", { 238, 220, 130 } },
    { "lightgoldenrod3", { 205, 190, 112 } },
    { "lightgoldenrod4", { 139, 129, 76 } },
    { "lightgoldenrodyellow", { 250, 250, 210 } },
    { "lightgray", { 211, 211, 211 } },
    { "lightgreen", { 144, 238, 144 } },
    { "lightgrey", { 211, 211, 211 } },
    { "lightpink", { 255, 182, 193 } },
    { "lightpink1", { 255, 174, 185 } },
    { "lightpink2", { 238, 162, 173 } },
    { "lightpink3", { 205, 140, 149 } },
    { "lightpink4", { 139, 95, 101 } },
    { "lightsalmon", { 255, 160, 122 } },
    { "lightsalmon1", { 255, 160, 122 } },
    { "lightsalmon2", { 238, 149, 114 } },
    { "lightsalmon3", { 205, 129, 98 } },
    { "lightsalmon4", { 139, 87, 66 } },
    { "lightseagreen", { 32, 178, 170 } },
    { "lightskyblue", { 135, 206, 250 } },
    { "lightskyblue1", { 176, 226, 255 } },
    { "lightskyblue2", { 164, 211, 238 } },
    { "lightskyblue3", { 141, 182, 205 } },
    { "lightskyblue4", { 96, 123, 139 } },
    { "lightslateblue", { 132, 112, 255 } },
    { "lightslategray", { 119, 136, 153 } },
    { "lightslategrey", { 119, 136, 153 } },
    { "lightsteelblue", { 176, 196, 222 } },
    { "lightsteelblue1", { 202, 225, 255 } },
    { "lightsteelblue2", { 188, 210, 238 } },
    { "lightsteelblue3", { 162, 181, 205 } },
    { "lightsteelblue4", { 110, 123, 139 } },
    { "lightyellow", { 255, 255, 224 } },
    { "lightyellow1", { 255, 255, 224 } },
    { "lightyellow2", { 238, 238, 209 } },
    { "lightyellow3", { 205, 205, 180 } },
    { "lightyellow4", { 139, 139, 122 } },
    { "lime", { 0, 255, 0 } },
    { "limegreen", { 50, 205, 50 } },
    { "linen", { 250, 240, 230 } },
    { "magenta", { 255, 0, 255 } },
    { "magenta1", { 255, 0, 255 } },
    { "magenta2", { 238, 0, 238 } },
    { "magenta3", { 205, 0, 205 } },
    { "magenta4", { 139, 0, 139 } },
    { "maroon", { 176, 48, 96 } },
    { "maroon1", { 255, 52, 179 } },
    { "maroon2", { 238, 48, 167 } },
    { "maroon3", { 205, 41, 144 } },
    { "maroon4", { 139, 28, 98 } },
    { "mediumaquamarine", { 102, 205, 170 } },
    { "mediumblue", { 0, 0, 205 } },
    { "mediumorchid", { 186, 85, 211 } },
    { "mediumorchid1", { 224, 102, 255 } },
    { "mediumorchid2", { 209, 95, 238 } },
    { "mediumorchid3", { 180, 82, 205 } },
    { "mediumorchid4", { 122, 55, 139 } },
    { "mediumpurple", { 147, 112, 219 } },
    { "mediumpurple1", { 171, 130, 255 } },
    { "mediumpurple2", { 159, 121, 238 } },
    { "mediumpurple3", { 137, 104, 205 } },
    { "mediumpurple4", { 93, 71, 139 } },
    { "mediumseagreen", { 60, 179, 113 } },
    { "mediumslateblue", { 123, 104, 238 } },
    { "mediumspringgreen", { 0, 250, 154 } },
    { "mediumturquoise", { 72, 209, 204 } },
    { "mediumvioletred", { 199, 21, 133 } },
    { "midnightblue", { 25, 25, 112 } },
    { "mintcream", { 245, 255, 250 } },
    { "mistyrose", { 255, 228, 225 } },
    { "mistyrose1", { 255, 228, 225 } },
    { "mistyrose2", { 238, 213, 210 } },
    { "mistyrose3", { 205, 183, 181 } },
    { "mistyrose4", { 139, 125, 123 } },
    { "moccasin", { 255, 228, 181 } },
    { "navajowhite", { 255, 222, 173 } },
    { "navajowhite1", { 255, 222, 173 } },
    { "navajowhite2", { 238, 207, 161 } },
    { "navajowhite3", { 205, 179, 139 } },
    { "navajowhite4", { 139, 121, 94 } },
    { "navy", { 0, 0, 128 } },
    { "navyblue", { 0, 0, 128 } },
    { "oldlace", { 253, 245, 230 } },
    { "olive", { 128, 128, 0 } },
    { "olivedrab", { 107, 142, 35 } },
    { "olivedrab1", { 192, 255, 62 } },
    { "olivedrab2", { 179, 238, 58 } },
    { "olivedrab3", { 154, 205, 50 } },
    { "olivedrab4", { 105, 139, 34 } },
    { "orange", { 255, 165, 0 } },
    { "orange1", { 255, 165, 0 } },
    { "orange2", { 238, 154, 0 } },
    { "orange3", { 205, 133, 0 } },
    { "orange4", { 139, 90, 0 } },
    { "orangered", { 255, 69, 0 } },
    { "orangered1", { 255, 69, 0 } },
    { "orangered2", { 238, 64, 0 } },
    { "orangered3", { 205, 55, 0 } },
    { "orangered4", { 139, 37, 0 } },
    { "orchid", { 218, 112, 214 } },
    { "orchid1", { 255, 131, 250 } },
    { "orchid2", { 238, 122, 233 } },
    { "orchid3", { 205, 105, 201 } },
    { "orchid4", { 139, 71, 137 } },
    { "palegoldenrod", { 238, 232, 170 } },
    { "palegreen", { 152, 251, 152 } },
    { "palegreen1", { 154, 255, 154 } },
    { "palegreen2", { 144, 238, 144 } },
    { "palegreen3", { 124, 205, 124 } },
    { "palegreen4", { 84, 139, 84 } },
    { "paleturquoise", { 175, 238, 238 } },
    { "paleturquoise1", { 187, 255, 255 } },
    { "paleturquoise2", { 174, 238, 238 } },
    { "paleturquoise3", { 150, 205, 205 } },
    { "paleturquoise4", { 102, 139, 139 } },
    { "palevioletred", { 219, 112, 147 } },
    { "palevioletred1", { 255, 130, 171 } },
    { "palevioletred2", { 238, 121, 159 } },
    { "palevioletred3", { 205, 104, 137 } },
    { "palevioletred4", { 139, 71, 93 } },
    { "papayawhip", { 255, 239, 213 } },
    { "peachpuff", { 255, 218, 185 } },
    { "peachpuff1", { 255, 218, 185 } },
    { "peachpuff2", { 238, 203, 173 } },
    { "peachpuff3", { 205, 175, 149 } },
    { "peachpuff4", { 139, 119, 101 } },
    { "peru", { 205, 133, 63 } },
    { "pink", { 255, 192, 203 } },
    { "pink1", { 255, 181, 197 } },
    { "pink2", { 238, 169, 184 } },
    { "pink3", { 205, 145, 158 } },
    { "pink4", { 139, 99, 108 } },
    { "plum", { 221, 160, 221 } },
    { "plum1", { 255, 187, 255 } },
    { "plum2", { 238, 174, 238 } },
    { "plum3", { 205, 150, 205 } },
    { "plum4", { 139, 102, 139 } },
    { "powderblue", { 176, 224, 230 } },
    { "purple", { 160, 32, 240 } },
    { "purple1", { 155, 48, 255 } },
    { "purple2", { 145, 44, 238 } },
    { "purple3", { 125, 38, 205 } },
    { "purple4", { 85, 26, 139 } },
    { "rebeccapurple", { 102, 51, 153 } },
    { "red", { 255, 0, 0 } },
    { "red1", { 255, 0, 0 } },
    { "red2", { 238, 0, 0 } },
    { "red3", { 205, 0, 0 } },
    { "red4", { 139, 0, 0 } },
    { "rosybrown", { 188, 143, 143 } },
    { "rosybrown1", { 255, 193, 193 } },
    { "rosybrown2", { 238, 180, 180 } },
    { "rosybrown3", { 205, 155, 155 } },
    { "rosybrown4", { 139, 105, 105 } },
    { "royalblue", { 65, 105, 225 } },
    { "royalblue1", { 72, 118, 255 } },
    { "royalblue2", { 67, 110, 238 } },
    { "royalblue3", { 58, 95, 205 } },
    { "royalblue4", { 39, 64, 139 } },
    { "saddlebrown", { 139, 69, 19 } },
    { "salmon", { 250, 128, 114 } },
    { "salmon1", { 255, 140, 105 } },
    { "salmon2", { 238, 130, 98 } },
    { "salmon3", { 205, 112, 84 } },
    { "salmon4", { 139, 76, 57 } },
    { "sandybrown", { 244, 164, 96 } },
    { "seagreen", { 46, 139, 87 } },
    { "seagreen1", { 84, 255, 159 } },
    { "seagreen2", { 78, 238, 148 } },
    { "seagreen3", { 67, 205, 128 } },
    { "seagreen4", { 46, 139, 87 } },
    { "seashell", { 255, 245, 238 } },
    { "seashell1", { 255, 245, 238 } },
    { "seashell2", { 238, 229, 222 } },
    { "seashell3", { 205, 197, 191 } },
    { "seashell4", { 139, 134, 130 } },
    { "sienna", { 160, 82, 45 } },
    { "sienna1", { 255, 130, 71 } },
    { "sienna2", { 238, 121, 66 } },
    { "sienna3", { 205, 104, 57 } },
    { "sienna4", { 139, 71, 38 } },
    { "silver", { 192, 192, 192 } },
    { "skyblue", { 135, 206, 235 } },
    { "skyblue1", { 135, 206, 255 } },
    { "skyblue2", { 126, 192, 238 } },
    { "skyblue3", { 108, 166, 205 } },
    { "skyblue4", { 74, 112, 139 } },
    { "slateblue", { 106, 90, 205 } },
    { "slateblue1", { 131, 111, 255 } },
    { "slateblue2", { 122, 103, 238 } },
    { "slateblue3", { 105, 89, 205 } },
    { "slateblue4", { 71, 60, 139 } },
    { "slategray", { 112, 128, 144 } },
    { "slategray1", { 198, 226, 255 } },
    { "slategray2", { 185, 211, 238 } },
    { "slategray3", { 159, 182, 205 } },
    { "slategray4", { 108, 123, 139 } },
    { "slategrey", { 112, 128, 144 } },
    { "snow", { 255, 250, 250 } },
    { "snow1", { 255, 250, 250 } },
    { "snow2", { 238, 233, 233 } },
    { "snow3", { 205, 201, 201 } },
    { "snow4", { 139, 137, 137 } },
    { "springgreen", { 0, 255, 127 } },
    { "springgreen1", { 0, 255, 127 } },
    { "springgreen2", { 0, 238, 118 } },
    { "springgreen3", { 0, 205, 102 } },
    { "springgreen4", { 0, 139, 69 } },
    { "steelblue", { 70, 130, 180 } },
    { "steelblue1", { 99, 184, 255 } },
    { "steelblue2", { 92, 172, 238 } },
    { "steelblue3", { 79, 148, 205 } },
    { "steelblue4", { 54, 100, 139 } },
    { "tan", { 210, 180, 140 } },
    { "tan1", { 255, 165, 79 } },
    { "tan2", { 238, 154, 73 } },
    { "tan3", { 205, 133, 63 } },
    { "tan4", { 139, 90, 43 } },
    { "teal", { 0, 128, 128 } },
    { "thistle", { 216, 191, 216 } },
    { "thistle1", { 255, 225, 255 } },
    { "thistle2", { 238, 210, 238 } },
    { "thistle3", { 205, 181, 205 } },
    { "thistle4", { 139, 123, 139 } },
    { "tomato", { 255, 99, 71 } },
    { "tomato1", { 255, 99, 71 } },
    { "tomato2", { 238, 92, 66 } },
    { "tomato3", { 205, 79, 57 } },
    { "tomato4", { 139, 54, 38 } },
    { "turquoise", { 64, 224, 208 } },
    { "turquoise1", { 0, 245, 255 } },
    { "turquoise2", { 0, 229, 238 } },
    { "turquoise3", { 0, 197, 205 } },
    { "turquoise4", { 0, 134, 139 } },
    { "violet", { 238, 130, 238 } },
    { "violetred", { 208, 32, 144 } },
    { "violetred1", { 255, 62, 150 } },
    { "violetred2", { 238, 58, 140 } },
    { "violetred3", { 205, 50, 120 } },
    { "violetred4", { 139, 34, 82 } },
    { "webgray", { 128, 128, 128 } },
    { "webgreen", { 0, 128, 0 } },
    { "webgrey", { 128, 128, 128 } },
    { "webmaroon", { 128, 0, 0 } },
    { "webpurple", { 128, 0, 128 } },
    { "wheat", { 245, 222, 179 } },
    { "wheat1", { 255, 231, 186 } },
    { "wheat2", { 238, 216, 174 } },
    { "wheat3", { 205, 186, 150 } },
    { "wheat4", { 139, 126, 102 } },
    { "white", { 255, 255, 255 } },
    { "whitesmoke", { 245, 245, 245 } },
    { "x11gray", { 190, 190, 190 } },
    { "x11green", { 0, 255, 0 } },
    { "x11grey", { 190, 190, 190 } },
    { "x11maroon", { 176, 48, 96 } },
    { "x11purple", { 160, 32, 240 } },
    { "yellow", { 255, 255, 0 } },
    { "yellow1", { 255, 255, 0 } },
    { "yellow2", { 238, 238, 0 } },
    { "yellow3", { 205, 205, 0 } },
    { "yellow4", { 139, 139, 0 } },
    { "yellowgreen", { 154, 205, 50 } },
};
template<std::size_t N> constexpr bool namedColorsSorted(const NamedColor (&table)[N])
{
    for (std::size_t i = 1; i < N; ++i) {
        if (!(table[i - 1].name < table[i].name))
            return false;
    }
    return true;
}
static_assert(namedColorsSorted(namedColors), "namedColors must be sorted by name for the binary search");

// A colour name as the database keys it: case and spaces do not distinguish
// names. A name longer than any in the table cannot match, so it is refused
// before it is copied.
[[nodiscard]] std::optional<PaletteRGB> parseColorName(std::string_view spec)
{
    constexpr std::size_t longestName = 32;
    std::array<char, longestName> key {};
    std::size_t length = 0;
    for (const char character : spec) {
        if (character == ' ')
            continue;
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x21 || byte > 0x7E || length == key.size())
            return std::nullopt;
        key[length++] = static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
    }
    const std::string_view wanted(key.data(), length);
    const auto* found = std::ranges::lower_bound(namedColors, wanted, {}, &NamedColor::name);
    if (found == std::end(namedColors) || found->name != wanted)
        return std::nullopt;
    // Replicated bytes, as Xlib scales an 8-bit database entry.
    const auto widen = [](std::uint8_t value) { return static_cast<std::uint16_t>(value * 0x101); };
    return PaletteRGB { widen(found->rgb[0]), widen(found->rgb[1]), widen(found->rgb[2]) };
}

} // namespace

std::optional<PaletteRGB> parseColorSpecification(std::string_view spec)
{
    if (peek(spec) == '#')
        return parseHashForm(spec.substr(1));

    std::string_view cursor = spec;
    if (matchPrefix(cursor, "rgb"))
        return parseRGBForm(cursor);

    cursor = spec;
    if (matchPrefix(cursor, "rgbi")) {
        const std::optional<ParsedTriple> triple = parseTriple(cursor);
        if (!triple)
            return std::nullopt;
        // RGB intensity is already device-dependent: no gamut compression, straight to the tables.
        return rgbIntensityToDeviceRGB({ triple->first, triple->second, triple->third }, ZeroIntensity::Lowest);
    }

    // Each space carries Xcms' range check; a spec outside it is rejected, never clamped.
    cursor = spec;
    if (matchPrefix(cursor, "ciexyz")) {
        const std::optional<ParsedTriple> triple = parseTriple(cursor);
        if (!triple || triple->second < 0.0 || triple->second > 1.0)
            return std::nullopt;
        return xyzToDeviceRGB({ triple->first, triple->second, triple->third });
    }
    cursor = spec;
    if (matchPrefix(cursor, "cieuvy")) {
        const std::optional<ParsedTriple> triple = parseTriple(cursor);
        if (!triple || triple->third < 0.0 || triple->third > 1.0)
            return std::nullopt;
        return xyzToDeviceRGB(uvyToXYZ({ triple->first, triple->second, triple->third }));
    }
    cursor = spec;
    if (matchPrefix(cursor, "ciexyy")) {
        const std::optional<ParsedTriple> triple = parseTriple(cursor);
        if (!triple)
            return std::nullopt;
        if (triple->first < 0.0 || triple->first > 1.0 || triple->second < 0.0 || triple->second > 1.0 || triple->third < 0.0 || triple->third > 1.0)
            return std::nullopt;
        return xyzToDeviceRGB(xyyToXYZ(triple->first, triple->second, triple->third));
    }
    cursor = spec;
    if (matchPrefix(cursor, "cielab")) {
        const std::optional<ParsedTriple> triple = parseTriple(cursor);
        if (!triple || triple->first < 0.0 || triple->first > 100.0)
            return std::nullopt;
        return xyzToDeviceRGB(labToXYZ(triple->first, triple->second, triple->third));
    }
    cursor = spec;
    if (matchPrefix(cursor, "cieluv")) {
        const std::optional<ParsedTriple> triple = parseTriple(cursor);
        if (!triple || triple->first < 0.0 || triple->first > 100.0)
            return std::nullopt;
        return xyzToDeviceRGB(uvyToXYZ(luvToUVY(triple->first, triple->second, triple->third)));
    }
    cursor = spec;
    if (matchPrefix(cursor, "tekhvc")) {
        const std::optional<ParsedTriple> triple = parseTriple(cursor);
        if (!triple)
            return std::nullopt;
        if (triple->second < 0.0 || triple->second > 100.0 || triple->third < 0.0)
            return std::nullopt;
        return xyzToDeviceRGB(hvcToXYZ({ triple->first, triple->second, triple->third }));
    }

    // No colour space claimed it, so it is a name, as XParseColor then asks
    // the colour database: `red`, `light sky blue`.
    return parseColorName(spec);
}

std::optional<PaletteRGB> parseKittyColorSpecification(std::string_view specification)
{
    auto cursor = specification;
    if (!matchPrefix(cursor, "rgbi"))
        return parseColorSpecification(specification);
    const auto triple = parseTriple(cursor);
    if (!triple || std::isnan(triple->first) || std::isnan(triple->second) || std::isnan(triple->third))
        return std::nullopt;
    const auto channel = [](double value) {
        return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0) * 257);
    };
    return PaletteRGB { channel(triple->first), channel(triple->second), channel(triple->third) };
}

} // namespace Ubiquitty::OSC::XParseColor
