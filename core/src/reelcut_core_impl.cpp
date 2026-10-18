#include "reelcut_core_impl.h"
#include "logos_sdk.h"
#include "forum_post.h"
#include <QMetaObject>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

static const char* kVersion = "0.2.0";
// Reelcut's own sessions on this computer's shrooms-agent: "reelcut-claude", "reelcut-pi".
static const char* kLocalSession = "reelcut";
static const char* kImage = "ghcr.io/vpavlin/reelcut:latest";
static const size_t kLogLines = 40;
// reelcut moves with YouTube (yt-dlp, format choice): look for a newer image at most this often.
static const qint64 kImageMaxAgeMs = 24LL * 3600 * 1000;
// The prompt goes to the agent as one command-line argument; Linux caps a single one at 128 KiB.
static const size_t kMaxPrompt = 120 * 1024;

// ── helpers ─────────────────────────────────────────────────────────────────

// Typed reads that never throw: json::value() throws on a field of another type, and jobs.json or a
// settings argument can hold anything.
static std::string jstr(const json& o, const char* k, const std::string& def)
{
    if (!o.is_object()) return def;
    auto it = o.find(k);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : def;
}
static int64_t jnum(const json& o, const char* k, int64_t def)
{
    if (!o.is_object()) return def;
    auto it = o.find(k);
    if (it == o.end() || !it->is_number()) return def;
    return it->is_number_float() ? (int64_t)it->get<double>() : it->get<int64_t>();
}
static bool jbool(const json& o, const char* k, bool def)
{
    if (!o.is_object()) return def;
    auto it = o.find(k);
    return (it != o.end() && it->is_boolean()) ? it->get<bool>() : def;
}

// Every process callback runs inside a Qt signal: an exception escaping it terminates the whole
// module process. Wrap them all; log what was swallowed.
template <typename F>
static auto guarded(const char* where, F fn)
{
    return [where, fn](auto&&... args) mutable {
        try {
            // QProcess's own signals carry a QPrivateSignal tag; a callback without parameters skips it.
            // (Qt also probes the wrapper with other argument counts; those never run.)
            if constexpr (std::is_invocable_v<F&, decltype(args)...>) fn(std::forward<decltype(args)>(args)...);
            else if constexpr (std::is_invocable_v<F&>) fn();
        }
        catch (const std::exception& e) { std::fprintf(stderr, "[reelcut_core] %s: %s\n", where, e.what()); }
        catch (...) { std::fprintf(stderr, "[reelcut_core] %s: unknown exception\n", where); }
    };
}

static std::string rootDir()
{
    if (const char* e = std::getenv("REELCUT_HOME")) if (*e) return e;
    QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (movies.isEmpty()) movies = QDir::homePath() + "/Videos";
    return (movies + "/Reelcut").toStdString();
}

static std::string readFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static void writeFile(const std::string& path, const std::string& body)
{
    const std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::trunc); f << body; }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) fs::remove(tmp, ec);
}

static std::string localPathFrom(const std::string& s)
{
    if (s.rfind("file://", 0) == 0) return QUrl(QString::fromStdString(s)).toLocalFile().toStdString();
    return s;
}

static std::string trim(std::string s)
{
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

// What Basecamp's AppImage puts in its own environment, dropped before starting anything: its
// LD_PRELOAD makes coreutils refuse to start, and its library and Qt paths would be handed to
// programs built against the system's (podman, python, ffmpeg, xdg-open).
extern "C" char** environ;
// Where container tools live besides PATH. An app opened from the Finder on macOS gets only
// /usr/bin:/bin:/usr/sbin:/sbin, without Homebrew, Docker Desktop, podman's installer or OrbStack.
static QStringList extraBinDirs()
{
    const QString home = QDir::homePath();
    return {home + "/.local/bin", "/usr/local/bin", "/opt/homebrew/bin", "/opt/podman/bin", "/opt/local/bin",
            "/Applications/Docker.app/Contents/Resources/bin", home + "/.orbstack/bin", home + "/.rd/bin"};
}
static std::vector<std::string> cleanEnv()
{
    static const char* drop[] = {"LD_PRELOAD=", "LD_LIBRARY_PATH=", "QT_PLUGIN_PATH=", "QML2_IMPORT_PATH=",
                                 "QML_IMPORT_PATH=", "APPDIR=", "APPIMAGE=", "ARGV0=", "OWD=", "__BUNDLE_REAL_EXE="};
    std::vector<std::string> out;
    for (char** v = environ; v && *v; v++) {
        bool keep = true;
        for (const char* d : drop)
            if (std::strncmp(*v, d, std::strlen(d)) == 0) keep = false;
        if (keep) out.emplace_back(*v);
    }
    // The extra places on PATH too: docker runs its credential helper, podman its machine, by name.
    std::string path;
    for (auto& v : out) if (v.rfind("PATH=", 0) == 0) path = v.substr(5);
    if (path.empty()) path = "/usr/bin:/bin:/usr/sbin:/sbin";
    QStringList have = QString::fromStdString(path).split(':', Qt::SkipEmptyParts);
    for (auto& d : extraBinDirs()) if (!have.contains(d) && QFileInfo(d).isDir()) have << d;
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& v) { return v.rfind("PATH=", 0) == 0; }), out.end());
    out.emplace_back("PATH=" + have.join(':').toStdString());
    return out;
}
static QProcessEnvironment processEnv()
{
    QProcessEnvironment e;
    for (auto& v : cleanEnv()) {
        auto eq = v.find('=');
        if (eq != std::string::npos)
            e.insert(QString::fromStdString(v.substr(0, eq)), QString::fromStdString(v.substr(eq + 1)));
    }
    return e;
}
// Fire and forget (closing a task, removing a container), in the cleaned environment.
static void detached(const std::string& bin, const QStringList& args)
{
    QProcess p;
    p.setProcessEnvironment(processEnv());
    p.setProgram(QString::fromStdString(bin));
    p.setArguments(args);
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    p.startDetached();
}
static std::string findExe(const char* name)
{
    QString p = QStandardPaths::findExecutable(name);
    if (p.isEmpty()) p = QStandardPaths::findExecutable(name, extraBinDirs());
    return p.toStdString();
}

// "H:MM:SS" or "MM:SS" or seconds -> seconds; -1 when it isn't a time.
static double secs(std::string s)
{
    s = trim(s);
    static const QRegularExpression re("^(?:(\\d+):)?(\\d+):(\\d+(?:\\.\\d+)?)$");
    auto m = re.match(QString::fromStdString(s));
    if (m.hasMatch()) return m.captured(1).toDouble() * 3600 + m.captured(2).toDouble() * 60 + m.captured(3).toDouble();
    bool ok = false;
    double v = QString::fromStdString(s).toDouble(&ok);
    return ok ? v : -1;
}

static std::string hexBytes(const std::vector<uint8_t>& b)
{
    static const char* k = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b) { s.push_back(k[x >> 4]); s.push_back(k[x & 15]); }
    return s;
}

struct Clip {
    int n = 0;
    std::string start, end, speaker, hook, until, summary;
    double seconds = 0;
};

