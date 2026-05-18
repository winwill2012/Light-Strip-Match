#pragma once

#include <Arduino.h>

void wifiPortalBegin();
void wifiPortalLoop();
bool wifiPortalApRunning();
const char *wifiPortalApSsid();
