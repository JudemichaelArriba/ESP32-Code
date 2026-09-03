# TODO — Network Accountability & Reboot Occupancy Recovery

Design decisions confirmed with user (2026-09-03):

1. **NTP-not-yet-valid fallback**: assume OFF / NOT OCCUPIED until NTP is valid. Never
   restore presence or resume AC power from persisted state while `timeIsValid()` is
   false. Priority is preventing a false ON (e.g. middle-of-the-night reboot), even at
   the cost of a slower/more conservative recovery.
2. **Buffered decision logs**: any decision log entry queued locally during a Firebase
   outage and later replayed on reconnect must be flagged, e.g. `bufferedDuringOutage:
   true`, so downstream consumers (web app, billing, audit) can distinguish live vs.
   backfilled entries. Additive field only — do not remove/rename existing fields.
3. **Staleness cap on persisted occupancy/schedule timestamps**: add an explicit max-age
   guard (`OCCUPANCY_PERSIST_MAX_AGE_MS` or similar) rather than relying only on
   `windowKey`'s embedded date and `OCCUPANCY_HOLD_MS`. Chosen for defense-in-depth
   against clock anomalies (e.g. NTP briefly reporting a wrong date before correcting)
   — the safest option against falsely powering the AC outside any real schedule or
   manual window.

Implementation status: the code items below are done. The Testing section is
deliberately untouched and not yet executed (no compile, flash, or test run has
been performed).

Behavioural note discovered during implementation: the goal is **reboot
transparency** - a device that restarts mid-window reaches the same control
decision as one that never restarted. That means a warm recovery into a window
whose grace is already spent turns the AC off shortly after boot instead of
granting a fresh grace period. If the room is in fact occupied, the sensors
re-detect it and the next minute pass powers the AC back on.

---

## Problem 1 — Unlogged AC operation / lost energy during network outages

### 1A. Local decision-log ring buffer (RAM, drain on reconnect)
- [x] Add a bounded RAM ring buffer in `functions/logger_functions.h` (mirror the
      existing `DiagnosticRecord`/`DiagnosticHistory` pattern in
      `functions/persistence_functions.h`), sized to cover a full outage window
      (~10 minutes worth of expected events).
- [x] On `pushDecisionLog()` failure (Firebase unreachable), serialize the event
      (including its original wall-clock `updatedAt`) into the ring instead of
      dropping it.
- [x] On Firebase becoming ready again, drain the ring FIFO through the existing
      `pushDecisionLog()` path, preserving original timestamps for correct ordering.
- [x] Mark every replayed entry with `bufferedDuringOutage: true` (decision per item 2
      above). Confirm this new field is documented in `AGENTS.md`/`CLAUDE.md`
      Firebase Paths section.
- [x] Handle ring overflow (outage longer than buffer capacity) with an explicit
      "N events dropped" marker rather than silently losing the overflow.

### 1B. NVS checkpoint for energy state (reboot-during-outage survival)
- [x] Define an `EnergyCheckpoint` struct (dateKey, runtimeSeconds, sessionCount,
      sessionStartedAt) in `core/structures.h`.
- [x] Add load/save functions in `functions/persistence_functions.h` following the
      exact `restoreManualOverrideFromPreferences()` / `persistManualOverrideState()`
      pattern: versioned, change-detected writes (no redundant NVS writes), verified
      reads.
- [x] Checkpoint on each `flushEnergyRuntime()` call while Firebase is unreachable
      (piggyback on existing `ENERGY_FLUSH_INTERVAL_SEC` cadence — no new timer), and
      on `closeEnergySession()`.
- [x] On boot, before `initializeEnergyTrackingForCurrentState()`: if `timeIsValid()`
      is false, skip restoration entirely (per item 1 — do not resume any AC/energy
      state on unverified time). Once time is valid, if the persisted `dateKey`
      matches today, reconcile with Firebase's fetched `/energyState`/`/energyDaily`
      by taking the max of persisted vs. fetched runtime seconds.