// The clip lines reelcut cut reads: N | START | END | speaker | hook | until LASTWORD. Anything else
// in the text (an agent's greeting, a summary) is ignored, as reelcut ignores it.
static std::vector<Clip> parseClips(const std::string& text)
{
    std::vector<Clip> out;
    std::istringstream in(text);
    std::string line;
    static const QRegularExpression num("^(?:CLIP\\s*)?(\\d+)$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression startW("^start\\s*", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression endW("^end\\s*", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression untilW("^until\\s*", QRegularExpression::CaseInsensitiveOption);
    while (std::getline(in, line)) {
        QString l = QString::fromStdString(line).trimmed();
        if (l.startsWith('|')) l.remove(0, 1);
        if (l.endsWith('|')) l.chop(1);
        QStringList p = l.split('|');
        for (auto& x : p) x = x.trimmed();
        if (p.size() < 5) continue;
        auto m = num.match(p[0].remove('*').trimmed());
        if (!m.hasMatch()) continue;
        Clip c;
        c.n = m.captured(1).toInt();
        c.start = QString(p[1]).remove(startW).toStdString();
        c.end = QString(p[2]).remove(endW).toStdString();
        c.speaker = p[3].toStdString();
        c.hook = p[4].toStdString();
        if (p.size() > 5) c.until = QString(p[5]).remove(untilW).toStdString();
        // What is said and why it works (reelcut's prompt asks for it; a cell may hold a "|", so the rest).
        if (p.size() > 6) c.summary = p.mid(6).join(" | ").toStdString();
        double a = secs(c.start), b = secs(c.end);
        if (a < 0 || b < 0) continue;
        c.seconds = b - a;
        out.push_back(c);
    }
    return out;
}

// The clip list as reelcut cut reads it, one line per clip.
static std::string clipsText(const std::vector<Clip>& clips)
{
    std::string text;
    for (auto& c : clips) {
        text += std::to_string(c.n) + " | " + c.start + " | " + c.end + " | " + c.speaker + " | " + c.hook;
        if (!c.until.empty() || !c.summary.empty()) text += " | " + (c.until.empty() ? std::string() : "until " + c.until);
        if (!c.summary.empty()) text += " | " + c.summary;
        text += "\n";
    }
    return text;
}

static json clipsJson(const std::vector<Clip>& v)
{
    json a = json::array();
    for (auto& c : v)
        a.push_back({{"n", c.n}, {"start", c.start}, {"end", c.end}, {"speaker", c.speaker}, {"hook", c.hook},
                     {"until", c.until}, {"summary", c.summary}, {"seconds", c.seconds}});
    return a;
}

// The transcript lines of one clip, from START to END as the transcript stamps them.
static std::string clipLines(const std::string& transcript, const Clip& c)
{
    const double a = secs(c.start), e = secs(c.end);
    static const QRegularExpression stamp("^\\[(\\d+:\\d\\d:\\d\\d)\\] ?(.*)$");
    std::istringstream in(transcript);
    std::string line, text;
    while (std::getline(in, line)) {
        auto m = stamp.match(QString::fromStdString(line));
        if (!m.hasMatch()) continue;
        double t = secs(m.captured(1).toStdString());
        if (t < a - 0.5) continue;
        if (t > e + 0.5) break;
        text += line + "\n";
    }
    return text;
}

// YouTube texts in an answer: per reel a block "=== REEL N ===", then "TITLE: …", "DESCRIPTION:" and its
// lines, "TAGS: a, b, c". Anything before the first block (a greeting) is ignored. Clip number -> text.
static json parseYoutube(const std::string& text)
{
    json out = json::object();
    static const QRegularExpression head("^\\W*=+\\s*REEL\\s*#?\\s*(\\d+)\\s*=+\\W*$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression field("^\\W{0,3}(TITLE|DESCRIPTION|TAGS)\\W{0,3}:\\s*(.*)$", QRegularExpression::CaseInsensitiveOption);
    std::istringstream in(text);
    std::string line, cur, where;
    json block;
    auto flush = [&] {
        if (cur.empty()) return;
        std::string t = trim(jstr(block, "title", "")), d = trim(jstr(block, "description", ""));
        if (!t.empty() || !d.empty()) {
            json tags = json::array();
            for (auto& x : QString::fromStdString(jstr(block, "tagsText", "")).split(',')) {
                QString v = x.trimmed().remove('#');
                if (!v.isEmpty()) tags.push_back(v.toStdString());
            }
            out[cur] = {{"title", t}, {"description", d}, {"tags", tags}};
        }
        block = json::object(); where.clear();
    };
    while (std::getline(in, line)) {
        QString l = QString::fromStdString(line);
        if (l.endsWith('\r')) l.chop(1);
        auto h = head.match(l.trimmed());
        if (h.hasMatch()) { flush(); cur = h.captured(1).toStdString(); continue; }
        if (cur.empty()) continue;
        auto f = field.match(l.trimmed());
        if (f.hasMatch()) {
            where = f.captured(1).toLower().toStdString();
            std::string v = f.captured(2).toStdString();
            if (where == "title") block["title"] = v;
            else if (where == "tags") block["tagsText"] = v;
            else block["description"] = v.empty() ? std::string() : v + "\n";
            continue;
        }
        if (where == "description") block["description"] = jstr(block, "description", "") + l.toStdString() + "\n";
        else if (where == "tags" && !l.trimmed().isEmpty()) block["tagsText"] = jstr(block, "tagsText", "") + ", " + l.toStdString();
    }
    flush();
    return out;
}

// For sh -c: a path in single quotes.
static QString shq(const std::string& s)
{
    return "'" + QString::fromStdString(s).replace("'", "'\\''") + "'";
}

// A speaker's name as cut --speakers takes it ("A=NAME@left,…"): none of its separators, one line.
static std::string cleanName(const std::string& s)
{
    QString n = QString::fromStdString(s);
    n.replace(QRegularExpression("[,=@\\r\\n\\t]"), " ");
    n = n.simplified();
    if (n.size() > 40) n = n.left(40).trimmed();
    return n.toStdString();
}

// A caption colour as cut takes it: "#RRGGBB" (upper case), or "" for none or anything else.
static std::string hexColour(const std::string& s)
{
    QString c = QString::fromStdString(s).trimmed();
    if (!c.startsWith('#')) c.prepend('#');
    static const QRegularExpression re("^#[0-9A-Fa-f]{6}$");
    return re.match(c).hasMatch() ? c.toUpper().toStdString() : "";
}

// ── state ───────────────────────────────────────────────────────────────────

struct Reel {
    std::string file, name, thumb;
    double seconds = 0;
    int n = 0;                  // its clip's number
};

// A job's step is where it is in the pipeline; its status says whether that step runs, waits or stopped.
//   steps:  fetch -> transcribe -> diarize -> speakers -> pick -> clips -> cut -> done
//           (diarize: who speaks when; speakers: the person names them and says where they sit)
//   status: queued (waits for its turn) | running | waiting (for the person: clips to check) |
//           failed | cancelled | done
struct Job {
    std::string id, url, file, dir, title, channel;
    qint64 created = 0;
    double duration = 0;
    std::string step = "fetch", status = "queued", error;
    int progressI = 0, progressN = 0;
    std::string activity;
    std::deque<std::string> log;
    std::string agent, pickTask, pickReply;
    std::string clipsText, fixes;
    std::vector<Reel> reels;
    int words = 0;
    // Who speaks (reelcut diarize): {found, talk:{A:s}, samples:{A:[lines]}, names:{A:…}, sides:{A:left|middle|right}, error}
    json spk = json::object();
    int speakersWanted = 0;     // 0: counted from the voices
    std::string layout;         // cut --frame: "" (reelcut's own choice), fit, stack, speaker, crop
    int vw = 0, vh = 0;         // the video's size, for the layout
    bool newerTried = false;    // cut met an image older than its flags: pulled once, not again
    // Before picking, the steps overlap: what is done so far.
    bool fetched = false, transcribed = false, probed = false;
    std::string diar;           // who speaks: "" (not decided) | wanted | running | after (transcribe) | done | failed | off
    bool diarRetry = false;
    std::string brief;          // what the person looks for: topics, people, tone, keywords
    std::vector<int> chosen;    // the clips to cut, by number
    std::string intro, outro;   // cards (images in the library), "" for none
    std::string frame;          // one frame of the video, copied where the view may load it
    // Logos Storage: per place ("local", or a node's url) the CIDs of what went there: reel file name
    // (and "reels.json", the set) -> CID. What is uploading or failed now: file name -> "uploading" |
    // "failed: …" (not kept: an upload Basecamp closed on is simply done again).
    json stored = json::object();
    std::map<std::string, std::string> storing;
    // YouTube texts for the reels (issue #4): clip number -> {title, description, tags: [...]}.
    // Written by an agent, or pasted from any AI chat; ytState "asking" while an agent writes.
    json yt = json::object();
    std::string ytState, ytError, ytReply, ytWho, ytTask;
    quint64 ytGen = 0;
    // A forum to post the reels to (Forumer or Logos Forum): {app, forum, thread, threadTitle, auto}.
    // forumPosted: per thread id, reel file name -> the post's id there; "started" -> threads Reelcut started.
    json forum = json::object(), forumPosted = json::object();
    std::string forumState, forumError;     // forumState "posting" while replies go out
    quint64 forumGen = 0;
};

struct Running {
    QPointer<QProcess> proc;
    std::string jobId, container;
    bool cancelled = false;
};

struct ReelcutCorePrivate {
    ReelcutCoreImpl* q;
    std::vector<Job> jobs;
    // "auto" by default: a caged agent on this computer when shrooms-agent is here (Claude Code, else pi),
    // else the person copies the prompt into any AI chat. "local:claude" / "local:pi" = that one;
    // "" = always by hand; "MACHINE/SESSION" = that mesh session.
    std::string agent = "auto";
    int count = 12;             // clips to propose, best first (more than will be cut)
    int minLen = 20, maxLen = 55;   // seconds per clip, for the prompt
    int autoCount = 5;          // ticked (and cut, with autoCut) after the agent answers
    double lead = 0.25, tail = 0.4, fade = 0.3, card = 2;   // cut's edges and cards
    int captionMargin = -1;     // captions, px from the bottom of 1920; -1: where the layout puts them
    std::string colour, sayingColour, saidColour;   // the captions' words to come, being said, said: "#RRGGBB" or ""
    bool forcePull = false;     // look for a newer image before the next cut
    std::string brief, intro, outro;    // the last used, for a new job
    std::string runnerPref = "auto";
    bool autoCut = true;
    bool diarize = true;        // tell the speakers apart after transcribing

    // detected
    std::string runner, runnerBin, runnerError;
    bool imageChecked = false, imagePresent = false;
    qint64 lastPull = 0;

    json agents = json::array();
    bool listingAgents = false;
    std::string viewDir;

    std::map<std::string, Running> procs;   // by slot: see "running" below
    std::set<std::string> audioWatch;       // jobs looking for their audio every 2 s
    std::map<std::string, QPointer<QProcess>> picks;  // agent picks run beside it
    bool emitPending = false;

    // Logos Storage: where the reels go. "" (nowhere), "local" (this computer's node, storage_module),
    // "node" (a node elsewhere at storeNode, through storage_gateway: the same API).
    std::string storeTo, storeNode;
    std::string forumAlias = "Reelcut";
    // Logos Delivery, for the forums: started (or joined, when a forum app runs it) on first use.
    std::string dlState, dlError;           // "" | starting | ready | failed
    std::vector<std::function<void(const std::string&)>> dlWaiters;
    std::set<std::string> dlTopics;         // subscribed
    reelcut::forum::Key fkey;
    json forumThreads = json::object();     // "app|forum" -> {state, threads:[{id,title,author,ts}], error}
    QObject* loop = nullptr;                // this thread, for callbacks that come from another
    bool stEvents[2] = {false, false};
    std::string gwInitedFor;                // the url storage_gateway was last told
    struct StUpload { std::string job, file; int64_t size = 0; };
    std::map<std::string, StUpload> stSess;     // "<n>|<session>" -> what is uploading
    bool stPollArmed = false;

    explicit ReelcutCorePrivate(ReelcutCoreImpl* qq) : q(qq), loop(new QObject()) {}
    ~ReelcutCorePrivate() { delete loop; }

    std::string jobsFile() const { return rootDir() + "/jobs.json"; }

    Job* find(const std::string& id)
    {
        for (auto& j : jobs) if (j.id == id) return &j;
        return nullptr;
    }

    // ── persistence
    void load()
    {
        json j = json::parse(readFile(jobsFile()), nullptr, false);
        if (!j.is_object()) return;
        json s = j.contains("settings") ? j["settings"] : json::object();
        agent = jstr(s, "agent", agent);
        // "count" was the number of reels (6); now it proposes more, to choose from.
        count = (int)std::clamp<int64_t>(jnum(s, "candidates", count), 1, 30);
        readTuning(s);
        runnerPref = jstr(s, "runner", runnerPref);
        autoCut = jbool(s, "autoCut", autoCut);
        diarize = jbool(s, "diarize", diarize);
        lastPull = jnum(s, "lastPull", 0);
        if (!j.contains("jobs") || !j["jobs"].is_array()) return;
        for (auto& x : j["jobs"]) {
            Job b;
            b.id = jstr(x, "id", ""); b.url = jstr(x, "url", ""); b.file = jstr(x, "file", "");
            b.dir = jstr(x, "dir", ""); b.title = jstr(x, "title", ""); b.channel = jstr(x, "channel", "");
            b.created = jnum(x, "created", 0);
            b.duration = (double)jnum(x, "duration", 0);
            b.step = jstr(x, "step", "fetch"); b.status = jstr(x, "status", "failed"); b.error = jstr(x, "error", "");
            b.agent = jstr(x, "agent", ""); b.pickReply = jstr(x, "pickReply", "");
            b.clipsText = jstr(x, "clipsText", ""); b.fixes = jstr(x, "fixes", "");
            b.words = (int)jnum(x, "words", 0);
            if (x.contains("speakers") && x["speakers"].is_object()) b.spk = x["speakers"];
            b.speakersWanted = (int)std::clamp<int64_t>(jnum(x, "speakersWanted", 0), 0, 12);
            b.layout = jstr(x, "layout", "");
            b.vw = (int)jnum(x, "vw", 0); b.vh = (int)jnum(x, "vh", 0);
            // Jobs from before 0.1.5 say only their step.
            const bool early = b.step == "fetch", mid = early || b.step == "transcribe";
            b.fetched = jbool(x, "fetched", !early);
            b.transcribed = jbool(x, "transcribed", !mid);
            b.probed = jbool(x, "probed", !(mid || b.step == "diarize"));
            b.diar = jstr(x, "diar", mid || b.step == "diarize" ? "" : (jnum(b.spk, "found", 0) ? "done" : "off"));
            if (b.diar == "running") b.diar = "wanted";
            b.brief = jstr(x, "brief", "");
            b.intro = jstr(x, "intro", ""); b.outro = jstr(x, "outro", "");
            if (x.contains("stored") && x["stored"].is_object()) b.stored = x["stored"];
            if (x.contains("youtube") && x["youtube"].is_object()) b.yt = x["youtube"];
            b.ytError = jstr(x, "youtubeError", ""); b.ytReply = jstr(x, "youtubeReply", "");
            if (x.contains("forum") && x["forum"].is_object()) b.forum = x["forum"];
            if (x.contains("forumPosted") && x["forumPosted"].is_object()) b.forumPosted = x["forumPosted"];
            b.forumError = jstr(x, "forumError", "");
            if (x.contains("chosen") && x["chosen"].is_array())
                for (auto& n : x["chosen"]) if (n.is_number_integer()) b.chosen.push_back(n.get<int>());
            if (b.id.empty() || b.dir.empty()) continue;
            // Whatever ran when Basecamp closed did not finish.
            if (b.status == "running") { b.status = "cancelled"; b.error = "Stopped when Basecamp closed. Retry to run it again."; }
            jobs.push_back(b);
            loadReels(jobs.back());
            loadFrame(jobs.back());
        }
    }
    void save()
    {
        json arr = json::array();
        for (auto& b : jobs)
            arr.push_back({{"id", b.id}, {"url", b.url}, {"file", b.file}, {"dir", b.dir}, {"title", b.title},
                           {"channel", b.channel}, {"created", b.created}, {"duration", (int64_t)b.duration},
                           {"step", b.step}, {"status", b.status}, {"error", b.error}, {"agent", b.agent},
                           {"pickReply", b.pickReply}, {"clipsText", b.clipsText}, {"fixes", b.fixes},
                           {"words", b.words}, {"speakers", b.spk}, {"speakersWanted", b.speakersWanted},
                           {"layout", b.layout}, {"vw", b.vw}, {"vh", b.vh}, {"fetched", b.fetched},
                           {"transcribed", b.transcribed}, {"probed", b.probed}, {"diar", b.diar},
                           {"brief", b.brief}, {"chosen", b.chosen}, {"intro", b.intro}, {"outro", b.outro},
                           {"stored", b.stored}, {"youtube", b.yt}, {"youtubeError", b.ytError},
                           {"youtubeReply", b.ytReply}, {"forum", b.forum}, {"forumPosted", b.forumPosted},
                           {"forumError", b.forumError}});
        std::error_code ec;
        fs::create_directories(rootDir(), ec);
        json st = settingsJson();
        st["lastPull"] = lastPull;
        writeFile(jobsFile(), json({{"settings", st}, {"jobs", arr}}).dump(2));
    }
    json settingsJson() const
    {
        return {{"agent", agent}, {"candidates", count}, {"runner", runnerPref}, {"autoCut", autoCut}, {"diarize", diarize},
                {"autoCount", autoCount}, {"min", minLen}, {"max", maxLen}, {"lead", lead}, {"tail", tail}, {"fade", fade},
                {"card", card}, {"brief", brief}, {"intro", intro}, {"outro", outro}, {"captionMargin", captionMargin},
                {"colour", colour}, {"sayingColour", sayingColour}, {"saidColour", saidColour},
                {"storeTo", storeTo}, {"storeNode", storeNode}, {"forumAlias", forumAlias}};
    }
    static double jdbl(const json& o, const char* k, double def)
    {
        if (!o.is_object()) return def;
        auto it = o.find(k);
        return (it != o.end() && it->is_number()) ? it->get<double>() : def;
    }
    // The numbers, kept in range; for load and setSettings alike.
    void readTuning(const json& s)
    {
        autoCount = (int)std::clamp<int64_t>(jnum(s, "autoCount", autoCount), 1, 30);
        minLen = (int)std::clamp<int64_t>(jnum(s, "min", minLen), 5, 170);
        maxLen = (int)std::clamp<int64_t>(jnum(s, "max", maxLen), minLen + 5, 180);
        lead = std::clamp(jdbl(s, "lead", lead), 0.0, 5.0);
        tail = std::clamp(jdbl(s, "tail", tail), 0.0, 5.0);
        fade = std::clamp(jdbl(s, "fade", fade), 0.0, 3.0);
        card = std::clamp(jdbl(s, "card", card), 0.5, 10.0);
        captionMargin = (int)std::clamp<int64_t>(jnum(s, "captionMargin", captionMargin), -1, 1800);
        if (s.contains("storeTo") && s["storeTo"].is_string()) {
            const std::string t = s["storeTo"].get<std::string>();
            if (t == "" || t == "local" || t == "node") storeTo = t;
        }
        if (s.contains("storeNode") && s["storeNode"].is_string()) {
            std::string u = s["storeNode"].get<std::string>();
            while (!u.empty() && (u.back() == ' ' || u.back() == '/')) u.pop_back();
            while (!u.empty() && u.front() == ' ') u.erase(0, 1);
            if (!u.empty() && u.find("://") == std::string::npos) u = "http://" + u;
            if (u.empty() || u.rfind("http://", 0) == 0) storeNode = u;
        }
        for (auto [k, v] : {std::pair<const char*, std::string*>{"colour", &colour}, {"sayingColour", &sayingColour}, {"saidColour", &saidColour}})
            if (s.contains(k) && s[k].is_string()) *v = hexColour(s[k].get<std::string>());
        if (s.contains("brief") && s["brief"].is_string()) brief = s["brief"].get<std::string>().substr(0, 4000);
        // The name Reelcut posts under in the forums (with its key's fingerprint); "" = the key only.
        if (s.contains("forumAlias") && s["forumAlias"].is_string()) forumAlias = trim(s["forumAlias"].get<std::string>()).substr(0, 32);
        if (s.contains("intro") && s["intro"].is_string()) intro = s["intro"].get<std::string>();
        if (s.contains("outro") && s["outro"].is_string()) outro = s["outro"].get<std::string>();
    }

    // ── runner
    void detectRunner()
    {
        runner.clear(); runnerBin.clear(); runnerError.clear();
        auto tryRunner = [&](const std::string& r) {
            if (r == "podman" || r == "docker") {
                std::string bin = findExe(r.c_str());
                if (bin.empty()) return false;
                if (r == "docker") {
                    // A docker the user may not talk to (no docker group) is no runner.
                    QProcess p; p.setProcessEnvironment(processEnv());
                    p.start(QString::fromStdString(bin), {"info", "--format", "{{.ServerVersion}}"});
                    if (!p.waitForFinished(8000) || p.exitCode() != 0) return false;
                }
                runner = r; runnerBin = bin;
                return true;
            }
            if (r == "local") {
                std::string bin = findExe("reelcut");
                if (bin.empty()) return false;
                runner = r; runnerBin = bin;
                return true;
            }
            return false;
        };
        if (runnerPref != "auto") {
            if (!tryRunner(runnerPref))
                runnerError = runnerPref == "local" ? "There is no reelcut on PATH." : runnerPref + " is not installed or not usable here.";
        } else if (!tryRunner("podman") && !tryRunner("docker") && !tryRunner("local")) {
            runnerError = "Reelcut needs podman (or docker) for its container, or reelcut installed locally.";
        }
        imageChecked = false; imagePresent = runner == "local";
    }
    bool containerRunner() const { return runner == "podman" || runner == "docker"; }

    // The command for one reelcut step in a job's folder. Container paths are under /work.
    QStringList stepArgs(const Job& b, const QStringList& reelcutArgs, const std::string& name, const char* entrypoint)
    {
        QStringList a;
        if (!containerRunner()) return reelcutArgs;
        a << "run" << "--rm" << "--name" << QString::fromStdString(name)
          << "-v" << QString::fromStdString(b.dir) + (runner == "podman" ? ":/work:z" : ":/work");
        if (runner == "docker") a << "--user" << QString("%1:%2").arg(::getuid()).arg(::getgid());
        else a << "--userns" << "keep-id";
        if (entrypoint) a << "--entrypoint" << entrypoint;
        a << kImage << reelcutArgs;
        return a;
    }
    QString work(const Job& b, const std::string& rel) const
    {
        std::string base = containerRunner() ? "/work" : b.dir;
        return QString::fromStdString(rel.empty() ? base : base + "/" + rel);
    }

    // ── state
    void changed()
    {
        if (emitPending) return;
        emitPending = true;
        QTimer::singleShot(250, guarded("emit", [this] { emitPending = false; q->stateChanged(state().dump()); }));
    }
    void logLine(Job& b, const std::string& line)
    {
        std::string l = trim(line);
        if (l.empty()) return;
        b.log.push_back(l);
        while (b.log.size() > kLogLines) b.log.pop_front();
    }

    json state()
    {
        json js = json::array();
        // Newest first.
        for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
            auto& b = *it;
            json reels = json::array();
            const json here = storePlace().empty() || !b.stored.contains(storePlace()) ? json::object() : b.stored[storePlace()];
            for (auto& r : b.reels) {
                const std::string fname = fs::path(r.file).filename().string();
                auto st = b.storing.find(fname);
                const std::string key = std::to_string(r.n);
                reels.push_back({{"file", r.file}, {"name", r.name}, {"thumb", r.thumb}, {"seconds", r.seconds}, {"n", r.n},
                                 {"youtube", b.yt.contains(key) ? b.yt[key] : json(nullptr)},
                                 {"forumPost", jstr(postedIn(b), fs::path(r.file).filename().string().c_str(), "")},
                                 {"cid", jstr(here, fname.c_str(), "")},
                                 {"storing", st != b.storing.end() ? st->second : ""}});
            }
            auto setSt = b.storing.find("reels.json");
            json storage = {{"set", jstr(here, "reels.json", "")},
                            {"setStoring", setSt != b.storing.end() ? setSt->second : ""}};
            json log = json::array();
            for (auto& l : b.log) log.push_back(l);
            std::string page = b.dir + "/reels/index.html";
            std::error_code ec;
            js.push_back({{"id", b.id}, {"url", b.url}, {"file", b.file}, {"dir", b.dir}, {"title", b.title},
                          {"channel", b.channel}, {"created", b.created}, {"duration", b.duration},
                          {"step", b.step}, {"status", b.status}, {"error", b.error},
                          {"progress", {{"i", b.progressI}, {"n", b.progressN}}}, {"activity", b.activity},
                          {"log", log}, {"agent", b.agent}, {"pickTask", b.pickTask}, {"pickReply", b.pickReply},
                          {"clipsText", b.clipsText}, {"clips", clipsJson(parseClips(b.clipsText))},
                          {"fixes", b.fixes}, {"reels", reels}, {"words", b.words},
                          {"page", fs::exists(page, ec) ? page : ""},
                          {"speakers", b.spk}, {"speakersWanted", b.speakersWanted}, {"layout", b.layout},
                          {"width", b.vw}, {"height", b.vh}, {"frame", b.frame},
                          {"brief", b.brief}, {"chosen", b.chosen}, {"intro", b.intro}, {"outro", b.outro},
                          {"storage", storage},
                          {"forum", {{"target", b.forum}, {"state", b.forumState}, {"error", b.forumError}}},
                          {"youtube", {{"state", b.ytState}, {"error", b.ytError}, {"who", b.ytWho},
                                       {"reply", b.ytError.empty() ? "" : b.ytReply.substr(0, 4000)}}}});
        }
        return {
            {"ok", true},
            {"version", kVersion},
            {"root", rootDir()},
            {"runner", {{"kind", runner}, {"bin", runnerBin}, {"error", runnerError}, {"image", kImage},
                        {"imagePresent", imagePresent}}},
            {"settings", settingsJson()},
            {"agents", agents},
            {"shrooms", !findExe("shrooms-agent").empty()},
            {"localAgent", localAgent},
            {"thumbsOk", !viewDir.empty()},
            {"cards", cardsJson()},
            {"jobs", js},
            {"forumThreads", forumThreads},
            {"delivery", {{"state", dlState}, {"error", dlError}}},
            {"forumKey", fkey.ok() ? hexBytes(fkey.pk).substr(0, 16) : ""},
        };
    }

    // ── running: the steps before picking run side by side where they can
    //   "heavy"          transcribe, cut, previews, page: one at a time over all jobs (the CPU)
    //   "pull"           the reelcut image: one pull at a time
    //   "fetch/<id>"     the download (or the copy of a file); the audio comes first, then the video
    //   "diarize/<id>"   who speaks when: needs only the audio, runs beside transcribe (4 threads)
    //   "probe/<id>"     the video's size and one frame of it, for "who sits where"
    bool busy(const std::string& slot) const
    {
        auto it = procs.find(slot);
        return it != procs.end() && it->second.proc;
    }
    bool jobBusy(const std::string& id) const
    {
        for (auto& [k, r] : procs) if (r.proc && r.jobId == id) return true;
        return false;
    }
    static bool preparing(const Job& b) { return b.step == "fetch" || b.step == "transcribe" || b.step == "diarize"; }

    void schedule()
    {
        for (auto& b : jobs) {
            if (preparing(b)) { if (b.status == "queued" || b.status == "running") advance(b); }
            else if (b.status == "queued" && !busy("heavy")) runStep(b);
        }
    }

    void finishStep(Job& b, const std::string& error)
    {
        b.progressI = b.progressN = 0;
        b.activity.clear();
        if (!error.empty()) fail(b, error);
        save();
        changed();
        QTimer::singleShot(0, guarded("schedule", [this] { schedule(); }));
    }
    // A step that failed stops the rest of the job.
    void fail(Job& b, const std::string& error)
    {
        stopProcs(b.id, false);
        b.status = "failed"; b.error = error; b.activity.clear();
        if (b.diar == "running") b.diar = "";
    }
    // A process beside the queue ended: look at the job again.
    void sideDone()
    {
        save();
        changed();
        QTimer::singleShot(0, guarded("schedule", [this] { schedule(); }));
    }

    // Starts a process for the job in a slot; done(exitOk, stdout) runs when it ends (not when cancelled).
    void launch(Job& b, const std::string& slot, const std::string& bin, const QStringList& args, const std::string& container,
                std::function<void(bool, const std::string&)> done)
    {
        auto* p = new QProcess();
        p->setProcessEnvironment(processEnv());
        p->setWorkingDirectory(QString::fromStdString(b.dir));
        // Its own process group, so cancel stops reelcut's helpers (parakeet, ffmpeg) too.
        p->setChildProcessModifier([] { ::setsid(); });
        auto out = std::make_shared<std::string>();
        const std::string id = b.id;
        const bool heavySlot = slot == "heavy";
        // A container of the same name, stopped a moment ago, may still be on its way out.
        if (!container.empty() && containerRunner()) {
            QProcess rm; rm.setProcessEnvironment(processEnv());
            rm.start(QString::fromStdString(runnerBin), {"rm", "-f", "--ignore", QString::fromStdString(container)});
            rm.waitForFinished(20000);
        }
        procs[slot] = Running{p, id, container, false};
        b.status = "running";
        b.error.clear();
        QObject::connect(p, &QProcess::readyReadStandardOutput, guarded("stdout", [this, p, out, id] {
            QByteArray chunk = p->readAllStandardOutput();
            out->append(chunk.constData(), chunk.size());
            if (Job* j = find(id)) {
                for (auto& l : QString::fromUtf8(chunk).split('\n')) {
                    // cut prints one line per reel it wrote.
                    if (j->step == "cut" && l.contains(".mp4 ")) j->progressI++;
                    logLine(*j, l.toStdString());
                }
                changed();
            }
        }));
        QObject::connect(p, &QProcess::readyReadStandardError, guarded("stderr", [this, p, id, heavySlot] {
            QString chunk = QString::fromUtf8(p->readAllStandardError());
            Job* j = find(id);
            if (!j) return;
            // transcribe counts its pieces as "\ri/N".
            static const QRegularExpression prog("(\\d+)/(\\d+)\\s*$");
            for (auto& piece : chunk.split(QRegularExpression("[\r\n]"))) {
                auto m = prog.match(piece.trimmed());
                if (m.hasMatch() && piece.trimmed() == m.captured(0)) {
                    if (heavySlot) { j->progressI = m.captured(1).toInt(); j->progressN = m.captured(2).toInt(); }
                } else {
                    logLine(*j, piece.toStdString());
                }
            }
            if (heavySlot && j->status == "running" && preparing(*j)) describe(*j);
            changed();
        }));
        QObject::connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         guarded("finished", [this, p, out, id, slot, done](int code, QProcess::ExitStatus st) {
            p->deleteLater();
            auto it = procs.find(slot);
            bool cancelled = false;
            if (it != procs.end() && it->second.proc == p) { cancelled = it->second.cancelled; procs.erase(it); }
            else cancelled = true;   // replaced or stopped: its result no longer counts
            Job* j = find(id);
            if (!j) { QTimer::singleShot(0, guarded("schedule", [this] { schedule(); })); return; }
            if (cancelled) { save(); changed(); QTimer::singleShot(0, guarded("schedule", [this] { schedule(); })); return; }
            done(st == QProcess::NormalExit && code == 0, *out);
        }));
        QObject::connect(p, &QProcess::errorOccurred, guarded("start error", [this, p, id, bin, slot](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            p->deleteLater();
            auto it = procs.find(slot);
            if (it != procs.end() && it->second.proc == p) procs.erase(it);
            if (Job* j = find(id)) finishStep(*j, "Could not start " + bin + ".");
        }));
        p->start(QString::fromStdString(bin), args);
        changed();
    }

    // The audio is down: a complete audio.* (yt-dlp downloads to *.part, then renames), or the
    // whole download, or a copied file.
    bool audioReady(const Job& b) const
    {
        if (b.fetched) return true;
        if (!b.file.empty()) return false;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(b.dir, ec)) {
            std::string n = e.path().filename().string();
            if (n.rfind("audio.", 0) != 0) continue;
            std::string ext = e.path().extension().string();
            if (ext != ".part" && ext != ".ytdl" && ext != ".tmp" && ext != ".temp") return true;
        }
        return false;
    }

    // The steps before picking: download, transcribe as soon as the audio is down, tell the speakers
    // apart beside it, the frame once the video is down. Called whenever something changed.
    void advance(Job& b)
    {
        if (b.status != "queued" && b.status != "running") return;
        const std::string id = b.id;
        if (runner.empty()) { finishStep(b, runnerError.empty() ? "No way to run reelcut here." : runnerError); return; }
        if (b.diar.empty()) b.diar = diarize ? "wanted" : "off";
        // The image first: a new download looks for a newer one (once a day), so a fix in reelcut reaches an existing install.
        if (containerRunner()) {
            if (!imageChecked) {
                imageChecked = true;
                QProcess p; p.setProcessEnvironment(processEnv());
                p.start(QString::fromStdString(runnerBin), runner == "podman" ? QStringList{"image", "exists", kImage}
                                                                               : QStringList{"image", "inspect", kImage});
                imagePresent = p.waitForFinished(15000) && p.exitCode() == 0;
            }
            const bool stale = !b.fetched && b.file.empty() && !busy("fetch/" + id) &&
                               QDateTime::currentMSecsSinceEpoch() - lastPull > kImageMaxAgeMs;
            if (!imagePresent || stale) {
                if (!busy("pull")) pullImage(b);
                if (procs["pull"].jobId != id) { b.status = "queued"; describe(b); }
                return;
            }
        }
        // download
        if (!b.fetched && !busy("fetch/" + id)) startFetch(b);
        const bool audio = audioReady(b);
        if (audio && b.duration <= 0) readMeta(b);
        // transcribe: the CPU slot, shared with the other jobs
        if (audio && !b.transcribed && !busy("heavy")) startTranscribe(b);
        // who speaks, beside it (or after it, see startDiarize)
        if (b.diar == "after" && b.transcribed) b.diar = "wanted";
        // Side by side only with cores to spare: transcribe takes four, diarize four more. On a
        // 4-core machine the two together took longer than one after the other (257 s against 185 s).
        static const bool spare = std::thread::hardware_concurrency() >= 8;
        if (audio && b.diar == "wanted" && (spare || b.transcribed) && !busy("diarize/" + id)) startDiarize(b);
        // the frame, once the video is down
        if (b.fetched && !b.probed && (b.duration > 0 || b.transcribed) && !busy("probe/" + id)) startProbe(b);
        // The audio still downloading: look again in a moment.
        if (!audio && busy("fetch/" + id) && audioWatch.insert(id).second)
            QTimer::singleShot(2000, guarded("audio", [this, id] {
                audioWatch.erase(id);
                if (Job* j = find(id)) if (preparing(*j)) advance(*j);
            }));
        const bool diarDone = b.diar == "done" || b.diar == "failed" || b.diar == "off";
        if (b.fetched && b.transcribed && diarDone && b.probed && !jobBusy(id)) { prepared(b); return; }
        b.status = jobBusy(id) ? "running" : "queued";
        b.step = !b.fetched ? "fetch" : !b.transcribed ? "transcribe" : "diarize";
        describe(b);
        changed();
    }
    // What runs now, in one line: "Transcribing 3/8 · Telling the speakers apart · Downloading the video".
    void describe(Job& b)
    {
        const std::string id = b.id;
        std::vector<std::string> parts;
        auto h = procs.find("heavy");
        const bool transcribing = h != procs.end() && h->second.proc && h->second.jobId == id && preparing(b);
        if (busy("pull") && procs["pull"].jobId == id)
            parts.push_back(imagePresent ? "Looking for a newer reelcut image" : "Downloading the reelcut image (about 1 GB, once)");
        if (transcribing)
            parts.push_back(b.progressN ? "Transcribing " + std::to_string(b.progressI) + "/" + std::to_string(b.progressN) : "Transcribing");
        else if (preparing(b) && audioReady(b) && !b.transcribed && !busy("pull")) parts.push_back("Waiting to transcribe");
        if (busy("diarize/" + id)) {
            const int mins = std::max(1, (int)std::lround(b.duration / 3600.0 * 7));
            parts.push_back("Telling the speakers apart" + (b.duration > 0 ? " (about " + std::to_string(mins) + " min)" : std::string()));
        }
        if (busy("fetch/" + id)) parts.push_back(!b.file.empty() ? "Copying the video" : audioReady(b) ? "Downloading the video" : "Downloading the audio");
        if (busy("probe/" + id)) parts.push_back("Looking at the video");
        std::string a;
        for (auto& p : parts) a += (a.empty() ? "" : " · ") + p;
        if (!a.empty()) b.activity = a;
    }

    void pullImage(Job& b)
    {
        const bool had = imagePresent;
        launch(b, "pull", runnerBin, {"pull", kImage}, "", [this, id = b.id, had](bool ok, const std::string&) {
            Job* j = find(id);
            // Tried either way, so an offline machine doesn't try before every job.
            lastPull = QDateTime::currentMSecsSinceEpoch();
            forcePull = false;
            if (!ok && !had) { if (j) finishStep(*j, "Could not download the reelcut image. " + lastLog(*j)); return; }
            if (!ok && j) logLine(*j, "Could not look for a newer image; using the one already here.");
            imagePresent = true;
            // A cut that waited for the image runs now.
            if (j && !preparing(*j) && j->status == "running") j->status = "queued";
            sideDone();
        });
    }

    void startFetch(Job& b)
    {
        if (!b.file.empty()) {
            // A local video: copied into the job's folder, so the steps (and the container) see it.
            std::string src = b.file;
            std::string ext = fs::path(src).extension().string();
            if (ext.empty()) ext = ".mp4";
            launch(b, "fetch/" + b.id, "cp", {"--", QString::fromStdString(src), QString::fromStdString(b.dir + "/video" + ext)}, "",
                   [this, id = b.id, src](bool ok, const std::string&) {
                Job* j = find(id); if (!j) return;
                if (!ok) { finishStep(*j, "Could not copy " + src + ". " + lastLog(*j)); return; }
                if (j->title.empty()) j->title = fs::path(src).stem().string();
                j->fetched = true;
                sideDone();
            });
            return;
        }
        const std::string name = "reelcut-fetch-" + b.id;
        launch(b, "fetch/" + b.id, runnerBin, stepArgs(b, {"fetch", QString::fromStdString(b.url), work(b, "")}, name, nullptr), name,
               [this, id = b.id](bool ok, const std::string&) {
            Job* j = find(id); if (!j) return;
            if (!ok) { finishStep(*j, "Downloading failed. " + lastLog(*j)); return; }
            readMeta(*j);
            j->fetched = true;
            sideDone();
        });
    }

    void startTranscribe(Job& b)
    {
        const std::string name = "reelcut-" + b.id;
        b.progressI = b.progressN = 0;
        launch(b, "heavy", runnerBin, stepArgs(b, {"transcribe", work(b, "")}, name, nullptr), name,
               [this, id = b.id](bool ok, const std::string&) {
            Job* j = find(id); if (!j) return;
            if (!ok) { finishStep(*j, "Transcribing failed. " + lastLog(*j)); return; }
            countWords(*j);
            j->transcribed = true;
            j->progressI = j->progressN = 0;
            sideDone();
        });
    }

    // Who speaks when, into diarization.json; it labels the words if transcribe is done, else
    // transcribe does when it finishes.
    void startDiarize(Job& b)
    {
        const std::string name = "reelcut-diarize-" + b.id;
        QStringList args{"diarize", work(b, "")};
        if (b.speakersWanted > 0) args << "--speakers" << QString::number(b.speakersWanted);
        b.diar = "running";
        b.spk.erase("error");
        launch(b, "diarize/" + b.id, runnerBin, stepArgs(b, args, name, nullptr), name, [this, id = b.id](bool ok, const std::string&) {
            Job* j = find(id); if (!j) return;
            // Not fatal: the reels are cut without names.
            // A reelcut image from before 2026-10-14 needs the words first: once more after transcribing.
            if (!ok && !j->transcribed && !j->diarRetry) { j->diar = "after"; j->diarRetry = true; logLine(*j, "Telling the speakers apart again once the transcript is done."); }
            else if (!ok) { j->diar = "failed"; j->spk["error"] = "Could not tell the speakers apart. " + lastLog(*j); }
            else j->diar = "done";
            sideDone();
        });
    }

    // The video's size and one frame of it (40% in), for "who sits where".
    void startProbe(Job& b)
    {
        const std::string name = "reelcut-probe-" + b.id;
        const std::string w = work(b, "").toStdString();
        QString script = QString("v=$(ls %1/video.* 2>/dev/null | grep -v '\\.part$' | head -n 1); [ -n \"$v\" ] || exit 0; "
                                 "ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x \"$v\" > %1/.size; "
                                 "ffmpeg -loglevel error -y -ss %2 -i \"$v\" -frames:v 1 -vf scale=640:-2 %1/frame.jpg; exit 0")
                             .arg(shq(w), QString::number(b.duration > 0 ? (int)(b.duration * 0.4) : 60));
        QStringList args;
        std::string bin = runnerBin;
        if (containerRunner()) args = stepArgs(b, {"-c", script}, name, "sh");
        else { bin = "sh"; args = {"-c", script}; }
        launch(b, "probe/" + b.id, bin, args, name, [this, id = b.id](bool, const std::string&) {
            Job* j = find(id); if (!j) return;
            QStringList wh = QString::fromStdString(trim(readFile(j->dir + "/.size"))).split('x');
            if (wh.size() == 2) { j->vw = wh[0].toInt(); j->vh = wh[1].toInt(); }
            loadFrame(*j);
            j->probed = true;
            sideDone();
        });
    }

    // Everything before picking is done: name the speakers, or pick.
    void prepared(Job& b)
    {
        b.progressI = b.progressN = 0;
        b.activity.clear();
        if (b.diar == "done") {
            readSpeakers(b);
            // The transcript now says who speaks: the prompts made from the old one go.
            std::error_code ec;
            for (auto& e : fs::directory_iterator(b.dir, ec))
                if (e.path().filename().string().rfind("prompt-", 0) == 0) fs::remove(e.path(), ec);
            if (jnum(b.spk, "found", 0) >= 2) { b.step = "speakers"; b.status = "waiting"; finishStep(b, ""); return; }
        }
        toPicking(b);
    }

    // After picking: cut, previews, the page; one at a time over all jobs.
    void runStep(Job& b)
    {
        if (runner.empty()) { finishStep(b, runnerError.empty() ? "No way to run reelcut here." : runnerError); return; }
        if (containerRunner() && (!imagePresent || forcePull)) { if (!busy("pull")) pullImage(b); return; }
        const std::string name = "reelcut-" + b.id;
        if (b.step == "cut") {
            b.activity = "Cutting the reels";
            // New reels: what Storage holds is of the old ones.
            b.stored = json::object();
            b.storing.clear();
            std::error_code ec;
            fs::create_directories(b.dir + "/reels", ec);
            for (auto& e : fs::directory_iterator(b.dir + "/reels", ec))
                if (e.path().extension() == ".mp4" || e.path().extension() == ".ass" || e.path().extension() == ".jpg" || e.path().extension() == ".dur")
                    fs::remove(e.path(), ec);
            writeFile(b.dir + "/clips.txt", b.clipsText + "\n");
            QStringList args{"cut", work(b, ""), work(b, "clips.txt"), work(b, "reels")};
            for (auto& f : fixList(b.fixes)) args << "--fix" << QString::fromStdString(f);
            if (!b.layout.empty()) args << "--frame" << QString::fromStdString(b.layout);
            std::string who = speakersArg(b);
            if (!who.empty()) args << "--speakers" << QString::fromStdString(who);
            QStringList only;
            for (int n : b.chosen) only << QString::number(n);
            if (!only.isEmpty()) args << "--only" << only.join(",");
            args << "--lead" << QString::number(lead) << "--tail" << QString::number(tail) << "--fade" << QString::number(fade);
            // The cards go into the job's folder, where the container sees them.
            for (auto [flag, path, name] : {std::tuple<const char*, std::string, std::string>{"--intro", b.intro, "card-intro"},
                                            std::tuple<const char*, std::string, std::string>{"--outro", b.outro, "card-outro"}}) {
                if (path.empty() || !fs::exists(path, ec)) continue;
                const std::string dst = name + fs::path(path).extension().string();
                fs::copy_file(path, b.dir + "/" + dst, fs::copy_options::overwrite_existing, ec);
                if (!ec) args << flag << work(b, dst);
            }
            if (!b.intro.empty() || !b.outro.empty()) args << "--card" << QString::number(card);
            // The captions: only what was changed, so an older image still takes the rest.
            if (captionMargin >= 0) args << "--caption-margin" << QString::number(captionMargin);
            if (!colour.empty()) args << "--colour" << QString::fromStdString(colour);
            if (!sayingColour.empty()) args << "--saying-colour" << QString::fromStdString(sayingColour);
            if (!saidColour.empty()) args << "--said-colour" << QString::fromStdString(saidColour);
            b.progressN = b.chosen.empty() ? (int)parseClips(b.clipsText).size() : (int)b.chosen.size();
            launch(b, "heavy", runnerBin, stepArgs(b, args, name, nullptr), name, [this, id = b.id](bool ok, const std::string& out) {
                Job* j = find(id); if (!j) return;
                if (!ok && !j->newerTried && containerRunner() && logHas(*j, "unrecognized arguments")) {
                    // An image from before these settings: a newer one, then the cut again.
                    j->newerTried = true;
                    forcePull = true;
                    logLine(*j, "This reelcut image is older than the app's settings; looking for a newer one.");
                    j->status = "queued";
                    finishStep(*j, "");
                    return;
                }
                if (!ok) { finishStep(*j, "Cutting failed. " + lastLog(*j)); return; }
                (void)out;
                j->step = "thumbs"; j->status = "queued";
                finishStep(*j, "");
            });
        } else if (b.step == "thumbs") {
            b.activity = "Making previews";
            // One frame per reel for the view, and the page that plays them all.
            const QString reels = work(b, "reels");
            QString script = QString("for f in '%1'/*.mp4; do ffmpeg -loglevel error -y -ss 2 -i \"$f\" -frames:v 1 "
                                     "-vf scale=360:-2 \"${f%.mp4}.jpg\"; ffprobe -v error -show_entries format=duration "
                                     "-of csv=p=0 \"$f\" > \"${f%.mp4}.dur\"; done").arg(reels);
            QString title = QString::fromStdString(b.title.empty() ? "Reels" : b.title);
            QStringList args;
            std::string bin = runnerBin;
            if (containerRunner()) args = stepArgs(b, {"-c", script}, name, "sh");
            else { bin = "sh"; args = {"-c", script}; }
            launch(b, "heavy", bin, args, name, [this, id = b.id, title](bool ok, const std::string&) {
                Job* j = find(id); if (!j) return;
                if (!ok) logLine(*j, "Some previews could not be made.");
                loadReels(*j);
                pageThenDone(*j, title);
            });
        } else {
            finishStep(b, "Nothing to run at step " + b.step + ".");
        }
    }

    // After transcribing (and telling the speakers apart): an agent picks the clips, or the person.
    // A job that has clips already (the speakers were told apart again) keeps them.
    void toPicking(Job& j)
    {
        j.activity.clear();
        if (!j.clipsText.empty() || agent.empty()) { j.step = "clips"; j.status = "waiting"; finishStep(j, ""); return; }
        // Picking runs beside the queue, so the next job can start transcribing.
        j.step = "pick"; j.status = "waiting";
        finishStep(j, "");
        startPick(j, agent);
    }

    // diarization.json: [start, end, letter] per turn; transcript-ts.txt reads "[0:03:42] B: …".
    void readSpeakers(Job& j)
    {
        json names = j.spk.contains("names") && j.spk["names"].is_object() ? j.spk["names"] : json::object();
        json sides = j.spk.contains("sides") && j.spk["sides"].is_object() ? j.spk["sides"] : json::object();
        json talk = json::object();
        json turns = json::parse(readFile(j.dir + "/diarization.json"), nullptr, false);
        if (turns.is_array())
            for (auto& t : turns) {
                if (!t.is_array() || t.size() < 3 || !t[0].is_number() || !t[1].is_number() || !t[2].is_string()) continue;
                std::string who = t[2].get<std::string>();
                talk[who] = (talk.contains(who) ? talk[who].get<double>() : 0.0) + t[1].get<double>() - t[0].get<double>();
            }
        // An older reelcut labels only the words: each run of one speaker's words is a turn.
        if (!turns.is_array()) {
            json words = json::parse(readFile(j.dir + "/words.json"), nullptr, false);
            std::string cur;
            double t0 = 0, t1 = 0;
            auto close = [&] { if (!cur.empty()) talk[cur] = (talk.contains(cur) ? talk[cur].get<double>() : 0.0) + t1 - t0; };
            if (words.is_array())
                for (auto& w : words) {
                    if (!w.is_array() || w.size() < 4 || !w[0].is_number() || !w[1].is_number() || !w[3].is_string()) continue;
                    std::string who = w[3].get<std::string>();
                    if (who != cur) { close(); cur = who; t0 = w[0].get<double>(); }
                    t1 = w[1].get<double>();
                }
            close();
        }
        const int found = (int)talk.size();
        // A few lines each, spread over the recording, so the person can tell who is who.
        std::map<std::string, std::vector<std::string>> lines;
        static const QRegularExpression said("^\\[(\\d+:\\d\\d:\\d\\d)\\] ([A-Z]): (.*)$");
        std::istringstream in(readFile(j.dir + "/transcript-ts.txt"));
        std::string l;
        while (std::getline(in, l)) {
            auto m = said.match(QString::fromStdString(l));
            if (!m.hasMatch()) continue;
            QString text = m.captured(3).trimmed();
            if (text.split(' ', Qt::SkipEmptyParts).size() < 8) continue;
            if (text.size() > 180) text = text.left(177).trimmed() + "…";
            lines[m.captured(2).toStdString()].push_back("[" + m.captured(1).toStdString() + "] " + text.toStdString());
        }
        json samples = json::object();
        for (auto& [who, v] : lines) {
            json a = json::array();
            const size_t n = std::min<size_t>(3, v.size());
            for (size_t k = 0; k < n; k++) a.push_back(v[(k * 2 + 1) * v.size() / (n * 2)]);
            samples[who] = a;
        }
        j.spk = {{"found", found}, {"talk", talk}, {"samples", samples}, {"names", names}, {"sides", sides}};
        // Two people in a landscape recording (a call, an interview): one above the other.
        if (j.layout.empty() && found == 2 && j.vw > j.vh) j.layout = "stack";
        loadFrame(j);
    }

    // cut --speakers: "A=Corey Petty@left,B=Václav@right", the letters with a name or a side (left, middle, right).
    static std::string speakersArg(const Job& j)
    {
        if (jnum(j.spk, "found", 0) < 2) return "";
        json names = j.spk.contains("names") ? j.spk["names"] : json::object();
        json sides = j.spk.contains("sides") ? j.spk["sides"] : json::object();
        std::string out;
        for (char c = 'A'; c <= 'Z'; c++) {
            const char k[2] = {c, 0};
            std::string n = cleanName(jstr(names, k, "")), s = jstr(sides, k, "");
            if (s != "left" && s != "middle" && s != "right") s.clear();
            if (n.empty() && s.empty()) continue;
            if (!out.empty()) out += ",";
            out += std::string(k) + "=" + n + (s.empty() ? "" : "@" + s);
        }
        return out;
    }

    // For the agent: who A and B are.
    static std::string speakerNote(const Job& j)
    {
        if (jnum(j.spk, "found", 0) < 2) return "";
        json names = j.spk.contains("names") ? j.spk["names"] : json::object();
        std::string who;
        for (char c = 'A'; c <= 'Z'; c++) {
            const char k[2] = {c, 0};
            std::string n = cleanName(jstr(names, k, ""));
            if (n.empty()) continue;
            who += (who.empty() ? "" : ", ") + std::string(k) + " is " + n;
        }
        if (who.empty()) return "";
        return "Who speaks: the letters in the transcript are the speakers; " + who +
               ". Write their names in the speaker column.\n\n";
    }

    // ── cards: images shown before and after a reel (a λ, a QR code, a URL), the last few used
    static std::string cardsDir() { return rootDir() + "/.cards"; }
    static const size_t kCards = 8;
    std::vector<fs::path> cardFiles() const
    {
        std::vector<fs::path> v;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(cardsDir(), ec)) if (e.is_regular_file(ec)) v.push_back(e.path());
        // The most recently used first.
        std::sort(v.begin(), v.end(), [](const fs::path& a, const fs::path& b) {
            std::error_code e1, e2;
            return fs::last_write_time(a, e1) > fs::last_write_time(b, e2);
        });
        return v;
    }
    json cardsJson() const
    {
        json a = json::array();
        std::error_code ec;
        for (auto& f : cardFiles()) {
            std::string thumb;
            if (!viewDir.empty()) {
                // The view may load images only from its own folder.
                std::string dst = viewDir + "/cache/cards/" + f.filename().string();
                if (!fs::exists(dst, ec)) {
                    fs::create_directories(viewDir + "/cache/cards", ec);
                    fs::copy_file(f, dst, fs::copy_options::overwrite_existing, ec);
                }
                if (fs::exists(dst, ec)) thumb = dst;
            }
            std::string name = f.filename().string();
            auto dash = name.find('-');
            a.push_back({{"file", f.string()}, {"name", dash != std::string::npos ? name.substr(dash + 1) : name}, {"thumb", thumb}});
        }
        return a;
    }
    // A card in use stays in the library.
    static void touchCard(const std::string& path)
    {
        std::error_code ec;
        if (!path.empty() && path.rfind(cardsDir() + "/", 0) == 0 && fs::exists(path, ec))
            fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
    }

    void loadFrame(Job& b)
    {
        b.frame.clear();
        std::error_code ec;
        const std::string src = b.dir + "/frame.jpg";
        if (viewDir.empty() || !fs::exists(src, ec)) return;
        std::string cache = viewDir + "/cache/" + b.id;
        fs::create_directories(cache, ec);
        std::string dst = cache + "/frame.jpg";
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) b.frame = dst;
    }

    void pageThenDone(Job& b, const QString& title)
    {
        const std::string name = "reelcut-" + b.id;
        b.activity = "Writing the page";
        launch(b, "heavy", runnerBin, stepArgs(b, {"page", work(b, "reels"), "--title", title}, name, nullptr), name,
               [this, id = b.id](bool, const std::string&) {
            Job* j = find(id); if (!j) return;
            j->step = "done"; j->status = "done";
            finishStep(*j, "");
            if (Job* k = find(id)) storeReels(*k);
        });
    }

    // ── Logos Storage: the reels (and reels.json, the set) into a Storage node, as Settings says.
    // This computer's node is storage_module (the host's, on Basecamp 0.3); a node elsewhere is reached
    // through storage_gateway, which has the same API, so every call is written once.
    template <typename F> void withStorage(int n, F&& f)
    {
        if (n == 0) f(q->modules().storage_module);
        else f(q->modules().storage_gateway);
    }
    // Where the CIDs are kept: "local", or the node's url (another node holds other files).
    std::string storePlace() const { return storeTo == "local" ? "local" : storeTo == "node" ? storeNode : ""; }
    int storeNodeIndex() const { return storeTo == "local" ? 0 : 1; }
    void onLoop(std::function<void()> fn)
    {
        QMetaObject::invokeMethod(loop, guarded("storage", std::move(fn)), Qt::QueuedConnection);
    }
    static std::string sessKey(int n, const std::string& s) { return std::to_string(n) + "|" + s; }
    // Ready to upload: events registered once per module; the gateway told its node (and started).
    bool storageReady(int n, std::string& err)
    {
        try {
            withStorage(n, [&](auto& sm) {
                if (stEvents[n]) return;
                sm.onStorageUploadDone([this, n](const std::string& p) { onLoop([this, n, p] { uploadDone(n, json::parse(p, nullptr, false)); }); });
                stEvents[n] = true;
            });
        } catch (const std::exception& e) { err = std::string("Logos Storage is not available: ") + e.what(); return false; }
        if (n == 1 && gwInitedFor != storeNode) {
            bool ok = false;
            try { ok = q->modules().storage_gateway.init(json({{"url", storeNode}}).dump()); } catch (...) { ok = false; }
            if (!ok) { err = "That address does not look like a Storage node's (http://host:port)."; return false; }
            gwInitedFor = storeNode;
            q->modules().storage_gateway.startAsyncResult([](logos::AsyncResult<bool>) {}, 20000);
        }
        return true;
    }
    // Every reel not yet in the chosen node goes there; then the set (reels.json) once all are in.
    void storeReels(Job& b)
    {
        const std::string place = storePlace();
        if (place.empty() || b.reels.empty()) return;
        const int n = storeNodeIndex();
        std::string err;
        if (!storageReady(n, err)) { b.storing["reels.json"] = "failed: " + err; changed(); return; }
        if (!b.stored.contains(place) || !b.stored[place].is_object()) b.stored[place] = json::object();
        bool all = true;
        for (auto& r : b.reels) {
            const std::string fname = fs::path(r.file).filename().string();
            if (!jstr(b.stored[place], fname.c_str(), "").empty()) continue;
            all = false;
            auto st = b.storing.find(fname);
            if (st != b.storing.end() && st->second == "uploading") continue;
            upload(b, n, r.file, fname);
        }
        if (all && jstr(b.stored[place], "reels.json", "").empty() && b.storing["reels.json"] != "uploading") {
            // The set: what a person needs to fetch and show the reels, by CID.
            json set = {{"title", b.title}, {"source", b.url}, {"made", QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()},
                        {"reels", json::array()}};
            for (auto& r : b.reels) {
                const std::string fname = fs::path(r.file).filename().string();
                set["reels"].push_back({{"file", fname}, {"hook", r.name}, {"seconds", std::round(r.seconds * 10) / 10},
                                        {"cid", jstr(b.stored[place], fname.c_str(), "")}});
            }
            // Named by the job: the poll below finds uploads by name.
            const std::string path = b.dir + "/reels/reels-" + b.id + ".json";
            writeFile(path, set.dump(2) + "\n");
            upload(b, n, path, "reels.json");
        }
        save();
        changed();
    }
    void upload(Job& b, int n, const std::string& path, const std::string& fname)
    {
        b.storing[fname] = "uploading";
        const std::string id = b.id, name = fs::path(path).filename().string();
        std::error_code ec;
        const int64_t size = (int64_t)fs::file_size(path, ec);
        withStorage(n, [&](auto& sm) { sm.uploadUrlAsyncResult(path, 65536, true, [this, n, id, fname, name, size](logos::AsyncResult<StdLogosResult> ar) {
            const bool ok = ar.ok() && ar.value.success;
            const std::string sess = ok && ar.value.value.is_string() ? ar.value.value.get<std::string>() : "";
            const std::string err = !ar.ok() ? "Logos Storage did not answer (" + ar.error.code + ")."
                                             : (ar.value.error.empty() ? "Logos Storage refused the file." : ar.value.error);
            onLoop([this, n, id, fname, name, size, ok, sess, err] {
                Job* j = find(id); if (!j) return;
                if (!ok) { j->storing[fname] = "failed: " + err; changed(); return; }
                stSess[sessKey(n, sess)] = {id, fname, size};
                (void)name;
                schedulePoll();
            });
        }, 30000); });
    }
    void uploadDone(int n, const json& p)
    {
        auto it = stSess.find(sessKey(n, jstr(p, "sessionId", "")));
        if (it == stSess.end()) return;
        const std::string id = it->second.job, fname = it->second.file;
        stSess.erase(it);
        if (!jbool(p, "success", false)) { if (Job* j = find(id)) { j->storing[fname] = "failed: " + jstr(p, "error", "unknown error"); changed(); } return; }
        stored(id, fname, jstr(p, "cid", ""));
    }
    void stored(const std::string& id, const std::string& fname, const std::string& cid)
    {
        Job* j = find(id);
        if (!j || cid.empty()) return;
        const std::string place = storePlace();
        if (place.empty()) return;
        if (!j->stored.contains(place) || !j->stored[place].is_object()) j->stored[place] = json::object();
        j->stored[place][fname] = cid;
        j->storing.erase(fname);
        storeReels(*j);     // the next ones, or the set once all reels are in
        // All in: the reels can go to the forum, with their CIDs.
        if (fname == "reels.json" && jbool(j->forum, "auto", false) && !jstr(j->forum, "thread", "").empty()
            && j->forumState != "posting")
            postToForum(*j);
    }
    // Events can be lost: while uploads run, look at the node's list for the files by name.
    void schedulePoll()
    {
        if (stPollArmed) return;
        stPollArmed = true;
        QTimer::singleShot(3000, loop, guarded("storage poll", [this] {
            stPollArmed = false;
            if (stSess.empty() || storePlace().empty()) return;
            const int n = storeNodeIndex();
            withStorage(n, [&](auto& sm) { sm.manifestsAsyncResult([this, n](logos::AsyncResult<StdLogosResult> ar) {
                json arr = ar.ok() && ar.value.success ? ar.value.value : json();
                if (arr.is_string()) arr = json::parse(arr.get<std::string>(), nullptr, false);
                onLoop([this, n, arr] {
                    // name + size -> CID: a same-named file from before (a re-cut) differs in size.
                    std::map<std::pair<std::string, int64_t>, std::string> have;
                    if (arr.is_array())
                        for (auto& m : arr)
                            if (m.is_object()) have[{jstr(m, "filename", ""), (int64_t)jnum(m, "datasetSize", -1)}] = jstr(m, "cid", "");
                    std::vector<std::tuple<std::string, std::string, std::string>> done;   // key, job, cid
                    for (auto& [key, u] : stSess) {
                        if (key.rfind(std::to_string(n) + "|", 0) != 0) continue;
                        Job* j = find(u.job);
                        const std::string name = !j ? "" : u.file == "reels.json" ? "reels-" + j->id + ".json" : u.file;
                        auto h = have.find({name, u.size});
                        if (h != have.end()) done.push_back({key, u.job, h->second});
                    }
                    for (auto& [key, id, cid] : done) {
                        const std::string fname = stSess[key].file;
                        stSess.erase(key);
                        stored(id, fname, cid);
                    }
                    if (!stSess.empty()) schedulePoll();
                });
            }, 30000); });
        }));
    }

    // ── picking: an agent reads the transcript and answers with clip lines
    //   "auto", "local:claude", "local:pi": a session of Reelcut's own, in a cage, on this computer's
    //           shrooms-agent (its unix socket, ADR-056: no mesh needed). Claude Code uses the owner's
    //           Claude login, pi its own model settings. Without one, the person copies the prompt into a chat
    //   "MACHINE/SESSION": a session on the shrooms mesh, through the shrooms-agent CLI
    //   "": nobody, the person pastes the clips
    static std::string agentSocket()
    {
        if (const char* e = std::getenv("SHROOMS_AGENT_SOCKET")) if (*e) return e;
        if (const char* r = std::getenv("XDG_RUNTIME_DIR")) if (*r) return std::string(r) + "/shrooms-agent/agent.sock";
        return QDir::homePath().toStdString() + "/.local/state/shrooms-agent/agent.sock";
    }
    // One request to this computer's agent, over its socket; the reply's JSON (or null).
    json agentCall(const std::string& method, const std::string& path, const std::string& body, int timeoutS)
    {
        std::string curl = findExe("curl");
        if (curl.empty()) return json();
        QStringList a = {"-s", "--max-time", QString::number(timeoutS), "--unix-socket", QString::fromStdString(agentSocket()),
                         "-X", QString::fromStdString(method), "-H", "Content-Type: application/json"};
        if (!body.empty()) a << "--data-binary" << "@-";
        a << QString::fromStdString("http://agent" + path);
        QProcess p;
        p.setProcessEnvironment(processEnv());
        p.start(QString::fromStdString(curl), a);
        if (!body.empty()) { p.write(body.data(), (qint64)body.size()); }
        p.closeWriteChannel();
        if (!p.waitForFinished((timeoutS + 2) * 1000)) { p.kill(); return json(); }
        return json::parse(p.readAllStandardOutput().toStdString(), nullptr, false);
    }
    // A caged Claude Code can mark its task done with the shrooms tool, and that tool waits for an
    // approval (shrooms-agent allows only list_agents and task_status unasked). When that is what our
    // own session waits for, for this task: approve it, and its summary is the answer. Else "".
    std::string approveTaskUpdate(const std::string& session, const std::string& task)
    {
        std::string curl = findExe("curl");
        if (curl.empty()) return "";
        int64_t last = 0;
        json ss = agentCall("GET", "/v1/sessions", "", 10);
        if (ss.is_object() && ss.contains("sessions") && ss["sessions"].is_array())
            for (auto& x : ss["sessions"]) if (jstr(x, "name", "") == session) last = jnum(x, "last_seq", 0);
        // The event stream stays open: read what is there for a moment.
        QProcess p;
        p.setProcessEnvironment(processEnv());
        p.start(QString::fromStdString(curl),
                {"-s", "-N", "--max-time", "3", "--unix-socket", QString::fromStdString(agentSocket()),
                 QString::fromStdString("http://agent/v1/sessions/" + session + "/events?since=" + std::to_string(std::max<int64_t>(0, last - 40)))});
        if (!p.waitForFinished(6000)) p.kill();
        std::string rid, summary, state;
        std::istringstream in(p.readAllStandardOutput().toStdString());
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("data:", 0) != 0) continue;
            json e = json::parse(line.substr(5), nullptr, false);
            if (!e.is_object() || !e.contains("data") || !e["data"].is_object()) continue;
            const json& dt = e["data"];
            if (jstr(dt, "type", "") != "control_request" || !dt.contains("request") || !dt["request"].is_object()) continue;
            const json& rq = dt["request"];
            if (jstr(rq, "subtype", "") != "can_use_tool" || jstr(rq, "tool_name", "") != "mcp__shrooms__task_update") continue;
            if (!rq.contains("input") || !rq["input"].is_object() || jstr(rq["input"], "task", "") != task) continue;
            rid = jstr(dt, "request_id", "");
            summary = jstr(rq["input"], "summary", "");
            state = jstr(rq["input"], "state", "");
        }
        if (rid.empty() || state != "done" || trim(summary).empty()) return "";
        agentCall("POST", "/v1/sessions/" + session + "/prompts/" + rid, json({{"allow", true}}).dump(), 10);
        return summary;
    }
    // Whether "auto" can ask a local agent: a shrooms-agent on its socket, with pi and cages.
    json localAgent;
    void detectLocalAgent()
    {
        std::error_code ec;
        std::string why;
        json h;
        if (!fs::exists(agentSocket(), ec)) why = "no shrooms-agent on this computer";
        else if (findExe("curl").empty()) why = "curl is not installed";
        else if (!(h = agentCall("GET", "/v1/harnesses", "", 5)).is_object()) why = "shrooms-agent does not answer on its socket";
        else {
            bool cage = h.contains("cage") && jbool(h["cage"], "available", false);
            if (!cage) why = "shrooms-agent here cannot make cages (no podman)";
        }
        json hs = json::array();
        if (h.is_object() && h.contains("harnesses") && h["harnesses"].is_array())
            for (auto& x : h["harnesses"]) { std::string n = jstr(x, "name", ""); if (n == "claude" || n == "pi") hs.push_back(n); }
        if (why.empty() && hs.empty()) why = "shrooms-agent here has neither Claude Code nor pi";
        localAgent = {{"ok", why.empty()}, {"why", why}, {"harnesses", hs}};
    }
    // Which harness a local choice means, of those this computer's agent runs; "" when none fits.
    std::string localHarness(const std::string& choice)
    {
        auto has = [&](const char* n) {
            if (localAgent.is_object() && localAgent.contains("harnesses") && localAgent["harnesses"].is_array())
                for (auto& x : localAgent["harnesses"]) if (x.is_string() && x.get<std::string>() == n) return true;
            return false;
        };
        if (choice == "local:claude") return has("claude") ? "claude" : "";
        if (choice == "local:pi") return has("pi") ? "pi" : "";
        if (choice == "auto") return has("claude") ? "claude" : has("pi") ? "pi" : "";
        return "";
    }
    static std::string harnessLabel(const std::string& h)
    {
        return h == "claude" ? "Claude Code on this computer" : "pi on this computer";
    }
    static std::string localSessionName(const std::string& h) { return std::string(kLocalSession) + "-" + h; }
    // Reelcut's own session on this computer for a harness, caged, made the first time.
    std::string ensureLocalSession(const std::string& harness)
    {
        const std::string name = localSessionName(harness);
        json ss = agentCall("GET", "/v1/sessions", "", 10);
        if (ss.is_object() && ss.contains("sessions") && ss["sessions"].is_array())
            for (auto& x : ss["sessions"]) if (jstr(x, "name", "") == name) return "";
        json made = agentCall("POST", "/v1/sessions",
            json({{"name", name}, {"dir", rootDir() + "/.picker"}, {"harness", harness}, {"cage", json::object()}}).dump(), 30);
        if (made.is_object() && jstr(made, "name", "") == name) return "";
        return "Could not make a " + harness + " session on this computer's shrooms-agent: " +
               (made.is_object() ? jstr(made, "error", made.dump().substr(0, 200)) : std::string("no answer"));
    }

    void startPick(Job& b, const std::string& who)
    {
        if (who.empty()) { b.step = "clips"; b.status = "waiting"; save(); changed(); return; }
        if (runner.empty()) { b.status = "failed"; b.error = runnerError; save(); changed(); return; }
        const bool local = who == "auto" || who.rfind("local:", 0) == 0;
        std::string harness;
        if (local) { detectLocalAgent(); if (jbool(localAgent, "ok", false)) harness = localHarness(who); }
        std::string shrooms = findExe("shrooms-agent");
        if (local ? harness.empty() : shrooms.empty()) {
            // Nothing to ask: the person copies the prompt into any chat (the view shows how).
            b.step = "clips"; b.status = "waiting";
            b.error = local ? "" : "No agent could pick: shrooms-agent is not installed here. Copy the prompt into any AI chat instead.";
            save(); changed(); return;
        }
        const std::string label = local ? harnessLabel(harness) : who;
        b.step = "pick"; b.status = "running"; b.agent = who; b.error.clear(); b.pickTask.clear(); b.pickReply.clear();
        b.activity = "Asking " + label + " for " + std::to_string(count) + " clips to choose from";
        save(); changed();
        // First the prompt (reelcut prompt prints the instructions and the transcript)...
        QStringList pa = promptArgs(b);
        QStringList args = containerRunner() ? stepArgs(b, pa, "reelcut-prompt-" + b.id, nullptr) : pa;
        auto* p = new QProcess();
        p->setProcessEnvironment(processEnv());
        picks[b.id] = p;
        QObject::connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         guarded("prompt", [this, p, id = b.id, who, label, local, harness, shrooms](int code, QProcess::ExitStatus st) {
            p->deleteLater();
            std::string prompt = p->readAllStandardOutput().toStdString();
            Job* j = find(id);
            if (!j || j->status != "running" || j->step != "pick") return;
            if (st != QProcess::NormalExit || code != 0 || prompt.empty()) {
                pickFailed(*j, "Could not build the prompt: " + trim(p->readAllStandardError().toStdString()));
                return;
            }
            std::string ask = "From the Reelcut app on Basecamp (" + QSysInfo::machineHostName().toStdString() +
                "). Pick the clips below and reply with ONLY the clip lines in the format given, "
                "one per line, nothing else. Your reply is parsed by a program.\n\n" + speakerNote(*j) + prompt;
            if (!local && ask.size() > kMaxPrompt) {
                pickFailed(*j, "The transcript is too long to send to an agent in one message (" +
                           std::to_string(ask.size() / 1024) + " KiB). Copy the prompt into an AI chat instead.");
                return;
            }
            if (local) askLocal(*j, harness, ask);
            else askMesh(*j, who, ask, shrooms);
        }));
        p->start(QString::fromStdString(runnerBin), args);
        (void)label;
    }

    // reelcut prompt for a job: how many, how long, and the brief (from a file: it can be long).
    QStringList promptArgs(const Job& b)
    {
        QStringList pa = {"prompt", work(b, ""), "-n", QString::number(count), "--min", QString::number(minLen),
                          "--max", QString::number(maxLen)};
        std::error_code ec;
        if (!trim(b.brief).empty()) { writeFile(b.dir + "/brief.txt", b.brief + "\n"); pa << "--brief-file" << work(b, "brief.txt"); }
        else fs::remove(b.dir + "/brief.txt", ec);
        return pa;
    }

    // This computer's agent: the task sent without blocking, then polled, so a slow first turn (the
    // cage's image is built then) is waited for as long as it takes.
    void askLocal(Job& b, const std::string& harness, const std::string& ask, qint64 since = 0)
    {
        if (!since) since = QDateTime::currentMSecsSinceEpoch();
        std::string err = ensureLocalSession(harness);
        if (!err.empty()) { pickFailed(b, err); return; }
        // The first caged session builds the agent's image, and it takes no messages until that is done.
        json h = agentCall("GET", "/v1/harnesses", "", 5);
        json cage = h.is_object() && h.contains("cage") ? h["cage"] : json();
        if (!jbool(cage, "ready", true)) {
            std::string why = jstr(cage, "error", "");
            if (!why.empty() && !jbool(cage, "building", false)) { pickFailed(b, "The agent's cage could not be built: " + why); return; }
            if (QDateTime::currentMSecsSinceEpoch() - since > 45LL * 60 * 1000) { pickFailed(b, "The agent's cage was still being built after 45 minutes."); return; }
            b.activity = "Preparing the agent's cage (a few minutes, only the first time)";
            changed();
            QTimer::singleShot(15000, guarded("cage wait", [this, id = b.id, harness, ask, since] {
                Job* j = find(id);
                if (j && j->step == "pick" && j->status == "running") askLocal(*j, harness, ask, since);
            }));
            return;
        }
        json msg = {{"messageId", "reelcut-" + b.id + "-" + std::to_string(QDateTime::currentMSecsSinceEpoch())},
                    {"role", "ROLE_USER"}, {"parts", json::array({{{"text", ask}}})},
                    {"metadata", {{"shrooms/title", "Pick reel clips: " + (b.title.empty() ? b.id : b.title)}}}};
        json rq = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "SendMessage"},
                   {"params", {{"message", msg}, {"configuration", {{"blocking", false}}}}}};
        json r = agentCall("POST", "/a2a/" + localSessionName(harness), rq.dump(), 30);
        json t = r.is_object() && r.contains("result") && r["result"].is_object() && r["result"].contains("task") ? r["result"]["task"] : json();
        std::string task = jstr(t, "id", "");
        if (task.empty()) {
            std::string e = r.is_object() && r.contains("error") ? jstr(r["error"], "message", "") : "no answer";
            pickFailed(b, "Could not ask " + harnessLabel(harness) + ": " + e);
            return;
        }
        b.pickTask = task;
        b.activity = harnessLabel(harness) + " is picking the clips";
        save(); changed();
        pollLocal(b.id, harness, task, QDateTime::currentMSecsSinceEpoch());
    }
    static std::string taskState(const json& t)
    {
        std::string st = t.is_object() && t.contains("status") ? jstr(t["status"], "state", "") : "";
        st = QString::fromStdString(st).toLower().remove("task_state_").toStdString();
        return st;
    }
    static std::string taskText(const json& t)
    {
        std::string out;
        if (!t.is_object() || !t.contains("status") || !t["status"].is_object()) return out;
        auto& s = t["status"];
        if (s.contains("message") && s["message"].is_object() && s["message"].contains("parts") && s["message"]["parts"].is_array())
            for (auto& part : s["message"]["parts"]) out += jstr(part, "text", "") + "\n";
        return out;
    }
    void pollLocal(const std::string& id, const std::string& harness, const std::string& task, qint64 since)
    {
        QTimer::singleShot(8000, guarded("poll", [this, id, harness, task, since] {
            const std::string label = harnessLabel(harness);
            Job* j = find(id);
            if (!j || j->step != "pick" || j->status != "running" || j->pickTask != task) return;
            json r = agentCall("POST", "/a2a", json({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "GetTask"}, {"params", {{"id", task}}}}).dump(), 15);
            json t = r.is_object() && r.contains("result") && r["result"].is_object() && r["result"].contains("task") ? r["result"]["task"] : json();
            std::string st = taskState(t);
            // A worker that cannot mark its task done (no shrooms tools in its cage) still answers:
            // its turn has ended (the session is idle) and the answer is the task's status message.
            if (st == "working" && !trim(taskText(t)).empty()) {
                json ss = agentCall("GET", "/v1/sessions", "", 10);
                const std::string session = task.substr(0, task.find(':'));
                if (ss.is_object() && ss.contains("sessions") && ss["sessions"].is_array())
                    for (auto& x : ss["sessions"])
                        if (jstr(x, "name", "") == session && jstr(x, "state", "") == "idle") st = "completed";
            }
            // Its answer waits in a task_update for an approval: take it (approving closes the task).
            std::string viaUpdate;
            if (st == "input_required" && !(viaUpdate = approveTaskUpdate(task.substr(0, task.find(':')), task)).empty()) {
                agentCall("POST", "/a2a", json({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "AckTask"}, {"params", {{"id", task}}}}).dump(), 10);
                finishPick(*j, label, viaUpdate, "");
                return;
            }
            if (st == "completed") {
                // Close a task its worker left open, or the session never takes the next one.
                if (taskState(t) == "working")
                    agentCall("POST", "/v1/tasks/" + task, json({{"state", "done"}, {"summary", "Reelcut took the clips."},
                                                                 {"session", task.substr(0, task.find(':'))}}).dump(), 10);
                agentCall("POST", "/a2a", json({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "AckTask"}, {"params", {{"id", task}}}}).dump(), 10);
                finishPick(*j, label, taskText(t), "");
            } else if (st == "failed" || st == "canceled" || st == "rejected" || st == "input_required") {
                std::string text = trim(taskText(t));
                finishPick(*j, label, st == "input_required" ? "" : text,
                           st == "input_required" ? "it asked: " + text : "its task " + st + (text.empty() ? "" : ": " + text));
            } else if (QDateTime::currentMSecsSinceEpoch() - since > 60LL * 60 * 1000) {
                pickFailed(*j, label + " did not answer within an hour. Copy the prompt into an AI chat instead.");
            } else {
                pollLocal(id, harness, task, since);
            }
        }));
    }

    // A session on the mesh, through the CLI; --wait holds for up to ten minutes, then it is polled.
    void askMesh(Job& b, const std::string& who, const std::string& ask, const std::string& shrooms)
    {
        auto* a = new QProcess();
        a->setProcessEnvironment(processEnv());
        picks[b.id] = a;
        QObject::connect(a, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         guarded("pick", [this, a, id = b.id, who, shrooms](int code, QProcess::ExitStatus st) {
            a->deleteLater();
            std::string out = a->readAllStandardOutput().toStdString();
            std::string err = trim(a->readAllStandardError().toStdString());
            Job* j = find(id);
            if (!j || j->step != "pick" || j->status != "running") return;
            meshReply(*j, who, shrooms, out, st == QProcess::NormalExit && code == 0 ? "" : (err.empty() ? trim(out) : err));
        }));
        a->start(QString::fromStdString(shrooms),
                 {"a2a", "send", "--wait", "--title", "Pick reel clips", QString::fromStdString(who), QString::fromStdString(ask)});
    }
    // The CLI prints "task MACHINE/SESSION:ID: state", then what the session said.
    void meshReply(Job& j, const std::string& who, const std::string& shrooms, const std::string& out, const std::string& err)
    {
        if (!err.empty()) { finishPick(j, who, "", err); return; }
        static const QRegularExpression head("^task (\\S+): ([\\w-]+)");
        auto m = head.match(QString::fromStdString(out));
        std::string task = m.hasMatch() ? m.captured(1).toStdString() : "";
        std::string st = m.hasMatch() ? m.captured(2).toStdString() : "completed";
        if (!task.empty()) j.pickTask = task;
        if (st == "completed" || st == "failed" || st == "canceled" || st == "rejected" || st == "input-required" || task.empty()) {
            if (!task.empty() && st == "completed") detached(shrooms, {"a2a", "ack", QString::fromStdString(task)});
            finishPick(j, who, out, st == "completed" ? "" : who + "'s task " + st + ".");
            return;
        }
        // Still working: look again.
        j.activity = who + " is still picking";
        changed();
        QTimer::singleShot(20000, guarded("mesh poll", [this, id = j.id, who, shrooms, task] {
            Job* k = find(id);
            if (!k || k->step != "pick" || k->status != "running" || k->pickTask != task) return;
            auto* g = new QProcess();
            g->setProcessEnvironment(processEnv());
            picks[id] = g;
            QObject::connect(g, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                             guarded("mesh get", [this, g, id, who, shrooms](int code, QProcess::ExitStatus st) {
                g->deleteLater();
                Job* k2 = find(id);
                if (!k2 || k2->step != "pick" || k2->status != "running") return;
                std::string out = g->readAllStandardOutput().toStdString();
                meshReply(*k2, who, shrooms, out, st == QProcess::NormalExit && code == 0 ? "" : trim(g->readAllStandardError().toStdString()));
            }));
            g->start(QString::fromStdString(shrooms), {"a2a", "get", QString::fromStdString(task)});
        }));
    }

    void finishPick(Job& j, const std::string& who, const std::string& reply, const std::string& err)
    {
        picks.erase(j.id);
        j.pickReply = reply;
        auto clips = parseClips(reply);
        if (!err.empty() || clips.empty()) {
            pickFailed(j, !err.empty() ? "Asking " + who + " failed: " + err
                                       : who + " answered, but with no clip lines. Its reply is below; copy the prompt into an AI chat, or ask again.");
            return;
        }
        // Only the clip lines go into the list the person sees and edits.
        j.clipsText = clipsText(clips);
        j.activity.clear();
        // Best first: the top ones are ticked, and cut straight away when the settings say so.
        j.chosen.clear();
        for (size_t k = 0; k < clips.size() && (int)k < autoCount; k++) j.chosen.push_back(clips[k].n);
        if (autoCut) { j.step = "cut"; j.status = "queued"; }
        else { j.step = "clips"; j.status = "waiting"; }
        save(); changed();
        schedule();
    }
    void pickFailed(Job& b, const std::string& why)
    {
        picks.erase(b.id);
        b.step = "clips"; b.status = "waiting"; b.error = why; b.activity.clear();
        save(); changed();
    }

    // ── YouTube texts (issue #4): a title, a description and tags per reel, keyword-minded
    std::string youtubePrompt(const Job& b)
    {
        const std::string transcript = readFile(b.dir + "/transcript-ts.txt");
        auto clips = parseClips(b.clipsText);
        std::string out =
            "Write YouTube Shorts texts for the reels below, cut from one long video. For each reel:\n"
            "- TITLE: at most 70 characters, specific and searchable: the keywords a viewer would type, "
            "the speaker's name when they are known, no clickbait, no ALL CAPS, at most one emoji.\n"
            "- DESCRIPTION: 2 to 4 short paragraphs. The first line hooks and holds the main keywords "
            "(YouTube shows only it). Then what is said, in plain words, and a line pointing to the full "
            "video" + std::string(b.url.empty() ? "" : " (" + b.url + ")") + ". End with 2 or 3 hashtags, "
            "#Shorts among them.\n"
            "- TAGS: 8 to 15 comma-separated search phrases, most specific first.\n"
            "Write in the language the reel is spoken in. Use only what the transcript says: no invented facts, "
            "numbers or claims.\n\n"
            "Reply with ONLY these blocks, one per reel, nothing before or after (a program reads them):\n"
            "=== REEL N ===\nTITLE: …\nDESCRIPTION:\n…\nTAGS: …\n\n";
        out += "THE VIDEO: " + (b.title.empty() ? std::string("(no title)") : b.title) +
               (b.channel.empty() ? "" : " - " + b.channel) + "\n";
        if (!trim(b.brief).empty()) out += "WHAT THE MAKER IS AFTER: " + trim(b.brief) + "\n";
        // Who is who; the note's last sentence is for picking clips, not for these texts.
        out += QString::fromStdString(speakerNote(b)).replace(" Write their names in the speaker column.", "").toStdString() + "\n";
        for (auto& r : b.reels) {
            const Clip* c = nullptr;
            for (auto& x : clips) if (x.n == r.n) c = &x;
            if (!c) continue;
            out += "=== REEL " + std::to_string(r.n) + " === (" + std::to_string((int)std::lround(r.seconds)) + " s)\n";
            out += "Hook: " + c->hook + "\n";
            if (!trim(c->summary).empty()) out += "About: " + trim(c->summary) + "\n";
            out += "What is said:\n" + clipLines(transcript, *c) + "\n";
        }
        return out;
    }

    // Ask an agent for a text and hear back once: this computer's caged agent ("auto", "local:…") or a
    // mesh session (MACHINE/SESSION, through the CLI). done(reply, error) runs on the event loop, unless
    // stillWanted() says the asker moved on.
    using Reply = std::function<void(const std::string&, const std::string&)>;
    void askAgent(const std::string& who, const std::string& title, const std::string& ask,
                  std::function<bool()> stillWanted, std::function<void(const std::string&)> onTask, Reply done)
    {
        const bool local = who == "auto" || who.rfind("local:", 0) == 0;
        if (local) {
            detectLocalAgent();
            const std::string harness = jbool(localAgent, "ok", false) ? localHarness(who) : "";
            if (harness.empty()) { done("", "No agent on this computer: " + jstr(localAgent, "why", "shrooms-agent is not here")); return; }
            std::string err = ensureLocalSession(harness);
            if (!err.empty()) { done("", err); return; }
            json h = agentCall("GET", "/v1/harnesses", "", 5);
            if (h.is_object() && h.contains("cage") && !jbool(h["cage"], "ready", true)) {
                done("", "The agent's cage is still being prepared; try again in a few minutes.");
                return;
            }
            json msg = {{"messageId", "reelcut-yt-" + std::to_string(QDateTime::currentMSecsSinceEpoch())},
                        {"role", "ROLE_USER"}, {"parts", json::array({{{"text", ask}}})},
                        {"metadata", {{"shrooms/title", title}}}};
            json rq = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "SendMessage"},
                       {"params", {{"message", msg}, {"configuration", {{"blocking", false}}}}}};
            json r = agentCall("POST", "/a2a/" + localSessionName(harness), rq.dump(), 30);
            json t = r.is_object() && r.contains("result") && r["result"].is_object() && r["result"].contains("task") ? r["result"]["task"] : json();
            std::string task = jstr(t, "id", "");
            if (task.empty()) { done("", "Could not ask " + harnessLabel(harness) + "."); return; }
            onTask(task);
            pollAsk(task, QDateTime::currentMSecsSinceEpoch(), stillWanted, done);
            return;
        }
        std::string shrooms = findExe("shrooms-agent");
        if (shrooms.empty()) { done("", "shrooms-agent is not installed here."); return; }
        if (ask.size() > kMaxPrompt) { done("", "Too long to send to an agent in one message: copy the prompt into an AI chat instead."); return; }
        auto* a = new QProcess();
        a->setProcessEnvironment(processEnv());
        QObject::connect(a, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         guarded("ask", [this, a, who, shrooms, stillWanted, onTask, done](int code, QProcess::ExitStatus st) {
            a->deleteLater();
            if (!stillWanted()) return;
            std::string out = a->readAllStandardOutput().toStdString();
            std::string err = trim(a->readAllStandardError().toStdString());
            if (st != QProcess::NormalExit || code != 0) { done("", err.empty() ? trim(out) : err); return; }
            meshAsk(who, shrooms, out, stillWanted, onTask, done);
        }));
        a->start(QString::fromStdString(shrooms),
                 {"a2a", "send", "--wait", "--title", QString::fromStdString(title), QString::fromStdString(who), QString::fromStdString(ask)});
    }
    void pollAsk(const std::string& task, qint64 since, std::function<bool()> stillWanted, Reply done)
    {
        QTimer::singleShot(8000, guarded("ask poll", [this, task, since, stillWanted, done] {
            if (!stillWanted()) return;
            json r = agentCall("POST", "/a2a", json({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "GetTask"}, {"params", {{"id", task}}}}).dump(), 15);
            json t = r.is_object() && r.contains("result") && r["result"].is_object() && r["result"].contains("task") ? r["result"]["task"] : json();
            std::string st = taskState(t);
            const std::string session = task.substr(0, task.find(':'));
            // A caged worker can't mark its task done: an idle session with an answer has answered.
            if (st == "working" && !trim(taskText(t)).empty()) {
                json ss = agentCall("GET", "/v1/sessions", "", 10);
                if (ss.is_object() && ss.contains("sessions") && ss["sessions"].is_array())
                    for (auto& x : ss["sessions"]) if (jstr(x, "name", "") == session && jstr(x, "state", "") == "idle") st = "completed";
            }
            std::string viaUpdate;
            if (st == "input_required" && !(viaUpdate = approveTaskUpdate(session, task)).empty()) {
                agentCall("POST", "/a2a", json({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "AckTask"}, {"params", {{"id", task}}}}).dump(), 10);
                done(viaUpdate, "");
                return;
            }
            if (st == "completed") {
                if (taskState(t) == "working")
                    agentCall("POST", "/v1/tasks/" + task, json({{"state", "done"}, {"summary", "Reelcut took the texts."}, {"session", session}}).dump(), 10);
                agentCall("POST", "/a2a", json({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "AckTask"}, {"params", {{"id", task}}}}).dump(), 10);
                done(taskText(t), "");
            } else if (st == "failed" || st == "canceled" || st == "rejected" || st == "input_required") {
                done("", "its task " + st + (trim(taskText(t)).empty() ? "" : ": " + trim(taskText(t))));
            } else if (QDateTime::currentMSecsSinceEpoch() - since > 30LL * 60 * 1000) {
                done("", "no answer within half an hour.");
            } else {
                pollAsk(task, since, stillWanted, done);
            }
        }));
    }
    void meshAsk(const std::string& who, const std::string& shrooms, const std::string& out,
                 std::function<bool()> stillWanted, std::function<void(const std::string&)> onTask, Reply done)
    {
        static const QRegularExpression head("^task (\\S+): ([\\w-]+)");
        auto m = head.match(QString::fromStdString(out));
        std::string task = m.hasMatch() ? m.captured(1).toStdString() : "";
        std::string st = m.hasMatch() ? m.captured(2).toStdString() : "completed";
        if (!task.empty()) onTask(task);
        if (st == "completed" || task.empty()) {
            if (!task.empty()) detached(shrooms, {"a2a", "ack", QString::fromStdString(task)});
            done(out, "");
            return;
        }
        if (st == "failed" || st == "canceled" || st == "rejected" || st == "input-required") { done("", who + "'s task " + st + "."); return; }
        QTimer::singleShot(20000, guarded("ask mesh poll", [this, who, shrooms, task, stillWanted, onTask, done] {
            if (!stillWanted()) return;
            auto* g = new QProcess();
            g->setProcessEnvironment(processEnv());
            QObject::connect(g, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                             guarded("ask mesh get", [this, g, who, shrooms, stillWanted, onTask, done](int code, QProcess::ExitStatus st) {
                g->deleteLater();
                if (!stillWanted()) return;
                if (st != QProcess::NormalExit || code != 0) { done("", trim(g->readAllStandardError().toStdString())); return; }
                meshAsk(who, shrooms, g->readAllStandardOutput().toStdString(), stillWanted, onTask, done);
            }));
            g->start(QString::fromStdString(shrooms), {"a2a", "get", QString::fromStdString(task)});
        }));
    }

    // Texts from an answer (an agent's or a pasted one) into the job. Reels it doesn't cover keep theirs.
    std::string takeYoutube(Job& b, const std::string& reply)
    {
        json got = parseYoutube(reply);
        if (got.empty()) return "No reel blocks in the answer (=== REEL N === with TITLE, DESCRIPTION, TAGS).";
        for (auto& [k, v] : got.items()) b.yt[k] = v;
        return "";
    }
    std::string writeYoutube(Job& b, const std::string& who)
    {
        if (b.reels.empty()) return "Cut the reels first.";
        if (b.ytState == "asking") return "Already asking " + b.ytWho + ".";
        const std::string label = who == "auto" || who.rfind("local:", 0) == 0 ? "the agent on this computer" : who;
        b.ytState = "asking"; b.ytError.clear(); b.ytReply.clear(); b.ytWho = label; b.ytTask.clear();
        const quint64 gen = ++b.ytGen;
        changed();
        const std::string id = b.id;
        std::string ask = "From the Reelcut app on Basecamp (" + QSysInfo::machineHostName().toStdString() + "). " + youtubePrompt(b);
        askAgent(who, "YouTube texts: " + (b.title.empty() ? b.id : b.title), ask,
            [this, id, gen] { Job* j = find(id); return j && j->ytGen == gen && j->ytState == "asking"; },
            [this, id, gen](const std::string& task) { Job* j = find(id); if (j && j->ytGen == gen) j->ytTask = task; },
            [this, id, gen, label](const std::string& reply, const std::string& err) {
                Job* j = find(id);
                if (!j || j->ytGen != gen) return;
                j->ytState.clear(); j->ytTask.clear();
                j->ytReply = reply;
                j->ytError = !err.empty() ? "Asking " + label + " failed: " + err : takeYoutube(*j, reply);
                if (!j->ytError.empty() && err.empty()) j->ytError = label + " answered, but: " + j->ytError;
                save(); changed();
            });
        return "";
    }

    // ── Forums: the reels as replies in a Forumer or Logos Forum thread (forum_post.h), over Logos Delivery
    static bool forumerApp(const json& t) { return jstr(t, "app", "") == "forumer"; }
    // What went to the job's current thread: reel file name -> post id.
    static json postedIn(const Job& b)
    {
        const std::string th = jstr(b.forum, "thread", "");
        return !th.empty() && b.forumPosted.contains(th) && b.forumPosted[th].is_object() ? b.forumPosted[th] : json::object();
    }
    static std::string forumName(const json& t)
    {
        std::string n = jstr(t, "forum", "");
        return !n.empty() ? n : forumerApp(t) ? "public" : "Logos Forum";
    }
    static std::string forumTopic(const json& t)
    {
        return forumerApp(t) ? reelcut::forum::forumerTopic(forumName(t)) : reelcut::forum::logosForumTopic(forumName(t));
    }
    bool forumKey(std::string& err)
    {
        if (fkey.ok()) return true;
        std::error_code ec;
        fs::create_directories(rootDir(), ec);
        fkey = reelcut::forum::loadOrCreateKey(rootDir() + "/.forum-key", &err);
        if (!fkey.ok() && err.empty()) err = "No forum key.";
        return fkey.ok();
    }
    static std::string resultText(const logos::AsyncResult<StdLogosResult>& ar)
    {
        if (!ar.ok()) return "no answer (" + ar.error.code + ")";
        if (!ar.value.success) return ar.value.error.empty() ? "refused" : ar.value.error;
        return ar.value.value.is_string() ? ar.value.value.get<std::string>() : ar.value.value.dump();
    }

    // Logos Delivery: the node is shared by every app in Basecamp. Reelcut creates it (logos.dev, the
    // network both forums use by default) unless one runs already, then waits until it has peers.
    void ensureDelivery(std::function<void(const std::string&)> cb)
    {
        if (dlState == "ready") { cb(""); return; }
        dlWaiters.push_back(std::move(cb));
        if (dlState == "starting") return;
        dlState = "starting"; dlError.clear();
        changed();
        try {
            const std::string cfg = json({{"mode", "Core"}, {"preset", "logos.dev"}}).dump();
            q->modules().delivery_module.createNodeAsyncResult(cfg, [this](logos::AsyncResult<StdLogosResult> ar) {
                const bool created = ar.ok() && ar.value.success;
                onLoop([this, created] {
                    if (!created) { waitConnected(QDateTime::currentMSecsSinceEpoch()); return; }   // already running
                    q->modules().delivery_module.startAsyncResult([this](logos::AsyncResult<StdLogosResult>) {
                        onLoop([this] { waitConnected(QDateTime::currentMSecsSinceEpoch()); });
                    }, 60000);
                });
            }, 60000);
        } catch (const std::exception& e) {
            deliveryDone(std::string("Logos Delivery is not available: ") + e.what());
        }
    }
    void waitConnected(qint64 since)
    {
        q->modules().delivery_module.getConnectionStatusAsyncResult([this, since](logos::AsyncResult<StdLogosResult> ar) {
            const std::string st = resultText(ar);
            onLoop([this, since, st] {
                const QString s = QString::fromStdString(st);
                if (s.contains("connected", Qt::CaseInsensitive) && !s.contains("disconnected", Qt::CaseInsensitive)) { deliveryDone(""); return; }
                if (QDateTime::currentMSecsSinceEpoch() - since > 120000) { deliveryDone("Logos Delivery found no peers within two minutes (" + st + ")."); return; }
                QTimer::singleShot(3000, loop, guarded("delivery wait", [this, since] { waitConnected(since); }));
            });
        }, 15000);
    }
    void deliveryDone(const std::string& err)
    {
        dlState = err.empty() ? "ready" : "failed";
        dlError = err;
        auto waiters = std::move(dlWaiters);
        dlWaiters.clear();
        for (auto& w : waiters) w(err);
        if (dlState == "failed") dlState.clear();   // the next use tries again
        changed();
    }
    void subscribeTopic(const std::string& topic)
    {
        if (topic.empty() || dlTopics.count(topic)) return;
        dlTopics.insert(topic);
        q->modules().delivery_module.subscribeAsyncResult(topic, [](logos::AsyncResult<StdLogosResult>) {}, 30000);
    }

    // The threads of a forum, newest first, from the network's store nodes (as the forums' own
    // history does): for the picker. Only as far back as the store nodes keep messages.
    void loadThreads(const std::string& app, const std::string& name)
    {
        const json t = {{"app", app}, {"forum", name}};
        const std::string key = app + "|" + forumName(t);
        if (jstr(forumThreads.value(key, json::object()), "state", "") == "loading") return;
        forumThreads[key] = {{"state", "loading"}, {"threads", json::array()}, {"error", ""}};
        changed();
        ensureDelivery([this, t, key](const std::string& err) {
            if (!err.empty()) { forumThreads[key] = {{"state", "failed"}, {"threads", json::array()}, {"error", err}}; changed(); return; }
            auto acc = std::make_shared<std::map<std::string, reelcut::forum::Thread>>();
            queryThreads(t, key, acc, 0, "", 0, std::make_shared<std::string>());
        });
    }
    void queryThreads(const json& t, const std::string& key, std::shared_ptr<std::map<std::string, reelcut::forum::Thread>> acc,
                      size_t peer, const std::string& cursor, int page, std::shared_ptr<std::string> lastErr)
    {
        const auto peers = reelcut::forum::storeNodes();
        if (peer >= peers.size()) { threadsDone(key, *acc, *lastErr); return; }
        const std::string ct = forumTopic(t);
        json rq = {{"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()}, {"includeData", true},
                   {"paginationForward", true}, {"paginationLimit", 100},
                   {"pubsubTopic", reelcut::forum::pubsubTopic(ct, 3)}, {"contentTopics", json::array({ct})}};
        if (!cursor.empty()) rq["paginationCursor"] = cursor;
        q->modules().delivery_module.storeQueryAsyncResult(rq.dump(), peers[peer], 8000,
            [this, t, key, acc, peer, page, lastErr](logos::AsyncResult<StdLogosResult> ar) {
            json resp;
            std::string err;
            if (!ar.ok() || !ar.value.success) err = resultText(ar);
            else {
                resp = ar.value.value;
                if (resp.is_string()) resp = json::parse(resp.get<std::string>(), nullptr, false);
            }
            onLoop([this, t, key, acc, peer, page, lastErr, resp, err] {
                std::string next;
                if (!err.empty() || !resp.is_object() || !resp.contains("messages") || !resp["messages"].is_array()) {
                    *lastErr = err.empty() ? "a store node's answer was unreadable" : err;
                } else {
                    const bool app = forumerApp(t);
                    const std::string name = forumName(t);
                    for (auto& m : resp["messages"]) {
                        if (!m.is_object() || !m.contains("message") || !m["message"].is_object()) continue;
                        const std::string p64 = jstr(m["message"], "payload", "");
                        const QByteArray raw = QByteArray::fromBase64(QByteArray::fromStdString(p64));
                        const std::string payload(raw.constData(), size_t(raw.size()));
                        auto th = app ? reelcut::forum::forumerThreadOf(payload) : reelcut::forum::logosForumThreadOf(payload, name);
                        if (th) (*acc)[th->id] = *th;
                    }
                    next = jstr(resp, "paginationCursor", "");
                }
                if (!next.empty() && page < 10) queryThreads(t, key, acc, peer, next, page + 1, lastErr);
                else queryThreads(t, key, acc, peer + 1, "", 0, lastErr);
            });
        }, 20000);
    }
    void threadsDone(const std::string& key, const std::map<std::string, reelcut::forum::Thread>& acc, const std::string& lastErr)
    {
        std::vector<reelcut::forum::Thread> v;
        for (auto& [id, th] : acc) v.push_back(th);
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.ts > b.ts; });
        json arr = json::array();
        for (size_t i = 0; i < v.size() && i < 200; ++i)
            arr.push_back({{"id", v[i].id}, {"title", v[i].title}, {"author", v[i].author}, {"ts", v[i].ts}});
        forumThreads[key] = {{"state", "ready"}, {"threads", arr},
                             {"error", arr.empty() ? (lastErr.empty() ? "No threads found on the store nodes." : lastErr) : ""}};
        changed();
    }

    // Signing a Forumer post mines its proof-of-work (well under a second): off the event loop anyway.
    void signThen(std::function<reelcut::forum::Signed()> make, std::function<void(reelcut::forum::Signed)> done)
    {
        std::thread([this, make = std::move(make), done = std::move(done)]() mutable {
            reelcut::forum::Signed s = make();
            onLoop([s, done = std::move(done)] { done(s); });
        }).detach();
    }
    void sendSigned(const reelcut::forum::Signed& s, std::function<void(const std::string&)> done)
    {
        if (!s.ok()) { done(s.error); return; }
        subscribeTopic(s.topic);
        q->modules().delivery_module.sendAsyncResult(s.topic, std::vector<uint8_t>(s.wire.begin(), s.wire.end()),
            [this, done](logos::AsyncResult<StdLogosResult> ar) {
                const bool ok = ar.ok() && ar.value.success;
                const std::string err = ok ? "" : "Logos Delivery did not send it: " + resultText(ar);
                onLoop([done, err] { done(err); });
            }, 30000);
    }

    // What one reel's reply says: the YouTube texts when written, else its hook and summary; and where
    // to get the video (Logos Storage CID; the set's CID shares them all).
    std::string forumBody(const Job& b, const Reel& r)
    {
        const std::string key = std::to_string(r.n), fname = fs::path(r.file).filename().string();
        const json yt = b.yt.contains(key) ? b.yt[key] : json::object();
        std::string title = trim(jstr(yt, "title", "")), text = trim(jstr(yt, "description", ""));
        if (title.empty()) title = r.name;
        if (text.empty())
            for (auto& c : parseClips(b.clipsText)) if (c.n == r.n) text = trim(c.summary);
        const std::string place = storePlace();
        const json here = place.empty() || !b.stored.contains(place) ? json::object() : b.stored[place];
        const std::string cid = jstr(here, fname.c_str(), ""), set = jstr(here, "reels.json", "");
        std::string body = "**" + title + "**";
        if (!text.empty()) body += "\n\n" + text;
        body += "\n\n";
        if (!cid.empty()) body += "Watch it from Logos Storage: `" + cid + "`";
        else body += "(" + std::to_string((int)std::lround(r.seconds)) + " s reel)";
        if (!set.empty()) body += "\nAll the reels of this video: `" + set + "`";
        if (!b.url.empty()) body += "\nFull video: " + b.url;
        return body.substr(0, 9500);
    }

    std::string postToForum(Job& b)
    {
        const std::string thread = jstr(b.forum, "thread", "");
        if (thread.empty()) return "Choose a thread first, or start one.";
        if (b.reels.empty()) return "Cut the reels first.";
        if (b.forumState == "posting") return "";
        std::string err;
        if (!forumKey(err)) return err;
        b.forumState = "posting"; b.forumError.clear();
        const quint64 gen = ++b.forumGen;
        changed();
        const std::string id = b.id;
        ensureDelivery([this, id, gen](const std::string& e) {
            Job* j = find(id);
            if (!j || j->forumGen != gen) return;
            if (!e.empty()) { forumFailed(*j, e); return; }
            postNext(id, gen);
        });
        return "";
    }
    void forumFailed(Job& j, const std::string& e)
    {
        j.forumState.clear(); j.forumError = e;
        save(); changed();
    }
    // One reel at a time, a few seconds apart (Logos Forum paces a sender at 20 a minute).
    void postNext(const std::string& id, quint64 gen)
    {
        Job* j = find(id);
        if (!j || j->forumGen != gen || j->forumState != "posting") return;
        const Reel* r = nullptr;
        const json done = postedIn(*j);
        for (auto& x : j->reels)
            if (jstr(done, fs::path(x.file).filename().string().c_str(), "").empty()) { r = &x; break; }
        if (!r) { j->forumState.clear(); save(); changed(); return; }
        const json t = j->forum;
        const std::string fname = fs::path(r->file).filename().string(), body = forumBody(*j, *r), alias = forumAlias;
        const std::string thread = jstr(t, "thread", "");
        const int64_t ts = QDateTime::currentMSecsSinceEpoch();
        auto key = fkey;
        signThen([t, key, thread, body, alias, ts] {
            return forumerApp(t) ? reelcut::forum::forumerReply(key, forumName(t), thread, body, alias, ts)
                                 : reelcut::forum::logosForumReply(key, forumName(t), thread, body, alias, ts);
        }, [this, id, gen, fname, thread](reelcut::forum::Signed s) {
            sendSigned(s, [this, id, gen, fname, thread, pid = s.id](const std::string& err) {
                Job* k = find(id);
                if (!k || k->forumGen != gen) return;
                if (!err.empty()) { forumFailed(*k, err); return; }
                if (!k->forumPosted.contains(thread) || !k->forumPosted[thread].is_object()) k->forumPosted[thread] = json::object();
                k->forumPosted[thread][fname] = pid;
                save(); changed();
                QTimer::singleShot(3500, loop, guarded("forum next", [this, id, gen] { postNext(id, gen); }));
            });
        });
    }
    // A new thread (the full video's), then the reels go under it.
    std::string startThread(Job& b, const std::string& title, const std::string& body, const std::string& domains, bool thenPost)
    {
        if (trim(title).empty()) return "A thread needs a title.";
        if (b.forumState == "posting") return "Already posting.";
        std::string err;
        if (!forumKey(err)) return err;
        b.forumState = "posting"; b.forumError.clear();
        const quint64 gen = ++b.forumGen;
        changed();
        const std::string id = b.id, alias = forumAlias;
        std::vector<std::string> doms;
        for (auto& d : QString::fromStdString(domains).split(',', Qt::SkipEmptyParts)) doms.push_back(d.trimmed().toStdString());
        const json t = b.forum;
        ensureDelivery([this, id, gen, t, title, body, doms, alias, thenPost](const std::string& e) {
            Job* j = find(id);
            if (!j || j->forumGen != gen) return;
            if (!e.empty()) { forumFailed(*j, e); return; }
            auto key = fkey;
            const int64_t ts = QDateTime::currentMSecsSinceEpoch();
            signThen([t, key, title, body, doms, alias, ts] {
                return forumerApp(t) ? reelcut::forum::forumerThread(key, forumName(t), trim(title), body, doms, alias, ts)
                                     : reelcut::forum::logosForumThread(key, forumName(t), trim(title), body, alias, ts);
            }, [this, id, gen, title, thenPost](reelcut::forum::Signed s) {
                sendSigned(s, [this, id, gen, title, thenPost, tid = s.id](const std::string& err) {
                    Job* k = find(id);
                    if (!k || k->forumGen != gen) return;
                    if (!err.empty()) { forumFailed(*k, err); return; }
                    k->forum["thread"] = tid;
                    k->forum["threadTitle"] = trim(title);
                    if (!k->forumPosted.contains("started") || !k->forumPosted["started"].is_array()) k->forumPosted["started"] = json::array();
                    k->forumPosted["started"].push_back(tid);
                    k->forumState.clear();
                    save(); changed();
                    if (thenPost) QTimer::singleShot(3500, loop, guarded("forum after thread", [this, id] { if (Job* m = find(id)) postToForum(*m); }));
                });
            });
        });
        return "";
    }

    // ── results
    void readMeta(Job& b)
    {
        std::string meta = readFile(b.dir + "/meta.tsv");
        auto nl = meta.find('\n');
        if (nl != std::string::npos) meta = meta.substr(0, nl);
        std::vector<std::string> f;
        std::stringstream ss(meta);
        std::string x;
        while (std::getline(ss, x, '\t')) f.push_back(x);
        if (f.size() > 0 && !f[0].empty()) b.title = f[0];
        if (f.size() > 1 && f[1] != "NA") b.channel = f[1];
        if (f.size() > 2) b.duration = QString::fromStdString(f[2]).toDouble();
    }
    void countWords(Job& b)
    {
        json w = json::parse(readFile(b.dir + "/words.json"), nullptr, false);
        b.words = w.is_array() ? (int)w.size() : 0;
        if (b.duration <= 0 && w.is_array() && !w.empty() && w.back().is_array() && w.back().size() > 1 && w.back()[1].is_number())
            b.duration = w.back()[1].get<double>();
    }
    void loadReels(Job& b)
    {
        b.reels.clear();
        std::error_code ec;
        std::vector<fs::path> files;
        for (auto& e : fs::directory_iterator(b.dir + "/reels", ec))
            if (e.path().extension() == ".mp4") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        auto clips = parseClips(b.clipsText);
        for (auto& f : files) {
            Reel r;
            r.file = f.string();
            std::string stem = f.stem().string();
            auto dash = stem.find('-');
            int n = dash != std::string::npos ? std::atoi(stem.substr(0, dash).c_str()) : 0;
            r.name = dash != std::string::npos ? stem.substr(dash + 1) : stem;
            std::replace(r.name.begin(), r.name.end(), '-', ' ');
            r.n = n;
            for (auto& c : clips) if (c.n == n) { r.name = c.hook; r.seconds = c.seconds; break; }
            // The real length: cut runs on to the end of the sentence (or LASTWORD).
            double real = QString::fromStdString(trim(readFile((f.parent_path() / (stem + ".dur")).string()))).toDouble();
            if (real > 0) r.seconds = real;
            std::string jpg = (f.parent_path() / (stem + ".jpg")).string();
            if (!viewDir.empty() && fs::exists(jpg, ec)) {
                std::string cache = viewDir + "/cache/" + b.id;
                fs::create_directories(cache, ec);
                std::string dst = cache + "/" + stem + ".jpg";
                fs::copy_file(jpg, dst, fs::copy_options::overwrite_existing, ec);
                if (!ec) r.thumb = dst;
            }
            b.reels.push_back(r);
        }
    }
    static std::vector<std::string> fixList(const std::string& text)
    {
        std::vector<std::string> out;
        for (auto& part : QString::fromStdString(text).split(QRegularExpression("[\\n,;]"))) {
            QString p = part.trimmed();
            int eq = p.indexOf('=');
            if (eq > 0 && eq < p.size() - 1) out.push_back(p.toStdString());
        }
        return out;
    }
    // Why a step failed, for the person: yt-dlp's "ERROR: …" or reelcut's own "reelcut: …" line,
    // not the Python traceback that follows it; else the last line.
    static bool logHas(const Job& b, const char* text)
    {
        for (auto& l : b.log) if (l.find(text) != std::string::npos) return true;
        return false;
    }
    static std::string lastLog(const Job& b)
    {
        for (auto it = b.log.rbegin(); it != b.log.rend(); ++it)
            if (it->rfind("ERROR:", 0) == 0 || (it->rfind("reelcut: ", 0) == 0 && it->find("trying another way") == std::string::npos))
                return *it;
        for (auto it = b.log.rbegin(); it != b.log.rend(); ++it)
            if (!it->empty()) return *it;
        return "";
    }

    Job& newJob()
    {
        Job b;
        b.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toStdString();
        b.created = QDateTime::currentMSecsSinceEpoch();
        b.dir = rootDir() + "/" + QDateTime::currentDateTime().toString("yyyy-MM-dd-HHmm").toStdString() + "-" + b.id;
        b.brief = brief; b.intro = intro; b.outro = outro;
        std::error_code ec;
        fs::create_directories(b.dir, ec);
        jobs.push_back(b);
        return jobs.back();
    }

    // Ends every process of the job; their results no longer count. True when one ran.
    bool stopProcs(const std::string& id, bool)
    {
        bool any = false;
        for (auto& [slot, r] : procs) {
            if (!r.proc || r.jobId != id) continue;
            any = true;
            r.cancelled = true;
            QProcess* p = r.proc;
            pid_t pid = (pid_t)p->processId();
            if (pid > 0) ::kill(-pid, SIGTERM);
            if (!r.container.empty() && containerRunner())
                detached(runnerBin, {"rm", "-f", QString::fromStdString(r.container)});
            QTimer::singleShot(3000, guarded("kill", [p = QPointer<QProcess>(p), pid] { if (p && pid > 0) ::kill(-pid, SIGKILL); }));
        }
        return any;
    }

    void stop(Job& b)
    {
        if (stopProcs(b.id, true)) {
            b.status = "cancelled"; b.error = "Cancelled."; b.activity.clear();
            b.progressI = b.progressN = 0;
            if (b.diar == "running") b.diar = "wanted";
        }
        auto it = picks.find(b.id);
        if (it != picks.end() || (b.step == "pick" && b.status == "running")) {
            if (it != picks.end()) { if (it->second) it->second->kill(); picks.erase(it); }
            // This computer's agent: its task is cancelled too, so it stops spending on it.
            const bool localPick = b.agent == "auto" || b.agent.rfind("local:", 0) == 0;
            if (localPick && !b.pickTask.empty()) {
                agentCall("POST", "/a2a", json({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "CancelTask"}, {"params", {{"id", b.pickTask}}}}).dump(), 5);
                // The task's session is the part of its id before the colon; stop its turn too.
                std::string session = b.pickTask.substr(0, b.pickTask.find(':'));
                agentCall("POST", "/v1/sessions/" + session + "/interrupt", "", 5);
            }
            b.step = "clips"; b.status = "waiting"; b.error = "Cancelled."; b.activity.clear();
            if (!localPick) b.error = "Cancelled; the agent may still answer its task.";
        }
        if (b.status == "queued") { b.status = "cancelled"; b.error = "Cancelled."; }
    }
};

