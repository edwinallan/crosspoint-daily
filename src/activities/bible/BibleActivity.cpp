#include "BibleActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <I18nKeys.h>
#include <Logging.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace {
constexpr char LOG_TAG[] = "BIBLE";

bool equalsIgnoreCase(const char* left, const char* right) {
  if (!left || !right) return false;
  while (*left && *right) {
    if (std::tolower(static_cast<unsigned char>(*left)) != std::tolower(static_cast<unsigned char>(*right))) {
      return false;
    }
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

bool endsWithIgnoreCase(const char* value, const char* suffix) {
  if (!value || !suffix) return false;
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  return valueLength >= suffixLength && equalsIgnoreCase(value + valueLength - suffixLength, suffix);
}

void copyWithoutSuffix(char* destination, const size_t destinationSize, const char* source, const char* suffix) {
  if (!destination || destinationSize == 0) return;
  destination[0] = '\0';
  if (!source) return;
  size_t length = strlen(source);
  if (endsWithIgnoreCase(source, suffix)) length -= strlen(suffix);
  length = std::min(length, destinationSize - 1);
  memcpy(destination, source, length);
  destination[length] = '\0';
}

bool equivalentBookName(const char* left, const char* right) {
  if (equalsIgnoreCase(left, right)) return true;
  if (!left || !right) return false;
  const size_t leftLength = strlen(left);
  const size_t rightLength = strlen(right);
  if (leftLength + 1 == rightLength && std::tolower(static_cast<unsigned char>(right[rightLength - 1])) == 's') {
    return strncasecmp(left, right, leftLength) == 0;
  }
  if (rightLength + 1 == leftLength && std::tolower(static_cast<unsigned char>(left[leftLength - 1])) == 's') {
    return strncasecmp(left, right, rightLength) == 0;
  }
  return false;
}

int wrappedIndex(const int index, const int count) {
  if (count <= 0) return 0;
  const int remainder = index % count;
  return remainder < 0 ? remainder + count : remainder;
}
}  // namespace

const bible::VersionInfo* BibleActivity::currentVersion() const {
  if (versionIndex < 0 || versionIndex >= static_cast<int>(versions.size())) return nullptr;
  return &versions[versionIndex];
}

const bible::BookInfo* BibleActivity::currentBook() const {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return nullptr;
  return &books[bookIndex];
}

uint16_t BibleActivity::currentChapter() const {
  if (chapterIndex < 0 || chapterIndex >= static_cast<int>(chapters.size())) return 0;
  return chapters[chapterIndex];
}

void BibleActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const unsigned long openStartedAt = millis();

  versions.reserve(bible::MAX_VERSION_COUNT);
  books.reserve(bible::MAX_BOOK_COUNT);
  chapters.reserve(bible::MAX_CHAPTER_COUNT);
  pageOffsets.reserve(bible::MAX_PAGE_COUNT);

  bible::BibleLibrary::loadDailyVerse(dailyVerse, &dailyVerseIndex, &dailyVerseCount);
  const unsigned long dailyVerseLoadedAt = millis();
  if (bible::BibleLibrary::discoverVersions(versions)) {
    const char* uiLanguage = LANGUAGE_CODES[static_cast<size_t>(I18N.getLanguage())];
    for (size_t i = 0; i < versions.size(); ++i) {
      if (equalsIgnoreCase(versions[i].language, uiLanguage)) {
        versionIndex = static_cast<int>(i);
        break;
      }
    }
    const int rememberedVersion = findVersionIndex(APP_STATE.bibleResumeVersion, nullptr);
    if (rememberedVersion >= 0) versionIndex = rememberedVersion;
    selectDailyContext();
    if (resumeFromSleep) restoreSleepPosition();
  }

  LOG_DBG(LOG_TAG, "Open preparation: verse=%lums manifests=%lums total=%lums", dailyVerseLoadedAt - openStartedAt,
          millis() - dailyVerseLoadedAt, millis() - openStartedAt);

  // Bible opening is deliberately cache-only. Daily data is refreshed by the
  // explicit Sync action on Home so entering the reader never waits on Wi-Fi.
  requestUpdate();
}

void BibleActivity::onBeforeSleep() {
  switch (view) {
    case View::Home:
      APP_STATE.bibleResumeView = CrossPointState::BibleResumeView::Home;
      break;
    case View::Chapters:
      APP_STATE.bibleResumeView = CrossPointState::BibleResumeView::Chapters;
      break;
    case View::Reader:
      APP_STATE.bibleResumeView = CrossPointState::BibleResumeView::Reader;
      break;
  }

  const auto* version = currentVersion();
  const auto* book = currentBook();
  snprintf(APP_STATE.bibleResumeVersion, sizeof(APP_STATE.bibleResumeVersion), "%s", version ? version->directory : "");
  snprintf(APP_STATE.bibleResumeBook, sizeof(APP_STATE.bibleResumeBook), "%s", book ? book->id : "");
  APP_STATE.bibleResumeChapter = currentChapter();
  APP_STATE.bibleResumePage = static_cast<uint16_t>(std::max(currentPage, 0));
}

void BibleActivity::onExit() {
  if (const auto* version = currentVersion();
      version && strcmp(APP_STATE.bibleResumeVersion, version->directory) != 0) {
    snprintf(APP_STATE.bibleResumeVersion, sizeof(APP_STATE.bibleResumeVersion), "%s", version->directory);
    APP_STATE.saveToFile();
  }
  releaseChapter();
  versions.clear();
  books.clear();
  chapters.clear();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  Activity::onExit();
}

void BibleActivity::loop() {
  switch (view) {
    case View::Home:
      handleHomeInput();
      break;
    case View::Chapters:
      handleChapterInput();
      break;
    case View::Reader:
      handleReaderInput();
      break;
  }
}

void BibleActivity::handleHomeInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (homeMode == HomeMode::Versions) {
      homeMode = HomeMode::Books;
      homeFullRenderPending = true;
      requestUpdate();
      return;
    }
    activityManager.goHome(HomeMenuItem::BIBLE);
    return;
  }

  if (handleHomeSelect()) return;

  if (homeMode == HomeMode::Versions) {
    if (handleVersionNavigation()) return;
  } else {
    if (handleDailyVerseNavigation()) return;
    if (handleRepeatedBookNavigation()) return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (homeMode == HomeMode::Versions) {
    if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
      switchVersion(1);
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
      switchVersion(-1);
      return;
    }
  } else {
    if (swipe == MappedInputManager::SwipeDir::Left) {
      moveHomeBook(BookDirection::Right);
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Right) {
      moveHomeBook(BookDirection::Left);
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Up) {
      moveHomeBook(BookDirection::Down);
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      moveHomeBook(BookDirection::Up);
      return;
    }
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int selectorTop = renderer.getScreenHeight() * 7 / 10;
  if (homeMode == HomeMode::Books && !books.empty() &&
      mappedInput.wasTapInRect(0, selectorTop, renderer.getScreenWidth(),
                               renderer.getScreenHeight() - selectorTop - metrics.buttonHintsHeight)) {
    enterChapters();
  }
}