### 1C. Explicit outage-window markers
- [x] Record `"network_outage_start"` / `"network_outage_recovered"` events via the
      existing `recordPersistentDiagnostic()` NVS ring (already reboot-durable,
      already uploads once on next successful heartbeat) so the outage window itself
      is provable even if buffered detail logs are lost to a power cycle mid-outage.

---

## Problem 2 — Occupancy/grace state lost on reboot

### 2A. New persisted state (wall-clock anchored)
- [x] Define a persisted struct in `functions/persistence_functions.h` /
      `core/structures.h`: `persistedScheduleWindowKey`,
      `persistedScheduleWindowStartEpoch` (wall-clock epoch, not `millis()`),
      `persistedLastPresenceEpoch`, `persistedPresenceHeld`.
- [x] Add `OCCUPANCY_PERSIST_MAX_AGE_MS` (or equivalent) to `config/config.h` per
      decision 3 — a persisted epoch older than this ceiling is never trusted for
      restoration, regardless of windowKey match.
- [x] Write cadence (flash-wear-conscious, matching existing change-detection style):
  - [x] Persist new window key + start epoch only when `justEnteredSchedule` fires
        (window key actually changes).
  - [x] Persist presence epoch only on direct PIR/MLX evidence updates (never from a
        derived/held value — same rule as `lastPresenceDetectedMillis` today),
        throttled to at most once per ~30-60s during continuous occupancy.

### 2B. Boot-time restoration logic
- [x] Add restoration step before the first `runMinuteControl()` call (after WiFi/NTP
      are usable, alongside the existing manual-override restore step):
  1. [ ] Require `timeIsValid()` — if false, **skip restoration entirely**; fall
         through to today's fresh-entry behavior (AC off / not occupied) per
         decision 1.
  2. [ ] Evaluate `evaluateScheduleStatus()` for "now" to get current `windowKey`.
  3. [ ] Reject restoration if `(nowEpoch - persistedScheduleWindowStartEpoch)` or
         `(nowEpoch - persistedLastPresenceEpoch)` exceeds
         `OCCUPANCY_PERSIST_MAX_AGE_MS` (decision 3 staleness guard).
  4. [ ] If `persistedScheduleWindowKey == currentWindowKey` and the staleness check
         passes: reconstruct `scheduleWindowEnteredMillis` in the `millis()` domain
         from the persisted epoch delta, set `lastScheduleWindowKey` directly (so
         `isFirstMinuteRun` does not treat this as a fresh window entry and does not
         reset the grace baseline).
  5. [ ] If additionally `(nowEpoch - persistedLastPresenceEpoch) <=
         OCCUPANCY_HOLD_MS/1000`: reconstruct `lastPresenceDetectedMillis` the same
         way and set `presenceDetected = true` immediately, instead of waiting for a
         fresh sensor read.
  6. [ ] If window key differs, or any staleness check fails: fall through to
         today's existing fresh-entry behavior unchanged (no behavior regression for
         the cold-start case).

