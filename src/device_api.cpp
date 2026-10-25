#include "device_api.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <memory>

#include "http_common.h"
#include "logging.h"
#include "runtime.h"
#include "util.h"
#include "version.h"

namespace molink {
namespace {

using nlohmann::json;

std::string display_id(const std::string& id) {
  if (id.size() != 9) return id;
  return id.substr(0, 3) + "-" + id.substr(3, 3) + "-" + id.substr(6, 3);
}

std::string normalize_device_id(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char c : raw) {
    if (c == '-' || c == '_' || c == ' ' || c == '\t') continue;
    if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

/// A domain code is typed/copied by people, so dashes and spaces are ignored
/// and lowercase is accepted; the stored form is 9 uppercase characters.
std::string normalize_domain(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char c : raw) {
    if (c == '-' || c == '_' || c == ' ' || c == '\t') continue;
    if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out.push_back(static_cast<char>(c));
    }
  }
  return out.size() == 9 ? out : std::string();
}

std::string sanitize_platform(std::string value) {
  value = lowercase_ascii(trim(std::move(value)));
  if (value.size() > 16) return std::string();
  static const char* kAllowed[] = {"windows", "macos", "linux", "android",
                                   "ios",     "web",   "unknown"};
  for (const char* allowed : kAllowed) {
    if (value == allowed) return value;
  }
  return std::string();
}

/// A client-reported LAN address. Only numeric addresses are accepted; the
/// directory returns it to requesters that share the same observed public IP.
std::string sanitize_address(std::string value) {
  value = trim(std::move(value));
  if (value.empty() || value.size() > 64) return std::string();
  if (value.size() > 2 && value.front() == '[' && value.back() == ']') {
    value = value.substr(1, value.size() - 2);
  }
  const std::size_t scope = value.find('%');
  if (scope != std::string::npos) value.resize(scope);

  unsigned char buffer[16];
  if (inet_pton(AF_INET, value.c_str(), buffer) == 1) {
    static const unsigned char any[4] = {0, 0, 0, 0};
    if (std::memcmp(buffer, any, sizeof(any)) == 0) return std::string();
    return value;
  }
  if (inet_pton(AF_INET6, value.c_str(), buffer) == 1) {
    static const unsigned char any[16] = {0};
    if (std::memcmp(buffer, any, sizeof(any)) == 0) return std::string();
    return value;
  }
  return std::string();
}

std::string sanitize_avatar(std::string value) {
  value = trim(std::move(value));
  if (value.size() > 32768) return std::string();
  std::string cleaned;
  cleaned.reserve(value.size());
  for (unsigned char c : value) {
    if (c < 0x20 || c == 0x7f) continue;
    cleaned.push_back(static_cast<char>(c));
  }
  if (cleaned.rfind("preset:", 0) == 0) {
    return cleaned.size() <= 64 ? cleaned : std::string();
  }
  if (cleaned.rfind("data:image/", 0) == 0 &&
      cleaned.find(";base64,") != std::string::npos) {
    return cleaned;
  }
  return std::string();
}

bool parse_device_body(const uvcpp::uvcpp_web_request& request,
                       nlohmann::json& body) {
  if (request.body_empty()) return false;
  const std::string type = request.header("Content-Type", "");
  if (type.find("application/json") == std::string::npos) return false;
  try {
    body = nlohmann::json::parse(request.body_str());
  } catch (...) {
    return false;
  }
  return body.is_object();
}

std::string sanitize_name(std::string value) {
  value = trim(std::move(value));
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (c < 0x20 || c == 0x7f) continue;
    out.push_back(static_cast<char>(c));
  }
  if (out.size() > 96) out.resize(96);
  return out.empty() ? std::string("device") : out;
}

std::string query(const uvcpp::uvcpp_web_request& request, const char* name) {
  const std::string* value = request.query(name);
  return value == nullptr ? std::string() : trim(*value);
}

bool parse_port(const std::string& text, int& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || value <= 0 || value > 65535) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

bool online(const device_record& record, int offline_after_seconds,
            std::uint64_t now_ms) {
  if (record.last_seen_ms == 0) return false;
  return now_ms <=
         record.last_seen_ms +
             static_cast<std::uint64_t>(offline_after_seconds) * 1000u;
}

void rate_limited(uvcpp::uvcpp_web_response& response, int retry_after) {
  response.set_header("Retry-After", std::to_string(retry_after));
  json body;
  body["type"] = "error";
  body["code"] = "rate_limited";
  body["message"] = "too many requests";
  body["retry_after"] = retry_after;
  send_json(response, 429, body);
}

/// Runs an authenticated device lookup on the DB executor. The callback passes
/// an empty code on success, or an error code the route can send directly.
using device_done = std::function<void(const std::string&, const device_record&)>;

void with_device(server_runtime& runtime, const std::string& id,
                 const std::string& token, device_done done) {
  struct state {
    bool ok = false;
    bool found = false;
    std::string error;
    device_record device;
  };
  auto value = std::make_shared<state>();
  const std::string token_hash = sha256_hex(token);
  runtime.executor().read(
      runtime.device_app().loop(),
      [&runtime, id, token_hash, value](uvcpp::uvcpp_db_client& client,
                                        std::string& error) {
        if (!runtime.devices().find_authorized(
                client, runtime.db().dialect(), id, token_hash, value->device,
                value->found, error)) {
          return false;
        }
        value->ok = true;
        return true;
      },
      [value, done](bool ok, const std::string& error) {
        if (!ok) {
          log_error("device signaling lookup: %s", error.c_str());
          done("database_unavailable", device_record{});
        } else if (!value->found) {
          done("invalid_token", device_record{});
        } else {
          done(std::string(), value->device);
        }
      });
}

