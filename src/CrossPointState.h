#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  enum class SleepResumeTarget : uint8_t { Home, Reader, Bible };
  enum class BibleResumeView : uint8_t { Home, Chapters, Reader };

  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  SleepResumeTarget sleepResumeTarget = SleepResumeTarget::Home;
  bool showBootScreen = true;
  BibleResumeView bibleResumeView = BibleResumeView::Home;
  char bibleResumeVersion[32]{};
  char bibleResumeBook[12]{};
  uint16_t bibleResumeChapter = 0;
  uint16_t bibleResumePage = 0;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
