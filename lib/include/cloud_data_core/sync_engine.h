#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cloud_data_core/crdt.h"

/// Pure protocol logic for the delivery_module bridge (see PLAN.md "Sync
/// engine"): content-topic derivation and the op wire format. Free
/// functions with no state and, like the rest of cloud_data_core, no
/// dependency on the Logos SDK — CloudDataEngine calls into these to build
/// topics and encode/decode payloads, then hands the result to its
/// Transport (see transport.h), whose module-side implementation is what
/// actually reaches delivery_module.
///
/// Content-topic bucketing follows the privacy guidance in
/// logos-docs "About content topics": don't put a raw doc_id in the topic
/// string (that would let any peer subscribed to Filter/Store/Light-Push
/// link a specific document to an IP), so instead every doc in a collection
/// is bucketed into one of a small number of shared topics by hashing its
/// doc_id and using the leading bits as the bucket. The collectionId itself
/// is used verbatim as the topic's "content-topic-name" segment (the
/// per-app-feature name, analogous to the docs' `/supercrypto/1/notification/proto`
/// example) since it identifies a schema/feature, not a specific record.
///
/// The hash used for bucketing is a plain FNV-1a: it must be identical
/// across every peer and every platform this module ever runs on (so two
/// peers agree on which topic a given doc_id belongs to), which rules out
/// std::hash (implementation-defined, not guaranteed stable across standard
/// library versions/vendors).
namespace cloud_data_core::sync_engine {

/// Number of leading bytes of the doc_id hash used as the bucket, encoded
/// as hex in the topic string. 1 byte (2 hex chars, 256 buckets) matches
/// the granularity used in the logos-docs bucketing example.
constexpr int kDefaultBucketBytes = 1;

/// LIP-23 content topic for a collection/doc-id bucket:
/// `/{appName}/{version}/{collectionId}-{bucketHex}/proto`.
///
/// `bucketBytes == 0` disables bucketing entirely: the doc-derived segment
/// is dropped and the whole collection shares `/{appName}/{version}/{collectionId}/proto`.
/// That is *stronger* on the privacy axis this bucketing exists for — the
/// topic then carries nothing derived from any doc_id at all — and what it
/// trades away instead is scope: every subscriber to the collection receives
/// every doc's ops. Right for a broadcast-shaped app (a forum, where every
/// peer wants every post) and wrong for the private-per-document case, which
/// is why kDefaultBucketBytes stays 1.
std::string contentTopicForDoc(const std::string& appName, int version,
                                const std::string& collectionId, const std::string& docId,
                                int bucketBytes = kDefaultBucketBytes);

/// Control topic used to publish "latest snapshot CID" pointer messages for
/// a collection (see PLAN.md "Durability / bootstrap bridge"):
/// `/{appName}/{version}/{collectionId}/snapshot-ptr/proto`.
std::string snapshotPointerTopic(const std::string& appName, int version, const std::string& collectionId);

/// True if `contentTopic` is a snapshot-pointer control topic (as produced
/// by snapshotPointerTopic), false if it's a regular doc-bucket content
/// topic (as produced by contentTopicForDoc). Lets a caller route an
/// inbound transport message by topic shape without needing to know the
/// topic grammar itself — snapshot pointers and ops share one inbound
/// event on delivery_module's side.
bool isSnapshotPointerTopic(const std::string& contentTopic);

/// Wire-encodes one CrdtOp (plus the collectionId it belongs to, which
/// doesn't travel inside CrdtOp itself) as UTF-8 JSON bytes for
/// delivery_module.send(). Every op is self-contained and idempotent by
/// opId — see PLAN.md "no stateful per-peer sync handshake".
///
/// Note: v1 carries no signature/authenticity proof over the op. Anyone who
/// can subscribe to the bucket topic can publish a forged op for any
/// doc_id in that bucket; this is a real gap (distinct from the payload
/// *confidentiality* gap PLAN.md already flags as deferred) that a future
/// LEZ-backed signing layer would need to close before this module is used
/// for anything where write-authenticity matters.
std::vector<uint8_t> encodeOp(const std::string& collectionId, const CrdtOp& op);

/// Reverses encodeOp. Returns false (leaving outputs untouched) if `bytes`
/// isn't well-formed — e.g. sent by an incompatible version or a malformed
/// peer — so callers can drop it rather than crash on attacker-controlled
/// network input.
bool decodeOp(const std::vector<uint8_t>& bytes, std::string& collectionId, CrdtOp& op);

} // namespace cloud_data_core::sync_engine
