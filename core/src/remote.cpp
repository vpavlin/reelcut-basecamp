#include "remote.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <sodium.h>

#include <filesystem>
#include <map>

namespace fs = std::filesystem;

namespace reelcut::remote {

static long long nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

static std::string b64url(const std::string& raw)
{
    return QByteArray::fromStdString(raw).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals).toStdString();
}
static std::string unb64url(const std::string& s)
{
    return QByteArray::fromBase64(QByteArray::fromStdString(s), QByteArray::Base64UrlEncoding).toStdString();
}
static std::string hex(const unsigned char* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string o; o.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { o.push_back(d[p[i] >> 4]); o.push_back(d[p[i] & 15]); }
    return o;
}

// ── pairing ──────────────────────────────────────────────────────────────────────────────────
std::string newKey()
{
    if (sodium_init() < 0) return std::string();
    std::string k(32, '\0');
    randombytes_buf(k.data(), k.size());
    return k;
}
std::string pairId(const std::string& key)
{
    unsigned char h[32];
    crypto_generichash(h, sizeof h, (const unsigned char*)key.data(), key.size(),
                       (const unsigned char*)"reelcut-worker-pair", 19);
    return hex(h, 8);
}
std::string pairingCode(const std::string& name, const std::vector<std::string>& urls, const std::string& key)
{
    json j = {{"n", name}, {"u", urls}, {"k", b64url(key)}};
    return "rcw1:" + b64url(j.dump());
}
bool parsePairingCode(const std::string& codeIn, Pairing& out, std::string& error)
{
    std::string code = codeIn;
    while (!code.empty() && std::isspace((unsigned char)code.back())) code.pop_back();
    while (!code.empty() && std::isspace((unsigned char)code.front())) code.erase(0, 1);
    if (code.rfind("rcw1:", 0) != 0) { error = "Not a worker pairing code (it starts with rcw1:)."; return false; }
    json j = json::parse(unb64url(code.substr(5)), nullptr, false);
    if (!j.is_object() || !j.contains("k") || !j["k"].is_string()) { error = "The pairing code is damaged."; return false; }
    out.key = unb64url(j["k"].get<std::string>());
    if (out.key.size() != 32) { error = "The pairing code is damaged."; return false; }
    out.id = pairId(out.key);
    out.name = j.value("n", std::string("worker"));
    if (out.name.empty()) out.name = "worker";
    if (out.name.size() > 40) out.name.resize(40);
    out.urls.clear();
    if (j.contains("u") && j["u"].is_array())
        for (auto& u : j["u"]) if (u.is_string() && u.get<std::string>().rfind("http://", 0) == 0 && out.urls.size() < 12)
            out.urls.push_back(u.get<std::string>());
    if (out.urls.empty()) { error = "The pairing code has no address of the worker."; return false; }
    return true;
}
json pairingJson(const Pairing& p, bool withKey)
{
    json j = {{"id", p.id}, {"name", p.name}, {"urls", p.urls}, {"url", p.url}};
    if (withKey) j["key"] = b64url(p.key);
    return j;
}
bool pairingFrom(const json& j, Pairing& out)
{
    if (!j.is_object() || !j.contains("key") || !j["key"].is_string()) return false;
    out.key = unb64url(j["key"].get<std::string>());
    if (out.key.size() != 32) return false;
    out.id = pairId(out.key);
    out.name = j.value("name", std::string("worker"));
    out.url = j.value("url", std::string());
    out.urls.clear();
    if (j.contains("urls") && j["urls"].is_array())
        for (auto& u : j["urls"]) if (u.is_string()) out.urls.push_back(u.get<std::string>());
    return true;
}

// ── what may run ─────────────────────────────────────────────────────────────────────────────
namespace {
enum class V { None, Text, Num, Path };
struct Cmd { int positional; std::vector<int> pathPositions; std::map<std::string, V> flags; };
const std::map<std::string, Cmd>& commands()
{
    static const std::map<std::string, Cmd> c = {
        {"fetch", {2, {1}, {{"--only", V::Text}}}},
        {"transcribe", {1, {0}, {{"--threads", V::Num}, {"--jobs", V::Num}}}},
        {"diarize", {1, {0}, {{"--speakers", V::Num}, {"--threshold", V::Num}, {"--threads", V::Num}}}},
        {"prompt", {1, {0}, {{"-n", V::Num}, {"--brief", V::Text}, {"--brief-file", V::Path}, {"--min", V::Num}, {"--max", V::Num}}}},
        {"cut", {3, {0, 1, 2}, {{"--only", V::Text}, {"--lead", V::Num}, {"--tail", V::Num}, {"--fade", V::Num},
                               {"--intro", V::Path}, {"--outro", V::Path}, {"--card", V::Num}, {"--fix", V::Text},
                               {"--keep-fillers", V::None}, {"--speakers", V::Text}, {"--frame", V::Text},
                               {"--colour", V::Text}, {"--color", V::Text}, {"--saying-colour", V::Text},
                               {"--saying-color", V::Text}, {"--said-colour", V::Text}, {"--said-color", V::Text},
                               {"--caption-margin", V::Num}, {"--hook-margin", V::Num}, {"--ratio", V::Text},
                               {"--framing", V::Path}}}},
        {"scenes", {3, {0, 1, 2}, {{"--only", V::Text}, {"--lead", V::Num}, {"--tail", V::Num},
                                  {"--threshold", V::Num}, {"--least", V::Num}}}},
        {"probe", {1, {0}, {{"--at", V::Num}}}},
        {"thumbs", {1, {0}, {}}},
        {"page", {1, {0}, {{"--title", V::Text}}}},
    };
    return c;
}
bool workPath(const QString& p)
{
    if (p != "/work" && !p.startsWith("/work/")) return false;
    for (auto& part : p.split('/')) if (part == "..") return false;
    return !p.contains('\0');
}
bool number(const QString& s)
{
    bool ok = false;
    s.toDouble(&ok);
    return ok;
}
} // namespace

