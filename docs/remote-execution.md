# Remote execution for Reelcut

*2026-11-14. Step 1 (your own machines) is built and tested on branch `feat/remote-workers`; step 2 (a marketplace) is a
design for review. Status and the plan: the last section.*

Today every heavy step runs on the computer Basecamp runs on, in the reelcut container. This proposal moves them to
another machine, first one you own (step 1), then one rented from a stranger (step 2), while Basecamp stays where
you work.

## What a job is made of

| Step | Command | Load | Inputs | Outputs | Remote? |
|---|---|---|---|---|---|
| download | `reelcut fetch URL DIR` | network, minutes | the URL | `audio.*`, `video.*` (100 MB – 2 GB) | yes: the video never has to travel |
| transcribe | `reelcut transcribe DIR` | CPU, minutes | audio | `words.json`, transcripts (≈ 0.1–1 MB) | yes |
| speakers | `reelcut diarize DIR` | CPU | audio, words | `words.json` with speakers | yes |
| size + frame | `reelcut probe DIR` | small | video | `.size`, `frame.jpg` | yes (needs the video) |
| pick clips | an agent or the person | — | transcript | `clips.txt` | **local**: it's interactive (or a mesh agent) |
| scenes | `reelcut scenes DIR CLIPS OUT` | CPU | video, clips | a frame per scene + faces (≈ 50 KB each) | yes |
| frame by hand | the Framing screen | — | the frames | `framing.json` | **local** |
| cut | `reelcut cut DIR CLIPS OUT …` | CPU, minutes | video, words, clips, framing, cards | `reels/*.mp4` (≈ 5–30 MB each) | yes |
| thumbnails | `reelcut thumbs OUT` | small | reels | `*.jpg`, `*.dur` | yes |
| page | `reelcut page OUT` | none | reels | `index.html` | either |
| YouTube texts, posting | agents, Logos Forum | — | — | — | local (already over the network) |

So the work splits cleanly: the **heavy steps are each one `reelcut` command on a job folder**, and the interactive
steps (picking, framing, naming speakers) stay in Basecamp. The video lives where it was downloaded; only small
files (transcript, frames, clip lists, framing) go back and forth, plus the finished reels.

Done today as groundwork: `reelcut probe` and `reelcut thumbs` (vpavlin/reelcut bfd0f32) replace the two inline shell
scripts Reelcut Basecamp ran, so **every remote step is a reelcut command from a fixed list, never a shell script**.

## Step 1 — your own worker

### Shape

- **One module, two roles.** `reelcut_core` gets a *worker* role. On the strong machine it runs headless under
  `logosctl` (a systemd service, like the other hubs) or in a second Basecamp. Nothing new to install.
- **Basecamp stays the brain.** The local `reelcut_core` keeps the jobs, the screens, the agents and the decisions. The
  worker only runs reelcut commands in a folder per job and sends back what they wrote. A setting picks where the
  heavy steps run: *this computer* or a paired worker.
- **Pairing.** The worker shows a pairing code (a random 32-byte key, its name and its addresses). You paste it
  into Reelcut's settings. The worker serves only paired clients; a pairing can be removed on either side.
- **The link, step 1: HTTP over the LAN or the mesh.** Your own machines can reach each other directly, so the
  worker listens on a port and Basecamp calls it: submit a task, a long-poll stream of what it prints, cancel, list
  and fetch the job's files, put a file. Every request is signed with the pairing key (HMAC-SHA256 over the method,
  the path and a timestamp, plus the body's hash), replays are refused. Over the shrooms mesh the traffic is also
  inside WireGuard; on a plain LAN only the files travel in the clear. It's fast (a 1 GB import copies at LAN speed)
  and has none of Delivery's lost messages or Storage's NAT problems.
- **The link, step 1b / step 2: Logos Delivery + Logos Storage.** For a worker you can't reach directly (another
  network, a stranger), the same messages go over Delivery (sealed with the pairing key, or with X25519 boxes between
  strangers' keys; acked and resent) and the files through Storage CIDs. Step 2 needs this anyway; it is built
  after the HTTP link works.
- **Files.** Every file the worker writes comes back into the job's local folder, except the media (`audio.*`,
  `video.*`, half-written downloads): the transcript, the frames, the scenes, the reels. So the screens, the agents,
  forum posting and "open the reel" work exactly as for a local job. The files Basecamp writes (the clip list, the
  framing, the cards) go to the worker before each step.
- **What runs.** A task is `{job, step, argv}`: argv[0] from the list (`fetch transcribe diarize probe scenes cut
  thumbs page`), every path inside `/work`, flags from that command's own list. The worker runs it in the reelcut
  container with only the job folder mounted, at its own cores / low-priority settings. Progress (the `i/N` counter
  and the log) streams back about every 10 s, so the screens look as they do for a local job.

