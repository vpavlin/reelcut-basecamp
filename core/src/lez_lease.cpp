#include "lez_lease.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace reelcut::lease {

static const char* kB58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

std::string base58(const std::string& raw)
{
    std::vector<unsigned char> in(raw.begin(), raw.end());
    size_t zeros = 0;
    while (zeros < in.size() && in[zeros] == 0) ++zeros;
    std::vector<unsigned char> b58((in.size() - zeros) * 138 / 100 + 1, 0);
    size_t len = 0;
    for (size_t i = zeros; i < in.size(); ++i) {
        int carry = in[i];
        size_t j = 0;
        for (auto it = b58.rbegin(); (carry != 0 || j < len) && it != b58.rend(); ++it, ++j) {
            carry += 256 * (*it);
            *it = (unsigned char)(carry % 58);
            carry /= 58;
        }
        len = j;
    }
    auto it = b58.begin() + (b58.size() - len);
    while (it != b58.end() && *it == 0) ++it;
    std::string out(zeros, '1');
    for (; it != b58.end(); ++it) out.push_back(kB58[*it]);
    return out;
}

bool unbase58(const std::string& s, std::string& raw32)
{
    std::vector<unsigned char> out;
    size_t zeros = 0;
    while (zeros < s.size() && s[zeros] == '1') ++zeros;
    std::vector<unsigned char> b256(s.size() * 733 / 1000 + 1, 0);
    size_t len = 0;
    for (size_t i = zeros; i < s.size(); ++i) {
        const char* p = std::strchr(kB58, s[i]);
        if (!p || !s[i]) return false;
        int carry = (int)(p - kB58);
        size_t j = 0;
        for (auto it = b256.rbegin(); (carry != 0 || j < len) && it != b256.rend(); ++it, ++j) {
            carry += 58 * (*it);
            *it = (unsigned char)(carry % 256);
            carry /= 256;
        }
        len = j;
    }
    auto it = b256.begin() + (b256.size() - len);
    std::string r(zeros, '\0');
    for (; it != b256.end(); ++it) r.push_back((char)*it);
    if (r.size() != 32) return false;
    raw32 = r;
    return true;
}

std::string toHex(const std::string& raw)
{
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : raw) { o.push_back(d[c >> 4]); o.push_back(d[c & 15]); }
    return o;
}

std::string sha256(const std::string& data)
{
    unsigned char h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, (const unsigned char*)data.data(), data.size());
    return std::string((const char*)h, sizeof h);
}

std::string leaseAccount(const std::string& program, const std::string& worker, const std::string& clientKey)
{
    const std::string seed = sha256(std::string("reelcut-lease/v1") + worker + clientKey);
    std::string prefix("/LEE/v0.2/AccountId/PDA/", 24);
    prefix.append(8, '\0');
    return sha256(prefix + program + seed);
}

namespace {
struct Rd {
    const std::string& s; size_t i = 0; bool ok = true;
    std::string take(size_t n) { if (i + n > s.size()) { ok = false; return std::string(n, '\0'); } std::string r = s.substr(i, n); i += n; return r; }
    uint64_t u64() { std::string b = take(8); uint64_t v = 0; for (int k = 7; k >= 0; --k) v = (v << 8) | (unsigned char)b[k]; return v; }
    uint32_t u32() { std::string b = take(4); uint32_t v = 0; for (int k = 3; k >= 0; --k) v = (v << 8) | (unsigned char)b[k]; return v; }
};
void le(std::string& o, uint64_t v, int n) { for (int k = 0; k < n; ++k) { o.push_back((char)(v & 0xff)); v >>= 8; } }
} // namespace

std::optional<Offer> parseOffer(const std::string& st)
{
    Rd r{st};
    Offer o;
    o.treasury = r.take(32);
    o.priceLo = r.u64(); o.priceHi = r.u64();
    o.periodMs = r.u64(); o.maxPeriods = r.u32();
    o.open = r.take(1)[0] != 0;
    o.rentals = r.u64(); o.periodsSold = r.u64();
    if (!r.ok || r.i != st.size()) return std::nullopt;
    return o;
}

std::optional<Lease> parseLease(const std::string& st)
{
    Rd r{st};
    Lease l;
    l.worker = r.take(32); l.clientKey = r.take(32); l.payer = r.take(32);
    l.paidThroughMs = r.u64();
    if (!r.ok || r.i != st.size()) return std::nullopt;
    return l;
}

std::string setOfferMessage(const std::string& treasury, uint64_t price, uint64_t periodMs, uint32_t maxPeriods, bool open)
{
    std::string m(1, '\0');               // Message::SetOffer
    m += treasury;
    le(m, price, 8); le(m, 0, 8);         // u128
    le(m, periodMs, 8); le(m, maxPeriods, 4);
    m.push_back(open ? 1 : 0);
    return m;
}

std::string rentMessage(const std::string& clientKey, const std::string& payer, uint32_t periods, uint64_t nowMs)
{
    std::string m(1, '\1');               // Message::Rent
    m += clientKey; m += payer;
    le(m, periods, 4); le(m, nowMs, 8);
    return m;
}

std::string sharedKey(const std::string& mySecret, const std::string& myPublic, const std::string& theirPublic)
{
    if (sodium_init() < 0 || mySecret.size() != 32 || theirPublic.size() != 32) return "";
    unsigned char q[crypto_scalarmult_BYTES];
    if (crypto_scalarmult(q, (const unsigned char*)mySecret.data(), (const unsigned char*)theirPublic.data()) != 0) return "";
    // both sides hash the same bytes: the secret, then the two public keys in a fixed order
    const std::string a = std::min(myPublic, theirPublic), b = std::max(myPublic, theirPublic);
    unsigned char k[32];
    crypto_generichash_state st;
    crypto_generichash_init(&st, (const unsigned char*)"reelcut-market-v1", 17, sizeof k);
    crypto_generichash_update(&st, q, sizeof q);
    crypto_generichash_update(&st, (const unsigned char*)a.data(), a.size());
    crypto_generichash_update(&st, (const unsigned char*)b.data(), b.size());
    crypto_generichash_final(&st, k, sizeof k);
    sodium_memzero(q, sizeof q);
    return std::string((const char*)k, sizeof k);
}

bool newKeypair(std::string& secret, std::string& pub)
{
    if (sodium_init() < 0) return false;
    unsigned char pk[crypto_box_PUBLICKEYBYTES], sk[crypto_box_SECRETKEYBYTES];
    crypto_box_keypair(pk, sk);
    secret.assign((const char*)sk, sizeof sk);
    pub.assign((const char*)pk, sizeof pk);
    return true;
}

} // namespace reelcut::lease