bool checkArgv(const QStringList& argv, std::string& error)
{
    if (argv.isEmpty()) { error = "empty command"; return false; }
    if (argv.size() > 200) { error = "too many arguments"; return false; }
    auto it = commands().find(argv[0].toStdString());
    if (it == commands().end()) { error = "not a reelcut command this worker runs: " + argv[0].toStdString(); return false; }
    const Cmd& c = it->second;
    std::vector<QString> pos;
    for (int i = 1; i < argv.size(); ++i) {
        const QString& a = argv[i];
        if (a.size() > 4096) { error = "an argument is too long"; return false; }
        if (a.startsWith("-") && !(a.size() > 1 && a[1].isDigit())) {
            auto f = c.flags.find(a.toStdString());
            if (f == c.flags.end()) { error = "flag not allowed: " + a.toStdString(); return false; }
            if (f->second == V::None) continue;
            if (i + 1 >= argv.size()) { error = "flag without a value: " + a.toStdString(); return false; }
            const QString& v = argv[++i];
            if (f->second == V::Num && !number(v)) { error = "not a number for " + a.toStdString(); return false; }
            if (f->second == V::Path && !workPath(v)) { error = "a path outside the job: " + v.toStdString(); return false; }
            continue;
        }
        pos.push_back(a);
    }
    if ((int)pos.size() != c.positional) { error = "wrong number of arguments for " + argv[0].toStdString(); return false; }
    for (int i : c.pathPositions)
        if (!workPath(pos[i])) { error = "a path outside the job: " + pos[i].toStdString(); return false; }
    if (argv[0] == "fetch" && !(pos[0].startsWith("https://") || pos[0].startsWith("http://"))) {
        error = "fetch takes an http(s) address"; return false;
    }
    return true;
}

bool isMedia(const std::string& name)
{
    // transcribe's pieces (p/*.wav and their raw output): working files, they stay
    if (name.rfind("p/", 0) == 0) return true;
    const std::string base = fs::path(name).filename().string();
    if (base.rfind("audio.", 0) == 0 || base.rfind("video.", 0) == 0) return true;
    for (const char* e : {".part", ".ytdl", ".temp", ".tmp"})
        if (base.size() > strlen(e) && base.compare(base.size() - strlen(e), strlen(e), e) == 0) return true;
    return false;
}
bool safeName(const std::string& name)
{
    if (name.empty() || name.size() > 300 || name[0] == '/') return false;
    for (char ch : name) if (ch == '\0' || ch == '\\') return false;
    for (auto& part : QString::fromStdString(name).split('/'))
        if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}

// ── signing ──────────────────────────────────────────────────────────────────────────────────
std::string sha256Hex(const QByteArray& b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex().toStdString();
}
std::string sign(const std::string& key, const std::string& method, const std::string& pathQuery,
                 const std::string& ts, const std::string& bodySha)
{
    const std::string msg = method + "\n" + pathQuery + "\n" + ts + "\n" + bodySha;
    unsigned char mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, (const unsigned char*)key.data(), key.size());
    crypto_auth_hmacsha256_update(&st, (const unsigned char*)msg.data(), msg.size());
    crypto_auth_hmacsha256_final(&st, mac);
    return hex(mac, sizeof mac);
}

// ── the worker ───────────────────────────────────────────────────────────────────────────────
struct WorkerServer::Conn {
    QByteArray buf;
    bool headersDone = false;
    QString method, path, target;   // target: the request line's path and query, as signed
    QUrlQuery query;
    std::map<std::string, std::string> headers;
    qint64 length = 0;
    // PUT /v1/file streams to a file
    std::unique_ptr<QFile> sink;
    std::string sinkPath, sinkFinal, sinkSha, pair;
    qint64 got = 0;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    // GET /v1/file streams from one
    std::unique_ptr<QFile> source;
    bool retry = false;   // 503 instead of 403: a market client's lease is being checked
    // a long poll
    bool waiting = false;
    std::string waitPair;
    long long since = 0, waitUntil = 0;
};

