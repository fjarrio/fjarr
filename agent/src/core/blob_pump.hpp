#pragma once
// BlobPump — the one outbound blob pump: chunks a queued blob onto a bulk
// ChannelSender under the docs/08 watermarks, resumes on drain, reports
// completion. One per open bulk channel of a session. spec:
// docs/08-protocol.md#blob-frames ·
// docs/23-agent-core-architecture.md#datachannel-router
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

#include <fjarr/capability.hpp>

namespace fjarr::core {

struct BlobTransfer {
    std::string id;
    std::string data;
    std::uint64_t offset = 0;
    std::function<void(bool ok)> done;
};

class BlobPump {
  public:
    /// `chunk_payload` = the SCTP message limit minus the blob header (docs/08:
    /// also ≤ 256 KiB).
    explicit BlobPump(std::size_t chunk_payload);
    void enqueue(BlobTransfer t);
    /// Send what the sender takes; stops at the first refused chunk (above
    /// HIGH_WATER) and continues on the next call (the channel's drain signal). A
    /// null sender means the channel is not open yet: nothing is sent, nothing is
    /// lost.
    void pump(ChannelSender *sender);
    /// Drop a transfer (queued or in progress); done(false). Returns false if
    /// unknown.
    bool cancel(std::string_view id);
    /// done(false) for everything (session end).
    void fail_all();
    std::size_t queued_bytes() const;
    std::size_t size() const { return queue_.size(); }

  private:
    std::size_t chunk_;
    std::deque<BlobTransfer> queue_;
    bool pumping_ = false;
};

} // namespace fjarr::core