bool BibleActivity::handleDailyVerseNavigation() {
  int direction = 0;
  if (mappedInput.isPressed(MappedInputManager::Button::PageBack)) {
    direction = -1;
  } else if (mappedInput.isPressed(MappedInputManager::Button::PageForward)) {
    direction = 1;
  }

  if (!dailyVerseLongHandled && direction != 0 && dailyVerseCount > 1 &&
      mappedInput.getHeldTime() >= DAILY_VERSE_LONG_PRESS_MS) {
    dailyVerseLongHandled = true;
    moveDailyVerse(direction);
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    if (dailyVerseLongHandled) {
      dailyVerseLongHandled = false;
    } else {
      moveHomeBook(BookDirection::Left);
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    if (dailyVerseLongHandled) {
      dailyVerseLongHandled = false;
    } else {
      moveHomeBook(BookDirection::Right);
    }
    return true;
  }
  return false;
}

bool BibleActivity::handleHomeSelect() {
  if (!confirmLongHandled && homeMode == HomeMode::Books &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > VERSION_SELECT_LONG_PRESS_MS) {
    homeMode = HomeMode::Versions;
    bookRepeatDirection = BookDirection::None;
    confirmLongHandled = true;
    homeFullRenderPending = true;
    requestUpdate();
    return true;
  }

  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return false;
  if (confirmLongHandled) {
    confirmLongHandled = false;
    return true;
  }
  if (homeMode == HomeMode::Books && mappedInput.getHeldTime() > VERSION_SELECT_LONG_PRESS_MS) {
    homeMode = HomeMode::Versions;
    bookRepeatDirection = BookDirection::None;
    homeFullRenderPending = true;
    requestUpdate();
    return true;
  }

  if (homeMode == HomeMode::Versions) {
    homeMode = HomeMode::Books;
    homeFullRenderPending = true;
    requestUpdate();
  } else if (!books.empty()) {
    enterChapters();
  }
  return true;
}

bool BibleActivity::handleRepeatedBookNavigation() {
  if (books.empty()) return false;

  BookDirection pressedDirection = BookDirection::None;
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    pressedDirection = BookDirection::Up;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    pressedDirection = BookDirection::Down;
  }

  if (pressedDirection != BookDirection::None) {
    bookRepeatDirection = pressedDirection;
    lastBookRepeatMs = millis();
    moveHomeBook(pressedDirection);
    return true;
  }

  if (bookRepeatDirection == BookDirection::None) return false;
  const auto heldButton = buttonForBookDirection(bookRepeatDirection);
  if (!mappedInput.isPressed(heldButton)) {
    bookRepeatDirection = BookDirection::None;
    return false;
  }

  const unsigned long now = millis();
  if (mappedInput.getHeldTime() <= BOOK_REPEAT_START_MS || now - lastBookRepeatMs < BOOK_REPEAT_INTERVAL_MS) {
    return false;
  }

  lastBookRepeatMs = now;
  moveHomeBook(bookRepeatDirection);
  return true;
}

void BibleActivity::moveDailyVerse(const int direction) {
  if (dailyVerseCount < 2 || direction == 0) return;

  const uint16_t target = static_cast<uint16_t>(
      wrappedIndex(static_cast<int>(dailyVerseIndex) + direction, static_cast<int>(dailyVerseCount)));
  uint16_t refreshedCount = dailyVerseCount;
  bool loaded = false;
  {
    RenderLock lock(*this);
    loaded = bible::BibleLibrary::loadDailyVerseAt(target, dailyVerse, &refreshedCount);
    if (loaded) {
      dailyVerseIndex = target;
      dailyVerseCount = refreshedCount;
      selectDailyContext();
      bookRepeatDirection = BookDirection::None;
      homeFullRenderPending = true;
      homeSelectionChanged = true;
    }
  }
  if (loaded) requestUpdate();
}

MappedInputManager::Button BibleActivity::buttonForBookDirection(const BookDirection direction) const {
  switch (direction) {
    case BookDirection::Left:
      return MappedInputManager::Button::PageBack;
    case BookDirection::Right:
      return MappedInputManager::Button::PageForward;
    case BookDirection::Up:
      return MappedInputManager::Button::Left;
    case BookDirection::Down:
      return MappedInputManager::Button::Right;
    case BookDirection::None:
      return MappedInputManager::Button::Power;
  }
  return MappedInputManager::Button::Power;
}

void BibleActivity::moveHomeBook(const BookDirection direction) {
  if (books.empty() || direction == BookDirection::None) return;

  RenderLock lock(*this);
  const int count = static_cast<int>(books.size());
  const int columns = std::min(BOOK_GRID_COLUMNS, count);
  const int previousIndex = bookIndex;
  switch (direction) {
    case BookDirection::Left:
      bookIndex = ButtonNavigator::previousIndex(bookIndex, count);
      break;
    case BookDirection::Right:
      bookIndex = ButtonNavigator::nextIndex(bookIndex, count);
      break;
    case BookDirection::Up: {
      const int column = bookIndex % columns;
      bookIndex -= columns;
      if (bookIndex < 0) bookIndex = column + ((count - 1 - column) / columns) * columns;
      break;
    }
    case BookDirection::Down: {
      const int column = bookIndex % columns;
      bookIndex += columns;
      if (bookIndex >= count) bookIndex = column;
      break;
    }
    case BookDirection::None:
      return;
  }

  if (bookIndex != previousIndex) {
    homeSelectionChanged = true;
    lock.unlock();
    requestUpdate();
  }
}

bool BibleActivity::handleVersionNavigation() {
  int direction = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
    direction = -1;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
             mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
    direction = 1;
  }
  if (direction == 0) return false;
  switchVersion(direction);
  return true;
}