### 2C. Documentation
- [x] Update `AGENTS.md`/`CLAUDE.md` (per its own "update this document in the same
      change" rule):
  - [x] New global state in structures/globals sections.
  - [x] New config constants.
  - [x] New `bufferedDuringOutage` decision-log field under Firebase Paths.
  - [x] Updated Occupancy State Model section describing warm-recovery restoration
        and the NTP-invalid-at-boot fallback.
  - [x] Updated Boot Flow section with the new restoration step.

---

## Problem 3 — Field log findings (hang/reset incident + missing energy-checkpoint visibility)

Source: field serial log review of the reboot-recovery patch, covering a hang/reset
incident and a missing-visibility observation on the energy checkpoint path.

Status: 3A and 3B are implemented. 3C and 3D were investigated against the live
Firebase project and are documented below — both are blocked from a definitive
answer, 3C because the incident device reports to a different project than the one
in `config/secrets.h`, and 3D because the evidence is structurally destroyed before
it can be read back. Neither is blocked on code. No compile, flash, or test run has
been performed for this section.

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

### 3A. Retry backoff for buffered decision-log drain — DONE
- [x] Add a minimum-retry-interval guard to `drainBufferedDecisionLogs()`, matching
      the existing pattern (`DECISION_LOG_DRAIN_RETRY_MS` = 10s in `config/config.h`,
      checked the same way `HEARTBEAT_FAILURE_RETRY_MS` gates `tickHeartbeat()`).
- [x] On a failed drain attempt, record the failure timestamp
      (`lastDecisionLogDrainFailureMillis`) and skip further attempts until the
      interval elapses, instead of retrying on the very next loop pass. Applied to
      both the entry-replay write and the overflow-marker write.
- [x] Keep the existing per-cycle cap (`DECISION_LOG_DRAIN_PER_CYCLE`) as the
      secondary bound — the interval guard controls *how often* a drain attempt
      starts, the per-cycle cap controls *how much* it can send once it does.
- [x] Verify this does not delay legitimate fast recovery: only a *failed* write sets
      the cooldown, and every successful write clears it, so a healthy backlog still
      drains 2 entries per loop pass (24 entries in ~12 passes, sub-second) exactly
      as before. The cooldown only engages on the unhealthy path.

### 3B. Observability for the energy checkpoint path — DONE
- [x] Add a success-path `Serial.printf` to `persistEnergyCheckpoint()` confirming a
      checkpoint was written (dateKey + runtimeSeconds + sessionCount). Placed after
      the change-detection early return, so an unchanged session does not spam.
- [x] Add a `Serial.println` to `clearEnergyCheckpoint()` confirming a checkpoint was
      cleared, on the real-clear branch only. The `energyCheckpointStored` no-op
      branch returns before it and stays silent, so the every-flush healthy path
      prints nothing. Also added a failure line for the NVS-open path, which was
      previously silent (matches its sibling `persistEnergyCheckpoint()`).
- [ ] Consider (optional, discuss before implementing): whether to also add minimal
      success/failure logging to `syncEnergyDailyCache()`/`syncEnergyStateToFirebase()`
      themselves, since their current total silence is what made this incident hard
      to diagnose from Serial alone — weigh against Serial spam risk given these are
      called frequently (every flush cycle).

### 3C. Confirm whether energy data was actually lost in the observed incident
- [x] Check `/devices/{DEVICE_ID}/energyState` and `/energyDaily/{2026-09-03}` in
      Firebase directly for the incident window (~22:00:44 AC-ON through the
      22:03–22:09 reboot cycle).

**Result — check performed, but the incident device is NOT in this project.** The
Firebase project configured in `config/secrets.h` contains three devices
(`<secrets DEVICE_ID>`, `ESP32-2`, `ESP32-3`) and two rooms
(`-OrNzAAXr8C1goBRcD94`, `-P0WDZWPmMHebcKuwWXy`). None of it matches the serial log:

- The log's room `-OyrQH9PMhtWMqj_s-5t` ("Room 15", Thursday 21:00–23:00 window,
  minutes 1260–1380) **does not exist** under `/rooms`.
- All three devices report pre-patch firmware (`2026.08.29-recovery3-occupancy2-diag1`
  ×2, `2026.08.28-recovery2` ×1). None reports
  `2026.09.03-recovery3-occupancy3-accounting1`, yet the log printed
  `"Recovery: schedule window resumed"` — a string that only exists in the patched build.
- Neither active device rebooted during the incident: both show `bootCount: 1` with
  `lastHeartbeatSuccessUptimeMs ≈ 35.8M ms` (~9.9 h continuous) at
  `lastSeen 2026-09-03T23:39`, i.e. booted ~13:45 and never reset at 22:03 or 22:08.
- No device has an `energyDaily/2026-09-03` node at all; the newest daily bucket is
  `2026-09-02`, and every `energyState` is stale from 2026-09-02 or earlier.
