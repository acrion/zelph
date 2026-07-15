#include "network/hf_cache.hpp"

#include <doctest/doctest.h>

#include <filesystem>

namespace
{
    using namespace zelph::network::detail::hf_cache;

    RemoteMetadata metadata(const char* etag, const char* revision)
    {
        return {"hf://datasets/acrion/zelph/manifest.json", revision, etag, 123, 42};
    }
}

TEST_CASE("HF manifest cache revalidates identities")
{
    const auto cached = metadata("\"old\"", "old-revision");
    const auto current = metadata("\"new\"", "new-revision");

    CHECK(decide_manifest(true, cached, current) == ReuseDecision::refetch);
    CHECK(decide_manifest(true, cached, cached) == ReuseDecision::reuse);
    CHECK(decide_manifest(true, cached, std::nullopt) == ReuseDecision::reuse_offline);
    CHECK(decide_manifest(true, std::nullopt, std::nullopt) == ReuseDecision::refetch);
    CHECK(decide_manifest(false, cached, cached) == ReuseDecision::refetch);
}

TEST_CASE("HF object cache identity includes revision and range")
{
    const RemoteMetadata cached{"hf://datasets/acrion/zelph/shard", "revision-a", "\"same\"", 123, 42};
    ObjectCoordinates same{"hf://datasets/acrion/zelph/shard", "revision-a", "\"same\"", 10, 20};
    ObjectCoordinates changed_revision{"hf://datasets/acrion/zelph/shard", "revision-b", "\"same\"", 10, 20};

    CHECK(decide_object(true, cached, same) == ReuseDecision::reuse);
    CHECK(decide_object(true, cached, changed_revision) == ReuseDecision::refetch);
    CHECK(decide_object(false, cached, same) == ReuseDecision::refetch);
}

TEST_CASE("HF cache sidecars round-trip escaped metadata")
{
    const auto path = std::filesystem::temp_directory_path() / "zelph-hf-cache-sidecar-test.meta";
    const RemoteMetadata original{"hf://datasets/a/b/file\nname", "revision", "etag=value", 99, 1234};

    REQUIRE(write_sidecar(path, original));
    const auto loaded = read_sidecar(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->source_uri == original.source_uri);
    CHECK(loaded->revision == original.revision);
    CHECK(loaded->etag == original.etag);
    CHECK(loaded->content_length == original.content_length);
    CHECK(loaded->retrieved_at == original.retrieved_at);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
