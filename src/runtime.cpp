#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <thread>

#include <uvcpp/uvcpp_threadpool.h>

#include "admin_api.h"
#include "admin_ui.h"
#include "db/legacy_import.h"
#include "device_api.h"
#include "logging.h"
#include "tls.h"
#include "util.h"
#include "version.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define MOLINK_PID GetCurrentProcessId()
#else
#include <unistd.h>
#define MOLINK_PID getpid()
#endif

namespace molink {
namespace {

bool loopback_host(const std::string& host) {
  return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

}  // namespace

server_runtime::server_runtime(server_config config)
    : config_(std::move(config)),
      db_(config_.database),
      executor_(db_),
      auth_(users_, sessions_, session_policy{}) {}

server_runtime::~server_runtime() { shutdown(); }

bool server_runtime::initialize(std::string& error) {
  // Database work and password hashing share libuv's thread pool; size it
  // before the first submission (the warning is emitted otherwise).
  uvcpp::uvcpp_set_threadpool_size(
      std::max(8, config_.database.pool_max * 2 + 4));

  std::error_code ec;
  std::filesystem::create_directories(config_.data_dir, ec);
  if (ec) {
    error = "cannot create the data directory: " + ec.message();
    return false;
  }
#if !defined(_WIN32)
  std::filesystem::permissions(config_.data_dir,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
#endif

  log_level level = log_level::info;
  parse_log_level(config_.log.level, level);
  if (!logger::instance().open(config_.log.file, level, config_.log.max_size_mb,
                               config_.log.max_files, error)) {
    return false;
  }

  // Portable installs may be started by double-clicking the binary. Persist
  // the implicit config.json path so the admin console has a file to rewrite
  // when TLS settings change.
  if (config_.config_path.empty()) config_.config_path = "config.json";
  if (!prepare_tls(config_, error)) return false;
  if (device_tls()) {
    log_info("TLS enabled: device and admin listeners use %s",
             config_.device.tls_cert.c_str());
  } else {
    log_info("TLS disabled: both listeners serve plain HTTP");
  }

  if (!db_.open(error) || !db_.migrate(error)) return false;

  {
    auto* client = db_.acquire();
    if (client == nullptr) {
      error = "database pool has no connection: " + db_.last_error();
      return false;
    }
    bool ok = settings_.load(*client, db_.dialect(), settings_repo_, error);
    if (ok) {
      const std::uint64_t now = epoch_ms();
      ok = ban_cache_.load(*client, db_.dialect(), bans_repo_, now, error);
    }
    if (ok && !config_.skip_legacy_import) {
      ok = import_legacy_devices(db_, *client, config_.data_dir, error);
    }
    db_.release(client);
    if (!ok) return false;
  }

  const security_settings security = settings_.security();
  auth_.set_policy(session_policy{
      static_cast<std::uint64_t>(security.session.idle_seconds) * 1000u,
      static_cast<std::uint64_t>(security.session.absolute_seconds) * 1000u});

  if (config_.setup_token.empty() && !loopback_host(config_.admin.host)) {
    auto* client = db_.acquire();
    if (client != nullptr) {
      std::size_t user_count = 0;
      std::string count_error;
      if (users().count_all(*client, db_.dialect(), user_count,
                                           count_error) &&
          user_count == 0) {
        config_.setup_token = random_hex(16);
        log_warn("admin listener is not loopback; one-time setup token: %s",
                 config_.setup_token.c_str());
      }
      db_.release(client);
    }
  }

  ui_root_ = resolve_admin_ui_root(config_);

  device_app_.set_host(config_.device.host)
      .set_port(config_.device.port)
      .set_server_header(std::string(MOLINK_SERVER_PRODUCT) + "/" +
                         MOLINK_SERVER_VERSION)
      .set_access_log(false)
      .set_max_body_size(128u * 1024u)
      // HTTP/2 carries the WebSocket relay leg (RFC 8441). ALPN h2 is used
      // when the device listener has a certificate; a plain listener falls
      // back to h2c prior knowledge.
      .set_http2_enabled(true);
  if (!config_.device.tls_cert.empty()) {
    device_app_.enable_ssl(config_.device.tls_cert, config_.device.tls_key);
    if (!device_app_.ssl_error().empty()) {
      error = "device TLS: " + device_app_.ssl_error();
      return false;
    }
  } else {
    device_app_.set_h2c_enabled(true);
  }
  register_device_routes(device_app_, *this);
  relay_tcp_ =
      std::make_unique<relay_tcp_server>(device_app_.loop(), relay_);
  relay_quic_ =
      std::make_unique<relay_quic_server>(device_app_.loop(), relay_);
  relay_udp_ =
      std::make_unique<relay_udp_server>(device_app_.loop(), relay_);
  if (!config_.device.tls_cert.empty()) {
    std::ifstream cert_file(config_.device.tls_cert, std::ios::binary);
    if (cert_file) {
      device_cert_pem_.assign(std::istreambuf_iterator<char>(cert_file),
                              std::istreambuf_iterator<char>());
    }
  }
  relay_h3_ = std::make_unique<relay_h3_server>(
      device_app_.loop(), relay_, [this](std::function<void()> fn) {
        device_app_.post(std::move(fn));
      });

  admin_app_.set_host(config_.admin.host)
      .set_port(config_.admin.port)
      .set_server_header(std::string(MOLINK_SERVER_PRODUCT) + "/" +
                         MOLINK_SERVER_VERSION)
      .set_access_log(false)
      .set_max_body_size(256u * 1024u)
      .set_http2_enabled(false);
  if (!config_.admin.tls_cert.empty()) {
    admin_app_.enable_ssl(config_.admin.tls_cert, config_.admin.tls_key);
    if (!admin_app_.ssl_error().empty()) {
      error = "admin TLS: " + admin_app_.ssl_error();
      return false;
    }
  }
  register_admin_routes(admin_app_, *this, ui_root_);

  if (!loopback_host(config_.admin.host) && !admin_tls() &&
      !config_.allow_insecure_admin) {
    log_warn("admin API is bound to %s without TLS; put it behind HTTPS",
             config_.admin.host.c_str());
  }

  log_info("%s %s: device %s:%d, admin %s:%d, database %s",
           MOLINK_SERVER_PRODUCT, MOLINK_SERVER_VERSION,
           config_.device.host.c_str(), config_.device.port,
           config_.admin.host.c_str(), config_.admin.port,
           db_driver_name(db_.dialect().driver()));
  return true;
}

bool server_runtime::start(std::string& error) {
  int rc = admin_app_.start();
  if (rc != 0) {
    error = "cannot start the admin listener (" + std::to_string(rc) + ")";
    return false;
  }
  rc = device_app_.start();
  if (rc != 0) {
    admin_app_.stop();
    admin_app_.join();
    error = "cannot start the device listener (" + std::to_string(rc) + ")";
    return false;
  }
  if (relay_tcp_ && config_.relay.tcp_port != 0) {
    // The endpoint creates UV handles, so start it on the device loop.
    auto started = std::make_shared<std::promise<std::string>>();
    auto started_future = started->get_future();
    relay_tcp_server* server = relay_tcp_.get();
    const std::string host = config_.device.host;
    const int port = config_.relay.tcp_port;
    const std::string cert = config_.device.tls_cert;
    const std::string key = config_.device.tls_key;
    device_app_.post([server, host, port, cert, key, started] {
      std::string error;
      const bool ok = server->start(host, port, cert, key, error);
      started->set_value(ok ? std::string() : error);
    });
    const std::string relay_error = started_future.get();
    if (relay_error.empty()) {
      log_info("relay TCP listening on %s:%d (%s)", host.c_str(), port,
               relay_tcp_->secure() ? "TLS" : "plain");
    } else {
      log_warn("%s", relay_error.c_str());
    }
  }
  if (relay_quic_ && config_.relay.quic_port != 0) {
    auto started = std::make_shared<std::promise<std::string>>();
    auto started_future = started->get_future();
    relay_quic_server* server = relay_quic_.get();
    const std::string host = config_.device.host;
    const int port = config_.relay.quic_port;
    const std::string cert = config_.device.tls_cert;
    const std::string key = config_.device.tls_key;
    device_app_.post([server, host, port, cert, key, started] {
      std::string error;
      const bool ok = server->start(host, port, cert, key, error);
      started->set_value(ok ? std::string() : error);
    });
    const std::string relay_error = started_future.get();
    if (relay_error.empty()) {
      log_info("relay QUIC listening on %s:%d", host.c_str(), port);
    } else {
      log_warn("%s", relay_error.c_str());
    }
  }
  if (relay_udp_ && config_.relay.udp_port != 0) {
    auto started = std::make_shared<std::promise<std::string>>();
    auto started_future = started->get_future();
    relay_udp_server* server = relay_udp_.get();
    const std::string host = config_.device.host;
    const int port = config_.relay.udp_port;
    device_app_.post([server, host, port, started] {
      std::string error;
      const bool ok = server->start(host, port, error);
      started->set_value(ok ? std::string() : error);
    });
    const std::string relay_error = started_future.get();
    if (relay_error.empty()) {
      log_info("relay UDP listening on %s:%d", host.c_str(), port);
    } else {
      log_warn("%s", relay_error.c_str());
    }
  }
  if (relay_h3_ && config_.relay.http3_port != 0) {
    auto started = std::make_shared<std::promise<std::string>>();
    auto started_future = started->get_future();
    relay_h3_server* server = relay_h3_.get();
    const std::string host = config_.device.host;
    const int port = config_.relay.http3_port;
    const std::string cert = config_.device.tls_cert;
    const std::string key = config_.device.tls_key;
    device_app_.post([server, host, port, cert, key, started] {
      std::string error;
      const bool ok = server->start(host, port, cert, key, error);
      started->set_value(ok ? std::string() : error);
    });
    const std::string relay_error = started_future.get();
    if (relay_error.empty()) {
      log_info("relay HTTP/3 listening on %s:%d", host.c_str(), port);
    } else {
      log_warn("%s", relay_error.c_str());
    }
  }
  started_ = true;
  started_ms_ = epoch_ms();
  if (config_.print_ready) {
    std::printf(
        "{\"event\":\"ready\",\"service\":\"%s\",\"version\":\"%s\","
        "\"pid\":%ld,\"device\":{\"host\":\"%s\",\"port\":%d},"
        "\"admin\":{\"host\":\"%s\",\"port\":%d},"
        "\"relay\":{\"tcp\":{\"port\":%d,\"secure\":%s,\"listening\":%s},"
        "\"quic\":{\"port\":%d,\"listening\":%s},"
        "\"udp\":{\"port\":%d,\"listening\":%s},"
        "\"http3\":{\"port\":%d,\"listening\":%s}},"
        "\"database\":\"%s\",\"ui\":\"%s\"}\n",
        MOLINK_SERVER_PRODUCT, MOLINK_SERVER_VERSION,
        static_cast<long>(MOLINK_PID), config_.device.host.c_str(),
        config_.device.port, config_.admin.host.c_str(), config_.admin.port,
        relay_tcp_port(), relay_tcp_secure() ? "true" : "false",
        relay_tcp_listening() ? "true" : "false", relay_quic_port(),
        relay_quic_listening() ? "true" : "false", relay_udp_port(),
        relay_udp_listening() ? "true" : "false", relay_h3_port(),
        relay_h3_listening() ? "true" : "false",
        db_driver_name(db_.dialect().driver()), ui_root_.c_str());
    std::fflush(stdout);
  }
  return true;
}

void server_runtime::request_stop() { stopping_.store(true); }

void server_runtime::tick() {
  const std::uint64_t now = epoch_ms();
  if (now - last_prune_ms_ < 60000) return;
  last_prune_ms_ = now;
  limiter_.prune(now);
  executor_.write(
      admin_app_.loop(),
      [this, now](uvcpp::uvcpp_db_client& client, std::string& error) {
        sessions_.prune_expired(client, db_.dialect(), now);
        bans_repo_.prune_expired(client, db_.dialect(), now);
        (void)error;
        return true;
      },
      [](bool, const std::string&) {});
  if (last_audit_prune_ms_ == 0 || now - last_audit_prune_ms_ > 86400000ull) {
    last_audit_prune_ms_ = now;
    const std::uint64_t cutoff = now - 90ull * 86400000ull;
    executor_.write(
        admin_app_.loop(),
        [this, cutoff](uvcpp::uvcpp_db_client& client, std::string&) {
          audit_repo_.prune_older_than(client, db_.dialect(), cutoff);
          return true;
        },
        [](bool, const std::string&) {});
  }
}

void server_runtime::shutdown() {
  if (shutdown_done_) return;
  shutdown_done_ = true;
  stopping_.store(true);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (executor_.in_flight() > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (started_) {
    if (relay_tcp_) {
      // Close the relay endpoint on the loop thread before it goes away.
      auto stopped = std::make_shared<std::promise<void>>();
      auto stopped_future = stopped->get_future();
      relay_tcp_server* tcp = relay_tcp_.get();
      relay_quic_server* quic = relay_quic_.get();
      relay_udp_server* udp = relay_udp_.get();
      relay_h3_server* h3 = relay_h3_.get();
      device_app_.post([tcp, quic, udp, h3, stopped] {
        tcp->stop();
        if (quic) quic->stop();
        if (udp) udp->stop();
        if (h3) h3->stop();
        stopped->set_value();
      });
      stopped_future.wait_for(std::chrono::seconds(2));
    }
    device_app_.stop();
    admin_app_.stop();
    device_app_.join();
    admin_app_.join();
  }
  executor_.wait_idle();
  db_.close();
  logger::instance().flush();
}

bool server_runtime::relay_tcp_listening() const {
  return relay_tcp_ && relay_tcp_->listening();
}

int server_runtime::relay_tcp_port() const {
  return relay_tcp_ ? relay_tcp_->port() : 0;
}

bool server_runtime::relay_tcp_secure() const {
  return relay_tcp_ && relay_tcp_->secure();
}

bool server_runtime::relay_quic_listening() const {
  return relay_quic_ && relay_quic_->listening();
}

int server_runtime::relay_quic_port() const {
  return relay_quic_ ? relay_quic_->port() : 0;
}

bool server_runtime::relay_udp_listening() const {
  return relay_udp_ && relay_udp_->listening();
}

int server_runtime::relay_udp_port() const {
  return relay_udp_ ? relay_udp_->port() : 0;
}

bool server_runtime::relay_h3_listening() const {
  return relay_h3_ && relay_h3_->listening();
}

int server_runtime::relay_h3_port() const {
  return relay_h3_ ? relay_h3_->port() : 0;
}

void server_runtime::audit(const std::string& event, const std::string& result,
                           const std::string& actor_id,
                           const std::string& actor_name,
                           const std::string& role, const std::string& ip,
                           const std::string& target,
                           const std::string& details_json) {
  audit_record record;
  record.ts_ms = epoch_ms();
  record.event = event;
  record.result = result;
  record.actor_id = actor_id;
  record.actor_name = actor_name;
  record.role = role;
  record.ip = ip;
  record.target = target;
  record.details_json = details_json.empty() ? "{}" : details_json;
  executor_.write(
      admin_app_.loop(),
      [this, record](uvcpp::uvcpp_db_client& client, std::string& error) {
        return audit_repo_.append(client, db_.dialect(), record, error);
      },
      [](bool ok, const std::string& error) {
        if (!ok) log_error("audit: %s", error.c_str());
      });
}

}  // namespace molink
