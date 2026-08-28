// ac_Control
#ifndef AC_CONTROL_H
#define AC_CONTROL_H

#include "../core/structures.h"
#include "utility_functions.h"
#include "persistence_functions.h"
#include "firebase_functions.h"
#include "energy_functions.h"
#include "logger_functions.h"

bool applyAcState(bool targetPower, int targetTemp, const String& source);
bool applyAcState(bool targetPower, int targetTemp, const String& source, bool forceIr);
void applyControlJson(JsonVariant data);


static void sendIrBurst() {
  for (uint8_t i = 0; i < IR_SEND_REPEAT_COUNT; i++) {
    coolixAc.send();
    if (i < IR_SEND_REPEAT_COUNT - 1) {
      delay(IR_SEND_REPEAT_DELAY_MS);
    }
  }
}

static void sendOffIrBurst() {
  for (uint8_t i = 0; i < IR_SEND_REPEAT_COUNT; i++) {
    coolixAc.off();
    coolixAc.send();
    if (i < IR_SEND_REPEAT_COUNT - 1) {
      delay(IR_SEND_REPEAT_DELAY_MS);
    }
  }
}

bool applyAcState(bool targetPower, int targetTemp, const String& source) {
  return applyAcState(targetPower, targetTemp, source, false);
}

bool applyAcState(bool targetPower, int targetTemp, const String& source, bool forceIr) {
  targetTemp = normalizeACTemp((float)targetTemp);
  bool previousPower = acPowerState;
  int previousTemp = acTempState;
  String previousSource = acSourceState;
  bool sameState = targetPower == acPowerState && (!targetPower || targetTemp == acTempState);
  bool shouldSendIr = forceIr || !acIrStateTrusted || !sameState;

  if (sameState && !shouldSendIr) {
    if (acSourceState != source) {
      acSourceState = source;
      syncAcStateToFirebase();
      logAcStateChange(previousPower, previousTemp, previousSource,
                       acPowerState, acTempState, source, false,
                       "source_changed");
    }
    return false;
  }

  if (targetPower) {
    coolixAc.on();
    coolixAc.setFan(kCoolixFanAuto); // Explicitly set a valid fan state
    coolixAc.setMode(kCoolixCool);   // Explicitly set mode
    coolixAc.setTemp(targetTemp);
    sendIrBurst();
    acTempState = targetTemp;
    Serial.printf("IR: AC ON %dC (%s) [x%d]\n", targetTemp, source.c_str(), IR_SEND_REPEAT_COUNT);
  } else {
    sendOffIrBurst();
    Serial.printf("IR: AC OFF (%s) [x%d]\n", source.c_str(), IR_SEND_REPEAT_COUNT);
  }

  acIrStateTrusted = true;
  acPowerState  = targetPower;
  acSourceState = source;
  syncAcStateToFirebase();
  handleEnergyPowerTransition(previousPower, acPowerState, source);
  logAcStateChange(previousPower, previousTemp, previousSource,
                   acPowerState, acTempState, source, shouldSendIr,
                   "applied");
  return true;
}

void applyControlJson(JsonVariant data) {
  if (data.isNull()) return;

  if (!data["aiAutoApply"].isNull()) {
    bool nextAiAutoApply = data["aiAutoApply"].as<bool>();
    if (aiAutoApplyEnabled != nextAiAutoApply) {
      aiAutoApplyEnabled = nextAiAutoApply;
      logDecisionEvent("ai_toggle_changed", "control", acPowerState, acTempState,
                       -1, aiAutoApplyEnabled, false, "control_json");
    } else {
      aiAutoApplyEnabled = nextAiAutoApply;
    }
  }

  bool hasOverride = !data["overrideActive"].isNull() || !data["active"].isNull();
  bool hasPower    = !data["power"].isNull();
  bool hasTemp     = !data["temp"].isNull();

  if (hasOverride) {
    if (!data["overrideActive"].isNull()) {
      manualOverrideActive = data["overrideActive"].as<bool>();
    } else {
      manualOverrideActive = data["active"].as<bool>();
    }
  }

  if (hasPower) manualOverridePower = data["power"].as<bool>();
  if (hasTemp) {
    manualOverrideTemp = normalizeACTemp((float)data["temp"].as<int>());
    manualOverrideTargetTemp = manualOverrideTemp;
  }

  if (manualOverrideActive || hasPower || hasTemp) {
    manualOverrideActive = true;
    if (!hasPower) manualOverridePower = acPowerState || hasTemp;
    if (!hasTemp) {
      manualOverrideTemp = acTempState;
      manualOverrideTargetTemp = acTempState;
    }
    applyAcState(manualOverridePower, manualOverrideTargetTemp, "manual");
  }
  persistManualOverrideState();
}

#endif
