# Reelcut for Logos Basecamp

Paste a long video (a livestream, a Space, a talk, a podcast) and get short vertical reels with
word-by-word captions. A Basecamp app around [reelcut](https://github.com/vpavlin/reelcut).

- **Paste and go:** the video is downloaded, transcribed **on your computer** (Parakeet, on the
  CPU, nothing leaves it), an agent on your shrooms mesh picks the best moments from the
  transcript, and the reels are cut, 1080×1920 with captions and a hook title.
- **Or a file:** "use a video file…" takes a video from disk instead of an address.
- **Pick yourself:** with no agent set, or when you want other moments, write or edit the clip list
  (`N | START H:MM:SS | END H:MM:SS | speaker | hook | until LASTWORD`), with the transcript a click away.
- **Names:** transcription mishears names. "Misheard names" takes `Heard=Meant` pairs and fixes the
  captions when you cut again.
- **Watch:** the reels show as a grid of previews; a click plays one in your video player. "Open the
  page" opens an HTML page that plays them all; "Open folder" shows the files.
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
   (amd64 and arm64, about 1 GB, downloaded on the first job). Nothing else to install.
2. **reelcut on PATH** with its tools (yt-dlp, ffmpeg, parakeet-cli and the model): see the
   reelcut README.

Picking by an agent needs `shrooms-agent` and a session on the mesh to ask (default
`proteus/proteus`; change it in ⚙). The transcript goes to that agent as a task.

Basecamp 0.3 sandboxes views (no network, no files outside the view's own folder), so everything
runs in `reelcut_core`; the reel previews are copied to `<view folder>/cache/`.

## Build

```sh
cd core && nix build .#lgx-portable   # reelcut_core
cd ui   && nix build .#lgx-portable   # reelcut (the repo is private: add --option access-tokens github.com=$(gh auth token))
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

MIT or Apache-2.0, at your option.
