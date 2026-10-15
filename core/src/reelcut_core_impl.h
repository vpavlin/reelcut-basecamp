#pragma once

#include <string>
#include "logos_module_context.h"

struct ReelcutCorePrivate;

/**
 * ReelcutCoreImpl - the engine behind the Reelcut Basecamp view.
 *
 * Drives reelcut (https://github.com/vpavlin/reelcut) to turn a long video into short vertical
 * reels with word-by-word captions: fetch (yt-dlp) -> transcribe (Parakeet, locally, on the CPU)
 * -> pick the clips (a mesh agent, or the person) -> cut (ffmpeg) -> page.
 *
 * reelcut runs as the container image ghcr.io/vpavlin/reelcut under podman (or docker), or as a
 * `reelcut` on PATH when there is no container runtime. One job at a time runs; the others wait.
 * Each job lives in its own folder under ~/Videos/Reelcut (REELCUT_HOME overrides it).
 *
 * Rules the module glue imposes (see the logos-basecamp-module skill):
 *  - every public method returns a JSON string; mutations return the fresh snapshot
 *  - no default arguments, at most 4 arguments, no trailing comments on declarations
 *  - the steps run in the background; progress arrives through snapshot() and stateChanged
 */
class ReelcutCoreImpl : public LogosModuleContext
{
public:
    ReelcutCoreImpl();
    ~ReelcutCoreImpl() override;

    // Full state: the runner, the settings and every job with its step, progress, clips and reels.
    std::string snapshot();

    // The view's own directory. Reel thumbnails are copied to <dir>/cache, the only place the
    // sandboxed view may load a local image from.
    std::string setViewDir(const std::string& dir);

    // settingsJson: {"agent": "MACHINE/SESSION" or "" (pick through an AI chat), "candidates": 12, "runner": "auto|podman|docker|local",
    // "diarize": true, "autoCut": false, "autoCount": 5, "min": 20, "max": 55, "lead": 0.25, "tail": 0.4, "fade": 0.3, "card": 2}.
    std::string setSettings(const std::string& settingsJson);

    // The agents on the shrooms mesh, for the "who picks the clips" choice. Fills snapshot().agents.
    std::string listAgents();

    // A new job from a video address (anything yt-dlp takes). It fetches and transcribes, then
    // asks the agent from the settings for clips, or waits for them when there is none.
    std::string startJob(const std::string& url);

    // A new job from a video file on this computer (path or file:// URL).
    std::string importFile(const std::string& localPath);

    // Ask a mesh agent to pick the clips from the transcript ("" = the agent from the settings).
    std::string pickClips(const std::string& jobId, const std::string& agent);

    // The prompt for picking the clips (instructions + the transcript), to paste into any AI chat:
    // {"ok": true, "text": "…"}.
    std::string getPrompt(const std::string& jobId);

    // Tell the speakers apart (again): speakers = how many people talk, 0 = counted from the voices.
    std::string diarizeJob(const std::string& jobId, int64_t speakers);

    // speakersJson: {"names": {"A": "Corey Petty"}, "sides": {"A": "left"}, "layout": "stack|fit|speaker|crop|"}; proceed: go on to picking.
    std::string setSpeakers(const std::string& jobId, const std::string& speakersJson, bool proceed);

    // The clip list, one line per clip (a pasted chat answer is fine: only the clip lines are kept): N | START H:MM:SS | END H:MM:SS | speaker | hook | until LASTWORD
    std::string setClips(const std::string& jobId, const std::string& clipsText);

    // Cut the reels (the chosen ones, all when none is chosen). fixesText: one "Heard=Meant" per line or comma, for misheard names.
    std::string cutReels(const std::string& jobId, const std::string& fixesText);

    // optionsJson: {"brief": "what I'm looking for", "chosen": [2, 5, 7] (the clips to cut), "intro": card, "outro": card}.
    // The brief and the cards become the defaults for the next job.
    // The job's reels into the Logos Storage node chosen in Settings (again for those that failed).
    std::string storeReels(const std::string& jobId);
    std::string setJob(const std::string& jobId, const std::string& optionsJson);

    // The transcript lines of one clip, to see what it holds: {"ok": true, "n": 3, "text": "[0:03:42] B: …"}.
    std::string clipText(const std::string& jobId, int64_t n);

    // An image for the opening or closing cards: copied into the card library (the last few used).
    // The state comes back with "added": its path in the library.
    std::string addCard(const std::string& imagePath);

    // Stop what the job is running now.
    std::string cancelJob(const std::string& jobId);

    // Run the job again from the step that failed or was cancelled.
    std::string retryJob(const std::string& jobId);

    // Forget the job; deleteFiles also removes its folder (the video, the transcript, the reels).
    std::string removeJob(const std::string& jobId, bool deleteFiles);

    // Open a job's folder, a reel or its page with the system's default app. Only paths inside
    // the Reelcut folder open.
    std::string openPath(const std::string& path);

    // Opens an http(s) link in the browser (the sandboxed view's Qt.openUrlExternally can't).
    std::string openUrl(const std::string& url);

    void onContextReady() override;

logos_events:
    // The same JSON snapshot() returns, pushed after every change.
    void stateChanged(const std::string& json);

private:
    ReelcutCorePrivate* d = nullptr;
    friend struct ReelcutCorePrivate;
};