- `/decisionLogs` is **read-denied** (HTTP 401) for the device account — write-only by
  security rules — so decision-log history cannot be inspected with these credentials
  from any tooling that authenticates as the device.

- [ ] Re-run this check against the Firebase project/database the incident device
      actually reports to (the one containing Room 15 `-OyrQH9PMhtWMqj_s-5t`), then
      resolve the two branches below. Blocked until those credentials are available.
- [x] If it did land: confirm the pre-existing wall-clock flush logic correctly
      backfills the reboot-gap runtime — **confirmed from source, independent of the
      Firebase data.** `initializeEnergyTrackingForCurrentState()` only overwrites
      `lastFlushAt` when it is empty, so a session restored from Firebase keeps its
      original marker, and `flushEnergyRuntime()` then walks `lastFlushAt → now` and
      credits the entire span, including the time the ESP was rebooting. The AC keeps
      running through an ESP reset (IR is stateless on the AC side), so counting that
      span is correct, not an over-count.
- [ ] If it did not land: a crash inside the first 60s of a session (before the first
      periodic flush) has no checkpoint protection, since neither Firebase nor NVS has
      anything yet — `startEnergySession()` syncs to Firebase but does not write an NVS
      checkpoint. In the observed incident the freeze began ~5s after AC-on, well
      inside that window. Decide whether an immediate NVS checkpoint at session start
      is worth one extra flash write per AC-on transition, or whether this is accepted
      risk. Cannot be settled until the branch above is resolved.

### 3D. Root-cause confirmation for the hang itself (diagnostic only, not a fix)
- [x] Check the `resetReason` field on the next successful heartbeat after the incident.

**Result — structurally unavailable for this incident, independent of the project
mismatch.** Two mechanisms destroy the evidence before it can be read back:

1. **The hang's `resetReason` was never uploaded.** After the 22:03:06 reset the device
   never obtained WiFi (log Part 2 runs from 22:03:07 to the 22:08:29 portal timeout
   with no connection), so `tickHeartbeat()` never ran and that boot's reset reason was
   never written. The first heartbeat after the incident (22:08:41) reported the
   *22:08:29* portal-timeout restart instead — an `ESP.restart()`, i.e. `resetReason=3`,
   which says nothing about the hang.
2. **The breadcrumb that would have identified it is overwritten 60s later.** The RTC
   `runtimeBreadcrumb` survives a SW/watchdog reset, so the 22:03:07 boot should have
   captured it and written a `previous_reset` entry naming the exact blocking operation
   into the NVS diagnostic ring. That ring uploads on the first successful heartbeat
   (22:08:41) and is then cleared locally — but `/status` is written with `setJSON`, so
   the *next* heartbeat 60s later replaces the whole node and erases
   `diagnostics/items` and `previousResetOperation`. All three devices now show
   `diagnostics: {count: 0}` and no `previousReset*` fields, consistent with this.
   (Already noted in the Known Constraints section of `AGENTS.md`/`CLAUDE.md`.)

   Consequence: post-hoc diagnosis of this incident class is only possible if `/status`
   is read within ~60s of the recovery heartbeat. Worth considering whether the
   diagnostic ring should also append to a durable path rather than only to the
   `setJSON`-replaced `/status` node — deliberately not changed here.

- [ ] If reproducible, consider adding a temporary free-heap log
      (`ESP.getFreeHeap()`) immediately before and after the ML `HTTPClient` POST
      call, to confirm or rule out the three-concurrent-TLS-context
      (`fbdo` + `streamFbdo` + ML `HTTPClient`) heap-pressure hypothesis. This is a
      pre-existing condition unrelated to this patch's changes — diagnostic only,
      not something to fix under this section unless confirmed. Note: the two healthy
      devices report `freeHeap ≈ 170 KB` / `minimumFreeHeap ≈ 143 KB` while idle with
      no ML call in flight, so the headroom question is specifically about the third
      TLS context during the ML POST, not about steady-state heap.

---

