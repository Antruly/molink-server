#include "tls.h"

#include <nlohmann/json.hpp>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace molink {
namespace {

namespace fs = std::filesystem;

using bio_ptr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using x509_ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using pkey_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using ctx_ptr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using ext_ptr = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;

std::string openssl_error() {
  const unsigned long code = ERR_get_error();
  if (code == 0) return "unknown OpenSSL error";
  std::array<char, 256> buffer{};
  ERR_error_string_n(code, buffer.data(), buffer.size());
  return std::string(buffer.data());
}

bool is_regular_file(const std::string& path) {
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(path, ec);
}

std::string first_existing(const fs::path& dir,
                           std::initializer_list<const char*> names) {
  std::error_code ec;
  for (const char* name : names) {
    const fs::path candidate = dir / name;
    if (fs::is_regular_file(candidate, ec)) return candidate.string();
  }
  return {};
}

std::string host_name() {
#if defined(_WIN32)
  std::array<char, MAX_COMPUTERNAME_LENGTH + 1> buffer{};
  DWORD size = static_cast<DWORD>(buffer.size());
  if (GetComputerNameA(buffer.data(), &size) && size > 0) {
    return std::string(buffer.data(), size);
  }
#else
  std::array<char, 256> buffer{};
  if (gethostname(buffer.data(), buffer.size()) == 0) {
    buffer.back() = '\0';
    return std::string(buffer.data());
  }
#endif
  return "molink-server";
}

std::vector<std::string> local_ip_addresses() {
  std::vector<std::string> out;
  auto add = [&out](std::string value) {
    // getnameinfo may append a scope id (fe80::1%eth0); OpenSSL rejects it in
    // an IP SAN, and the scope is not part of the address.
    const std::size_t scope = value.find('%');
    if (scope != std::string::npos) value.resize(scope);
    if (value.empty()) return;
    if (std::find(out.begin(), out.end(), value) == out.end()) {
      out.push_back(value);
    }
  };
  add("127.0.0.1");
  add("::1");
#if defined(_WIN32)
  ULONG size = 16 * 1024;
  std::vector<unsigned char> storage(size);
  auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
  ULONG result = GetAdaptersAddresses(
      AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                     GAA_FLAG_SKIP_DNS_SERVER,
      nullptr, addresses, &size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    storage.resize(size);
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    result = GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                       GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addresses, &size);
  }
  if (result != NO_ERROR) return out;
  for (auto* adapter = addresses; adapter != nullptr;
       adapter = adapter->Next) {
    for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
         unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr == nullptr) continue;
      const sockaddr* address = unicast->Address.lpSockaddr;
      std::array<char, INET6_ADDRSTRLEN> buffer{};
      const int family = address->sa_family;
      if (family != AF_INET && family != AF_INET6) continue;
      const void* raw = family == AF_INET
                            ? static_cast<const void*>(
                                  &reinterpret_cast<const sockaddr_in*>(address)
                                       ->sin_addr)
                            : static_cast<const void*>(
                                  &reinterpret_cast<const sockaddr_in6*>(address)
                                       ->sin6_addr);
      if (inet_ntop(family, raw, buffer.data(), buffer.size()) != nullptr) {
        add(buffer.data());
      }
    }
  }
#else
  struct ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return out;
  for (struct ifaddrs* item = interfaces; item != nullptr;
       item = item->ifa_next) {
    if (item->ifa_addr == nullptr) continue;
    const int family = item->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;
    const socklen_t length =
        family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    std::array<char, NI_MAXHOST> buffer{};
    if (getnameinfo(item->ifa_addr, length, buffer.data(), buffer.size(),
                    nullptr, 0, NI_NUMERICHOST) == 0) {
      add(buffer.data());
    }
  }
  freeifaddrs(interfaces);
#endif
  return out;
}

bool add_extension(X509* certificate, int nid, const std::string& value) {
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
  ext_ptr extension(
      X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str()),
      X509_EXTENSION_free);
  if (!extension) return false;
  return X509_add_ext(certificate, extension.get(), -1) == 1;
}

