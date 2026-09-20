// spec: docs/08-protocol.md#blob-frames
#include "blob_pump.hpp"

#include <algorithm>

#include <fjarr/blob.hpp>

namespace fjarr::core {

BlobPump::BlobPump(std::size_t chunk_payload) : chunk_(std::min(chunk_payload, blob::MAX_CHUNK_PAYLOAD)) {}

void BlobPump::enqueue(BlobTransfer t) { queue_.push_back(std::move(t)); }

void BlobPump::pump(ChannelSender *sender) {
    if (!sender || pumping_) return; // re-entrancy: a done() that enqueues is served by this same loop
    pumping_ = true;
    while (!queue_.empty()) {
        BlobTransfer &t = queue_.front();
        const std::uint64_t len = t.data.size();
        bool stalled = false;
        while (t.offset < len) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_, len - t.offset));
            const auto *p = reinterpret_cast<const std::byte *>(t.data.data()) + t.offset;
            const auto frame = blob::encode_chunk(t.id, t.offset, len, std::span<const std::byte>(p, n));
            if (!sender->send_binary(frame)) {
                stalled = true; // above HIGH_WATER: the drain signal calls pump() again
                break;
            }
            t.offset += n;
        }
        if (stalled) break;
        BlobTransfer finished = std::move(queue_.front());
        queue_.pop_front();
        if (finished.done) finished.done(true);
    }
    pumping_ = false;
}

bool BlobPump::cancel(std::string_view id) {
    auto it = std::find_if(queue_.begin(), queue_.end(), [id](const BlobTransfer &t) { return t.id == id; });
    if (it == queue_.end()) return false;
    BlobTransfer dropped = std::move(*it);
    queue_.erase(it);
    if (dropped.done) dropped.done(false);
    return true;
}

void BlobPump::fail_all() {
    std::deque<BlobTransfer> gone;
    gone.swap(queue_);
    for (auto &t : gone)
        if (t.done) t.done(false);
}

std::size_t BlobPump::queued_bytes() const {
    std::size_t n = 0;
    for (const auto &t : queue_) n += t.data.size() - static_cast<std::size_t>(t.offset);
    return n;
}

} // namespace fjarr::core
