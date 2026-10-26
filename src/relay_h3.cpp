#include "relay_h3.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <uvcpp/uvcpp_config.h>

#ifndef MOLINK_SERVER_HAVE_H3_WS
#define MOLINK_SERVER_HAVE_H3_WS 0
#endif

#if MOLINK_SERVER_HAVE_H3_WS
#include <http3/uvcpp_h3_common.h>
#include <http3/uvcpp_h3_connection.h>
#include <http3/uvcpp_h3_ws_connection.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_ws_frame.h>

#include "handle/uvcpp_loop.h"
#include "relay.h"
#endif

namespace molink {

#if MOLINK_SERVER_HAVE_H3_WS
using nlohmann::json;
#endif

struct relay_h3_server::impl {
#if MOLINK_SERVER_HAVE_H3_WS
  struct conn_state;

  /// One accepted WebSocket-over-H3 connection. The h3 and WebSocket objects
  /// are owned by conn_state; this link only borrows the WebSocket.
  struct h3_relay_link final : relay_link,
                               std::enable_shared_from_this<h3_relay_link> {
    explicit h3_relay_link(relay_hub* hub) : hub(hub) {}

    void attach(uvcpp::uvcpp_h3_ws_connection* socket) { ws = socket; }

    void send_data(const char* data, std::size_t len, bool text,
                   std::function<void(int)> done) override {
      if (closed || ws == nullptr) {
        if (done) done(-1);
        return;
      }
      const int status =
          text ? ws->send_text(data, len) : ws->send_binary(data, len);
      if (done) done(status == 0 ? 0 : status);
    }

    void send_notice(const std::string& text) override {
      send_data(text.data(), text.size(), true, nullptr);
    }

    void close_link(const std::string& reason) override {
      (void)reason;
      if (closed) return;
      if (ws != nullptr) ws->close(uvcpp::ws_close_code::NORMAL, reason);
    }

    void on_text(const std::string& text) {
      if (closed) return;
      if (!attached) {
        authenticate(text);
        return;
      }
      if (hub != nullptr) {
        hub->forward(this, text.data(), text.size(), true);
      }
    }

    void on_binary(const uint8_t* data, std::size_t len) {
      if (closed || !attached || hub == nullptr) return;
      hub->forward(this, reinterpret_cast<const char*>(data), len, false);
    }

    void on_socket_closed() {
      if (hub != nullptr && attached) hub->detach(this);
      attached = false;
      closed = true;
      ws = nullptr;
    }

   private:
    void authenticate(const std::string& text) {
      json request;
      try {
        request = json::parse(text);
      } catch (const std::exception&) {
        fail("relay handshake is not JSON");
        return;
      }
      if (!request.is_object()) {
        fail("relay handshake is not an object");
        return;
      }
      const std::string connect_id = request.value("connect_id", "");
      const std::string token = request.value("token", "");
      if (connect_id.empty() || token.empty()) {
        fail("relay handshake needs connect_id and token");
        return;
      }
      std::string error;
      if (hub == nullptr || !hub->attach(this, connect_id, token, error)) {
        fail(error.empty() ? "relay attach failed" : error);
        return;
      }
      attached = true;
    }

    void fail(const std::string& message) {
      if (closed) return;
      closed = true;
      if (ws != nullptr) {
        json body;
        body["type"] = "relay-error";
        body["message"] = message;
        const std::string reply = body.dump();
        ws->send_text(reply.data(), reply.size());
        ws->close(uvcpp::ws_close_code::POLICY_VIOLATION, message);
      }
    }

    relay_hub* hub;
    uvcpp::uvcpp_h3_ws_connection* ws = nullptr;
    bool attached = false;
    bool closed = false;
  };

  struct conn_state {
    std::unique_ptr<uvcpp::uvcpp_h3_connection> h3;
    std::unique_ptr<uvcpp::uvcpp_h3_ws_connection> ws;
    std::shared_ptr<h3_relay_link> link;
    bool closed = false;
  };

  impl(uvcpp::uvcpp_loop* loop, relay_hub& hub, post_fn post)
      : loop(loop), hub(hub), post(std::move(post)) {}

  uvcpp::uvcpp_loop* loop;
  relay_hub& hub;
  post_fn post;
  std::unique_ptr<uvcpp::uvcpp_ssl_context> tls;
  std::unique_ptr<uvcpp::uvcpp_quic_server> server;
  std::map<uvcpp::uvcpp_quic_connection*, std::shared_ptr<conn_state>>
      connections;
  bool listening = false;
  int port = 0;
#else
  impl(uvcpp::uvcpp_loop*, relay_hub&, post_fn) {}
#endif
};

relay_h3_server::relay_h3_server(uvcpp::uvcpp_loop* loop, relay_hub& hub,
                                 post_fn post)
    : impl_(std::make_shared<impl>(loop, hub, std::move(post))) {}

relay_h3_server::~relay_h3_server() { stop(); }

bool relay_h3_server::start(const std::string& host, int port,
                            const std::string& cert, const std::string& key,
                            std::string& error) {
#if !MOLINK_SERVER_HAVE_H3_WS
  (void)host;
  (void)port;
  (void)cert;
  (void)key;
  error = "libuvcpp was built without the HTTP/3 WebSocket API";
  return false;
#else
  if (impl_->listening) return true;
  if (cert.empty()) {
    error = "HTTP/3 relay needs TLS (no device certificate)";
    return false;
  }
  impl_->tls =
      std::make_unique<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::SERVER);
  if (!impl_->tls->load_certificate_file(cert)) {
    error = "relay HTTP/3: could not load the certificate";
    return false;
  }
  if (!impl_->tls->load_private_key_file(key)) {
    error = "relay HTTP/3: could not load the private key";
    return false;
  }
  impl_->tls->set_alpn_select_protos({"h3"});