WorkerServer::WorkerServer(std::string root, RunFn run) : m_root(std::move(root)), m_run(std::move(run))
{
    // A new id at every start: a client that sees it change knows the tasks it waits for are gone.
    unsigned char b[8];
    randombytes_buf(b, sizeof b);
    m_boot = hex(b, sizeof b);
    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    QObject::connect(m_tick, &QTimer::timeout, this, [this] { flushWaiters(); });
    m_tick->start();
}
WorkerServer::~WorkerServer()
{
    for (auto& [id, t] : m_tasks) if (t.running && t.stop) t.stop();
    close();
}
std::string WorkerServer::listen(int port)
{
    close();
    m_srv = new QTcpServer(this);
    if (!m_srv->listen(QHostAddress::Any, (quint16)port)) {
        std::string e = m_srv->errorString().toStdString();
        delete m_srv; m_srv = nullptr;
        return "Could not listen on port " + std::to_string(port) + ": " + e;
    }
    m_port = m_srv->serverPort();
    QObject::connect(m_srv, &QTcpServer::newConnection, this, [this] { onConnection(); });
    return "";
}
void WorkerServer::close()
{
    if (m_srv) { m_srv->close(); m_srv->deleteLater(); m_srv = nullptr; }
}
bool WorkerServer::listening() const { return m_srv && m_srv->isListening(); }
void WorkerServer::setPairings(const std::vector<Pairing>& p)
{
    m_pairs.clear();
    for (auto& x : p) m_pairs[x.id] = x;
}
json WorkerServer::status() const
{
    json tasks = json::array();
    for (auto& [id, t] : m_tasks)
        if (t.running) tasks.push_back({{"id", id}, {"job", t.job}, {"cmd", t.argv.isEmpty() ? "" : t.argv[0].toStdString()},
                                        {"client", m_pairs.count(t.pair) ? m_pairs.at(t.pair).name : t.pair}, {"since", t.started}});
    json seen = json::object();
    for (auto& [p, ms] : m_seen) seen[m_pairs.count(p) ? m_pairs.at(p).name : p] = ms;
    return {{"listening", listening()}, {"port", m_port}, {"tasks", tasks}, {"seen", seen}};
}

void WorkerServer::onConnection()
{
    while (m_srv && m_srv->hasPendingConnections()) {
        QTcpSocket* s = m_srv->nextPendingConnection();
        m_conns[s] = std::make_unique<Conn>();
        QObject::connect(s, &QTcpSocket::readyRead, this, [this, s] { onData(s); });
        QObject::connect(s, &QTcpSocket::disconnected, this, [this, s] {
            auto it = m_conns.find(s);
            if (it != m_conns.end()) {
                if (it->second->sink) { it->second->sink->close(); QFile::remove(QString::fromStdString(it->second->sinkPath)); }
                m_conns.erase(it);
            }
            s->deleteLater();
        });
    }
}

void WorkerServer::onData(QTcpSocket* s)
{
    auto it = m_conns.find(s);
    if (it == m_conns.end()) return;
    Conn& c = *it->second;
    if (c.sink) {   // the body of a PUT, straight to its file
        QByteArray b = s->readAll();
        const qint64 take = std::min<qint64>(b.size(), c.length - c.got);
        c.sink->write(b.constData(), take);
        c.hash.addData(QByteArrayView(b.constData(), take));
        c.got += take;
        if (c.got >= c.length) {
            c.sink->close(); c.sink.reset();
            const std::string sha = c.hash.result().toHex().toStdString();
            if (!c.sinkSha.empty() && sha != c.sinkSha) { QFile::remove(QString::fromStdString(c.sinkPath)); reply(s, 400, {{"error", "the file arrived damaged"}}); return; }
            std::error_code ec;
            fs::rename(c.sinkPath, c.sinkFinal, ec);
            if (ec) { reply(s, 500, {{"error", ec.message()}}); return; }
            reply(s, 200, {{"ok", true}, {"sha", sha}});
        }
        return;
    }
    c.buf += s->readAll();
    if (!c.headersDone) {
        const int end = c.buf.indexOf("\r\n\r\n");
        if (end < 0) { if (c.buf.size() > 64 * 1024) s->abort(); return; }
        const QList<QByteArray> lines = c.buf.left(end).split('\n');
        const QList<QByteArray> rl = lines.value(0).trimmed().split(' ');
        if (rl.size() < 2) { s->abort(); return; }
        c.method = QString::fromLatin1(rl[0]);
        c.target = QString::fromLatin1(rl[1]);
        const QUrl u(c.target);
        c.path = u.path();
        c.query = QUrlQuery(u);
        c.headers.clear();
        for (int i = 1; i < lines.size(); ++i) {
            const int colon = lines[i].indexOf(':');
            if (colon <= 0) continue;
            c.headers[lines[i].left(colon).trimmed().toLower().toStdString()] = lines[i].mid(colon + 1).trimmed().toStdString();
        }
        c.length = QString::fromStdString(c.headers.count("content-length") ? c.headers["content-length"] : "0").toLongLong();
        c.buf.remove(0, end + 4);
        c.headersDone = true;
        // a PUT of a file: check who asks first, then stream the body to disk
        if (c.method == "PUT" && c.path == "/v1/file") {
            std::string pair, err;
            if (!auth(c, pair, err)) { reply(s, 403, {{"error", err}}); return; }
            const std::string job = c.query.queryItemValue("job", QUrl::FullyDecoded).toStdString(), name = c.query.queryItemValue("name", QUrl::FullyDecoded).toStdString();
            if (!safeName(job) || job.find('/') != std::string::npos || !safeName(name)) { reply(s, 400, {{"error", "bad name"}}); return; }
            const std::string dir = jobDir(pair, job);
            std::error_code ec;
            fs::create_directories(fs::path(dir + "/" + name).parent_path(), ec);
            c.sinkFinal = dir + "/" + name;
            c.sinkPath = c.sinkFinal + ".upload";
            c.sinkSha = c.query.queryItemValue("sha", QUrl::FullyDecoded).toStdString();
            c.sink = std::make_unique<QFile>(QString::fromStdString(c.sinkPath));
            if (!c.sink->open(QIODevice::WriteOnly | QIODevice::Truncate)) { c.sink.reset(); reply(s, 500, {{"error", "can't write"}}); return; }
            c.got = 0;
            // feed what already arrived
            QByteArray rest = c.buf; c.buf.clear();
            const qint64 take = std::min<qint64>(rest.size(), c.length);
            c.sink->write(rest.constData(), take);
            c.hash.addData(QByteArrayView(rest.constData(), take));
            c.got = take;
            if (c.got >= c.length) { c.buf.clear(); onData(s); }
            return;
        }
    }
    if (c.buf.size() < c.length) { if (c.length > 4 * 1024 * 1024) s->abort(); return; }
    handle(s, c);
}

