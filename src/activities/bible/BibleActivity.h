#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "bible/BibleLibrary.h"

class BibleActivity final : public Activity {
  enum class View : uint8_t { Home, Chapters, Reader };

  struct VisualLine {
    size_t start = 0;
    size_t length = 0;
    size_t next = 0;
  };

  static constexpr size_t LINE_BUFFER_SIZE = 512;
  static constexpr unsigned long BOOK_REPEAT_START_MS = 500;
  static constexpr unsigned long BOOK_REPEAT_INTERVAL_MS = 150;

  View view = View::Home;
  std::vector<bible::VersionInfo> versions;
  std::vector<bible::BookInfo> books;
  std::vector<uint16_t> chapters;
  std::vector<size_t> pageOffsets;
  bible::DailyVerse dailyVerse;

  int versionIndex = 0;
  int bookIndex = 0;
  int chapterIndex = 0;
  int currentPage = 0;
  int totalPages = 0;
  int linesPerPage = 1;
  int viewportLeft = 0;
  int viewportTop = 0;
  int viewportWidth = 0;
  int readerFontId = 0;
  int pagesUntilFullRefresh = 0;
  int bookRepeatDirection = 0;
  unsigned long lastBookRepeatMs = 0;

  bool readerLoadFailed = false;
  bool chapterHasNotes = false;
  std::unique_ptr<char[]> chapterText;
  size_t chapterTextLength = 0;
  size_t chapterTextCapacity = 0;
  char lineBuffer[LINE_BUFFER_SIZE]{};

  const bible::VersionInfo* currentVersion() const;
  const bible::BookInfo* currentBook() const;
  uint16_t currentChapter() const;

  void handleHomeInput();
  void handleChapterInput();
  void handleReaderInput();
  void renderHome();
  void renderChapters();
  void renderReader();

  void enterHome();
  void enterChapters();
  void openReader();
  void releaseChapter();
  bool handleRepeatedBookNavigation();
  void moveHomeBook(int direction);
  void changeChapterBook(int direction);
  bool switchVersionLocked(int direction);
  void switchVersion(int direction);
  bool loadReaderChapterLocked();
  void changeReaderChapter(int direction);
  void selectNearestChapter(uint16_t preferredChapter);
  int findBookIndex(const char* bookId) const;

  bool nextVisualLine(size_t offset, VisualLine& line);
  void buildPageIndex();
  bool copyVisualLine(const VisualLine& line);

 public:
  explicit BibleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Bible", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return view == View::Reader; }
};
