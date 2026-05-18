#pragma once

#include <Arduino.h>

// MAX98357 I2S：BCLK=GPIO6, LRC=GPIO7, DIN=GPIO3
void gameAudioInit();
void gameAudioTick(uint32_t nowMs);

void gameAudioScore();
void gameAudioFire(uint8_t color);
void gameAudioUi();
void gameAudioBegin();

void gameAudioSeqStartUpgrade(uint32_t nowMs);
void gameAudioSeqStartGameOver(uint32_t nowMs);
void gameAudioSeqStop();
