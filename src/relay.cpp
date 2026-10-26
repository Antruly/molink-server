#include "relay.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "logging.h"
#include "util.h"

namespace molink {
namespace {

constexpr std::uint64_t kUnusedSessionTtlMs = 10 * 60 * 1000;
constexpr const char* kReadyText = "{\"type\":\"relay-ready\"}";
constexpr const char* kClosedText = "{\"type\":\"relay-closed\"}";

}  // namespace

bool relay_hub::send(peer& target, relay_link* link, const char* data,
                     std::size_t len, bool text) {
  if (link == nullptr || (data == nullptr && len > 0)) return false;
  if (!target.pending) {
    target.pending = std::make_shared<std::atomic<std::uint64_t>>(0);
  }
  // Check *before* enqueueing. A message may overshoot the cap by its own
  // size, which is bounded by the protocol's maximum message size, so memory
  // stays bounded even for one huge keyframe.
  const std::uint64_t peer_pending =
      target.pending->load(std::memory_order_relaxed);
  const std::uint64_t total_pending =
      total_pending_->load(std::memory_order_relaxed);
  if (peer_pending > kMaxPeerBacklogBytes ||
      total_pending > kMaxTotalBacklogBytes) {
    const std::uint64_t dropped = dropped_messages_.fetch_add(1) + 1;
    const std::uint64_t now = epoch_ms();
    std::uint64_t last_log = last_drop_log_ms_.load(
        std::memory_order_relaxed);
    if (now > last_log && now - last_log > 5000 &&
        last_drop_log_ms_.compare_exchange_strong(last_log, now)) {
      log_warn("relay backlog full: dropped %llu message(s); peer %llu KiB, "
               "total %llu KiB",
               static_cast<unsigned long long>(dropped),
               static_cast<unsigned long long>(peer_pending / 1024),
               static_cast<unsigned long long>(total_pending / 1024));
    }
    return false;
  }

  target.pending->fetch_add(len, std::memory_order_relaxed);
  total_pending_->fetch_add(len, std::memory_order_relaxed);
  // The completion callback may run synchronously (dead link) or much later;
  // it owns shared counters and never touches the session map, so it cannot
  // deadlock against mutex_ or race with detach().
  std::shared_ptr<std::atomic<std::uint64_t>> pending = target.pending;
  std::shared_ptr<std::atomic<std::uint64_t>> total = total_pending_;
  auto completed = [pending, total, len](int) {
    pending->fetch_sub(len, std::memory_order_relaxed);
    total->fetch_sub(len, std::memory_order_relaxed);
  };
  link->send_data(data, len, text, std::move(completed));
  return true;
}

relay_hub::session* relay_hub::find_session(const std::string& connect_id) {
  auto it = sessions_.find(connect_id);
  return it == sessions_.end() ? nullptr : &it->second;
}

relay_hub::peer* relay_hub::find_peer(session& value, const std::string& token,
                                      int& side) {
  for (int i = 0; i < 2; ++i) {
    if (!value.sides[i].token.empty() && value.sides[i].token == token) {
      side = i;
      return &value.sides[i];
    }
  }
  return nullptr;
}

bool relay_hub::allocate(const std::string& connect_id,
                         const std::string& device_id, allocation& out,
                         std::string& error) {
  if (connect_id.empty() || device_id.empty()) {
    error = "connect_id and device id are required";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const std::uint64_t now = epoch_ms();
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    const bool unused = it->second.sides[0].link == nullptr &&
                        it->second.sides[1].link == nullptr;
    if (unused && now > it->second.created_ms &&
        now - it->second.created_ms > kUnusedSessionTtlMs) {
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
  auto existing = sessions_.find(connect_id);
  if (existing == sessions_.end() && sessions_.size() >= kMaxSessions) {
    error = "relay server is busy; try again later";
    return false;
  }
  session& value = sessions_[connect_id];
  if (value.created_ms == 0) value.created_ms = now;
  value.connect_id = connect_id;

  for (int i = 0; i < 2; ++i) {
    if (value.sides[i].device_id == device_id) {
      if (!value.sides[i].pending) {
        value.sides[i].pending =
            std::make_shared<std::atomic<std::uint64_t>>(0);
      }
      out.connect_id = connect_id;
      out.token = value.sides[i].token;
      return true;
    }
  }
  for (int i = 0; i < 2; ++i) {
    if (value.sides[i].device_id.empty()) {
      value.sides[i].device_id = device_id;
      value.sides[i].token = random_hex(24);
      value.sides[i].updated_ms = now;
      value.sides[i].pending =
          std::make_shared<std::atomic<std::uint64_t>>(0);
      out.connect_id = connect_id;
      out.token = value.sides[i].token;
      return true;
    }
  }
  error = "relay session is full";
  return false;
}

bool relay_hub::attach(relay_link* link, const std::string& connect_id,
                       const std::string& token, std::string& error) {
  if (link == nullptr || connect_id.empty() || token.empty()) {
    error = "missing relay credentials";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  session* value = find_session(connect_id);
  if (value == nullptr) {
    error = "unknown relay session";
    return false;
  }
  int side = -1;
  peer* entry = find_peer(*value, token, side);
  if (entry == nullptr) {
    error = "invalid relay token";
    return false;
  }
  if (entry->link != nullptr && entry->link != link) {
    error = "relay token already in use";
    return false;
  }
  entry->link = link;
  entry->updated_ms = epoch_ms();
  bound_[link] = {connect_id, side};

  peer& other = value->sides[side == 0 ? 1 : 0];
  if (other.link != nullptr) {
    entry->link->send_notice(kReadyText);
    other.link->send_notice(kReadyText);
  }
  return true;
}

bool relay_hub::forward(relay_link* from, const char* data, std::size_t len,
                        bool text) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto bound = bound_.find(from);
  if (bound == bound_.end()) return false;
  session* value = find_session(bound->second.first);
  if (value == nullptr) return false;
  const int side = bound->second.second;
  peer& other = value->sides[side == 0 ? 1 : 0];
  if (other.link == nullptr) return false;
  other.updated_ms = epoch_ms();
  return send(other, other.link, data, len, text);
}

void relay_hub::detach(relay_link* link) {
  relay_link* other_link = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto bound = bound_.find(link);
    if (bound == bound_.end()) return;
    const std::string connect_id = bound->second.first;
    const int side = bound->second.second;
    bound_.erase(bound);
    auto it = sessions_.find(connect_id);
    if (it == sessions_.end()) return;
    session& value = it->second;
    value.sides[side].link = nullptr;
    peer& other = value.sides[side == 0 ? 1 : 0];
    other_link = other.link;
    if (other_link == nullptr) sessions_.erase(it);
  }
  if (other_link != nullptr) other_link->send_notice(kClosedText);
}

relay_hub::stats_snapshot relay_hub::stats() {
  stats_snapshot out;
  std::lock_guard<std::mutex> lock(mutex_);
  out.sessions = sessions_.size();
  out.bound = bound_.size();
  out.dropped = dropped_messages_.load(std::memory_order_relaxed);
  out.pending_bytes = total_pending_->load(std::memory_order_relaxed);
  return out;
}

void relay_hub::close_session(const std::string& connect_id) {
  relay_link* first = nullptr;
  relay_link* second = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(connect_id);
    if (it == sessions_.end()) return;
    first = it->second.sides[0].link;
    second = it->second.sides[1].link;
    if (first != nullptr) bound_.erase(first);
    if (second != nullptr) bound_.erase(second);
    sessions_.erase(it);
  }
  if (first != nullptr) {
    first->send_notice(kClosedText);
    first->close_link("relay closed");
  }
  if (second != nullptr && second != first) {
    second->send_notice(kClosedText);
    second->close_link("relay closed");
  }
}

}  // namespace molink
