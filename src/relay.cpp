#include "relay.h"

#include <algorithm>
#include <cstring>

#include "util.h"

namespace molink {
namespace {

constexpr std::uint64_t kUnusedSessionTtlMs = 10 * 60 * 1000;
constexpr const char* kReadyText = "{\"type\":\"relay-ready\"}";
constexpr const char* kClosedText = "{\"type\":\"relay-closed\"}";

}  // namespace

bool relay_hub::send(uvcpp::uvcpp_ws_connection* conn, const char* data,
                     std::size_t len, bool text) {
  if (conn == nullptr || (data == nullptr && len > 0)) return false;
  if (text) {
    conn->send_text(data, len);
  } else {
    conn->send_binary(data, len);
  }
  return true;
}

void relay_hub::notify(uvcpp::uvcpp_ws_connection* conn, const char* text) {
  if (conn != nullptr) conn->send_text(text, std::strlen(text));
}

relay_hub::session* relay_hub::find_session(const std::string& connect_id) {
  auto it = sessions_.find(connect_id);
  return it == sessions_.end() ? nullptr : &it->second;
}

relay_hub::peer* relay_hub::find_peer(session& value, const std::string& token,
                                      int& side) {
  for (int i = 0; i < 2; ++i) {
    if (!value.sides[i].token.empty() && value.sides[i].token == token) {
      side = i;
      return &value.sides[i];
    }
  }
  return nullptr;
}

bool relay_hub::allocate(const std::string& connect_id,
                         const std::string& device_id, allocation& out,
                         std::string& error) {
  if (connect_id.empty() || device_id.empty()) {
    error = "connect_id and device id are required";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const std::uint64_t now = epoch_ms();
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    const bool unused = it->second.sides[0].conn == nullptr &&
                        it->second.sides[1].conn == nullptr;
    if (unused && now > it->second.created_ms &&
        now - it->second.created_ms > kUnusedSessionTtlMs) {
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
  session& value = sessions_[connect_id];
  if (value.created_ms == 0) value.created_ms = now;
  value.connect_id = connect_id;

  for (int i = 0; i < 2; ++i) {
    if (value.sides[i].device_id == device_id) {
      out.connect_id = connect_id;
      out.token = value.sides[i].token;
      return true;
    }
  }
  for (int i = 0; i < 2; ++i) {
    if (value.sides[i].device_id.empty()) {
      value.sides[i].device_id = device_id;
      value.sides[i].token = random_hex(24);
      value.sides[i].updated_ms = now;
      out.connect_id = connect_id;
      out.token = value.sides[i].token;
      return true;
    }
  }
  error = "relay session is full";
  return false;
}

bool relay_hub::attach(uvcpp::uvcpp_ws_connection* conn,
                       const std::string& connect_id, const std::string& token,
                       std::string& error) {
  if (conn == nullptr || connect_id.empty() || token.empty()) {
    error = "missing relay credentials";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  session* value = find_session(connect_id);
  if (value == nullptr) {
    error = "unknown relay session";
    return false;
  }
  int side = -1;
  peer* entry = find_peer(*value, token, side);
  if (entry == nullptr) {
    error = "invalid relay token";
    return false;
  }
  if (entry->conn != nullptr && entry->conn != conn) {
    error = "relay token already in use";
    return false;
  }
  entry->conn = conn;
  entry->updated_ms = epoch_ms();
  bound_[conn] = {connect_id, side};

  peer& other = value->sides[side == 0 ? 1 : 0];
  if (other.conn != nullptr) {
    notify(entry->conn, kReadyText);
    notify(other.conn, kReadyText);
  }
  return true;
}

bool relay_hub::forward(uvcpp::uvcpp_ws_connection* from, const char* data,
                        std::size_t len, bool text) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto bound = bound_.find(from);
  if (bound == bound_.end()) return false;
  session* value = find_session(bound->second.first);
  if (value == nullptr) return false;
  const int side = bound->second.second;
  peer& other = value->sides[side == 0 ? 1 : 0];
  if (other.conn == nullptr) return false;
  other.updated_ms = epoch_ms();
  return send(other.conn, data, len, text);
}

void relay_hub::detach(uvcpp::uvcpp_ws_connection* conn) {
  uvcpp::uvcpp_ws_connection* other_conn = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto bound = bound_.find(conn);
    if (bound == bound_.end()) return;
    const std::string connect_id = bound->second.first;
    const int side = bound->second.second;
    bound_.erase(bound);
    auto it = sessions_.find(connect_id);
    if (it == sessions_.end()) return;
    session& value = it->second;
    value.sides[side].conn = nullptr;
    peer& other = value.sides[side == 0 ? 1 : 0];
    other_conn = other.conn;
    if (other_conn == nullptr) sessions_.erase(it);
  }
  notify(other_conn, kClosedText);
}

void relay_hub::close_session(const std::string& connect_id) {
  uvcpp::uvcpp_ws_connection* first = nullptr;
  uvcpp::uvcpp_ws_connection* second = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(connect_id);
    if (it == sessions_.end()) return;
    first = it->second.sides[0].conn;
    second = it->second.sides[1].conn;
    if (first != nullptr) bound_.erase(first);
    if (second != nullptr) bound_.erase(second);
    sessions_.erase(it);
  }
  if (first != nullptr) {
    notify(first, kClosedText);
    first->close(uvcpp::ws_close_code::NORMAL, "relay closed");
  }
  if (second != nullptr && second != first) {
    notify(second, kClosedText);
    second->close(uvcpp::ws_close_code::NORMAL, "relay closed");
  }
}

}  // namespace molink