void BibleActivity::handleChapterInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    enterHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!chapters.empty()) openReader();
    return;
  }

  if (!chapters.empty() && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    chapterIndex = ButtonNavigator::previousIndex(chapterIndex, static_cast<int>(chapters.size()));
    requestUpdate();
    return;
  }
  if (!chapters.empty() && mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    chapterIndex = ButtonNavigator::nextIndex(chapterIndex, static_cast<int>(chapters.size()));
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    changeChapterBook(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    changeChapterBook(1);
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.headerHeight;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  switch (handleListTouch(chapterIndex, static_cast<int>(chapters.size()), contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      if (!chapters.empty()) openReader();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  const int pageItems = GUI.getListPageItems(contentHeight, false);
  const auto swipe = mappedInput.wasSwipe();
  if (!chapters.empty() && swipe == MappedInputManager::SwipeDir::Up) {
    chapterIndex = ButtonNavigator::nextPageIndex(chapterIndex, static_cast<int>(chapters.size()), pageItems);
    requestUpdate();
  } else if (!chapters.empty() && swipe == MappedInputManager::SwipeDir::Down) {
    chapterIndex = ButtonNavigator::previousPageIndex(chapterIndex, static_cast<int>(chapters.size()), pageItems);
    requestUpdate();
  }
}

void BibleActivity::changeChapterBook(const int direction) {
  if (books.empty() || direction == 0) return;

  {
    RenderLock lock(*this);
    const uint16_t preferredChapter = currentChapter();
    bookIndex = direction < 0 ? ButtonNavigator::previousIndex(bookIndex, static_cast<int>(books.size()))
                              : ButtonNavigator::nextIndex(bookIndex, static_cast<int>(books.size()));
    chapters.clear();
    if (const auto* version = currentVersion()) {
      if (const auto* book = currentBook()) {
        bible::BibleLibrary::loadAvailableChapters(*version, *book, chapters);
      }
    }
    selectNearestChapter(preferredChapter == 0 ? 1 : preferredChapter);
  }
  requestUpdate();
}

void BibleActivity::handleReaderInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (readerNoteMode == ReaderNoteMode::Popup) {
      readerNoteMode = ReaderNoteMode::Selecting;
      requestUpdate();
      return;
    }
    if (readerNoteMode == ReaderNoteMode::Selecting) {
      readerNoteMode = ReaderNoteMode::Reading;
      requestUpdate();
      return;
    }
    enterChapters();
    return;
  }

  if (readerNoteMode == ReaderNoteMode::Reading && chapterNoteCount > 0 && !readerConfirmLongHandled &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > NOTE_SELECT_LONG_PRESS_MS) {
    readerConfirmLongHandled = true;
    activateNoteSelection();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (readerConfirmLongHandled) {
      readerConfirmLongHandled = false;
      return;
    }
    if (readerNoteMode == ReaderNoteMode::Reading && chapterNoteCount > 0 &&
        mappedInput.getHeldTime() > NOTE_SELECT_LONG_PRESS_MS) {
      activateNoteSelection();
      return;
    }
    if (readerNoteMode == ReaderNoteMode::Popup) {
      readerNoteMode = ReaderNoteMode::Selecting;
      requestUpdate();
      return;
    }
    if (readerNoteMode == ReaderNoteMode::Selecting) {
      readerNoteMode = ReaderNoteMode::Popup;
      requestUpdate();
      return;
    }
    switchVersion(1);
    return;
  }

  const bool swapFront = mappedInput.isNavDirectionSwapped();
  const auto previousFrontButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextFrontButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;

  if (readerNoteMode == ReaderNoteMode::Selecting) {
    if (mappedInput.wasReleased(previousFrontButton) || mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      moveSelectedNote(-1);
    } else if (mappedInput.wasReleased(nextFrontButton) ||
               mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      moveSelectedNote(1);
    }
    return;
  }
  if (readerNoteMode == ReaderNoteMode::Popup) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    changeReaderChapter(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    changeReaderChapter(1);
    return;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  const bool previousPage = mappedInput.wasReleased(previousFrontButton) || touch.prev;
  const bool nextPage = mappedInput.wasReleased(nextFrontButton) || touch.next;
  if (previousPage && currentPage > 0) {
    --currentPage;
    requestUpdate();
  } else if (nextPage && currentPage + 1 < totalPages) {
    ++currentPage;
    requestUpdate();
  }
}

void BibleActivity::enterHome() {
  {
    RenderLock lock(*this);
    releaseChapter();
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    view = View::Home;
    homeMode = HomeMode::Books;
    homeFullRenderPending = true;
    bookRepeatDirection = BookDirection::None;
    confirmLongHandled = false;
    dailyVerseLongHandled = false;
  }
  requestUpdate();
}

void BibleActivity::enterChapters() {
  {
    RenderLock lock(*this);
    uint16_t preferredChapter = currentChapter();
    const auto* selectedBook = currentBook();
    if (dailyJumpPending && dailySelectionAvailable && selectedBook && strcmp(selectedBook->id, dailyBookId) == 0) {
      preferredChapter = dailyVerse.referenceChapter;
    }
    releaseChapter();
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    view = View::Chapters;
    chapters.clear();
    if (const auto* version = currentVersion()) {
      if (const auto* book = currentBook()) {
        bible::BibleLibrary::loadAvailableChapters(*version, *book, chapters);
      }
    }
    selectNearestChapter(preferredChapter == 0 ? 1 : preferredChapter);
  }
  requestUpdate();
}

void BibleActivity::openReader() {
  {
    RenderLock lock(*this);
    view = View::Reader;
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    loadReaderChapterLocked();
  }
  requestUpdate();
}

void BibleActivity::releaseChapter() {
  chapterText.reset();
  chapterTextLength = 0;
  chapterTextCapacity = 0;
  chapterNoteCount = 0;
  selectedNoteIndex = 0;
  readerNoteMode = ReaderNoteMode::Reading;
  readerConfirmLongHandled = false;
  readerLoadFailed = false;
  showingDailyApiText = false;
  pageOffsets.clear();
  currentPage = 0;
  totalPages = 0;
}

int BibleActivity::findBookIndex(const char* bookId) const {
  if (!bookId) return -1;
  for (size_t i = 0; i < books.size(); ++i) {
    if (strcmp(books[i].id, bookId) == 0) return static_cast<int>(i);
  }
  return -1;
}

int BibleActivity::findVersionIndex(const char* abbreviation, const char* name) const {
  for (size_t i = 0; i < versions.size(); ++i) {
    const auto& version = versions[i];
    if ((abbreviation &&
         (equalsIgnoreCase(version.id, abbreviation) || equalsIgnoreCase(version.directory, abbreviation))) ||
        (name && equalsIgnoreCase(version.displayName, name))) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int BibleActivity::findDailyBookIndex() const {
  if (!dailyVerse.valid || dailyVerse.referenceBook[0] == '\0') return -1;
  for (size_t i = 0; i < books.size(); ++i) {
    if (equalsIgnoreCase(books[i].id, dailyVerse.referenceBook) ||
        equivalentBookName(books[i].name, dailyVerse.referenceBook)) {
      return static_cast<int>(i);
    }
  }

  // Book names in API references follow the API translation and can differ
  // from the active version's language. Resolve the name in the source
  // manifest, then use its canonical ID in the active version.
  int sourceVersion = findVersionIndex(dailyVerse.translationAbbreviation, nullptr);
  if (sourceVersion < 0 && endsWithIgnoreCase(dailyVerse.translationName, " Modified")) {
    char sourceAbbreviation[sizeof(dailyVerse.translationAbbreviation)]{};
    char sourceName[sizeof(dailyVerse.translationName)]{};
    copyWithoutSuffix(sourceAbbreviation, sizeof(sourceAbbreviation), dailyVerse.translationAbbreviation, " Modified");
    copyWithoutSuffix(sourceName, sizeof(sourceName), dailyVerse.translationName, " Modified");
    sourceVersion = findVersionIndex(sourceAbbreviation, sourceName);
  }

  bible::BookInfo sourceBook{};
  if (sourceVersion >= 0 &&
      bible::BibleLibrary::findBookByName(versions[sourceVersion], dailyVerse.referenceBook, sourceBook)) {
    return findBookIndex(sourceBook.id);
  }

  // If the API translation is not installed, another manifest may still use
  // the same reference language and provide the canonical ID.
  for (size_t i = 0; i < versions.size(); ++i) {
    if (static_cast<int>(i) == sourceVersion) continue;
    if (bible::BibleLibrary::findBookByName(versions[i], dailyVerse.referenceBook, sourceBook)) {
      return findBookIndex(sourceBook.id);
    }
  }
  return -1;
}

void BibleActivity::selectDailyContext() {
  dailyTranslationCustom = false;
  dailySelectionAvailable = false;
  dailyJumpPending = false;
  dailySourceVersionIndex = -1;
  dailyBookId[0] = '\0';
  if (versions.empty()) {
    books.clear();
    return;
  }

  // The displayed API verse may change the selected book/chapter, but never
  // the user's Bible version. Version changes happen only through the version
  // selector.
  if (versionIndex < 0 || versionIndex >= static_cast<int>(versions.size())) versionIndex = 0;
  if (dailyVerse.valid) {
    // The API abbreviation is the identity boundary: a display-name match alone
    // does not turn a custom translation into a local one.
    const int exact = findVersionIndex(dailyVerse.translationAbbreviation, nullptr);
    dailyTranslationCustom = exact < 0;
  }

  if (!bible::BibleLibrary::loadBooks(versions[versionIndex], books)) return;
  bookIndex = 0;
  const int dailyBook = findDailyBookIndex();

  if (dailyBook < 0 || dailyVerse.referenceChapter == 0 || dailyVerse.referenceVerseStart == 0) return;
  bookIndex = dailyBook;
  snprintf(dailyBookId, sizeof(dailyBookId), "%s", books[bookIndex].id);
  dailySourceVersionIndex = versionIndex;
  dailySelectionAvailable = true;
  dailyJumpPending = true;
}

void BibleActivity::restoreSleepPosition() {
  const int savedVersion = findVersionIndex(APP_STATE.bibleResumeVersion, nullptr);
  if (savedVersion >= 0) {
    versionIndex = savedVersion;
    if (!bible::BibleLibrary::loadBooks(versions[versionIndex], books)) return;
  }

  const int savedBook = findBookIndex(APP_STATE.bibleResumeBook);
  if (savedBook >= 0) bookIndex = savedBook;

  switch (APP_STATE.bibleResumeView) {
    case CrossPointState::BibleResumeView::Home:
      view = View::Home;
      return;
    case CrossPointState::BibleResumeView::Chapters:
    case CrossPointState::BibleResumeView::Reader:
      break;
  }

  chapters.clear();
  const auto* version = currentVersion();
  const auto* book = currentBook();
  if (!version || !book || !bible::BibleLibrary::loadAvailableChapters(*version, *book, chapters)) {
    view = View::Home;
    return;
  }
  selectNearestChapter(APP_STATE.bibleResumeChapter == 0 ? 1 : APP_STATE.bibleResumeChapter);

  if (APP_STATE.bibleResumeView == CrossPointState::BibleResumeView::Chapters) {
    view = View::Chapters;
    return;
  }

  view = View::Reader;
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  if (loadReaderChapterLocked() && totalPages > 0) {
    currentPage = std::min(static_cast<int>(APP_STATE.bibleResumePage), totalPages - 1);
  }
}

bool BibleActivity::isDailyBookAndChapter() const {
  const auto* book = currentBook();
  return dailySelectionAvailable && book && strcmp(book->id, dailyBookId) == 0 &&
         currentChapter() == dailyVerse.referenceChapter;
}

bool BibleActivity::shouldUseDailyApiText() const {
  return dailyTranslationCustom && versionIndex == dailySourceVersionIndex && isDailyBookAndChapter();
}

bool BibleActivity::findVerseOffset(const uint16_t verse, size_t& offset) const {
  if (!chapterText || verse == 0) return false;
  size_t lineStart = 0;
  while (lineStart < chapterTextLength) {
    char* parseEnd = nullptr;
    const unsigned long number = strtoul(chapterText.get() + lineStart, &parseEnd, 10);
    if (parseEnd != chapterText.get() + lineStart && number == verse &&
        (*parseEnd == bible::VERSE_NUMBER_END || *parseEnd == bible::NOTE_MARKER_START)) {
      offset = lineStart;
      return true;
    }
    const char* newline =
        static_cast<const char*>(memchr(chapterText.get() + lineStart, '\n', chapterTextLength - lineStart));
    if (!newline) break;
    lineStart = static_cast<size_t>(newline - chapterText.get()) + 1;
  }
  return false;
}

bool BibleActivity::switchVersionLocked(const int direction) {
  if (versions.size() < 2 || direction == 0) return false;

  char selectedBookId[12]{};
  if (const auto* book = currentBook()) snprintf(selectedBookId, sizeof(selectedBookId), "%s", book->id);
  const uint16_t preferredChapter = currentChapter();
  const int count = static_cast<int>(versions.size());
  const int immediate = wrappedIndex(versionIndex + direction, count);
  int selectedVersion = -1;
  bible::BookInfo matchedBook{};

  if (selectedBookId[0] == '\0') {
    selectedVersion = immediate;
  } else if (bible::BibleLibrary::findBook(versions[immediate], selectedBookId, matchedBook)) {
    selectedVersion = immediate;
  } else {
    // A selected version may omit a book. Prefer the next version in that
    // version's language, as required by the Bible storage contract.
    for (int step = 1; step < count; ++step) {
      const int candidate = wrappedIndex(immediate + step * direction, count);
      if (candidate == versionIndex || !equalsIgnoreCase(versions[candidate].language, versions[immediate].language)) {
        continue;
      }
      if (bible::BibleLibrary::findBook(versions[candidate], selectedBookId, matchedBook)) {
        selectedVersion = candidate;
        break;
      }
    }
    // If that language has no compatible version, keep navigation usable by
    // selecting the next detected version that does contain the book.
    for (int step = 1; selectedVersion < 0 && step < count; ++step) {
      const int candidate = wrappedIndex(immediate + step * direction, count);
      if (candidate == versionIndex) continue;
      if (bible::BibleLibrary::findBook(versions[candidate], selectedBookId, matchedBook)) selectedVersion = candidate;
    }
  }

  if (selectedVersion < 0 || selectedVersion == versionIndex) return false;
  const int previousVersion = versionIndex;
  versionIndex = selectedVersion;
  if (!bible::BibleLibrary::loadBooks(versions[versionIndex], books)) {
    versionIndex = previousVersion;
    bible::BibleLibrary::loadBooks(versions[versionIndex], books);
    return false;
  }

  if (selectedBookId[0] != '\0') {
    const int preservedBook = findBookIndex(selectedBookId);
    bookIndex = preservedBook >= 0 ? preservedBook : 0;
  } else {
    bookIndex = 0;
  }

  if (view != View::Home) {
    chapters.clear();
    if (const auto* book = currentBook()) {
      bible::BibleLibrary::loadAvailableChapters(versions[versionIndex], *book, chapters);
    }
    selectNearestChapter(preferredChapter == 0 ? 1 : preferredChapter);
  }
  if (view == View::Reader) loadReaderChapterLocked();
  return true;
}

void BibleActivity::switchVersion(const int direction) {
  bool changed = false;
  {
    RenderLock lock(*this);
    changed = switchVersionLocked(direction);
  }
  if (changed) {
    homeFullRenderPending = true;
    if (view == View::Home) homeSelectionChanged = true;
    requestUpdate();
  }
}

void BibleActivity::selectNearestChapter(const uint16_t preferredChapter) {
  if (chapters.empty()) {
    chapterIndex = 0;
    return;
  }
  const auto position = std::lower_bound(chapters.begin(), chapters.end(), preferredChapter);
  if (position == chapters.end()) {
    chapterIndex = static_cast<int>(chapters.size()) - 1;
  } else {
    chapterIndex = static_cast<int>(position - chapters.begin());
  }
}

bool BibleActivity::loadReaderChapterLocked() {
  releaseChapter();
  const auto* version = currentVersion();
  const auto* book = currentBook();
  const uint16_t chapter = currentChapter();
  if (!version || !book || chapter == 0) {
    readerLoadFailed = true;
    return false;
  }

  showingDailyApiText = shouldUseDailyApiText();
  const bool loaded = showingDailyApiText
                          ? bible::BibleLibrary::loadDailyChapter(dailyVerse, tr(STR_BIBLE_NOTES), chapterText,
                                                                  chapterTextLength, chapterTextCapacity)
                          : bible::BibleLibrary::loadChapter(*version, *book, chapter, chapterText, chapterTextLength,
                                                             chapterTextCapacity, chapterNotes.data(),
                                                             chapterNotes.size(), chapterNoteCount);
  if (!loaded) {
    readerLoadFailed = true;
    return false;
  }

  buildPageIndex();
  if (dailyJumpPending && isDailyBookAndChapter()) {
    size_t verseOffset = 0;
    if (findVerseOffset(dailyVerse.referenceVerseStart, verseOffset)) currentPage = pageForTextOffset(verseOffset);
    dailyJumpPending = false;
  }
  readerLoadFailed = pageOffsets.empty();
  return !readerLoadFailed;
}

void BibleActivity::changeReaderChapter(const int direction) {
  if (chapters.empty() || direction == 0) return;
  const int candidate = chapterIndex + direction;
  if (candidate < 0 || candidate >= static_cast<int>(chapters.size())) return;

  {
    RenderLock lock(*this);
    chapterIndex = candidate;
    loadReaderChapterLocked();
  }
  requestUpdate();
}

int BibleActivity::pageForTextOffset(const size_t offset) const {
  if (pageOffsets.empty()) return 0;
  const auto nextPage = std::upper_bound(pageOffsets.begin(), pageOffsets.end(), offset);
  return nextPage == pageOffsets.begin() ? 0 : static_cast<int>(nextPage - pageOffsets.begin() - 1);
}

void BibleActivity::activateNoteSelection() {
  if (chapterNoteCount == 0) return;
  selectedNoteIndex = 0;
  for (size_t i = 0; i < chapterNoteCount; ++i) {
    if (pageForTextOffset(chapterNotes[i].markerOffset) >= currentPage) {
      selectedNoteIndex = i;
      break;
    }
    selectedNoteIndex = i;
  }
  currentPage = pageForTextOffset(chapterNotes[selectedNoteIndex].markerOffset);
  readerNoteMode = ReaderNoteMode::Selecting;
  requestUpdate();
}

void BibleActivity::moveSelectedNote(const int direction) {
  if (chapterNoteCount == 0 || direction == 0) return;
  const int count = static_cast<int>(chapterNoteCount);
  selectedNoteIndex = static_cast<size_t>(wrappedIndex(static_cast<int>(selectedNoteIndex) + direction, count));
  currentPage = pageForTextOffset(chapterNotes[selectedNoteIndex].markerOffset);
  requestUpdate();
}

bool BibleActivity::nextVisualLine(const size_t offset, VisualLine& line) {
  if (!chapterText || offset >= chapterTextLength || viewportWidth <= 0) return false;
  const char* text = chapterText.get();

  if (text[offset] == '\n') {
    line = VisualLine{offset, 0, offset + 1};
    return true;
  }

  size_t paragraphEnd = offset;
  while (paragraphEnd < chapterTextLength && text[paragraphEnd] != '\n') ++paragraphEnd;
  size_t candidateEnd = std::min(paragraphEnd, offset + LINE_BUFFER_SIZE - 1);
  if (candidateEnd < paragraphEnd) {
    while (candidateEnd > offset && (static_cast<unsigned char>(text[candidateEnd]) & 0xC0) == 0x80) --candidateEnd;
  }

  const auto fits = [this, text, offset](const size_t end) {
    const size_t length = end - offset;
    if (length >= LINE_BUFFER_SIZE) return false;
    memcpy(lineBuffer, text + offset, length);
    lineBuffer[length] = '\0';
    return measureVisualText(lineBuffer) <= viewportWidth;
  };

  while (candidateEnd > offset && !fits(candidateEnd)) {
    size_t space = candidateEnd;
    while (space > offset && text[space - 1] != ' ') --space;
    if (space > offset) {
      candidateEnd = space - 1;
      continue;
    }
    --candidateEnd;
    while (candidateEnd > offset && (static_cast<unsigned char>(text[candidateEnd]) & 0xC0) == 0x80) --candidateEnd;
  }

  if (candidateEnd == offset) {
    candidateEnd = offset + 1;
    while (candidateEnd < paragraphEnd && (static_cast<unsigned char>(text[candidateEnd]) & 0xC0) == 0x80) {
      ++candidateEnd;
    }
  }

  size_t lineEnd = candidateEnd;
  while (lineEnd > offset && text[lineEnd - 1] == ' ') --lineEnd;
  size_t next = candidateEnd;
  if (candidateEnd >= paragraphEnd) {
    next = paragraphEnd < chapterTextLength ? paragraphEnd + 1 : paragraphEnd;
  } else {
    while (next < paragraphEnd && text[next] == ' ') ++next;
  }
  if (next <= offset) next = std::min(chapterTextLength, offset + 1);
  line = VisualLine{offset, lineEnd - offset, next};
  return true;
}

int BibleActivity::measureVisualText(char* text) {
  if (!text) return 0;
  int width = 0;

  char* verseSeparator = strchr(text, bible::VERSE_NUMBER_END);
  char savedVerseSeparator = '\0';
  char* verseNumberEnd = nullptr;
  if (verseSeparator) {
    savedVerseSeparator = *verseSeparator;
    *verseSeparator = ' ';
    verseNumberEnd = text;
    while (*verseNumberEnd >= '0' && *verseNumberEnd <= '9') ++verseNumberEnd;
    if (verseNumberEnd == text) verseNumberEnd = nullptr;
  }

  char* segment = text;
  char* cursor = text;
  if (verseNumberEnd) {
    const char savedEnd = *verseNumberEnd;
    *verseNumberEnd = '\0';
    width += renderer.getTextAdvanceX(readerFontId, text, EpdFontFamily::BOLD);
    *verseNumberEnd = savedEnd;
    segment = verseNumberEnd;
    cursor = verseNumberEnd;
  }
  while (*cursor) {
    if (*cursor != bible::NOTE_MARKER_START) {
      ++cursor;
      continue;
    }
    const char savedStart = *cursor;
    *cursor = '\0';
    width += renderer.getTextAdvanceX(readerFontId, segment, EpdFontFamily::REGULAR);
    *cursor = savedStart;

    char* markerEnd = strchr(cursor + 1, bible::NOTE_MARKER_END);
    if (!markerEnd) {
      segment = cursor;
      break;
    }
    const char savedEnd = *markerEnd;
    *markerEnd = '\0';
    char markerLabel[16]{};
    snprintf(markerLabel, sizeof(markerLabel), "[%s]", cursor + 1);
    width += renderer.getTextAdvanceX(SMALL_FONT_ID, markerLabel, EpdFontFamily::REGULAR);
    *markerEnd = savedEnd;
    cursor = markerEnd + 1;
    segment = cursor;
  }
  width += renderer.getTextAdvanceX(readerFontId, segment, EpdFontFamily::REGULAR);
  if (verseSeparator) *verseSeparator = savedVerseSeparator;
  return width;
}

void BibleActivity::drawVisualText(int x, const int y, char* text) {
  if (!text) return;

  char* verseSeparator = strchr(text, bible::VERSE_NUMBER_END);
  char savedVerseSeparator = '\0';
  char* verseNumberEnd = nullptr;
  if (verseSeparator) {
    savedVerseSeparator = *verseSeparator;
    *verseSeparator = ' ';
    verseNumberEnd = text;
    while (*verseNumberEnd >= '0' && *verseNumberEnd <= '9') ++verseNumberEnd;
    if (verseNumberEnd == text) verseNumberEnd = nullptr;
  }

  char* segment = text;
  char* cursor = text;
  if (verseNumberEnd) {
    const char savedEnd = *verseNumberEnd;
    *verseNumberEnd = '\0';
    renderer.drawText(readerFontId, x, y, text, true, EpdFontFamily::BOLD);
    x += renderer.getTextAdvanceX(readerFontId, text, EpdFontFamily::BOLD);
    *verseNumberEnd = savedEnd;
    segment = verseNumberEnd;
    cursor = verseNumberEnd;
  }
  while (*cursor) {
    if (*cursor != bible::NOTE_MARKER_START) {
      ++cursor;
      continue;
    }
    const char savedStart = *cursor;
    *cursor = '\0';
    renderer.drawText(readerFontId, x, y, segment);
    x += renderer.getTextAdvanceX(readerFontId, segment, EpdFontFamily::REGULAR);
    *cursor = savedStart;

    char* markerEnd = strchr(cursor + 1, bible::NOTE_MARKER_END);
    if (!markerEnd) {
      segment = cursor;
      break;
    }
    const char savedEnd = *markerEnd;
    *markerEnd = '\0';
    char markerLabel[16]{};
    snprintf(markerLabel, sizeof(markerLabel), "[%s]", cursor + 1);
    const int markerWidth = renderer.getTextAdvanceX(SMALL_FONT_ID, markerLabel, EpdFontFamily::REGULAR);
    const int markerHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const unsigned long markerNumber = strtoul(cursor + 1, nullptr, 10);
    const bool selected = readerNoteMode != ReaderNoteMode::Reading && selectedNoteIndex < chapterNoteCount &&
                          markerNumber == chapterNotes[selectedNoteIndex].number;
    if (selected) renderer.fillRoundedRect(x - 1, y - 2, markerWidth + 2, markerHeight, 2, Color::Black);
    renderer.drawText(SMALL_FONT_ID, x, y - 2, markerLabel, !selected);
    x += markerWidth;
    *markerEnd = savedEnd;
    cursor = markerEnd + 1;
    segment = cursor;
  }
  renderer.drawText(readerFontId, x, y, segment);
  if (verseSeparator) *verseSeparator = savedVerseSeparator;
}

void BibleActivity::buildPageIndex() {
  pageOffsets.clear();
  currentPage = 0;
  totalPages = 0;
  if (!chapterText || chapterTextLength == 0) return;

  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int screenMargin = SETTINGS.screenMargin;
  readerFontId = SETTINGS.getReaderFontId();
  viewportLeft = marginLeft + screenMargin;
  viewportTop = marginTop + screenMargin + renderer.getLineHeight(SMALL_FONT_ID);
  viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight - screenMargin * 2;
  const int bottomReservation = std::max(screenMargin, UITheme::getInstance().getStatusBarHeight());
  const int viewportHeight = renderer.getScreenHeight() - viewportTop - marginBottom - bottomReservation;
  const int lineHeight = renderer.getLineHeight(readerFontId);
  linesPerPage = std::max(1, viewportHeight / std::max(1, lineHeight));

  if (renderer.isSdCardFont(readerFontId)) {
    renderer.ensureSdCardFontReady(readerFontId, chapterText.get(), /*styleMask=*/0x01);
    renderer.ensureSdCardFontReady(readerFontId, "0123456789", /*styleMask=*/0x02);
  }

  pageOffsets.push_back(0);
  size_t offset = 0;
  int pageLines = 0;
  while (offset < chapterTextLength && pageOffsets.size() < bible::MAX_PAGE_COUNT) {
    VisualLine line{};
    if (!nextVisualLine(offset, line) || line.next <= offset) break;
    offset = line.next;
    if (++pageLines >= linesPerPage && offset < chapterTextLength) {
      pageOffsets.push_back(offset);
      pageLines = 0;
    }
  }
  totalPages = static_cast<int>(pageOffsets.size());
  LOG_DBG(LOG_TAG, "Loaded %s %u: %u bytes, %d pages", currentBook() ? currentBook()->id : "?", currentChapter(),
          static_cast<unsigned>(chapterTextLength), totalPages);
}

bool BibleActivity::copyVisualLine(const VisualLine& line) {
  if (!chapterText || line.length >= LINE_BUFFER_SIZE || line.start + line.length > chapterTextLength) return false;
  memcpy(lineBuffer, chapterText.get() + line.start, line.length);
  lineBuffer[line.length] = '\0';
  return true;
}

void BibleActivity::render(RenderLock&&) {
  switch (view) {
    case View::Home:
      renderHome();
      break;
    case View::Chapters:
      renderChapters();
      break;
    case View::Reader:
      renderReader();
      break;
  }
}

void BibleActivity::renderHome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto* version = currentVersion();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = std::max(1, contentBottom - contentTop);
  const int verseHeight = contentHeight * 7 / 10;
  const int selectorTop = contentTop + verseHeight;
  const int selectorHeaderTop = selectorTop + metrics.verticalSpacing;
  const int bookTop = selectorHeaderTop + metrics.headerHeight;
  const Rect bookBounds{metrics.contentSidePadding, bookTop, pageWidth - metrics.contentSidePadding * 2,
                        std::max(1, contentBottom - bookTop)};
  const auto* book = currentBook();

  const int bookCount = static_cast<int>(books.size());
  const int columns = bookCount > 0 ? std::min(BOOK_GRID_COLUMNS, bookCount) : 0;
  const int rows = columns > 0 ? (bookCount + columns - 1) / columns : 0;
  const bool selectionOnly = !homeFullRenderPending && renderedHomeBookIndex != bookIndex &&
                             renderedHomeBookIndex >= 0 && renderedHomeBookIndex < bookCount && bookIndex >= 0 &&
                             bookIndex < bookCount;
  homeFullRenderPending = false;

  if (selectionOnly) {
    renderer.fillRect(0, selectorHeaderTop, pageWidth, metrics.headerHeight, false);
    GUI.drawSubHeader(renderer, Rect{0, selectorHeaderTop, pageWidth, metrics.headerHeight},
                      book ? book->name : tr(STR_BOOK), version ? version->id : nullptr);
    drawHomeBookCell(bookBounds, columns, rows, renderedHomeBookIndex, true);
    if (bookIndex != renderedHomeBookIndex) drawHomeBookCell(bookBounds, columns, rows, bookIndex, true);
    renderedHomeBookIndex = bookIndex;
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BIBLE),
                 version ? version->displayName : nullptr);
  GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.headerHeight}, tr(STR_BIBLE_DAILY_VERSE));

  const int verseTextTop = contentTop + metrics.headerHeight;
  const int verseFooterHeight =
      renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const Rect verseBounds{metrics.contentSidePadding, verseTextTop, pageWidth - metrics.contentSidePadding * 2,
                         std::max(1, verseHeight - metrics.headerHeight - verseFooterHeight)};
  if (dailyVerse.valid) {
    UITheme::drawCenteredWrappedText(renderer, verseBounds, UI_12_FONT_ID, dailyVerse.text,
                                     std::max(1, verseBounds.height / renderer.getLineHeight(UI_12_FONT_ID)), true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::CENTER);
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_12_FONT_ID,
                              contentTop + verseHeight - verseFooterHeight, dailyVerse.reference, true,
                              EpdFontFamily::BOLD);
    snprintf(lineBuffer, sizeof(lineBuffer), "%s (%s)", dailyVerse.translationName, dailyVerse.translationAbbreviation);
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, SMALL_FONT_ID,
                              contentTop + verseHeight - renderer.getLineHeight(SMALL_FONT_ID), lineBuffer);
    drawMemorisationGauge(pageWidth, contentTop + verseHeight - renderer.getLineHeight(SMALL_FONT_ID));
  } else {
    UITheme::drawCenteredWrappedText(renderer, verseBounds, UI_12_FONT_ID, tr(STR_BIBLE_DAILY_VERSE_UNAVAILABLE), 3);
  }

  renderer.drawLine(metrics.contentSidePadding, selectorTop, pageWidth - metrics.contentSidePadding, selectorTop);
  GUI.drawSubHeader(renderer, Rect{0, selectorHeaderTop, pageWidth, metrics.headerHeight},
                    book ? book->name : tr(STR_BOOK), version && homeMode == HomeMode::Books ? version->id : nullptr);
  if (version && homeMode == HomeMode::Versions) {
    const int badgePadding = std::max(2, metrics.verticalSpacing / 4);
    const int badgeTextWidth = renderer.getTextWidth(SMALL_FONT_ID, version->id);
    const int badgeTextHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int badgeWidth = badgeTextWidth + badgePadding * 2;
    const int badgeHeight = badgeTextHeight + badgePadding * 2;
    const int badgeX = pageWidth - metrics.contentSidePadding - badgeWidth;
    const int badgeY = selectorHeaderTop + std::max(0, (metrics.headerHeight - badgeHeight) / 2);
    renderer.fillRect(badgeX, badgeY, badgeWidth, badgeHeight, true);
    renderer.drawText(SMALL_FONT_ID, badgeX + badgePadding, badgeY + badgePadding, version->id, false);
  }

  if (book) {
    for (int index = 0; index < bookCount; ++index) {
      drawHomeBookCell(bookBounds, columns, rows, index, false);
    }
  } else {
    const char* message = versions.empty() ? tr(STR_BIBLE_NO_VERSIONS) : tr(STR_BIBLE_NO_BOOKS);
    UITheme::drawCenteredWrappedText(renderer, bookBounds, UI_12_FONT_ID, message, 3);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderedHomeBookIndex = bookIndex;
  renderer.displayBuffer();
}

