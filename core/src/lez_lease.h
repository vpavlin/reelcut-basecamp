#pragma once

// The reelcut_lease program on LEZ v0.4 (../../lez/reelcut_lease): account ids, the lease account of a
// client key at a worker, the messages (borsh) and the states, so Reelcut can rent and check leases.
// Account ids: 32 bytes; shown and sent to the sequencer's JSON-RPC in base58, to lez_core in hex.

#include <cstdint>
#include <optional>
#include <string>

namespace reelcut::lease {

std::string base58(const std::string& raw32);
bool unbase58(const std::string& s, std::string& raw32);   // false: not a 32-byte base58 id
std::string toHex(const std::string& raw);
std::string sha256(const std::string& data);

// for_public_pda(program, sha256("reelcut-lease/v1" ‖ worker ‖ client_key))
std::string leaseAccount(const std::string& program, const std::string& worker, const std::string& clientKey);

struct Offer {
    std::string treasury;
    uint64_t priceLo = 0, priceHi = 0;   // u128
    uint64_t periodMs = 0;
    uint32_t maxPeriods = 0;
    bool open = false;
    uint64_t rentals = 0, periodsSold = 0;
};
struct Lease {
    std::string worker, clientKey, payer;
    uint64_t paidThroughMs = 0;
};
std::optional<Offer> parseOffer(const std::string& state);
std::optional<Lease> parseLease(const std::string& state);

std::string setOfferMessage(const std::string& treasury, uint64_t price, uint64_t periodMs, uint32_t maxPeriods, bool open);
std::string rentMessage(const std::string& clientKey, const std::string& payer, uint32_t periods, uint64_t nowMs);

// The two keys' shared secret as a 32-byte HMAC key for the worker link (X25519, then BLAKE2b with both
// public keys). "" when the keys are bad.
std::string sharedKey(const std::string& mySecret, const std::string& myPublic, const std::string& theirPublic);
bool newKeypair(std::string& secret, std::string& pub);

} // namespace reelcut::lease
