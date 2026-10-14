# Reelcut for Logos Basecamp

Paste a long video (a livestream, a Space, a talk, a podcast) and get short vertical reels with
word-by-word captions. A Basecamp app around [reelcut](https://github.com/vpavlin/reelcut), which does
the work in the public container image `ghcr.io/vpavlin/reelcut` (amd64 and arm64).

## Install

In Basecamp, add the package repository `https://apps.vpavlin.xyz/logos-repo.json` (or find
Reelcut on [apps.vpavlin.xyz](https://apps.vpavlin.xyz)) and install **Reelcut**; it brings
`reelcut_core` with it. Linux x86_64, Linux arm64 and macOS (Apple Silicon); Basecamp 0.3.

## What it does

- **Paste and go:** the video is downloaded and transcribed **on your computer** (Parakeet, on the
  CPU, nothing leaves it). Then the best moments are picked from the transcript and the reels are
  cut, 1080×1920 with captions and a hook title.
- **Who speaks:** for an interview, a podcast or a call, the voices are told apart on your
  computer too (about 7 minutes per hour of video; beside the transcription on a machine with 8 or
  more cores, after it on a smaller one). Transcribing starts as soon as the audio is down, while
  the video still downloads. Reelcut shows how long
  each one talks, a few of their lines and a frame of the video; you name them once and say who
  sits left and who right. The reels then show each name as that person starts to speak, captions
  come in each speaker's colour, and the agent is told who is who. Wrong count? Count again with
  the right number. Off in ⚙ for a single speaker.
- **Layout:** two people in a landscape recording are cut one above the other, each cropped to
  their own picture (the listener slightly dimmed). Per job, choose the whole picture, follow
  whoever speaks, or a crop of the middle instead.
- **Picking the moments:**
  - **An agent on this computer, automatic:** with [shrooms-agent](https://github.com/vpavlin/shrooms)
    installed (no mesh needed) and podman, Reelcut starts a session of its own in a container (a
    cage) and every new video goes to it; its answer is cut straight away. Choose in ⚙ which agent:
    **Claude Code** with your existing Claude login, or **pi** with the model and key in pi's own
    settings. "Automatic" takes Claude Code when it is there, else pi.
  - **An agent on your shrooms mesh:** any session on your other machines, chosen in ⚙.
  - **Any AI chat, no setup:** without an agent, "Copy the prompt" puts the instructions and the
    transcript on the clipboard; paste them into Claude, ChatGPT or any other chat, then paste its
    answer back.
- **Steer it:** "What I'm looking for" (topics, people, tone, keywords) goes with every prompt and
  counts more than the general advice; the last one is kept for the next video.
- **Choose:** the agent proposes more clips than you will cut (12 by default, best first, 20–55 s
  each), each with a sentence on what is said and why it works. Tick as many as you like ("Show
  the words" shows the transcript of one), then "Cut these".
- **Edges and cards:** a little air before the first word and after the last, and a fade in and
  out ("Breathing room" in ⚙: 1.5 s each side, 0.8 s fades). An opening and a closing card per
  job (a λ, a QR code, a URL), from a small library of the last images used.
- **Or a file:** "use a video file…" takes a video from disk instead of an address.
- **Edit the clips:** the clip list (`N | START H:MM:SS | END H:MM:SS | speaker | hook | until LASTWORD`)
  can be edited by hand, with the transcript a click away.
- **Names:** transcription mishears names. "Misheard names" takes `Heard=Meant` pairs and fixes the
  captions when you cut again.
- **Watch:** the reels show as a grid of previews. A click opens a page in your browser that plays
  them all; "Open in the video player" opens one reel in your desktop player, "Open folder" shows the files.
- **Jobs:** each video is a job with its own folder in `~/Videos/Reelcut`. One heavy step runs at a
  time; jobs can be stopped, retried from the step that failed, or deleted.

## Packages

| Package | Type | What it does |
|---|---|---|
| `reelcut_core` | core (universal) | Runs reelcut, keeps the jobs, asks the agent, makes the previews |
| `reelcut` | ui_qml | The view. Pure QML; renders `reelcut_core.snapshot()` |

## What it needs on the computer

One of, in this order:

1. **podman** (or docker the user may run): reelcut runs from the image `ghcr.io/vpavlin/reelcut`
   (amd64 and arm64, about 1 GB, downloaded on the first job and checked for a newer version once a
   day). Nothing else to install.
2. **reelcut on PATH** with its tools (yt-dlp, ffmpeg, parakeet-cli and the model): see the
   reelcut README.

Picking needs nothing more: copy the prompt into any AI chat. For automatic picking, install
`shrooms-agent` from [shrooms](https://github.com/vpavlin/shrooms): on its own it serves this
computer only, on a socket only you can open, and Reelcut makes its sessions there (`reelcut-claude`,
`reelcut-pi`, caged, in `~/Videos/Reelcut/.picker`). The agent needs a key: your Claude login for
Claude Code, or a provider in pi's settings. A shrooms mesh adds your other machines' sessions.

Basecamp 0.3 sandboxes views (no network, no files outside the view's own folder), so everything
runs in `reelcut_core`; the reel previews are copied to `<view folder>/cache/`.

## Build

```sh
cd core && nix build .#lgx-portable   # reelcut_core
cd ui   && nix build .#lgx-portable   # reelcut
```

Other platforms: `.github/workflows/platform-modules.yml` (`-f runner=ubuntu-24.04-arm` or `macos-14`),
then `lgx merge`.

## Test

- **Headless core:** load `reelcut_core` under `logosctl` and call it (`startJob`, `pickClips`,
  `cutReels`, `snapshot`, …).
- **View render:** `ui/harness/render.sh "<qml steps>"` renders `Main.qml` offscreen with a `logos`
  bridge that forwards every call to that live core (session `reelcut`), and writes screenshots to
  `ui/harness/shots/`.

## License

MIT OR Apache-2.0, at your option: [LICENSE-MIT](LICENSE-MIT), [LICENSE-APACHE-v2](LICENSE-APACHE-v2).