std::vector<std::string> string_array(const json& body, const char* key) {
  std::vector<std::string> out;
  if (!body.contains(key) || !body[key].is_array()) return out;
  for (const json& item : body[key]) {
    if (item.is_string()) out.push_back(item.get<std::string>());
  }
  return out;
}

json signal_peer_json(const signal_peer& peer) {
  json out;
  out["id"] = peer.id;
  out["display_id"] = peer.display_id.empty() ? display_id(peer.id)
                                              : peer.display_id;
  out["name"] = peer.name;
  out["platform"] = peer.platform;
  out["avatar"] = peer.avatar;
  return out;
}

signal_peer to_signal_peer(const device_record& record) {
  signal_peer peer;
  peer.id = record.id;
  peer.name = record.name;
  peer.display_id = display_id(record.id);
  peer.platform = record.platform;
  peer.avatar = record.avatar;
  return peer;
}

json signal_session_json(const signal_session_view& session) {
  json out;
  out["connect_id"] = session.connect_id;
  out["from"] = signal_peer_json(session.from);
  out["to"] = signal_peer_json(session.to);
  out["offer"] = session.offer;
  out["answer"] = session.answer;
  out["from_candidates"] = session.from_candidates;
  out["to_candidates"] = session.to_candidates;
  out["created_ms"] = session.created_ms;
  out["expires_ms"] = session.expires_ms;
  out["answered"] = session.answered;
  out["closed"] = session.closed;
  out["expired"] = session.expired;
  return out;
}

int signal_http_status(signal_status status) {
  switch (status) {
    case signal_status::ok:
      return 200;
    case signal_status::not_found:
      return 404;
    case signal_status::expired:
    case signal_status::closed:
    case signal_status::conflict:
      return 409;
    case signal_status::forbidden:
      return 403;
    case signal_status::too_many:
      return 429;
    case signal_status::invalid:
      return 400;
  }
  return 400;
}

const char* signal_error_code(signal_status status) {
  switch (status) {
    case signal_status::ok:
      return "ok";
    case signal_status::not_found:
      return "connect_not_found";
    case signal_status::expired:
      return "connect_expired";
    case signal_status::closed:
      return "connect_closed";
    case signal_status::conflict:
      return "connect_conflict";
    case signal_status::forbidden:
      return "connect_forbidden";
    case signal_status::too_many:
      return "connect_limit";
    case signal_status::invalid:
      return "bad_request";
  }
  return "bad_request";
}

void send_lookup_error(uvcpp::uvcpp_web_response& response,
                       const std::string& code) {
  if (code == "invalid_token") {
    send_error(response, 401, "invalid_token", "unknown device or token");
  } else {
    send_error(response, 503, "directory_unavailable",
               "the device directory is unavailable");
  }
}

bool signaling_limited(server_runtime& runtime, const std::string& id,
                       const uvcpp::uvcpp_web_request& request,
                       uvcpp::uvcpp_web_response& response) {
  int retry = 0;
  const std::string key = "sig:" + request.peer_ip() + ":" + id;
  if (!runtime.limiter().allow(key, 8.0, 60, epoch_ms(), &retry)) {
    rate_limited(response, retry);
    return true;
  }
  return false;
}

void send_signal_status(uvcpp::uvcpp_web_response& response,
                        signal_status status) {
  if (status == signal_status::ok) {
    json body;
    body["ok"] = true;
    body["server_time"] = epoch_ms();
    send_json(response, 200, body);
    return;
  }
  send_error(response, signal_http_status(status), signal_error_code(status),
             status == signal_status::too_many
                 ? "too many pending connection requests"
                 : "the signaling request could not be completed");
}

}  // namespace