### In Basecamp

- Settings → **Where the work runs**: *This computer* / a paired worker (name, online, cores, what it is doing),
  *Pair a worker…*.
- The pipeline view shows where each step runs ("transcribing on studio-box, 14/52").
- Cancel works remotely; a worker that goes away mid-step leaves the step *waiting for studio-box*, and it continues
  or is retried locally (one click) when it doesn't come back.

### What step 1 does not do

- No payment, no strangers: the worker trusts its paired clients and the client trusts the worker's results.
- A local video file is sent to the worker once, over the same link (LAN speed); it stays there.
- The worker must be reachable directly (same network, or the mesh). Otherwise: step 1b below.

## Step 2 — a marketplace

The same task, offered to people you don't know. What changes is **finding a worker, paying it, and trusting it**.

### Two ways to buy

1. **A job** (stateless): "run *this* step on *these* inputs, give me the outputs". Inputs travel by Storage CID or URL,
   outputs come back as CIDs. Any step that is one reelcut command fits; a whole download → transcribe → speakers
   chain is one job too (it shares the download). Cut can be split across several workers by clip.
2. **A lease** (rented compute): "this machine for me for an hour". It's step 1 with a stranger: the folder stays on
   the worker between steps, so the video is downloaded once and every later step is fast. Paid per block of time.

### The market

- **Offers and asks are public, signed records on Logos Delivery** (one content topic, Store keeps them; the Rumor
  record format fits: a signed record with an expiry). An offer: worker key, what it runs (reelcut image version,
  cores, RAM, GPU), price per job kind and per lease-minute, where (region, rough bandwidth), its LEZ address. An ask:
  the step, input size/duration, the most it pays, a deadline.
- **Matching:** the client picks an offer (or a worker answers an ask), then they talk privately: sealed boxes
  (X25519) between their keys instead of step 1's shared pairing key. Same task and file messages as step 1.

### Paying with LEZ

What exists today (checked 2026-11-13, read-only):

- **A public testnet** (`testnet.lez.logos.co`). It is slow and irregular right now (blocks minutes apart, one
  ~17-minute gap), and it has been reset with every LEZ version (v0.2 → v0.3 → v0.4-rc in three months).
- **No faucet for the native token.** Funds come from a Logos dev, the bridge from the L1 testnet (about an hour),
  or a transfer from someone who has some.
- **Transfers from a Basecamp core module work:** `lez_core` (official catalog) has `transfer_public`,
  `get_balance`, `send_generic_public_transaction` (call any program) and program deployment. The module holds the
  wallet and signs. One wallet per Basecamp, no hardware-wallet signing, desktop only.
- **Each transaction reserves about 16 M units of fee** (at today's base fee) and takes minutes to confirm. So
  per-minute payments as separate transfers are out.
- **No escrow is deployed.** The pattern exists: the atomic-swaps proof of concept has an HTLC (lock / claim / refund
  after a timelock, `logos-co/atomic-swaps-poc`, LEZ v0.2.2), and current LEZ has what it needs (clock accounts,
  validity windows, `custody_transfer` out of a program-held account).

Proposal:

1. **An escrow program** (ours, deployed with `lez_core`): the client locks the price under a job id; the worker
   is paid when the client releases it, or automatically when the client says nothing for N hours after the
   result was delivered; the client gets it back if no result came by the deadline. Roughly 3–5 days for the
   program and its tests, 1–2 weeks with wiring into Basecamp and surviving a testnet reset.
2. **Jobs: one escrow per job** (or per batch of jobs with one worker), released on accepting the results.
3. **Leases: prepaid blocks**, e.g. 30 minutes locked at a time, released as each block is used. Finer billing
   later with off-chain signed vouchers (the client signs "I owe you up to X", the worker claims the last one once:
   a one-way payment channel), a custom program in the same 1–2 weeks.
4. **Until the escrow exists**, the market can run on plain `transfer_public`: prepay small jobs and accept the
   risk, and build reputation. That's enough for a demo.

**Two existing LEZ programs to build on instead of starting from zero** (from Backpack and from the anoncomms
team, 2026-11-13):

- **`backpack_access`** (vpavlin/backpack-access, on the LEZ v0.3 testnet; Backpack 0.5 uses it): a paid
  membership registry. Per *config* (here: one per worker) a price per period and a treasury; a member's
  `register(pubkey, periods)` pays `price × periods` to the treasury **in the same transaction** and sets
  `paid_through`; expiry is a check against the chain's clock, no transaction. **That is a lease as it stands:** the
  worker serves a client key while `paid_through > now`. Backpack's Basecamp glue (`bp_lez.hpp`, the program's FFI
  library loaded at run time with its own wallet) can be copied. Leases need no new program.
