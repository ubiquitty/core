// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/ProportionalOutline.h"

#include "Core/Codec/ZlibInflate.h"
#include "Core/Rendering/ProportionalOutlineData.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace Ubiquitty::Rendering::ProportionalOutline {

namespace {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct GlyphRecord {
    std::uint32_t codePoint = 0;
    std::uint32_t firstEnd = 0; // index into the face's contour ends
    std::uint32_t firstPoint = 0; // index into the face's points and curve flags
    std::uint16_t contourCount = 0;
    std::uint16_t pointCount = 0;
};

struct FaceOutlines {
    int unitsPerEm = 0;
    std::vector<GlyphRecord> glyphs; // sorted by code point
    std::vector<std::uint16_t> ends;
    std::vector<std::int16_t> points; // x, y pairs
    std::vector<std::uint8_t> isOnCurve;
};

struct OutlineSet {
    FaceOutlines faces[2];
};

// The layout is Tools/gen-proportional-outlines'; a stream that ends
// mid-record keeps the faces read whole so far, and the rest answer absent.
OutlineSet load()
{
    OutlineSet set;
    std::vector<std::uint8_t> blob(proportionalOutlineBlobLength);
    if (!Codec::Zlib::inflateExact({ proportionalOutlineCompressed, static_cast<std::size_t>(proportionalOutlineCompressedLength) },
            { blob.data(), blob.size() }))
        return set;
    const auto u16 = [&](std::size_t at) { return static_cast<std::uint16_t>(blob[at] | (blob[at + 1] << 8)); };
    std::size_t at = 0;
    while (at + 7 <= blob.size()) {
        const int faceIndex = blob[at];
        if (faceIndex > 1)
            return set;
        FaceOutlines outlines;
        outlines.unitsPerEm = u16(at + 1);
        const std::uint32_t glyphCount = u16(at + 3) | (static_cast<std::uint32_t>(u16(at + 5)) << 16);
        at += 7;
        for (std::uint32_t index = 0; index < glyphCount; ++index) {
            if (at + 6 > blob.size())
                return set;
            GlyphRecord record;
            record.codePoint = u16(at);
            record.contourCount = u16(at + 2);
            record.pointCount = u16(at + 4);
            at += 6;
            const std::size_t recordBytes = 2 * static_cast<std::size_t>(record.contourCount) + 5 * static_cast<std::size_t>(record.pointCount);
            if (at + recordBytes > blob.size())
                return set;
            record.firstEnd = static_cast<std::uint32_t>(outlines.ends.size());
            record.firstPoint = static_cast<std::uint32_t>(outlines.isOnCurve.size());
            for (int contour = 0; contour < record.contourCount; ++contour, at += 2)
                outlines.ends.push_back(u16(at));
            for (int point = 0; point < record.pointCount; ++point, at += 4) {
                outlines.points.push_back(static_cast<std::int16_t>(u16(at)));
                outlines.points.push_back(static_cast<std::int16_t>(u16(at + 2)));
            }
            outlines.isOnCurve.insert(outlines.isOnCurve.end(), blob.begin() + static_cast<std::ptrdiff_t>(at), blob.begin() + static_cast<std::ptrdiff_t>(at + record.pointCount));
            at += record.pointCount;
            outlines.glyphs.push_back(record);
        }
        set.faces[faceIndex] = std::move(outlines);
    }
    return set;
}

// Inflated once, on first use, and read only after: an immutable table every
// thread may share.
const OutlineSet& outlineSet()
{
    static const OutlineSet set = load();
    return set;
}

const GlyphRecord* glyphFor(const FaceOutlines& outlines, std::uint32_t codePoint)
{
    const auto found = std::ranges::lower_bound(outlines.glyphs, codePoint, {}, &GlyphRecord::codePoint);
    return found != outlines.glyphs.end() && found->codePoint == codePoint ? &*found : nullptr;
}

// Signed-area coverage accumulation over a glyph's box: each line segment
// adds, to the pixels it crosses, the share of every row it spans that lies
// to their right, and a running sum along a row is then the area of each
// pixel the outline covers, nonzero winding. The accumulator is the
// glyph's box and two columns more, so a segment reaching its right edge
// writes inside it.
class Accumulator {
public:
    Accumulator(int width, int height)
        : m_width(width)
        , m_height(height)
        , m_area(static_cast<std::size_t>(width + 2) * static_cast<std::size_t>(height), 0.0f)
    {
    }

