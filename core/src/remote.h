#pragma once

// Remote execution (docs/remote-execution.md, step 1): the heavy reelcut steps run on another computer
// you own, a *worker*. Basecamp keeps the jobs and the screens; the worker runs reelcut commands from a
// fixed list in a folder per job and hands the files back.
//
// Step 1's link is HTTP over the LAN or the mesh: the worker listens (WorkerServer), Basecamp calls it
// (WorkerClient). Every request is signed with the pairing key (HMAC-SHA256); the worker serves only
// paired clients. The messages are the ones a Logos Delivery link will carry for step 2.
//
//   GET  /v1/hello                       name, version, cores, what runs
//   POST /v1/task      {id, job, argv}   run `reelcut argv…` in the job's folder (argv checked); the same
//                                        id again: attaches, or {finished} when it already ended well
//   GET  /v1/events?since=N&wait=S       what the tasks printed, and how they ended (long poll)
//   POST /v1/cancel    {id}
//   GET  /v1/files?job=J                 the job folder: [{name, size, mtime}]
//   GET  /v1/file?job=J&name=N           a file;  PUT the same to write one, DELETE to remove it
//   DELETE /v1/job?job=J                 forget a job and its folder

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class QTcpServer;
class QTcpSocket;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace reelcut::remote {

using json = nlohmann::json;

// ── pairing ──────────────────────────────────────────────────────────────────────────────────
struct Pairing {
    std::string id;                 // 16 hex chars, derived from the key
    std::string name;               // the worker's name (on the client), the client's label (on the worker)
    std::string key;                // 32 raw bytes
    std::vector<std::string> urls;  // client side: where the worker may be reached ("http://host:port")
    std::string url;                // client side: the one that answered last
    long long tasksUntil = 0;       // worker side, a market client: no new steps after this (ms; 0 = no limit)
};
std::string newKey();                                   // 32 random bytes
std::string pairId(const std::string& key);
// "rcw1:" + base64url({"n": name, "u": [urls], "k": base64url(key)})
std::string pairingCode(const std::string& name, const std::vector<std::string>& urls, const std::string& key);
bool parsePairingCode(const std::string& code, Pairing& out, std::string& error);
json pairingJson(const Pairing& p, bool withKey);
bool pairingFrom(const json& j, Pairing& out);

// ── what may run ─────────────────────────────────────────────────────────────────────────────
// A reelcut command from the fixed list, its flags from that command's own list, every path in
// /work and without "..". The worker runs it as argv in the container: no shell anywhere.
bool checkArgv(const QStringList& argv, std::string& error);

// Media stay on the worker: audio.*, video.* and anything half-written.
bool isMedia(const std::string& name);
// A name inside a job's folder: relative, no "..", no leading dot directories beyond ".size".
bool safeName(const std::string& name);

// ── signing ──────────────────────────────────────────────────────────────────────────────────
std::string sign(const std::string& key, const std::string& method, const std::string& pathQuery,
                 const std::string& ts, const std::string& bodySha);
std::string sha256Hex(const QByteArray& b);

// ── the worker ───────────────────────────────────────────────────────────────────────────────
// How the worker runs a command: the module's container runner. Returns a stop function.
struct RunHooks {
    std::function<void(const std::string& line)> out;   // stdout, a line at a time
    std::function<void(const std::string& chunk)> err;  // stderr as it comes ("\r3/8" progress included)
    std::function<void(bool ok)> done;
};
using RunFn = std::function<std::function<void()>(const std::string& dir, const QStringList& argv,
                                                   const std::string& name, RunHooks hooks)>;

class WorkerServer : public QObject {
public:
    WorkerServer(std::string root, RunFn run);
    ~WorkerServer() override;
    // Listen on port (all addresses). "" when it does, else why not.
    std::string listen(int port);
    void close();
    bool listening() const;
    int port() const { return m_port; }
    void setPairings(const std::vector<Pairing>& p);
    void setInfo(const json& info) { m_info = info; }   // name, version, cores: what /v1/hello says
    json status() const;                                // for the snapshot: tasks running, clients seen
    long long lastSeen(const std::string& pair) const { auto it = m_seen.find(pair); return it == m_seen.end() ? 0 : it->second; }
    std::function<void()> onChange;
    // A client that isn't paired: a market client ("m-" + its public key in hex). Fill `out` (id, key,
    // name) and return 1 to serve it, 0 when its lease is being checked (asked to try again in a moment),
    // -1 to refuse it (`why`).
    std::function<int(const std::string& pairId, Pairing& out, std::string& why)> resolve;

private:
    struct Conn;
    struct Task {
        std::string id, pair, job, dir;
        QStringList argv;
        bool running = true, ok = false;
        std::function<void()> stop;
        long long started = 0, ended = 0;
    };
    struct Event { long long seq; std::string pair; json body; };