// ── module API ──────────────────────────────────────────────────────────────

ReelcutCoreImpl::ReelcutCoreImpl() : d(new ReelcutCorePrivate(this))
{
    // A damaged jobs.json must never stop the module from loading.
    try { d->load(); }
    catch (const std::exception& e) { std::fprintf(stderr, "[reelcut_core] load: %s\n", e.what()); d->jobs.clear(); }
}

ReelcutCoreImpl::~ReelcutCoreImpl()
{
    // Stop what runs without letting its callbacks reach the state being deleted.
    auto end = [](QProcess* p) {
        if (!p) return;
        p->disconnect();
        pid_t pid = (pid_t)p->processId();
        if (pid > 0) ::kill(-pid, SIGTERM);
        p->kill();
        p->waitForFinished(2000);
        delete p;
    };
    for (auto& [slot, r] : d->procs) {
        if (r.proc && !r.container.empty() && d->containerRunner())
            detached(d->runnerBin, {"rm", "-f", QString::fromStdString(r.container)});
        end(r.proc);
    }
    for (auto& [id, p] : d->picks) end(p);
    delete d;
}

void ReelcutCoreImpl::onContextReady()
{
    // No calls to other modules here (Basecamp 0.3 rejects them).
    QTimer::singleShot(0, guarded("start", [this] { d->detectRunner(); d->detectLocalAgent(); d->changed(); d->schedule(); }));
}

