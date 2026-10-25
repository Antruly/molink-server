#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <webapp/uvcpp_web_ws.h>

namespace molink {

/// Pairs the two device WebSockets of a signaling session and forwards
/// messages between them.
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

  /// Bind a WebSocket to a token. When both sides are bound, each receives
  /// {"type":"relay-ready"}.
  bool attach(uvcpp::uvcpp_ws_connection* conn, const std::string& connect_id,
              const std::string& token, std::string& error);

  /// Forward to the peer. Returns false when the peer has not attached yet.
  bool forward(uvcpp::uvcpp_ws_connection* from, const char* data,
               std::size_t len, bool text);

  /// Drop a connection; tells the peer and frees the session when both are
  /// gone.
  void detach(uvcpp::uvcpp_ws_connection* conn);

  /// Close both sides (signaling session closed/expired).
  void close_session(const std::string& connect_id);

 private:
  struct peer {
    std::string device_id;
    std::string token;
    uvcpp::uvcpp_ws_connection* conn = nullptr;
    std::uint64_t updated_ms = 0;
  };
  struct session {
    std::string connect_id;
    peer sides[2];
    std::uint64_t created_ms = 0;
  };

  static void notify(uvcpp::uvcpp_ws_connection* conn, const char* text);
  static bool send(uvcpp::uvcpp_ws_connection* conn, const char* data,
                   std::size_t len, bool text);
  session* find_session(const std::string& connect_id);
  peer* find_peer(session& value, const std::string& token, int& side);

  std::mutex mutex_;
  std::map<std::string, session> sessions_;
  std::map<uvcpp::uvcpp_ws_connection*, std::pair<std::string, int>> bound_;
};

}  // namespace molink
