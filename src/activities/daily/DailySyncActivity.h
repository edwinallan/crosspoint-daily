#pragma once

#include <cstdint>

#include "activities/Activity.h"

class DailySyncActivity final : public Activity {
 public:
  explicit DailySyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DailySync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Connecting || state == State::Syncing; }

 private:
  enum class State : uint8_t { Connecting, Syncing, Succeeded, Failed, NoWifi };

  State state = State::Connecting;
  bool wifiActivated = false;

  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void runSync();
};
