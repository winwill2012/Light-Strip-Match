#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <FastLED.h>
#include <U8g2lib.h>

#include "game_config.h"
#include "wifi_portal.h"

// ===================== 引脚（ESP32-C3 Super Mini）=====================
static constexpr int PIN_WS2812 = 2;
static constexpr int PIN_I2C_SDA = 1;
static constexpr int PIN_I2C_SCL = 0;
static constexpr int PIN_BUZZER = 5;
static constexpr int PIN_BTN_RED = 20;
static constexpr int PIN_BTN_GREEN = 10;
static constexpr int PIN_BTN_BLUE = 9;
static constexpr int PIN_BTN_GAME = 8;

// ===================== 灯带布局 =====================
static constexpr uint16_t kMaxLeds = 64;
static constexpr uint16_t kQueueLen = 3;
static uint16_t g_numLeds = 30;

static inline uint16_t playLen() {
  return (g_numLeds > kQueueLen) ? (g_numLeds - kQueueLen) : 1;
}

static void syncLedCountFromConfig() {
  g_numLeds = gameConfigLedCount();
}

static constexpr uint32_t kTierUpIntervalMs = 30000;
static constexpr uint8_t kMaxDifficulty = 5;
static constexpr uint32_t kExplosionDurMs = 320;
static constexpr int kMaxExplosions = 4;

static constexpr uint8_t kPreviewBrightness = 110;
static constexpr uint8_t kPlayBrightness = 255;

static uint8_t g_queue[kQueueLen];
static uint8_t g_play[kMaxLeds - kQueueLen];
static CRGB g_leds[kMaxLeds];

static uint32_t g_gravityIntervalMs = 333;
static uint32_t g_spawnIntervalMs = 3000;
static uint32_t g_nextSpawnMs = 0;
static uint32_t g_nextGravityMs = 0;
static uint32_t g_nextBulletMs = 0;
static uint32_t g_nextSpeedupMs = 0;

static uint32_t g_score = 0;
static uint32_t g_highScore = 0;
static uint16_t g_speedLevel = 1;
static bool g_gameOver = false;
static bool g_showSplashTips = true;

// ===================== 飞行子弹 =====================
static constexpr int kMaxBullets = 6;
struct Bullet {
  int8_t row;
  uint8_t color;
  bool active;
};
static Bullet g_bullets[kMaxBullets];

struct Explosion {
  int8_t playRow;
  uint8_t color;
  uint32_t startMs;
  bool active;
};
static Explosion g_explosions[kMaxExplosions];

// ===================== OLED（SSD1306 128x64 I2C）=====================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C g_u8g2(U8G2_R0, U8X8_PIN_NONE);

enum class GameScreen : uint8_t { Splash, StartFlash, Playing };
static GameScreen g_screen = GameScreen::Splash;

static constexpr uint32_t kStartFlashStepMs = 130;
static constexpr int kStartFlashSteps = 6; // 红→绿→蓝 各闪 2 轮
static uint32_t g_startFlashT0 = 0;

// ===================== 按键去抖（低电平按下）=====================
static constexpr uint32_t kDebounceMs = 35;
static constexpr uint32_t kLongPressMs = 850;

struct DebouncedPin {
  int pin;
  bool down;
  bool prevRaw;
  bool changed;
  uint32_t rawChangeMs;
};

static DebouncedPin g_dRed{PIN_BTN_RED, false, false, false, 0};
static DebouncedPin g_dGreen{PIN_BTN_GREEN, false, false, false, 0};
static DebouncedPin g_dBlue{PIN_BTN_BLUE, false, false, false, 0};
static DebouncedPin g_dGame{PIN_BTN_GAME, false, false, false, 0};

static void initDebounced(DebouncedPin &p) {
  p.prevRaw = digitalRead(p.pin) == LOW;
  p.down = p.prevRaw;
  p.changed = false;
  p.rawChangeMs = millis();
}

static void buzzerScore() {
  tone(PIN_BUZZER, 1568, 70);
}

// 发射子弹（短促「嗖」感，三色略区分音高）
static void buzzerFire(const uint8_t color) {
  uint16_t f = 1050;
  switch (color) {
    case 1:
      f = 920;
      break;
    case 2:
      f = 1180;
      break;
    case 3:
      f = 1420;
      break;
    default:
      break;
  }
  tone(PIN_BUZZER, f, 28);
}