- **Payment streams** (LIP-155, repo `lez-payment-streams`, guest deployed on the LEZ testnet 2026-08-20; Store
  eligibility is its reference use): a stream from payer to provider that accrues over time and is claimed by the
  provider. The natural fit for per-minute leases once it's usable from Basecamp; to evaluate.
- Jobs still want an escrow (lock / release / refund after a deadline); the HTLC pattern above.

**Finding workers and talking to strangers** (anoncomms):

- Offers can later use Logos **service discovery** (DISC-NG over the Kad DHT: a service id with signed record data:
  price, capacity, key). It's in nim-libp2p and wired into Delivery, but liblogosdelivery has no app-level
  advertise/lookup yet, so start with a public offers topic on Delivery and move over when the API exists.
- First contact: the buyer's ephemeral X25519 key in the request; the reply topic derived from the shared secret
  (as Loam's secret-derived topics). Optionally over **Mix** (Delivery's lightpush has an anonymity level) so the
  worker never learns who the buyer is; Mix packets are ~4.9 KB, so only control messages go that way, files by
  Storage CID.
- Paying anonymously isn't possible yet (LEZ private execution for payments is planned, not done).
- Reputation: nothing exists for markets; ours (receipts signed by both sides, small first jobs, maybe a stake).

Risks: testnet resets (program ids change; keep them in config, not code), no faucet (onboarding needs a person),
minutes-long confirmations (a worker starts on "locked, pending" for small amounts, waits for confirmation for big
ones).

### Trust

- The worker sees the inputs. A URL job reveals only the URL; an imported private video is visible to whoever runs it.
  Asks can say "public videos only".
- Results can't be proven correct, so: cheap checks on arrival (the reel's length matches the clip, the transcript's
  words cover the audio's length), a reputation per worker key built from accepted jobs, and small first jobs.
- The worker is protected by the container: only the job folder, a fixed command list, no shell, CPU and time limits.

### What to build as separate pieces

- **reelcut_core**: client and worker roles, the task protocol (shared by both steps).
- **A market module** (generic, not Reelcut-specific): post and read offers/asks, match, reputation. Other apps
  with heavy steps (transcription in general, rendering) could use the same market.
- **An escrow program on LEZ** (and a small Basecamp module to call it), unless one exists.

## Status and plan

### Done (step 1, branch `feat/remote-workers`, Reelcut 0.6.0, not released yet)

- **reelcut** CLI: `probe` and `thumbs` commands (bfd0f32, image built).
- **reelcut_core**: worker role and client role, the signed HTTP link, file mirroring, cancel, re-attach after a
  restart (and reuse of a step that finished meanwhile), noticing a restarted worker. Unit test: 30 checks.
- **View**: Settings → *Where the work runs* (pick a worker, add one by its code, see what it runs), *Let my other
  computers use this one* (name, make a pairing code, unpair); running jobs say "· on crib-worker".
- **Tested live** with two logosctl sessions on the crib (worker + Basecamp): a 6-minute YouTube video through
  download, transcription (1095 words), speakers, scenes, cut, thumbnails and page, all on the worker; reels
  byte-identical on the Basecamp side, no video copied; a local file import; cancel; Basecamp restart; worker restart.
- **Not yet:** a test in the Basecamp GUI itself, and two different machines (the test was two sessions on one).

### Plan

| # | What | Size | Needs from you |
|---|---|---|---|
| 1a | **Release 0.6.0 to lan03** (Linux x86 + ARM + Mac, merged), then you try it: Basecamp on your laptop, a worker on a stronger machine | building now | which machine is the worker |
| 1b | **The Logos link for your own workers**: same steps over Logos Messaging (sealed with the pairing key, acked and resent), files through Logos Storage; for a worker you can't reach directly | 1–2 days | — |
| 2a | **Market without money**: workers post signed offers on a public Delivery topic; first contact with X25519 keys; jobs carry their inputs by URL or Storage CID | ~2 days | OK to start before payments |
| 2b | **Leases paid on LEZ** with Backpack's `backpack_access` program as it is (one config per worker: price per hour; the worker serves a client while it's paid) | 1–2 days | LEZ testnet funds for two test wallets |
| 2c | **Jobs paid on LEZ**: an escrow program (lock, release on accept, refund after a deadline), or payment streams (LIP-155) if they fit | 1–2 weeks | — |

Open questions:

1. Should the market be a **generic module** (any app with heavy steps could post jobs), or **Reelcut-only** first and
   extracted later? I'd start Reelcut-only, with the messages kept app-neutral.
2. Pricing: per job kind (transcribe a minute of audio, cut a reel) or per minute of machine time? Leases price by
   time anyway; for jobs I'd start per job kind.