## Problem 4 — Energy checkpoint retired on a reachability probe instead of a confirmed write

Source: field serial log, 2026-09-04 WiFi-outage recovery. Found by the user.

**Verification policy for this section: code review only. No test run, no compile,
no flash, no hardware verification of any kind after implementing — only reading
the code back to confirm the change is correct.**

Observed sequence — the checkpoint was deleted while every Firebase write was
still failing:

```
Energy checkpoint: saved 2026-09-04 runtime=4169s sessions=8.
WiFi restored; rebuilding Firebase session without rebooting.
Firebase initialized.
Heartbeat: write failed (token is not ready (revoked or expired))
Energy checkpoint: cleared, totals are back in Firebase.   <-- deleted anyway
DecisionLog: write failed (token is not ready (revoked or expired))
Failed to read /rooms: token is not ready (revoked or expired)
```

Findings:

1. **`flushEnergyRuntime()` decides clear-vs-persist from `canSyncEnergyToFirebase()`**,
   which is `firebaseInitialized && WiFi.status() == WL_CONNECTED && Firebase.ready()`
   — a reachability probe, not a write result. After a session rebuild all three are
   true while the token is still stale, so the guard says "reachable" and the
   checkpoint is cleared even though nothing was written.
2. **Both write helpers already return `bool`, and both results are discarded.**
   `syncEnergyDailyCache()` and `syncEnergyStateToFirebase()` each return `false` for
   *unreachable* and for *write failed*, so the correct signal was available and unused.
3. **`closeEnergySession()` has the same bug inverted**: `if (!canSyncEnergyToFirebase())
   persistEnergyCheckpoint();` — when a write fails but reachability reads true, no
   checkpoint is written at all and the session close is unprotected.
4. **The code is internally inconsistent**: `restoreEnergyCheckpointIfNeeded()` already
   does it correctly with `if (syncEnergyDailyCache()) clearEnergyCheckpoint();`. One
   site right, two wrong.
5. **The log line asserts something never verified** ("totals are back in Firebase").
6. **Blast radius**: `energyDailyCache.runtimeSeconds` is an absolute total held in RAM,
   so a later successful flush still pushes the correct cumulative value — no loss while
   the device stays up. The danger is the window between `clearEnergyCheckpoint()` and
   the next confirmed write: NVS empty, Firebase has nothing, RAM is the only copy. A
   reboot in that window loses the runtime permanently, which is exactly what the
   checkpoint exists to prevent. Not theoretical on a device that has been
   watchdog-rebooting during outages.
7. **Root cause is the same trap already documented for the drain backoff**:
   `Firebase.ready()` reports token lifecycle state, not whether a write will succeed.
   The project already enforces this principle for occupancy (`lastPresenceReported`
   means confirmed, not attempted); the energy checkpoint needed it and did not get it.

### 4A. Gate the clear/persist decision on confirmed writes — DONE
- [x] `flushEnergyRuntime()`: introduce a `writesConfirmed` flag; set it false when
      `syncEnergyDailyCache()` returns false inside the day-segment loop, and false when
      `syncEnergyStateToFirebase()` returns false at the end.
- [x] Replace `if (canSyncEnergyToFirebase())` with `if (writesConfirmed)` for the
      clear-vs-persist decision. Require **both** writes to be confirmed, since the
      checkpoint carries `runtimeSeconds`/`sessionCount` (from `/energyDaily`) and
      `lastFlushAt` (from `/energyState`).
- [x] Confirm the offline path is unchanged: both helpers already return false when
      unreachable, so an outage still persists the checkpoint exactly as before. This
      change only closes the "reachable but the write failed" hole.

### 4B. Same fix for the session-close path — DONE
- [x] `closeEnergySession()`: replace `if (!canSyncEnergyToFirebase()) persistEnergyCheckpoint();`
      with `if (!syncEnergyStateToFirebase()) persistEnergyCheckpoint();` so a failed
      write persists the checkpoint instead of silently skipping it.

