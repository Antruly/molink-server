#include <cstdio>
#include <filesystem>

#include "db/database.h"
#include "db/repositories.h"
#include "security/ip.h"
#include "util.h"

namespace {

int failures = 0;

#define CHECK(condition)                                              \
  do {                                                                \
    if (!(condition)) {                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                   #condition);                                       \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

}  // namespace

int main() {
  using namespace molink;
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "molink-server-repository-test";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  database_config config;
  config.driver = "sqlite";
  config.url = sqlite_url_for(dir.string());
  config.pool_min = 1;
  config.pool_max = 2;
  config.busy_timeout_ms = 2000;

  database db(config);
  std::string err;
  CHECK(db.open(err));
  CHECK(db.migrate(err));
  if (failures != 0) return 1;

  const sql_dialect& dialect = db.dialect();
  device_repository devices;
  user_repository users;
  session_repository sessions;
  settings_repository settings;
  ban_repository bans;

  uvcpp::uvcpp_db_client* client = db.acquire();
  CHECK(client != nullptr);
  if (client == nullptr) return 1;

  CHECK(settings.ensure_defaults(*client, dialect, err));
  std::string json;
  bool found = false;
  CHECK(settings.get(*client, dialect, "security", json, found, err));
  CHECK(found && json.find("allow_mainland_china") != std::string::npos);
  std::int64_t revision = 0;
  CHECK(settings.revision(*client, dialect, revision, err));
  CHECK(settings.bump_revision(*client, dialect, revision, err));
  CHECK(revision >= 2);

  admin_user user;
  user.id = "u_test";
  user.username = "admin";
  user.display_name = "Administrator";
  user.role = "admin";
  user.password_iterations = 600000;
  user.password_salt = "00112233445566778899aabbccddeeff";
  user.password_hash = "deadbeef";
  user.created_ms = epoch_ms();
  user.updated_ms = user.created_ms;
  CHECK(users.create(*client, dialect, user, err));
  admin_user loaded;
  CHECK(users.find_by_username(*client, dialect, "ADMIN", loaded, found, err));
  CHECK(found && loaded.username == "admin");
  std::size_t admins = 0;
  CHECK(users.count_enabled_admins(*client, dialect, admins, err));
  CHECK(admins == 1);

  session_record session;
  session.id = "s_test";
  session.token_sha256 = "tokenhash";
  session.user_id = user.id;
  session.created_ms = epoch_ms();
  session.last_seen_ms = session.created_ms;
  session.expires_ms = session.created_ms + 3600000;
  session.ip = "127.0.0.1";
  CHECK(sessions.create(*client, dialect, session, err));
  session_record loaded_session;
  CHECK(sessions.find_by_token(*client, dialect, "tokenhash", loaded_session,
                               found, err));
  CHECK(found && loaded_session.user_id == user.id);

  device_record device;
  device.token_hash = "device-token-hash";
  device.name = "Office";
  device.peer_ip = "127.0.0.1";
  device.port = 28418;
  device.quic_port = 28420;
  device.tcp_port = 28421;
  device.version = "0.8.0";
  CHECK(devices.insert(*client, dialect, device, err));
  CHECK(device.id.size() == 9);

  bool heartbeat_found = false;
  CHECK(devices.heartbeat(*client, dialect, device.id, device.token_hash,
                          "192.168.1.20", "127.0.0.1", 28418, 28420, 28421,
                          "aa:bb", "PEM", "0.8.1", "Renamed", "linux", "",
                          heartbeat_found, err));
  CHECK(heartbeat_found);
  // Omitting the web port (0) must not clear it. The data ports are always
  // reported, so they stay as they were sent.
  CHECK(devices.heartbeat(*client, dialect, device.id, device.token_hash, "",
                          "127.0.0.1", 0, 28420, 28421, "", "", "", "", "", "",
                          heartbeat_found, err));
  CHECK(heartbeat_found);
  device_record refreshed;
  CHECK(devices.find(*client, dialect, device.id, refreshed, err));
  CHECK(refreshed.port == 28418);
  CHECK(refreshed.quic_port == 28420);
  CHECK(refreshed.tcp_port == 28421);
  // A later heartbeat with 0 clears a data port that stopped listening.
  CHECK(devices.heartbeat(*client, dialect, device.id, device.token_hash, "",
                          "127.0.0.1", 0, 0, 28421, "", "", "", "", "", "",
                          heartbeat_found, err));
  CHECK(devices.find(*client, dialect, device.id, refreshed, err));
  CHECK(refreshed.quic_port == 0);
  CHECK(refreshed.tcp_port == 28421);
  CHECK(!devices.heartbeat(*client, dialect, device.id, "wrong-token", "", "",
                           0, 0, 0, "", "", "", "", "", "", heartbeat_found,
                           err) ||
        !heartbeat_found);

  // --- domains -----------------------------------------------------------
  std::string domain;
  CHECK(devices.create_domain(*client, dialect, device.id, domain, err));
  CHECK(domain.size() == 9);
  bool joined = false;
  bool code_valid = false;
  CHECK(devices.join_domain(*client, dialect, device.id, device.token_hash,
                            domain, joined, code_valid, err));
  CHECK(joined && code_valid);
  std::string current_domain;
  bool domain_valid = false;
  CHECK(devices.domain_for_device(*client, dialect, device.id, current_domain,
                                  domain_valid, err));
  CHECK(current_domain == domain && domain_valid);

  device_record second;
  second.token_hash = "second-device-token";
  second.name = "Field";
  second.peer_ip = "127.0.0.1";
  second.port = 28419;
  CHECK(devices.insert(*client, dialect, second, err));
  CHECK(devices.join_domain(*client, dialect, second.id, second.token_hash,
                            domain, joined, code_valid, err));
  CHECK(joined && code_valid);

  std::vector<device_record> peers;
  CHECK(devices.list_domain_devices(*client, dialect, domain, device.id, 100,
                                    peers, err));
  CHECK(peers.size() == 1 && peers.front().id == second.id);

  // Rotating the owner's code revokes it for every member.
  std::string rotated;
  CHECK(devices.rotate_domain(*client, dialect, device.id, device.token_hash,
                              rotated, joined, err));
  CHECK(joined && rotated.size() == 9 && rotated != domain);
  CHECK(devices.domain_for_device(*client, dialect, second.id, current_domain,
                                  domain_valid, err));
  CHECK(current_domain == domain && !domain_valid);
  CHECK(devices.join_domain(*client, dialect, second.id, second.token_hash,
                            domain, joined, code_valid, err));
  CHECK(joined && !code_valid);
  CHECK(devices.join_domain(*client, dialect, second.id, second.token_hash,
                            rotated, joined, code_valid, err));
  CHECK(joined && code_valid);
  CHECK(devices.list_domain_devices(*client, dialect, rotated, device.id, 100,
                                    peers, err));
  CHECK(peers.size() == 1 && peers.front().id == second.id);

  device_query query;
  query.page = 1;
  query.page_size = 10;
  device_page page;
  CHECK(devices.list(*client, dialect, query, 90000, epoch_ms(), page, err));
  CHECK(page.total == 2);
  bool saw_renamed = false;
  for (const device_record& item : page.items) {
    if (item.name != "Renamed") continue;
    saw_renamed = true;
    CHECK(item.platform == "linux");
  }
  CHECK(saw_renamed);

  ban_record ban;
  ban.id = "ban_test";
  ban.target = "8.8.8.0/24";
  ban.target_kind = "cidr";
  ban.source = "manual";
  ban.created_ms = epoch_ms();
  ban.expires_ms = 0;
  CHECK(bans.add(*client, dialect, ban, err));
  ip_addr address;
  CHECK(parse_ip("8.8.8.8", address));
  ban_record hit;
  bool banned = false;
  CHECK(bans.match(*client, dialect, address, epoch_ms(), hit, banned, err));
  CHECK(banned && hit.id == "ban_test");
  CHECK(parse_ip("8.8.4.4", address));
  CHECK(bans.match(*client, dialect, address, epoch_ms(), hit, banned, err));
  CHECK(!banned);

  CHECK(devices.revoke(*client, dialect, device.id, found));
  CHECK(found);
  db.release(client);
  db.close();
  std::filesystem::remove_all(dir, ec);

  if (failures != 0) {
    std::fprintf(stderr, "%d repository checks failed\n", failures);
    return 1;
  }
  std::printf("ok: repositories\n");
  return 0;
}