std::string ReelcutCoreImpl::snapshot()
{
    return d->state().dump();
}

std::string ReelcutCoreImpl::setViewDir(const std::string& dir)
{
    std::string p = localPathFrom(dir);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    d->viewDir = p;
    for (auto& b : d->jobs) { d->loadReels(b); d->loadFrame(b); }
    return d->state().dump();
}

std::string ReelcutCoreImpl::setSettings(const std::string& settingsJson)
{
    json s = json::parse(settingsJson, nullptr, false);
    if (!s.is_object()) return json({{"ok", false}, {"error", "Bad settings."}}).dump();
    if (s.contains("agent") && s["agent"].is_string()) d->agent = trim(s["agent"].get<std::string>());
    if (s.contains("candidates")) d->count = (int)std::clamp<int64_t>(jnum(s, "candidates", d->count), 1, 30);
    d->readTuning(s);
    if (s.contains("autoCut")) d->autoCut = jbool(s, "autoCut", d->autoCut);
    if (s.contains("diarize")) d->diarize = jbool(s, "diarize", d->diarize);
    if (s.contains("runner") && s["runner"].is_string()) {
        std::string r = s["runner"].get<std::string>();
        if (r == "auto" || r == "podman" || r == "docker" || r == "local") { d->runnerPref = r; d->detectRunner(); }
    }
    d->save();
    d->schedule();
    return d->state().dump();
}

