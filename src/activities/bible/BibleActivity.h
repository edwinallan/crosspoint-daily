#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "bible/BibleLibrary.h"

struct Rect;

class BibleActivity final : public Activity {
  enum class View : uint8_t { Home, Chapters, Reader };
  enum class HomeMode : uint8_t { Books, Versions };
  enum class ReaderNoteMode : uint8_t { Reading, Selecting, Popup };
  enum class BookDirection : uint8_t { None, Left, Right, Up, Down };

  struct VisualLine {
    size_t start = 0;
    size_t length = 0;
    size_t next = 0;
  };

  static constexpr size_t LINE_BUFFER_SIZE = 512;
  static constexpr int BOOK_GRID_COLUMNS = 11;
  static constexpr unsigned long BOOK_REPEAT_START_MS = 500;
  static constexpr unsigned long BOOK_REPEAT_INTERVAL_MS = 150;
  static constexpr unsigned long VERSION_SELECT_LONG_PRESS_MS = 600;
  static constexpr unsigned long NOTE_SELECT_LONG_PRESS_MS = 600;

  View view = View::Home;
  HomeMode homeMode = HomeMode::Books;
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
  BookDirection bookRepeatDirection = BookDirection::None;
  unsigned long lastBookRepeatMs = 0;

  bool readerLoadFailed = false;
  bool confirmLongHandled = false;
  bool readerConfirmLongHandled = false;
  bool dailyTranslationCustom = false;
  bool dailySelectionAvailable = false;
  bool dailyJumpPending = false;
  bool showingDailyApiText = false;
  bool homeSelectionChanged = false;
  bool homeFullRenderPending = true;
  bool resumeFromSleep = false;
  int dailySourceVersionIndex = -1;
  int renderedHomeBookIndex = 0;
  char dailyBookId[12]{};
  ReaderNoteMode readerNoteMode = ReaderNoteMode::Reading;
  std::array<bible::ChapterNote, bible::MAX_CHAPTER_NOTE_COUNT> chapterNotes{};
  size_t chapterNoteCount = 0;
  size_t selectedNoteIndex = 0;
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
  void drawHomeBookCell(const Rect& bounds, int columns, int rows, int index, bool eraseFirst);
  void renderChapters();
  void renderReader();

  void enterHome();
  void enterChapters();
  void openReader();
  void releaseChapter();
  bool handleHomeSelect();
  bool handleRepeatedBookNavigation();
  bool handleVersionNavigation();
  void moveHomeBook(BookDirection direction);
  MappedInputManager::Button buttonForBookDirection(BookDirection direction) const;
  void changeChapterBook(int direction);
  bool switchVersionLocked(int direction);
  void switchVersion(int direction);
  bool loadReaderChapterLocked();
  void changeReaderChapter(int direction);
  void activateNoteSelection();
  void moveSelectedNote(int direction);
  int pageForTextOffset(size_t offset) const;
  void selectNearestChapter(uint16_t preferredChapter);
  int findBookIndex(const char* bookId) const;
  int findVersionIndex(const char* abbreviation, const char* name) const;
  int findDailyBookIndex() const;
  void selectDailyContext();
  void restoreSleepPosition();
  bool isDailyBookAndChapter() const;
  bool shouldUseDailyApiText() const;
  bool findVerseOffset(uint16_t verse, size_t& offset) const;

  bool nextVisualLine(size_t offset, VisualLine& line);
  void buildPageIndex();
  bool copyVisualLine(const VisualLine& line);
  int measureVisualText(char* text);
  void drawVisualText(int x, int y, char* text);
  void drawMemorisationGauge(int pageWidth, int y);
  void drawNotePopup();
  void drawWrappedNoteText(const Rect& bounds, const char* text);

 public:
  explicit BibleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool resumeFromSleep = false)
      : Activity("Bible", renderer, mappedInput), resumeFromSleep(resumeFromSleep) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return view == View::Reader; }
  bool isBibleActivity() const override { return true; }
  void onBeforeSleep() override;
};
