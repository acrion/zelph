#include "network/hf_cache.hpp"
#include "network/hf_transfer.hpp"

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
    const auto cached  = metadata("\"old\"", "old-revision");
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
    ObjectCoordinates    same{"hf://datasets/acrion/zelph/shard", "revision-a", "\"same\"", 10, 20};
    ObjectCoordinates    changed_revision{"hf://datasets/acrion/zelph/shard", "revision-b", "\"same\"", 10, 20};

    CHECK(decide_object(true, cached, same) == ReuseDecision::reuse);
    CHECK(decide_object(true, cached, changed_revision) == ReuseDecision::refetch);
    CHECK(decide_object(false, cached, same) == ReuseDecision::refetch);
}

TEST_CASE("HF cache sidecars round-trip escaped metadata")
{
    const auto           path = std::filesystem::temp_directory_path() / "zelph-hf-cache-sidecar-test.meta";
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

TEST_CASE("HF transfer budgets use conservative host throughput")
{
    using namespace zelph::network::detail::hf_transfer;

    const Limits limits{};
    CHECK(conservative_rate(std::nullopt, limits) == 262144);
    CHECK(conservative_rate(HostStats{"huggingface.co", 1048576, 3}, limits) == 262144);
    CHECK(conservative_rate(HostStats{"huggingface.co", 1024, 3}, limits) == 65536);
    CHECK(payload_timeout_seconds(16ULL * 1024 * 1024, 262144, limits) == 301);
}

TEST_CASE("HF transfer command includes phase-aware curl limits")
{
    using namespace zelph::network::detail::hf_transfer;

    const Limits  limits{};
    const Request probe{Operation::probe, "hf://datasets/a/b/manifest.json", "https://example.test/manifest.json", {}, "/tmp/headers", "/tmp/metrics", {}, {}, 0, 0};
    const auto    probe_command = build_curl_command(probe, limits, 262144);
    CHECK(probe_command.find("--connect-timeout 15") != std::string::npos);
    CHECK(probe_command.find("--max-time 45") != std::string::npos);
    CHECK(probe_command.find("--retry-max-time 45") != std::string::npos);
    CHECK(probe_command.find(" -I ") != std::string::npos);

    const Request fetch{Operation::fetch, "hf://datasets/a/b/shard", "https://example.test/shard", "/tmp/body", {}, "/tmp/metrics", "revision", "etag", 10, 20};
    const auto    fetch_command = build_curl_command(fetch, limits, 262144);
    CHECK(fetch_command.find("--speed-limit 1024") != std::string::npos);
    CHECK(fetch_command.find("--speed-time 90") != std::string::npos);
    CHECK(fetch_command.find("--range 10-29") != std::string::npos);
}

TEST_CASE("HF transfer metrics classify completed slow and stalled calls")
{
    using namespace zelph::network::detail::hf_transfer;

    const Request fetch{Operation::fetch, "hf://datasets/a/b/shard", "https://example.test/shard", "/tmp/body", {}, "/tmp/metrics", {}, {}, 0, 1024 * 1024};
    Metrics       completed{};
    completed.curl_exit                = 0;
    completed.average_bytes_per_second = 100000;
    CHECK(classify(fetch, completed, 262144) == Outcome::slow_completed);
    completed.average_bytes_per_second = 262144;
    CHECK(classify(fetch, completed, 262144) == Outcome::completed);

    Metrics stalled{};
    stalled.curl_exit                = 28;
    stalled.connect_milliseconds     = 1;
    stalled.received_bytes           = 10;
    stalled.average_bytes_per_second = 1;
    CHECK(classify(fetch, stalled, 262144) == Outcome::stalled);

    Metrics response_timeout{};
    response_timeout.curl_exit            = 28;
    response_timeout.connect_milliseconds = 1;
    CHECK(classify(fetch, response_timeout, 262144) == Outcome::response_timeout);
}

TEST_CASE("HF transfer metrics parse curl JSON")
{
    using namespace zelph::network::detail::hf_transfer;

    const auto path = std::filesystem::temp_directory_path() / "zelph-hf-transfer-metrics.json";
    {
        std::ofstream output(path);
        output << R"({"time_namelookup":0.01,"time_connect":0.02,"time_appconnect":0.03,"time_starttransfer":0.04,"time_total":0.50,"size_download":1234,"speed_download":2468,"http_code":200,"exitcode":0})";
    }
    const auto metrics = read_metrics(path);
    REQUIRE(metrics.has_value());
    CHECK(metrics->dns_milliseconds == 10);
    CHECK(metrics->connect_milliseconds == 20);
    CHECK(metrics->tls_milliseconds == 30);
    CHECK(metrics->first_byte_milliseconds == 40);
    CHECK(metrics->total_milliseconds == 500);
    CHECK(metrics->received_bytes == 1234);
    CHECK(metrics->average_bytes_per_second == 2468);
    CHECK(metrics->http_status == 200);
    CHECK(metrics->curl_exit == 0);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("HF transfer throughput history ignores latency-dominated objects")
{
    using namespace zelph::network::detail::hf_transfer;

    const auto      root = std::filesystem::temp_directory_path() / "zelph-hf-transfer-host-stats";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    Metrics metrics{};
    metrics.average_bytes_per_second = 1024 * 1024;

    update_host_stats(root, "huggingface.co", 31, metrics);
    CHECK_FALSE(read_host_stats(host_stats_path(root, "huggingface.co")).has_value());

    update_host_stats(root, "huggingface.co", 64 * 1024, metrics);
    const auto stored = read_host_stats(host_stats_path(root, "huggingface.co"));
    REQUIRE(stored.has_value());
    CHECK(stored->throughput_bytes_per_second == 1024 * 1024);
    std::filesystem::remove_all(root, ignored);
}
