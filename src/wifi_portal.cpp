#include "wifi_portal.h"

#include <WebServer.h>
#include <WiFi.h>

#include <esp_wifi.h>
#include <nvs_flash.h>

#include "config_page.h"
#include "game_config.h"

static WebServer g_srv(80);

// 纯 ASCII 热点名，密码至少 8 位
static const char kApSsid[] = "LSMatch";
static constexpr const char *kApPass = "12345678";

static bool g_webStarted = false;
static bool g_apRunning = false;
static uint32_t g_lastApCheckMs = 0;
static uint8_t g_apChannelInUse = 6;

static uint32_t parseJsonUInt(const String &body, const char *key, const uint32_t defVal) {
  const String pat = String("\"") + key + "\":";
  const int pos = body.indexOf(pat);
  if (pos < 0) {
    return defVal;
  }
  return static_cast<uint32_t>(body.substring(pos + pat.length()).toInt());
}

static String buildConfigJson() {
  const GameConfig &c = gameConfigGet();
  String j = "{\"ledCount\":";
  j += c.ledCount;
  j += ",\"bulletMs\":";
  j += c.bulletStepMs;
  for (uint8_t i = 0; i < kGameConfigLevels; ++i) {
    char ks[16];
    char kg[16];
    snprintf(ks, sizeof(ks), ",\"spawn%u\":", static_cast<unsigned>(i + 1));
    snprintf(kg, sizeof(kg), ",\"grav%u\":", static_cast<unsigned>(i + 1));
    j += ks;
    j += c.spawnMs[i];
    j += kg;
    j += c.gravityMs[i];
  }
  j += "}";
  return j;
}

static bool applyJsonBody(const String &body) {
  GameConfig cfg = gameConfigGet();
  cfg.ledCount = static_cast<uint16_t>(parseJsonUInt(body, "ledCount", cfg.ledCount));
  cfg.bulletStepMs = parseJsonUInt(body, "bulletMs", cfg.bulletStepMs);
  for (uint8_t i = 0; i < kGameConfigLevels; ++i) {
    char ks[8];
    char kg[8];
    snprintf(ks, sizeof(ks), "spawn%u", static_cast<unsigned>(i + 1));
    snprintf(kg, sizeof(kg), "grav%u", static_cast<unsigned>(i + 1));
    cfg.spawnMs[i] = parseJsonUInt(body, ks, cfg.spawnMs[i]);
    cfg.gravityMs[i] = parseJsonUInt(body, kg, cfg.gravityMs[i]);
  }
  return gameConfigUpdate(cfg);
}

static void handleRoot() {
  g_srv.send_P(200, "text/html; charset=utf-8", CONFIG_PAGE);
}

static void handleGetConfig() {
  g_srv.send(200, "application/json", buildConfigJson());
}

static void handlePostConfig() {
  const bool ok = applyJsonBody(g_srv.arg("plain"));
  g_srv.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleReset() {
  gameConfigResetDefaults();
  gameConfigSave();
  String j = "{\"ok\":true,";
  j += buildConfigJson().substring(1);
  g_srv.send(200, "application/json", j);
}

static bool ensureNvsFlash() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    if (nvs_flash_erase() != ESP_OK) {
      return false;
    }
    err = nvs_flash_init();
  }
  return err == ESP_OK;
}

static void logApDiagnostics(const char *tag) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);

  wifi_config_t conf = {};
  esp_wifi_get_config(WIFI_IF_AP, &conf);

  Serial.print(F("[WiFi] "));
  Serial.println(tag);
  Serial.print(F("  mode="));
  Serial.println(static_cast<int>(mode));
  Serial.print(F("  SSID="));
  Serial.println(reinterpret_cast<const char *>(conf.ap.ssid));
  Serial.print(F("  ch="));
  Serial.println(conf.ap.channel ? conf.ap.channel : g_apChannelInUse);
  Serial.print(F("  hidden="));
  Serial.println(conf.ap.ssid_hidden);
  Serial.print(F("  auth="));
  Serial.println(conf.ap.authmode);
  Serial.print(F("  IP="));
  Serial.println(WiFi.softAPIP());
  Serial.print(F("  TX dBm*0.25="));
  Serial.println(WiFi.getTxPower());
}

// ESP32-C3 Super Mini：过高发射功率会导致 softAP 有 IP 但手机扫不到热点
static void applySuperMiniTxPower() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);
}

static bool startAccessPoint(const uint8_t channel, const bool openNetwork) {
  WiFi.persistent(false);

  // 勿调用 WiFi.disconnect(true)/softAPdisconnect(true)：会使 C3 射频停发 beacon 但 softAPIP 仍正常
  WiFi.mode(WIFI_OFF);
  delay(300);
  WiFi.mode(WIFI_AP);
  delay(100);

  if (!WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0))) {
    Serial.println(F("[WiFi] softAPConfig failed"));
  }

  const bool ok = openNetwork ? WiFi.softAP(kApSsid, nullptr, channel, 0, 8)
                              : WiFi.softAP(kApSsid, kApPass, channel, 0, 8);
  if (!ok) {
    Serial.println(F("[WiFi] softAP() returned false"));
    g_apRunning = false;
    return false;
  }

  delay(400);
  applySuperMiniTxPower();
  delay(200);

  g_apChannelInUse = channel;
  const IPAddress ip = WiFi.softAPIP();
  g_apRunning = (ip != IPAddress(0, 0, 0, 0));
  return g_apRunning;
}

static void tryStartAp() {
  const uint8_t channels[] = {6, 1, 11};
  for (const uint8_t ch : channels) {
    if (startAccessPoint(ch, false)) {
      logApDiagnostics("AP OK (WPA2)");
      return;
    }
  }

  Serial.println(F("[WiFi] WPA2 failed, try OPEN ch6..."));
  if (startAccessPoint(6, true)) {
    logApDiagnostics("AP OK (OPEN)");
    return;
  }

  Serial.println(F("[WiFi] AP FAILED"));
  g_apRunning = false;
}

bool wifiPortalApRunning() {
  return g_apRunning && (WiFi.softAPIP() != IPAddress(0, 0, 0, 0));
}

const char *wifiPortalApSsid() {
  return kApSsid;
}

void wifiPortalBegin() {
  ensureNvsFlash();
  tryStartAp();

  if (!g_webStarted) {
    g_srv.on("/", HTTP_GET, handleRoot);
    g_srv.on("/api/config", HTTP_GET, handleGetConfig);
    g_srv.on("/api/config", HTTP_POST, handlePostConfig);
    g_srv.on("/api/reset", HTTP_POST, handleReset);
    g_srv.begin();
    g_webStarted = true;
  }
}

void wifiPortalLoop() {
  g_srv.handleClient();

  const uint32_t now = millis();
  if (now - g_lastApCheckMs < 60000) {
    return;
  }
  g_lastApCheckMs = now;

  if (wifiPortalApRunning()) {
    return;
  }

  Serial.println(F("[WiFi] AP lost, restart..."));
  tryStartAp();
}
