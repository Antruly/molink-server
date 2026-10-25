#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace molink {

/// Metadata about the certificate currently used by the listeners.
struct certificate_info {
  bool found = false;
  bool self_signed = false;
  std::string subject;
  std::string issuer;
  std::uint64_t not_before_ms = 0;
  std::uint64_t not_after_ms = 0;
  std::vector<std::string> dns_names;
  std::vector<std::string> ip_addresses;
  std::string fingerprint_sha256;
};

/// The directory relative TLS paths are resolved against: the directory that
/// holds the config file, or the process working directory when the server was
/// started without one.
std::string tls_base_dir(const server_config& config);

/// Resolve the TLS material for both listeners, generate a self-signed pair on
/// first start when the default paths do not exist yet, and validate that the
/// certificate and key belong together. On success `config.device` and
/// `config.admin` carry absolute paths (or are cleared when TLS is off).
bool prepare_tls(server_config& config, std::string& error);

/// Generate a self-signed RSA certificate/key pair. Parent directories are
/// created; the key is written with owner-only permissions on POSIX.
bool generate_self_signed(const std::string& cert_path,
                          const std::string& key_path,
                          const std::string& common_name,
                          const std::vector<std::string>& dns_names,
                          const std::vector<std::string>& ip_addresses,
                          std::string& error);

/// Replace the pair at @p cert_path / @p key_path with a new self-signed
/// certificate covering this host's name and addresses.
bool reset_self_signed(const std::string& cert_path, const std::string& key_path,
                       std::string& error);

/// Parse a PEM certificate and describe it.
bool certificate_info_for(const std::string& cert_path, certificate_info& out,
                          std::string& error);

/// Validate a PEM certificate/key pair supplied by the admin console.
bool validate_certificate_pair(const std::string& cert_pem,
                               const std::string& key_pem,
                               std::string& error);

/// Persist an uploaded certificate pair to disk.
bool write_certificate_pair(const std::string& cert_path,
                            const std::string& key_path,
                            const std::string& cert_pem,
                            const std::string& key_pem, std::string& error);

/// The TLS values the admin console can change. They only take effect after a
/// restart.
struct tls_update {
  bool enabled = true;
  std::string cert = "cer/server.crt";
  std::string key = "cer/server.key";
  bool auto_self_signed = true;
};

/// Where the admin console stores TLS changes. It lives in the data directory
/// so the service account can write it even when the main config file is
/// root-owned (system packages).
std::string tls_settings_path(const server_config& config);

/// Load the overlay written by the admin console. `found` is false when the
/// file does not exist; it takes precedence over the main config file.
bool read_tls_overlay(const server_config& config, tls_update& out, bool& found,
                      std::string& error);

/// Write the overlay (creating the data directory if needed). The main config
/// file is not modified.
bool save_tls_settings(const server_config& config, const tls_update& update,
                       std::string& error);

}  // namespace molink
