#pragma once

// Posts for the two Logos forums, written the way each one's own client writes them, so their
// readers take Reelcut's replies like any other: Forumer (github.com/bristinWild/forumer, wire v1
// on /forumer/3/<forum>/proto) and Logos Forum (github.com/edenbd1/lp-0026-forum, post v1 on
// /logos-forum/1/<slug>-<hash>/json). Both sign with Ed25519; Reelcut has one key of its own,
// shown under an alias. Only what posting a reply (or a new thread) needs, and reading threads
// back for the picker: no sync, no local post log.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace reelcut::forum {

// Reelcut's signing key: made once, kept in `path` (owner-only).
struct Key {
    std::vector<uint8_t> pk, sk;   // 32 and 64 bytes (libsodium crypto_sign)
    bool ok() const { return pk.size() == 32 && sk.size() == 64; }
};
Key loadOrCreateKey(const std::string& path, std::string* error = nullptr);

// A signed post: the bytes to send on `topic`, and the post's id.
struct Signed {
    std::string topic, wire, id, error;
    bool ok() const { return error.empty(); }
};

// One thread, as the picker shows it.
struct Thread {
    std::string id, title, author;
    int64_t ts = 0;
};

// ── Forumer
std::string forumerTopic(const std::string& forum = "public");
// A reply directly under a thread's top post (root = parent = the thread's id).
Signed forumerReply(const Key& key, const std::string& forum, const std::string& thread,
                    const std::string& body, const std::string& alias, int64_t tsMs, int powBits = 18);
// A new thread: a title, a body, 1..3 domains (topic tags).
Signed forumerThread(const Key& key, const std::string& forum, const std::string& title,
                     const std::string& body, const std::vector<std::string>& domains,
                     const std::string& alias, int64_t tsMs, int powBits = 18);
// A thread from a message on the topic ("post" messages of kind post, signature checked).
std::optional<Thread> forumerThreadOf(const std::string& payload);
// "basecamp://intent/forumer.open?p=…" (a thread link from Forumer's Share) or a bare 32-hex id.
std::optional<std::string> forumerThreadFromLink(const std::string& text);
std::vector<std::string> forumerDomains(const std::vector<std::string>& raw);

// ── Logos Forum
std::string logosForumTopic(const std::string& forumName = "Logos Forum");
Signed logosForumReply(const Key& key, const std::string& forumName, const std::string& topicId,
                       const std::string& body, const std::string& alias, int64_t tsMs);
Signed logosForumThread(const Key& key, const std::string& forumName, const std::string& title,
                        const std::string& body, const std::string& alias, int64_t tsMs);
// A thread (a "topic" post of that forum) from a message on the topic, signature checked.
std::optional<Thread> logosForumThreadOf(const std::string& payload, const std::string& forumName);

// The logos.dev store nodes Logos Forum asks for history, and the pubsub topic (autosharding)
// a content topic lives on there.
std::vector<std::string> storeNodes();
std::string pubsubTopic(const std::string& contentTopic, int cluster = 3, int shards = 8);

} // namespace reelcut::forum
