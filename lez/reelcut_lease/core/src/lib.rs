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
    pub paid_through_ms: u64,
}

#[derive(BorshSerialize, BorshDeserialize, Clone, PartialEq, Eq, Debug)]
pub enum Message {
    /// To the worker's offer actor, signed by the worker.
    SetOffer(Offer),
    /// To the worker's offer actor, signed by `payer`. `max_amount` and `treasury` are the terms the payer
    /// read: the rental is refused if the offer now charges more or pays elsewhere.
    Rent { client_key: [u8; 32], payer: AccountId, periods: u32, now_ms: u64, max_amount: u128, treasury: AccountId },
    /// From the offer to a lease (this program only).
    Extend { worker: AccountId, client_key: [u8; 32], add_ms: u64, now_ms: u64 },
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


#[cfg(test)]
mod tests {
    use super::*;
    /// Pinned: Reelcut computes lease accounts itself (core/src/lez_lease.cpp, test in remote_test.cpp).
    #[test]
    fn lease_account_vector() {
        let program = AccountId::new([1u8; 32]);
        let worker = AccountId::new([2u8; 32]);
        let got = lease_account(&program, &worker, &[3u8; 32]);
        println!("LEASE_VECTOR {}", hex::encode(got.value()));
        assert_eq!(hex::encode(got.value()), LEASE_VECTOR);
    }
    /// Pinned: Reelcut builds Rent itself (rentMessage in core/src/lez_lease.cpp).
    #[test]
    fn rent_encoding_vector() {
        let m = Message::Rent { client_key: [3u8; 32], payer: AccountId::new([4u8; 32]), periods: 2, now_ms: 7,
                                max_amount: 10, treasury: AccountId::new([5u8; 32]) };
        assert_eq!(hex::encode(borsh::to_vec(&m).unwrap()), RENT_VECTOR);
    }
    const RENT_VECTOR: &str = "01030303030303030303030303030303030303030303030303030303030303030304040404040404040404040404040404040404040404040404040404040404040200000007000000000000000a0000000000000000000000000000000505050505050505050505050505050505050505050505050505050505050505";
    const LEASE_VECTOR: &str = "d6b54235f1b14e19a5912f69323730fe470996707828dbae0b386e62b27f43cb";
}