std::string ReelcutCoreImpl::listAgents()
{
    d->detectLocalAgent();
    std::string shrooms = findExe("shrooms-agent");
    if (shrooms.empty()) return json({{"ok", false}, {"error", "shrooms-agent is not installed here."}}).dump();
    if (d->listingAgents) return d->state().dump();
    d->listingAgents = true;
    auto* p = new QProcess();
    p->setProcessEnvironment(processEnv());
    QObject::connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), guarded("agents", [this, p](int, QProcess::ExitStatus) {
        p->deleteLater();
        d->listingAgents = false;
        json a = json::array();
        // "MACHINE/SESSION<TAB>harness, state, in DIR"
        for (auto& line : QString::fromUtf8(p->readAllStandardOutput()).split('\n')) {
            QStringList f = line.split('\t');
            if (f.size() < 2 || !f[0].contains('/')) continue;
            QStringList m = f[1].split(", ");
            a.push_back({{"name", f[0].trimmed().toStdString()}, {"harness", m.value(0).toStdString()},
                         {"state", m.value(1).toStdString()}});
        }
        d->agents = a;
        d->changed();
    }));
    p->start(QString::fromStdString(shrooms), {"a2a", "list"});
    return d->state().dump();
}

std::string ReelcutCoreImpl::startJob(const std::string& url)
{
    std::string u = trim(url);
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0)
        return json({{"ok", false}, {"error", "Paste a video address that starts with http:// or https://."}}).dump();
    if (u.size() > 4096) return json({{"ok", false}, {"error", "That address is too long."}}).dump();
    Job& b = d->newJob();
    b.url = u;
    b.title = u;
    d->save();
    d->schedule();
    d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::importFile(const std::string& localPath)
{
    std::string p = localPathFrom(localPath);
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) return json({{"ok", false}, {"error", "That is not a file."}}).dump();
    Job& b = d->newJob();
    b.file = p;
    b.title = fs::path(p).stem().string();
    d->save();
    d->schedule();
    d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::pickClips(const std::string& jobId, const std::string& agent)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    std::error_code ec;
    if (!fs::exists(b->dir + "/transcript-ts.txt", ec)) return json({{"ok", false}, {"error", "Transcribe it first."}}).dump();
    if (b->status == "running") return json({{"ok", false}, {"error", "Wait for the current step to finish."}}).dump();
    std::string who = trim(agent).empty() ? d->agent : trim(agent);
    if (who.empty()) return json({{"ok", false}, {"error", "Choose an agent in the settings first."}}).dump();
    d->startPick(*b, who);
    return d->state().dump();
}

