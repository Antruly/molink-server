#include "relay_tcp.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <handle/uvcpp_loop.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>

#include "logging.h"
#include "relay.h"

namespace molink {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxHandshakeBytes = 4096;
constexpr std::size_t kMaxSendBacklogBytes = 8u * 1024u * 1024u;

/// One TCP connection speaking the relay stream protocol.
///
/// The connection's read/close callbacks hold the last shared_ptr, so the
/// object lives until libuvcpp releases the callbacks.
class tcp_relay_link final : public relay_link,
                             public std::enable_shared_from_this<tcp_relay_link> {
 public:
  tcp_relay_link(uvcpp::uvcpp_tcp_client* client, relay_hub* hub)
      : client_(client), hub_(hub) {}

  /// `closed_cb` runs after the relay bookkeeping, for the listener's live
  /// table. Both go in the one close slot: set_on_close is single-slot.
  void begin(std::function<void()> closed_cb) {
    auto self = shared_from_this();
    client_->read_start_events(
        [self](uvcpp::uvcpp_tcp_client&, const uvcpp::net_read_result& result) {
          self->on_read(result);
        });
    client_->set_on_close(
        [self, closed_cb = std::move(closed_cb)] {
          self->on_close();
          if (closed_cb) closed_cb();
        });
  }

  void send_data(const char* data, std::size_t len, bool /*text*/,
                 std::function<void(int)> done) override {
    if (closed_ || client_ == nullptr || (data == nullptr && len > 0)) {
      if (done) done(-1);
      return;
    }
    if (queued_ + len > kMaxSendBacklogBytes) {
      if (done) done(-1);
      fail("relay backlog exceeded");
      return;
    }
    queued_ += len;
    queue_.push_back(
        {std::string(data, len), std::move(done)});
    pump();
  }

  void send_notice(const std::string& text) override {
    std::string line = text;
    line.push_back('\n');
    send_data(line.data(), line.size(), true, nullptr);
  }

  void close_link(const std::string& reason) override {
    if (closed_) return;
    closed_ = true;
    fail_pending();
    if (client_ != nullptr) {
      uvcpp::uvcpp_tcp_client* client = client_;
      client_ = nullptr;
      client->close();
    }
    (void)reason;
  }

 private:
  struct outbound {
    std::string bytes;
    std::function<void(int)> done;
  };

  void on_read(const uvcpp::net_read_result& result) {
    if (closed_ || result.event != uvcpp::net_read_event::DATA) return;
    if (attached_) {
      if (hub_ != nullptr) {
        hub_->forward(this, result.data, result.size, false);
      }
      return;
    }
    handshake_.append(result.data, result.size);
    if (handshake_.size() > kMaxHandshakeBytes) {
      fail("relay handshake too large");
      return;
    }
    const std::size_t newline = handshake_.find('\n');
    if (newline == std::string::npos) return;
    const std::string line = handshake_.substr(0, newline);
    const std::string rest = handshake_.substr(newline + 1);
    handshake_.clear();
    if (!authenticate(line)) return;
    if (!rest.empty()) hub_->forward(this, rest.data(), rest.size(), false);
  }

  bool authenticate(const std::string& line) {
    json request;
    try {
      request = json::parse(line);
    } catch (const std::exception&) {
      fail("relay handshake is not JSON");
      return false;
    }
    if (!request.is_object()) {
      fail("relay handshake is not an object");
      return false;
    }
    const std::string connect_id = request.value("connect_id", std::string());
    const std::string token = request.value("token", std::string());
    if (connect_id.empty() || token.empty()) {
      fail("relay handshake needs connect_id and token");
      return false;
    }
    std::string error;
    if (hub_ == nullptr || !hub_->attach(this, connect_id, token, error)) {
      fail(error.empty() ? "relay attach failed" : error);
      return false;
    }
    attached_ = true;
    return true;
  }

  void fail(const std::string& message) {
    if (closed_) return;
    closed_ = true;
    json body;
    body["type"] = "relay-error";
    body["message"] = message;
    std::string line = body.dump();
    line.push_back('\n');
    if (client_ != nullptr && !attached_) {
      // Best effort; the close below may race the write, which is harmless.
      client_->write(line.data(), line.size(), [](int) {});
    }
    fail_pending();
    if (client_ != nullptr) {
      uvcpp::uvcpp_tcp_client* client = client_;
      client_ = nullptr;
      client->close();
    }
  }

  void on_close() {
    if (hub_ != nullptr && attached_) {
      hub_->detach(this);
    }
    attached_ = false;
    closed_ = true;
    client_ = nullptr;
    fail_pending();
  }

  void fail_pending() {
    std::deque<outbound> pending;
    pending.swap(queue_);
    queued_ = 0;
    for (outbound& entry : pending) {
      if (entry.done) entry.done(-1);
    }
  }

  void pump() {
    if (writing_ || queue_.empty() || closed_ || client_ == nullptr) return;
    writing_ = true;
    outbound entry = std::move(queue_.front());
    queue_.pop_front();
    const std::size_t size = entry.bytes.size();
    auto bytes = std::make_shared<std::string>(std::move(entry.bytes));
    auto done = std::move(entry.done);
    const int status = client_->write(
        bytes->data(), bytes->size(), [this, bytes, size, done](int write_status) {
          writing_ = false;
          queued_ -= size;
          if (done) done(write_status);
          if (write_status != 0) {
            fail("relay write failed");
            return;
          }
          pump();
        });
    if (status != 0) {
      writing_ = false;
      queued_ -= size;
      if (done) done(status);
      fail("relay write failed");
    }
  }

  uvcpp::uvcpp_tcp_client* client_;
  relay_hub* hub_;
  std::string handshake_;
  std::deque<outbound> queue_;
  std::size_t queued_ = 0;
  bool writing_ = false;
  bool attached_ = false;
  bool closed_ = false;
};

}  // namespace

