#include "relay_udp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <handle/uvcpp_udp.h>
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_define.h>

#include "logging.h"
#include "relay.h"
#include "util.h"

namespace molink {
namespace {

using nlohmann::json;

/// Drop an unattached/idle mapping after this long; UDP has no FIN to tell us
/// a device disappeared.
constexpr std::uint64_t kPeerIdleTtlMs = 5 * 60 * 1000;
constexpr std::uint64_t kSweepIntervalMs = 30 * 1000;

std::string notice(const char* type, const std::string& message) {
  json body;
  body["type"] = type;
  if (!message.empty()) body["message"] = message;
  return body.dump();
}

std::string endpoint_key(const char* ip, int port) {
  return std::string(ip != nullptr ? ip : "") + ":" + std::to_string(port);
}

}  // namespace

struct relay_udp_server::impl {
  struct peer_link;

  impl(uvcpp::uvcpp_loop* loop, relay_hub& hub) : loop(loop), hub(hub) {}

  /// Queue one datagram; the completion runs `done` (may be null).
  int send_datagram(const struct sockaddr* destination, const char* data,
                    std::size_t len, std::function<void(int)> done);

  uvcpp::uvcpp_loop* loop;
  relay_hub& hub;
  std::unique_ptr<uvcpp::uvcpp_udp> udp;
  std::unique_ptr<uvcpp::uvcpp_timer> sweep;
  std::map<std::string, std::shared_ptr<peer_link>> peers;
  bool listening = false;
  int port = 0;

  struct send_payload {
    std::vector<uint8_t> data;
    uv_buf_t buffer{};
    uvcpp::uvcpp_udp_send* request = nullptr;
    std::function<void(int)> done;
  };

