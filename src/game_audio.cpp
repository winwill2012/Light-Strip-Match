#include "game_audio.h"

#include <cstring>

#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "game_sounds.h"

// ===================== MAX98357 I2S 引脚 =====================
static constexpr int PIN_I2S_BCLK = 6;
static constexpr int PIN_I2S_LRC = 7;
static constexpr int PIN_I2S_DIN = 3;

static constexpr uint32_t kSampleRate = 44100;
static constexpr size_t kChunkSamples = 512;
static constexpr float kSfxGain = 1.0f;
static constexpr float kUiGain = 0.55f;
static int16_t g_pcm[kChunkSamples];
static bool g_i2sOk = false;
static TaskHandle_t g_audioTask = nullptr;
static SemaphoreHandle_t g_audioMx = nullptr;

enum class SoundPri : uint8_t { Sfx = 1, Upgrade = 2, GameOver = 3 };

struct Playback {
  const uint8_t *data = nullptr;
  size_t posBytes = 0;
  size_t totalBytes = 0;
  SoundPri priority = SoundPri::Sfx;
  float gain = 1.0f;
  bool active = false;
};
static Playback g_play;

static int16_t softLimit(const int32_t v) {
  if (v > 32767) {
    return 32767;
  }
  if (v < -32768) {
    return static_cast<int16_t>(-32768);
  }
  return static_cast<int16_t>(v);
}

static void applyGainStereo(int16_t *samples, const size_t sampleCount, const float gain) {
  if (gain >= 0.999f) {
    return;
  }
  for (size_t i = 0; i < sampleCount; ++i) {
    const int32_t scaled = static_cast<int32_t>(static_cast<float>(samples[i]) * gain);
    samples[i] = softLimit(scaled);
  }
}

static bool writePcmToI2s(const uint8_t *data, const size_t bytes) {
  size_t sent = 0;
  while (sent < bytes) {
    size_t written = 0;
    const esp_err_t err =
        i2s_write(I2S_NUM_0, data + sent, bytes - sent, &written, portMAX_DELAY);
    if (err != ESP_OK || written == 0) {
      return false;
    }
    sent += written;
  }
  return true;
}

static void stopPlaybackLocked() {
  g_play.active = false;
  g_play.data = nullptr;
  g_play.posBytes = 0;
  g_play.totalBytes = 0;
}

static bool canReplaceLocked(const SoundPri incoming) {
  if (!g_play.active) {
    return true;
  }
  if (incoming == SoundPri::Sfx && g_play.priority == SoundPri::Sfx) {
    return true;
  }
  return static_cast<uint8_t>(incoming) >= static_cast<uint8_t>(g_play.priority);
}

static bool pumpOneChunk() {
  if (!g_i2sOk) {
    return false;
  }

  if (xSemaphoreTake(g_audioMx, pdMS_TO_TICKS(5)) != pdTRUE) {
    return g_play.active;
  }

  if (!g_play.active || g_play.data == nullptr) {
    xSemaphoreGive(g_audioMx);
    return false;
  }

  const size_t remain = g_play.totalBytes - g_play.posBytes;
  if (remain == 0) {
    stopPlaybackLocked();
    xSemaphoreGive(g_audioMx);
    return false;
  }

  size_t srcBytes = remain;
  if (srcBytes > kChunkSamples * sizeof(int16_t)) {
    srcBytes = kChunkSamples * sizeof(int16_t);
  }

  memcpy_P(g_pcm, g_play.data + g_play.posBytes, srcBytes);
  applyGainStereo(g_pcm, srcBytes / sizeof(int16_t), g_play.gain);

  xSemaphoreGive(g_audioMx);

  if (!writePcmToI2s(reinterpret_cast<const uint8_t *>(g_pcm), srcBytes)) {
    return g_play.active;
  }

  if (xSemaphoreTake(g_audioMx, pdMS_TO_TICKS(5)) != pdTRUE) {
    return g_play.active;
  }
  g_play.posBytes += srcBytes;
  if (g_play.posBytes >= g_play.totalBytes) {
    stopPlaybackLocked();
  }
  xSemaphoreGive(g_audioMx);
  return g_play.active;
}

static void audioTask(void * /*arg*/) {
  constexpr int kMaxChunksPerWake = 32;
  for (;;) {
    if (g_play.active) {
      for (int n = 0; n < kMaxChunksPerWake && g_play.active; ++n) {
        if (!pumpOneChunk()) {
          break;
        }
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    } else {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
  }
}

static void startSound(const GameSound &snd, const SoundPri pri, const float gain) {
  if (!g_i2sOk || snd.bytes == 0 || g_audioMx == nullptr) {
    return;
  }

  if (xSemaphoreTake(g_audioMx, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }

  if (!canReplaceLocked(pri)) {
    xSemaphoreGive(g_audioMx);
    return;
  }

  g_play.data = snd.data;
  g_play.posBytes = 0;
  g_play.totalBytes = snd.bytes;
  g_play.priority = pri;
  g_play.gain = gain;
  g_play.active = true;
  xSemaphoreGive(g_audioMx);

  if (g_audioTask != nullptr) {
    xTaskNotifyGive(g_audioTask);
  }
}

void gameAudioInit() {
  g_audioMx = xSemaphoreCreateMutex();
  if (g_audioMx == nullptr) {
    return;
  }

  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 10;
  cfg.dma_buf_len = 512;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_LRC;
  pins.data_out_num = PIN_I2S_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  g_i2sOk = (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) == ESP_OK);
  if (g_i2sOk) {
    g_i2sOk = (i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK);
  }
  if (g_i2sOk) {
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_set_sample_rates(I2S_NUM_0, kSampleRate);
    xTaskCreate(audioTask, "audio", 4096, nullptr, 6, &g_audioTask);
  }
}

void gameAudioTick(const uint32_t /*nowMs*/) {
  // 音频由独立任务喂 DMA，主循环不再重复唤醒
}

void gameAudioScore() {
  startSound(kSoundScore, SoundPri::Sfx, kSfxGain);
}

void gameAudioFire(const uint8_t /*color*/) {
  startSound(kSoundShoot, SoundPri::Sfx, kSfxGain);
}

void gameAudioUi() {
  startSound(kSoundMenu, SoundPri::Sfx, kUiGain);
}

void gameAudioBegin() {
  startSound(kSoundBegin, SoundPri::Sfx, kSfxGain);
}

void gameAudioSeqStartUpgrade(const uint32_t /*nowMs*/) {
  if (g_play.active && g_play.priority == SoundPri::GameOver) {
    return;
  }
  startSound(kSoundUpgrade, SoundPri::Upgrade, kSfxGain);
}

void gameAudioSeqStartGameOver(const uint32_t /*nowMs*/) {
  startSound(kSoundGameOver, SoundPri::GameOver, kSfxGain);
}

void gameAudioSeqStop() {
  if (g_audioMx == nullptr) {
    return;
  }
  if (xSemaphoreTake(g_audioMx, pdMS_TO_TICKS(20)) == pdTRUE) {
    stopPlaybackLocked();
    xSemaphoreGive(g_audioMx);
  }
}