    void line(Point from, Point to)
    {
        if (from.y == to.y)
            return;
        const double direction = from.y < to.y ? 1.0 : -1.0;
        if (from.y > to.y)
            std::swap(from, to);
        const double slope = (to.x - from.x) / (to.y - from.y);
        double x = from.x;
        int firstRow = static_cast<int>(std::floor(from.y));
        if (from.y < 0.0) {
            x -= from.y * slope;
            firstRow = 0;
        }
        const int endRow = std::min(m_height, static_cast<int>(std::ceil(to.y)));
        const int stride = m_width + 2;
        for (int row = firstRow; row < endRow; ++row) {
            const double spanY = std::min(static_cast<double>(row + 1), to.y) - std::max(static_cast<double>(row), from.y);
            const double nextX = x + slope * spanY;
            const double share = spanY * direction;
            const double left = std::clamp(std::min(x, nextX), 0.0, static_cast<double>(m_width));
            const double right = std::clamp(std::max(x, nextX), 0.0, static_cast<double>(m_width));
            float* const rowArea = m_area.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride);
            const double leftFloor = std::floor(left);
            const int leftColumn = static_cast<int>(leftFloor);
            const int rightColumn = static_cast<int>(std::ceil(right));
            if (rightColumn <= leftColumn + 1) {
                // Inside one column: the share splits between it and the next
                // by where the segment's middle stands in it.
                const double middle = 0.5 * (left + right) - leftFloor;
                rowArea[leftColumn] += static_cast<float>(share - share * middle);
                rowArea[leftColumn + 1] += static_cast<float>(share * middle);
            } else {
                // Across columns: a triangle in the first and last, and an even
                // ramp across those between.
                const double inverse = 1.0 / (right - left);
                const double leftFraction = left - leftFloor;
                const double firstArea = 0.5 * inverse * (1.0 - leftFraction) * (1.0 - leftFraction);
                const double rightFraction = right - std::ceil(right) + 1.0;
                const double lastArea = 0.5 * inverse * rightFraction * rightFraction;
                rowArea[leftColumn] += static_cast<float>(share * firstArea);
                if (rightColumn == leftColumn + 2) {
                    rowArea[leftColumn + 1] += static_cast<float>(share * (1.0 - firstArea - lastArea));
                } else {
                    const double secondArea = inverse * (1.5 - leftFraction);
                    rowArea[leftColumn + 1] += static_cast<float>(share * (secondArea - firstArea));
                    for (int column = leftColumn + 2; column < rightColumn - 1; ++column)
                        rowArea[column] += static_cast<float>(share * inverse);
                    const double beforeLast = secondArea + static_cast<double>(rightColumn - leftColumn - 3) * inverse;
                    rowArea[rightColumn - 1] += static_cast<float>(share * (1.0 - beforeLast - lastArea));
                }
                rowArea[rightColumn] += static_cast<float>(share * lastArea);
            }
            x = nextX;
        }
    }

    // A quadratic, flattened into lines a fraction of a pixel from the curve.
    void quadratic(Point from, Point control, Point to)
    {
        const double deviationX = from.x - 2.0 * control.x + to.x;
        const double deviationY = from.y - 2.0 * control.y + to.y;
        const double deviation = deviationX * deviationX + deviationY * deviationY;
        if (deviation < 0.333) {
            line(from, to);
            return;
        }
        const int segments = 1 + static_cast<int>(std::floor(std::sqrt(std::sqrt(3.0 * deviation))));
        Point previous = from;
        for (int segment = 1; segment <= segments; ++segment) {
            const double t = static_cast<double>(segment) / segments;
            const double inverse = 1.0 - t;
            const Point next { inverse * inverse * from.x + 2.0 * inverse * t * control.x + t * t * to.x,
                inverse * inverse * from.y + 2.0 * inverse * t * control.y + t * t * to.y };
            line(previous, next);
            previous = next;
        }
    }

    // The coverage of each pixel, kept where it exceeds what the plane holds.
    void composite(int originX, int originY, int planeWidth, int planeHeight, std::span<std::uint8_t> plane) const
    {
        const int stride = m_width + 2;
        for (int row = 0; row < m_height; ++row) {
            const int planeY = originY + row;
            double sum = 0.0;
            for (int column = 0; column < m_width; ++column) {
                sum += m_area[static_cast<std::size_t>(row) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(column)];
                const int planeX = originX + column;
                if (planeY < 0 || planeY >= planeHeight || planeX < 0 || planeX >= planeWidth)
                    continue;
                const auto value = static_cast<std::uint8_t>(std::lround(std::min(1.0, std::fabs(sum)) * 255.0));
                std::uint8_t& destination = plane[static_cast<std::size_t>(planeY) * static_cast<std::size_t>(planeWidth) + static_cast<std::size_t>(planeX)];
                destination = std::max(destination, value);
            }
        }
    }

private:
    int m_width;
    int m_height;
    std::vector<float> m_area;
};

} // namespace