std::string san_string(const std::vector<std::string>& dns_names,
                       const std::vector<std::string>& ip_addresses) {
  std::string value;
  auto append = [&value](const std::string& entry) {
    if (entry.empty()) return;
    if (!value.empty()) value += ",";
    value += entry;
  };
  for (const std::string& name : dns_names) append("DNS:" + name);
  for (const std::string& ip : ip_addresses) append("IP:" + ip);
  return value;
}

bool write_text(const std::string& path, const std::string& data,
                std::string& error) {
  // The path overload handles non-ASCII directories on Windows.
  std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot write " + path;
    return false;
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.flush();
  if (!out) {
    error = "cannot write " + path;
    return false;
  }
  return true;
}

std::string pem_from_x509(X509* certificate, bool private_key,
                          EVP_PKEY* key) {
  bio_ptr bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio) return {};
  int ok = 0;
  if (private_key) {
    ok = PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr,
                                  nullptr);
  } else {
    ok = PEM_write_bio_X509(bio.get(), certificate);
  }
  if (ok != 1) return {};
  char* data = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &data);
  if (length <= 0 || data == nullptr) return {};
  return std::string(data, static_cast<std::size_t>(length));
}

std::uint64_t asn1_time_ms(const ASN1_TIME* value) {
  if (value == nullptr) return 0;
  std::tm tm{};
  if (ASN1_TIME_to_tm(value, &tm) != 1) return 0;
#if defined(_WIN32)
  const std::time_t seconds = _mkgmtime(&tm);
#else
  const std::time_t seconds = timegm(&tm);
#endif
  if (seconds < 0) return 0;
  return static_cast<std::uint64_t>(seconds) * 1000ull;
}

std::string name_common_name(const X509_NAME* name) {
  if (name == nullptr) return {};
  std::array<char, 256> buffer{};
  const int length = X509_NAME_get_text_by_NID(name, NID_commonName,
                                               buffer.data(), buffer.size());
  if (length <= 0) return {};
  return std::string(buffer.data(), static_cast<std::size_t>(length));
}

std::string hex_encode(const unsigned char* data, std::size_t length) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0f]);
  }
  return out;
}

bool read_file_text(const std::string& path, std::string& out,
                    std::string& error) {
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>());
  if (out.empty()) {
    error = "empty file: " + path;
    return false;
  }
  return true;
}