static void buzzerUi() {
  tone(PIN_BUZZER, 523, 55);
}

// 非阻塞蜂鸣：升级约 2s（紧张短句）、失败约 5s
struct BuzzerSeq {
  enum Kind : uint8_t { None = 0, Upgrade = 1, GameOver = 2 } kind = None;
  uint32_t t0 = 0;
  int16_t lastSeg = -1;
};
static BuzzerSeq g_buz;

static void buzzerSeqStop() {
  noTone(PIN_BUZZER);
  g_buz.kind = BuzzerSeq::None;
  g_buz.lastSeg = -1;
}

static void buzzerSeqStartUpgrade(const uint32_t now) {
  if (g_buz.kind == BuzzerSeq::GameOver) {
    return;
  }
  noTone(PIN_BUZZER);
  g_buz.kind = BuzzerSeq::Upgrade;
  g_buz.t0 = now;
  g_buz.lastSeg = -1;
}

static void buzzerSeqStartGameOver(const uint32_t now) {
  noTone(PIN_BUZZER);
  g_buz.kind = BuzzerSeq::GameOver;
  g_buz.t0 = now;
  g_buz.lastSeg = -1;
}

static void buzzerSeqTick(const uint32_t now) {
  if (g_buz.kind == BuzzerSeq::None) {
    return;
  }
  const uint32_t dt = now - g_buz.t0;
  if (g_buz.kind == BuzzerSeq::Upgrade) {
    if (dt >= 2000) {
      buzzerSeqStop();
      return;
    }
    // 紧张感：快速高低交替 + 随时间抬升的低音（类似警报）
    constexpr uint32_t kStepMs = 65;
    const int seg = static_cast<int>(dt / kStepMs);
    if (seg != g_buz.lastSeg) {
      g_buz.lastSeg = static_cast<int16_t>(seg);
      const bool hi = (seg & 1) != 0;
      const unsigned lowHz = 220 + static_cast<unsigned>(dt / 35);
      const unsigned highHz = 880 + static_cast<unsigned>((dt / 50) % 180);
      tone(PIN_BUZZER, hi ? highHz : lowHz, static_cast<unsigned>(kStepMs + 30));
    }
    return;
  }
  if (g_buz.kind == BuzzerSeq::GameOver) {
    if (dt >= 5000) {
      buzzerSeqStop();
      return;
    }
    constexpr uint32_t kStepMs = 200;
    const int seg = static_cast<int>(dt / kStepMs);
    if (seg != g_buz.lastSeg) {
      g_buz.lastSeg = static_cast<int16_t>(seg);
      const int hz = 400 - seg * 14;
      tone(PIN_BUZZER, static_cast<unsigned>(hz > 90 ? hz : 90), static_cast<unsigned>(kStepMs + 30));
    }
  }
}

static void tickDebounced(DebouncedPin &p, const uint32_t now) {
  const bool rawDown = digitalRead(p.pin) == LOW;
  p.changed = false;
  if (rawDown != p.prevRaw) {
    p.prevRaw = rawDown;
    p.rawChangeMs = now;
  }
  if (rawDown != p.down && (now - p.rawChangeMs) >= kDebounceMs) {
    p.down = rawDown;
    p.changed = true;
  }
}

static CRGB crgbFromId(uint8_t id, uint8_t scale) {
  CRGB out = CRGB::Black;
  switch (id) {
    case 1:
      out = CRGB::Red;
      break;
    case 2:
      out = CRGB::Green;
      break;
    case 3:
      out = CRGB::Blue;
      break;
    default:
      return out;
  }
  out.nscale8(scale);
  return out;
}

static CRGB cellLedColor(uint16_t pi) {
  if (pi < playLen() && g_play[pi] != 0) {
    return crgbFromId(g_play[pi], kPlayBrightness);
  }
  return CRGB::Black;
}

// 若前两颗同色，则不再生成同色（避免连续三颗同色）
static uint8_t randomColorNoTriple(const uint8_t prev1, const uint8_t prev2) {
  uint8_t c = static_cast<uint8_t>(random(1, 4));
  if (prev1 >= 1 && prev1 <= 3 && prev1 == prev2) {
    while (c == prev1) {
      c = static_cast<uint8_t>(random(1, 4));
    }
  }
  return c;
}

static void fillQueueRandom() {
  g_queue[0] = randomColorNoTriple(0, 0);
  g_queue[1] = randomColorNoTriple(g_queue[0], 0);
  g_queue[2] = randomColorNoTriple(g_queue[1], g_queue[0]);
}

