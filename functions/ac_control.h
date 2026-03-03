#ifndef AC_CONTROL_H
#define AC_CONTROL_H

#include "../core/structures.h"
#include "utility_functions.h"
#include "firebase_functions.h"

bool applyAcState(bool targetPower, int targetTemp, const String& source);
void applyControlJson(JsonVariant data);

// Implementation
bool applyAcState(bool targetPower, int targetTemp, const String& source) {
  targetTemp = normalizeACTemp((float)targetTemp);

  if (targetPower == acPowerState && (!targetPower || targetTemp == acTempState)) {
    if (acSourceState != source) {
      acSourceState = source;
      syncAcStateToFirebase();
    }
    return false;
  }

  if (targetPower) {
    coolixAc.on();
    coolixAc.setMode(kCoolixCool);
    coolixAc.setTemp(targetTemp);
    coolixAc.send();
    acTempState = targetTemp;
    Serial.printf("IR: AC ON %dC (%s)\n", targetTemp, source.c_str());
  } else {
    coolixAc.off();
    coolixAc.send();
    Serial.printf("IR: AC OFF (%s)\n", source.c_str());
  }

  acPowerState = targetPower;
  acSourceState = source;
  syncAcStateToFirebase();
  return true;
}

void applyControlJson(JsonVariant data) {
  if (data.isNull()) return;

  bool hasOverride = !data["overrideActive"].isNull() || !data["active"].isNull();
  bool hasPower = !data["power"].isNull();
  bool hasTemp = !data["temp"].isNull();

  if (hasOverride) {
    if (!data["overrideActive"].isNull()) {
      manualOverrideActive = data["overrideActive"].as<bool>();
    } else {
      manualOverrideActive = data["active"].as<bool>();
    }
  }

  if (hasPower) manualOverridePower = data["power"].as<bool>();
  if (hasTemp) manualOverrideTemp = normalizeACTemp((float)data["temp"].as<int>());

  if (manualOverrideActive || hasPower || hasTemp) {
    manualOverrideActive = true;
    if (!hasPower) manualOverridePower = acPowerState || hasTemp;
    if (!hasTemp) manualOverrideTemp = acTempState;
    applyAcState(manualOverridePower, manualOverrideTemp, "manual");
  }
}

#endif
