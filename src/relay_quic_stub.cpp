#include "relay_quic.h"

#include <string>

/// Used when libuvcpp was built without QUIC (for example the macOS CI
/// build, whose libuvcpp has no ngtcp2/nghttp3). The endpoint stays down and
/// /v1/connect/relay does not advertise it; TCP, HTTP/2, WebSocket and the
/// other relay transports remain available.
namespace molink {

struct relay_quic_server::impl {};

relay_quic_server::relay_quic_server(uvcpp::uvcpp_loop*, relay_hub&) {}

relay_quic_server::~relay_quic_server() = default;

bool relay_quic_server::start(const std::string&, int, const std::string&,
                              const std::string&, std::string& error) {
  error = "this build has no QUIC relay endpoint";
  return false;
}

void relay_quic_server::stop() {}

bool relay_quic_server::listening() const { return false; }

int relay_quic_server::port() const { return 0; }

}  // namespace molink