// 即将落入 play[0] 时，避免与 play[1]、play[2] 纵向连成三同色
static void fixQueueHeadBeforeSpawn() {
  if (g_play[0] != 0 || g_queue[0] < 1) {
    return;
  }
  if (playLen() >= 3 && g_play[1] != 0 && g_play[1] == g_play[2] && g_play[2] == g_queue[0]) {
    uint8_t c = 0;
    do {
      c = randomColorNoTriple(g_play[2], g_play[1]);
    } while (c == g_queue[0]);
    g_queue[0] = c;
  }
}

static void clearBullets() {
  for (int i = 0; i < kMaxBullets; ++i) {
    g_bullets[i].active = false;
  }
}

static void clearExplosions() {
  for (int i = 0; i < kMaxExplosions; ++i) {
    g_explosions[i].active = false;
  }
}

static uint16_t playIndexToPhys(const int8_t playPi) {
  const uint16_t row = static_cast<uint16_t>(kQueueLen + static_cast<uint16_t>(playPi));
  return g_numLeds - 1 - row;
}

static void startExplosion(const int8_t playRow, const uint8_t color, const uint32_t now) {
  for (int i = 0; i < kMaxExplosions; ++i) {
    if (!g_explosions[i].active) {
      g_explosions[i].playRow = playRow;
      g_explosions[i].color = color;
      g_explosions[i].startMs = now;
      g_explosions[i].active = true;
      return;
    }
  }
  g_explosions[0].playRow = playRow;
  g_explosions[0].color = color;
  g_explosions[0].startMs = now;
  g_explosions[0].active = true;
}

static void drawExplosionOverlay(const Explosion &ex, const uint32_t now) {
  const uint32_t dt = now - ex.startMs;
  if (dt >= kExplosionDurMs) {
    return;
  }
  const uint8_t fade = static_cast<uint8_t>(255 - (dt * 255UL) / kExplosionDurMs);
  int spread = 1 + static_cast<int>(dt / 70);
  if (spread > 3) {
    spread = 3;
  }
  for (int d = -spread; d <= spread; ++d) {
    const int pi = static_cast<int>(ex.playRow) + d;
    if (pi < 0 || pi >= static_cast<int>(playLen())) {
      continue;
    }
    const uint16_t phys = playIndexToPhys(static_cast<int8_t>(pi));
    const int ad = d < 0 ? -d : d;
    uint8_t br = fade;
    if (ad > 0) {
      br = scale8(fade, static_cast<uint8_t>(240 - ad * 55));
    }
    CRGB c = crgbFromId(ex.color, br);
    if (ad == 0 && dt < 80) {
      const uint8_t flash = static_cast<uint8_t>(255 - (dt * 255UL) / 80);
      c = CRGB::White;
      c.nscale8(flash);
    }
    g_leds[phys] += c;
  }
}

static void applyExplosionsToLeds(const uint32_t now) {
  for (int i = 0; i < kMaxExplosions; ++i) {
    if (!g_explosions[i].active) {
      continue;
    }
    if (now - g_explosions[i].startMs >= kExplosionDurMs) {
      g_explosions[i].active = false;
      continue;
    }
    drawExplosionOverlay(g_explosions[i], now);
  }
}

static void applyDifficultyFromLevel() {
  g_spawnIntervalMs = gameConfigSpawnForLevel(g_speedLevel);
  g_gravityIntervalMs = gameConfigGravityForLevel(g_speedLevel);
}

static void tryTierUp(const uint32_t now) {
  if (g_gameOver || g_screen != GameScreen::Playing) {
    return;
  }
  if (now < g_nextSpeedupMs || g_speedLevel >= kMaxDifficulty) {
    return;
  }
  g_nextSpeedupMs += kTierUpIntervalMs;
  g_speedLevel++;
  applyDifficultyFromLevel();
  if (g_nextGravityMs > now + g_gravityIntervalMs) {
    g_nextGravityMs = now + g_gravityIntervalMs;
  }
  if (g_nextSpawnMs > now + g_spawnIntervalMs) {
    g_nextSpawnMs = now + g_spawnIntervalMs;
  }
  buzzerSeqStartUpgrade(now);
}