std::string ReelcutCoreImpl::getPrompt(const std::string& jobId)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    std::error_code ec;
    if (!fs::exists(b->dir + "/transcript-ts.txt", ec)) return json({{"ok", false}, {"error", "Transcribe it first."}}).dump();
    // reelcut prompt prints the instructions (the brief, the lengths) and the transcript.
    std::string text;
    {
        if (d->runner.empty()) return json({{"ok", false}, {"error", d->runnerError}}).dump();
        QStringList pa = d->promptArgs(*b);
        QStringList args = d->containerRunner() ? d->stepArgs(*b, pa, "reelcut-prompt-" + b->id, nullptr) : pa;
        QProcess p;
        p.setProcessEnvironment(processEnv());
        p.start(QString::fromStdString(d->runnerBin), args);
        if (!p.waitForFinished(30000) || p.exitCode() != 0)
            return json({{"ok", false}, {"error", "Could not build the prompt: " + trim(p.readAllStandardError().toStdString())}}).dump();
        text = p.readAllStandardOutput().toStdString();
    }
    return json({{"ok", true}, {"text", ReelcutCorePrivate::speakerNote(*b) + text}}).dump();
}

std::string ReelcutCoreImpl::setClips(const std::string& jobId, const std::string& clipsText)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (b->status == "running") return json({{"ok", false}, {"error", "Wait for the current step to finish."}}).dump();
    // A pasted chat answer may wrap the clip lines in prose or a table: keep just the clip lines.
    auto clips = parseClips(clipsText);
    if (clips.empty() && !trim(clipsText).empty())
        return json({{"ok", false}, {"error", "No clip lines in that. Each clip is one line: N | START H:MM:SS | END H:MM:SS | speaker | hook | until LASTWORD"}}).dump();
    b->clipsText = ::clipsText(clips);
    // Ticks for clips that are gone go too.
    std::vector<int> keep;
    for (int n : b->chosen) for (auto& c : clips) if (c.n == n) { keep.push_back(n); break; }
    b->chosen = keep;
    if (b->step == "pick" || b->step == "clips") b->error.clear();
    d->save();
    d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::cutReels(const std::string& jobId, const std::string& fixesText)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (b->status == "running" || b->status == "queued") return json({{"ok", false}, {"error", "Wait for the current step to finish."}}).dump();
    std::error_code ec;
    if (!fs::exists(b->dir + "/words.json", ec)) return json({{"ok", false}, {"error", "Transcribe it first."}}).dump();
    if (parseClips(b->clipsText).empty())
        return json({{"ok", false}, {"error", "There are no clips to cut. One line per clip: N | START | END | speaker | hook | until LASTWORD"}}).dump();
    b->fixes = fixesText;
    b->step = "cut"; b->status = "queued"; b->error.clear();
    d->save();
    d->schedule();
    d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::cancelJob(const std::string& jobId)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    d->stop(*b);
    d->save();
    d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::retryJob(const std::string& jobId)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (b->status == "running" || b->status == "queued") return d->state().dump();
    if (d->runner.empty() || !d->runnerError.empty()) d->detectRunner();
    b->error.clear();
    if (b->step == "pick" || b->step == "clips") { d->startPick(*b, b->agent.empty() ? d->agent : b->agent); return d->state().dump(); }
    if (b->step == "done" || b->step == "speakers") return d->state().dump();
    b->status = "queued";
    d->save();
    d->schedule();
    d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::removeJob(const std::string& jobId, bool deleteFiles)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    d->stop(*b);
    std::string dir = b->dir, id = b->id;
    d->jobs.erase(std::remove_if(d->jobs.begin(), d->jobs.end(), [&](const Job& x) { return x.id == id; }), d->jobs.end());
    std::error_code ec;
    // Only ever a job folder inside the Reelcut folder.
    if (deleteFiles && dir.rfind(rootDir() + "/", 0) == 0 && dir.find("/../") == std::string::npos) fs::remove_all(dir, ec);
    if (!d->viewDir.empty()) fs::remove_all(d->viewDir + "/cache/" + id, ec);
    d->save();
    d->changed();
    return d->state().dump();
}