struct relay_tcp_server::impl {
  impl(uvcpp::uvcpp_loop* loop, relay_hub& hub) : hub(hub), loop(loop) {}

  std::unique_ptr<uvcpp::uvcpp_ssl_context> tls;
  std::unique_ptr<uvcpp::uvcpp_tcp_server> server;
  relay_hub& hub;
  uvcpp::uvcpp_loop* loop;
  std::map<uvcpp::uvcpp_tcp_client*, std::weak_ptr<tcp_relay_link>> live;
  bool listening = false;
  bool secure = false;
  int port = 0;
};

relay_tcp_server::relay_tcp_server(uvcpp::uvcpp_loop* loop, relay_hub& hub)
    : impl_(std::make_unique<impl>(loop, hub)) {}

relay_tcp_server::~relay_tcp_server() { stop(); }

bool relay_tcp_server::start(const std::string& host, int port,
                             const std::string& cert, const std::string& key,
                             std::string& error) {
  if (impl_->listening) return true;
  if (!cert.empty()) {
    impl_->tls =
        std::make_unique<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::SERVER);
    if (!impl_->tls->load_certificate_file(cert)) {
      error = "relay TCP: could not load the certificate";
      return false;
    }
    if (!impl_->tls->load_private_key_file(key)) {
      error = "relay TCP: could not load the private key";
      return false;
    }
  }

  impl_->server = std::make_unique<uvcpp::uvcpp_tcp_server>(impl_->loop);
  if (impl_->tls) impl_->server->set_ssl_context(impl_->tls.get());
  const int bind_status = impl_->server->bind(host.c_str(), port);
  if (bind_status != 0) {
    error = "relay TCP: cannot bind " + host + ":" + std::to_string(port) +
            " (" + std::to_string(bind_status) + ")";
    impl_->server.reset();
    return false;
  }

  impl* self = impl_.get();
  const int listen_status = impl_->server->listen(
      [self](uvcpp::uvcpp_tcp_client* client) {
        if (client == nullptr) return;
        auto link = std::make_shared<tcp_relay_link>(client, &self->hub);
        self->live[client] = link;
        // Erase from live when the connection closes; capture impl by raw
        // pointer because the server owns the callbacks and impl outlives.
        link->begin([self, client] { self->live.erase(client); });
      });
  if (listen_status != 0) {
    error = "relay TCP: cannot listen on " + host + ":" +
            std::to_string(port) + " (" + std::to_string(listen_status) + ")";
    impl_->server.reset();
    return false;
  }
  impl_->listening = true;
  impl_->secure = impl_->tls != nullptr;
  impl_->port = port;
  return true;
}

void relay_tcp_server::stop() {
  if (!impl_ || !impl_->server) return;
  impl_->listening = false;
  std::vector<uvcpp::uvcpp_tcp_client*> clients;
  for (auto& entry : impl_->live) clients.push_back(entry.first);
  impl_->live.clear();
  for (uvcpp::uvcpp_tcp_client* client : clients) client->close();
  impl_->server->stop();
  impl_->server.reset();
  impl_->tls.reset();
  impl_->port = 0;
  impl_->secure = false;
}

bool relay_tcp_server::listening() const {
  return impl_ && impl_->listening;
}

bool relay_tcp_server::secure() const { return impl_ && impl_->secure; }

int relay_tcp_server::port() const { return impl_ ? impl_->port : 0; }

}  // namespace molink