bool read_pem_certificate(const std::string& path, x509_ptr& certificate,
                          std::string& error) {
  std::string text;
  if (!read_file_text(path, text, error)) return false;
  bio_ptr bio(BIO_new_mem_buf(text.data(), static_cast<int>(text.size())),
              BIO_free);
  if (!bio) {
    error = "cannot read certificate: " + path;
    return false;
  }
  certificate.reset(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
  if (!certificate) {
    error = "cannot parse certificate: " + path + " (" + openssl_error() + ")";
    return false;
  }
  return true;
}

bool read_pem_private_key(const std::string& path, pkey_ptr& key,
                          std::string& error) {
  std::string text;
  if (!read_file_text(path, text, error)) return false;
  bio_ptr bio(BIO_new_mem_buf(text.data(), static_cast<int>(text.size())),
              BIO_free);
  if (!bio) {
    error = "cannot read private key: " + path;
    return false;
  }
  key.reset(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
  if (!key) {
    error = "cannot parse private key: " + path + " (" + openssl_error() + ")";
    return false;
  }
  return true;
}

bool verify_pair_files(const std::string& cert_path, const std::string& key_path,
                       std::string& error) {
  x509_ptr certificate(nullptr, X509_free);
  pkey_ptr key(nullptr, EVP_PKEY_free);
  if (!read_pem_certificate(cert_path, certificate, error)) return false;
  if (!read_pem_private_key(key_path, key, error)) return false;
  if (X509_check_private_key(certificate.get(), key.get()) != 1) {
    error = "the certificate and private key do not match: " + cert_path;
    return false;
  }
  return true;
}

bool write_pair(const std::string& cert_path, const std::string& key_path,
                const std::string& cert_pem, const std::string& key_pem,
                std::string& error) {
  const fs::path cert_file(cert_path);
  const fs::path key_file(key_path);
  std::error_code ec;
  fs::create_directories(cert_file.parent_path(), ec);
  if (ec) {
    error = "cannot create " + cert_file.parent_path().string() + ": " +
            ec.message();
    return false;
  }
  fs::create_directories(key_file.parent_path(), ec);
  if (ec) {
    error = "cannot create " + key_file.parent_path().string() + ": " +
            ec.message();
    return false;
  }
  if (!write_text(cert_path, cert_pem, error)) return false;
  if (!write_text(key_path, key_pem, error)) {
    std::error_code ignored;
    fs::remove(cert_path, ignored);
    return false;
  }
#if !defined(_WIN32)
  fs::permissions(key_path, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, ec);
#endif
  return true;
}

bool write_json_atomic(const std::string& target, const std::string& text,
                       std::string& error) {
  const std::string temporary = target + ".tmp";
  if (!write_text(temporary, text, error)) return false;
#if defined(_WIN32)
  const std::wstring from = fs::path(temporary).wstring();
  const std::wstring to = fs::path(target).wstring();
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    error = "cannot replace " + target;
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
#else
  std::error_code ec;
  fs::rename(temporary, target, ec);
  if (ec) {
    error = "cannot replace " + target + ": " + ec.message();
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
#endif
  return true;
}

}  // namespace

std::string tls_base_dir(const server_config& config) {
  std::error_code ec;
  if (!config.config_path.empty()) {
    const fs::path absolute = fs::absolute(config.config_path, ec);
    if (!ec && !absolute.parent_path().empty()) {
      return absolute.parent_path().string();
    }
  }
  const fs::path current = fs::current_path(ec);
  return ec ? std::string(".") : current.string();
}

bool generate_self_signed(const std::string& cert_path,
                          const std::string& key_path,
                          const std::string& common_name,
                          const std::vector<std::string>& dns_names,
                          const std::vector<std::string>& ip_addresses,
                          std::string& error) {
  ctx_ptr context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                  EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0) {
    error = "cannot start RSA key generation: " + openssl_error();
    return false;
  }
  EVP_PKEY* raw_key = nullptr;
  if (EVP_PKEY_keygen(context.get(), &raw_key) <= 0) {
    error = "cannot generate the RSA key: " + openssl_error();
    return false;
  }
  pkey_ptr key(raw_key, EVP_PKEY_free);

  x509_ptr certificate(X509_new(), X509_free);
  if (!certificate) {
    error = "cannot allocate a certificate";
    return false;
  }
  X509_set_version(certificate.get(), 2);
  const std::uint64_t serial =
      static_cast<std::uint64_t>(std::time(nullptr)) ^
      reinterpret_cast<std::uintptr_t>(certificate.get());
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()),
                   static_cast<long>(serial & 0x7fffffff));
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -3600);
  // Ten years: a self-signed certificate is regenerated when it expires.
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()),
                  10LL * 365 * 24 * 3600);
  X509_set_pubkey(certificate.get(), key.get());

  // OpenSSL 4 returns a const name; filling in the subject of a certificate
  // that has not been signed yet is still the intended use.
  const X509_NAME* name = X509_get_subject_name(certificate.get());
  if (name == nullptr) {
    error = "cannot build the certificate subject";
    return false;
  }
  const std::string subject =
      common_name.empty() ? std::string("moLink Server") : common_name;
  if (X509_NAME_add_entry_by_txt(const_cast<X509_NAME*>(name), "CN",
                                 MBSTRING_UTF8,
                                 reinterpret_cast<const unsigned char*>(
                                     subject.c_str()),
                                 -1, -1, 0) != 1) {
    error = "cannot set the certificate subject";
    return false;
  }
  X509_set_issuer_name(certificate.get(), name);

  if (!add_extension(certificate.get(), NID_basic_constraints,
                     "critical,CA:TRUE") ||
      !add_extension(certificate.get(), NID_key_usage,
                     "critical,digitalSignature,keyEncipherment,keyCertSign") ||
      !add_extension(certificate.get(), NID_ext_key_usage, "serverAuth")) {
    error = "cannot add certificate extensions: " + openssl_error();
    return false;
  }
  const std::string san = san_string(dns_names, ip_addresses);
  if (!san.empty() &&
      !add_extension(certificate.get(), NID_subject_alt_name, san)) {
    error = "cannot add the subject alternative names: " + openssl_error();
    return false;
  }

  if (X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
    error = "cannot sign the certificate: " + openssl_error();
    return false;
  }

  const std::string cert_pem = pem_from_x509(certificate.get(), false, nullptr);
  const std::string key_pem = pem_from_x509(nullptr, true, key.get());
  if (cert_pem.empty() || key_pem.empty()) {
    error = "cannot encode the certificate: " + openssl_error();
    return false;
  }
  return write_pair(cert_path, key_path, cert_pem, key_pem, error);
}

