#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace molink {

/// One attached relay peer, independent of the wire protocol.
///
/// WebSocket implements this directly; the TCP/QUIC relay listeners adapt a
/// stream connection to the same surface. `send_data` forwards session bytes
/// (opaque to the server), `send_notice` delivers the relay's own JSON
/// messages (ready/closed), and `close_link` ends the connection.
class relay_link {
 public:
  virtual ~relay_link() = default;
  /// `text` only matters to WebSocket links; stream links carry the same
  /// bytes and ignore it. `done` runs once the link accepted (or failed to
  /// accept) the message, for backlog accounting.
  virtual void send_data(const char* data, std::size_t len, bool text,
                         std::function<void(int)> done) = 0;
  virtual void send_notice(const std::string& json) = 0;
  virtual void close_link(const std::string& reason) = 0;
};

/// Pairs the two device connections of a signaling session and forwards data
/// between them, whatever the wire protocol on each side.
///
/// The relay is a data path, not a security boundary: it only accepts
/// connections that present the per-session token issued by
/// POST /v1/connect/relay (which authenticates the device token first), and
/// the session above it still authenticates with the verification code. The
/// payload is opaque to the server.
class relay_hub {
 public:
  struct allocation {
    std::string connect_id;
    std::string token;
  };

  /// Return (or create) this device's relay token for the session. Fails when
  /// two other devices already own the session's two sides.
  bool allocate(const std::string& connect_id, const std::string& device_id,
                allocation& out, std::string& error);

  /// Bind a link to a token. When both sides are bound, each receives
  /// {"type":"relay-ready"}.
  bool attach(relay_link* link, const std::string& connect_id,
              const std::string& token, std::string& error);

  /// Forward to the peer. Returns false when the peer has not attached yet.
  bool forward(relay_link* from, const char* data, std::size_t len, bool text);

  /// Drop a link; tells the peer and frees the session when both are gone.
  void detach(relay_link* link);

  /// Close both sides (signaling session closed/expired).
  void close_session(const std::string& connect_id);

 private:
  /// A peer whose TCP peer stops reading must not make the server buffer an
  /// unbounded stream: libuvcpp's WebSocket send queue has no limit of its
  /// own. Once a direction exceeds the backlog caps, further messages are
  /// dropped (the receiver's tile-clock repair asks for what it missed)
  /// instead of growing the process until it is OOM-killed.
  static constexpr std::uint64_t kMaxPeerBacklogBytes =
      4u * 1024u * 1024u;
  static constexpr std::uint64_t kMaxTotalBacklogBytes =
      128u * 1024u * 1024u;

  struct peer {
    std::string device_id;
    std::string token;
    relay_link* link = nullptr;
    std::uint64_t updated_ms = 0;
    /// Bytes handed to the send queue and not yet completed.
    std::shared_ptr<std::atomic<std::uint64_t>> pending;
  };
  struct session {
    std::string connect_id;
    peer sides[2];
    std::uint64_t created_ms = 0;
  };

  /// Queue one message, accounting its bytes until the send completes.
  bool send(peer& target, relay_link* link, const char* data,
            std::size_t len, bool text);
  session* find_session(const std::string& connect_id);
  peer* find_peer(session& value, const std::string& token, int& side);

  std::mutex mutex_;
  std::map<std::string, session> sessions_;
  std::map<relay_link*, std::pair<std::string, int>> bound_;
  std::shared_ptr<std::atomic<std::uint64_t>> total_pending_ =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  std::atomic<std::uint64_t> dropped_messages_{0};
  std::atomic<std::uint64_t> last_drop_log_ms_{0};
};

}  // namespace molink