static void resetGameCore(const uint32_t now) {
  buzzerSeqStop();
  syncLedCountFromConfig();
  memset(g_play, 0, sizeof(g_play));
  fillQueueRandom();
  clearBullets();
  clearExplosions();
  g_speedLevel = 1;
  applyDifficultyFromLevel();
  g_score = 0;
  g_gameOver = false;
  g_nextSpawnMs = now + g_spawnIntervalMs;
  g_nextGravityMs = now + g_gravityIntervalMs;
  g_nextBulletMs = now + gameConfigBulletStepMs();
  g_nextSpeedupMs = now + kTierUpIntervalMs;
}

static void trySpawn();

static void beginPlayingTimers(const uint32_t now) {
  // 勿用 now：同帧内会先 trySpawn 再重力，play[0] 空出后会立刻二次生成
  g_nextGravityMs = now + g_gravityIntervalMs;
  g_nextSpawnMs = now + g_spawnIntervalMs;
  g_nextBulletMs = now + gameConfigBulletStepMs();
  g_nextSpeedupMs = now + kTierUpIntervalMs;
}

static void beginStartFlash(const uint32_t now) {
  resetGameCore(now);
  g_startFlashT0 = now;
  g_screen = GameScreen::StartFlash;
  g_showSplashTips = false;
  {
    Preferences pr;
    if (pr.begin("lsmatch", false)) {
      pr.putBool("tip", true);
      pr.end();
    }
  }
  buzzerUi();
}

static void tickStartFlash(const uint32_t now) {
  if (g_screen != GameScreen::StartFlash) {
    return;
  }
  const uint32_t elapsed = now - g_startFlashT0;
  if (elapsed < kStartFlashStepMs * static_cast<uint32_t>(kStartFlashSteps)) {
    return;
  }
  g_screen = GameScreen::Playing;
  beginPlayingTimers(now);
  trySpawn();
}

static void applyGravityOnce() {
  for (int i = static_cast<int>(playLen()) - 2; i >= 0; --i) {
    if (g_play[i] != 0 && g_play[i + 1] == 0) {
      g_play[i + 1] = g_play[i];
      g_play[i] = 0;
    }
  }
}

static void trySpawn() {
  if (g_play[0] != 0) {
    return;
  }
  fixQueueHeadBeforeSpawn();
  g_play[0] = g_queue[0];
  g_queue[0] = g_queue[1];
  g_queue[1] = g_queue[2];
  g_queue[2] = randomColorNoTriple(g_queue[1], g_queue[0]);
}

static bool cellHasFlyingBullet(int8_t row) {
  for (int i = 0; i < kMaxBullets; ++i) {
    if (g_bullets[i].active && g_bullets[i].row == row) {
      return true;
    }
  }
  return false;
}

static void triggerGameOver(const uint32_t now) {
  if (g_gameOver) {
    return;
  }
  g_gameOver = true;
  buzzerSeqStartGameOver(now);
}

// 同色：子弹与灯珠一起消失并得分；异色：立即失败
static void resolveBulletIntoGemCell(Bullet &bul, const int8_t cellRow, const uint32_t now) {
  if (cellRow < 0 || cellRow >= static_cast<int8_t>(playLen()) || g_play[cellRow] == 0) {
    return;
  }
  const uint8_t gcol = g_play[cellRow];
  const uint8_t bcol = bul.color;
  if (bcol == gcol) {
    g_play[cellRow] = 0;
    bul.active = false;
    startExplosion(cellRow, gcol, now);
    g_score += g_speedLevel;
    buzzerScore();
    if (g_score > g_highScore) {
      g_highScore = g_score;
      Preferences pr;
      if (pr.begin("lsmatch", false)) {
        pr.putULong("hi", g_highScore);
        pr.end();
      }
    }
    return;
  }
  bul.active = false;
  triggerGameOver(now);
}

// 每步上移一格（可见）；顶行无格则飞出
static void stepBullet(Bullet &bul, const int bi, const uint32_t now) {
  if (!bul.active) {
    return;
  }
  const int8_t r = bul.row;
  if (g_play[r] != 0) {
    resolveBulletIntoGemCell(bul, r, now);
    return;
  }
  if (r <= 0) {
    bul.active = false;
    return;
  }
  const int8_t nr = r - 1;
  for (int k = 0; k < kMaxBullets; ++k) {
    if (k != bi && g_bullets[k].active && g_bullets[k].row == nr) {
      return;
    }
  }
  if (g_play[nr] != 0) {
    resolveBulletIntoGemCell(bul, nr, now);
    return;
  }
  bul.row = nr;
}

