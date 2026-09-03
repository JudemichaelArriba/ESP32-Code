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

No code has been changed yet. This file only tracks the implementation plan.

---

## Problem 1 — Unlogged AC operation / lost energy during network outages

### 1A. Local decision-log ring buffer (RAM, drain on reconnect)
- [ ] Add a bounded RAM ring buffer in `functions/logger_functions.h` (mirror the
      existing `DiagnosticRecord`/`DiagnosticHistory` pattern in
      `functions/persistence_functions.h`), sized to cover a full outage window
      (~10 minutes worth of expected events).
- [ ] On `pushDecisionLog()` failure (Firebase unreachable), serialize the event
      (including its original wall-clock `updatedAt`) into the ring instead of
      dropping it.
- [ ] On Firebase becoming ready again, drain the ring FIFO through the existing
      `pushDecisionLog()` path, preserving original timestamps for correct ordering.
- [ ] Mark every replayed entry with `bufferedDuringOutage: true` (decision per item 2
      above). Confirm this new field is documented in `AGENTS.md`/`CLAUDE.md`
      Firebase Paths section.
- [ ] Handle ring overflow (outage longer than buffer capacity) with an explicit
      "N events dropped" marker rather than silently losing the overflow.

### 1B. NVS checkpoint for energy state (reboot-during-outage survival)
- [ ] Define an `EnergyCheckpoint` struct (dateKey, runtimeSeconds, sessionCount,
      sessionStartedAt) in `core/structures.h`.
- [ ] Add load/save functions in `functions/persistence_functions.h` following the
      exact `restoreManualOverrideFromPreferences()` / `persistManualOverrideState()`
      pattern: versioned, change-detected writes (no redundant NVS writes), verified
      reads.
- [ ] Checkpoint on each `flushEnergyRuntime()` call while Firebase is unreachable
      (piggyback on existing `ENERGY_FLUSH_INTERVAL_SEC` cadence — no new timer), and
      on `closeEnergySession()`.
- [ ] On boot, before `initializeEnergyTrackingForCurrentState()`: if `timeIsValid()`
      is false, skip restoration entirely (per item 1 — do not resume any AC/energy
      state on unverified time). Once time is valid, if the persisted `dateKey`
      matches today, reconcile with Firebase's fetched `/energyState`/`/energyDaily`
      by taking the max of persisted vs. fetched runtime seconds.

### 1C. Explicit outage-window markers
- [ ] Record `"network_outage_start"` / `"network_outage_recovered"` events via the
      existing `recordPersistentDiagnostic()` NVS ring (already reboot-durable,
      already uploads once on next successful heartbeat) so the outage window itself
      is provable even if buffered detail logs are lost to a power cycle mid-outage.

---

## Problem 2 — Occupancy/grace state lost on reboot

### 2A. New persisted state (wall-clock anchored)
- [ ] Define a persisted struct in `functions/persistence_functions.h` /
      `core/structures.h`: `persistedScheduleWindowKey`,
      `persistedScheduleWindowStartEpoch` (wall-clock epoch, not `millis()`),
      `persistedLastPresenceEpoch`, `persistedPresenceHeld`.
- [ ] Add `OCCUPANCY_PERSIST_MAX_AGE_MS` (or equivalent) to `config/config.h` per
      decision 3 — a persisted epoch older than this ceiling is never trusted for
      restoration, regardless of windowKey match.
- [ ] Write cadence (flash-wear-conscious, matching existing change-detection style):
  - [ ] Persist new window key + start epoch only when `justEnteredSchedule` fires
        (window key actually changes).
  - [ ] Persist presence epoch only on direct PIR/MLX evidence updates (never from a
        derived/held value — same rule as `lastPresenceDetectedMillis` today),
        throttled to at most once per ~30-60s during continuous occupancy.

### 2B. Boot-time restoration logic
- [ ] Add restoration step before the first `runMinuteControl()` call (after WiFi/NTP
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
- [ ] Update `AGENTS.md`/`CLAUDE.md` (per its own "update this document in the same
      change" rule):
  - [ ] New global state in structures/globals sections.
  - [ ] New config constants.
  - [ ] New `bufferedDuringOutage` decision-log field under Firebase Paths.
  - [ ] Updated Occupancy State Model section describing warm-recovery restoration
        and the NTP-invalid-at-boot fallback.
  - [ ] Updated Boot Flow section with the new restoration step.

---

## Testing (per CLAUDE.md's "Before Finishing" / Testing section)
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
