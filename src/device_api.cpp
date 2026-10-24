#include "device_api.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#include <cstdlib>

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
    body["endpoints"]["unregister"] = "POST /v1/devices/unregister";
    send_json(response, 200, body);
  });

  auto health = [&runtime](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& response,
                           uvcpp::uvcpp_web_next next) {
    auto result = std::make_shared<json>();
    runtime.executor().read(
        runtime.device_app().loop(),
        [](uvcpp::uvcpp_db_client& client, std::string& error) {
          const auto status = client.ping();
          if (!uvcpp::uvcpp_db_ok(status)) {
            error = client.last_error();
            return false;
          }
          return true;
        },
        [&response, next, result, &runtime](bool ok, const std::string&) {
          (*result)["status"] = ok ? "ok" : "degraded";
          (*result)["service"] = MOLINK_SERVER_PRODUCT;
          (*result)["version"] = MOLINK_SERVER_VERSION;
          (*result)["database"] = ok ? "ok" : "unavailable";
          (*result)["devices"] = 0;
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
             state->device.address.clear();

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
             struct heartbeat_state {
               bool found = false;
               std::string domain;
               bool domain_valid = false;
             };
             auto state = std::make_shared<heartbeat_state>();
             runtime.executor().write(
                 runtime.device_app().loop(),
                 [&runtime, id, token, ip, port, version, name, platform, avatar,
                  state](uvcpp::uvcpp_db_client& client, std::string& error) {
                   const std::string token_hash = sha256_hex(token);
                   if (!runtime.devices().heartbeat(
                           client, runtime.db().dialect(), id, token_hash, "",
                           ip, port, version, name, platform, avatar,
                           state->found, error)) {
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
        [&response, next, records, batch, &runtime](bool ok,
                                                    const std::string& error) {
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
          body["address"] = record.address;
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

  app.get("/v1/devices", lookup);
  app.get("/v1/devices/lookup", lookup);
}

}  // namespace molink
