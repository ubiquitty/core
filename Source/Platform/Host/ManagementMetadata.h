// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform/Host/TokenStore.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <memory>
#include <sstream>

namespace Ubiquitty::Host {

/// Records provenance only; authorization always comes from the token and grant stores.
struct ManagementMetadata {
    std::string identifier;
    std::uint64_t createdUnix = 0;
    std::string createdBy;
    std::uint64_t lastUsedUnix = 0;
    std::string lastUsedBy;
};

/// Reads atomic provenance snapshots and commits while the caller holds TokenStoreLock.
class ManagementMetadataStore {
public:
    explicit ManagementMetadataStore(const std::string& tokenPath)
        : m_path(tokenPath + ".metadata")
    {
        PAL::UnixFileDescriptor descriptor(::open(m_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (!descriptor) {
            m_isValid = errno == ENOENT;
            return;
        }
        struct stat information {};
        if (::fstat(descriptor.get(), &information) != 0 || !S_ISREG(information.st_mode)
            || information.st_uid != ::geteuid() || (information.st_mode & 077)
            || information.st_size > 8 * 1024 * 1024) {
            return;
        }
        const auto closeFile = [](FILE* file) { std::fclose(file); };
        std::unique_ptr<FILE, decltype(closeFile)> file(::fdopen(descriptor.get(), "r"), closeFile);
        if (!file) {
            return;
        }
        (void)descriptor.release();
        std::array<char, 8192> line;
        if (!std::fgets(line.data(), static_cast<int>(line.size()), file.get()) || std::string_view(line.data()) != "ubiquitty-management-metadata 1\n") {
            return;
        }
        while (std::fgets(line.data(), static_cast<int>(line.size()), file.get())) {
            if (m_records.size() >= 4096 || !std::string_view(line.data()).ends_with('\n')) {
                return;
            }
            ManagementMetadata record;
            std::istringstream input(line.data());
            if (!(input >> std::quoted(record.identifier) >> record.createdUnix >> std::quoted(record.createdBy)
                    >> record.lastUsedUnix >> std::quoted(record.lastUsedBy))
                || !validText(record.identifier, 4096) || !validText(record.createdBy, 128) || !validText(record.lastUsedBy, 128)) {
                return;
            }
            input >> std::ws;
            if (!input.eof()) {
                return;
            }
            m_records.push_back(std::move(record));
        }
        m_isValid = !std::ferror(file.get());
    }

    /// Reports whether the complete metadata file was read successfully.
    [[nodiscard]] bool isValid() const { return m_isValid; }

    /// Returns the recorded facts, or unknown facts when none were captured.
    [[nodiscard]] ManagementMetadata metadata(std::string_view identifier) const
    {
        if (m_isValid) {
            for (const auto& record : m_records) {
                if (record.identifier == identifier) {
                    return record;
                }
            }
        }
        return {};
    }

    /// Records the time and actor of an explicit issuance or grant.
    [[nodiscard]] bool created(std::string identifier, std::uint64_t time, std::string actor)
    {
        auto* record = entry(std::move(identifier), actor);
        if (!record) {
            return false;
        }
        record->createdUnix = time;
        record->createdBy = std::move(actor);
        return save();
    }

    /// Records an authenticated use without inventing its creation history.
    [[nodiscard]] bool used(std::string identifier, std::uint64_t time, std::string actor)
    {
        auto* record = entry(std::move(identifier), actor);
        if (!record) {
            return false;
        }
        record->lastUsedUnix = time;
        record->lastUsedBy = std::move(actor);
        return save();
    }

private:
    /// Rejects strings that could exceed a record or introduce another line.
    ///
    /// - Parameters:
    ///   - text: The metadata field to validate.
    ///   - maximum: The maximum field length in bytes.
    /// - Returns: Whether the field is safe to serialize.
    static bool validText(std::string_view text, std::size_t maximum)
    {
        return text.size() <= maximum && std::ranges::none_of(text, [](unsigned char byte) { return byte < 32 || byte == 127; });
    }

    /// Finds the identifier or allocates a bounded provenance record.
    ///
    /// - Parameters:
    ///   - identifier: The token or grant identity to record.
    ///   - actor: The provenance label to validate.
    /// - Returns: The mutable record, or null when the store or input is invalid.
    ManagementMetadata* entry(std::string identifier, std::string_view actor)
    {
        if (!m_isValid || !validText(identifier, 4096) || !validText(actor, 128)) {
            return nullptr;
        }
        for (auto& record : m_records) {
            if (record.identifier == identifier) {
                return &record;
            }
        }
        if (m_records.size() >= 4096) {
            const auto oldest = std::ranges::min_element(m_records, {}, [](const auto& record) { return std::max(record.createdUnix, record.lastUsedUnix); });
            m_records.erase(oldest);
        }
        m_records.push_back({ std::move(identifier), 0, {}, 0, {} });
        return &m_records.back();
    }

    /// Commits the complete provenance snapshot by an atomic rename.
    ///
    /// - Returns: Whether the file and containing directory were committed.
    bool save()
    {
        std::ostringstream output;
        output << "ubiquitty-management-metadata 1\n";
        for (const auto& record : m_records) {
            output << std::quoted(record.identifier) << ' ' << record.createdUnix << ' ' << std::quoted(record.createdBy)
                   << ' ' << record.lastUsedUnix << ' ' << std::quoted(record.lastUsedBy) << '\n';
        }
        const auto bytes = output.str();
        if (bytes.size() > 8 * 1024 * 1024) {
            return false;
        }
        std::string temporary = m_path + ".XXXXXX";
        PAL::UnixFileDescriptor descriptor(::mkstemp(temporary.data()));
        if (!descriptor) {
            return false;
        }
        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(descriptor.get(), bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                ::unlink(temporary.c_str());
                return false;
            }
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(descriptor.get()) != 0 || ::rename(temporary.c_str(), m_path.c_str()) != 0) {
            ::unlink(temporary.c_str());
            return false;
        }
        return syncTokenParent(tokenParentDirectory(m_path).c_str()) == 0;
    }

    std::string m_path;
    std::vector<ManagementMetadata> m_records;
    bool m_isValid = false;
};

} // namespace Ubiquitty::Host