bool reset_self_signed(const std::string& cert_path, const std::string& key_path,
                       std::string& error) {
  const std::string hostname = host_name();
  std::vector<std::string> dns{"localhost", hostname};
  std::sort(dns.begin(), dns.end());
  dns.erase(std::unique(dns.begin(), dns.end()), dns.end());
  return generate_self_signed(cert_path, key_path, hostname, dns,
                              local_ip_addresses(), error);
}

bool certificate_info_for(const std::string& cert_path, certificate_info& out,
                          std::string& error) {
  out = certificate_info{};
  if (cert_path.empty() || !is_regular_file(cert_path)) return true;
  x509_ptr certificate(nullptr, X509_free);
  if (!read_pem_certificate(cert_path, certificate, error)) return false;
  out.found = true;
  const X509_NAME* subject = X509_get_subject_name(certificate.get());
  const X509_NAME* issuer = X509_get_issuer_name(certificate.get());
  out.subject = name_common_name(subject);
  out.issuer = name_common_name(issuer);
  out.self_signed =
      subject != nullptr && issuer != nullptr &&
      X509_NAME_cmp(subject, issuer) == 0;
  out.not_before_ms = asn1_time_ms(X509_get0_notBefore(certificate.get()));
  out.not_after_ms = asn1_time_ms(X509_get0_notAfter(certificate.get()));

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  if (X509_digest(certificate.get(), EVP_sha256(), digest.data(),
                  &digest_length) == 1) {
    out.fingerprint_sha256 = hex_encode(digest.data(), digest_length);
  }

  GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(certificate.get(), NID_subject_alt_name, nullptr,
                       nullptr));
  if (names != nullptr) {
    for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
      const GENERAL_NAME* entry = sk_GENERAL_NAME_value(names, i);
      if (entry == nullptr) continue;
      if (entry->type == GEN_DNS) {
        unsigned char* utf8 = nullptr;
        const int length = ASN1_STRING_to_UTF8(&utf8, entry->d.dNSName);
        if (length > 0 && utf8 != nullptr) {
          out.dns_names.emplace_back(reinterpret_cast<char*>(utf8),
                                     static_cast<std::size_t>(length));
        }
        OPENSSL_free(utf8);
      } else if (entry->type == GEN_IPADD) {
        const ASN1_OCTET_STRING* ip = entry->d.iPAddress;
        std::array<char, INET6_ADDRSTRLEN> buffer{};
        const void* address = ASN1_STRING_get0_data(ip);
        const int length = ASN1_STRING_length(ip);
        if (address != nullptr &&
            (length == 4 || length == 16) &&
            inet_ntop(length == 4 ? AF_INET : AF_INET6, address, buffer.data(),
                      buffer.size()) != nullptr) {
          out.ip_addresses.emplace_back(buffer.data());
        }
      }
    }
    GENERAL_NAMES_free(names);
  }
  return true;
}

