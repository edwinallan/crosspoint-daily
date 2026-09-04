#include "DailySyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <utility>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "bible/BibleLibrary.h"
#include "components/UITheme.h"
#include "fontIds.h"

void DailySyncActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    state = State::Syncing;
    requestUpdate();
    return;
  }

  wifiActivated = true;
  launchWifiSelection();
}

void DailySyncActivity::onExit() {
  Activity::onExit();

  // Wi-Fi/TLS leaves fragmented heap on this constrained target. Match the
  // established network-activity cleanup and return to Home without a splash.
  if (wifiActivated && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void DailySyncActivity::launchWifiSelection() {
  auto activity = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
  if (!activity) {
    LOG_ERR("DAILY", "OOM: WifiSelectionActivity");
    state = State::Failed;
    requestUpdate();
    return;
  }

  startActivityForResult(std::move(activity),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void DailySyncActivity::onWifiSelectionComplete(const bool connected) {
  state = connected ? State::Syncing : State::NoWifi;
  requestUpdate();
}

void DailySyncActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    state = State::NoWifi;
    requestUpdate();
    return;
  }

  state = bible::BibleLibrary::refreshDailyVerse() ? State::Succeeded : State::Failed;
  requestUpdate();
}

void DailySyncActivity::loop() {
  if (state == State::Syncing) {
    // Complete the e-ink update before the blocking HTTPS request begins.
    requestUpdateAndWait();
    runSync();
    return;
  }

  if (state == State::Connecting) return;

  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) finish();
}

void DailySyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DAILY_SYNC));

  switch (state) {
    case State::Connecting:
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_CONNECTING));
      break;
    case State::Syncing:
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_DAILY_SYNCING));
      break;
    case State::Succeeded:
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_DAILY_SYNC_COMPLETE), true,
                                EpdFontFamily::BOLD);
      break;
    case State::Failed:
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_DAILY_SYNC_FAILED), true,
                                EpdFontFamily::BOLD);
      break;
    case State::NoWifi:
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_WIFI_CONN_FAILED), true,
                                EpdFontFamily::BOLD);
      break;
  }

  if (state != State::Connecting && state != State::Syncing) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
