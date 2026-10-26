#include "relay_quic.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>

#include <handle/uvcpp_loop.h>
#include <net/uvcpp_net_read.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uvcpp/uvcpp_config.h>

#include "logging.h"
#include "relay.h"

namespace molink {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxHandshakeBytes = 4096;
constexpr std::size_t kMaxSendBacklogBytes = 8u * 1024u * 1024u;

/// One QUIC stream speaking the relay stream protocol.
class quic_relay_link final
    : public relay_link,
      public std::enable_shared_from_this<quic_relay_link> {
 public:
  quic_relay_link(uvcpp::uvcpp_quic_connection* connection, int64_t stream_id,
                  relay_hub* hub)
      : connection_(connection), stream_id_(stream_id), hub_(hub) {}

  void send_data(const char* data, std::size_t len, bool /*text*/,
                 std::function<void(int)> done) override {
    if (closed_ || connection_ == nullptr || (data == nullptr && len > 0)) {
      if (done) done(-1);
      return;
    }
    if (queued_ + len > kMaxSendBacklogBytes) {
      if (done) done(-1);
      fail("relay backlog exceeded");
      return;
    }
    const int status = connection_->write_stream(stream_id_, data, len, false);
    if (status != 0) {
      if (done) done(status);
      fail("relay write failed");
      return;
    }
    queued_ += len;
    writes_.push_back({len, std::move(done)});
  }

  void send_notice(const std::string& text) override {
    std::string line = text;
    line.push_back('\n');
    send_data(line.data(), line.size(), true, nullptr);
  }

  void close_link(const std::string& reason) override {
    (void)reason;
    if (closed_) return;
    closed_ = true;
    fail_pending();
    if (connection_ != nullptr) connection_->close();
  }

  void on_read(const uvcpp::net_read_result& result) {
    if (closed_ || result.event != uvcpp::net_read_event::DATA) return;
    if (attached_) {
      if (hub_ != nullptr) hub_->forward(this, result.data, result.size, false);
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

  void on_write(int status) {
    if (writes_.empty()) return;
    pending write = std::move(writes_.front());
    writes_.pop_front();
    queued_ -= write.size;
    if (write.done) write.done(status);
    if (status != 0) {
      fail("relay write failed");
      return;
    }
  }

  void on_close() {
    if (hub_ != nullptr && attached_) hub_->detach(this);
    attached_ = false;
    closed_ = true;
    connection_ = nullptr;
    fail_pending();
  }

  int64_t stream_id() const { return stream_id_; }

 private:
  struct pending {
    std::size_t size = 0;
    std::function<void(int)> done;
  };

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
    if (!attached_ && connection_ != nullptr) {
      json body;
      body["type"] = "relay-error";
      body["message"] = message;
      std::string line = body.dump();
      line.push_back('\n');
      connection_->write_stream(stream_id_, line.data(), line.size(), false);
    }
    fail_pending();
    if (connection_ != nullptr) connection_->close();
  }

  void fail_pending() {
    std::deque<pending> writes;
    writes.swap(writes_);
    queued_ = 0;
    for (pending& write : writes) {
      if (write.done) write.done(-1);
    }
  }

  uvcpp::uvcpp_quic_connection* connection_;
  int64_t stream_id_;
  relay_hub* hub_;
  std::string handshake_;
  std::deque<pending> writes_;
  std::size_t queued_ = 0;
  bool attached_ = false;
  bool closed_ = false;
};

/// Routes the connection-wide callbacks to the one session stream.
struct connection_state {
  std::shared_ptr<quic_relay_link> link;
};

}  // namespace

struct relay_quic_server::impl {
  impl(uvcpp::uvcpp_loop* loop, relay_hub& hub) : hub(hub), loop(loop) {}