static bool trySpawnBullet(uint8_t color) {
  const int8_t bottom = static_cast<int8_t>(playLen() - 1);
  if (cellHasFlyingBullet(bottom)) {
    return false;
  }
  for (int i = 0; i < kMaxBullets; ++i) {
    if (!g_bullets[i].active) {
      g_bullets[i].active = true;
      g_bullets[i].row = bottom;
      g_bullets[i].color = color;
      buzzerFire(color);
      return true;
    }
  }
  return false;
}

static void moveBulletsOnce(const uint32_t now) {
  int order[kMaxBullets];
  int n = 0;
  for (int i = 0; i < kMaxBullets; ++i) {
    if (g_bullets[i].active) {
      order[n++] = i;
    }
  }
  for (int a = 0; a < n; ++a) {
    for (int b = a + 1; b < n; ++b) {
      if (g_bullets[order[b]].row > g_bullets[order[a]].row) {
        const int t = order[a];
        order[a] = order[b];
        order[b] = t;
      }
    }
  }
  for (int j = 0; j < n; ++j) {
    const int bi = order[j];
    stepBullet(g_bullets[bi], bi, now);
  }
}

static void sceneToLeds(const uint32_t now) {
  if (g_screen == GameScreen::StartFlash) {
    const uint32_t elapsed = now - g_startFlashT0;
    const int step = static_cast<int>(elapsed / kStartFlashStepMs) % 3;
    CRGB c = CRGB::Black;
    switch (step) {
      case 0:
        c = CRGB::Red;
        break;
      case 1:
        c = CRGB::Green;
        break;
      default:
        c = CRGB::Blue;
        break;
    }
    fill_solid(g_leds, g_numLeds, c);
    return;
  }
  if (g_gameOver) {
    const bool on = ((millis() / 350) & 1U) != 0;
    fill_solid(g_leds, g_numLeds, on ? CRGB::Red : CRGB::Black);
    return;
  }
  for (uint16_t row = 0; row < g_numLeds; ++row) {
    const uint16_t phys = g_numLeds - 1 - row;
    if (row < kQueueLen) {
      const uint16_t qi = kQueueLen - 1 - row;
      g_leds[phys] = crgbFromId(g_queue[qi], kPreviewBrightness);
    } else {
      const uint16_t pi = row - kQueueLen;
      g_leds[phys] = cellLedColor(pi);
    }
  }
  for (int i = 0; i < kMaxBullets; ++i) {
    if (!g_bullets[i].active) {
      continue;
    }
    const int8_t br = g_bullets[i].row;
    if (br < 0 || br >= static_cast<int8_t>(playLen())) {
      continue;
    }
    const uint16_t row = static_cast<uint16_t>(kQueueLen + static_cast<uint16_t>(br));
    const uint16_t phys = g_numLeds - 1 - row;
    switch (g_bullets[i].color) {
      case 1:
        g_leds[phys] = CRGB::Red;
        break;
      case 2:
        g_leds[phys] = CRGB::Green;
        break;
      case 3:
        g_leds[phys] = CRGB::Blue;
        break;
      default:
        break;
    }
  }
  applyExplosionsToLeds(now);
  for (uint16_t i = g_numLeds; i < kMaxLeds; ++i) {
    g_leds[i] = CRGB::Black;
  }
}

static uint32_t g_gameDownSince = 0;
static bool g_gameWasDown = false;
static bool g_gameLongDone = false;

static void handleGameButton(const uint32_t now) {
  tickDebounced(g_dGame, now);

  if (g_dGame.down) {
    if (!g_gameWasDown) {
      g_gameDownSince = now;
      g_gameLongDone = false;
    } else if (!g_gameLongDone && (now - g_gameDownSince) >= kLongPressMs) {
      g_gameLongDone = true;
      resetGameCore(now);
      g_screen = GameScreen::Splash;
      buzzerUi();
    }
    g_gameWasDown = true;
    return;
  }

  if (g_gameWasDown) {
    if (!g_gameLongDone && (now - g_gameDownSince) < kLongPressMs) {
      if (g_gameOver) {
        // 失败态仅允许长按复位
      } else if (g_screen == GameScreen::Splash) {
        beginStartFlash(now);
      }
    }
    g_gameWasDown = false;
  }
}

