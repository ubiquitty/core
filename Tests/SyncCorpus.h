// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/SyncCorpus.h — reads the Sync PTY corpus (Tests/Corpus/sync).
//
// An entry is a stream of bytes a program wrote to its PTY, cut into the reads a
// host makes off it: when each became readable, how long it is, and whether a
// would-block followed. Synthetic entries also mark each logical redraw's byte
// span, which is what lets a measurement say a frame showed half a repaint.
//
// The format is the manifest's and the .reads files' (see manifest.txt); this
// reader refuses anything else rather than guessing, so a malformed entry is a
// failing test with the line that broke it, not a silently shorter replay.

#include "LoopbackPaneLink.h"

#include "Core/Codec/ZlibInflate.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace UbiquittyTest {

/// One logical repaint's byte span, half-open.
struct RedrawSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};

/// One recorded read, by offset into the entry's bytes.
struct CorpusRead {
    std::uint64_t atMicroseconds = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
    bool isFollowedByWouldBlock = true;
};

/// One corpus entry, loaded.
struct CorpusEntry {
    std::string name;
    int rows = 0;
    int columns = 0;
    /// `synthetic` or `recorded`.
    std::string origin;
    /// The first comment line of the .reads file.
    std::string description;
    std::string bytes;
    std::vector<CorpusRead> reads;
    std::vector<RedrawSpan> redraws;
    /// When the user sent each key, on the replay clock; the first read at or
    /// after it is the key's echo. Synthetic entries only.
    std::vector<std::uint64_t> inputs;

    /// The reads as the loopback link replays them; views into `bytes`.
    [[nodiscard]] std::vector<PTYRead> ptyReads() const
    {
        std::vector<PTYRead> out;
        out.reserve(reads.size());
        for (const CorpusRead& read : reads)
            out.push_back({ read.atMicroseconds, std::string_view(bytes).substr(read.offset, read.length), read.isFollowedByWouldBlock });
        return out;
    }

    /// Whether `offset` lies strictly inside a marked redraw: some of the
    /// repaint has been parsed and some has not.
    [[nodiscard]] bool isInsideRedraw(std::size_t offset) const
    {
        for (const RedrawSpan& span : redraws) {
            if (offset > span.begin && offset < span.end)
                return true;
        }
        return false;
    }
};

/// Returns the corpus directory, as the build system names it.
///
/// - Returns: The directory, or an empty path in a target the build did not
///   hand one.
inline std::filesystem::path syncCorpusDirectory()
{
#ifdef UBQ_SYNC_PTY_CORPUS_DIR
    return std::filesystem::path(UBQ_SYNC_PTY_CORPUS_DIR);
#else
    return {};
#endif
}

/// Loads every entry the manifest names.
///
/// - Parameters:
///   - directory: The corpus directory.
///   - error: Receives what refused, naming the file and line.
/// - Returns: The entries, or nothing when any file is missing or malformed.
inline std::optional<std::vector<CorpusEntry>> loadSyncCorpus(const std::filesystem::path& directory, std::string& error)
{
    const auto readFile = [](const std::filesystem::path& path, std::string& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file.good())
            return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    };

    std::string manifest;
    if (!readFile(directory / "manifest.txt", manifest)) {
        error = "no manifest at " + (directory / "manifest.txt").string();
        return std::nullopt;
    }
    std::vector<CorpusEntry> entries;
    std::istringstream manifestLines(manifest);
    std::string line;
    int lineNumber = 0;
    while (std::getline(manifestLines, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream fields(line);
        CorpusEntry entry;
        std::string streamName;
        if (!(fields >> entry.name >> entry.rows >> entry.columns >> entry.origin >> streamName) || entry.rows <= 0 || entry.columns <= 0) {
            error = "manifest.txt:" + std::to_string(lineNumber) + ": want <name> <rows> <columns> <origin> <stream>";
            return std::nullopt;
        }
        if (entry.origin != "synthetic" && entry.origin != "recorded") {
            error = "manifest.txt:" + std::to_string(lineNumber) + ": origin is synthetic or recorded, not " + entry.origin;
            return std::nullopt;
        }

        std::string readsText;
        const std::filesystem::path readsPath = directory / (entry.name + ".reads");
        if (!readFile(readsPath, readsText)) {
            error = "no reads file " + readsPath.string();
            return std::nullopt;
        }
        std::istringstream readsLines(readsText);
        std::string readsLine;
        int readsLineNumber = 0;
        std::size_t offset = 0;
        std::uint64_t previousAt = 0;
        while (std::getline(readsLines, readsLine)) {
            ++readsLineNumber;
            const std::string where = entry.name + ".reads:" + std::to_string(readsLineNumber);
            if (readsLine.empty())
                continue;
            if (readsLine[0] == '#') {
                if (entry.description.empty())
                    entry.description = readsLine.substr(readsLine.find_first_not_of("# "));
                continue;
            }
            std::istringstream readFields(readsLine);
            if (readsLine.rfind("redraw ", 0) == 0) {
                std::string keyword;
                RedrawSpan span;
                if (!(readFields >> keyword >> span.begin >> span.end) || span.end <= span.begin) {
                    error = where + ": want redraw <begin> <end>";
                    return std::nullopt;
                }
                entry.redraws.push_back(span);
                continue;
            }
            if (readsLine.rfind("input ", 0) == 0) {
                std::string keyword;
                std::uint64_t at = 0;
                if (!(readFields >> keyword >> at)) {
                    error = where + ": want input <microseconds>";
                    return std::nullopt;
                }
                entry.inputs.push_back(at);
                continue;
            }
            CorpusRead read;
            std::string ending;
            if (!(readFields >> read.atMicroseconds >> read.length >> ending) || read.length == 0 || (ending != "block" && ending != "more")) {
                error = where + ": want <microseconds> <length> <block|more>";
                return std::nullopt;
            }
            if (read.atMicroseconds < previousAt) {
                error = where + ": a read earlier than the one before it";
                return std::nullopt;
            }
            previousAt = read.atMicroseconds;
            read.offset = offset;
            read.isFollowedByWouldBlock = ending == "block";
            offset += read.length;
            entry.reads.push_back(read);
        }

        std::string stored;
        if (!readFile(directory / streamName, stored)) {
            error = "no stream " + (directory / streamName).string();
            return std::nullopt;
        }
        if (streamName.ends_with(".z")) {
            entry.bytes.resize(offset);
            const std::span<const std::uint8_t> input(reinterpret_cast<const std::uint8_t*>(stored.data()), stored.size()); // the stored bytes viewed as the zlib stream they are
            const std::span<std::uint8_t> output(reinterpret_cast<std::uint8_t*>(entry.bytes.data()), entry.bytes.size()); // the string's storage viewed as inflate's output
            if (!Ubiquitty::Codec::Zlib::inflateExact(input, output)) {
                error = streamName + ": does not inflate to the " + std::to_string(offset) + " bytes its reads add up to";
                return std::nullopt;
            }
        } else {
            entry.bytes = std::move(stored);
        }
        if (entry.bytes.size() != offset) {
            error = entry.name + ": the reads add up to " + std::to_string(offset) + " bytes and the stream holds " + std::to_string(entry.bytes.size());
            return std::nullopt;
        }
        for (const RedrawSpan& span : entry.redraws) {
            if (span.end > entry.bytes.size()) {
                error = entry.name + ": a redraw ends past the stream";
                return std::nullopt;
            }
        }
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        error = "the manifest names no entries";
        return std::nullopt;
    }
    return entries;
}

} // namespace UbiquittyTest
