#include "forum_post.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

namespace reelcut::forum {
namespace {

using json = nlohmann::json;
using Bytes = std::vector<uint8_t>;

bool sodiumReady()
{
    static const bool ok = sodium_init() >= 0;
    return ok;
}

std::string hex(const uint8_t* d, size_t n)
{
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s.push_back(k[d[i] >> 4]); s.push_back(k[d[i] & 15]); }
    return s;
}
bool isLowerHex(const std::string& s, size_t len)
{
    return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string b64(const uint8_t* d, size_t n, int variant)
{
    std::string out(sodium_base64_encoded_len(n, variant), '\0');
    sodium_bin2base64(out.data(), out.size(), d, n, variant);
    out.resize(out.find('\0'));
    return out;
}
std::optional<Bytes> unb64(const std::string& s, int variant)
{
    Bytes out(s.size() + 3);
    size_t len = 0;
    if (sodium_base642bin(out.data(), out.size(), s.data(), s.size(), nullptr, &len, nullptr, variant) != 0) return std::nullopt;
    out.resize(len);
    return out;
}
Bytes bytesOf(const std::string& s) { return Bytes(s.begin(), s.end()); }
Bytes cat(Bytes a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
Bytes blake2b(const Bytes& in, size_t outLen)
{
    Bytes h(outLen);
    crypto_generichash(h.data(), outLen, in.data(), in.size(), nullptr, 0);
    return h;
}
int leadingZeroBits(const Bytes& h)
{
    int bits = 0;
    for (uint8_t b : h) {
        if (b == 0) { bits += 8; continue; }
        for (int i = 7; i >= 0 && !((b >> i) & 1); --i) ++bits;
        break;
    }
    return bits;
}

// ── Forumer (lib/forumer_core/src/post.cpp): canonical = the signed fields as JSON, keys sorted, no
// whitespace (nlohmann's own dump); work on BLAKE2b-256(canonical || nonce); sig = Ed25519 over
// canonical || nonce; id = hex BLAKE2b-128(canonical || nonce || sig).
constexpr int kForumerVersion = 1;
constexpr size_t kForumerNonce = 8;
constexpr size_t kForumerMaxEncoded = 16 * 1024;

json forumerCanonical(const std::string& kind, const std::string& forum, const std::string& root,
                      const std::string& parent, const std::string& title, const std::string& body,
                      const std::vector<std::string>& domains, int64_t ts, const Bytes& pk,
                      const std::string& alias, int pow)
{
    return json{{"v", kForumerVersion}, {"kind", kind}, {"forum", forum}, {"root", root}, {"parent", parent},
                {"title", title}, {"body", body}, {"domains", domains}, {"media", json::array()},
                {"ts", ts}, {"pk", b64(pk.data(), pk.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING)},
                {"disc", alias.empty() ? "persona" : "alias"}, {"alias", alias}, {"fp", true}, {"pow", pow}};
}

Signed forumerSign(const Key& key, const std::string& forum, json doc, int pow)
{
    Signed s;
    s.topic = forumerTopic(forum);
    if (!sodiumReady() || !key.ok()) { s.error = "no signing key"; return s; }
    if (s.topic.empty()) { s.error = "not a Forumer forum name: " + forum; return s; }
    Bytes canonical;
    try { canonical = bytesOf(doc.dump()); } catch (const json::exception&) { s.error = "text that is not UTF-8"; return s; }
    Bytes nonce(kForumerNonce);
    randombytes_buf(nonce.data(), nonce.size());
    while (leadingZeroBits(blake2b(cat(canonical, nonce), 32)) < pow)
        for (size_t i = 0; i < nonce.size() && ++nonce[i] == 0; ++i) {}
    const Bytes msg = cat(canonical, nonce);
    Bytes sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, msg.data(), msg.size(), key.sk.data());
    const Bytes idh = blake2b(cat(msg, sig), 16);
    s.id = hex(idh.data(), idh.size());
    doc["nonce"] = b64(nonce.data(), nonce.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    doc["sig"] = b64(sig.data(), sig.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    doc["id"] = s.id;
    const std::string env = doc.dump();
    if (env.size() > kForumerMaxEncoded) { s.error = "the post is too long for Forumer (16 KiB)"; return s; }
    // sync.cpp: {"v":1,"t":"post","env":"<the signed envelope JSON>"}
    s.wire = json{{"v", 1}, {"t", "post"}, {"env", env}}.dump();
    return s;
}

// Forumer's alias rules (post.cpp aliasProblem), the part Reelcut's alias could break.
std::string forumerAliasProblem(const std::string& a)
{
    if (a.empty()) return "";
    if (a.size() > 32) return "an alias can be at most 32 bytes";
    if (a.front() == ' ' || a.back() == ' ') return "an alias can't start or end with a space";
    for (unsigned char c : a) if (c < 0x20 || c == 0x7f) return "an alias can't hold control characters";
    return "";
}

// ── Logos Forum (core/src/post.cpp): signing bytes = length-prefixed (u32 LE) fields in a fixed
// order, then the raw author key; sig = Ed25519 over them; id = hex SHA-256 of them.
constexpr size_t kLfMaxBody = 20000, kLfMaxTitle = 200, kLfMaxAlias = 40, kLfMaxForum = 80;

void putU32(Bytes& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); }
void putU64(Bytes& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(uint8_t(v >> (8 * i))); }
void putStr(Bytes& o, const std::string& s) { putU32(o, uint32_t(s.size())); o.insert(o.end(), s.begin(), s.end()); }

Bytes lfSigning(const std::string& kind, const std::string& forum, const std::string& topicId,
                const std::string& title, const std::string& body, const std::string& mode,
                const std::string& alias, uint64_t ts, const Bytes& author)
{
    Bytes o;
    putStr(o, "logos-forum:post:v1");
    putU32(o, 1);
    putStr(o, kind); putStr(o, forum); putStr(o, topicId); putStr(o, title); putStr(o, body);
    putStr(o, mode); putStr(o, alias);
    putU64(o, ts);
    o.insert(o.end(), author.begin(), author.end());
    return o;
}

Signed lfSign(const Key& key, const std::string& kind, const std::string& forum, const std::string& topicId,
              const std::string& title, const std::string& body, const std::string& alias, int64_t ts)
{
    Signed s;
    s.topic = logosForumTopic(forum);
    if (!sodiumReady() || !key.ok()) { s.error = "no signing key"; return s; }
    if (forum.empty() || forum.size() > kLfMaxForum) { s.error = "not a Logos Forum name: " + forum; return s; }
    if (title.size() > kLfMaxTitle || body.size() > kLfMaxBody || alias.size() > kLfMaxAlias) { s.error = "too long for Logos Forum"; return s; }
    const std::string mode = alias.empty() ? "id" : "alias";
    const Bytes sb = lfSigning(kind, forum, topicId, title, body, mode, alias, uint64_t(ts), key.pk);
    Bytes sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, sb.data(), sb.size(), key.sk.data());
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, sb.data(), sb.size());
    s.id = hex(h, sizeof h);
    json j = {{"v", 1}, {"kind", kind}, {"forum", forum}, {"body", body}, {"mode", mode}, {"ts", ts},
              {"author", b64(key.pk.data(), key.pk.size(), sodium_base64_VARIANT_ORIGINAL)},
              {"sig", b64(sig.data(), sig.size(), sodium_base64_VARIANT_ORIGINAL)}};
    if (kind == "topic") j["title"] = title;
    else j["topic"] = topicId;
    if (mode == "alias") j["alias"] = alias;
    try { s.wire = j.dump(); } catch (const json::exception&) { s.error = "text that is not UTF-8"; }
    return s;
}

std::string str(const json& j, const char* k)
{
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
int64_t num(const json& j, const char* k)
{
    auto it = j.find(k);
    return it != j.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}

} // namespace

Key loadOrCreateKey(const std::string& path, std::string* error)
{
    Key k;
    if (!sodiumReady()) { if (error) *error = "libsodium did not start"; return k; }
    {
        std::ifstream in(path, std::ios::binary);
        Bytes seed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (seed.size() == crypto_sign_SEEDBYTES) {
            k.pk.resize(crypto_sign_PUBLICKEYBYTES); k.sk.resize(crypto_sign_SECRETKEYBYTES);
            crypto_sign_seed_keypair(k.pk.data(), k.sk.data(), seed.data());
            sodium_memzero(seed.data(), seed.size());
            return k;
        }
    }
    Bytes seed(crypto_sign_SEEDBYTES);
    randombytes_buf(seed.data(), seed.size());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(seed.data()), std::streamsize(seed.size()));
        if (!out) { if (error) *error = "could not save the forum key in " + path; return Key(); }
    }
    ::chmod(path.c_str(), 0600);
    k.pk.resize(crypto_sign_PUBLICKEYBYTES); k.sk.resize(crypto_sign_SECRETKEYBYTES);
    crypto_sign_seed_keypair(k.pk.data(), k.sk.data(), seed.data());
    sodium_memzero(seed.data(), seed.size());
    return k;
}