static void handleFireButtons(const uint32_t now) {
  if (g_screen != GameScreen::Playing || g_gameOver) {
    return;
  }
  tickDebounced(g_dRed, now);
  tickDebounced(g_dGreen, now);
  tickDebounced(g_dBlue, now);
  if (g_dRed.changed && g_dRed.down) {
    trySpawnBullet(1);
  }
  if (g_dGreen.changed && g_dGreen.down) {
    trySpawnBullet(2);
  }
  if (g_dBlue.changed && g_dBlue.down) {
    trySpawnBullet(3);
  }
}

static uint32_t g_lastDrawMs = 0;

// 128×64 外边框路径长度（顺时针，与 drawFrame(0,0,128,64) 重合）
static constexpr int kBorderPathLen = 379;

static void borderPathIndexToXY(int idx, int &x, int &y) {
  idx %= kBorderPathLen;
  if (idx < 128) {
    x = idx;
    y = 0;
    return;
  }
  idx -= 128;
  if (idx < 62) {
    x = 127;
    y = 1 + idx;
    return;
  }
  idx -= 62;
  if (idx < 127) {
    x = 126 - idx;
    y = 63;
    return;
  }
  idx -= 127;
  x = 0;
  y = 62 - idx;
}

static void drawOledOuterFrameAndDot(const uint32_t now, const bool chaseDot) {
  g_u8g2.setDrawColor(1);
  g_u8g2.drawFrame(0, 0, 128, 64);
  if (chaseDot) {
    int bx = 0;
    int by = 0;
    borderPathIndexToXY(static_cast<int>((now / 28) % kBorderPathLen), bx, by);
    // 与 1px 白边框同色时普通绘制不可见；用 XOR 在边框上形成移动的「缺口」
    g_u8g2.setDrawColor(2);
    g_u8g2.drawDisc(bx, by, 2);
    g_u8g2.setDrawColor(1);
  }
}

static void drawUtf8Centered(const uint8_t *font, int baselineY, const char *utf8) {
  g_u8g2.setFont(font);
  const int w = g_u8g2.getUTF8Width(utf8);
  g_u8g2.drawUTF8((128 - w) / 2, baselineY, utf8);
}

static void drawSplashScreen(const uint32_t now) {
  drawOledOuterFrameAndDot(now, true);

  for (int i = 0; i < 8; ++i) {
    if (((now / 90 + i) & 3U) == 0U) {
      continue;
    }
    const int px = 6 + static_cast<int>((now / 35 + i * 29) % 116);
    const int py = 6 + static_cast<int>((now / 47 + i * 17) % 52);
    g_u8g2.drawPixel(px, py);
  }

  const char *title = "灯带消消乐";
  g_u8g2.setFont(u8g2_font_wqy16_t_gb2312a);
  const int titleW = g_u8g2.getUTF8Width(title);
  const int titleX = (128 - titleW) / 2;
  const int bob = static_cast<int>((now / 200) % 3) - 1;
  const int titleY = 21 + bob;
  g_u8g2.drawUTF8(titleX, titleY, title);
  const int underlineY = titleY + 5;
  g_u8g2.drawHLine(titleX, underlineY, titleW);

  const int scanW = 6;
  const int scanX = titleX + static_cast<int>((now / 22) % (titleW + scanW)) - scanW / 2;
  if (scanX >= titleX - 2 && scanX + scanW <= titleX + titleW + 2) {
    g_u8g2.setDrawColor(2);
    g_u8g2.drawBox(scanX, titleY - 15, scanW, 17);
    g_u8g2.setDrawColor(1);
  }

  constexpr int kBarX = 20;
  constexpr int kBarY = 34;
  constexpr int kBarW = 88;
  constexpr int kBarH = 5;
  g_u8g2.drawFrame(kBarX, kBarY, kBarW, kBarH);
  const int segW = 14;
  const int segX = kBarX + 1 + static_cast<int>((now / 45) % (kBarW - segW - 2));
  g_u8g2.drawBox(segX, kBarY + 1, segW, kBarH - 2);

  g_u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
  if (g_showSplashTips) {
    const bool blink = ((now / 480) & 1U) != 0;
    if (blink) {
      drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 48, "短按游戏键：开始");
    }
    drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 58, "长按游戏键：重新开始");
    if (wifiPortalApRunning()) {
      char apHint[24];
      snprintf(apHint, sizeof(apHint), "热点:%s", wifiPortalApSsid());
      drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 62, apHint);
    } else {
      drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 62, "WiFi未就绪");
    }
  } else {
    drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 50, "按游戏键开始");
  }
}