  impl_->server = std::make_unique<uvcpp::uvcpp_quic_server>(impl_->loop);
  impl_->server->set_ssl_context(impl_->tls.get());
  impl_->server->set_alpn_select_protos({"h3"});
  const int bind_status = impl_->server->bind(host.c_str(), port);
  if (bind_status != 0) {
    error = "relay HTTP/3: cannot register " + host + ":" +
            std::to_string(port) + " (" + std::to_string(bind_status) + ")";
    impl_->server.reset();
    return false;
  }

  std::shared_ptr<impl> self = impl_;
  const int listen_status = impl_->server->listen(
      [self](uvcpp::uvcpp_quic_connection* connection) {
        if (connection == nullptr) return;
        auto state = std::make_shared<impl::conn_state>();
        state->h3 =
            std::make_unique<uvcpp::uvcpp_h3_connection>(connection, true);
        self->connections[connection] = state;

        std::weak_ptr<impl> weak_self = self;
        std::weak_ptr<impl::conn_state> weak_state = state;
        uvcpp::uvcpp_h3_connection::callbacks callbacks;
        callbacks.on_connect_request =
            [weak_self, weak_state](uvcpp::uvcpp_h3_connection& h3,
                                    const uvcpp::h3_request& request) {
              auto state = weak_state.lock();
              auto owner = weak_self.lock();
              if (!state || !owner || state->closed) return;
              if (request.protocol != "websocket") return;
              auto link =
                  std::make_shared<impl::h3_relay_link>(&owner->hub);
              std::weak_ptr<impl::h3_relay_link> weak_link = link;
              uvcpp::uvcpp_h3_ws_connection::callbacks ws_callbacks;
              ws_callbacks.on_text = [weak_link](const std::string& text) {
                if (auto alive = weak_link.lock()) alive->on_text(text);
              };
              ws_callbacks.on_binary = [weak_link](const uint8_t* data,
                                                   std::size_t len) {
                if (auto alive = weak_link.lock()) {
                  alive->on_binary(data, len);
                }
              };
              ws_callbacks.on_close = [weak_link](uvcpp::ws_close_code,
                                                  const std::string&) {
                if (auto alive = weak_link.lock()) alive->on_socket_closed();
              };
              ws_callbacks.on_error = [weak_link](int,
                                                  const std::string&) {
                if (auto alive = weak_link.lock()) alive->on_socket_closed();
              };
              auto ws = uvcpp::uvcpp_h3_ws_connection::accept(h3, request,
                                                              ws_callbacks);
              if (!ws) return;
              state->ws = std::move(ws);
              state->link = link;
              link->attach(state->ws.get());
            };
        callbacks.on_disconnect = [weak_self, weak_state, connection](
                                      uvcpp::uvcpp_h3_connection&) {
          auto state = weak_state.lock();
          if (state) {
            state->closed = true;
            if (state->link) state->link->on_socket_closed();
            state->ws.reset();
          }
          auto owner = weak_self.lock();
          if (owner && owner->post) {
            owner->post([weak_self, connection] {
              if (auto alive = weak_self.lock()) {
                alive->connections.erase(connection);
              }
            });
          }
        };
        state->h3->start(callbacks);
      });

  if (listen_status != 0) {
    error = "relay HTTP/3: cannot listen on " + host + ":" +
            std::to_string(port) + " (" + std::to_string(listen_status) + ")";
    impl_->server.reset();
    impl_->tls.reset();
    return false;
  }
  impl_->listening = true;
  impl_->port = impl_->server->configured_port();
  return true;
#endif
}

void relay_h3_server::stop() {
#if MOLINK_SERVER_HAVE_H3_WS
  if (!impl_ || !impl_->server) return;
  impl_->listening = false;
  impl_->connections.clear();
  impl_->server->stop();
  impl_->server.reset();
  impl_->tls.reset();
  impl_->port = 0;
#endif
}

bool relay_h3_server::listening() const {
#if MOLINK_SERVER_HAVE_H3_WS
  return impl_ && impl_->listening;
#else
  return false;
#endif
}

int relay_h3_server::port() const {
#if MOLINK_SERVER_HAVE_H3_WS
  return impl_ ? impl_->port : 0;
#else
  return 0;
#endif
}

}  // namespace molink
