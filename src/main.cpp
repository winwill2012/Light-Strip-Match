#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <FastLED.h>
#include <U8g2lib.h>

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
static constexpr uint16_t kNumLeds = 30;
static constexpr uint16_t kQueueLen = 3;
static constexpr uint16_t kPlayLen = kNumLeds - kQueueLen;

static constexpr uint32_t kTierUpIntervalMs = 30000;
static constexpr uint8_t kMaxDifficulty = 5;

static constexpr uint8_t kPreviewBrightness = 110;
static constexpr uint8_t kPlayBrightness = 255;

static uint8_t g_queue[kQueueLen];
static uint8_t g_play[kPlayLen];
static CRGB g_leds[kNumLeds];

static uint32_t g_gravityIntervalMs = 333;
static uint32_t g_spawnIntervalMs = 3000;
static uint32_t g_nextSpawnMs = 0;
static uint32_t g_nextGravityMs = 0;
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

// ===================== OLED（SSD1306 128x64 I2C）=====================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C g_u8g2(U8G2_R0, U8X8_PIN_NONE);

enum class GameScreen : uint8_t { Splash, Playing };
static GameScreen g_screen = GameScreen::Splash;

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
  if (g_play[pi] != 0) {
    return crgbFromId(g_play[pi], kPlayBrightness);
  }
  return CRGB::Black;
}

static void fillQueueRandom() {
  for (uint16_t i = 0; i < kQueueLen; ++i) {
    g_queue[i] = static_cast<uint8_t>(random(1, 4));
  }
}

static void clearBullets() {
  for (int i = 0; i < kMaxBullets; ++i) {
    g_bullets[i].active = false;
  }
}

// 等级 1..5：生成间隔、重力间隔（每秒 N 格 ≈ 1000/N ms）
static constexpr uint32_t kSpawnIntervalByLevel[kMaxDifficulty] = {3000, 2500, 2000, 1500, 1000};
static constexpr uint32_t kGravityIntervalByLevel[kMaxDifficulty] = {333, 333, 333, 250, 250};

static void applyDifficultyFromLevel() {
  const uint8_t idx =
      (g_speedLevel < 1) ? 0
                         : (g_speedLevel > kMaxDifficulty ? static_cast<uint8_t>(kMaxDifficulty - 1)
                                                          : static_cast<uint8_t>(g_speedLevel - 1));
  g_spawnIntervalMs = kSpawnIntervalByLevel[idx];
  g_gravityIntervalMs = kGravityIntervalByLevel[idx];
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
  memset(g_play, 0, sizeof(g_play));
  fillQueueRandom();
  clearBullets();
  g_speedLevel = 1;
  applyDifficultyFromLevel();
  g_score = 0;
  g_gameOver = false;
  g_nextSpawnMs = now + g_spawnIntervalMs;
  g_nextGravityMs = now + g_gravityIntervalMs;
  g_nextSpeedupMs = now + kTierUpIntervalMs;
}