void BibleActivity::drawHomeBookCell(const Rect& bounds, const int columns, const int rows, const int index,
                                     const bool eraseFirst) {
  if (columns <= 0 || rows <= 0 || index < 0 || index >= static_cast<int>(books.size())) return;

  const int cellWidth = std::max(1, bounds.width / columns);
  const int cellHeight = std::max(1, bounds.height / rows);
  const int column = index % columns;
  const int row = index / columns;
  const int cellX = bounds.x + column * cellWidth;
  const int cellY = bounds.y + row * cellHeight;
  const int cellRight = column + 1 == columns ? bounds.x + bounds.width : cellX + cellWidth;
  const int cellBottom = row + 1 == rows ? bounds.y + bounds.height : cellY + cellHeight;
  const int width = cellRight - cellX;
  const int height = cellBottom - cellY;
  const bool selected = index == bookIndex;

  if (eraseFirst) renderer.fillRect(cellX, cellY, width, height, false);
  if (selected) {
    renderer.fillRect(cellX, cellY, width, height, true);
  } else {
    renderer.drawRect(cellX, cellY, width, height, true);
  }

  char label[5]{};
  snprintf(label, sizeof(label), "%.4s", books[index].id);
  const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, cellX + std::max(0, (width - labelWidth) / 2),
                    cellY + std::max(0, (height - labelHeight) / 2), label, !selected);
}