// ── Forumer
std::string forumerTopic(const std::string& forum)
{
    // sync.cpp contentTopic: a lowercase name of [a-z0-9-], 1..64.
    if (forum.empty() || forum.size() > 64) return "";
    for (char c : forum) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return "";
    return "/forumer/3/" + forum + "/proto";
}

std::vector<std::string> forumerDomains(const std::vector<std::string>& raw)
{
    std::vector<std::string> out;
    for (const auto& tag : raw) {
        std::string clean;
        bool dash = false;
        for (char ch : tag) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (std::isspace(c) || ch == '-' || ch == '_') dash = !clean.empty();
            else if (c < 128 && std::isalnum(c)) { if (dash) clean.push_back('-'); dash = false; clean.push_back(char(std::tolower(c))); }
        }
        if (clean.size() < 2 || clean.size() > 32) continue;
        if (std::find(out.begin(), out.end(), clean) != out.end()) continue;
        out.push_back(clean);
        if (out.size() == 3) break;
    }
    return out;
}

Signed forumerReply(const Key& key, const std::string& forum, const std::string& thread,
                    const std::string& body, const std::string& alias, int64_t tsMs, int powBits)
{
    Signed s;
    if (!isLowerHex(thread, 32)) { s.error = "not a Forumer thread id"; return s; }
    if (body.empty() || body.size() > 10000) { s.error = "a Forumer reply holds 1 to 10000 bytes"; return s; }
    if (auto p = forumerAliasProblem(alias); !p.empty()) { s.error = p; return s; }
    return forumerSign(key, forum, forumerCanonical("reply", forum, thread, thread, "", body, {}, tsMs, key.pk, alias, powBits), powBits);
}

