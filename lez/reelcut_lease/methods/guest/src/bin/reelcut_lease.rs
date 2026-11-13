//! reelcut_lease: rent a Reelcut worker by the period, paid in the native token.
//!
//! Two kinds of actor under this program:
//!
//! - **Offer** `Actor(worker, this program)`: the worker's terms (treasury, price per period, period
//!   length, longest rental), written by the worker (`SetOffer`, signed by the worker's account).
//! - **Lease** `Actor(lease, this program)`, `lease = for_public_pda(this program, seed(worker, client))`:
//!   how long a client key is paid for at that worker (`paid_through_ms`), and who paid.
//!
//! `Rent` goes to the worker's offer. The offer reads its own terms, charges the payer (who signs the
//! transaction) `price × periods` into the treasury, and tells the lease to extend by `periods` from
//! `max(now, paid_through)`. "Now" is the client's claim, held to within `CLOCK_SLACK_MS` of the block
//! time by the transaction's timestamp window: v0.4 programs don't read a clock.
//!
//! The worker serves a client key while its lease's `paid_through_ms` is ahead of the time it reads
//! from the chain. Nothing else is on chain: the work itself goes between the two directly.


use lee_core::{
    account::{AccountId, Actor},
    native_token,
    program::{ReceiveInput, Response, SyncMessage},
};
use reelcut_lease_core::*;

fn offer_state(input: &ReceiveInput) -> Option<OfferState> {
    if input.pre_state.as_ref().is_empty() { None } else { Some(borsh::from_slice(input.pre_state.as_ref()).expect("offer state")) }
}

lee_core::actor_entrypoint!(handle_message);

fn handle_message(input: &ReceiveInput, message: Message) -> Response {
    match message {
        Message::SetOffer(offer) => {
            assert!(input.is_authorized, "only the worker sets its offer");
            assert!(input.from.is_none(), "an offer is set by a transaction of the worker's");
            assert!(offer.period_ms >= 60_000, "a period is a minute or more");
            assert!(offer.max_periods >= 1, "allow at least one period");
            let record = offer_state(input).map(|s| s.record).unwrap_or_default();
            Response::set_state(borsh::to_vec(&OfferState { offer, record }).expect("encode"))
        }
        Message::Rent { client_key, payer, periods, now_ms } => {
            let mut state = offer_state(input).expect("this worker has no offer");
            let offer = state.offer.clone();
            assert!(offer.open, "this worker takes no new rentals");
            assert!(periods >= 1 && periods <= offer.max_periods, "periods out of range");
            let amount = offer.price_per_period.checked_mul(u128::from(periods)).expect("price overflows");
            let worker = input.receiver.account_id;
            let program = input.receiver.program_account_id;
            let seed = lease_seed(&worker, &client_key);
            let lease = AccountId::for_public_pda(&program, &seed);
            let add_ms = offer.period_ms.checked_mul(u64::from(periods)).expect("time overflows");
            state.record.rentals += 1;
            state.record.periods_sold += u64::from(periods);
            let mut r = Response::set_state(borsh::to_vec(&state).expect("encode"));
            if amount > 0 {
                // The payer signed the transaction, so its native balance may be debited here.
                r = r.send_sync(
                    Actor::native_balance(payer),
                    &native_token::Message::Transfer { to: offer.treasury, amount },
                );
            }
            r.send(
                SyncMessage::new(
                    Actor::new(lease, program),
                    &Message::Extend { worker, client_key, payer, add_ms, now_ms },
                )
                .with_pda_seeds(vec![seed]),
            )
            .try_timestamp_window(now_ms.saturating_sub(CLOCK_SLACK_MS)..now_ms.saturating_add(CLOCK_SLACK_MS))
            .expect("a valid time window")
        }
        Message::Extend { worker, client_key, payer, add_ms, now_ms } => {
            assert!(input.from_own_program() && input.is_authorized, "a lease is extended by its worker's offer only");
            let from = input.from.expect("sender").account_id;
            assert_eq!(from, worker, "a lease belongs to one worker");
            let mut lease = if input.pre_state.as_ref().is_empty() {
                Lease { worker, client_key, payer, paid_through_ms: 0 }
            } else {
                borsh::from_slice::<Lease>(input.pre_state.as_ref()).expect("lease state")
            };
            assert_eq!(lease.client_key, client_key, "another client's lease");
            // The first payer owns the lease; others may only extend it.
            let _ = payer;
            lease.paid_through_ms = lease.paid_through_ms.max(now_ms).saturating_add(add_ms);
            Response::set_state(borsh::to_vec(&lease).expect("encode"))
        }
    }
}
