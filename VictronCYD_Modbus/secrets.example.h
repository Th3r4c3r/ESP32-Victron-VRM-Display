#pragma once
// -----------------------------------------------------------------------------
//  SECRETS TEMPLATE (Modbus / realtime variant)
//  1. Copy this file to "secrets.h" (same folder).
//  2. Fill in your values.
//  secrets.h is git-ignored.
// -----------------------------------------------------------------------------

// WiFi (2.4 GHz)
#define SECRET_WIFI_SSID "YOUR_WIFI_SSID"
#define SECRET_WIFI_PASS "YOUR_WIFI_PASSWORD"

// GX device (Cerbo GX / Venus OS) IP address on your LAN.
// Enable on the GX: Settings -> Services -> Modbus TCP = ON.
#define SECRET_GX_IP "192.168.1.50"

// Label shown at the top-left of the screen.
#define SECRET_SITE_NAME "Home"