bool WorkerServer::auth(Conn& c, std::string& pair, std::string& error)
{
    pair = c.headers.count("x-rc-pair") ? c.headers["x-rc-pair"] : "";
    auto p = m_pairs.find(pair);
    Pairing market;
    // a market client is asked about on every request: its lease can run out (resolve() caches the chain)
    if (resolve && pair.rfind("m-", 0) == 0) {
        std::string why;
        const int r = resolve(pair, market, why);
        if (r <= 0) { error = r == 0 ? "checking your lease, try again in a moment" : why; c.retry = r == 0; return false; }
        m_pairs[pair] = market;
        p = m_pairs.find(pair);
    }
    if (p == m_pairs.end()) { error = "not paired with this worker"; return false; }
    const std::string ts = c.headers.count("x-rc-ts") ? c.headers["x-rc-ts"] : "";
    const std::string sig = c.headers.count("x-rc-sig") ? c.headers["x-rc-sig"] : "";
    const std::string bodySha = c.headers.count("x-rc-body") ? c.headers["x-rc-body"] : "";
    const long long t = QString::fromStdString(ts).toLongLong();
    if (std::llabs(nowMs() - t) > 120000) { error = "the clocks differ by more than two minutes"; return false; }
    if (sig != sign(p->second.key, c.method.toStdString(), c.target.toStdString(), ts, bodySha)) { error = "bad signature"; return false; }
    if (!m_sigs.insert(sig).second) { error = "replayed request"; return false; }
    if (m_sigs.size() > 20000) m_sigs.clear();
    m_seen[pair] = nowMs();
    return true;
}

std::string WorkerServer::jobDir(const std::string& pair, const std::string& job) const
{
    return m_root + "/" + pair + "/" + job;
}

void WorkerServer::reply(QTcpSocket* s, int code, const json& body)
{
    const QByteArray b = QByteArray::fromStdString(body.dump());
    const char* text = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
                     : code == 402 ? "Payment Required" : code == 503 ? "Service Unavailable" : "Error";
    QByteArray h = "HTTP/1.1 " + QByteArray::number(code) + " " + text + "\r\nContent-Type: application/json\r\nContent-Length: " +
                   QByteArray::number(b.size()) + "\r\nConnection: close\r\n\r\n";
    s->write(h + b);
    s->disconnectFromHost();
}

void WorkerServer::replyFile(QTcpSocket* s, const std::string& path)
{
    auto f = std::make_unique<QFile>(QString::fromStdString(path));
    if (!f->open(QIODevice::ReadOnly)) { reply(s, 404, {{"error", "no such file"}}); return; }
    const qint64 size = f->size();
    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + QByteArray::number(size) +
             "\r\nConnection: close\r\n\r\n");
    QFile* raw = f.release();
    raw->setParent(s);
    auto pump = [s, raw] {
        while (s->bytesToWrite() < 1024 * 1024 && !raw->atEnd()) s->write(raw->read(256 * 1024));
        if (raw->atEnd() && s->bytesToWrite() == 0) s->disconnectFromHost();
    };
    QObject::connect(s, &QTcpSocket::bytesWritten, s, pump);
    pump();
}