    void onConnection();
    void onData(QTcpSocket* s);
    void handle(QTcpSocket* s, Conn& c);
    void reply(QTcpSocket* s, int code, const json& body);
    void replyFile(QTcpSocket* s, const std::string& path);
    bool auth(Conn& c, std::string& pair, std::string& error);
    std::string jobDir(const std::string& pair, const std::string& job) const;
    void push(const std::string& pair, json ev);
    void flushWaiters();
    json tasksJson(const std::string& pair) const;

    std::string m_root;
    RunFn m_run;
    QTcpServer* m_srv = nullptr;
    int m_port = 0;
    std::map<std::string, Pairing> m_pairs;   // id -> pairing
    std::map<std::string, Task> m_tasks;
    std::vector<Event> m_events;
    long long m_seq = 0, m_lastFlushed = 0;
    bool m_flushArmed = false;
    std::map<QTcpSocket*, std::unique_ptr<Conn>> m_conns;
    std::map<std::string, long long> m_seen;  // pair -> last request (ms)
    std::set<std::string> m_sigs;             // signatures seen lately (replay)
    json m_info = json::object();
    QTimer* m_tick = nullptr;
    std::string m_boot;
};

// ── the client ───────────────────────────────────────────────────────────────────────────────
class WorkerClient : public QObject {
public:
    explicit WorkerClient(Pairing p);
    ~WorkerClient() override;
    const Pairing& pairing() const { return m_p; }
    void setPairing(const Pairing& p) { m_p = p; }

    // Find a URL that answers (the last good one first). cb(ok, hello or error).
    void hello(std::function<void(bool, const json&)> cb);
    void submit(const json& task, std::function<void(bool, const json&)> cb);
    void cancel(const std::string& taskId);
    void files(const std::string& job, std::function<void(bool, const json&)> cb);
    void download(const std::string& job, const std::string& name, const std::string& path,
                  std::function<void(bool, const std::string& error)> cb);
    void upload(const std::string& job, const std::string& name, const std::string& path,
                std::function<void(bool, const std::string& error)> cb);
    void remove(const std::string& job, const std::string& name, std::function<void(bool)> cb);
    void forget(const std::string& job);
    // Blocking (a local event loop), for the few synchronous calls: argv's output, or "" and error.
    std::string runSync(const std::string& job, const QStringList& argv, int timeoutMs, std::string& error);

    // The event stream: started once; each event goes to onEvent.
    void listen();
    std::function<void(const json& ev)> onEvent;
    std::function<void(bool online, const std::string& why)> onOnline;
    bool online() const { return m_online; }
    json lastHello() const { return m_hello; }
    std::string boot() const { return m_boot; }

private:
    QNetworkReply* request(const std::string& method, const std::string& pathQuery, const QByteArray& body,
                           QIODevice* bodyDevice = nullptr);
    void poll();
    bool noteBoot(const std::string& boot, const json& tasks);   // true: the worker restarted
    void setOnline(bool on, const std::string& why);

    Pairing m_p;
    QNetworkAccessManager* m_nam = nullptr;
    long long m_since = 0;
    bool m_listening = false, m_online = false;
    QPointer<QNetworkReply> m_poll;
    json m_hello = json::object();
    int m_failures = 0;
    std::string m_boot;   // the worker's run we follow
};

} // namespace reelcut::remote
