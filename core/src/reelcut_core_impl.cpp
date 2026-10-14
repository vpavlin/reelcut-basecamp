#include "reelcut_core_impl.h"

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

static const char* kVersion = "0.1.6";
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
    if (p.isEmpty()) p = QStandardPaths::findExecutable(name, {QDir::homePath() + "/.local/bin", "/usr/local/bin"});
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

struct Clip {
    int n = 0;
    std::string start, end, speaker, hook, until;
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
        double a = secs(c.start), b = secs(c.end);
        if (a < 0 || b < 0) continue;
        c.seconds = b - a;
        out.push_back(c);
    }
    return out;
}

static json clipsJson(const std::vector<Clip>& v)
{
    json a = json::array();
    for (auto& c : v)
        a.push_back({{"n", c.n}, {"start", c.start}, {"end", c.end}, {"speaker", c.speaker}, {"hook", c.hook},
                     {"until", c.until}, {"seconds", c.seconds}});
    return a;
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

// ── state ───────────────────────────────────────────────────────────────────

struct Reel {
    std::string file, name, thumb;
    double seconds = 0;
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
    // Who speaks (reelcut diarize): {found, talk:{A:s}, samples:{A:[lines]}, names:{A:…}, sides:{A:left|right}, error}
    json spk = json::object();
    int speakersWanted = 0;     // 0: counted from the voices
    std::string layout;         // cut --frame: "" (reelcut's own choice), fit, stack, speaker, crop
    int vw = 0, vh = 0;         // the video's size, for the layout
    // Before picking, the steps overlap: what is done so far.
    bool fetched = false, transcribed = false, probed = false;
    std::string diar;           // who speaks: "" (not decided) | wanted | running | after (transcribe) | done | failed | off
    bool diarRetry = false;
    std::string frame;          // one frame of the video, copied where the view may load it
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
    int count = 6;
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

    explicit ReelcutCorePrivate(ReelcutCoreImpl* qq) : q(qq) {}

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
        count = (int)std::clamp<int64_t>(jnum(s, "count", count), 1, 20);
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
                           {"transcribed", b.transcribed}, {"probed", b.probed}, {"diar", b.diar}});
        std::error_code ec;
        fs::create_directories(rootDir(), ec);
        json st = settingsJson();
        st["lastPull"] = lastPull;
        writeFile(jobsFile(), json({{"settings", st}, {"jobs", arr}}).dump(2));
    }
    json settingsJson() const
    {
        return {{"agent", agent}, {"count", count}, {"runner", runnerPref}, {"autoCut", autoCut}, {"diarize", diarize}};
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
            for (auto& r : b.reels)
                reels.push_back({{"file", r.file}, {"name", r.name}, {"thumb", r.thumb}, {"seconds", r.seconds}});
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
                          {"width", b.vw}, {"height", b.vh}, {"frame", b.frame}});
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
            {"jobs", js},
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
        if (containerRunner() && !imagePresent) { if (!busy("pull")) pullImage(b); return; }
        const std::string name = "reelcut-" + b.id;
        if (b.step == "cut") {
            b.activity = "Cutting the reels";
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
            b.progressN = (int)parseClips(b.clipsText).size();
            launch(b, "heavy", runnerBin, stepArgs(b, args, name, nullptr), name, [this, id = b.id](bool ok, const std::string& out) {
                Job* j = find(id); if (!j) return;
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

    // cut --speakers: "A=Corey Petty@left,B=Václav@right", the letters with a name or a side.
    static std::string speakersArg(const Job& j)
    {
        if (jnum(j.spk, "found", 0) < 2) return "";
        json names = j.spk.contains("names") ? j.spk["names"] : json::object();
        json sides = j.spk.contains("sides") ? j.spk["sides"] : json::object();
        std::string out;
        for (char c = 'A'; c <= 'Z'; c++) {
            const char k[2] = {c, 0};
            std::string n = cleanName(jstr(names, k, "")), s = jstr(sides, k, "");
            if (s != "left" && s != "right") s.clear();
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
        });
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
        b.activity = "Asking " + label + " to pick " + std::to_string(count) + " clips";
        save(); changed();
        // First the prompt (reelcut prompt prints the instructions and the transcript)...
        QStringList pa = {"prompt", work(b, ""), "-n", QString::number(count)};
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
            writeFile(j->dir + "/prompt-" + std::to_string(count) + ".txt", prompt);
            if (local) askLocal(*j, harness, ask);
            else askMesh(*j, who, ask, shrooms);
        }));
        p->start(QString::fromStdString(runnerBin), args);
        (void)label;
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
        std::string text;
        for (auto& c : clips) {
            text += std::to_string(c.n) + " | " + c.start + " | " + c.end + " | " + c.speaker + " | " + c.hook;
            if (!c.until.empty()) text += " | until " + c.until;
            text += "\n";
        }
        j.clipsText = text;
        j.activity.clear();
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
            if (n >= 1 && n <= (int)clips.size()) { r.name = clips[n - 1].hook; r.seconds = clips[n - 1].seconds; }
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
    if (s.contains("count")) d->count = (int)std::clamp<int64_t>(jnum(s, "count", d->count), 1, 20);
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
    // One file per reel count, made once: reelcut prompt prints the instructions and the transcript.
    const std::string file = b->dir + "/prompt-" + std::to_string(d->count) + ".txt";
    std::string text = readFile(file);
    if (text.empty()) {
        if (d->runner.empty()) return json({{"ok", false}, {"error", d->runnerError}}).dump();
        QStringList pa = {"prompt", d->work(*b, ""), "-n", QString::number(d->count)};
        QStringList args = d->containerRunner() ? d->stepArgs(*b, pa, "reelcut-prompt-" + b->id, nullptr) : pa;
        QProcess p;
        p.setProcessEnvironment(processEnv());
        p.start(QString::fromStdString(d->runnerBin), args);
        if (!p.waitForFinished(30000) || p.exitCode() != 0)
            return json({{"ok", false}, {"error", "Could not build the prompt: " + trim(p.readAllStandardError().toStdString())}}).dump();
        text = p.readAllStandardOutput().toStdString();
        writeFile(file, text);
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
    std::string text;
    for (auto& c : clips) {
        text += std::to_string(c.n) + " | " + c.start + " | " + c.end + " | " + c.speaker + " | " + c.hook;
        if (!c.until.empty()) text += " | until " + c.until;
        text += "\n";
    }
    b->clipsText = text;
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
            if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z' && v.is_string() && (v == "left" || v == "right")) sides[k] = v;
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