void register_device_routes(uvcpp::uvcpp_web_app& app, server_runtime& runtime) {
  app.use([&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
    if (runtime.stopping().load()) {
      response.status(503).json_str(R"({"type":"error","code":"shutting_down"})");
      response.end();
      return;
    }
    // The directory is consumed by the native clients, not a browser app; the
    // desktop UI calls /v1/devices directly from a loopback origin.
    response.set_header("Access-Control-Allow-Origin", "*");
    response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response.set_header("Access-Control-Allow-Headers", "Content-Type");
    response.set_header("X-Content-Type-Options", "nosniff");
    if (request.method() == uvcpp::http_method::HTTP_OPTIONS) {
      response.status(204).end();
      return;
    }

    const std::string path = request.path();
    const bool device_api = path.compare(0, 4, "/v1/") == 0;
    if (!device_api) {
      next();
      return;
    }

    const security_settings security = runtime.settings().security();
    ip_addr address;
    if (!effective_ip(request, security.trusted_proxies, address)) {
      send_error(response, 400, "bad_client_ip", "could not resolve the client address");
      return;
    }
    const std::string ip = canonical_ip(address);

    ban_record ban;
    if (runtime.bans().blocked(address, epoch_ms(), ban)) {
      int retry = 0;
      if (ban.expires_ms > epoch_ms()) {
        retry = static_cast<int>((ban.expires_ms - epoch_ms()) / 1000);
      }
      response.set_header("Retry-After", std::to_string(retry));
      json body;
      body["type"] = "error";
      body["code"] = "ip_banned";
      body["message"] = "this address is temporarily blocked";
      send_json(response, 403, body);
      return;
    }
    if (!policy_allows(security.policy, address)) {
      send_error(response, 403, "ip_denied",
                 "this address is not allowed by the server policy");
      return;
    }

    const limit_settings& limits = security.limits;
    int retry = 0;
    const std::string key = "dev:ip:" + ip;
    if (!runtime.limiter().allow(
            key, static_cast<double>(limits.device_ip_per_minute) / 60.0,
            limits.device_ip_burst, epoch_ms(), &retry)) {
      rate_limited(response, retry);
      return;
    }
    next();
  });

  app.get("/", [&runtime](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& response,
                          uvcpp::uvcpp_web_next) {
    const server_settings settings = runtime.settings().server();
    json body;
    body["service"] = MOLINK_SERVER_PRODUCT;
    body["version"] = MOLINK_SERVER_VERSION;
    body["public_url"] = settings.public_url;
    body["endpoints"]["register"] = "POST /v1/devices/register";
    body["endpoints"]["heartbeat"] = "POST /v1/devices/heartbeat";
    body["endpoints"]["lookup"] = "GET /v1/devices?id=ABC-DEF-GHJ";
    body["endpoints"]["domain"] = "POST /v1/devices/domain";
    body["endpoints"]["domain_devices"] = "GET /v1/devices/domain";
    body["endpoints"]["unregister"] = "POST /v1/devices/unregister";
    send_json(response, 200, body);
  });

  auto health = [&runtime](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& response,
                           uvcpp::uvcpp_web_next next) {
    auto result = std::make_shared<json>();
    auto devices = std::make_shared<std::int64_t>(0);
    runtime.executor().read(
        runtime.device_app().loop(),
        [devices](uvcpp::uvcpp_db_client& client, std::string& error) {
          const auto status = client.ping();
          if (!uvcpp::uvcpp_db_ok(status)) {
            error = client.last_error();
            return false;
          }
          uvcpp::uvcpp_db_table table;
          const auto count_status =
              client.query("SELECT COUNT(*) AS n FROM devices", &table);
          if (uvcpp::uvcpp_db_ok(count_status) && table.row_count() > 0) {
            *devices = table.value("n").to_int64();
          }
          return true;
        },
        [&response, next, result, devices](bool ok, const std::string&) {
          (*result)["status"] = ok ? "ok" : "degraded";
          (*result)["service"] = MOLINK_SERVER_PRODUCT;
          (*result)["version"] = MOLINK_SERVER_VERSION;
          (*result)["database"] = ok ? "ok" : "unavailable";
          (*result)["devices"] = *devices;
          send_json(response, ok ? 200 : 503, *result);
          next();
        });
  };
  app.get("/healthz", health);
  app.get("/api/health", health);

  app.post("/v1/devices/register",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const server_settings settings = runtime.settings().server();
             const security_settings security = runtime.settings().security();
             const limit_settings& limits = security.limits;
             nlohmann::json body;
             const bool has_body = parse_device_body(request, body);
             auto field = [&](const char* key) -> std::string {
               if (has_body && body.contains(key) && body[key].is_string()) {
                 return trim(body[key].get<std::string>());
               }
               const std::string* value = request.query(key);
               return value == nullptr ? std::string() : trim(*value);
             };
             auto field_int = [&](const char* key, int fallback) -> int {
               if (has_body && body.contains(key) &&
                   body[key].is_number_integer()) {
                 return body[key].get<int>();
               }
               const std::string* value = request.query(key);
               if (value == nullptr) return fallback;
               try {
                 return std::stoi(*value);
               } catch (...) {
                 return fallback;
               }
             };
             ip_addr address;
             if (!effective_ip(request, security.trusted_proxies, address)) {
               send_error(response, 400, "bad_client_ip",
                          "could not resolve the client address");
               return;
             }
             const std::string ip = canonical_ip(address);

             if (!settings.registration_token.empty()) {
               const std::string invite = field("invite");
               if (!constant_time_equals(invite, settings.registration_token)) {
                 send_error(response, 403, "invalid_invite",
                            "the registration invite is not valid");
                 return;
               }
             }

             int retry = 0;
             if (!runtime.limiter().allow(
                     "register:" + ip,
                     static_cast<double>(limits.register_per_hour) / 3600.0,
                     limits.register_burst, epoch_ms(), &retry)) {
               rate_limited(response, retry);
               return;
             }

             const std::string name = sanitize_name(field("name"));
             const int port = field_int("port", 0);
             if (port <= 0 || port > 65535) {
               send_error(response, 400, "bad_port", "port must be 1..65535");
               return;
             }
             std::string version = field("version");
             if (version.size() > 32) version.resize(32);
             const std::string platform = sanitize_platform(field("platform"));
             const std::string avatar = sanitize_avatar(field("avatar"));
             const std::string local_address = sanitize_address(field("address"));

             const std::string token = random_hex(16);
             if (token.empty()) {
               send_error(response, 500, "internal_error",
                          "secure random generator is unavailable");
               return;
             }
             struct state_t {
               device_record device;
               std::string domain;
               std::string code;
               std::string message;
             };
             auto state = std::make_shared<state_t>();
             state->device.name = name;
             state->device.peer_ip = ip;
             state->device.port = port;
             state->device.version = version;
             state->device.platform = platform;
             state->device.avatar = avatar;
             state->device.token_hash = sha256_hex(token);
             state->device.address = local_address;

             runtime.executor().write(
                 runtime.device_app().loop(),
                 [&runtime, state, ip, limit = limits.devices_per_ip](
                     uvcpp::uvcpp_db_client& client, std::string& error) {
                   std::size_t count = 0;
                   if (!runtime.devices().count_for_ip(
                           client, runtime.db().dialect(), ip, count, error)) {
                     return false;
                   }
                   if (count >= static_cast<std::size_t>(limit)) {
                     state->code = "ip_quota";
                     state->message = "too many devices registered from this address";
                     return false;
                   }
                   if (!runtime.devices().insert(client, runtime.db().dialect(),
                                                 state->device, error)) {
                     return false;
                   }
                   // Every client gets a channel of its own; sharing the code
                   // is how it invites other devices into that channel.
                   std::string domain;
                   if (!runtime.devices().create_domain(
                           client, runtime.db().dialect(), state->device.id,
                           domain, error)) {
                     return false;
                   }
                   bool joined = false;
                   bool valid = false;
                   if (!runtime.devices().join_domain(
                           client, runtime.db().dialect(), state->device.id,
                           state->device.token_hash, domain, joined, valid,
                           error) ||
                       !joined || !valid) {
                     if (error.empty()) error = "could not assign a domain code";
                     return false;
                   }
                   state->device.domain_code = domain;
                   state->domain = std::move(domain);
                   return true;
                 },
                 [&response, next, state, token, &runtime](
                     bool ok, const std::string& error) {
                   if (!ok) {
                     if (!state->code.empty()) {
                       send_error(response, 429, state->code, state->message);
                     } else {
                       log_error("device register: %s", error.c_str());
                       send_error(response, 503, "database_unavailable",
                                  "the device directory is unavailable");
                     }
                     next();
                     return;
                   }
                   const server_settings settings =
                       runtime.settings().server();
                   json body;
                   body["device_id"] = state->device.id;
                   body["display_id"] = display_id(state->device.id);
                   body["token"] = token;
                   body["domain"] = state->domain;
                   body["domain_valid"] = !state->domain.empty();
                   body["server_time"] = epoch_ms();
                   body["public_url"] = settings.public_url;
                   send_json(response, 200, body);
                   next();
                 });
           });

  app.post("/v1/devices/heartbeat",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const security_settings security = runtime.settings().security();
             ip_addr address;
             if (!effective_ip(request, security.trusted_proxies, address)) {
               send_error(response, 400, "bad_client_ip",
                          "could not resolve the client address");
               return;
             }
             const std::string ip = canonical_ip(address);
             nlohmann::json body;
             const bool has_body = parse_device_body(request, body);
             auto field = [&](const char* key) -> std::string {
               if (has_body && body.contains(key) && body[key].is_string()) {
                 return trim(body[key].get<std::string>());
               }
               const std::string* value = request.query(key);
               return value == nullptr ? std::string() : trim(*value);
             };
             auto field_int = [&](const char* key, int fallback) -> int {
               if (has_body && body.contains(key) &&
                   body[key].is_number_integer()) {
                 return body[key].get<int>();
               }
               const std::string* value = request.query(key);
               if (value == nullptr) return fallback;
               try {
                 return std::stoi(*value);
               } catch (...) {
                 return fallback;
               }
             };
             const std::string id = normalize_device_id(field("id"));
             const std::string token = field("token");
             if (id.empty() || token.empty()) {
               send_error(response, 400, "bad_request",
                          "id and token are required");
               return;
             }
             int retry = 0;
             if (!runtime.limiter().allow(
                     "hb:dev:" + id,
                     static_cast<double>(security.limits
                                             .heartbeat_per_device_per_minute) /
                         60.0,
                     security.limits.heartbeat_burst, epoch_ms(), &retry)) {
               rate_limited(response, retry);
               return;
             }
             if (!runtime.limiter().allow(
                     "hb:ip:" + ip,
                     static_cast<double>(security.limits
                                             .heartbeat_per_ip_per_minute) /
                         60.0,
                     security.limits.heartbeat_per_ip_per_minute, epoch_ms())) {
               rate_limited(response, 60);
               return;
             }
             const int port = field_int("port", 0);
             const std::string version = field("version");
             // An omitted name must stay omitted: sanitize_name maps empty to
             // "device", which would overwrite the registered name.
             const std::string raw_name = field("name");
             const std::string name =
                 raw_name.empty() ? std::string() : sanitize_name(raw_name);
             const std::string platform = sanitize_platform(field("platform"));
             const std::string avatar = sanitize_avatar(field("avatar"));
             const std::string local_address = sanitize_address(field("address"));
             struct heartbeat_state {
               bool found = false;
               std::string domain;
               bool domain_valid = false;
             };
             auto state = std::make_shared<heartbeat_state>();
             runtime.executor().write(
                 runtime.device_app().loop(),
                 [&runtime, id, token, ip, port, version, name, platform, avatar,
                  local_address, state](uvcpp::uvcpp_db_client& client,
                                        std::string& error) {
                   const std::string token_hash = sha256_hex(token);
                   if (!runtime.devices().heartbeat(
                           client, runtime.db().dialect(), id, token_hash,
                           local_address, ip, port, version, name, platform,
                           avatar, state->found, error)) {
                     return false;
                   }
                   if (!state->found) return true;
                   return runtime.devices().domain_for_device(
                       client, runtime.db().dialect(), id, state->domain,
                       state->domain_valid, error);
                 },
                 [&response, next, state](bool ok, const std::string& error) {
                   if (!ok) {
                     log_error("device heartbeat: %s", error.c_str());
                     send_error(response, 503, "database_unavailable",
                                "the device directory is unavailable");
                   } else if (!state->found) {
                     send_error(response, 401, "invalid_token",
                                "unknown device or token");
                   } else {
                     // The domain fields ride on every heartbeat so a client
                     // learns immediately when its code was revoked.
                     json body;
                     body["domain"] = state->domain;
                     body["domain_valid"] = state->domain_valid;
                     body["server_time"] = epoch_ms();
                     send_json(response, 200, body);
                   }
                   next();
                 });
           });

  // Domain ("channel") management. Codes are only ever issued here; a client
  // joins a group by entering a code another device shared with it. Rotating
  // revokes the code the caller owns, so every device that joined it becomes
  // "unknown_domain" on its next heartbeat.
  app.post("/v1/devices/domain",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const security_settings security = runtime.settings().security();
             nlohmann::json payload;
             const bool has_body = parse_device_body(request, payload);
             auto text = [&](const char* key) -> std::string {
               if (has_body && payload.contains(key) &&
                   payload[key].is_string()) {
                 return trim(payload[key].get<std::string>());
               }
               return query(request, key);
             };
             const std::string id = normalize_device_id(text("id"));
             const std::string token = text("token");
             const std::string action = lowercase_ascii(text("action"));
             if (id.empty() || token.empty()) {
               send_error(response, 400, "bad_request",
                          "id and token are required");
               return;
             }
             if (action != "join" && action != "rotate" &&
                 action != "leave") {
               send_error(response, 400, "bad_action",
                          "action must be join, rotate or leave");
               return;
             }
             std::string code;
             if (action == "join") {
               code = normalize_domain(text("code"));
               if (code.empty()) {
                 send_error(response, 400, "bad_domain",
                            "a 9-character domain code is required");
                 return;
               }
             }
             int retry = 0;
             if (!runtime.limiter().allow("domain:" + id, 10.0 / 60.0, 5,
                                          epoch_ms(), &retry)) {
               rate_limited(response, retry);
               return;
             }
             struct domain_state {
               bool found = false;
               bool code_valid = false;
               bool valid = false;
               std::string domain;
             };
             auto state = std::make_shared<domain_state>();
             const std::string token_hash = sha256_hex(token);
             runtime.executor().write(
                 runtime.device_app().loop(),
                 [&runtime, id, token_hash, action, code,
                  state](uvcpp::uvcpp_db_client& client, std::string& error) {
                   if (action == "join") {
                     if (!runtime.devices().join_domain(
                             client, runtime.db().dialect(), id, token_hash,
                             code, state->found, state->code_valid, error)) {
                       return false;
                     }
                     if (!state->found || !state->code_valid) return true;
                     state->domain = code;
                     state->valid = true;
                     return true;
                   }
                   if (action == "leave") {
                     if (!runtime.devices().clear_domain(
                             client, runtime.db().dialect(), id, token_hash,
                             state->found, error)) {
                       return false;
                     }
                     state->valid = state->found;
                     return true;
                   }
                   if (!runtime.devices().rotate_domain(
                           client, runtime.db().dialect(), id, token_hash,
                           state->domain, state->found, error)) {
                     return false;
                   }
                   state->valid = state->found;
                   return true;
                 },
                 [&response, next, state,
                  action](bool ok, const std::string& error) {
                   if (!ok) {
                     log_error("device domain: %s", error.c_str());
                     send_error(response, 503, "database_unavailable",
                                "the device directory is unavailable");
                   } else if (!state->found) {
                     send_error(response, 401, "invalid_token",
                                "unknown device or token");
                   } else if (action == "join" && !state->code_valid) {
                     send_error(response, 404, "unknown_domain",
                                "no such domain code");
                   } else {
                     json body;
                     body["domain"] = state->domain;
                     body["domain_valid"] = state->valid;
                     body["server_time"] = epoch_ms();
                     send_json(response, 200, body);
                   }
                   next();
                 });
           });

  // Same-domain device list. Only a member of the code can read it, and the
  // caller is left out of its own list.
  app.get("/v1/devices/domain",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            const security_settings security = runtime.settings().security();
            const std::string id = normalize_device_id(query(request, "id"));
            const std::string token = query(request, "token");
            const std::string requested = normalize_domain(query(request, "code"));
            if (id.empty() || token.empty()) {
              send_error(response, 400, "bad_request",
                         "id and token are required");
              return;
            }
            int retry = 0;
            if (!runtime.limiter().allow(
                    "domain-list:" + id,
                    static_cast<double>(security.limits.lookup_per_minute) /
                        60.0,
                    security.limits.lookup_burst, epoch_ms(), &retry)) {
              rate_limited(response, retry);
              return;
            }
            struct list_state {
              bool found = false;
              bool valid = false;
              std::string domain;
              std::vector<device_record> devices;
            };
            auto state = std::make_shared<list_state>();
            const std::string token_hash = sha256_hex(token);
            runtime.executor().read(
                runtime.device_app().loop(),
                [&runtime, id, token_hash, requested,
                 state](uvcpp::uvcpp_db_client& client, std::string& error) {
                  device_record device;
                  if (!runtime.devices().find_authorized(
                          client, runtime.db().dialect(), id, token_hash,
                          device, state->found, error)) {
                    return false;
                  }
                  if (!state->found) return true;
                  if (!runtime.devices().domain_for_device(
                          client, runtime.db().dialect(), id, state->domain,
                          state->valid, error)) {
                    return false;
                  }
                  if (!state->valid || state->domain.empty()) return true;
                  if (!requested.empty() && requested != state->domain) {
                    state->valid = false;
                    return true;
                  }
                  return runtime.devices().list_domain_devices(
                      client, runtime.db().dialect(), state->domain, id, 100,
                      state->devices, error);
                },
                [&response, next, state,
                 &runtime](bool ok, const std::string& error) {
                  if (!ok) {
                    log_error("device domain list: %s", error.c_str());
                    send_error(response, 503, "database_unavailable",
                               "the device directory is unavailable");
                  } else if (!state->found) {
                    send_error(response, 401, "invalid_token",
                               "unknown device or token");
                  } else if (!state->valid) {
                    send_error(response, 404, "unknown_domain",
                               "no such domain code");
                  } else {
                    const server_settings settings =
                        runtime.settings().server();
                    const std::uint64_t now = epoch_ms();
                    json body;
                    body["domain"] = state->domain;
                    body["devices"] = json::array();
                    for (const device_record& record : state->devices) {
                      json item;
                      item["id"] = record.id;
                      item["display_id"] = display_id(record.id);
                      item["name"] = record.name;
                      item["platform"] = record.platform;
                      item["avatar"] = record.avatar;
                      item["address"] = record.address;
                      item["peer_ip"] = record.peer_ip;
                      item["port"] = record.port;
                      item["online"] =
                          record.last_seen_ms != 0 &&
                          now <= record.last_seen_ms +
                                     static_cast<std::uint64_t>(
                                         settings.offline_after_seconds) *
                                         1000u;
                      item["last_seen_ms"] = record.last_seen_ms;
                      body["devices"].push_back(std::move(item));
                    }
                    body["server_time"] = now;
                    send_json(response, 200, body);
                  }
                  next();
                });
          });

  app.post("/v1/devices/unregister",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             const security_settings security = runtime.settings().security();
             ip_addr address;
             effective_ip(request, security.trusted_proxies, address);
             const std::string ip = canonical_ip(address);
             const std::string id = normalize_device_id(query(request, "id"));
             const std::string token = query(request, "token");
             if (id.empty() || token.empty()) {
               send_error(response, 400, "bad_request",
                          "id and token are required");
               return;
             }
             int retry = 0;
             if (!runtime.limiter().allow(
                     "unregister:" + ip,
                     static_cast<double>(security.limits
                                             .unregister_per_minute) /
                         60.0,
                     security.limits.unregister_burst, epoch_ms(), &retry)) {
               rate_limited(response, retry);
               return;
             }
             auto found = std::make_shared<bool>(false);
             runtime.executor().write(
                 runtime.device_app().loop(),
                 [&runtime, id, token, found](uvcpp::uvcpp_db_client& client,
                                               std::string& error) {
                   return runtime.devices().remove(
                       client, runtime.db().dialect(), id, sha256_hex(token),
                       *found);
                 },
                 [&response, next, found](bool ok, const std::string&) {
                   if (!ok) {
                     send_error(response, 503, "database_unavailable",
                                "the device directory is unavailable");
                   } else if (!*found) {
                     send_error(response, 401, "invalid_token",
                                "unknown device or token");
                   } else {
                     response.status(204).end();
                   }
                   next();
                 });
           });

  auto lookup = [&runtime](uvcpp::uvcpp_web_request& request,
                           uvcpp::uvcpp_web_response& response,
                           uvcpp::uvcpp_web_next next) {
    const security_settings security = runtime.settings().security();
    const std::string* ids_query = request.query("ids");
    const bool batch = ids_query != nullptr;
    std::vector<std::string> ids;
    if (batch) {
      for (const std::string& part : split(*ids_query, ',')) {
        const std::string id = normalize_device_id(part);
        if (id.empty()) continue;
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
          ids.push_back(id);
        }
        if (ids.size() >= 20) break;
      }
      if (ids.empty()) {
        send_error(response, 400, "bad_request", "ids is empty");
        return;
      }
    } else {
      const std::string id = normalize_device_id(query(request, "id"));
      if (id.empty()) {
        send_error(response, 400, "bad_request", "id is required");
        return;
      }
      ids.push_back(id);
    }

    ip_addr address;
    if (!effective_ip(request, security.trusted_proxies, address)) {
      send_error(response, 400, "bad_client_ip",
                 "could not resolve the client address");
      return;
    }
    int retry = 0;
    if (!runtime.limiter().allow(
            "lookup:" + canonical_ip(address),
            static_cast<double>(security.limits.lookup_per_minute) / 60.0,
            security.limits.lookup_burst, epoch_ms(), &retry)) {
      rate_limited(response, retry);
      return;
    }

    const std::string requester = canonical_ip(address);
    const bool requester_loopback = is_loopback(address);
    auto records = std::make_shared<std::vector<device_record>>();
    runtime.executor().read(
        runtime.device_app().loop(),
        [&runtime, ids, records](uvcpp::uvcpp_db_client& client,
                                 std::string& error) {
          for (const std::string& id : ids) {
            device_record record;
            if (runtime.devices().find(client, runtime.db().dialect(), id,
                                       record, error)) {
              records->push_back(std::move(record));
              error.clear();
            } else if (error == "device not found") {
              error.clear();
            } else {
              return false;
            }
          }
          return true;
        },
        [&response, next, records, batch, &runtime, requester,
         requester_loopback](bool ok, const std::string& error) {
          if (!ok) {
            log_error("device lookup: %s", error.c_str());
            send_error(response, 503, "database_unavailable",
                       "the device directory is unavailable");
            next();
            return;
          }
          const server_settings settings = runtime.settings().server();
          const std::uint64_t now = epoch_ms();
          auto online = [&](const device_record& record) {
            return record.last_seen_ms != 0 &&
                   now <= record.last_seen_ms +
                              static_cast<std::uint64_t>(
                                  settings.offline_after_seconds) *
                                  1000u;
          };
          if (batch) {
            json body;
            body["devices"] = json::array();
            for (const device_record& record : *records) {
              json item;
              item["id"] = record.id;
              item["display_id"] = display_id(record.id);
              item["name"] = record.name;
              item["platform"] = record.platform;
              item["avatar"] = record.avatar;
              item["online"] = online(record);
              item["last_seen_ms"] = record.last_seen_ms;
              body["devices"].push_back(std::move(item));
            }
            send_json(response, 200, body);
            next();
            return;
          }
          if (records->empty()) {
            send_error(response, 404, "not_found", "unknown device");
            next();
            return;
          }
          const device_record& record = records->front();
          json body;
          body["id"] = record.id;
          body["display_id"] = display_id(record.id);
          body["name"] = record.name;
          // A LAN address is only useful (and only safe to disclose) to a
          // requester behind the same NAT as the target.
          const bool same_nat =
              requester_loopback ||
              (!record.peer_ip.empty() && record.peer_ip == requester);
          body["address"] = same_nat ? record.address : std::string();
          body["peer_ip"] = record.peer_ip;
          body["port"] = record.port;
          body["version"] = record.version;
          body["platform"] = record.platform;
          body["avatar"] = record.avatar;
          body["online"] = online(record);
          body["created_ms"] = record.created_ms;
          body["last_seen_ms"] = record.last_seen_ms;
          body["public_url"] = settings.public_url;
          send_json(response, 200, body);
          next();
        });
  };

  // ---- P2P signaling ----------------------------------------------------
  // Clients exchange ICE offers/answers and trickled candidates through the
  // directory; the data path is established afterwards by the clients
  // (libjuice) or falls back to a relay. The directory never parses the SDP.
  app.post("/v1/connect",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             nlohmann::json body;
             if (!parse_device_body(request, body)) {
               send_error(response, 400, "bad_request",
                          "a JSON body is required");
               return;
             }
             auto text = [&](const char* key) -> std::string {
               if (!body.contains(key) || !body[key].is_string()) {
                 return std::string();
               }
               return trim(body[key].get<std::string>());
             };
             const std::string id = normalize_device_id(text("id"));
             const std::string token = text("token");
             const std::string target = normalize_device_id(text("target"));
             const std::string offer = text("offer");
             const std::vector<std::string> candidates =
                 string_array(body, "candidates");
             if (id.empty() || token.empty() || target.empty() ||
                 offer.empty()) {
               send_error(response, 400, "bad_request",
                          "id, token, target and offer are required");
               return;
             }
             if (id == target) {
               send_error(response, 400, "bad_request",
                          "a device cannot connect to itself");
               return;
             }
             if (signaling_limited(runtime, id, request, response)) return;
             with_device(
                 runtime, id, token,
                 [&runtime, target, offer, candidates, &response,
                  next](const std::string& code, const device_record& from) {
                   if (!code.empty()) {
                     send_lookup_error(response, code);
                     next();
                     return;
                   }
                   struct state {
                     bool ok = false;
                     bool found = false;
                     std::string error;
                     device_record to;
                   };
                   auto value = std::make_shared<state>();
                   runtime.executor().read(
                       runtime.device_app().loop(),
                       [&runtime, target, value](uvcpp::uvcpp_db_client& client,
                                                 std::string& error) {
                         if (!runtime.devices().find(
                                 client, runtime.db().dialect(), target,
                                 value->to, error)) {
                           return false;
                         }
                         value->ok = true;
                         value->found = !value->to.id.empty();
                         return true;
                       },
                       [&runtime, from, offer, candidates, &response, next,
                        value](bool ok, const std::string& error) {
                         if (!ok || value == nullptr) {
                           log_error("device connect target: %s", error.c_str());
                           send_lookup_error(response, "database_unavailable");
                           next();
                           return;
                         }
                         if (!value->found) {
                           send_error(response, 404, "unknown_device",
                                      "the target device is not registered");
                           next();
                           return;
                         }
                         const int offline_after =
                             runtime.settings().server().offline_after_seconds;
                         if (!online(value->to, offline_after, epoch_ms())) {
                           send_error(response, 409, "device_offline",
                                      "the target device is offline");
                           next();
                           return;
                         }
                         signal_create request_value;
                         request_value.from = to_signal_peer(from);
                         request_value.to = to_signal_peer(value->to);
                         request_value.offer = offer;
                         request_value.candidates = candidates;
                         std::string connect_id;
                         std::uint64_t expires_ms = 0;
                         const signal_status status =
                             runtime.signaling().create(request_value,
                                                        connect_id,
                                                        expires_ms);
                         if (status != signal_status::ok) {
                           send_signal_status(response, status);
                           next();
                           return;
                         }
                         json out;
                         out["connect_id"] = connect_id;
                         out["expires_ms"] = expires_ms;
                         out["server_time"] = epoch_ms();
                         out["to"] = signal_peer_json(request_value.to);
                         send_json(response, 200, out);
                         next();
                       });
                 });
           });

  app.get("/v1/connect/pending",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            const std::string id = normalize_device_id(query(request, "id"));
            const std::string token = query(request, "token");
            if (id.empty() || token.empty()) {
              send_error(response, 400, "bad_request",
                         "id and token are required");
              return;
            }
            if (signaling_limited(runtime, id, request, response)) return;
            with_device(
                runtime, id, token,
                [&runtime, id, &response, next](const std::string& code,
                                                const device_record&) {
                  if (!code.empty()) {
                    send_lookup_error(response, code);
                    next();
                    return;
                  }
                  std::vector<signal_session_view> sessions;
                  const signal_status status =
                      runtime.signaling().pending(id, sessions);
                  if (status != signal_status::ok) {
                    send_signal_status(response, status);
                    next();
                    return;
                  }
                  json out;
                  out["sessions"] = json::array();
                  for (const signal_session_view& session : sessions) {
                    out["sessions"].push_back(signal_session_json(session));
                  }
                  out["server_time"] = epoch_ms();
                  send_json(response, 200, out);
                  next();
                });
          });

  app.get("/v1/connect/status",
          [&runtime](uvcpp::uvcpp_web_request& request,
                     uvcpp::uvcpp_web_response& response,
                     uvcpp::uvcpp_web_next next) {
            const std::string id = normalize_device_id(query(request, "id"));
            const std::string token = query(request, "token");
            const std::string connect_id = query(request, "connect_id");
            if (id.empty() || token.empty() || connect_id.empty()) {
              send_error(response, 400, "bad_request",
                         "id, token and connect_id are required");
              return;
            }
            if (signaling_limited(runtime, id, request, response)) return;
            with_device(
                runtime, id, token,
                [&runtime, id, connect_id, &response,
                 next](const std::string& code, const device_record&) {
                  if (!code.empty()) {
                    send_lookup_error(response, code);
                    next();
                    return;
                  }
                  signal_session_view session;
                  const signal_status status =
                      runtime.signaling().find(id, connect_id, session);
                  if (status != signal_status::ok) {
                    send_signal_status(response, status);
                    next();
                    return;
                  }
                  json out;
                  out["session"] = signal_session_json(session);
                  out["server_time"] = epoch_ms();
                  send_json(response, 200, out);
                  next();
                });
          });

  app.post("/v1/connect/answer",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             nlohmann::json body;
             if (!parse_device_body(request, body)) {
               send_error(response, 400, "bad_request",
                          "a JSON body is required");
               return;
             }
             auto text = [&](const char* key) -> std::string {
               if (!body.contains(key) || !body[key].is_string()) {
                 return std::string();
               }
               return trim(body[key].get<std::string>());
             };
             const std::string id = normalize_device_id(text("id"));
             const std::string token = text("token");
             const std::string connect_id = text("connect_id");
             const std::string answer = text("answer");
             const std::vector<std::string> candidates =
                 string_array(body, "candidates");
             if (id.empty() || token.empty() || connect_id.empty() ||
                 answer.empty()) {
               send_error(response, 400, "bad_request",
                          "id, token, connect_id and answer are required");
               return;
             }
             if (signaling_limited(runtime, id, request, response)) return;
             with_device(
                 runtime, id, token,
                 [&runtime, id, connect_id, answer, candidates, &response,
                  next](const std::string& code, const device_record&) {
                   if (!code.empty()) {
                     send_lookup_error(response, code);
                     next();
                     return;
                   }
                   const signal_status status = runtime.signaling().answer(
                       id, connect_id, answer, candidates);
                   send_signal_status(response, status);
                   next();
                 });
           });

  app.post("/v1/connect/candidates",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             nlohmann::json body;
             if (!parse_device_body(request, body)) {
               send_error(response, 400, "bad_request",
                          "a JSON body is required");
               return;
             }
             auto text = [&](const char* key) -> std::string {
               if (!body.contains(key) || !body[key].is_string()) {
                 return std::string();
               }
               return trim(body[key].get<std::string>());
             };
             const std::string id = normalize_device_id(text("id"));
             const std::string token = text("token");
             const std::string connect_id = text("connect_id");
             const std::vector<std::string> candidates =
                 string_array(body, "candidates");
             if (id.empty() || token.empty() || connect_id.empty() ||
                 candidates.empty()) {
               send_error(response, 400, "bad_request",
                          "id, token, connect_id and candidates are required");
               return;
             }
             if (signaling_limited(runtime, id, request, response)) return;
             with_device(
                 runtime, id, token,
                 [&runtime, id, connect_id, candidates, &response,
                  next](const std::string& code, const device_record&) {
                   if (!code.empty()) {
                     send_lookup_error(response, code);
                     next();
                     return;
                   }
                   const signal_status status =
                       runtime.signaling().add_candidates(id, connect_id,
                                                          candidates);
                   send_signal_status(response, status);
                   next();
                 });
           });

  app.post("/v1/connect/close",
           [&runtime](uvcpp::uvcpp_web_request& request,
                      uvcpp::uvcpp_web_response& response,
                      uvcpp::uvcpp_web_next next) {
             nlohmann::json body;
             if (!parse_device_body(request, body)) {
               send_error(response, 400, "bad_request",
                          "a JSON body is required");
               return;
             }
             auto text = [&](const char* key) -> std::string {
               if (!body.contains(key) || !body[key].is_string()) {
                 return std::string();
               }
               return trim(body[key].get<std::string>());
             };
             const std::string id = normalize_device_id(text("id"));
             const std::string token = text("token");
             const std::string connect_id = text("connect_id");
             if (id.empty() || token.empty() || connect_id.empty()) {
               send_error(response, 400, "bad_request",
                          "id, token and connect_id are required");
               return;
             }
             if (signaling_limited(runtime, id, request, response)) return;
             with_device(
                 runtime, id, token,
                 [&runtime, id, connect_id, &response,
                  next](const std::string& code, const device_record&) {
                   if (!code.empty()) {
                     send_lookup_error(response, code);
                     next();
                     return;
                   }
                   const signal_status status =
                       runtime.signaling().close(id, connect_id);
                   send_signal_status(response, status);
                   next();
                 });
           });

  app.get("/v1/devices", lookup);
  app.get("/v1/devices/lookup", lookup);
}

}  // namespace molink
