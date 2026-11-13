// Remote execution: a worker and a client over localhost, with a fake runner (no container).
//   cmake -S core/test -B /tmp/rt && cmake --build /tmp/rt && /tmp/rt/remote_test
#include "remote.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace reelcut::remote;
namespace fs = std::filesystem;

static int fails = 0;
static void check(bool ok, const char* what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; }

template <class F> static void waitFor(F done, int ms = 5000)
{
    QEventLoop l; QTimer t; t.setInterval(20);
    QObject::connect(&t, &QTimer::timeout, &l, [&] { if (done()) l.quit(); });
    QTimer::singleShot(ms, &l, &QEventLoop::quit);
    t.start(); if (!done()) l.exec();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("argv checks\n");
    std::string e;
    check(checkArgv({"transcribe", "/work"}, e), "transcribe /work");
    check(checkArgv({"fetch", "https://youtu.be/x", "/work"}, e), "fetch a URL");
    check(!checkArgv({"fetch", "file:///etc/passwd", "/work"}, e), "fetch refuses file:");
    check(!checkArgv({"transcribe", "/etc"}, e), "a path outside /work");
    check(!checkArgv({"transcribe", "/work/../etc"}, e), "no ..");
    check(!checkArgv({"sh", "-c", "id"}, e), "no other commands");
    check(checkArgv({"cut", "/work", "/work/clips.txt", "/work/reels", "--only", "1,3", "--fix", "Corey=Cory", "--keep-fillers", "--intro", "/work/card-intro.png"}, e), "cut with flags");
    check(!checkArgv({"cut", "/work", "/work/clips.txt", "/work/reels", "--intro", "/home/x.png"}, e), "a card outside /work");
    check(!checkArgv({"cut", "/work", "/work/clips.txt", "/work/reels", "--evil", "1"}, e), "an unknown flag");
    check(!checkArgv({"diarize", "/work", "--speakers", "two"}, e), "a number that isn't");
    check(isMedia("video.mp4") && isMedia("audio.webm") && isMedia("x.part") && isMedia("p/0001.wav") && !isMedia("words.json") && !isMedia("reels/01-a.mp4"), "media");
    check(safeName("reels/01.mp4") && !safeName("../x") && !safeName("/etc/x") && !safeName("a//b"), "names");

    std::printf("pairing\n");
    const std::string key = newKey();
    const std::string code = pairingCode("box", {"http://127.0.0.1:1", "http://127.0.0.1:0"}, key);
    Pairing p;
    check(parsePairingCode(code, p, e) && p.key == key && p.name == "box" && p.urls.size() == 2 && p.id == pairId(key), "code round trip");
    check(!parsePairingCode("rcw1:xx", p, e), "a damaged code");

    std::printf("worker and client\n");
    QTemporaryDir root, local;
    int runs = 0;
    QStringList lastArgv;
    RunFn run = [&](const std::string& dir, const QStringList& argv, const std::string&, RunHooks h) -> std::function<void()> {
        ++runs; lastArgv = argv;
        auto stopped = std::make_shared<bool>(false);
        // a fake reelcut: writes files and prints like the real one
        QTimer::singleShot(50, [=] {
            if (*stopped) return;
            if (argv[0] == "transcribe") {
                std::ofstream(dir + "/words.json") << "[[0,1,\"hi\"]]";
                h.err("\r1/2"); h.err("\r2/2");
                h.out("done");
                h.done(true);
            } else if (argv[0] == "cut") {
                fs::create_directories(dir + "/reels");
                std::ofstream(dir + "/reels/01-a.mp4") << std::string(3 * 1024 * 1024, 'x');
                std::ofstream(dir + "/video.mp4") << "big";
                h.out("reels/01-a.mp4 12s");
                h.done(true);
            } else if (argv[0] == "prompt") {
                h.out("PROMPT TEXT"); h.done(true);
            } else if (argv[0] == "diarize") {
                // runs until stopped
            } else h.done(false);
        });
        return [=] { *stopped = true; h.done(false); };
    };
    WorkerServer srv(root.path().toStdString(), run);
    check(srv.listen(0).empty() && srv.port() > 0, "listens");
    Pairing wp; wp.key = key; wp.id = pairId(key); wp.name = "laptop";
    srv.setPairings({wp});
    srv.setInfo({{"name", "box"}, {"cores", 8}});

    Pairing cp = p;
    cp.urls = {"http://127.0.0.1:1", "http://127.0.0.1:" + std::to_string(srv.port())};
    WorkerClient cl(cp);
    bool helloOk = false, helloDone = false;
    cl.hello([&](bool ok, const json& j) { helloOk = ok && j.value("name", std::string()) == "box"; helloDone = true; });
    waitFor([&] { return helloDone; });
    check(helloOk && cl.online(), "hello finds the address that answers");

    Pairing bad = cp; bad.key = newKey(); bad.id = cp.id; bad.url = cl.pairing().url;
    WorkerClient intruder(bad);
    bool refused = false, rdone = false;
    intruder.hello([&](bool ok, const json&) { refused = !ok; rdone = true; });
    waitFor([&] { return rdone; });
    check(refused, "a wrong key is refused");

    std::vector<json> evs;
    cl.onEvent = [&](const json& ev) { evs.push_back(ev); };
    cl.listen();

    // upload a small file, run, follow the events
    const std::string clips = local.path().toStdString() + "/clips.txt";
    std::ofstream(clips) << "1 | 0:00:01 | 0:00:20 | A | hook | until x\n";
    bool up = false, upDone = false;
    cl.upload("job1", "clips.txt", clips, [&](bool ok, const std::string&) { up = ok; upDone = true; });
    waitFor([&] { return upDone; });
    check(up && fs::exists(root.path().toStdString() + "/" + cp.id + "/job1/clips.txt"), "a file goes up");

    bool sub = false, subDone = false;
    cl.submit({{"id", "t1"}, {"job", "job1"}, {"argv", {"transcribe", "/work"}}}, [&](bool ok, const json&) { sub = ok; subDone = true; });
    waitFor([&] { return subDone; });
    check(sub, "a task is taken");
    auto doneOf = [&](const std::string& id) { for (auto& e : evs) if (e.value("task", std::string()) == id && e.value("kind", std::string()) == "done") return true; return false; };
    waitFor([&] { return doneOf("t1"); });
    bool sawProgress = false;
    for (auto& e : evs) if (e.value("task", std::string()) == "t1" && e.value("kind", std::string()) == "err" && e.value("text", std::string()).find("2/2") != std::string::npos) sawProgress = true;
    check(doneOf("t1") && sawProgress, "events: progress and the end");

    json again; bool againDone = false;
    cl.submit({{"id", "t1"}, {"job", "job1"}, {"argv", {"transcribe", "/work"}}}, [&](bool ok, const json& j) { if (ok) again = j; againDone = true; });
    waitFor([&] { return againDone; });
    check(again.value("finished", false) && runs == 1, "the same task again: its result, not a second run");

    bool refusedTask = false, rtDone = false;
    cl.submit({{"id", "t2"}, {"job", "job1"}, {"argv", {"sh", "-c", "id"}}}, [&](bool ok, const json&) { refusedTask = !ok; rtDone = true; });
    waitFor([&] { return rtDone; });
    check(refusedTask, "a task outside the list is refused");

    // a cut: big file back, media listed but not fetched by the caller
    cl.submit({{"id", "t3"}, {"job", "job1"}, {"argv", {"cut", "/work", "/work/clips.txt", "/work/reels"}}}, [](bool, const json&) {});
    waitFor([&] { return doneOf("t3"); });
    json listing; bool lsDone = false;
    cl.files("job1", [&](bool ok, const json& j) { if (ok) listing = j; lsDone = true; });
    waitFor([&] { return lsDone; });
    bool hasReel = false, hasVideo = false;
    for (auto& f : listing["files"]) { if (f["name"] == "reels/01-a.mp4") hasReel = true; if (f["name"] == "video.mp4") hasVideo = true; }
    check(hasReel && hasVideo, "the job's files are listed");
    const std::string dst = local.path().toStdString() + "/reels/01-a.mp4";
    bool dl = false, dlDone = false;
    cl.download("job1", "reels/01-a.mp4", dst, [&](bool ok, const std::string&) { dl = ok; dlDone = true; });
    waitFor([&] { return dlDone; }, 10000);
    check(dl && fs::file_size(dst) == 3u * 1024 * 1024, "a 3 MB reel comes back whole");

    // an empty file both ways, and what travels is not the plaintext
    const std::string empty = local.path().toStdString() + "/empty.txt";
    std::ofstream(empty) << "";
    bool eu = false, euDone = false;
    cl.upload("job1", "empty.txt", empty, [&](bool ok, const std::string&) { eu = ok; euDone = true; });
    waitFor([&] { return euDone; });
    bool ed = false, edDone = false;
    cl.download("job1", "empty.txt", local.path().toStdString() + "/back.txt", [&](bool ok, const std::string&) { ed = ok; edDone = true; });
    waitFor([&] { return edDone; });
    check(eu && ed && fs::file_size(local.path().toStdString() + "/back.txt") == 0, "an empty file goes up and comes back");
    {
        std::string n1;
        const QByteArray f = seal(key, "req|" + cp.id, "{\"m\":\"GET\",\"p\":\"/v1/hello\",\"t\":1}", &n1);
        check(!f.contains("hello") && !f.contains("/v1/"), "a sealed request doesn't show its path");
        QByteArray buf = f; std::string plain;
        check(openFrame(key, "req|" + cp.id, buf, plain) == 1 && plain.find("/v1/hello") != std::string::npos, "and opens with the key");
        QByteArray b2 = f; std::string p2;
        check(openFrame(newKey(), "req|" + cp.id, b2, p2) == -1, "not with another key");
        StreamWriter w(key, "ad"); QByteArray st = w.header() + w.push("secret reel bytes", true);
        check(!st.contains("secret reel"), "a file stream doesn't show its bytes");
        StreamReader rd(key, "ad"); QByteArray out;
        check(rd.feed(st, out) && rd.done() && out == "secret reel bytes", "and decrypts whole");
        StreamWriter w2(key, "ad"); QByteArray st2 = w2.header() + w2.push("x", true); st2[st2.size() - 3] ^= 1;
        StreamReader rd2(key, "ad"); QByteArray out2;
        check(!rd2.feed(st2, out2), "a changed byte is caught");
        StreamWriter w3(key, "ad-a"); QByteArray st3 = w3.header() + w3.push("x", true);
        StreamReader rd3(key, "ad-b"); QByteArray out3;
        check(!rd3.feed(st3, out3), "a stream for another request is refused");
    }

    // cancel
    cl.submit({{"id", "t4"}, {"job", "job1"}, {"argv", {"diarize", "/work"}}}, [](bool, const json&) {});
    waitFor([&] { return runs >= 3; });
    cl.cancel("t4");
    waitFor([&] { return doneOf("t4"); });
    check(doneOf("t4"), "cancel ends the task");

    // the synchronous call
    std::string err;
    const std::string out = cl.runSync("job1", {"prompt", "/work"}, 5000, err);
    check(out.find("PROMPT TEXT") != std::string::npos, "runSync returns the output");

    bool gone = false;
    cl.forget("job1");
    waitFor([&] { gone = !fs::exists(root.path().toStdString() + "/" + cp.id + "/job1"); return gone; });
    check(gone, "forgetting a job removes its folder");

    // the worker starts again (a task of ours running there): the client hears it
    WorkerClient fresh(cl.pairing());   // a client that has had no event reply yet
    std::vector<json> fevs;
    fresh.onEvent = [&](const json& ev) { fevs.push_back(ev); };
    bool fh = false; fresh.hello([&](bool, const json&) { fh = true; }); waitFor([&] { return fh; });
    fresh.listen();
    cl.submit({{"id", "t9"}, {"job", "job2"}, {"argv", {"diarize", "/work"}}}, [](bool, const json&) {});
    waitFor([&] { return runs >= 5; });
    const int port = srv.port();
    auto srv2 = std::make_unique<WorkerServer>(root.path().toStdString(), run);
    srv.close();
    waitFor([&] { return srv2->listen(port).empty(); }, 3000);
    srv2->setPairings({wp});
    bool resync = false;
    waitFor([&] { for (auto& e : evs) if (e.value("kind", std::string()) == "resync") resync = true; return resync; }, 45000);
    check(resync, "a restarted worker is noticed (its boot id changed)");
    bool fresync = false;
    waitFor([&] { for (auto& e : fevs) if (e.value("kind", std::string()) == "resync") fresync = true; return fresync; }, 45000);
    check(fresync, "...also by a client whose first long poll hadn't returned yet");

    std::printf(fails ? "REMOTE FAILED (%d)\n" : "REMOTE GREEN\n", fails);
    return fails ? 1 : 0;
}
