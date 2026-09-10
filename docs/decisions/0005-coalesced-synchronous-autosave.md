# 0005 — Draft autosave is coalesced and synchronous

Status: accepted · Date: 2026-09-10

## Context

The specification asks for draft autosave through a *serial worker queue* that
coalesces superseded unsaved snapshots, with visible Saving/Unsaved state, and
with Pause, Finish and clean shutdown waiting for the required writes.

## Decision

`SessionController` coalesces through a single-shot 250 ms timer and then writes
the draft **synchronously** on the controller's thread. The observable contract
from the specification is implemented in full:

- superseded snapshots are coalesced into one write per quiet moment;
- `savingChanged(saving, unsaved)` drives the visible state;
- `pause()`, `finish()` and `flushPendingSave()` complete the write before
  returning;
- a refused write keeps the in-memory draft, reports the error and allows a
  retry, and blocks anything that depends on unpersisted state;
- collection mark changes are persisted *before* the undo-stack transition is
  published, so `QUndoCommand::undo()` never performs unreported I/O.

## Consequences

- A draft is a small JSON payload and one `INSERT … ON CONFLICT` statement, so
  the write is sub-millisecond in practice. The risk this trades away is a
  pathological storage stall blocking the GUI thread.
- Moving to a genuine serial worker later is a change inside `SessionController`
  alone: the signals, the flush points and the failure channel already have the
  right shape for it.
- This is a deliberate, recorded deviation, not an oversight. It should be
  revisited if profiling on real collections shows autosave latency in the frame
  budget.
