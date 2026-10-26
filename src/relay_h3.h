#pragma once

#include <functional>
#include <memory>
#include <string>

namespace uvcpp {
class uvcpp_loop;
}

namespace molink {

class relay_hub;

/// HTTP/3 relay endpoint: WebSocket-over-HTTP/3 (RFC 9220) carrying the same
/// per-session pairing as `WS /v1/relay`.
///
/// A device connects with ALPN "h3", opens an extended CONNECT with
/// `:protocol = websocket`, sends one JSON text message
/// `{"connect_id":"...","token":"..."}`, and then exchanges opaque text/binary
/// messages; the server pairs and forwards them through relay_hub.
///
/// Requires libuvcpp with the HTTP/3 WebSocket API. All methods run on the
/// device app loop; `post` is used to destroy an h3 connection after its
/// disconnect callback returns.
class relay_h3_server {
 public:
  using post_fn = std::function<void(std::function<void()>)>;

  relay_h3_server(uvcpp::uvcpp_loop* loop, relay_hub& hub, post_fn post);
  ~relay_h3_server();

  relay_h3_server(const relay_h3_server&) = delete;
  relay_h3_server& operator=(const relay_h3_server&) = delete;

  bool start(const std::string& host, int port, const std::string& cert,
             const std::string& key, std::string& error);
  void stop();

  bool listening() const;
  int port() const;

 private:
  struct impl;
  std::shared_ptr<impl> impl_;
};

}  // namespace molink
