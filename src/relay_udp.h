#pragma once

#include <memory>
#include <string>

namespace uvcpp {
class uvcpp_loop;
}

namespace molink {

class relay_hub;

/// UDP relay endpoint: the datagram sibling of `WS /v1/relay`.
///
/// A device sends one JSON datagram `{"connect_id":"...","token":"..."}` to
/// this port; the server remembers its source address and, once the other
/// side does the same, forwards every subsequent datagram opaquely between
/// the two addresses. The clients fragment/reassemble MOLK packets themselves
/// (the same 1200-byte tagged datagrams the ICE transport uses), so the server
/// never parses session data.
///
/// Datagrams are unreliable by design; a lost fragment is recovered by the
/// session's tile-clock repair, exactly like the ICE path. Pairing notices are
/// JSON datagrams (first byte '{'), which is distinct from the 0x00/0x01/0x02
/// data tags.
///
/// All methods run on the loop that owns the UDP socket.
class relay_udp_server {
 public:
  relay_udp_server(uvcpp::uvcpp_loop* loop, relay_hub& hub);
  ~relay_udp_server();

  relay_udp_server(const relay_udp_server&) = delete;
  relay_udp_server& operator=(const relay_udp_server&) = delete;

  bool start(const std::string& host, int port, std::string& error);
  void stop();

  bool listening() const;
  int port() const;

 private:
  struct impl;
  std::shared_ptr<impl> impl_;
};

}  // namespace molink