void WorkerServer::handle(QTcpSocket* s, Conn& c)
{
    std::string pair, err;
    if (!auth(c, pair, err)) { reply(s, c.retry ? 503 : 403, {{"error", err}}); return; }
    const QByteArray body = c.buf.left(c.length);
    const std::string bodySha = c.headers.count("x-rc-body") ? c.headers["x-rc-body"] : "";
    if (!body.isEmpty() && bodySha != sha256Hex(body)) { reply(s, 400, {{"error", "the body does not match its signature"}}); return; }
    const json in = body.isEmpty() ? json::object() : json::parse(body.toStdString(), nullptr, false);
    const std::string job = c.query.queryItemValue("job", QUrl::FullyDecoded).toStdString();
    const bool jobOk = safeName(job) && job.find('/') == std::string::npos;

    if (c.method == "GET" && c.path == "/v1/hello") {
        json h = m_info;
        h["tasks"] = tasksJson(pair);
        h["client"] = m_pairs[pair].name;
        h["boot"] = m_boot;
        reply(s, 200, h);
        return;
    }
    if (c.method == "POST" && c.path == "/v1/task") {
        if (!in.is_object() || !in.contains("argv") || !in["argv"].is_array()) { reply(s, 400, {{"error", "no argv"}}); return; }
        const std::string id = in.value("id", std::string()), tjob = in.value("job", std::string());
        if (!safeName(id) || id.find('/') != std::string::npos || !safeName(tjob) || tjob.find('/') != std::string::npos) {
            reply(s, 400, {{"error", "bad task or job id"}}); return;
        }
        QStringList argv;
        for (auto& a : in["argv"]) { if (!a.is_string()) { reply(s, 400, {{"error", "argv holds strings"}}); return; } argv << QString::fromStdString(a.get<std::string>()); }
        std::string why;
        if (!checkArgv(argv, why)) { reply(s, 400, {{"error", why}}); return; }
        const long long until = m_pairs[pair].tasksUntil;
        if (until > 0 && nowMs() > until) { reply(s, 402, {{"error", "your rental has ended: renew it to start new steps"}}); return; }
        const std::string tid = pair + "/" + id;
        auto existing = m_tasks.find(tid);
        if (existing != m_tasks.end() && existing->second.running) { reply(s, 200, {{"ok", true}, {"attached", true}}); return; }
        // The same task (same job, command and inputs) finished well while the client was away: its
        // result is in the folder already.
        if (existing != m_tasks.end() && existing->second.ok && nowMs() - existing->second.ended < 24LL * 3600 * 1000) {
            reply(s, 200, {{"ok", true}, {"finished", true}, {"result", true}});
            return;
        }
        // forget tasks that ended more than a day ago
        for (auto it = m_tasks.begin(); it != m_tasks.end();)
            it = (!it->second.running && nowMs() - it->second.ended > 24LL * 3600 * 1000) ? m_tasks.erase(it) : std::next(it);
        Task t;
        t.id = id; t.pair = pair; t.job = tjob; t.dir = jobDir(pair, tjob); t.argv = argv; t.started = nowMs();
        std::error_code ec;
        fs::create_directories(t.dir, ec);
        m_tasks[tid] = t;
        RunHooks h;
        h.out = [this, pair, id](const std::string& line) { push(pair, {{"task", id}, {"kind", "out"}, {"text", line}}); };
        h.err = [this, pair, id](const std::string& chunk) { push(pair, {{"task", id}, {"kind", "err"}, {"text", chunk}}); };
        h.done = [this, pair, id, tid](bool ok) {
            auto it = m_tasks.find(tid);
            if (it != m_tasks.end()) { it->second.running = false; it->second.ok = ok; it->second.ended = nowMs(); }
            push(pair, {{"task", id}, {"kind", "done"}, {"ok", ok}});
            if (onChange) onChange();
        };
        m_tasks[tid].stop = m_run(t.dir, argv, "reelcut-w-" + pair.substr(0, 6) + "-" + id, h);
        if (onChange) onChange();
        reply(s, 200, {{"ok", true}});
        return;
    }
    if (c.method == "GET" && c.path == "/v1/events") {
        c.waiting = true;
        c.waitPair = pair;
        c.since = c.query.queryItemValue("since", QUrl::FullyDecoded).toLongLong();
        c.waitUntil = nowMs() + std::min(30, std::max(0, c.query.queryItemValue("wait", QUrl::FullyDecoded).toInt())) * 1000LL;
        flushWaiters();
        return;
    }
    if (c.method == "POST" && c.path == "/v1/cancel") {
        const std::string tid = pair + "/" + in.value("id", std::string());
        auto it = m_tasks.find(tid);
        if (it != m_tasks.end() && it->second.running && it->second.stop) it->second.stop();
        reply(s, 200, {{"ok", true}});
        return;
    }
    if (c.method == "GET" && c.path == "/v1/files") {
        if (!jobOk) { reply(s, 400, {{"error", "bad job"}}); return; }
        json files = json::array();
        const std::string dir = jobDir(pair, job);
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string rel = fs::relative(it->path(), dir, ec).string();
            if (rel.size() > 7 && rel.compare(rel.size() - 7, 7, ".upload") == 0) continue;
            const auto mt = std::chrono::duration_cast<std::chrono::milliseconds>(it->last_write_time(ec).time_since_epoch()).count();
            files.push_back({{"name", rel}, {"size", (long long)it->file_size(ec)}, {"mtime", (long long)mt}});
        }
        reply(s, 200, {{"files", files}});
        return;
    }
    if (c.method == "GET" && c.path == "/v1/file") {
        const std::string name = c.query.queryItemValue("name", QUrl::FullyDecoded).toStdString();
        if (!jobOk || !safeName(name)) { reply(s, 400, {{"error", "bad name"}}); return; }
        replyFile(s, jobDir(pair, job) + "/" + name);
        return;
    }
    if (c.method == "DELETE" && c.path == "/v1/file") {
        const std::string name = c.query.queryItemValue("name", QUrl::FullyDecoded).toStdString();
        if (!jobOk || !safeName(name)) { reply(s, 400, {{"error", "bad name"}}); return; }
        std::error_code ec;
        fs::remove(jobDir(pair, job) + "/" + name, ec);
        reply(s, 200, {{"ok", true}});
        return;
    }
    if (c.method == "DELETE" && c.path == "/v1/job") {
        if (!jobOk) { reply(s, 400, {{"error", "bad job"}}); return; }
        for (auto& [id, t] : m_tasks) if (t.pair == pair && t.job == job && t.running && t.stop) t.stop();
        std::error_code ec;
        fs::remove_all(jobDir(pair, job), ec);
        reply(s, 200, {{"ok", true}});
        return;
    }
    reply(s, 404, {{"error", "no such call"}});
}

