//! reelcut-lease: deploy and drive the reelcut_lease program from a LEZ wallet (tests, ops).
//!
//!   reelcut_lease deploy <payer>                                 -> program account id
//!   reelcut_lease offer  <program> <worker> <treasury> <price> <period_ms> <max_periods> [--closed]
//!   reelcut_lease rent   <program> <worker> <client_key_hex> <payer> <periods>
//!   reelcut_lease show   <program> <worker> [client_key_hex]     -> the offer (and a lease) as JSON
//!
//! The wallet comes from the environment like `wallet` itself (LEE_WALLET_HOME_DIR, config).
use anyhow::{Context as _, Result, bail};
use clap::{Parser, Subcommand};
use lee::{AccountId, Actor};
use lee_core::native_token::NATIVE_TOKEN_PROGRAM_ID;
use reelcut_lease_core::*;
use sequencer_service_rpc::SequencerClient;
use wallet::{AccountIdentity, WalletCore};

#[derive(Parser)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    Deploy { payer: AccountId },
    Offer { program: AccountId, worker: AccountId, treasury: AccountId, price: u128, period_ms: u64, max_periods: u32,
            #[arg(long)] closed: bool },
    Rent { program: AccountId, worker: AccountId, client_key: String, payer: AccountId, periods: u32 },
    Show { program: AccountId, worker: AccountId, client_key: Option<String> },
}

fn key32(s: &str) -> Result<[u8; 32]> {
    let v = hex::decode(s.trim_start_matches("0x")).context("client key: hex")?;
    v.try_into().map_err(|_| anyhow::anyhow!("client key: 32 bytes"))
}

fn now_ms() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis() as u64
}

fn ser(m: &Message) -> Vec<u8> {
    borsh::to_vec(m).expect("encode")
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    let mut w = WalletCore::from_env()?;
    match cli.cmd {
        Cmd::Deploy { payer } => {
            let elf = reelcut_lease_methods::REELCUT_LEASE_ELF.to_vec();
            let id = program_deployment::deploy_program(&mut w, elf, payer).await?;
            println!("{id}");
        }
        Cmd::Offer { program, worker, treasury, price, period_ms, max_periods, closed } => {
            let m = Message::SetOffer(Offer { treasury, price_per_period: price, period_ms, max_periods, open: !closed });
            let accounts = vec![AccountIdentity::Public(worker).select_program_actor_state(program)];
            let h = w.send_pub_tx_paid_by(accounts, 0, ser(&m).into(), None).await.map_err(|e| anyhow::anyhow!("{e:?}"))?;
            println!("{}", hex::encode(h.as_ref()));
        }
        Cmd::Rent { program, worker, client_key, payer, periods } => {
            let ck = key32(&client_key)?;
            let offer = read_offer(&w, program, worker).await?.context("this worker has no offer")?;
            let m = Message::Rent { client_key: ck, payer, periods, now_ms: now_ms() };
            let accounts = vec![
                AccountIdentity::PublicNoSign(worker).select_program_actor_state(program),
                AccountIdentity::Public(payer).select_program_actor_state(NATIVE_TOKEN_PROGRAM_ID),
                AccountIdentity::PublicNoSign(offer.offer.treasury).select_program_actor_state(NATIVE_TOKEN_PROGRAM_ID),
                AccountIdentity::PublicPda { program, seed: lease_seed(&worker, &ck) }.select_program_actor_state(program),
            ];
            let h = w.send_pub_tx_paid_by(accounts, 0, ser(&m).into(), Some(payer)).await.map_err(|e| anyhow::anyhow!("{e:?}"))?;
            println!("{}", hex::encode(h.as_ref()));
        }
        Cmd::Show { program, worker, client_key } => {
            let offer = read_offer(&w, program, worker).await?;
            let mut out = format!("{{\"offer\":{}", match &offer {
                Some(o) => format!("{{\"treasury\":\"{}\",\"price\":{},\"periodMs\":{},\"maxPeriods\":{},\"open\":{},\"rentals\":{},\"periodsSold\":{}}}",
                    o.offer.treasury, o.offer.price_per_period, o.offer.period_ms, o.offer.max_periods, o.offer.open,
                    o.record.rentals, o.record.periods_sold),
                None => "null".into(),
            });
            if let Some(ck) = client_key {
                let ck = key32(&ck)?;
                let lease = lease_account(&program, &worker, &ck);
                let acc = w.get_account_view(Actor::new(lease, program)).await.map_err(|e| anyhow::anyhow!("{e:?}"))?;
                let st = acc.map(|a| a.data.actor_state(program).as_ref().to_vec()).unwrap_or_default();
                out += &if st.is_empty() { ",\"lease\":null".to_string() } else {
                    let l: Lease = borsh::from_slice(&st)?;
                    format!(",\"lease\":{{\"account\":\"{lease}\",\"payer\":\"{}\",\"paidThroughMs\":{},\"nowMs\":{}}}", l.payer, l.paid_through_ms, now_ms())
                };
            }
            println!("{out}}}");
        }
    }
    Ok(())
}

async fn read_offer(w: &WalletCore<SequencerClient>, program: AccountId, worker: AccountId) -> Result<Option<OfferState>> {
    let acc = w.get_account_view(Actor::new(worker, program)).await.map_err(|e| anyhow::anyhow!("{e:?}"))?;
    let st = acc.map(|a| a.data.actor_state(program).as_ref().to_vec()).unwrap_or_default();
    if st.is_empty() { return Ok(None); }
    if st.len() < 32 { bail!("not an offer"); }
    Ok(Some(borsh::from_slice(&st)?))
}