Signed forumerThread(const Key& key, const std::string& forum, const std::string& title,
                     const std::string& body, const std::vector<std::string>& domains,
                     const std::string& alias, int64_t tsMs, int powBits)
{
    Signed s;
    const auto d = forumerDomains(domains);
    if (title.empty() || title.size() > 200) { s.error = "a Forumer thread needs a title of 1 to 200 bytes"; return s; }
    if (body.size() > 10000) { s.error = "a Forumer post holds at most 10000 bytes"; return s; }
    if (d.empty()) { s.error = "a Forumer thread needs a domain (a topic tag)"; return s; }
    if (auto p = forumerAliasProblem(alias); !p.empty()) { s.error = p; return s; }
    return forumerSign(key, forum, forumerCanonical("post", forum, "", "", title, body, d, tsMs, key.pk, alias, powBits), powBits);
}

std::optional<Thread> forumerThreadOf(const std::string& payload)
{
    if (!sodiumReady()) return std::nullopt;
    const json m = json::parse(payload, nullptr, false);
    if (!m.is_object() || str(m, "t") != "post" || num(m, "v") != 1) return std::nullopt;
    const json e = json::parse(str(m, "env"), nullptr, false);
    if (!e.is_object() || str(e, "kind") != "post" || num(e, "v") != kForumerVersion) return std::nullopt;
    try {
        auto pk = unb64(str(e, "pk"), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
        auto nonce = unb64(str(e, "nonce"), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
        auto sig = unb64(str(e, "sig"), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
        if (!pk || !nonce || !sig || pk->size() != 32 || sig->size() != crypto_sign_BYTES) return std::nullopt;
        json c = e;
        c.erase("nonce"); c.erase("sig"); c.erase("id");
        const Bytes msg = cat(bytesOf(c.dump()), *nonce);
        if (crypto_sign_verify_detached(sig->data(), msg.data(), msg.size(), pk->data()) != 0) return std::nullopt;
        const Bytes idh = blake2b(cat(msg, *sig), 16);
        Thread t;
        t.id = hex(idh.data(), idh.size());
        if (t.id != str(e, "id")) return std::nullopt;
        t.title = str(e, "title");
        t.ts = num(e, "ts");
        const std::string alias = str(e, "alias");
        t.author = str(e, "disc") == "anonymous" ? "Anonymous" : alias.empty() ? "a persona" : alias;
        return t;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> forumerThreadFromLink(const std::string& text)
{
    std::string t = text;
    t.erase(0, t.find_first_not_of(" \t\r\n"));
    t.erase(t.find_last_not_of(" \t\r\n") + 1);
    if (isLowerHex(t, 32)) return t;
    const std::string prefix = "basecamp://intent/forumer.open?p=";
    if (t.rfind(prefix, 0) != 0 || !sodiumReady()) return std::nullopt;
    std::string p = t.substr(prefix.size());
    if (auto amp = p.find('&'); amp != std::string::npos) p.resize(amp);
    while (!p.empty() && p.back() == '=') p.pop_back();
    std::replace(p.begin(), p.end(), '+', '-');
    std::replace(p.begin(), p.end(), '/', '_');
    auto raw = unb64(p, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    if (!raw) return std::nullopt;
    const json j = json::parse(std::string(raw->begin(), raw->end()), nullptr, false);
    const std::string topic = j.is_object() ? str(j, "topic") : "";
    if (!isLowerHex(topic, 32)) return std::nullopt;
    return topic;
}

// ── Logos Forum
std::string logosForumTopic(const std::string& forum)
{
    if (!sodiumReady()) return "";
    std::string slug;
    for (char c : forum) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) slug.push_back(char(std::tolower(u)));
        else if (!slug.empty() && slug.back() != '-') slug.push_back('-');
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, reinterpret_cast<const uint8_t*>(forum.data()), forum.size());
    return "/logos-forum/1/" + (slug.empty() ? std::string("forum") : slug) + "-" + hex(h, 4) + "/json";
}

Signed logosForumReply(const Key& key, const std::string& forumName, const std::string& topicId,
                       const std::string& body, const std::string& alias, int64_t tsMs)
{
    if (!isLowerHex(topicId, 64)) { Signed s; s.error = "not a Logos Forum thread id"; return s; }
    return lfSign(key, "reply", forumName, topicId, "", body, alias, tsMs);
}

Signed logosForumThread(const Key& key, const std::string& forumName, const std::string& title,
                        const std::string& body, const std::string& alias, int64_t tsMs)
{
    if (title.empty()) { Signed s; s.error = "a Logos Forum thread needs a title"; return s; }
    return lfSign(key, "topic", forumName, "", title, body, alias, tsMs);
}

std::optional<Thread> logosForumThreadOf(const std::string& payload, const std::string& forumName)
{
    if (!sodiumReady()) return std::nullopt;
    const json j = json::parse(payload, nullptr, false);
    if (!j.is_object() || num(j, "v") != 1 || str(j, "kind") != "topic" || str(j, "forum") != forumName) return std::nullopt;
    auto author = unb64(str(j, "author"), sodium_base64_VARIANT_ORIGINAL);
    auto sig = unb64(str(j, "sig"), sodium_base64_VARIANT_ORIGINAL);
    if (!author || !sig || author->size() != 32 || sig->size() != crypto_sign_BYTES) return std::nullopt;
    const std::string title = str(j, "title"), mode = str(j, "mode"), alias = str(j, "alias");
    if (title.empty()) return std::nullopt;
    const int64_t ts = num(j, "ts");
    const Bytes sb = lfSigning("topic", forumName, "", title, str(j, "body"), mode, alias, uint64_t(ts), *author);
    if (crypto_sign_verify_detached(sig->data(), sb.data(), sb.size(), author->data()) != 0) return std::nullopt;
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, sb.data(), sb.size());
    Thread t;
    t.id = hex(h, sizeof h);
    t.title = title;
    t.ts = ts;
    t.author = mode == "anon" ? "Anonymous" : !alias.empty() ? alias : "@" + hex(author->data(), 4);
    return t;
}

std::vector<std::string> storeNodes()
{
    // core's DeliveryTransport::default_store_peers("logos.dev") in Logos Forum: the dev fleet, cluster 3.
    return {
        "/dns4/delivery-01.do-ams3.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAmTUbnxLGT9JvV6mu9oPyDjqHK4Phs1VDJNUgESgNSkuby",
        "/dns4/delivery-02.do-ams3.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAmMK7PYygBtKUQ8EHp7EfaD3bCEsJrkFooK8RQ2PVpJprH",
        "/dns4/delivery-01.gc-us-central1-a.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAm4S1JYkuzDKLKQvwgAhZKs9otxXqt8SCGtB4hoJP1S397",
        "/dns4/delivery-02.gc-us-central1-a.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAm8Y9kgBNtjxvCnf1X6gnZJW5EGE4UwwCL3CCm55TwqBiH",
        "/dns4/delivery-01.ac-cn-hongkong-c.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAm8YokiNun9BkeA1ZRmhLbtNUvcwRr64F69tYj9fkGyuEP",
        "/dns4/delivery-02.ac-cn-hongkong-c.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAkvwhGHKNry6LACrB8TmEFoCJKEX29XR5dDUzk3UT3UNSE",
    };
}

std::string pubsubTopic(const std::string& ct, int cluster, int shards)
{
    // /app/version/name/encoding: the shard is SHA-256(app + version), last 8 bytes, mod shards.
    std::vector<std::string> parts;
    size_t i = 1;
    while (i <= ct.size()) {
        const size_t j = ct.find('/', i);
        parts.push_back(ct.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (parts.size() < 2 || shards <= 0 || !sodiumReady()) return "";
    const std::string b = parts[0] + parts[1];
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, reinterpret_cast<const uint8_t*>(b.data()), b.size());
    uint64_t v = 0;
    for (int k = 24; k < 32; ++k) v = (v << 8) | h[k];
    return "/waku/2/rs/" + std::to_string(cluster) + "/" + std::to_string(v % uint64_t(shards));
}

} // namespace reelcut::forum
