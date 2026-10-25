#include "signaling.h"

#include <algorithm>

#include "util.h"

namespace molink {
namespace {

constexpr std::uint64_t kClosedRetentionMs = 30 * 1000;

bool acceptable_offer(const std::string& value, std::size_t limit) {
  return !value.empty() && value.size() <= limit;
}

}  // namespace

std::size_t signaling_service::sessions_for(
    const std::string& device_id) const {
  const std::uint64_t now = epoch_ms();
  std::size_t count = 0;
  for (const auto& entry : sessions_) {
    const session& value = entry.second;
    if (value.closed || now >= value.expires_ms) continue;
    if (value.from.id == device_id || value.to.id == device_id) ++count;
  }
  return count;
}

bool signaling_service::valid_candidates(
    const std::vector<std::string>& candidates) {
  for (const std::string& candidate : candidates) {
    if (candidate.empty() || candidate.size() > kMaxCandidateBytes) {
      return false;
    }
  }
  return true;
}

void signaling_service::append_candidates(
    std::vector<std::string>& target,
    const std::vector<std::string>& candidates) {
  for (const std::string& candidate : candidates) {
    if (target.size() >= kMaxCandidates) break;
    if (std::find(target.begin(), target.end(), candidate) == target.end()) {
      target.push_back(candidate);
    }
  }
}

signal_status signaling_service::create(const signal_create& request,
                                        std::string& connect_id,
                                        std::uint64_t& expires_ms) {
  if (request.from.id.empty() || request.to.id.empty() ||
      request.from.id == request.to.id) {
    return signal_status::invalid;
  }
  if (!acceptable_offer(request.offer, kMaxOfferBytes) ||
      request.candidates.size() > kMaxCandidates ||
      !valid_candidates(request.candidates)) {
    return signal_status::invalid;
  }

  const std::uint64_t now = epoch_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  prune(now);
  if (sessions_for(request.from.id) >= kMaxSessionsPerDevice ||
      sessions_for(request.to.id) >= kMaxSessionsPerDevice) {
    return signal_status::too_many;
  }

  session value;
  value.from = request.from;
  value.to = request.to;
  value.offer = request.offer;
  value.from_candidates = request.candidates;
  value.created_ms = now;
  value.updated_ms = now;
  value.expires_ms = now + kSessionTtlMs;

  connect_id = random_hex(16);
  expires_ms = value.expires_ms;
  sessions_.emplace(connect_id, std::move(value));
  return signal_status::ok;
}

signal_status signaling_service::pending(
    const std::string& to_id, std::vector<signal_session_view>& out) {
  out.clear();
  if (to_id.empty()) return signal_status::invalid;
  const std::uint64_t now = epoch_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  prune(now);
  for (const auto& entry : sessions_) {
    if (entry.second.to.id != to_id || entry.second.closed) continue;
    out.push_back(view(entry.first, entry.second, now));
  }
  std::sort(out.begin(), out.end(),
            [](const signal_session_view& a, const signal_session_view& b) {
              return a.created_ms < b.created_ms;
            });
  return signal_status::ok;
}

signal_status signaling_service::answer(
    const std::string& from_id, const std::string& connect_id,
    const std::string& answer, const std::vector<std::string>& candidates) {
  if (!acceptable_offer(answer, kMaxAnswerBytes) ||
      candidates.size() > kMaxCandidates || !valid_candidates(candidates)) {
    return signal_status::invalid;
  }
  const std::uint64_t now = epoch_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  prune(now);
  const auto it = sessions_.find(connect_id);
  if (it == sessions_.end()) return signal_status::not_found;
  session& value = it->second;
  if (value.to.id != from_id) return signal_status::forbidden;
  if (value.closed) return signal_status::closed;
  if (now >= value.expires_ms) return signal_status::expired;
  if (value.answered) return signal_status::conflict;
  value.answer = answer;
  value.answered = true;
  append_candidates(value.to_candidates, candidates);
  value.updated_ms = now;
  value.expires_ms = now + kSessionTtlMs;
  return signal_status::ok;
}

signal_status signaling_service::add_candidates(
    const std::string& device_id, const std::string& connect_id,
    const std::vector<std::string>& candidates) {
  if (candidates.size() > kMaxCandidates || !valid_candidates(candidates)) {
    return signal_status::invalid;
  }
  const std::uint64_t now = epoch_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  prune(now);
  const auto it = sessions_.find(connect_id);
  if (it == sessions_.end()) return signal_status::not_found;
  session& value = it->second;
  if (value.closed) return signal_status::closed;
  if (now >= value.expires_ms) return signal_status::expired;
  if (value.from.id == device_id) {
    append_candidates(value.from_candidates, candidates);
  } else if (value.to.id == device_id) {
    append_candidates(value.to_candidates, candidates);
  } else {
    return signal_status::forbidden;
  }
  value.updated_ms = now;
  value.expires_ms = now + kSessionTtlMs;
  return signal_status::ok;
}

signal_status signaling_service::find(const std::string& device_id,
                                      const std::string& connect_id,
                                      signal_session_view& out) {
  const std::uint64_t now = epoch_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  prune(now);
  const auto it = sessions_.find(connect_id);
  if (it == sessions_.end()) return signal_status::not_found;
  if (it->second.from.id != device_id && it->second.to.id != device_id) {
    return signal_status::forbidden;
  }
  out = view(connect_id, it->second, now);
  return signal_status::ok;
}

signal_status signaling_service::close(const std::string& device_id,
                                       const std::string& connect_id) {
  const std::uint64_t now = epoch_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sessions_.find(connect_id);
  if (it == sessions_.end()) return signal_status::not_found;
  session& value = it->second;
  if (value.from.id != device_id && value.to.id != device_id) {
    return signal_status::forbidden;
  }
  value.closed = true;
  value.updated_ms = now;
  return signal_status::ok;
}

void signaling_service::prune(std::uint64_t now) {
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    const session& value = it->second;
    const bool expired = now >= value.expires_ms;
    const bool finished =
        value.closed && now > value.updated_ms &&
        now - value.updated_ms > kClosedRetentionMs;
    if (finished || (expired && now > value.expires_ms + kClosedRetentionMs)) {
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

signal_session_view signaling_service::view(const std::string& connect_id,
                                            const session& value,
                                            std::uint64_t now) {
  signal_session_view out;
  out.connect_id = connect_id;
  out.from = value.from;
  out.to = value.to;
  out.offer = value.offer;
  out.answer = value.answer;
  out.from_candidates = value.from_candidates;
  out.to_candidates = value.to_candidates;
  out.created_ms = value.created_ms;
  out.expires_ms = value.expires_ms;
  out.answered = value.answered;
  out.closed = value.closed;
  out.expired = now >= value.expires_ms;
  return out;
}

}  // namespace molink
