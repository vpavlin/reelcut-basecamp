// Reelcut's forum posts, checked by each forum's own library.
#include "forum_post.h"
#include "forumer_core/post.h"
#include "forumer_core/sync.h"
#include "forum/post.h"
#include "forum/engine.h"
#include <cstdio>
#include <string>
#include <vector>
namespace rf = reelcut::forum;
static int fails = 0;
#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL %s\n", what); ++fails; } else std::printf("ok   %s\n", what); } while (0)

static std::optional<forumer::post::Post> forumerDecode(const std::string& wire) {
    auto r = forumer::sync::decode(std::vector<uint8_t>(wire.begin(), wire.end()));
    if (!r.message || !r.message->post) return std::nullopt;
    return r.message->post;
}

int main() {
    std::string err;
    rf::Key key = rf::loadOrCreateKey("forumtest.key", &err);
    CHECK(key.ok(), "key made and loaded");
    rf::Key again = rf::loadOrCreateKey("forumtest.key", &err);
    CHECK(again.pk == key.pk, "the same key the second time");
    const int64_t ts = 1792300000000;

    // ── Forumer
    CHECK(rf::forumerTopic("public") == forumer::sync::contentTopic("public"), "Forumer content topic = forumer's");
    auto th = rf::forumerThread(key, "public", "DevClub #12: Corey Petty on neutrality", "Full video: https://example.com/v",
                                {"Dev Club", "video"}, "Reelcut", ts);
    CHECK(th.ok(), "Forumer thread signed");
    auto tp = forumerDecode(th.wire);
    CHECK(tp.has_value(), "forumer decodes the thread message");
    if (tp) {
        CHECK(forumer::post::verify(*tp, 18) == forumer::post::Error::None, "forumer verifies the thread (pow 18)");
        CHECK(tp->id == th.id, "thread id = forumer's");
        CHECK(tp->authorDisplay().rfind("Reelcut · fr:", 0) == 0, "shown as 'Reelcut · fr:…'");
        CHECK(tp->content.domains == std::vector<std::string>({"dev-club", "video"}), "domains normalised as forumer does");
    }
    auto t = rf::forumerThreadOf(th.wire);
    CHECK(t && t->id == th.id && t->title == "DevClub #12: Corey Petty on neutrality", "the picker reads the thread back");
    auto rp = rf::forumerReply(key, "public", th.id, "**Why neutrality matters**\n\nCID zDvZ…\n#Shorts", "Reelcut", ts + 1000);
    CHECK(rp.ok(), "Forumer reply signed");
    auto rpp = forumerDecode(rp.wire);
    CHECK(rpp && forumer::post::verify(*rpp, 16) == forumer::post::Error::None, "forumer verifies the reply");
    CHECK(rpp && rpp->content.root == th.id && rpp->content.parent == th.id && rpp->id == rp.id, "the reply sits under the thread");
    CHECK(!rf::forumerThreadOf(rp.wire), "a reply is not listed as a thread");
    auto unicode = rf::forumerReply(key, "public", th.id, "Žluťoučký kůň 🐴", "", ts);
    auto up = forumerDecode(unicode.wire);
    CHECK(up && forumer::post::verify(*up, 16) == forumer::post::Error::None && up->authorDisplay().rfind("fr:", 0) == 0, "non-ASCII body, persona disclosure");
    CHECK(!rf::forumerReply(key, "public", "nothex", "x", "", ts).ok(), "a bad thread id is refused");
    // a thread link as Forumer's links.js writes it: base64url of {"topic":…,"day":…}
    {
        std::string json = "{\"topic\":\"" + th.id + "\",\"day\":20744}";
        // base64url (no padding) by hand
        static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string o;
        for (size_t i = 0; i < json.size(); i += 3) {
            unsigned n = (unsigned char)json[i] << 16 | (i + 1 < json.size() ? (unsigned char)json[i + 1] << 8 : 0) | (i + 2 < json.size() ? (unsigned char)json[i + 2] : 0);
            o += A[(n >> 18) & 63]; o += A[(n >> 12) & 63];
            if (i + 1 < json.size()) o += A[(n >> 6) & 63];
            if (i + 2 < json.size()) o += A[n & 63];
        }
        auto id = rf::forumerThreadFromLink("basecamp://intent/forumer.open?p=" + o);
        CHECK(id && *id == th.id, "a Forumer thread link gives the thread id");
        CHECK(rf::forumerThreadFromLink(th.id) == th.id, "a bare id works too");
        CHECK(!rf::forumerThreadFromLink("https://example.com"), "anything else doesn't");
    }

    // ── Logos Forum
    const std::string F = "Logos Forum";
    CHECK(rf::logosForumTopic(F) == forum::content_topic(F), "Logos Forum content topic = its own");
    CHECK(rf::pubsubTopic(rf::logosForumTopic(F), 3) == forum::pubsub_topic(forum::content_topic(F), 3), "pubsub shard = its own");
    auto lt = rf::logosForumThread(key, F, "DevClub #12", "Full video here", "Reelcut", ts);
    CHECK(lt.ok(), "Logos Forum thread signed");
    forum::Check why = forum::Check::Ok;
    auto ltp = forum::decode(lt.wire, &why);
    CHECK(ltp.has_value(), "logos forum decodes and verifies the thread");
    CHECK(ltp && ltp->id() == lt.id, "thread id = its own");
    auto lt2 = rf::logosForumThreadOf(lt.wire, F);
    CHECK(lt2 && lt2->id == lt.id && lt2->title == "DevClub #12" && lt2->author == "Reelcut", "the picker reads it back");
    auto lr = rf::logosForumReply(key, F, lt.id, "**Why neutrality matters** 🐴\nCID zDv…", "Reelcut", ts + 5);
    auto lrp = forum::decode(lr.wire, &why);
    CHECK(lrp && lrp->topic_id == lt.id && lrp->id() == lr.id && lrp->kind == forum::Kind::Reply, "logos forum verifies the reply under the thread");
    auto lr2 = rf::logosForumReply(key, F, lt.id, "no alias", "", ts);
    CHECK(forum::decode(lr2.wire, &why).has_value(), "identity mode works too");
    CHECK(!rf::logosForumThreadOf(lr.wire, F), "a reply is not listed as a thread");
    CHECK(!rf::logosForumThreadOf(lt.wire, "Other Forum"), "another forum's thread is not listed");

    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