json WorkerServer::tasksJson(const std::string& pair) const
{
    json a = json::array();
    for (auto& [tid, t] : m_tasks)
        if (t.pair == pair && t.running) a.push_back({{"id", t.id}, {"job", t.job}, {"cmd", t.argv.isEmpty() ? "" : t.argv[0].toStdString()}});
    return a;
}

void WorkerServer::push(const std::string& pair, json ev)
{
    // Consecutive output of one task merges into one event (a chatty ffmpeg makes thousands).
    if (!m_events.empty()) {
        Event& last = m_events.back();
        const std::string kind = ev.value("kind", std::string());
        if (last.pair == pair && (kind == "out" || kind == "err") && last.body.value("kind", std::string()) == kind &&
            last.body.value("task", std::string()) == ev.value("task", std::string()) &&
            last.body.value("text", std::string()).size() < 16384 && last.seq > m_lastFlushed) {
            last.body["text"] = last.body.value("text", std::string()) + (kind == "out" ? "\n" : "") + ev.value("text", std::string());
            return;
        }
    }
    ev["seq"] = ++m_seq;
    m_events.push_back({m_seq, pair, ev});
    if (m_events.size() > 4000) m_events.erase(m_events.begin(), m_events.begin() + 1000);
    // answer the waiting polls soon (batched: a little later rather than one reply per line)
    if (!m_flushArmed) {
        m_flushArmed = true;
        QTimer::singleShot(ev.value("kind", std::string()) == "done" ? 0 : 400, this, [this] { m_flushArmed = false; flushWaiters(); });
    }
}

void WorkerServer::flushWaiters()
{
    const long long now = nowMs();
    std::vector<QTcpSocket*> done;
    for (auto& [s, c] : m_conns) {
        if (!c->waiting) continue;
        json evs = json::array();
        for (auto& e : m_events) if (e.pair == c->waitPair && e.seq > c->since) evs.push_back(e.body);
        if (evs.empty() && now < c->waitUntil) continue;
        c->waiting = false;
        if (!m_events.empty()) m_lastFlushed = m_events.back().seq;
        json h = {{"events", evs}, {"seq", m_seq}, {"tasks", tasksJson(c->waitPair)}, {"boot", m_boot}};
        // events trimmed away (or a restarted worker): say so, the client re-syncs
        if (!m_events.empty() && c->since > 0 && c->since < m_events.front().seq - 1) h["gap"] = true;
        done.push_back(s);
        reply(s, 200, h);
    }
}

// ── the client ───────────────────────────────────────────────────────────────────────────────
WorkerClient::WorkerClient(Pairing p) : m_p(std::move(p)), m_nam(new QNetworkAccessManager(this)) {}
WorkerClient::~WorkerClient() { if (m_poll) m_poll->abort(); }

QNetworkReply* WorkerClient::request(const std::string& method, const std::string& pathQuery, const QByteArray& body,
                                     QIODevice* bodyDevice)
{
    const std::string base = m_p.url.empty() ? (m_p.urls.empty() ? "" : m_p.urls[0]) : m_p.url;
    QNetworkRequest rq(QUrl(QString::fromStdString(base + pathQuery)));
    const std::string ts = std::to_string(nowMs());
    const std::string bodySha = body.isEmpty() ? "" : sha256Hex(body);
    // sign what the server will see: the path and the query as sent
    const QUrl u = rq.url();
    QString pq = u.path(QUrl::FullyEncoded);
    if (u.hasQuery()) pq += "?" + u.query(QUrl::FullyEncoded);
    rq.setRawHeader("X-Rc-Pair", QByteArray::fromStdString(m_p.id));
    rq.setRawHeader("X-Rc-Ts", QByteArray::fromStdString(ts));
    rq.setRawHeader("X-Rc-Body", QByteArray::fromStdString(bodySha));
    rq.setRawHeader("X-Rc-Sig", QByteArray::fromStdString(sign(m_p.key, method, pq.toStdString(), ts, bodySha)));
    if (!body.isEmpty()) rq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (bodyDevice) {
        rq.setHeader(QNetworkRequest::ContentLengthHeader, bodyDevice->size());
        return m_nam->sendCustomRequest(rq, QByteArray::fromStdString(method), bodyDevice);
    }
    if (method == "GET") return m_nam->get(rq);
    if (method == "DELETE") return m_nam->deleteResource(rq);
    return m_nam->sendCustomRequest(rq, QByteArray::fromStdString(method), body);
}

