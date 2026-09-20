#pragma once
// Blob frames — the one binary framing of every `blob` bulk channel: a named,
// sized blob sent as offset-ordered chunks that an envelope refers to. The
// chunk codec, the reference object and the receive-side assembler live here so
// no capability re-implements them. spec: docs/08-protocol.md#blob-frames
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <fjarr/capability.hpp>

namespace fjarr::blob {

inline constexpr std::uint8_t VERSION = 1;
inline constexpr std::size_t HEADER_BYTES = 37;                   // version(1) id(16) offset(8) len(8) payload_len(4)
inline constexpr std::size_t MAX_CHUNK_PAYLOAD = 256 * 1024;      // and ≤ the SCTP message limit (docs/08)
inline constexpr std::size_t DEFAULT_MAX_BLOB = 16 * 1024 * 1024; // whole-blob reassembly cap
inline constexpr std::size_t PENDING_BYTES = 8 * 1024 * 1024;     // incomplete/unclaimed blobs kept per channel
inline constexpr std::chrono::milliseconds PENDING_TTL{30'000};

/// What travels in an envelope: `{"blob": id, "len": n, "type": media type}`
/// (the `blob-ref` schema fragment).
struct BlobRef {
    std::string id;
    std::uint64_t len = 0;
    std::string type;
    nlohmann::json to_json() const;
    static std::optional<BlobRef> from_json(const nlohmann::json &j);
};

/// UUID text ↔ the 16 header bytes. nullopt for anything but 8-4-4-4-12 hex.
std::optional<std::array<std::uint8_t, 16>> uuid_bytes(std::string_view text);
std::string uuid_text(const std::array<std::uint8_t, 16> &bytes);

/// One frame: header + payload. Throws FjarrError(payload-invalid) for a
/// malformed id or an oversized payload.
std::vector<std::byte> encode_chunk(std::string_view blob_id, std::uint64_t offset, std::uint64_t blob_len,
                                    std::span<const std::byte> payload);
/// Parse a frame; nullopt when the version, the lengths or the offset disagree
/// (docs/08: dropped and counted). The chunk's payload view points into
/// `frame`.
std::optional<BlobChunk> parse_chunk(std::span<const std::byte> frame);

/// Collects chunks into whole blobs under the docs/08 bounds: a blob above
/// `max_blob` is refused, the store holds at most `pending_bytes` (oldest
/// evicted) and forgets a blob idle for `ttl`. Chunks of one blob must arrive
/// in offset order (the channel is ordered); a gap discards the blob.
class BlobAssembler {
  public:
    using clock = std::chrono::steady_clock;
    explicit BlobAssembler(std::size_t max_blob = DEFAULT_MAX_BLOB, std::size_t pending_bytes = PENDING_BYTES,
                           std::chrono::milliseconds ttl = PENDING_TTL);
    /// Returns the blob id when this chunk completed it.
    std::optional<std::string> on_chunk(const BlobChunk &chunk, clock::time_point now = clock::now());
    bool complete(const std::string &id) const;
    /// The bytes of a complete blob, removed from the store; nullopt if unknown
    /// or incomplete.
    std::optional<std::string> take(const std::string &id);
    void discard(const std::string &id);
    void expire(clock::time_point now = clock::now());
    std::size_t pending_bytes() const { return bytes_; }
    std::size_t size() const { return entries_.size(); }
    unsigned dropped() const { return dropped_; }

  private:
    struct Entry {
        std::string data;
        std::uint64_t len = 0;
        bool complete = false;
        clock::time_point first{};
        clock::time_point last{};
    };
    void erase(std::map<std::string, Entry>::iterator it);
    std::size_t max_blob_;
    std::size_t limit_;
    std::chrono::milliseconds ttl_;
    std::map<std::string, Entry> entries_;
    std::size_t bytes_ = 0;
    unsigned dropped_ = 0;
};

} // namespace fjarr::blob
