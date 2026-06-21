#pragma once
// -----------------------------------------------------------------------------
//  SECRETS TEMPLATE
//  1. Copy this file to "secrets.h" (same folder).
//  2. Fill in your real values below.
//  secrets.h is git-ignored, so your credentials never get committed.
// -----------------------------------------------------------------------------

// WiFi (2.4 GHz — the ESP32 does not support 5 GHz)
#define SECRET_WIFI_SSID "YOUR_WIFI_SSID"
#define SECRET_WIFI_PASS "YOUR_WIFI_PASSWORD"

// VRM Personal Access Token.
// Create one at: VRM Portal -> Preferences -> Integrations -> Access tokens.
#define SECRET_VRM_TOKEN "YOUR_VRM_PERSONAL_ACCESS_TOKEN"

// Installation id (idSite). Find it in the VRM portal URL (installation/<idSite>/...)
// or via:  GET https://vrmapi.victronenergy.com/v2/users/{idUser}/installations
#define SECRET_VRM_SITE  "123456"

// Label shown at the top-left of the screen.
#define SECRET_SITE_NAME "Home"
