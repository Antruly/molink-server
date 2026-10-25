#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace molink {

enum class signal_status {
  ok,
  not_found,
  expired,
  closed,
  forbidden,
  too_many,
  conflict,
  invalid,
};

struct signal_peer {
  std::string id;
  std::string name;
  std::string display_id;
  std::string platform;
  std::string avatar;
};

struct signal_create {
  signal_peer from;
  signal_peer to;
  std::string offer;
  std::vector<std::string> candidates;
};

struct signal_session_view {
  std::string connect_id;
  signal_peer from;
  signal_peer to;
  std::string offer;
  std::string answer;
  std::vector<std::string> from_candidates;
  std::vector<std::string> to_candidates;
  std::uint64_t created_ms = 0;
  std::uint64_t expires_ms = 0;
  bool answered = false;
  bool closed = false;
  bool expired = false;
};

/// In-memory ICE signaling relay for device-to-device connections.
///
/// Both sides authenticate as registered devices on every call. The service
/// never interprets the SDP: it stores the offer/answer and the trickled
/// candidates until one side closes the session or it expires. The actual
/// data path (libjuice/QUIC or the relay) is outside this class.
class signaling_service {
 public:
  static constexpr std::size_t kMaxSessionsPerDevice = 8;
  static constexpr std::size_t kMaxOfferBytes = 64 * 1024;
  static constexpr std::size_t kMaxAnswerBytes = 64 * 1024;
  static constexpr std::size_t kMaxCandidates = 64;
  static constexpr std::size_t kMaxCandidateBytes = 1024;
  static constexpr std::uint64_t kSessionTtlMs = 90 * 1000;

  signal_status create(const signal_create& request, std::string& connect_id,
                       std::uint64_t& expires_ms);
  signal_status pending(const std::string& to_id,
                        std::vector<signal_session_view>& out);
  signal_status answer(const std::string& from_id,
                       const std::string& connect_id,
                       const std::string& answer,
                       const std::vector<std::string>& candidates);
  signal_status add_candidates(const std::string& device_id,
                               const std::string& connect_id,
                               const std::vector<std::string>& candidates);
  signal_status find(const std::string& device_id,
                     const std::string& connect_id,
                     signal_session_view& out);
  signal_status close(const std::string& device_id,
                      const std::string& connect_id);

 private:
  /// Drops expired and long-closed sessions. Callers must hold the mutex.
  void prune(std::uint64_t now);

  struct session {
    signal_peer from;
    signal_peer to;
    std::string offer;
    std::string answer;
    std::vector<std::string> from_candidates;
    std::vector<std::string> to_candidates;
    std::uint64_t created_ms = 0;
    std::uint64_t expires_ms = 0;
    std::uint64_t updated_ms = 0;
    bool answered = false;
    bool closed = false;
  };

  static bool valid_candidates(const std::vector<std::string>& candidates);
  static void append_candidates(std::vector<std::string>& target,
                                const std::vector<std::string>& candidates);
  static signal_session_view view(const std::string& connect_id,
                                  const session& value, std::uint64_t now);
  std::size_t sessions_for(const std::string& device_id) const;

  mutable std::mutex mutex_;
  std::map<std::string, session> sessions_;
};

}  // namespace molink