static void applyGravityOnce() {
  for (int i = static_cast<int>(kPlayLen) - 2; i >= 0; --i) {
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
  g_play[0] = g_queue[0];
  g_queue[0] = g_queue[1];
  g_queue[1] = g_queue[2];
  g_queue[2] = static_cast<uint8_t>(random(1, 4));
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
  if (cellRow < 0 || cellRow >= static_cast<int8_t>(kPlayLen) || g_play[cellRow] == 0) {
    return;
  }
  const uint8_t gcol = g_play[cellRow];
  const uint8_t bcol = bul.color;
  if (bcol == gcol) {
    g_play[cellRow] = 0;
    bul.active = false;
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
  const int8_t bottom = static_cast<int8_t>(kPlayLen - 1);
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

static void sceneToLeds() {
  if (g_gameOver) {
    const bool on = ((millis() / 350) & 1U) != 0;
    fill_solid(g_leds, kNumLeds, on ? CRGB::Red : CRGB::Black);
    return;
  }
  for (uint16_t row = 0; row < kNumLeds; ++row) {
    const uint16_t phys = kNumLeds - 1 - row;
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
    if (br < 0 || br >= static_cast<int8_t>(kPlayLen)) {
      continue;
    }
    const uint16_t row = static_cast<uint16_t>(kQueueLen + static_cast<uint16_t>(br));
    const uint16_t phys = kNumLeds - 1 - row;
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
        g_screen = GameScreen::Playing;
        resetGameCore(now);
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
  // 顶区下方流星点（不与大号得分区重叠）
  for (int s = 0; s < 5; ++s) {
    const int px = 3 + static_cast<int>((now / (48 + s * 11) + s * 23) % 122);
    const int py = 27;
    g_u8g2.drawPixel(px, py);
  }
}

static void drawOled(const uint32_t now) {
  const bool chaseBorderDot = (g_screen == GameScreen::Playing && !g_gameOver);
  const uint32_t minPeriod = chaseBorderDot ? 33u : 80u;
  if ((now - g_lastDrawMs) < minPeriod) {
    return;
  }
  g_lastDrawMs = now;
  g_u8g2.clearBuffer();
  g_u8g2.setFontMode(1);

  if (g_screen == GameScreen::Splash) {
    g_u8g2.setDrawColor(1);
    drawOledOuterFrameAndDot(now, false);
    const char *title = "灯带消消乐";
    g_u8g2.setFont(u8g2_font_wqy16_t_gb2312a);
    const int titleW = g_u8g2.getUTF8Width(title);
    const int titleX = (128 - titleW) / 2;
    constexpr int kTitleBaseline = 24;
    g_u8g2.drawUTF8(titleX, kTitleBaseline, title);
    const int underlineY = kTitleBaseline + 5;
    g_u8g2.drawHLine(titleX, underlineY, titleW);
    g_u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
    if (g_showSplashTips) {
      drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 38, "短按游戏键：开始");
      drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 50, "长按游戏键：重新开始");
      drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 62, "红绿蓝键：发射子弹");
    } else {
      drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 46, "按游戏键开始");
    }
    g_u8g2.sendBuffer();
    return;
  }

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

  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(g_score));

  if (g_gameOver) {
    // 结束态：得分上移略缩小，提示放在中下部；不画底部四格以免与说明抢空间
    g_u8g2.setFont(u8g2_font_logisoso24_tf);
    const int swGo = g_u8g2.getStrWidth(buf);
    g_u8g2.drawStr((128 - swGo) / 2, 32, buf);
    g_u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
    drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 48, "游戏结束");
    drawUtf8Centered(u8g2_font_wqy12_t_gb2312a, 60, "长按游戏键重开");
  } else {
    if (g_screen == GameScreen::Playing) {
      drawPlayingHudExtras(now);
    }
    g_u8g2.setFont(u8g2_font_logisoso26_tf);
    const int sw = g_u8g2.getStrWidth(buf);
    g_u8g2.drawStr((128 - sw) / 2, 40, buf);
    drawBottomFourKeyHints();
  }
  drawOledOuterFrameAndDot(now, chaseBorderDot);
  g_u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(150);

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

  FastLED.addLeds<WS2812, PIN_WS2812, GRB>(g_leds, kNumLeds);
  FastLED.setBrightness(40);
  FastLED.clear(true);

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
  g_speedLevel = 1;
  applyDifficultyFromLevel();
  g_score = 0;
  g_nextSpawnMs = now + g_spawnIntervalMs;
  g_nextGravityMs = now + g_gravityIntervalMs;
  g_nextSpeedupMs = now + kTierUpIntervalMs;
  g_screen = GameScreen::Splash;
}

void loop() {
  const uint32_t now = millis();

  handleGameButton(now);
  handleFireButtons(now);

  if (g_screen == GameScreen::Playing && !g_gameOver) {
    tryTierUp(now);
    if (now >= g_nextGravityMs) {
      g_nextGravityMs = now + g_gravityIntervalMs;
      applyGravityOnce();
      if (g_play[kPlayLen - 1] != 0) {
        triggerGameOver(now);
      } else {
        moveBulletsOnce(now);
      }
    }
    if (now >= g_nextSpawnMs) {
      g_nextSpawnMs = now + g_spawnIntervalMs;
      trySpawn();
    }
  }

  sceneToLeds();
  FastLED.show();
  drawOled(now);
  buzzerSeqTick(now);
}