static void drawBottomFourKeyHints() {
  const int y0 = 48;
  const int h = 14;
  const int x0 = 1;
  const int widths[4] = {32, 32, 31, 31};
  int x = x0;
  g_u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
  for (int i = 0; i < 4; ++i) {
    const int w = widths[i];
    g_u8g2.drawFrame(x, y0, w, h);
    const char *label = nullptr;
    switch (i) {
      case 0:
        label = "红";
        break;
      case 1:
        label = "绿";
        break;
      case 2:
        label = "蓝";
        break;
      default:
        label = "\xe2\x86\x91";
        break;
    }
    const int tw = g_u8g2.getUTF8Width(label);
    const int tx = x + (w - tw) / 2;
    const int ty = y0 + 12;
    g_u8g2.drawUTF8(tx, ty, label);
    x += w;
  }
}

static void drawTopHudBar() {
  g_u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
  g_u8g2.drawUTF8(2, 12, "得分");
  const int wScoreLbl = g_u8g2.getUTF8Width("得分");

  char hist[20];
  snprintf(hist, sizeof(hist), "历史：%lu", static_cast<unsigned long>(g_highScore));
  const int histW = g_u8g2.getUTF8Width(hist);
  const int histX = 126 - histW;
  g_u8g2.drawUTF8(histX, 12, hist);

  if (!g_gameOver) {
    char lvTxt[12];
    snprintf(lvTxt, sizeof(lvTxt), "%u级", static_cast<unsigned>(g_speedLevel));
    const int lvW = g_u8g2.getUTF8Width(lvTxt);
    int lvX = (128 - lvW) / 2;
    const int minLvX = 2 + wScoreLbl + 4;
    const int maxLvX = histX - lvW - 4;
    if (lvX < minLvX) {
      lvX = minLvX;
    }
    if (lvX > maxLvX) {
      lvX = maxLvX;
    }
    if (lvX <= maxLvX) {
      g_u8g2.drawUTF8(lvX, 12, lvTxt);
    }
  }
}

static void drawBigScoreCentered(const char *asciiScore, const int baselineY) {
  g_u8g2.setFont(u8g2_font_logisoso26_tf);
  int w = g_u8g2.getStrWidth(asciiScore);
  if (w > 120) {
    g_u8g2.setFont(u8g2_font_logisoso22_tf);
    w = g_u8g2.getStrWidth(asciiScore);
  }
  if (w > 120) {
    g_u8g2.setFont(u8g2_font_logisoso20_tf);
    w = g_u8g2.getStrWidth(asciiScore);
  }
  g_u8g2.drawStr((128 - w) / 2, baselineY, asciiScore);
}

static void drawPlayingHudExtras(const uint32_t now) {
  // 细进度条：距下次升级（满级时条内呼吸）
  constexpr int kBarX = 2;
  constexpr int kBarY = 43;
  constexpr int kBarW = 124;
  constexpr int kBarH = 4;
  int innerFill = 0;
  if (g_speedLevel >= kMaxDifficulty) {
    innerFill = ((now / 240) & 1U) != 0 ? (kBarW - 4) : (kBarW - 24);
  } else if (g_nextSpeedupMs > now) {
    const uint32_t rem = g_nextSpeedupMs - now;
    innerFill = static_cast<int>((static_cast<uint64_t>(kBarW - 4) * (kTierUpIntervalMs - rem)) / kTierUpIntervalMs);
  } else {
    innerFill = kBarW - 4;
  }
  if (innerFill > kBarW - 4) {
    innerFill = kBarW - 4;
  }
  g_u8g2.setDrawColor(1);
  g_u8g2.drawFrame(kBarX, kBarY, kBarW, kBarH);
  if (innerFill > 0) {
    g_u8g2.drawBox(kBarX + 1, kBarY + 1, innerFill, kBarH - 2);
  }
}