// Spawns the desktop's opener (xdg-open, open on macOS) for a file or an http(s) link.
static std::string spawnOpen(const std::string& target)
{
#ifdef __APPLE__
    std::string bin = "open";
#else
    std::string bin = "xdg-open";
#endif
    auto env = cleanEnv();
    std::vector<char*> envp;
    for (auto& v : env) envp.push_back(const_cast<char*>(v.c_str()));
    envp.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char* argv[] = {const_cast<char*>(bin.c_str()), const_cast<char*>(target.c_str()), nullptr};
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return json({{"ok", false}, {"error", bin + ": " + std::strerror(rc)}}).dump();
    std::thread([pid]() { ::waitpid(pid, nullptr, 0); }).detach();
    return json({{"ok", true}}).dump();
}

std::string ReelcutCoreImpl::openUrl(const std::string& url)
{
    if ((url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) || url.size() > 4096)
        return json({{"ok", false}, {"error", "Only http and https links open."}}).dump();
    for (unsigned char c : url)
        if (c <= 0x20 || c == 0x7f) return json({{"ok", false}, {"error", "That is not a link."}}).dump();
    return spawnOpen(url);
}

std::string ReelcutCoreImpl::openPath(const std::string& path)
{
    std::error_code ec;
    fs::path p = fs::weakly_canonical(localPathFrom(path), ec);
    fs::path root = fs::weakly_canonical(rootDir(), ec);
    std::string ps = p.string(), rs = root.string();
    if (ps.rfind(rs + "/", 0) != 0 && ps != rs) return json({{"ok", false}, {"error", "Only files in the Reelcut folder open."}}).dump();
    if (!fs::exists(p, ec)) return json({{"ok", false}, {"error", "It is not there any more."}}).dump();
    return spawnOpen(ps);
}

