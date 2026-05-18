// wifi_functions.h
#ifndef WIFI_FUNCTIONS_H
#define WIFI_FUNCTIONS_H

#include "../core/structures.h"
#include <WiFiManager.h>
#include <string.h>

#ifndef WIFI_PORTAL_PASSWORD
#error "Define WIFI_PORTAL_PASSWORD in config/secrets.h"
#endif

static WiFiManager wm;

static unsigned long wifiResetPressedSince = 0;
static bool wifiResetHandled = false;
static uint8_t wifiReconnectFailureCount = 0;

static const char* wifiPortalPassword() {
  const size_t len = strlen(WIFI_PORTAL_PASSWORD);
  return (len >= 8 && len <= 63) ? WIFI_PORTAL_PASSWORD : nullptr;
}

static String makeWiFiHostname() {
  String hostname = "OcuTemp-";
  hostname += String(DEVICE_ID);
  hostname.replace('_', '-');
  hostname.replace(' ', '-');
  hostname.replace('.', '-');

  if (hostname.length() > 31) {
    hostname = hostname.substring(0, 31);
  }

  return hostname;
}

static void configureWiFiManager() {
  wm.setDebugOutput(WIFI_MANAGER_DEBUG_OUTPUT);
  wm.setHostname(makeWiFiHostname());
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_SEC);
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SEC);
  wm.setCaptivePortalEnable(true);
  wm.setMinimumSignalQuality(WIFI_MIN_SIGNAL_QUALITY);
  wm.setRemoveDuplicateAPs(true);
  wm.setShowInfoErase(false);
  wm.setShowInfoUpdate(false);
}

void resetWiFiReconnectFailures() {
  wifiReconnectFailureCount = 0;
}

void resetWiFiSettingsAndRestart() {
  Serial.println("WiFi: clearing saved credentials and restarting...");
  wm.resetSettings();
  delay(500);
  ESP.restart();
}

bool startWiFiConfigPortal() {
  configureWiFiManager();
  WiFi.mode(WIFI_STA);

  Serial.println(String("WiFi: starting captive portal AP ") + WIFI_PORTAL_SSID);
  bool connected = wm.startConfigPortal(WIFI_PORTAL_SSID, wifiPortalPassword());

  if (connected) {
    resetWiFiReconnectFailures();
    Serial.println("WiFi: connected from captive portal.");
    Serial.println("WiFi IP: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("WiFi: captive portal closed without a connection.");
  return false;
}

void restartIntoWiFiProvisioning(const String& reason) {
  Serial.println(String("WiFi: ") + reason + ", restarting into provisioning flow...");
  delay(500);
  ESP.restart();
}

void noteWiFiReconnectFailure() {
  if (WiFi.status() == WL_CONNECTED) {
    resetWiFiReconnectFailures();
    return;
  }

  if (wifiReconnectFailureCount < 255) {
    wifiReconnectFailureCount++;
  }

  Serial.printf("WiFi: reconnect attempt %u/%u\n",
                wifiReconnectFailureCount,
                WIFI_MAX_RECONNECT_FAILURES);

  if (wifiReconnectFailureCount >= WIFI_MAX_RECONNECT_FAILURES) {
    restartIntoWiFiProvisioning("reconnect limit reached");
  }
}

void setupWiFiProvisioning() {
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  WiFi.mode(WIFI_STA);
  configureWiFiManager();

  Serial.println("WiFi: connecting with saved credentials...");
  bool connected = wm.autoConnect(WIFI_PORTAL_SSID, wifiPortalPassword());

  if (!connected) {
    restartIntoWiFiProvisioning("initial connection or captive portal timed out");
  }

  resetWiFiReconnectFailures();
  Serial.println("WiFi: connected.");
  Serial.println("WiFi IP: " + WiFi.localIP().toString());
}

void serviceWiFiProvisioning() {
  const bool resetPressed = digitalRead(WIFI_RESET_PIN) == LOW;

  if (!resetPressed) {
    wifiResetPressedSince = 0;
    wifiResetHandled = false;
    return;
  }

  if (wifiResetPressedSince == 0) {
    wifiResetPressedSince = millis();
    Serial.println("WiFi: reset button pressed, hold to clear credentials...");
    return;
  }

  if (!wifiResetHandled &&
      (millis() - wifiResetPressedSince) >= WIFI_RESET_HOLD_MS) {
    wifiResetHandled = true;
    resetWiFiSettingsAndRestart();
  }
}

#endif
