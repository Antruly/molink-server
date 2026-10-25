#include <cstdio>

#include "signaling.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", message);
  ++failures;
}

molink::signal_peer peer(const char* id, const char* name) {
  molink::signal_peer value;
  value.id = id;
  value.name = name;
  value.display_id = id;
  value.platform = "linux";
  return value;
}

}  // namespace

int main() {
  using namespace molink;

  signaling_service service;
  signal_create request;
  request.from = peer("AAAA11111", "alpha");
  request.to = peer("BBBB22222", "beta");
  request.offer = "v=0\r\n...offer...";
  request.candidates = {"candidate:1 1 udp 1 192.0.2.1 5000 typ host"};

  std::string connect_id;
  std::uint64_t expires_ms = 0;
  expect(service.create(request, connect_id, expires_ms) == signal_status::ok,
         "create succeeds");
  expect(!connect_id.empty(), "connect id is issued");
  expect(expires_ms > 0, "expiry is set");

  expect(service.create(request, connect_id, expires_ms) == signal_status::ok,
         "a second create succeeds");

  std::vector<signal_session_view> pending;
  expect(service.pending("BBBB22222", pending) == signal_status::ok,
         "pending succeeds");
  expect(pending.size() == 2, "both sessions are pending");
  expect(pending[0].from.name == "alpha", "pending carries the initiator");
  expect(pending[0].from_candidates.size() == 1,
         "pending carries the offer candidates");

  const std::string session_id = pending[0].connect_id;
  expect(service.find("CCCC33333", session_id, pending[0]) ==
             signal_status::forbidden,
         "a third device cannot read the session");

  expect(service.answer("BBBB22222", session_id, "v=0\r\n...answer...",
                        {"candidate:2 1 udp 1 198.51.100.1 6000 typ host"}) ==
             signal_status::ok,
         "the target answers");
  expect(service.answer("BBBB22222", session_id, "again", {}) ==
             signal_status::conflict,
         "a second answer is rejected");
  expect(service.answer("AAAA11111", session_id, "wrong-side", {}) ==
             signal_status::forbidden,
         "the initiator cannot answer");

  signal_session_view view;
  expect(service.find("AAAA11111", session_id, view) == signal_status::ok,
         "the initiator reads the session");
  expect(view.answered, "the session is answered");
  expect(view.answer == "v=0\r\n...answer...", "the answer is relayed");
  expect(view.to_candidates.size() == 1, "the target candidates are relayed");

  expect(service.add_candidates("AAAA11111", session_id,
                                {"candidate:3 1 udp 1 203.0.113.1 7000 typ host"}) ==
             signal_status::ok,
         "trickled candidates are accepted");
  expect(service.find("BBBB22222", session_id, view) == signal_status::ok,
         "the target reads the session");
  expect(view.from_candidates.size() == 2, "both sides see the candidates");

  expect(service.close("AAAA11111", session_id) == signal_status::ok,
         "either side closes");
  expect(service.find("BBBB22222", session_id, view) == signal_status::ok,
         "a closed session is still readable");
  expect(view.closed, "the closed flag is reported");
  expect(service.answer("BBBB22222", session_id, "late", {}) ==
             signal_status::closed,
         "a closed session cannot be answered");

  // Session limit: 8 sessions involving a device, including the two pending
  // above minus the closed one.
  signal_create extra;
  extra.from = peer("DDDD44444", "delta");
  extra.to = peer("AAAA11111", "alpha");
  extra.offer = "offer";
  int created = 0;
  for (int i = 0; i < 12; ++i) {
    std::string id;
    std::uint64_t expires = 0;
    if (service.create(extra, id, expires) == signal_status::ok) ++created;
  }
  expect(created > 0 && created < 12, "the per-device session limit applies");

  expect(service.close("DDDD44444", "missing-session") ==
             signal_status::not_found,
         "an unknown session is not found");

  // Invalid input.
  signal_create invalid = request;
  invalid.offer.clear();
  std::string unused_id;
  std::uint64_t unused_expiry = 0;
  expect(service.create(invalid, unused_id, unused_expiry) ==
             signal_status::invalid,
         "an empty offer is rejected");

  if (failures == 0) {
    std::printf("ok: signaling\n");
    return 0;
  }
  return 1;
}