static void drawOled(const uint32_t now) {
  const bool chaseBorderDot =
      ((g_screen == GameScreen::Playing || g_screen == GameScreen::StartFlash) && !g_gameOver);
  uint32_t minPeriod = 80u;
  if (g_screen == GameScreen::Splash) {
    minPeriod = 40u;
  } else if (g_gameOver) {
    minPeriod = 50u;
  } else if (chaseBorderDot) {
    minPeriod = 33u;
  }
  if ((now - g_lastDrawMs) < minPeriod) {
    return;
  }
  g_lastDrawMs = now;
  g_u8g2.clearBuffer();
  g_u8g2.setFontMode(1);

  if (g_screen == GameScreen::Splash) {
    g_u8g2.setDrawColor(1);
    drawSplashScreen(now);
    g_u8g2.sendBuffer();
    return;
  }

  // 失败态：整屏每秒闪烁（亮 0.5s / 灭 0.5s），不再显示「游戏结束」
  if (g_gameOver && ((now / 500) & 1U) != 0U) {
    g_u8g2.sendBuffer();
    return;
  }

  drawTopHudBar();

  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(g_score));

  if (g_gameOver) {
    drawBigScoreCentered(buf, 44);
    g_u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
    drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 60, "长按游戏键重开");
  } else if (g_screen == GameScreen::StartFlash) {
    g_u8g2.setFont(u8g2_font_wqy16_t_gb2312a);
    drawUtf8Centered(u8g2_font_wqy16_t_gb2312a, 38, "准备");
  } else {
    if (g_screen == GameScreen::Playing) {
      drawPlayingHudExtras(now);
    }
    drawBigScoreCentered(buf, 44);
    drawBottomFourKeyHints();
  }
  drawOledOuterFrameAndDot(now, chaseBorderDot);
  g_u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(400);

  pinMode(PIN_BTN_RED, INPUT_PULLUP);
  pinMode(PIN_BTN_GREEN, INPUT_PULLUP);
  pinMode(PIN_BTN_BLUE, INPUT_PULLUP);
  pinMode(PIN_BTN_GAME, INPUT_PULLUP);

  initDebounced(g_dRed);
  initDebounced(g_dGreen);
  initDebounced(g_dBlue);
  initDebounced(g_dGame);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  g_u8g2.begin();

  randomSeed(esp_random());

  gameConfigLoad();

  syncLedCountFromConfig();
  FastLED.addLeds<WS2812, PIN_WS2812, GRB>(g_leds, kMaxLeds);
  FastLED.setBrightness(40);

  const uint32_t now = millis();
  {
    Preferences pr;
    if (pr.begin("lsmatch", true)) {
      g_highScore = pr.getULong("hi", 0);
      g_showSplashTips = !pr.getBool("tip", false);
      pr.end();
    } else {
      g_highScore = 0;
      g_showSplashTips = true;
    }
  }
  memset(g_play, 0, sizeof(g_play));
  fillQueueRandom();
  clearBullets();
  clearExplosions();
  g_speedLevel = 1;
  applyDifficultyFromLevel();
  g_score = 0;
  g_nextSpawnMs = now + g_spawnIntervalMs;
  g_nextGravityMs = now + g_gravityIntervalMs;
  g_nextBulletMs = now + gameConfigBulletStepMs();
  g_nextSpeedupMs = now + kTierUpIntervalMs;
  g_screen = GameScreen::Splash;

  // WiFi 放在外设初始化之后；C3 Super Mini 对初始化顺序与发射功率敏感
  wifiPortalBegin();
  if (wifiPortalApRunning()) {
    fill_solid(g_leds, g_numLeds, CRGB::Green);
  } else {
    fill_solid(g_leds, g_numLeds, CRGB::Red);
  }
  FastLED.show();
  delay(300);
  FastLED.clear(true);
}

void loop() {
  const uint32_t now = millis();

  wifiPortalLoop();
  handleGameButton(now);
  tickStartFlash(now);
  handleFireButtons(now);

  if (g_screen == GameScreen::Playing && !g_gameOver) {
    tryTierUp(now);
    if (now >= g_nextBulletMs) {
      g_nextBulletMs = now + gameConfigBulletStepMs();
      moveBulletsOnce(now);
    }
    if (now >= g_nextGravityMs) {
      g_nextGravityMs = now + g_gravityIntervalMs;
      applyGravityOnce();
      if (g_play[playLen() - 1] != 0) {
        triggerGameOver(now);
      }
    }
    if (now >= g_nextSpawnMs) {
      g_nextSpawnMs = now + g_spawnIntervalMs;
      trySpawn();
    }
  }

  sceneToLeds(now);
  FastLED.show();
  drawOled(now);
  buzzerSeqTick(now);
}
