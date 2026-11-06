// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Host tests for Source/Core/Rendering/BackgroundImage.cpp — the window's
// background image as a software frame composites it.
//
// Pins:
//   - an image file's bytes decode through Core's own decoders;
//   - a pixel the frame covers whole is left as it is;
//   - a translucent pixel takes the image in the share its alpha leaves open,
//     in linear light, with alpha growing by the same share;
//   - a pixel outside the placed rectangle is untouched;
//   - Tile repeats at native size, so a pixel lands on its texel exactly;
//   - the software renderer composites the layer the frame options carry.

#include "Core/Rendering/BackgroundImage.h"
#include "Core/Rendering/PNGEncoder.h"
#include "Core/Rendering/TerminalSnapshot.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using Ubiquitty::Rendering::BackgroundImageLayer;
using Ubiquitty::Rendering::BackgroundImageMode;
using Ubiquitty::Rendering::BackgroundImagePixels;

BackgroundImagePixels solidImage(int width, int height, std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    BackgroundImagePixels image;
    image.width = width;
    image.height = height;
    for (int index = 0; index < width * height; ++index)
        image.rgba.insert(image.rgba.end(), { red, green, blue, 255 });
    return image;
}

std::vector<std::uint8_t> solidFrame(int width, int height, std::uint8_t value, std::uint8_t alpha)
{
    std::vector<std::uint8_t> frame;
    for (int index = 0; index < width * height; ++index)
        frame.insert(frame.end(), { value, value, value, alpha });
    return frame;
}

// The expected channel: the frame's colour plus the image in the open share,
// in linear light, encoded by rounding.
int expectedChannel(int frame, int image, double opacity, double frameAlpha)
{
    const auto decode = [](double code) {
        const double encoded = code / 255.0;
        return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
    };
    const double linear = decode(frame) + decode(image) * opacity * (1.0 - frameAlpha);
    const double encoded = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(std::fmin(encoded, 1.0) * 255.0));
}

} // namespace

TEST(anImageFileDecodesThroughCoresOwnDecoders)
{
    const std::vector<std::uint8_t> rgba { 10, 20, 30, 255, 40, 50, 60, 128 };
    const std::vector<std::uint8_t> png = Ubiquitty::Rendering::encodePNG(rgba, { 2, 1 });
    const BackgroundImagePixels image = Ubiquitty::Rendering::decodeBackgroundImage(png);
    REQUIRE(!image.isEmpty());
    CHECK(image.width == 2 && image.height == 1);
    CHECK(image.rgba == rgba);
    const std::vector<std::uint8_t> garbage { 1, 2, 3 };
    CHECK(Ubiquitty::Rendering::decodeBackgroundImage(garbage).isEmpty());
}

TEST(aCoveredPixelKeepsWhatTheFrameDrew)
{
    const BackgroundImagePixels image = solidImage(4, 4, 200, 100, 50);
    std::vector<std::uint8_t> frame = solidFrame(4, 4, 30, 255);
    const std::vector<std::uint8_t> before = frame;
    Ubiquitty::Rendering::compositeBackgroundImage(BackgroundImageLayer { &image, BackgroundImageMode::Stretch, 1.0f }, frame, { 4, 4 });
    CHECK(frame == before);
}

TEST(aTranslucentPixelTakesTheImageInTheShareItLeavesOpen)
{
    const BackgroundImagePixels image = solidImage(8, 8, 200, 100, 50);
    std::vector<std::uint8_t> frame = solidFrame(4, 4, 30, 217);
    Ubiquitty::Rendering::compositeBackgroundImage(BackgroundImageLayer { &image, BackgroundImageMode::Tile, 0.6f }, frame, { 4, 4 });
    const double frameAlpha = 217.0 / 255.0;
    CHECK(frame[0] == expectedChannel(30, 200, 0.6, frameAlpha));
    CHECK(frame[1] == expectedChannel(30, 100, 0.6, frameAlpha));
    CHECK(frame[2] == expectedChannel(30, 50, 0.6, frameAlpha));
    CHECK(frame[3] == static_cast<int>(std::lround((frameAlpha + 0.6 * (1.0 - frameAlpha)) * 255.0)));
}

TEST(aPixelOutsideThePlacedRectangleIsUntouched)
{
    // A wide image fitted into a square frame leaves bands above and below.
    const BackgroundImagePixels image = solidImage(8, 2, 255, 255, 255);
    std::vector<std::uint8_t> frame = solidFrame(8, 8, 0, 0);
    Ubiquitty::Rendering::compositeBackgroundImage(BackgroundImageLayer { &image, BackgroundImageMode::Fit, 1.0f }, frame, { 8, 8 });
    CHECK(frame[3] == 0);
    CHECK(frame[(4 * 8 + 4) * 4 + 3] == 255);
    CHECK(frame[(7 * 8 + 4) * 4 + 3] == 0);
}

TEST(tileRepeatsAtNativeSizeOnTheTexel)
{
    BackgroundImagePixels image = solidImage(2, 1, 0, 0, 0);
    image.rgba[4] = 255; // the second texel is red
    std::vector<std::uint8_t> frame = solidFrame(5, 1, 0, 0);
    Ubiquitty::Rendering::compositeBackgroundImage(BackgroundImageLayer { &image, BackgroundImageMode::Tile, 1.0f }, frame, { 5, 1 });
    for (int x = 0; x < 5; ++x)
        CHECK(frame[static_cast<std::size_t>(x) * 4] == (x % 2 == 1 ? 255 : 0));
}

TEST(theSoftwareRendererCompositesTheLayerTheOptionsCarry)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, 2, 4);
    const BackgroundImagePixels image = solidImage(3, 3, 250, 10, 10);
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    options.style.backgroundOpacity = 0.5f;
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> plain = Ubiquitty::Rendering::renderTerminalSnapshotRGBA(*terminal, options, width, height);
    options.backgroundImage = { &image, BackgroundImageMode::Stretch, 1.0f };
    const std::vector<std::uint8_t> withImage = Ubiquitty::Rendering::renderTerminalSnapshotRGBA(*terminal, options, width, height);
    REQUIRE(plain.size() == withImage.size() && !plain.empty());
    const std::size_t middle = (static_cast<std::size_t>(height / 2) * static_cast<std::size_t>(width) + static_cast<std::size_t>(width / 2)) * 4;
    CHECK(withImage[middle + 3] > plain[middle + 3]);
    CHECK(withImage[middle] > plain[middle]);
}

UBIQUITTY_TEST_MAIN()
