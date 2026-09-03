# TODO — Serial Log Findings (Field Validation Follow-up)

Source: field serial log review of the reboot-recovery patch (see prior report),
covering a hang/reset incident and a missing-visibility observation on the energy
checkpoint path. No code has been changed yet. This file only tracks the plan.

Findings, in order of severity:

1. **`drainBufferedDecisionLogs()` has no retry backoff**, unlike every other retry
   site in this codebase (`HEARTBEAT_FAILURE_RETRY_MS`, `FIREBASE_STREAM_RETRY_MS`,
   `ROOMS_FETCH_RETRY_MS`). Once anything is buffered, it attempts a blocking
   `Firebase.RTDB.pushJSON()` on every single loop pass with no minimum interval,
   gated only by `Firebase.ready()` — a token/auth flag that does not necessarily
   reflect the live health of the socket that just broke. Confirmed real gap;
   plausible contributing/amplifying factor in the observed hang, independent of
   whether it was the specific call that hung.
2. **No success-path Serial logging on the energy checkpoint.**
   `persistEnergyCheckpoint()` and `clearEnergyCheckpoint()` only print on failure
   (NVS open/write failure); there is no confirmation line when a checkpoint write
   or clear succeeds. This is why the log showed nothing for the energy path even
   though the checkpoint code may have run correctly (or never had a chance to run
   at all in this specific incident — see finding 4).
3. **The wider energy-sync pipeline is already silent** (`syncEnergyDailyCache()`,
   `syncEnergyStateToFirebase()` have zero `Serial.println` calls, success or
   failure) — pre-existing, not introduced by this patch, but compounds finding 2
   and makes the whole energy path hard to audit from Serial alone.
4. **Unconfirmed**: whether the ~22:00:44 AC-ON energy-session-start actually
   reached Firebase before the SSL breakdown began. If it did, the pre-existing
   wall-clock flush logic should have backfilled the reboot-gap runtime correctly
   on its own (unrelated to the NVS checkpoint, which never got a chance to fire —
   the first periodic flush wasn't due for ~60s and the freeze began ~5s after
   AC-on). Needs verification against Firebase directly, not just the Serial log.

---

## A. Retry backoff for buffered decision-log drain

- [ ] Add a minimum-retry-interval guard to `drainBufferedDecisionLogs()`, matching
      the existing pattern (e.g. a new `DECISION_LOG_DRAIN_RETRY_MS` constant in
      `config/config.h`, checked the same way `HEARTBEAT_FAILURE_RETRY_MS` gates
      `tickHeartbeat()`).
- [ ] On a failed drain attempt, record the failure timestamp and skip further
      attempts until the interval elapses, instead of retrying on the very next
      loop pass.
- [ ] Keep the existing per-cycle cap (`DECISION_LOG_DRAIN_PER_CYCLE`) as the
      secondary bound — the interval guard controls *how often* a drain attempt
      starts, the per-cycle cap controls *how much* it can send once it does.
- [ ] Verify this does not delay legitimate fast recovery (e.g. a brief blip):
      the interval should be short enough that a normal reconnect still drains
      promptly, not just long enough to avoid hammering a broken session.

## B. Observability for the energy checkpoint path

- [ ] Add a success-path `Serial.println`/`Serial.printf` to `persistEnergyCheckpoint()`
      confirming a checkpoint was written (dateKey + runtimeSeconds), not just the
      existing failure-path messages.
- [ ] Add a `Serial.println` to `clearEnergyCheckpoint()` confirming a checkpoint was
      cleared (only when one actually existed — the existing `energyCheckpointStored`
      guard already tracks this, so the log only needs to fire on the real-clear
      branch, not the already-empty no-op branch).
- [ ] Consider (optional, discuss before implementing): whether to also add minimal
      success/failure logging to `syncEnergyDailyCache()`/`syncEnergyStateToFirebase()`
      themselves, since their current total silence is what made this incident hard
      to diagnose from Serial alone — weigh against Serial spam risk given these are
      called frequently (every flush cycle).

## C. Confirm whether energy data was actually lost in the observed incident

- [ ] Check `/devices/{DEVICE_ID}/energyState` and `/energyDaily/{2026-09-03}` in
      Firebase directly for the incident window (~22:00:44 AC-ON through the
      22:03–22:09 reboot cycle) to determine whether the initial session-start sync
      landed before the SSL breakdown.
- [ ] If it did land: confirm the pre-existing wall-clock flush logic correctly
      backfilled the reboot-gap runtime on the next flush after recovery (expected,
      per the pre-existing `lastFlushAt`-based catch-up walk in `flushEnergyRuntime()`).
- [ ] If it did not land: this is a real gap distinct from findings A/B — a crash
      within the window before the *first* periodic flush (60s) has no checkpoint
      protection yet, since nothing has been persisted locally or remotely by that
      point. Decide whether an immediate NVS checkpoint at session-start (in addition
      to the existing periodic-flush checkpoint) is worth the extra flash write per
      AC-on transition, or whether this narrow window is an accepted risk.

## D. Root-cause confirmation for the hang itself (diagnostic only, not a fix)

- [ ] Check the `resetReason` field on the next successful heartbeat after this kind
      of incident (`6` = task watchdog, `4` = panic, `3` = software reset) to confirm
      or rule out the task-watchdog hypothesis.
- [ ] If reproducible, consider adding a temporary free-heap log
      (`ESP.getFreeHeap()`) immediately before and after the ML `HTTPClient` POST
      call, to confirm or rule out the three-concurrent-TLS-context
      (`fbdo` + `streamFbdo` + ML `HTTPClient`) heap-pressure hypothesis. This is a
      pre-existing condition unrelated to this patch's changes — diagnostic only,
      not something to fix under this TODO unless confirmed.

---

## Testing (not to be executed until requested)
- [ ] Simulate a Firebase outage while a decision log is buffered and confirm
      `drainBufferedDecisionLogs()` now waits the configured interval between
      attempts instead of retrying every loop pass.
- [ ] Confirm the interval guard doesn't measurably delay drain-after-recovery for
      a short, normal-length outage.
- [ ] Confirm `persistEnergyCheckpoint()`/`clearEnergyCheckpoint()` success lines
      appear in Serial during a real outage-then-recovery cycle.
- [ ] Cross-check a live AC-on transition's Firebase writes (`/energyState`,
      `/acState`) against Serial output to confirm finding C's hypothesis either way.
- [ ] Re-run the exact field scenario (ML call immediately followed by AC-on
      decision logging) if reproducible, to see whether resetReason confirms the
      watchdog hypothesis.
