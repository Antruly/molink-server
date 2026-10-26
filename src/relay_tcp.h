#pragma once

#include <memory>
#include <string>

namespace uvcpp {
class uvcpp_loop;
}

namespace molink {

class relay_hub;

/// TCP relay endpoint: the stream sibling of `WS /v1/relay`.
///
/// A device connects (TLS when the server has a certificate), sends one JSON
/// line `{"connect_id":"...","token":"..."}`, and then exchanges opaque
/// session bytes. The first line is the same relay token issued by
/// POST /v1/connect/relay; pairing and forwarding go through the same
/// relay_hub as the WebSocket path.
///
/// All methods run on the loop that owns the listening socket.
class relay_tcp_server {
 public:
  relay_tcp_server(uvcpp::uvcpp_loop* loop, relay_hub& hub);
  ~relay_tcp_server();

  relay_tcp_server(const relay_tcp_server&) = delete;
  relay_tcp_server& operator=(const relay_tcp_server&) = delete;

  /// `cert`/`key` empty means plain TCP (a --no-tls deployment).
  bool start(const std::string& host, int port, const std::string& cert,
             const std::string& key, std::string& error);
  void stop();

  bool listening() const;
  bool secure() const;
  int port() const;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace molink