bool hasGlyph(ProportionalStrike::Face face, std::uint32_t codePoint)
{
    return glyphFor(outlineSet().faces[static_cast<int>(face)], codePoint) != nullptr;
}

bool rasterise(ProportionalStrike::Face face, std::uint32_t codePoint, double pixelsPerEm, double horizontalScale,
    double penX, double baselineY, int width, int height, std::span<std::uint8_t> coverage, double slant)
{
    if (width <= 0 || height <= 0 || coverage.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) || pixelsPerEm <= 0.0)
        return false;
    const FaceOutlines& outlines = outlineSet().faces[static_cast<int>(face)];
    const GlyphRecord* glyph = glyphFor(outlines, codePoint);
    if (!glyph || outlines.unitsPerEm <= 0)
        return false;
    if (glyph->pointCount == 0)
        return true;
    // Font units to the plane's pixels, y down; a slant moves each point
    // across by its height above the baseline.
    const double scaleY = pixelsPerEm / outlines.unitsPerEm;
    const double scaleX = scaleY * horizontalScale;
    const auto pointAt = [&](int index) {
        const std::size_t at = 2 * (static_cast<std::size_t>(glyph->firstPoint) + static_cast<std::size_t>(index));
        const double rise = outlines.points[at + 1] * scaleY;
        return Point { penX + outlines.points[at] * scaleX + slant * rise, baselineY - rise };
    };
    double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (int index = 0; index < glyph->pointCount; ++index) {
        const Point point = pointAt(index);
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    const int originX = static_cast<int>(std::floor(minX));
    const int originY = static_cast<int>(std::floor(minY));
    const int boxWidth = static_cast<int>(std::ceil(maxX)) - originX + 1;
    const int boxHeight = static_cast<int>(std::ceil(maxY)) - originY + 1;
    if (boxWidth <= 0 || boxHeight <= 0 || boxWidth > 4096 || boxHeight > 4096)
        return false;
    Accumulator accumulator(boxWidth, boxHeight);
    const auto local = [&](Point point) { return Point { point.x - originX, point.y - originY }; };
    int first = 0;
    for (int contour = 0; contour < glyph->contourCount; ++contour) {
        const int last = outlines.ends[glyph->firstEnd + static_cast<std::uint32_t>(contour)];
        const int count = last - first + 1;
        if (count < 2 || last >= glyph->pointCount) {
            first = last + 1;
            continue;
        }
        const auto isOn = [&](int index) { return outlines.isOnCurve[glyph->firstPoint + static_cast<std::uint32_t>(first + (index % count))] != 0; };
        const auto at = [&](int index) { return local(pointAt(first + (index % count))); };
        // A contour may begin off the curve; its first on-curve point, or the
        // midpoint of its first two controls, starts it.
        int start = 0;
        while (start < count && !isOn(start))
            ++start;
        Point startPoint;
        if (start == count) {
            const Point a = at(0);
            const Point b = at(1);
            startPoint = { 0.5 * (a.x + b.x), 0.5 * (a.y + b.y) };
            start = 0;
        } else {
            startPoint = at(start);
        }
        Point current = startPoint;
        bool hasControl = false;
        Point control;
        for (int step = 1; step <= count; ++step) {
            const int index = start + step;
            const Point point = at(index);
            if (isOn(index)) {
                if (hasControl)
                    accumulator.quadratic(current, control, point);
                else
                    accumulator.line(current, point);
                current = point;
                hasControl = false;
            } else if (hasControl) {
                // Two controls in a row imply the curve's point between them.
                const Point middle { 0.5 * (control.x + point.x), 0.5 * (control.y + point.y) };
                accumulator.quadratic(current, control, middle);
                current = middle;
                control = point;
            } else {
                control = point;
                hasControl = true;
            }
        }
        if (hasControl)
            accumulator.quadratic(current, control, startPoint);
        else if (current.x != startPoint.x || current.y != startPoint.y)
            accumulator.line(current, startPoint);
        first = last + 1;
    }
    accumulator.composite(originX, originY, width, height, coverage);
    return true;
}

} // namespace Ubiquitty::Rendering::ProportionalOutline
