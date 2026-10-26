#include "relay_udp.h"

#include <string>

/// Used when the libuvcpp archive statically links libuv and ships no uv.lib
/// (the current Windows release archive). The server calls uv_* directly on
/// the UDP endpoint, so linking a second libuv runtime would be unsafe;
/// instead the UDP relay endpoint is compiled out and the other relay
/// transports (WS, TCP, QUIC, HTTP/3, HTTP/2) remain available.
namespace molink {

relay_udp_server::relay_udp_server(uvcpp::uvcpp_loop*, relay_hub&) {}

relay_udp_server::~relay_udp_server() = default;

bool relay_udp_server::start(const std::string&, int, std::string& error) {
  error = "this build has no UDP relay endpoint";
  return false;
}

void relay_udp_server::stop() {}

bool relay_udp_server::listening() const { return false; }

int relay_udp_server::port() const { return 0; }

}  // namespace molink
