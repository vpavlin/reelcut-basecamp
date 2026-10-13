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

static const char* kVersion = "0.1.0";
static const char* kImage = "ghcr.io/vpavlin/reelcut:latest";
static const size_t kLogLines = 40;
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
            if constexpr (std::is_invocable_v<F&, decltype(args)...>) fn(std::forward<decltype(args)>(args)...);
            else fn();
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

// ── state ───────────────────────────────────────────────────────────────────

struct Reel {
    std::string file, name, thumb;
    double seconds = 0;
};

// A job's step is where it is in the pipeline; its status says whether that step runs, waits or stopped.
//   steps:  fetch -> transcribe -> pick -> clips -> cut -> done
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
};

struct Running {
    QPointer<QProcess> proc;
    std::string jobId, container;
    bool cancelled = false;
};

struct ReelcutCorePrivate {
    ReelcutCoreImpl* q;
    std::vector<Job> jobs;
    std::string agent = "proteus/proteus";
    int count = 6;
    std::string runnerPref = "auto";
    bool autoCut = true;

    // detected
    std::string runner, runnerBin, runnerError;
    bool imageChecked = false, imagePresent = false;

    json agents = json::array();
    bool listingAgents = false;
    std::string viewDir;

    Running heavy;              // fetch / transcribe / cut / thumbs: one at a time
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
            if (b.id.empty() || b.dir.empty()) continue;
            // Whatever ran when Basecamp closed did not finish.
            if (b.status == "running") { b.status = "cancelled"; b.error = "Stopped when Basecamp closed. Retry to run it again."; }
            jobs.push_back(b);
            loadReels(jobs.back());
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
                           {"words", b.words}});
        std::error_code ec;
        fs::create_directories(rootDir(), ec);
        writeFile(jobsFile(), json({{"settings", settingsJson()}, {"jobs", arr}}).dump(2));
    }
    json settingsJson() const
    {
        return {{"agent", agent}, {"count", count}, {"runner", runnerPref}, {"autoCut", autoCut}};
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
          << "-v" << QString::fromStdString(b.dir) + (runner == "podman" ? ":/work:Z" : ":/work");
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
                          {"page", fs::exists(page, ec) ? page : ""}});
        }
        return {
            {"ok", true},
            {"version", kVersion},
            {"root", rootDir()},
            {"runner", {{"kind", runner}, {"bin", runnerBin}, {"error", runnerError}, {"image", kImage},
                        {"imagePresent", imagePresent}}},
            {"settings", settingsJson()},
            {"agents", agents},
            {"thumbsOk", !viewDir.empty()},
            {"jobs", js},
        };
    }

    // ── the heavy queue: one step of one job at a time
    void schedule()
    {
        if (heavy.proc) return;
        for (auto& b : jobs)
            if (b.status == "queued") { runStep(b); return; }
    }

    void finishStep(Job& b, const std::string& error)
    {
        b.progressI = b.progressN = 0;
        b.activity.clear();
        if (!error.empty()) { b.status = "failed"; b.error = error; }
        save();
        changed();
        QTimer::singleShot(0, guarded("schedule", [this] { schedule(); }));
    }

    // Starts a process for the job; done(exitOk, stdout) runs when it ends (not when cancelled).
    void launch(Job& b, const std::string& bin, const QStringList& args, const std::string& container,
                std::function<void(bool, const std::string&)> done)
    {
        auto* p = new QProcess();
        p->setProcessEnvironment(processEnv());
        p->setWorkingDirectory(QString::fromStdString(b.dir));
        // Its own process group, so cancel stops reelcut's helpers (parakeet, ffmpeg) too.
        p->setChildProcessModifier([] { ::setsid(); });
        auto out = std::make_shared<std::string>();
        auto errBuf = std::make_shared<std::string>();
        const std::string id = b.id;
        heavy = Running{p, id, container, false};
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
        QObject::connect(p, &QProcess::readyReadStandardError, guarded("stderr", [this, p, errBuf, id] {
            QString chunk = QString::fromUtf8(p->readAllStandardError());
            Job* j = find(id);
            if (!j) return;
            // transcribe counts its pieces as "\ri/N".
            static const QRegularExpression prog("(\\d+)/(\\d+)\\s*$");
            for (auto& piece : chunk.split(QRegularExpression("[\r\n]"))) {
                auto m = prog.match(piece.trimmed());
                if (m.hasMatch() && piece.trimmed() == m.captured(0)) {
                    j->progressI = m.captured(1).toInt(); j->progressN = m.captured(2).toInt();
                } else {
                    logLine(*j, piece.toStdString());
                }
            }
            changed();
        }));
        QObject::connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         guarded("finished", [this, p, out, id, done](int code, QProcess::ExitStatus st) {
            p->deleteLater();
            bool cancelled = heavy.jobId == id && heavy.cancelled;
            if (heavy.proc == p || !heavy.proc) heavy = Running{};
            Job* j = find(id);
            if (!j) { QTimer::singleShot(0, guarded("schedule", [this] { schedule(); })); return; }
            if (cancelled) {
                j->status = "cancelled"; j->error = "Cancelled.";
                finishStep(*j, "");
                return;
            }
            done(st == QProcess::NormalExit && code == 0, *out);
        }));
        QObject::connect(p, &QProcess::errorOccurred, guarded("start error", [this, p, id, bin](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            p->deleteLater();
            if (heavy.proc == p) heavy = Running{};
            if (Job* j = find(id)) finishStep(*j, "Could not start " + bin + ".");
        }));
        p->start(QString::fromStdString(bin), args);
        changed();
    }

    void runStep(Job& b)
    {
        if (b.step == "fetch" && !b.file.empty()) { copyFile(b); return; }
        if (runner.empty()) { finishStep(b, runnerError.empty() ? "No way to run reelcut here." : runnerError); return; }
        if (containerRunner() && !imagePresent) { ensureImage(b); return; }
        const std::string name = "reelcut-" + b.id;
        if (b.step == "fetch") {
            b.activity = "Downloading the video";
            launch(b, runnerBin, stepArgs(b, {"fetch", QString::fromStdString(b.url), work(b, "")}, name, nullptr), name,
                   [this, id = b.id](bool ok, const std::string&) {
                Job* j = find(id); if (!j) return;
                if (!ok) { finishStep(*j, "Downloading failed. " + lastLog(*j)); return; }
                readMeta(*j);
                j->step = "transcribe"; j->status = "queued";
                finishStep(*j, "");
            });
        } else if (b.step == "transcribe") {
            b.activity = "Transcribing";
            launch(b, runnerBin, stepArgs(b, {"transcribe", work(b, "")}, name, nullptr), name,
                   [this, id = b.id](bool ok, const std::string&) {
                Job* j = find(id); if (!j) return;
                if (!ok) { finishStep(*j, "Transcribing failed. " + lastLog(*j)); return; }
                countWords(*j);
                // Picking runs beside the queue, so the next job can start transcribing.
                j->step = agent.empty() ? "clips" : "pick";
                j->status = "waiting";
                finishStep(*j, "");
                if (!agent.empty()) startPick(*j, agent);
            });
        } else if (b.step == "cut") {
            b.activity = "Cutting the reels";
            std::error_code ec;
            fs::create_directories(b.dir + "/reels", ec);
            for (auto& e : fs::directory_iterator(b.dir + "/reels", ec))
                if (e.path().extension() == ".mp4" || e.path().extension() == ".ass" || e.path().extension() == ".jpg" || e.path().extension() == ".dur")
                    fs::remove(e.path(), ec);
            writeFile(b.dir + "/clips.txt", b.clipsText + "\n");
            QStringList args{"cut", work(b, ""), work(b, "clips.txt"), work(b, "reels")};
            for (auto& f : fixList(b.fixes)) args << "--fix" << QString::fromStdString(f);
            b.progressN = (int)parseClips(b.clipsText).size();
            launch(b, runnerBin, stepArgs(b, args, name, nullptr), name, [this, id = b.id](bool ok, const std::string& out) {
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
            launch(b, bin, args, name, [this, id = b.id, title](bool ok, const std::string&) {
                Job* j = find(id); if (!j) return;
                if (!ok) logLine(*j, "Some previews could not be made.");
                loadReels(*j);
                pageThenDone(*j, title);
            });
        } else {
            finishStep(b, "Nothing to run at step " + b.step + ".");
        }
    }

    void pageThenDone(Job& b, const QString& title)
    {
        const std::string name = "reelcut-" + b.id;
        b.activity = "Writing the page";
        launch(b, runnerBin, stepArgs(b, {"page", work(b, "reels"), "--title", title}, name, nullptr), name,
               [this, id = b.id](bool, const std::string&) {
            Job* j = find(id); if (!j) return;
            j->step = "done"; j->status = "done";
            finishStep(*j, "");
        });
    }

    void ensureImage(Job& b)
    {
        if (!imageChecked) {
            imageChecked = true;
            QProcess p; p.setProcessEnvironment(processEnv());
            QStringList args = runner == "podman" ? QStringList{"image", "exists", kImage}
                                                  : QStringList{"image", "inspect", kImage};
            p.start(QString::fromStdString(runnerBin), args);
            imagePresent = p.waitForFinished(15000) && p.exitCode() == 0;
            if (imagePresent) { runStep(b); return; }
        }
        b.activity = "Downloading the reelcut image (about 1 GB, once)";
        launch(b, runnerBin, {"pull", kImage}, "", [this, id = b.id](bool ok, const std::string&) {
            Job* j = find(id); if (!j) return;
            if (!ok) { finishStep(*j, "Could not download the reelcut image. " + lastLog(*j)); return; }
            imagePresent = true;
            j->status = "queued";
            finishStep(*j, "");
        });
    }

    void copyFile(Job& b)
    {
        // A local video: copied into the job's folder, so the steps (and the container) see it.
        std::string src = b.file;
        std::string ext = fs::path(src).extension().string();
        if (ext.empty()) ext = ".mp4";
        std::string dst = b.dir + "/video" + ext;
        b.activity = "Copying the video";
        launch(b, "cp", {"--", QString::fromStdString(src), QString::fromStdString(dst)}, "",
               [this, id = b.id, src](bool ok, const std::string&) {
            Job* j = find(id); if (!j) return;
            if (!ok) { finishStep(*j, "Could not copy " + src + ". " + lastLog(*j)); return; }
            if (j->title.empty()) j->title = fs::path(src).stem().string();
            j->step = "transcribe"; j->status = "queued";
            finishStep(*j, "");
        });
    }

    // ── picking: a mesh agent reads the transcript and answers with clip lines
    void startPick(Job& b, const std::string& who)
    {
        if (who.empty()) { b.step = "clips"; b.status = "waiting"; save(); changed(); return; }
        if (runner.empty()) { b.status = "failed"; b.error = runnerError; save(); changed(); return; }
        std::string shrooms = findExe("shrooms-agent");
        if (shrooms.empty()) {
            b.step = "clips"; b.status = "waiting";
            b.error = "shrooms-agent is not installed here, so no agent can pick. Write the clips yourself.";
            save(); changed(); return;
        }
        b.step = "pick"; b.status = "running"; b.agent = who; b.error.clear(); b.pickTask.clear(); b.pickReply.clear();
        b.activity = "Asking " + who + " to pick " + std::to_string(count) + " clips";
        save(); changed();
        // First the prompt (reelcut prompt prints the instructions and the transcript)...
        QStringList pa = {"prompt", work(b, ""), "-n", QString::number(count)};
        QStringList args = containerRunner() ? stepArgs(b, pa, "reelcut-prompt-" + b.id, nullptr) : pa;
        auto* p = new QProcess();
        p->setProcessEnvironment(processEnv());
        picks[b.id] = p;
        QObject::connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         guarded("prompt", [this, p, id = b.id, who, shrooms](int code, QProcess::ExitStatus st) {
            p->deleteLater();
            std::string prompt = p->readAllStandardOutput().toStdString();
            Job* j = find(id);
            if (!j || j->status != "running" || j->step != "pick") return;
            if (st != QProcess::NormalExit || code != 0 || prompt.empty()) {
                pickFailed(*j, "Could not build the prompt: " + trim(p->readAllStandardError().toStdString()));
                return;
            }
            std::string ask = "From the Reelcut app on Basecamp (" + QSysInfo::machineHostName().toStdString() +
                "), for vpavlin. Pick the clips below and reply with ONLY the clip lines in the format given, "
                "one per line, nothing else. Your reply is parsed by a program.\n\n" + prompt;
            if (ask.size() > kMaxPrompt) {
                pickFailed(*j, "The transcript is too long to send to an agent in one message (" +
                           std::to_string(ask.size() / 1024) + " KiB). Write the clips yourself, or pick from a shorter part.");
                return;
            }
            // ...then the agent, as a task on the mesh. --wait prints its reply.
            auto* a = new QProcess();
            a->setProcessEnvironment(processEnv());
            picks[id] = a;
            QObject::connect(a, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                             guarded("pick", [this, a, id, who, shrooms](int code2, QProcess::ExitStatus st2) {
                a->deleteLater();
                std::string reply = a->readAllStandardOutput().toStdString();
                std::string err = trim(a->readAllStandardError().toStdString());
                Job* j2 = find(id);
                if (!j2 || j2->step != "pick") return;
                picks.erase(id);
                // The task id ("MACHINE/SESSION:ID"), to close the task once the reply is in.
                static const QRegularExpression task("\\b([\\w.-]+/[\\w.-]+:[\\w-]{8,})\\b");
                auto m = task.match(QString::fromStdString(reply + "\n" + err));
                if (m.hasMatch()) {
                    j2->pickTask = m.captured(1).toStdString();
                    detached(shrooms, {"a2a", "ack", m.captured(1)});
                }
                j2->pickReply = reply;
                auto clips = parseClips(reply);
                if (st2 != QProcess::NormalExit || code2 != 0 || clips.empty()) {
                    pickFailed(*j2, clips.empty() && code2 == 0
                        ? who + " answered, but with no clip lines. Its reply is below; write the clips yourself or ask again."
                        : "Asking " + who + " failed: " + (err.empty() ? trim(reply) : err));
                    return;
                }
                // Only the clip lines go into the list the person sees and edits.
                std::string text;
                for (auto& c : clips) {
                    text += std::to_string(c.n) + " | " + c.start + " | " + c.end + " | " + c.speaker + " | " + c.hook;
                    if (!c.until.empty()) text += " | until " + c.until;
                    text += "\n";
                }
                j2->clipsText = text;
                j2->activity.clear();
                if (autoCut) { j2->step = "cut"; j2->status = "queued"; }
                else { j2->step = "clips"; j2->status = "waiting"; }
                save(); changed();
                schedule();
            }));
            a->start(QString::fromStdString(shrooms),
                     {"a2a", "send", "--wait", "--title", "Pick reel clips", QString::fromStdString(who), QString::fromStdString(ask)});
        }));
        p->start(QString::fromStdString(runnerBin), args);
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
    static std::string lastLog(const Job& b)
    {
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

    void stop(Job& b)
    {
        if (heavy.proc && heavy.jobId == b.id) {
            heavy.cancelled = true;
            QProcess* p = heavy.proc;
            pid_t pid = (pid_t)p->processId();
            if (pid > 0) ::kill(-pid, SIGTERM);
            if (!heavy.container.empty() && containerRunner())
                detached(runnerBin, {"rm", "-f", QString::fromStdString(heavy.container)});
            QTimer::singleShot(3000, guarded("kill", [p = QPointer<QProcess>(p), pid] { if (p && pid > 0) ::kill(-pid, SIGKILL); }));
        }
        auto it = picks.find(b.id);
        if (it != picks.end()) {
            if (it->second) it->second->kill();
            picks.erase(it);
            b.step = "clips"; b.status = "waiting"; b.error = "Cancelled; the agent may still answer its task."; b.activity.clear();
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
    if (d->heavy.proc && !d->heavy.container.empty() && d->containerRunner())
        detached(d->runnerBin, {"rm", "-f", QString::fromStdString(d->heavy.container)});
    end(d->heavy.proc);
    for (auto& [id, p] : d->picks) end(p);
    delete d;
}

void ReelcutCoreImpl::onContextReady()
{
    // No calls to other modules here (Basecamp 0.3 rejects them).
    QTimer::singleShot(0, guarded("start", [this] { d->detectRunner(); d->changed(); d->schedule(); }));
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
    for (auto& b : d->jobs) d->loadReels(b);
    return d->state().dump();
}

std::string ReelcutCoreImpl::setSettings(const std::string& settingsJson)
{
    json s = json::parse(settingsJson, nullptr, false);
    if (!s.is_object()) return json({{"ok", false}, {"error", "Bad settings."}}).dump();
    if (s.contains("agent") && s["agent"].is_string()) d->agent = trim(s["agent"].get<std::string>());
    if (s.contains("count")) d->count = (int)std::clamp<int64_t>(jnum(s, "count", d->count), 1, 20);
    if (s.contains("autoCut")) d->autoCut = jbool(s, "autoCut", d->autoCut);
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

std::string ReelcutCoreImpl::setClips(const std::string& jobId, const std::string& clipsText)
{
    Job* b = d->find(jobId);
    if (!b) return json({{"ok", false}, {"error", "No such job."}}).dump();
    if (b->status == "running") return json({{"ok", false}, {"error", "Wait for the current step to finish."}}).dump();
    b->clipsText = clipsText;
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
    if (b->step == "done") return d->state().dump();
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

std::string ReelcutCoreImpl::openPath(const std::string& path)
{
    std::error_code ec;
    fs::path p = fs::weakly_canonical(localPathFrom(path), ec);
    fs::path root = fs::weakly_canonical(rootDir(), ec);
    std::string ps = p.string(), rs = root.string();
    if (ps.rfind(rs + "/", 0) != 0 && ps != rs) return json({{"ok", false}, {"error", "Only files in the Reelcut folder open."}}).dump();
    if (!fs::exists(p, ec)) return json({{"ok", false}, {"error", "It is not there any more."}}).dump();
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
    char* argv[] = {const_cast<char*>(bin.c_str()), const_cast<char*>(ps.c_str()), nullptr};
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return json({{"ok", false}, {"error", bin + ": " + std::strerror(rc)}}).dump();
    std::thread([pid]() { ::waitpid(pid, nullptr, 0); }).detach();
    return json({{"ok", true}}).dump();
}
