/*
Copyright (c) 2026 acrion innovations GmbH

Small, offline-testable policy layer for the Hugging Face remote cache.
*/
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace zelph::network::detail::hf_cache
{
    inline constexpr const char* cache_version = "v2";

    struct RemoteMetadata
    {
        std::string source_uri;
        std::string revision;
        std::string etag;
        uint64_t    content_length = 0;
        int64_t     retrieved_at   = 0;
    };

    struct ObjectCoordinates
    {
        std::string source_uri;
        std::string revision;
        std::string etag;
        uint64_t    offset = 0;
        uint64_t    length = 0;
    };

    enum class ReuseDecision
    {
        refetch,
        reuse,
        reuse_offline,
    };

    inline bool has_identity(const RemoteMetadata& metadata)
    {
        return !metadata.revision.empty() || !metadata.etag.empty();
    }

    inline bool same_remote(const RemoteMetadata& a, const RemoteMetadata& b)
    {
        if (a.source_uri != b.source_uri || a.content_length != b.content_length)
        {
            return false;
        }
        if (!a.revision.empty() && !b.revision.empty() && a.revision != b.revision)
        {
            return false;
        }
        if (!a.etag.empty() && !b.etag.empty() && a.etag != b.etag)
        {
            return false;
        }
        return has_identity(a) && has_identity(b);
    }

    inline ReuseDecision decide_manifest(const bool body_exists,
                                         const std::optional<RemoteMetadata>& cached,
                                         const std::optional<RemoteMetadata>& observed)
    {
        if (!body_exists)
        {
            return ReuseDecision::refetch;
        }
        if (observed && cached && same_remote(*cached, *observed))
        {
            return ReuseDecision::reuse;
        }
        // A failed HEAD/resolve must not make an otherwise usable manifest
        // unavailable. Callers must emit a diagnostic when taking this path.
        // A body without a valid sidecar is not an offline fallback candidate.
        if (!observed)
        {
            return cached ? ReuseDecision::reuse_offline : ReuseDecision::refetch;
        }
        return ReuseDecision::refetch;
    }

    inline ReuseDecision decide_object(const bool body_exists,
                                       const std::optional<RemoteMetadata>& cached,
                                       const ObjectCoordinates& requested)
    {
        if (!body_exists || !cached)
        {
            return ReuseDecision::refetch;
        }

        // The sidecar stores the full remote object's Content-Length, while
        // `requested.length` is only this range's length.  Range identity is
        // part of the filename; compare the remote identity here.
        if (cached->source_uri == requested.source_uri
            && (cached->revision.empty() || requested.revision.empty() || cached->revision == requested.revision)
            && (cached->etag.empty() || requested.etag.empty() || cached->etag == requested.etag)
            && has_identity(*cached))
        {
            return ReuseDecision::reuse;
        }
        return ReuseDecision::refetch;
    }

    inline std::string escape_field(std::string value)
    {
        for (size_t pos = 0; (pos = value.find('\\', pos)) != std::string::npos; pos += 2)
        {
            value.replace(pos, 1, "\\\\");
        }
        for (size_t pos = 0; (pos = value.find('\n', pos)) != std::string::npos; pos += 2)
        {
            value.replace(pos, 1, "\\n");
        }
        return value;
    }

    inline std::string unescape_field(std::string value)
    {
        for (size_t pos = 0; (pos = value.find("\\\\", pos)) != std::string::npos;)
        {
            value.replace(pos, 2, "\\");
            ++pos;
        }
        for (size_t pos = 0; (pos = value.find("\\n", pos)) != std::string::npos;)
        {
            value.replace(pos, 2, "\n");
            ++pos;
        }
        return value;
    }

    inline bool write_sidecar(const std::filesystem::path& path, const RemoteMetadata& metadata)
    {
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            return false;
        }
        out << "version=" << cache_version << '\n'
            << "source_uri=" << escape_field(metadata.source_uri) << '\n'
            << "revision=" << escape_field(metadata.revision) << '\n'
            << "etag=" << escape_field(metadata.etag) << '\n'
            << "content_length=" << metadata.content_length << '\n'
            << "retrieved_at=" << metadata.retrieved_at << '\n';
        return static_cast<bool>(out);
    }

    inline std::optional<RemoteMetadata> read_sidecar(const std::filesystem::path& path)
    {
        std::ifstream in(path);
        if (!in)
        {
            return std::nullopt;
        }

        RemoteMetadata metadata;
        std::string version;
        std::string line;
        while (std::getline(in, line))
        {
            const auto equals = line.find('=');
            if (equals == std::string::npos)
            {
                continue;
            }
            const auto key   = line.substr(0, equals);
            const auto value = line.substr(equals + 1);
            if (key == "version") version = value;
            else if (key == "source_uri") metadata.source_uri = unescape_field(value);
            else if (key == "revision") metadata.revision = unescape_field(value);
            else if (key == "etag") metadata.etag = unescape_field(value);
            else if (key == "content_length")
            {
                try { metadata.content_length = std::stoull(value); }
                catch (...) { return std::nullopt; }
            }
            else if (key == "retrieved_at")
            {
                try { metadata.retrieved_at = std::stoll(value); }
                catch (...) { return std::nullopt; }
            }
        }
        if (version != cache_version || metadata.source_uri.empty())
        {
            return std::nullopt;
        }
        return metadata;
    }
}