bool validate_certificate_pair(const std::string& cert_pem,
                               const std::string& key_pem,
                               std::string& error) {
  if (cert_pem.empty() || key_pem.empty()) {
    error = "certificate and key are required";
    return false;
  }
  bio_ptr cert_bio(BIO_new_mem_buf(cert_pem.data(),
                                   static_cast<int>(cert_pem.size())),
                   BIO_free);
  x509_ptr certificate(
      cert_bio ? PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr)
               : nullptr,
      X509_free);
  if (!certificate) {
    error = "the certificate is not valid PEM";
    return false;
  }
  bio_ptr key_bio(BIO_new_mem_buf(key_pem.data(),
                                  static_cast<int>(key_pem.size())),
                  BIO_free);
  pkey_ptr key(
      key_bio ? PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr)
              : nullptr,
      EVP_PKEY_free);
  if (!key) {
    error = "the private key is not valid PEM or is encrypted";
    return false;
  }
  if (X509_check_private_key(certificate.get(), key.get()) != 1) {
    error = "the certificate and private key do not match";
    return false;
  }
  return true;
}

bool write_certificate_pair(const std::string& cert_path,
                            const std::string& key_path,
                            const std::string& cert_pem,
                            const std::string& key_pem, std::string& error) {
  if (!validate_certificate_pair(cert_pem, key_pem, error)) return false;
  return write_pair(cert_path, key_path, cert_pem, key_pem, error);
}

bool prepare_tls(server_config& config, std::string& error) {
  // The admin console writes changes to <data_dir>/tls.json so it can work
  // with a read-only config file (system packages). The overlay wins over the
  // main config, environment and CLI.
  tls_update overlay;
  bool overlay_found = false;
  std::string overlay_error;
  if (!read_tls_overlay(config, overlay, overlay_found, overlay_error)) {
    error = overlay_error;
    return false;
  }
  if (overlay_found) {
    config.tls.enabled = overlay.enabled;
    config.tls.cert = overlay.cert;
    config.tls.key = overlay.key;
    config.tls.auto_self_signed = overlay.auto_self_signed;
    config.device.tls_cert.clear();
    config.device.tls_key.clear();
    config.admin.tls_cert.clear();
    config.admin.tls_key.clear();
  }

  if (!config.tls.enabled) {
    config.device.tls_cert.clear();
    config.device.tls_key.clear();
    config.admin.tls_cert.clear();
    config.admin.tls_key.clear();
    return true;
  }

  const fs::path base = tls_base_dir(config);
  auto resolve = [&base](const std::string& value) -> std::string {
    if (value.empty()) return {};
    const fs::path path(value);
    if (path.is_absolute()) return path.lexically_normal().string();
    return (base / path).lexically_normal().string();
  };

  // Legacy convention: <config>/tls or <data_dir>/tls with fullchain.pem and
  // privkey.pem. Only used when the default pair is absent, so existing
  // installs keep working without editing JSON.
  std::string legacy_cert =
      first_existing(base / "tls", {"fullchain.pem", "cert.pem"});
  std::string legacy_key =
      first_existing(base / "tls", {"privkey.pem", "key.pem"});
  if (legacy_cert.empty() || legacy_key.empty()) {
    const fs::path data_tls = fs::path(config.data_dir) / "tls";
    const std::string cert =
        first_existing(data_tls, {"fullchain.pem", "cert.pem"});
    const std::string key = first_existing(data_tls, {"privkey.pem", "key.pem"});
    if (!cert.empty() && !key.empty()) {
      legacy_cert = cert;
      legacy_key = key;
    }
  }
  if (!legacy_cert.empty() && !legacy_key.empty() &&
      !is_regular_file(resolve(config.tls.cert)) &&
      !is_regular_file(resolve(config.tls.key))) {
    config.tls.cert = legacy_cert;
    config.tls.key = legacy_key;
  }

  const bool device_override = !config.device.tls_cert.empty();
  const bool admin_override = !config.admin.tls_cert.empty();

  struct pair {
    std::string cert;
    std::string key;
  };
  pair device{resolve(device_override ? config.device.tls_cert : config.tls.cert),
              resolve(device_override ? config.device.tls_key : config.tls.key)};
  pair admin{resolve(admin_override ? config.admin.tls_cert : config.tls.cert),
             resolve(admin_override ? config.admin.tls_key : config.tls.key)};

  if (device.cert.empty() || device.key.empty() || admin.cert.empty() ||
      admin.key.empty()) {
    error = "TLS is enabled but no certificate path is configured";
    return false;
  }

  std::set<std::string> handled;
  auto ensure = [&](pair& target, bool allow_generate,
                    const char* listener) -> bool {
    const std::string id = target.cert + "\n" + target.key;
    if (handled.count(id) != 0) return true;
    const bool cert_exists = is_regular_file(target.cert);
    const bool key_exists = is_regular_file(target.key);
    if (cert_exists && key_exists) {
      if (!verify_pair_files(target.cert, target.key, error)) return false;
      handled.insert(id);
      return true;
    }
    if (cert_exists != key_exists) {
      error = std::string(listener) + ": the certificate and key must both "
              "exist (" + target.cert + ", " + target.key + ")";
      return false;
    }
    if (!allow_generate || !config.tls.auto_self_signed) {
      error = std::string(listener) + ": certificate not found: " +
              target.cert;
      return false;
    }
    const std::string hostname = host_name();
    std::vector<std::string> dns{"localhost", hostname};
    std::sort(dns.begin(), dns.end());
    dns.erase(std::unique(dns.begin(), dns.end()), dns.end());
    if (!generate_self_signed(target.cert, target.key, hostname, dns,
                              local_ip_addresses(), error)) {
      return false;
    }
    handled.insert(id);
    return true;
  };

  if (!ensure(device, !device_override, "device")) return false;
  if (!ensure(admin, !admin_override, "admin")) return false;

  config.device.tls_cert = device.cert;
  config.device.tls_key = device.key;
  config.admin.tls_cert = admin.cert;
  config.admin.tls_key = admin.key;
  return true;
}