std::string ReelcutCoreImpl::diarizeJob(const std::string& jobId, int64_t speakers)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (b->status == "running" || b->status == "queued") return json({{"ok", false}, {"error", "Wait for the current step to finish."}}).dump();
    std::error_code ec;
    if (!fs::exists(b->dir + "/words.json", ec)) return json({{"ok", false}, {"error", "Transcribe it first."}}).dump();
    if (d->runner.empty()) return json({{"ok", false}, {"error", d->runnerError}}).dump();
    b->speakersWanted = (int)std::clamp<int64_t>(speakers, 0, 12);
    b->spk.erase("error");
    b->diar = "wanted";
    b->diarRetry = false;
    b->step = "diarize"; b->status = "queued"; b->error.clear();
    d->save();
    d->schedule();
    d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::setSpeakers(const std::string& jobId, const std::string& speakersJson, bool proceed)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    json s = json::parse(speakersJson, nullptr, false);
    if (!s.is_object()) return json({{"ok", false}, {"error", "Bad speakers."}}).dump();
    if (s.contains("names") && s["names"].is_object()) {
        json names = json::object();
        for (auto& [k, v] : s["names"].items())
            if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z' && v.is_string()) names[k] = cleanName(v.get<std::string>());
        b->spk["names"] = names;
    }
    if (s.contains("sides") && s["sides"].is_object()) {
        json sides = json::object();
        for (auto& [k, v] : s["sides"].items())
            if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z' && v.is_string() && (v == "left" || v == "middle" || v == "right")) sides[k] = v;
        b->spk["sides"] = sides;
    }
    if (s.contains("layout") && s["layout"].is_string()) {
        std::string l = s["layout"].get<std::string>();
        if (l == "" || l == "fit" || l == "stack" || l == "speaker" || l == "crop") b->layout = l;
    }
    if (proceed && b->step == "speakers" && b->status == "waiting") d->toPicking(*b);
    d->save();
    d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::storeReels(const std::string& jobId)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (d->storePlace().empty()) return json({{"ok", false}, {"error", "Choose a Logos Storage node in Settings first."}}).dump();
    if (b->reels.empty()) return json({{"ok", false}, {"error", "No reels to put there yet."}}).dump();
    // Failed ones go again.
    for (auto it = b->storing.begin(); it != b->storing.end();)
        it = it->second.rfind("failed", 0) == 0 ? b->storing.erase(it) : std::next(it);
    d->storeReels(*b);
    return d->state().dump();
}

std::string ReelcutCoreImpl::setJob(const std::string& jobId, const std::string& optionsJson)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    json o = json::parse(optionsJson, nullptr, false);
    if (!o.is_object()) return json({{"ok", false}, {"error", "Bad options."}}).dump();
    // The brief and the cards are also the defaults for the next job.
    if (o.contains("brief") && o["brief"].is_string()) d->brief = b->brief = o["brief"].get<std::string>().substr(0, 4000);
    for (const char* k : {"intro", "outro"}) {
        if (!o.contains(k) || !o[k].is_string()) continue;
        std::string v = o[k].get<std::string>();
        if (!v.empty() && v.rfind(ReelcutCorePrivate::cardsDir() + "/", 0) != 0)
            return json({{"ok", false}, {"error", "Add the image to the cards first."}}).dump();
        ReelcutCorePrivate::touchCard(v);
        (std::string(k) == "intro" ? b->intro : b->outro) = v;
        (std::string(k) == "intro" ? d->intro : d->outro) = v;
    }
    if (o.contains("chosen") && o["chosen"].is_array()) {
        auto clips = parseClips(b->clipsText);
        b->chosen.clear();
        for (auto& n : o["chosen"])
            if (n.is_number_integer())
                for (auto& c : clips) if (c.n == n.get<int>()) { b->chosen.push_back(c.n); break; }
    }
    d->save();
    d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::writeYoutube(const std::string& jobId, const std::string& agent)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    const std::string who = agent.empty() ? d->agent : agent;
    if (who.empty()) return json({{"ok", false}, {"error", "No agent chosen: copy the prompt into an AI chat instead."}}).dump();
    std::string err = d->writeYoutube(*b, who);
    if (!err.empty()) return json({{"ok", false}, {"error", err}}).dump();
    return d->state().dump();
}

std::string ReelcutCoreImpl::youtubePrompt(const std::string& jobId)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (b->reels.empty()) return json({{"ok", false}, {"error", "Cut the reels first."}}).dump();
    return json({{"ok", true}, {"text", d->youtubePrompt(*b)}}).dump();
}

std::string ReelcutCoreImpl::setYoutube(const std::string& jobId, const std::string& text)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    std::string err = d->takeYoutube(*b, text);
    if (!err.empty()) return json({{"ok", false}, {"error", err}}).dump();
    b->ytError.clear(); b->ytReply.clear();
    if (b->ytState == "asking") { b->ytState.clear(); ++b->ytGen; }   // a pasted answer wins over a pending one
    d->save(); d->changed();
    return d->state().dump();
}

std::string ReelcutCoreImpl::stopYoutube(const std::string& jobId)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (b->ytState == "asking") { b->ytState.clear(); ++b->ytGen; d->changed(); }
    return d->state().dump();
}

std::string ReelcutCoreImpl::setForum(const std::string& jobId, const std::string& targetJson)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    json in = json::parse(targetJson, nullptr, false);
    if (!in.is_object()) return json({{"ok", false}, {"error", "Not a forum target."}}).dump();
    json t = b->forum.is_object() ? b->forum : json::object();
    if (in.contains("app") && in["app"].is_string()) {
        const std::string app = in["app"].get<std::string>();
        if (app != "forumer" && app != "logos_forum" && !app.empty()) return json({{"ok", false}, {"error", "Forumer or Logos Forum."}}).dump();
        if (app != jstr(t, "app", "")) { t.erase("thread"); t.erase("threadTitle"); t.erase("forum"); }
        t["app"] = app;
    }
    if (in.contains("forum") && in["forum"].is_string()) {
        const std::string f = trim(in["forum"].get<std::string>());
        if (f != jstr(t, "forum", "")) { t.erase("thread"); t.erase("threadTitle"); }
        t["forum"] = f;
    }
    if (in.contains("thread") && in["thread"].is_string()) {
        const std::string raw = trim(in["thread"].get<std::string>());
        std::string id;
        if (raw.empty()) id = "";
        else if (ReelcutCorePrivate::forumerApp(t)) {
            auto x = reelcut::forum::forumerThreadFromLink(raw);
            if (!x) return json({{"ok", false}, {"error", "Paste a Forumer thread link (Share on the thread), or its id."}}).dump();
            id = *x;
        } else {
            QString h = QString::fromStdString(raw).toLower();
            if (!QRegularExpression("^[0-9a-f]{64}$").match(h).hasMatch())
                return json({{"ok", false}, {"error", "A Logos Forum thread id is 64 hex characters: pick the thread from the list."}}).dump();
            id = h.toStdString();
        }
        t["thread"] = id;
        t["threadTitle"] = in.contains("threadTitle") && in["threadTitle"].is_string() ? in["threadTitle"].get<std::string>() : std::string();
    }
    if (in.contains("auto") && in["auto"].is_boolean()) t["auto"] = in["auto"].get<bool>();
    if (!ReelcutCorePrivate::forumTopic(t).empty() || jstr(t, "app", "").empty()) {
        b->forum = t;
        b->forumError.clear();
        d->save(); d->changed();
        return d->state().dump();
    }
    return json({{"ok", false}, {"error", "Not a forum name this app can use (Forumer: lowercase letters, digits and -)."}}).dump();
}

std::string ReelcutCoreImpl::loadForumThreads(const std::string& app, const std::string& forumName)
{
    if (app != "forumer" && app != "logos_forum") return json({{"ok", false}, {"error", "Forumer or Logos Forum."}}).dump();
    d->loadThreads(app, forumName);
    return d->state().dump();
}

std::string ReelcutCoreImpl::startForumThread(const std::string& jobId, const std::string& title, const std::string& body,
                                              const std::string& domains, bool thenPost)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (jstr(b->forum, "app", "").empty()) return json({{"ok", false}, {"error", "Choose the forum first."}}).dump();
    std::string err = d->startThread(*b, title, body, domains, thenPost);
    if (!err.empty()) return json({{"ok", false}, {"error", err}}).dump();
    return d->state().dump();
}

std::string ReelcutCoreImpl::postToForum(const std::string& jobId)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    std::string err = d->postToForum(*b);
    if (!err.empty()) return json({{"ok", false}, {"error", err}}).dump();
    return d->state().dump();
}

std::string ReelcutCoreImpl::stopForum(const std::string& jobId)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (b->forumState == "posting") { b->forumState.clear(); ++b->forumGen; d->save(); d->changed(); }
    return d->state().dump();
}

std::string ReelcutCoreImpl::clipText(const std::string& jobId, int64_t n)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    const Clip* c = nullptr;
    auto clips = parseClips(b->clipsText);
    for (auto& x : clips) if (x.n == n) c = &x;
    if (!c) return json({{"ok", false}, {"error", "No such clip."}}).dump();
    // The sentences from START to END, as the transcript stamps them.
    return json({{"ok", true}, {"n", n}, {"text", trim(clipLines(readFile(b->dir + "/transcript-ts.txt"), *c))}}).dump();
}

std::string ReelcutCoreImpl::addCard(const std::string& imagePath)
{
    std::string p = localPathFrom(imagePath);
    std::error_code ec;
    std::string ext = QString::fromStdString(fs::path(p).extension().string()).toLower().toStdString();
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".webp")
        return json({{"ok", false}, {"error", "A card is an image: PNG, JPEG or WebP."}}).dump();
    if (p.empty() || !fs::is_regular_file(p, ec) || fs::file_size(p, ec) > 20u * 1024 * 1024)
        return json({{"ok", false}, {"error", "That is not an image file (or it is over 20 MB)."}}).dump();
    const std::string dir = ReelcutCorePrivate::cardsDir();
    fs::create_directories(dir, ec);
    // The same image again: the one already there.
    for (auto& f : d->cardFiles())
        if (f.filename().string().substr(f.filename().string().find('-') + 1) == fs::path(p).filename().string() &&
            fs::file_size(f, ec) == fs::file_size(p, ec)) {
            ReelcutCorePrivate::touchCard(f.string());
            json st = d->state(); st["added"] = f.string();
            return st.dump();
        }
    std::string name = QString::fromStdString(fs::path(p).filename().string()).replace(QRegularExpression("[^\\w.-]"), "_").toStdString();
    std::string dst = dir + "/" + std::to_string(QDateTime::currentMSecsSinceEpoch()) + "-" + name;
    fs::copy_file(p, dst, ec);
    if (ec) return json({{"ok", false}, {"error", "Could not copy the image: " + ec.message()}}).dump();
    // The library keeps the last few; cards a job still uses stay.
    auto files = d->cardFiles();
    for (size_t k = ReelcutCorePrivate::kCards; k < files.size(); k++) {
        bool used = false;
        for (auto& j : d->jobs) used = used || j.intro == files[k].string() || j.outro == files[k].string();
        if (!used) fs::remove(files[k], ec);
    }
    d->changed();
    json st = d->state(); st["added"] = dst;
    return st.dump();
}
