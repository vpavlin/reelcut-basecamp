# reelcut_lease: rent a Reelcut worker on LEZ (v0.4)

A LEZ v0.4 program (raw `lee_core` actors, no SPEL): a worker publishes an offer (treasury, price per
period, period length, longest rental); a client rents N periods, which in ONE transaction pays
`price × N` from the payer's account to the treasury (native token transfer) and extends the lease of
the client's key at that worker (`paid_through_ms`, from `max(now, paid_through)`). The worker serves a
client key while its lease is paid. Rent carries the terms the payer read (`max_amount`, `treasury`):
if the worker changed its offer in between, the rental is refused instead of charging more or paying
elsewhere. Who paid isn't recorded: anyone may pay for any client key's lease. The offer keeps a record only the program writes (rentals, periods
sold). Reviews / reputation: an open design, see ../docs/remote-execution.md.

- `core/`: messages and states (borsh), shared by the program and clients; the lease account is
  `for_public_pda(program, sha256("reelcut-lease/v1" ‖ worker ‖ client_key))`.
- `methods/guest/src/bin/reelcut_lease.rs`: the program.
- `src/main.rs`: `reelcut_lease deploy|offer|rent|show`, a wallet-based tool for tests and ops
  (`rent --max N --treasury ID` overrides the terms it sends, to test the refusals).
- `cargo test -p reelcut_lease_core` pins a lease account; core/test/remote_test.cpp checks Reelcut's own
  computation against the same value.

## Build and run (no Docker)

It builds inside the LEZ workspace at tag `v0.4.0-rc3` (it uses the workspace's pinned crates):

```sh
git -C <logos-execution-zone> worktree add ../lez-040 v0.4.0-rc3
cp -r lez/reelcut_lease ../lez-040/examples/
# add to ../lez-040/Cargo.toml [workspace] members:
#   "examples/reelcut_lease", "examples/reelcut_lease/core",
#   "examples/reelcut_lease/methods", "examples/reelcut_lease/methods/guest"
curl -L https://risczero.com/install | bash && rzup install cargo-risczero 3.0.5 && rzup install r0vm 3.0.5 && rzup install rust
cargo build --release -p reelcut_lease -p wallet
cargo build --release --features standalone -p sequencer_service
```

A local chain (mocked Bedrock, dev mode without proofs), and the funded dev accounts:

```sh
sequencer_service setup ~/lez-local/seq --config lez/sequencer/service/networks/local.json \
  --funding-public-key 2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26 --create-channel
RISC0_DEV_MODE=1 sequencer_service start ~/lez-local/seq          # RPC 127.0.0.1:3040
export LEE_WALLET_HOME_DIR=~/lez-local/w LEE_WALLET_PASSWORD=x     # with lez/wallet/configs/debug/wallet_config.json
wallet check-health
wallet account import public --private-key 10a26a9aec7d34b82364eeae45c5294dbb0a764b000b94eeb9b58511dc487c4d
wallet account import public --private-key 717940b1cc55e5d6b2066dbf1d9a3f26f212f4db08d02388177fcfedd8a9be1b
```

## Tested (local v0.4.0-rc3 chain, 2026-11-13)

- deploy (2.5 min: several segments, 15 s blocks);
- offer 1,000,000 per hour, max 24; rent 2 periods: payer −2,000,000 (+ fee), treasury +2,000,000,
  lease paid through now + 2 h, record rentals 1 / periods 2;
- renew 1 period: +3,600,000 ms from the previous end, not from now;
- 25 periods (> max 24): refused by the program, nothing moved.

## Tested after the SPEL agent's review (2026-11-15, program HpSbyVRX…)

- rent 2 with `--max 1999999` (offer asks 2,000,000): refused, "the price went up since you read the offer";
- rent 2 with another treasury: refused; the payer's balance unchanged by both;
- rent 2 normally: treasury +2,000,000, lease paid through now + 20 min (10-minute periods), record 1 / 2.
