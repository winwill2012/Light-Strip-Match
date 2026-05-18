#include "game_audio.h"

#include <cstring>

#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "game_sounds.h"

// ===================== MAX98357 I2S 引脚 =====================
static constexpr int PIN_I2S_BCLK = 6;
static constexpr int PIN_I2S_LRC = 7;
static constexpr int PIN_I2S_DIN = 3;

static constexpr uint32_t kSampleRate = 44100;
static constexpr size_t kChunkSamples = 512;
static constexpr float kSfxGain = 1.48f;
static constexpr float kUiGain = 1.15f;
static constexpr float kTrebleAmt = 0.42f;
static constexpr int kPrimeChunks = 6;

static int16_t g_pcm[kChunkSamples];
static int16_t g_prevMono = 0;
static bool g_i2sOk = false;
static TaskHandle_t g_audioTask = nullptr;

enum class SoundPri : uint8_t { Sfx = 1, Upgrade = 2, GameOver = 3 };

struct Playback {
  const uint8_t *data = nullptr;
  size_t posBytes = 0;
  size_t totalBytes = 0;
  SoundPri priority = SoundPri::Sfx;
  float gain = 1.0f;
  volatile bool active = false;
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

static int16_t softCompress(const int32_t v) {
  constexpr int32_t kThresh = 26000;
  if (v > kThresh) {
    return static_cast<int16_t>(kThresh + ((v - kThresh) * 3) / 8);
  }
  if (v < -kThresh) {
    return static_cast<int16_t>(-kThresh + ((v + kThresh) * 3) / 8);
  }
  return static_cast<int16_t>(v);
}

static void resetFxState() {
  g_prevMono = 0;
}

static void processChunk(int16_t *samples, const size_t count, const float gain) {
  for (size_t i = 0; i + 1 < count; i += 2) {
    const int32_t mono = (static_cast<int32_t>(samples[i]) + static_cast<int32_t>(samples[i + 1])) / 2;
    const int32_t bright = mono + static_cast<int32_t>(static_cast<float>(mono - g_prevMono) * kTrebleAmt);
    g_prevMono = static_cast<int16_t>(mono);

    int32_t out = static_cast<int32_t>(static_cast<float>(bright) * gain);
    out = softCompress(out);
    out = softLimit(out);
    const int16_t s = static_cast<int16_t>(out);
    samples[i] = s;
    samples[i + 1] = s;
  }
}

static void stopPlayback() {
  g_play.active = false;
  g_play.data = nullptr;
  g_play.posBytes = 0;
  g_play.totalBytes = 0;
  resetFxState();
}

static bool canReplace(const SoundPri incoming) {
  if (!g_play.active) {
    return true;
  }
  return static_cast<uint8_t>(incoming) >= static_cast<uint8_t>(g_play.priority);
}

static bool pumpOneChunk() {
  if (!g_i2sOk || !g_play.active || g_play.data == nullptr) {
    return false;
  }

  const size_t remain = g_play.totalBytes - g_play.posBytes;
  if (remain == 0) {
    stopPlayback();
    return false;
  }

  size_t srcBytes = remain;
  if (srcBytes > kChunkSamples * sizeof(int16_t)) {
    srcBytes = kChunkSamples * sizeof(int16_t);
  }
  const size_t srcSamples = srcBytes / sizeof(int16_t);

  memcpy_P(g_pcm, g_play.data + g_play.posBytes, srcBytes);
  processChunk(g_pcm, srcSamples, g_play.gain);

  size_t written = 0;
  i2s_write(I2S_NUM_0, g_pcm, srcBytes, &written, portMAX_DELAY);
  g_play.posBytes += srcBytes;

  if (g_play.posBytes >= g_play.totalBytes) {
    stopPlayback();
    return false;
  }
  return true;
}

static void primePlayback() {
  for (int i = 0; i < kPrimeChunks; ++i) {
    if (!pumpOneChunk()) {
      break;
    }
  }
}

static void audioTask(void * /*arg*/) {
  constexpr int kMaxChunksPerWake = 24;
  for (;;) {
    if (g_play.active) {
      for (int n = 0; n < kMaxChunksPerWake && g_play.active; ++n) {
        if (!pumpOneChunk()) {
          break;
        }
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
    } else {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
  }
}

static void startSound(const GameSound &snd, const SoundPri pri, const float gain) {
  if (!g_i2sOk || snd.bytes == 0) {
    return;
  }
  if (!canReplace(pri)) {
    return;
  }
  resetFxState();
  g_play.data = snd.data;
  g_play.posBytes = 0;
  g_play.totalBytes = snd.bytes;
  g_play.priority = pri;
  g_play.gain = gain;
  g_play.active = true;
  primePlayback();
  if (g_audioTask != nullptr) {
    xTaskNotifyGive(g_audioTask);
  }
}

void gameAudioInit() {
  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 512;
  cfg.use_apll = true;
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
    xTaskCreate(audioTask, "audio", 3072, nullptr, 5, &g_audioTask);
  }
}

void gameAudioTick(const uint32_t /*nowMs*/) {
  if (g_play.active && g_audioTask != nullptr) {
    xTaskNotifyGive(g_audioTask);
  }
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
  stopPlayback();
}