static json replyJson(QNetworkReply* r, bool& ok)
{
    const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray b = r->readAll();
    json j = json::parse(b.toStdString(), nullptr, false);
    ok = r->error() == QNetworkReply::NoError && code == 200 && j.is_object();
    if (!ok) {
        std::string e = j.is_object() && j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>()
                                                                                         : r->errorString().toStdString();
        j = {{"error", e}};
    }
    return j;
}

void WorkerClient::setOnline(bool on, const std::string& why)
{
    if (on == m_online) return;
    m_online = on;
    if (onOnline) onOnline(on, why);
}

void WorkerClient::hello(std::function<void(bool, const json&)> cb)
{
    // the last good URL first, then the others
    std::vector<std::string> order;
    if (!m_p.url.empty()) order.push_back(m_p.url);
    for (auto& u : m_p.urls) if (u != m_p.url) order.push_back(u);
    auto tryAt = std::make_shared<std::function<void(size_t, std::string)>>();
    *tryAt = [this, order, cb, tryAt](size_t i, std::string lastErr) {
        if (i >= order.size()) { setOnline(false, lastErr); cb(false, {{"error", lastErr.empty() ? "no address answered" : lastErr}}); return; }
        const std::string keep = m_p.url;
        m_p.url = order[i];
        QNetworkReply* r = request("GET", "/v1/hello", QByteArray());
        r->setProperty("rcTimeout", 4000);
        QTimer::singleShot(4000, r, [r] { if (r->isRunning()) r->abort(); });
        QObject::connect(r, &QNetworkReply::finished, this, [this, r, i, cb, tryAt, keep] {
            r->deleteLater();
            bool ok = false;
            json j = replyJson(r, ok);
            if (ok) { m_hello = j; m_failures = 0; noteBoot(j.value("boot", std::string()), j["tasks"]); setOnline(true, ""); cb(true, j); return; }
            const std::string e = j.value("error", std::string("no answer"));
            // a worker that answered but refused (not paired, clock): no use trying the other addresses
            const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code == 403) { setOnline(false, e); cb(false, j); return; }
            (*tryAt)(i + 1, m_p.url + ": " + e);
        });
    };
    (*tryAt)(0, "");
}

void WorkerClient::submit(const json& task, std::function<void(bool, const json&)> cb)
{
    QNetworkReply* r = request("POST", "/v1/task", QByteArray::fromStdString(task.dump()));
    QObject::connect(r, &QNetworkReply::finished, this, [r, cb] { r->deleteLater(); bool ok = false; json j = replyJson(r, ok); cb(ok, j); });
}

void WorkerClient::cancel(const std::string& taskId)
{
    QNetworkReply* r = request("POST", "/v1/cancel", QByteArray::fromStdString(json({{"id", taskId}}).dump()));
    QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
}

static std::string q(const std::string& s) { return QUrl::toPercentEncoding(QString::fromStdString(s)).toStdString(); }

void WorkerClient::files(const std::string& job, std::function<void(bool, const json&)> cb)
{
    QNetworkReply* r = request("GET", "/v1/files?job=" + q(job), QByteArray());
    QObject::connect(r, &QNetworkReply::finished, this, [r, cb] { r->deleteLater(); bool ok = false; json j = replyJson(r, ok); cb(ok, j); });
}

void WorkerClient::download(const std::string& job, const std::string& name, const std::string& path,
                            std::function<void(bool, const std::string&)> cb)
{
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    auto f = std::make_shared<QFile>(QString::fromStdString(path + ".download"));
    if (!f->open(QIODevice::WriteOnly | QIODevice::Truncate)) { cb(false, "can't write " + path); return; }
    QNetworkReply* r = request("GET", "/v1/file?job=" + q(job) + "&name=" + q(name), QByteArray());
    QObject::connect(r, &QNetworkReply::readyRead, this, [r, f] { f->write(r->readAll()); });
    QObject::connect(r, &QNetworkReply::finished, this, [r, f, path, cb] {
        r->deleteLater();
        f->write(r->readAll());
        f->close();
        const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() != QNetworkReply::NoError || code != 200) {
            QFile::remove(f->fileName());
            cb(false, r->errorString().toStdString());
            return;
        }
        std::error_code ec;
        fs::rename(f->fileName().toStdString(), path, ec);
        cb(!ec, ec ? ec.message() : "");
    });
}

void WorkerClient::upload(const std::string& job, const std::string& name, const std::string& path,
                          std::function<void(bool, const std::string&)> cb)
{
    auto* f = new QFile(QString::fromStdString(path));
    if (!f->open(QIODevice::ReadOnly)) { delete f; cb(false, "can't read " + path); return; }
    // the sha, read once (files to upload are the small ones Basecamp writes, or one imported video)
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(f);
    f->seek(0);
    const std::string sha = h.result().toHex().toStdString();
    QNetworkReply* r = request("PUT", "/v1/file?job=" + q(job) + "&name=" + q(name) + "&sha=" + sha, QByteArray(), f);
    f->setParent(r);
    QObject::connect(r, &QNetworkReply::finished, this, [r, cb] { r->deleteLater(); bool ok = false; json j = replyJson(r, ok); cb(ok, ok ? "" : j.value("error", std::string("failed"))); });
}