  std::unique_ptr<uvcpp::uvcpp_ssl_context> tls;
  std::unique_ptr<uvcpp::uvcpp_quic_server> server;
  relay_hub& hub;
  uvcpp::uvcpp_loop* loop;
  bool listening = false;
  int port = 0;
};

relay_quic_server::relay_quic_server(uvcpp::uvcpp_loop* loop, relay_hub& hub)
    : impl_(std::make_unique<impl>(loop, hub)) {}

relay_quic_server::~relay_quic_server() { stop(); }

bool relay_quic_server::start(const std::string& host, int port,
                              const std::string& cert, const std::string& key,
                              std::string& error) {
#if !UVCPP_QUIC_ENABLE
  (void)host;
  (void)port;
  (void)cert;
  (void)key;
  error = "this libuvcpp build has no QUIC";
  return false;
#else
  if (impl_->listening) return true;
  if (cert.empty()) {
    error = "QUIC relay needs TLS (no device certificate)";
    return false;
  }
  impl_->tls =
      std::make_unique<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::SERVER);
  if (!impl_->tls->load_certificate_file(cert)) {
    error = "relay QUIC: could not load the certificate";
    return false;
  }
  if (!impl_->tls->load_private_key_file(key)) {
    error = "relay QUIC: could not load the private key";
    return false;
  }
  impl_->tls->set_alpn_select_protos({"molink-relay"});

  impl_->server = std::make_unique<uvcpp::uvcpp_quic_server>(impl_->loop);
  impl_->server->set_ssl_context(impl_->tls.get());
  impl_->server->set_alpn_select_protos({"molink-relay"});
  const int bind_status = impl_->server->bind(host.c_str(), port);
  if (bind_status != 0) {
    error = "relay QUIC: cannot register " + host + ":" +
            std::to_string(port) + " (" + std::to_string(bind_status) + ")";
    impl_->server.reset();
    return false;
  }

  impl* self = impl_.get();
  const int listen_status = impl_->server->listen(
      [self](uvcpp::uvcpp_quic_connection* connection) {
        if (connection == nullptr) return;
        auto state = std::make_shared<connection_state>();
        uvcpp::uvcpp_quic_connection::callbacks callbacks;
        callbacks.on_stream_open = [self, state](
                                       uvcpp::uvcpp_quic_connection& conn,
                                       int64_t stream_id) {
          if (state->link) {
            conn.shutdown_stream_read(stream_id, 0);
            return;
          }
          auto link = std::make_shared<quic_relay_link>(&conn, stream_id,
                                                        &self->hub);
          state->link = link;
        };
        callbacks.on_read = [state](uvcpp::uvcpp_quic_connection&,
                                    int64_t stream_id,
                                    const uvcpp::net_read_result& result) {
          if (!state->link) return;
          if (stream_id != state->link->stream_id()) return;
          state->link->on_read(result);
        };
        callbacks.on_write = [state](uvcpp::uvcpp_quic_connection&,
                                     int64_t stream_id, int status) {
          if (!state->link) return;
          if (stream_id != state->link->stream_id()) return;
          state->link->on_write(status);
        };
        callbacks.on_close = [state](uvcpp::uvcpp_quic_connection&, int) {
          if (state->link) state->link->on_close();
        };
        connection->set_callbacks(callbacks);
      });
  if (listen_status != 0) {
    error = "relay QUIC: cannot listen on " + host + ":" +
            std::to_string(port) + " (" + std::to_string(listen_status) + ")";
    impl_->server.reset();
    return false;
  }
  impl_->listening = true;
  impl_->port = impl_->server->configured_port();
  return true;
#endif
}

void relay_quic_server::stop() {
#if UVCPP_QUIC_ENABLE
  if (!impl_ || !impl_->server) return;
  impl_->listening = false;
  impl_->server->stop();
  impl_->server.reset();
  impl_->tls.reset();
  impl_->port = 0;
#endif
}

bool relay_quic_server::listening() const {
#if UVCPP_QUIC_ENABLE
  return impl_ && impl_->listening;
#else
  return false;
#endif
}

int relay_quic_server::port() const {
#if UVCPP_QUIC_ENABLE
  return impl_ ? impl_->port : 0;
#else
  return 0;
#endif
}

}  // namespace molink