std::string tls_settings_path(const server_config& config) {
  return (fs::path(config.data_dir) / "tls.json").string();
}

bool read_tls_overlay(const server_config& config, tls_update& out, bool& found,
                      std::string& error) {
  out = tls_update{};
  found = false;
  const std::string target = tls_settings_path(config);
  std::error_code ec;
  if (!fs::is_regular_file(target, ec)) return true;
  std::ifstream in(target, std::ios::binary);
  if (!in) {
    error = "cannot read " + target;
    return false;
  }
  nlohmann::json document;
  try {
    in >> document;
  } catch (const std::exception& e) {
    error = std::string("TLS settings are not valid JSON: ") + e.what();
    return false;
  }
  if (!document.is_object()) {
    error = "TLS settings must contain a JSON object";
    return false;
  }
  const nlohmann::json& tls =
      document.contains("tls") && document["tls"].is_object()
          ? document["tls"]
          : document;
  out.enabled = tls.value("enabled", out.enabled);
  out.cert = tls.value("cert", out.cert);
  out.key = tls.value("key", out.key);
  out.auto_self_signed = tls.value("auto_self_signed", out.auto_self_signed);
  found = true;
  return true;
}

bool save_tls_settings(const server_config& config, const tls_update& update,
                       std::string& error) {
  const fs::path directory(config.data_dir);
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec) {
    error = "cannot create " + directory.string() + ": " + ec.message();
    return false;
  }
  nlohmann::json document = nlohmann::json::object();
  document["enabled"] = update.enabled;
  document["cert"] = update.cert;
  document["key"] = update.key;
  document["auto_self_signed"] = update.auto_self_signed;
  const std::string text = document.dump(2) + "\n";
  return write_json_atomic(tls_settings_path(config), text, error);
}

}  // namespace molink