void WorkerClient::remove(const std::string& job, const std::string& name, std::function<void(bool)> cb)
{
    QNetworkReply* r = request("DELETE", "/v1/file?job=" + q(job) + "&name=" + q(name), QByteArray());
    QObject::connect(r, &QNetworkReply::finished, this, [r, cb] { r->deleteLater(); bool ok = false; replyJson(r, ok); cb(ok); });
}

void WorkerClient::forget(const std::string& job)
{
    QNetworkReply* r = request("DELETE", "/v1/job?job=" + q(job), QByteArray());
    QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
}

std::string WorkerClient::runSync(const std::string& job, const QStringList& argv, int timeoutMs, std::string& error)
{
    // Submit, then follow the events until it ends, in a local event loop. Every reply is tied to the
    // loop object, so nothing calls back into this frame once it has returned.
    const std::string id = "sync-" + std::to_string(nowMs());
    json a = json::array();
    for (auto& s : argv) a.push_back(s.toStdString());
    QEventLoop loop;
    bool ok = false, finished = false;
    std::string out;
    long long since = m_since;
    std::vector<QPointer<QNetworkReply>> pending;
    auto finish = [&](const std::string& err) { if (finished) return; if (!err.empty() && error.empty()) error = err; finished = true; loop.quit(); };
    std::function<void()> next = [&] {
        if (finished) return;
        QNetworkReply* r = request("GET", "/v1/events?since=" + std::to_string(since) + "&wait=10", QByteArray());
        pending.push_back(r);
        QObject::connect(r, &QNetworkReply::finished, &loop, [&, r] {
            r->deleteLater();
            bool got = false;
            json j = replyJson(r, got);
            if (!got) { finish(j.value("error", std::string("no answer"))); return; }
            since = j.value("seq", since);
            for (auto& e : j["events"]) {
                if (e.value("task", std::string()) != id) continue;
                const std::string kind = e.value("kind", std::string());
                if (kind == "out") out += e.value("text", std::string()) + "\n";
                if (kind == "done") { ok = e.value("ok", false); finish(ok ? "" : "it failed on the worker"); return; }
            }
            next();
        });
    };
    QNetworkReply* sub = request("POST", "/v1/task", QByteArray::fromStdString(json({{"id", id}, {"job", job}, {"argv", a}}).dump()));
    pending.push_back(sub);
    QObject::connect(sub, &QNetworkReply::finished, &loop, [&, sub] {
        sub->deleteLater();
        bool sent = false;
        json j = replyJson(sub, sent);
        if (!sent) { finish(j.value("error", std::string("refused"))); return; }
        next();
    });
    QTimer::singleShot(timeoutMs, &loop, [&] { finish("the worker took too long"); });
    loop.exec();
    for (auto& r : pending) if (r && r->isRunning()) { r->disconnect(&loop); r->abort(); }
    return ok ? out : std::string();
}

bool WorkerClient::noteBoot(const std::string& boot, const json& tasks)
{
    if (boot.empty() || boot == m_boot) return false;
    const bool restarted = !m_boot.empty();
    m_boot = boot;
    if (!restarted) return false;
    m_since = 0;
    if (onEvent) onEvent({{"kind", "resync"}, {"tasks", tasks.is_array() ? tasks : json::array()}});
    return true;
}

void WorkerClient::listen()
{
    if (m_listening) return;
    m_listening = true;
    poll();
}

void WorkerClient::poll()
{
    if (!m_listening) return;
    QNetworkReply* r = request("GET", "/v1/events?since=" + std::to_string(m_since) + "&wait=25", QByteArray());
    m_poll = r;
    QTimer::singleShot(40000, r, [r] { if (r->isRunning()) r->abort(); });
    QObject::connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        bool ok = false;
        json j = replyJson(r, ok);
        if (!ok) {
            ++m_failures;
            setOnline(false, j.value("error", std::string("no answer")));
            // try the other addresses now and then; back off up to 30 s
            const int wait = std::min(30000, 1000 * (1 << std::min(m_failures, 5)));
            QTimer::singleShot(wait, this, [this] { hello([this](bool, const json&) { poll(); }); });
            return;
        }
        m_failures = 0;
        setOnline(true, "");
        // a worker that started again (another boot id) numbers its events from 1 again
        if (noteBoot(j.value("boot", std::string()), j["tasks"])) { poll(); return; }
        const long long seq = j.value("seq", m_since);
        if (onEvent) {
            if (j.value("gap", false)) onEvent({{"kind", "resync"}, {"tasks", j["tasks"]}});
            for (auto& e : j["events"]) onEvent(e);
            onEvent({{"kind", "tasks"}, {"tasks", j["tasks"]}});
        }
        m_since = seq;
        poll();
    });
}

} // namespace reelcut::remote
