#pragma once

#include <memory>
#include <string>

namespace uvcpp {
class uvcpp_loop;
}

namespace molink {

class relay_hub;

/// QUIC relay endpoint: the preferred stream sibling of `WS /v1/relay`.
///
/// The device connects with ALPN "molink-relay" (separate from the desktop's
/// direct-session ALPN so the two listeners cannot be confused), opens one
/// bidirectional stream, and sends the same one-line handshake as the TCP
/// relay: `{"connect_id":"...","token":"..."}\n`. The server pairs both sides
/// through relay_hub and forwards opaque bytes over the stream.
///
/// All methods run on the loop that owns the UDP socket.
class relay_quic_server {
 public:
  relay_quic_server(uvcpp::uvcpp_loop* loop, relay_hub& hub);
  ~relay_quic_server();

  relay_quic_server(const relay_quic_server&) = delete;
  relay_quic_server& operator=(const relay_quic_server&) = delete;

  /// QUIC requires TLS; an empty certificate leaves the endpoint down.
  bool start(const std::string& host, int port, const std::string& cert,
             const std::string& key, std::string& error);
  void stop();

  bool listening() const;
  int port() const;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace molink