### 4C. Correct the misleading log line — DONE
- [x] `clearEnergyCheckpoint()`: change "cleared, totals are back in Firebase" to
      wording that reflects what was actually verified, e.g. "cleared after confirmed
      Firebase write".

### 4D. Documentation — DONE
- [x] `AGENTS.md`/`CLAUDE.md` energy module bullet: "whenever a flush **cannot reach
      Firebase**" → "whenever a flush **write is not confirmed**".
- [x] New **Do NOT** rule generalising the occupancy principle: never decide durability
      (clearing a checkpoint, retiring local state, marking something synced) from a
      reachability probe such as `Firebase.ready()`; only a confirmed write result may
      retire local durable state.

### 4E. Explicitly out of scope for this section — ACKNOWLEDGED, NOT CHANGED
- [x] `startEnergySession()` discards both sync results and writes no checkpoint, so a
      crash inside the first 60s of a session is still unprotected. Already tracked as
      the open decision in **3C** — needs a call on whether one extra flash write per
      AC-on transition is acceptable. Not changed here.
- [x] The corrupt-`lastFlushAt` repair path at the top of `flushEnergyRuntime()` also
      discards its sync result, but it accumulates no runtime, so there is nothing to
      lose. Not changed here.
- [x] The watchdog / circuit-breaker work (loop stalls past `LOOP_WATCHDOG_TIMEOUT_MS`
      during an outage because `WiFi.status()` still reports connected while every
      synchronous Firebase call runs to a full timeout) is a separate, larger problem
      and is **not yet tracked in this file**. Confirmed by a real panic:
      `task_wdt: Task watchdog got triggered ... loopTask (CPU 1)`.

---

## Testing (Run only a test when user says so /per CLAUDE.md's "Before Finishing" / Testing section)
- [ ] Outage with no reboot: verify decision logs buffer and replay in order with
      `bufferedDuringOutage: true`, energy runtime keeps accumulating correctly.
- [ ] Outage + reboot mid-outage: verify energy checkpoint restores correctly and
      does not double-count against the Firebase-side value once reconnected.
- [ ] Reboot mid-schedule-window with recent real presence (<5 min old): verify AC
      stays on and does not falsely shut off after only ~5 min from boot.
- [ ] Reboot mid-schedule-window with stale presence (room already empty before
      reboot): verify AC does not get an unintended fresh grace period restart.
- [ ] Reboot with NTP not yet valid: verify device stays OFF / not-occupied until
      NTP resolves, even if a schedule should be active (decision 1).
- [ ] Reboot after long power-off (hours/days) landing inside a same-day window by
      coincidence: verify `OCCUPANCY_PERSIST_MAX_AGE_MS` prevents false restoration.
- [ ] Reboot outside any schedule/manual window: verify no restoration path can
      cause an unintended AC ON.
- [ ] Ring buffer overflow during an unusually long outage: verify explicit
      drop-count marker instead of silent loss.
- [ ] Flash-wear sanity check: confirm NVS write frequency stays bounded under
      continuous occupancy (throttled presence-epoch writes) and rapid brownout-loop
      conditions (window-key writes only on actual key change).
- [ ] Simulate a Firebase outage while a decision log is buffered and confirm
      `drainBufferedDecisionLogs()` now waits the configured interval between
      attempts instead of retrying every loop pass (Problem 3A).
- [ ] Confirm the interval guard doesn't measurably delay drain-after-recovery for
      a short, normal-length outage (Problem 3A).
- [ ] Confirm `persistEnergyCheckpoint()`/`clearEnergyCheckpoint()` success lines
      appear in Serial during a real outage-then-recovery cycle (Problem 3B).
- [ ] Cross-check a live AC-on transition's Firebase writes (`/energyState`,
      `/acState`) against Serial output to confirm Problem 3C's hypothesis either way.
- [ ] Re-run the exact field scenario (ML call immediately followed by AC-on
      decision logging) if reproducible, to see whether resetReason confirms the
      watchdog hypothesis (Problem 3D).
