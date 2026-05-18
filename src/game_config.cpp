#include "game_config.h"

#include <Preferences.h>

static GameConfig g_cfg;
static bool g_cfgLoaded = false;

static const uint32_t kDefaultSpawn[kGameConfigLevels] = {2000, 1800, 1500, 1200, 1000};
static const uint32_t kDefaultGravity[kGameConfigLevels] = {300, 275, 250, 200, 175};
static constexpr uint32_t kDefaultBullet = 15;
static constexpr uint16_t kDefaultLedCount = 30;

static uint32_t clampU32(const uint32_t v, const uint32_t lo, const uint32_t hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

void gameConfigResetDefaults() {
  for (uint8_t i = 0; i < kGameConfigLevels; ++i) {
    g_cfg.spawnMs[i] = kDefaultSpawn[i];
    g_cfg.gravityMs[i] = kDefaultGravity[i];
  }
  g_cfg.bulletStepMs = kDefaultBullet;
  g_cfg.ledCount = kDefaultLedCount;
}

static void sanitize(GameConfig &cfg) {
  for (uint8_t i = 0; i < kGameConfigLevels; ++i) {
    cfg.spawnMs[i] = clampU32(cfg.spawnMs[i], 300, 15000);
    cfg.gravityMs[i] = clampU32(cfg.gravityMs[i], 50, 2000);
  }
  cfg.bulletStepMs = clampU32(cfg.bulletStepMs, 20, 500);
  if (cfg.ledCount < kGameConfigLedMin) {
    cfg.ledCount = kGameConfigLedMin;
  }
  if (cfg.ledCount > kGameConfigLedMax) {
    cfg.ledCount = kGameConfigLedMax;
  }
}

void gameConfigLoad() {
  gameConfigResetDefaults();
  Preferences pr;
  if (!pr.begin("lscfg", true)) {
    g_cfgLoaded = true;
    return;
  }
  if (!pr.getBool("ok", false)) {
    pr.end();
    g_cfgLoaded = true;
    return;
  }
  for (uint8_t i = 0; i < kGameConfigLevels; ++i) {
    char keySp[8];
    char keyGr[8];
    snprintf(keySp, sizeof(keySp), "sp%u", static_cast<unsigned>(i));
    snprintf(keyGr, sizeof(keyGr), "gr%u", static_cast<unsigned>(i));
    g_cfg.spawnMs[i] = pr.getULong(keySp, g_cfg.spawnMs[i]);
    g_cfg.gravityMs[i] = pr.getULong(keyGr, g_cfg.gravityMs[i]);
  }
  g_cfg.bulletStepMs = pr.getULong("bullet", g_cfg.bulletStepMs);
  g_cfg.ledCount = static_cast<uint16_t>(pr.getUInt("leds", g_cfg.ledCount));
  pr.end();
  sanitize(g_cfg);
  g_cfgLoaded = true;
}

void gameConfigSave() {
  sanitize(g_cfg);
  Preferences pr;
  if (!pr.begin("lscfg", false)) {
    return;
  }
  pr.putBool("ok", true);
  for (uint8_t i = 0; i < kGameConfigLevels; ++i) {
    char keySp[8];
    char keyGr[8];
    snprintf(keySp, sizeof(keySp), "sp%u", static_cast<unsigned>(i));
    snprintf(keyGr, sizeof(keyGr), "gr%u", static_cast<unsigned>(i));
    pr.putULong(keySp, g_cfg.spawnMs[i]);
    pr.putULong(keyGr, g_cfg.gravityMs[i]);
  }
  pr.putULong("bullet", g_cfg.bulletStepMs);
  pr.putUInt("leds", g_cfg.ledCount);
  pr.end();
}

const GameConfig &gameConfigGet() {
  if (!g_cfgLoaded) {
    gameConfigLoad();
  }
  return g_cfg;
}

bool gameConfigUpdate(const GameConfig &cfg) {
  g_cfg = cfg;
  sanitize(g_cfg);
  gameConfigSave();
  return true;
}

uint32_t gameConfigSpawnForLevel(const uint8_t level1to5) {
  const GameConfig &c = gameConfigGet();
  const uint8_t idx = (level1to5 < 1) ? 0 : (level1to5 > kGameConfigLevels ? kGameConfigLevels - 1 : level1to5 - 1);
  return c.spawnMs[idx];
}

uint32_t gameConfigGravityForLevel(const uint8_t level1to5) {
  const GameConfig &c = gameConfigGet();
  const uint8_t idx = (level1to5 < 1) ? 0 : (level1to5 > kGameConfigLevels ? kGameConfigLevels - 1 : level1to5 - 1);
  return c.gravityMs[idx];
}

uint32_t gameConfigBulletStepMs() {
  return gameConfigGet().bulletStepMs;
}

uint16_t gameConfigLedCount() {
  return gameConfigGet().ledCount;
}