  struct peer_link final : relay_link,
                           std::enable_shared_from_this<peer_link> {
    peer_link(impl* owner, std::string key, struct sockaddr_storage address,
              int address_len)
        : owner(owner),
          key(std::move(key)),
          address(address),
          address_len(address_len) {}

    void send_data(const char* data, std::size_t len, bool /*text*/,
                   std::function<void(int)> done) override {
      if (closed || owner == nullptr) {
        if (done) done(-1);
        return;
      }
      const int status = owner->send_datagram(
          reinterpret_cast<const struct sockaddr*>(&address), data, len,
          std::move(done));
      if (status != 0 && done) done(status);
    }

    void send_notice(const std::string& text) override {
      send_data(text.data(), text.size(), true, nullptr);
    }

    void close_link(const std::string& reason) override {
      (void)reason;
      if (closed) return;
      closed = true;
      if (attached && owner != nullptr) owner->hub.detach(this);
      attached = false;
    }

    impl* owner;
    std::string key;
    struct sockaddr_storage address;
    int address_len = 0;
    bool attached = false;
    bool closed = false;
    std::uint64_t last_seen_ms = 0;
  };
};

int relay_udp_server::impl::send_datagram(
    const struct sockaddr* destination, const char* data, std::size_t len,
    std::function<void(int)> done) {
  if (udp == nullptr || destination == nullptr ||
      (data == nullptr && len > 0)) {
    return -1;
  }
  auto* payload = new send_payload();
  payload->data.assign(reinterpret_cast<const uint8_t*>(data),
                       reinterpret_cast<const uint8_t*>(data) + len);
  payload->buffer.base = reinterpret_cast<char*>(payload->data.data());
  payload->buffer.len = payload->data.size();
  payload->done = std::move(done);
  auto* request = new uvcpp::uvcpp_udp_send();
  request->init();
  request->set_uv_buf(&payload->buffer, false);
  request->set_data(payload);
  payload->request = request;
  const int rc = udp->send(
      request, &payload->buffer, 1, destination,
      [](uvcpp::uvcpp_udp_send* req, int status) {
        auto* payload = static_cast<send_payload*>(req->get_data());
        if (payload != nullptr) {
          if (payload->done) payload->done(status);
          delete payload;
        }
        delete req;
      });
  if (rc != 0) {
    delete payload;
    delete request;
    return rc;
  }
  return 0;
}

relay_udp_server::relay_udp_server(uvcpp::uvcpp_loop* loop, relay_hub& hub)
    : impl_(std::make_shared<impl>(loop, hub)) {}

relay_udp_server::~relay_udp_server() { stop(); }

bool relay_udp_server::start(const std::string& host, int port,
                             std::string& error) {
  if (impl_->listening) return true;
  impl_->udp = std::make_unique<uvcpp::uvcpp_udp>(impl_->loop);

  struct sockaddr_in bind_address;
  std::memset(&bind_address, 0, sizeof(bind_address));
  const char* bind_host = host.empty() ? "0.0.0.0" : host.c_str();
  if (uv_ip4_addr(bind_host, port, &bind_address) != 0) {
    error = "relay UDP: invalid bind address " + host;
    impl_->udp.reset();
    return false;
  }
  if (impl_->udp->bind(reinterpret_cast<const struct sockaddr*>(&bind_address),
                       0) != 0) {
    error = std::string("relay UDP: cannot bind ") + bind_host + ":" +
            std::to_string(port);
    impl_->udp.reset();
    return false;
  }
  // Forwarded keyframes arrive as a burst of datagrams; give the socket room
  // to queue them instead of dropping the tail.
  int buffer_size = 4 * 1024 * 1024;
  uv_recv_buffer_size(
      reinterpret_cast<uv_handle_t*>(impl_->udp->get_handle()), &buffer_size);
  buffer_size = 4 * 1024 * 1024;
  uv_send_buffer_size(
      reinterpret_cast<uv_handle_t*>(impl_->udp->get_handle()), &buffer_size);

  struct sockaddr_storage bound;
  std::memset(&bound, 0, sizeof(bound));
  int bound_len = sizeof(bound);
  int actual_port = port;
  if (impl_->udp->getsockname(reinterpret_cast<struct sockaddr*>(&bound),
                              &bound_len) == 0 &&
      bound.ss_family == AF_INET) {
    actual_port = ntohs(reinterpret_cast<struct sockaddr_in*>(&bound)->sin_port);
  }

  std::shared_ptr<impl> self = impl_;
  if (impl_->udp->recv_start(
          [](uvcpp::uvcpp_handle*, std::size_t size, uv_buf_t* buf) {
            uvcpp::uvcpp_buf::alloc_buf(buf, size > 0 ? size : 2048);
          },
          [self](uvcpp::uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
                 const struct sockaddr* addr, unsigned int) {
            if (nread > 0 && buf->base != nullptr && addr != nullptr) {
              const char* data = buf->base;
              const std::size_t len = static_cast<std::size_t>(nread);
              char ip[INET6_ADDRSTRLEN] = {};
              int peer_port = 0;
              int address_len = 0;
              if (addr->sa_family == AF_INET) {
                auto* v4 = reinterpret_cast<const struct sockaddr_in*>(addr);
                uv_ip4_name(v4, ip, sizeof(ip));
                peer_port = ntohs(v4->sin_port);
                address_len = sizeof(struct sockaddr_in);
              } else if (addr->sa_family == AF_INET6) {
                auto* v6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
                uv_ip6_name(v6, ip, sizeof(ip));
                peer_port = ntohs(v6->sin6_port);
                address_len = sizeof(struct sockaddr_in6);
              } else {
                uvcpp::uvcpp_free_bytes(buf->base);
                return;
              }
              const std::string key = endpoint_key(ip, peer_port);
              const std::uint64_t now = epoch_ms();
              auto found = self->peers.find(key);
              if (found != self->peers.end()) {
                found->second->last_seen_ms = now;
                if (found->second->attached) {
                  self->hub.forward(found->second.get(), data, len, false);
                }
              } else {
                // First datagram from this address: the JSON handshake.
                json request;
                bool parsed = true;
                try {
                  request = json::parse(std::string(data, len));
                } catch (const std::exception&) {
                  parsed = false;
                }
                if (parsed && request.is_object()) {
                  const std::string connect_id =
                      request.value("connect_id", std::string());
                  const std::string token =
                      request.value("token", std::string());
                  if (!connect_id.empty() && !token.empty()) {
                    struct sockaddr_storage stored;
                    std::memset(&stored, 0, sizeof(stored));
                    std::memcpy(&stored, addr,
                                static_cast<std::size_t>(address_len));
                    auto link = std::make_shared<impl::peer_link>(
                        self.get(), key, stored, address_len);
                    link->last_seen_ms = now;
                    std::string attach_error;
                    if (!self->hub.attach(link.get(), connect_id, token,
                                          attach_error)) {
                      const std::string reply =
                          notice("relay-error", attach_error);
                      self->send_datagram(addr, reply.data(), reply.size(),
                                          nullptr);
                    } else {
                      link->attached = true;
                      self->peers[key] = link;
                    }
                  }
                }
              }
            }
            if (buf->base != nullptr) uvcpp::uvcpp_free_bytes(buf->base);
          }) != 0) {
    error = std::string("relay UDP: cannot receive on ") + bind_host + ":" +
            std::to_string(port);
    impl_->udp.reset();
    return false;
  }

  impl_->sweep = std::make_unique<uvcpp::uvcpp_timer>(impl_->loop);
  impl_->sweep->init();
  impl_->sweep->start(
      [self](uvcpp::uvcpp_timer*) {
        const std::uint64_t now = epoch_ms();
        for (auto it = self->peers.begin(); it != self->peers.end();) {
          if (now > it->second->last_seen_ms &&
              now - it->second->last_seen_ms > kPeerIdleTtlMs) {
            auto link = it->second;
            it = self->peers.erase(it);
            link->close_link("relay peer idle");
            link->closed = true;
          } else {
            ++it;
          }
        }
      },
      kSweepIntervalMs, kSweepIntervalMs);

  impl_->listening = true;
  impl_->port = actual_port;
  return true;
}

void relay_udp_server::stop() {
  if (!impl_ || !impl_->udp) return;
  impl_->listening = false;
  if (impl_->sweep) {
    impl_->sweep->stop();
    impl_->sweep.reset();
  }
  for (auto& entry : impl_->peers) {
    entry.second->close_link("relay stopped");
    entry.second->closed = true;
  }
  impl_->peers.clear();
  impl_->udp->recv_stop();
  impl_->udp.reset();
  impl_->port = 0;
}

bool relay_udp_server::listening() const {
  return impl_ && impl_->listening;
}

int relay_udp_server::port() const { return impl_ ? impl_->port : 0; }

}  // namespace molink