void BibleActivity::drawMemorisationGauge(const int pageWidth, const int y) {
  if (dailyVerse.memorisationScale == 0) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int SEGMENT_WIDTH = 5;
  constexpr int SEGMENT_HEIGHT = 4;
  constexpr int SEGMENT_GAP = 2;
  constexpr uint8_t MAX_SEGMENTS = 8;
  const uint8_t scale = std::min(dailyVerse.memorisationScale, MAX_SEGMENTS);
  const uint8_t level = std::min(dailyVerse.memorisationLevel, scale);
  const int width = scale * SEGMENT_WIDTH + (scale - 1) * SEGMENT_GAP;
  const int x = pageWidth - metrics.contentSidePadding - width;
  const int top = y + std::max(0, (renderer.getLineHeight(SMALL_FONT_ID) - SEGMENT_HEIGHT) / 2);
  for (uint8_t segment = 0; segment < scale; ++segment) {
    const int segmentX = x + segment * (SEGMENT_WIDTH + SEGMENT_GAP);
    if (segment < level) {
      renderer.fillRect(segmentX, top, SEGMENT_WIDTH, SEGMENT_HEIGHT, true);
    } else {
      renderer.drawRect(segmentX, top, SEGMENT_WIDTH, SEGMENT_HEIGHT, true);
    }
  }
}

