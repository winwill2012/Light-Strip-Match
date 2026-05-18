#pragma once

#include <Arduino.h>

static constexpr uint8_t kGameConfigLevels = 5;
static constexpr uint16_t kGameConfigLedMin = 8;
static constexpr uint16_t kGameConfigLedMax = 64;

struct GameConfig {
  uint32_t spawnMs[kGameConfigLevels];
  uint32_t gravityMs[kGameConfigLevels];
  uint32_t bulletStepMs;
  uint16_t ledCount;
};

void gameConfigLoad();
void gameConfigSave();
const GameConfig &gameConfigGet();
bool gameConfigUpdate(const GameConfig &cfg);
void gameConfigResetDefaults();

uint32_t gameConfigSpawnForLevel(uint8_t level1to5);
uint32_t gameConfigGravityForLevel(uint8_t level1to5);
uint32_t gameConfigBulletStepMs();
uint16_t gameConfigLedCount();
