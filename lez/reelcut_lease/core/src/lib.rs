//! reelcut_lease's messages and states, shared by the program and its clients.
use borsh::{BorshDeserialize, BorshSerialize};
use lee_core::{account::AccountId, program::PdaSeed};

/// How far the client's "now" may be from the block's time.
pub const CLOCK_SLACK_MS: u64 = 120_000;

#[derive(BorshSerialize, BorshDeserialize, Clone, PartialEq, Eq, Debug)]
pub struct Offer {
    pub treasury: AccountId,
    pub price_per_period: u128,
    pub period_ms: u64,
    pub max_periods: u32,
    /// Off: no new rentals (running leases stay valid).
    pub open: bool,
    /// The worker's link key (X25519 public key): clients talk only to the worker holding it, so an
    /// offer seen elsewhere (with another address or key) can't send them to someone else.
    pub key: [u8; 32],
}

/// What clients did at a worker: written by this program only. (Reviews and other reputation
/// signals are a separate design: docs/remote-execution.md in reelcut-basecamp.)
#[derive(BorshSerialize, BorshDeserialize, Clone, PartialEq, Eq, Debug, Default)]
pub struct Record {
    pub rentals: u64,
    pub periods_sold: u64,
}

/// An offer actor's state: the terms and the record.
#[derive(BorshSerialize, BorshDeserialize, Clone, PartialEq, Eq, Debug)]
pub struct OfferState {
    pub offer: Offer,
    pub record: Record,
}

#[derive(BorshSerialize, BorshDeserialize, Clone, PartialEq, Eq, Debug)]
pub struct Lease {
    pub worker: AccountId,
    pub client_key: [u8; 32],
    pub payer: AccountId,
    pub paid_through_ms: u64,
}

#[derive(BorshSerialize, BorshDeserialize, Clone, PartialEq, Eq, Debug)]
pub enum Message {
    /// To the worker's offer actor, signed by the worker.
    SetOffer(Offer),
    /// To the worker's offer actor, signed by `payer`.
    Rent { client_key: [u8; 32], payer: AccountId, periods: u32, now_ms: u64 },
    /// From the offer to a lease (this program only).
    Extend { worker: AccountId, client_key: [u8; 32], payer: AccountId, add_ms: u64, now_ms: u64 },
}

/// The lease account of a client key at a worker.
pub fn lease_seed(worker: &AccountId, client_key: &[u8; 32]) -> PdaSeed {
    use sha2::{Digest as _, Sha256};
    let mut h = Sha256::new();
    h.update(b"reelcut-lease/v1");
    h.update(worker.value());
    h.update(client_key);
    PdaSeed::new(h.finalize().into())
}

/// The lease account of a client key at a worker, under the program `program`.
pub fn lease_account(program: &AccountId, worker: &AccountId, client_key: &[u8; 32]) -> AccountId {
    AccountId::for_public_pda(program, &lease_seed(worker, client_key))
}

