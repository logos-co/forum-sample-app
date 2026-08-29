#pragma once

#include <string>

/// Minimal RFC 4648 base64 decoder, shared by both module adapters.
///
/// Both of this module's dependencies base64-encode payloads on the wire:
/// storage_module's downloadChunks() delivers each chunk independently
/// encoded (see storage_module_plugin.h's storageDownloadProgress payload),
/// and delivery_module's storeQuery() returns message payloads encoded the
/// same way. Decoding each chunk on arrival and concatenating raw bytes is
/// correct regardless of where chunk boundaries fall, whereas concatenating
/// the base64 text first would break if an interior chunk's encoding happens
/// to include padding.
///
/// Lives in src/ rather than lib/ deliberately: this is wire-format
/// knowledge about two specific Logos modules, not something
/// cloud_data_core should carry (the library's Transport/BlobStore
/// interfaces are raw bytes in, raw bytes out).
std::string base64Decode(const std::string& input);
