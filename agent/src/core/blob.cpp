// spec: docs/08-protocol.md#blob-frames
#include <fjarr/blob.hpp>

#include <algorithm>
#include <cstring>

#include <fjarr/errors.hpp>

namespace fjarr::blob {

namespace {
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
void put_u64(std::byte *out, std::uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        out[i] = static_cast<std::byte>(v & 0xff);
        v >>= 8;
    }
}
void put_u32(std::byte *out, std::uint32_t v) {
    for (int i = 3; i >= 0; i--) {
        out[i] = static_cast<std::byte>(v & 0xff);
        v >>= 8;
    }
}
std::uint64_t get_u64(const std::byte *in) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | static_cast<std::uint8_t>(in[i]);
    return v;
}
std::uint32_t get_u32(const std::byte *in) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | static_cast<std::uint8_t>(in[i]);
    return v;
}
} // namespace

nlohmann::json BlobRef::to_json() const { return nlohmann::json{{"blob", id}, {"len", len}, {"type", type}}; }

std::optional<BlobRef> BlobRef::from_json(const nlohmann::json &j) {
    if (!j.is_object() || !j.contains("blob") || !j["blob"].is_string() || !j.contains("len") || !j["len"].is_number_unsigned())
        return std::nullopt;
    BlobRef r;
    r.id = j["blob"].get<std::string>();
    r.len = j["len"].get<std::uint64_t>();
    if (j.contains("type") && j["type"].is_string()) r.type = j["type"].get<std::string>();
    if (!uuid_bytes(r.id)) return std::nullopt;
    return r;
}

std::optional<std::array<std::uint8_t, 16>> uuid_bytes(std::string_view text) {
    if (text.size() != 36) return std::nullopt;
    std::array<std::uint8_t, 16> out{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return std::nullopt;
            continue;
        }
        const int hi = hex_value(c);
        const int lo = hex_value(text[++i]);
        if (hi < 0 || lo < 0 || n >= 16) return std::nullopt;
        out[n++] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return n == 16 ? std::optional{out} : std::nullopt;
}

std::string uuid_text(const std::array<std::uint8_t, 16> &bytes) {
    static const char *hex = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (std::size_t i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += hex[bytes[i] >> 4];
        s += hex[bytes[i] & 0xf];
    }
    return s;
}

std::vector<std::byte> encode_chunk(std::string_view blob_id, std::uint64_t offset, std::uint64_t blob_len,
                                    std::span<const std::byte> payload) {
    const auto id = uuid_bytes(blob_id);
    if (!id) throw FjarrError("payload-invalid", "blob id is not a uuid: " + std::string(blob_id));
    if (payload.size() > MAX_CHUNK_PAYLOAD) throw FjarrError("payload-invalid", "blob chunk above 256 KiB (docs/08)");
    if (offset + payload.size() > blob_len) throw FjarrError("payload-invalid", "blob chunk overruns blob_len");
    std::vector<std::byte> frame(HEADER_BYTES + payload.size());
    frame[0] = static_cast<std::byte>(VERSION);
    for (std::size_t i = 0; i < 16; i++) frame[1 + i] = static_cast<std::byte>((*id)[i]);
    put_u64(frame.data() + 17, offset);
    put_u64(frame.data() + 25, blob_len);
    put_u32(frame.data() + 33, static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) std::memcpy(frame.data() + HEADER_BYTES, payload.data(), payload.size());
    return frame;
}

std::optional<BlobChunk> parse_chunk(std::span<const std::byte> frame) {
    if (frame.size() < HEADER_BYTES) return std::nullopt;
    if (static_cast<std::uint8_t>(frame[0]) != VERSION) return std::nullopt;
    std::array<std::uint8_t, 16> id{};
    for (std::size_t i = 0; i < 16; i++) id[i] = static_cast<std::uint8_t>(frame[1 + i]);
    BlobChunk c;
    c.blob_id = uuid_text(id);
    c.offset = get_u64(frame.data() + 17);
    c.blob_len = get_u64(frame.data() + 25);
    const std::uint32_t payload_len = get_u32(frame.data() + 33);
    if (payload_len != frame.size() - HEADER_BYTES) return std::nullopt; // payload_len disagrees with the message
    if (payload_len > MAX_CHUNK_PAYLOAD) return std::nullopt;
    if (c.offset > c.blob_len || payload_len > c.blob_len - c.offset) return std::nullopt; // overruns blob_len
    c.payload = frame.subspan(HEADER_BYTES);
    return c;
}

// ------------------------------------------------------------ assembler

BlobAssembler::BlobAssembler(std::size_t max_blob, std::size_t pending_bytes, std::chrono::milliseconds ttl)
    : max_blob_(max_blob),
      limit_(pending_bytes),
      ttl_(ttl) {}

void BlobAssembler::erase(std::map<std::string, Entry>::iterator it) {
    bytes_ -= it->second.data.size();
    entries_.erase(it);
}

std::optional<std::string> BlobAssembler::on_chunk(const BlobChunk &chunk, clock::time_point now) {
    if (chunk.blob_len > max_blob_) {
        dropped_++;
        return std::nullopt;
    }
    auto it = entries_.find(chunk.blob_id);
    if (it == entries_.end()) {
        if (chunk.offset != 0) { // a blob that starts mid-way was already discarded
                                 // (or never seen): drop
            dropped_++;
            return std::nullopt;
        }
        it = entries_.emplace(chunk.blob_id, Entry{}).first;
        it->second.len = chunk.blob_len;
        it->second.first = now;
        it->second.data.reserve(static_cast<std::size_t>(chunk.blob_len));
    }
    Entry &e = it->second;
    if (e.complete || chunk.blob_len != e.len || chunk.offset != e.data.size()) { // a gap or a changed length: the blob is discarded
        dropped_++;
        erase(it);
        return std::nullopt;
    }
    e.data.append(reinterpret_cast<const char *>(chunk.payload.data()), chunk.payload.size());
    e.last = now;
    bytes_ += chunk.payload.size();
    e.complete = e.data.size() == e.len;
    // Bounded store: evict the oldest other blobs while over the limit (docs/08:
    // 8 MiB per channel).
    while (bytes_ > limit_ && entries_.size() > 1) {
        auto oldest = entries_.end();
        for (auto j = entries_.begin(); j != entries_.end(); ++j)
            if (j != it && (oldest == entries_.end() || j->second.first < oldest->second.first)) oldest = j;
        if (oldest == entries_.end()) break;
        dropped_++;
        erase(oldest);
    }
    return e.complete ? std::optional{chunk.blob_id} : std::nullopt;
}

bool BlobAssembler::complete(const std::string &id) const {
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.complete;
}

std::optional<std::string> BlobAssembler::take(const std::string &id) {
    auto it = entries_.find(id);
    if (it == entries_.end() || !it->second.complete) return std::nullopt;
    std::string out = std::move(it->second.data);
    bytes_ -= out.size();
    entries_.erase(it);
    return out;
}

void BlobAssembler::discard(const std::string &id) {
    auto it = entries_.find(id);
    if (it != entries_.end()) erase(it);
}

void BlobAssembler::expire(clock::time_point now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now - it->second.last > ttl_) {
            auto next = std::next(it);
            dropped_++;
            erase(it);
            it = next;
        } else {
            ++it;
        }
    }
}

} // namespace fjarr::blob