void BibleActivity::renderChapters() {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto* version = currentVersion();
  const auto* book = currentBook();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SELECT_CHAPTER),
                 version ? version->displayName : nullptr);

  const int subHeaderTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawSubHeader(renderer, Rect{0, subHeaderTop, pageWidth, metrics.headerHeight},
                    book ? book->name : tr(STR_BIBLE_NO_BOOKS), tr(STR_BIBLE_CHAPTER_NAV_HINT));
  const int contentTop = subHeaderTop + metrics.headerHeight;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (chapters.empty()) {
    UITheme::drawCenteredWrappedText(
        renderer,
        Rect{metrics.contentSidePadding, contentTop, pageWidth - metrics.contentSidePadding * 2, contentHeight},
        UI_12_FONT_ID, tr(STR_NO_CHAPTERS), 3);
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(chapters.size()),
                 chapterIndex, [this](const int index) {
                   char label[48];
                   snprintf(label, sizeof(label), "%s %u", tr(STR_CHAPTER), chapters[index]);
                   return std::string(label);
                 });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BibleActivity::drawWrappedNoteText(const Rect& bounds, const char* text) {
  if (!text || bounds.width <= 0 || bounds.height <= 0) return;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  int y = bounds.y;
  size_t offset = 0;
  const size_t textLength = strlen(text);
  while (offset < textLength && y + lineHeight <= bounds.y + bounds.height) {
    while (offset < textLength && (text[offset] == ' ' || text[offset] == '\n' || text[offset] == '\r')) ++offset;
    if (offset >= textLength) break;
    size_t paragraphEnd = offset;
    while (paragraphEnd < textLength && text[paragraphEnd] != '\n' && text[paragraphEnd] != '\r') ++paragraphEnd;
    size_t end = std::min(paragraphEnd, offset + LINE_BUFFER_SIZE - 1);
    while (end > offset && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    while (end > offset) {
      const size_t length = end - offset;
      memcpy(lineBuffer, text + offset, length);
      lineBuffer[length] = '\0';
      if (renderer.getTextAdvanceX(UI_12_FONT_ID, lineBuffer, EpdFontFamily::REGULAR) <= bounds.width) break;
      --end;
      while (end > offset && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    }
    if (end == offset) end = std::min(textLength, offset + 1);
    if (end < paragraphEnd) {
      size_t wordEnd = end;
      while (wordEnd > offset && text[wordEnd - 1] != ' ') --wordEnd;
      if (wordEnd > offset) end = wordEnd - 1;
    }
    while (end > offset && text[end - 1] == ' ') --end;
    const size_t length = std::min(end - offset, LINE_BUFFER_SIZE - 1);
    memcpy(lineBuffer, text + offset, length);
    lineBuffer[length] = '\0';
    renderer.drawText(UI_12_FONT_ID, bounds.x, y, lineBuffer);
    y += lineHeight;
    offset = end;
    while (offset < paragraphEnd && text[offset] == ' ') ++offset;
    if (offset >= paragraphEnd) offset = paragraphEnd + (paragraphEnd < textLength ? 1 : 0);
  }
}

void BibleActivity::drawNotePopup() {
  if (selectedNoteIndex >= chapterNoteCount || !chapterText) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int sideMargin = std::max(metrics.popupMarginX, metrics.contentSidePadding);
  const int statusHeight = UITheme::getInstance().getStatusBarHeight();
  const int popupBottom = screenHeight - marginBottom - statusHeight - metrics.verticalSpacing;
  const int popupHeight = std::min(screenHeight / 2, std::max(110, screenHeight * 2 / 5));
  const int x = marginLeft + sideMargin;
  const int width = screenWidth - marginLeft - marginRight - sideMargin * 2;
  const int y = std::max(marginTop + metrics.verticalSpacing, popupBottom - popupHeight);
  const int height = popupBottom - y;
  const int frame = std::max(1, metrics.popupFrameThickness);
  const int radius = std::max(4, metrics.popupCornerRadius);
  renderer.fillRoundedRect(x, y, width, height, radius, Color::Black);
  renderer.fillRoundedRect(x + frame, y + frame, width - frame * 2, height - frame * 2, std::max(1, radius - frame),
                           Color::White);

  const auto& note = chapterNotes[selectedNoteIndex];
  const int padding = std::max(metrics.verticalSpacing, 8);
  snprintf(lineBuffer, sizeof(lineBuffer), "%s %u  •  %s %u", tr(STR_BIBLE_NOTES), note.number, tr(STR_BIBLE_VERSE),
           note.verse);
  renderer.drawText(UI_12_FONT_ID, x + padding, y + padding, lineBuffer);
  const int headerHeight = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawLine(x + padding, y + padding + headerHeight, x + width - padding, y + padding + headerHeight);
  const int textTop = y + padding + headerHeight + std::max(4, metrics.verticalSpacing / 2);
  drawWrappedNoteText(Rect{x + padding, textTop, width - padding * 2, y + height - padding - textTop},
                      chapterText.get() + note.textOffset);
}

void BibleActivity::renderReader() {
  renderer.clearScreen();
  if (readerLoadFailed || !chapterText || pageOffsets.empty()) {
    const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
    UITheme::drawCenteredWrappedText(renderer, screen, UI_12_FONT_ID, tr(STR_BIBLE_CHAPTER_UNAVAILABLE), 3);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;
  const int lineHeight = renderer.getLineHeight(readerFontId);
  GUI.drawHelpText(renderer,
                   Rect{viewportLeft, viewportTop - renderer.getLineHeight(SMALL_FONT_ID), viewportWidth,
                        renderer.getLineHeight(SMALL_FONT_ID)},
                   tr(STR_BIBLE_READER_HINT));

  auto drawPageLines = [this, lineHeight]() {
    size_t offset = pageOffsets[currentPage];
    int y = viewportTop;
    for (int lineIndex = 0; lineIndex < linesPerPage && offset < chapterTextLength; ++lineIndex) {
      VisualLine line{};
      if (!nextVisualLine(offset, line) || line.next <= offset) break;
      if (line.length > 0 && copyVisualLine(line)) drawVisualText(viewportLeft, y, lineBuffer);
      y += lineHeight;
      offset = line.next;
    }
  };

  auto* fontCacheManager = renderer.getFontCacheManager();
  auto prewarm = fontCacheManager->createPrewarmScope();
  drawPageLines();
  prewarm.endScanAndPrewarm();
  drawPageLines();

  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    const char* versionLabel =
        showingDailyApiText ? dailyVerse.translationAbbreviation : (currentVersion() ? currentVersion()->id : "");
    snprintf(lineBuffer, sizeof(lineBuffer), "%s %s %u", versionLabel,
             currentBook() ? currentBook()->name : tr(STR_BIBLE), currentChapter());
    title = lineBuffer;
  }
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0.0f;
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, std::move(title));
  if (readerNoteMode == ReaderNoteMode::Popup) drawNotePopup();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
}
