#include "BibleActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <I18nKeys.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "activities/reader/ReaderUtils.h"
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

  versions.reserve(bible::MAX_VERSION_COUNT);
  books.reserve(bible::MAX_BOOK_COUNT);
  chapters.reserve(bible::MAX_CHAPTER_COUNT);
  pageOffsets.reserve(bible::MAX_PAGE_COUNT);

  bible::BibleLibrary::loadDailyVerse(dailyVerse);
  if (bible::BibleLibrary::discoverVersions(versions)) {
    const char* uiLanguage = LANGUAGE_CODES[static_cast<size_t>(I18N.getLanguage())];
    for (size_t i = 0; i < versions.size(); ++i) {
      if (equalsIgnoreCase(versions[i].language, uiLanguage)) {
        versionIndex = static_cast<int>(i);
        break;
      }
    }
    bible::BibleLibrary::loadBooks(versions[versionIndex], books);
  }

  requestUpdate();
}

void BibleActivity::onExit() {
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
    activityManager.goHome(HomeMenuItem::BIBLE);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!books.empty()) enterChapters();
    return;
  }

  if (!books.empty() && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    bookIndex = ButtonNavigator::previousIndex(bookIndex, static_cast<int>(books.size()));
    requestUpdate();
    return;
  }
  if (!books.empty() && mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    bookIndex = ButtonNavigator::nextIndex(bookIndex, static_cast<int>(books.size()));
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    switchVersion(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    switchVersion(1);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (!books.empty() && swipe == MappedInputManager::SwipeDir::Left) {
    bookIndex = ButtonNavigator::nextIndex(bookIndex, static_cast<int>(books.size()));
    requestUpdate();
    return;
  }
  if (!books.empty() && swipe == MappedInputManager::SwipeDir::Right) {
    bookIndex = ButtonNavigator::previousIndex(bookIndex, static_cast<int>(books.size()));
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Up) {
    switchVersion(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    switchVersion(-1);
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int selectorTop = renderer.getScreenHeight() * 7 / 10;
  if (!books.empty() && mappedInput.wasTapInRect(0, selectorTop, renderer.getScreenWidth(),
                                                 renderer.getScreenHeight() - selectorTop - metrics.buttonHintsHeight)) {
    enterChapters();
  }
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

  if (!chapters.empty() && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    chapterIndex = ButtonNavigator::previousIndex(chapterIndex, static_cast<int>(chapters.size()));
    requestUpdate();
    return;
  }
  if (!chapters.empty() && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    chapterIndex = ButtonNavigator::nextIndex(chapterIndex, static_cast<int>(chapters.size()));
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    switchVersion(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    switchVersion(1);
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.headerHeight;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
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

void BibleActivity::handleReaderInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    enterChapters();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switchVersion(1);
    return;
  }

  const bool swapFront = mappedInput.isNavDirectionSwapped();
  const auto previousChapterButton =
      swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextChapterButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  if (mappedInput.wasReleased(previousChapterButton)) {
    changeReaderChapter(-1);
    return;
  }
  if (mappedInput.wasReleased(nextChapterButton)) {
    changeReaderChapter(1);
    return;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  const bool previousPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) || touch.prev;
  const bool nextPage = mappedInput.wasReleased(MappedInputManager::Button::PageForward) || touch.next;
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
  }
  requestUpdate();
}

void BibleActivity::enterChapters() {
  {
    RenderLock lock(*this);
    const uint16_t preferredChapter = currentChapter();
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
  chapterHasNotes = false;
  readerLoadFailed = false;
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
  if (changed) requestUpdate();
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

  if (!bible::BibleLibrary::loadChapter(*version, *book, chapter, chapterText, chapterTextLength,
                                        chapterTextCapacity, chapterHasNotes)) {
    readerLoadFailed = true;
    return false;
  }

  buildPageIndex();
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
    return renderer.getTextAdvanceX(readerFontId, lineBuffer, EpdFontFamily::REGULAR) <= viewportWidth;
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
  const int viewportHeight =
      renderer.getScreenHeight() - viewportTop - marginBottom - bottomReservation;
  const int lineHeight = renderer.getLineHeight(readerFontId);
  linesPerPage = std::max(1, viewportHeight / std::max(1, lineHeight));

  if (renderer.isSdCardFont(readerFontId)) {
    renderer.ensureSdCardFontReady(readerFontId, chapterText.get(), /*styleMask=*/0x01);
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
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto* version = currentVersion();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BIBLE),
                 version ? version->displayName : nullptr);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = std::max(1, contentBottom - contentTop);
  const int verseHeight = contentHeight * 7 / 10;
  GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.headerHeight}, tr(STR_BIBLE_DAILY_VERSE));

  const int verseTextTop = contentTop + metrics.headerHeight;
  const int verseFooterHeight = renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) +
                                metrics.verticalSpacing;
  const Rect verseBounds{metrics.contentSidePadding, verseTextTop,
                         pageWidth - metrics.contentSidePadding * 2,
                         std::max(1, verseHeight - metrics.headerHeight - verseFooterHeight)};
  if (dailyVerse.valid) {
    UITheme::drawCenteredWrappedText(renderer, verseBounds, UI_12_FONT_ID, dailyVerse.text,
                                     std::max(1, verseBounds.height / renderer.getLineHeight(UI_12_FONT_ID)), true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::CENTER);
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, UI_12_FONT_ID,
                              contentTop + verseHeight - verseFooterHeight, dailyVerse.reference, true,
                              EpdFontFamily::BOLD);
    snprintf(lineBuffer, sizeof(lineBuffer), "%s (%s)", dailyVerse.translationName,
             dailyVerse.translationAbbreviation);
    UITheme::drawCenteredText(renderer, Rect{0, 0, pageWidth, pageHeight}, SMALL_FONT_ID,
                              contentTop + verseHeight - renderer.getLineHeight(SMALL_FONT_ID), lineBuffer);
  } else {
    UITheme::drawCenteredWrappedText(renderer, verseBounds, UI_12_FONT_ID, tr(STR_BIBLE_DAILY_VERSE_UNAVAILABLE), 3);
  }

  const int selectorTop = contentTop + verseHeight;
  renderer.drawLine(metrics.contentSidePadding, selectorTop, pageWidth - metrics.contentSidePadding, selectorTop);
  GUI.drawSubHeader(renderer, Rect{0, selectorTop + metrics.verticalSpacing, pageWidth, metrics.headerHeight},
                    tr(STR_BOOK), version ? version->id : nullptr);

  const auto* book = currentBook();
  const int bookTop = selectorTop + metrics.verticalSpacing + metrics.headerHeight;
  const Rect bookBounds{metrics.contentSidePadding, bookTop, pageWidth - metrics.contentSidePadding * 2,
                        std::max(1, contentBottom - bookTop)};
  if (book) {
    renderer.drawRect(bookBounds.x, bookBounds.y, bookBounds.width, bookBounds.height);
    UITheme::drawCenteredWrappedText(renderer, bookBounds, UI_12_FONT_ID, book->name, 2, true,
                                     EpdFontFamily::BOLD);
    GUI.drawHelpText(renderer, Rect{bookBounds.x, bookBounds.y + bookBounds.height - renderer.getLineHeight(SMALL_FONT_ID),
                                    bookBounds.width, renderer.getLineHeight(SMALL_FONT_ID)},
                     tr(STR_BIBLE_VERSION_UP_DOWN));
  } else {
    const char* message = versions.empty() ? tr(STR_BIBLE_NO_VERSIONS) : tr(STR_BIBLE_NO_BOOKS);
    UITheme::drawCenteredWrappedText(renderer, bookBounds, UI_12_FONT_ID, message, 3);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
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
                    book ? book->name : tr(STR_BIBLE_NO_BOOKS), tr(STR_BIBLE_CHAPTER_VERSION_HINT));
  const int contentTop = subHeaderTop + metrics.headerHeight;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (chapters.empty()) {
    UITheme::drawCenteredWrappedText(renderer, Rect{metrics.contentSidePadding, contentTop,
                                                     pageWidth - metrics.contentSidePadding * 2, contentHeight},
                                     UI_12_FONT_ID, tr(STR_NO_CHAPTERS), 3);
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(chapters.size()),
                 chapterIndex, [this](const int index) {
                   char label[48];
                   snprintf(label, sizeof(label), "%s %u", tr(STR_CHAPTER), chapters[index]);
                   return std::string(label);
                 });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
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
  GUI.drawHelpText(renderer, Rect{viewportLeft, viewportTop - renderer.getLineHeight(SMALL_FONT_ID), viewportWidth,
                                  renderer.getLineHeight(SMALL_FONT_ID)},
                   tr(STR_BIBLE_READER_HINT));

  auto drawPageLines = [this, lineHeight]() {
    size_t offset = pageOffsets[currentPage];
    int y = viewportTop;
    for (int lineIndex = 0; lineIndex < linesPerPage && offset < chapterTextLength; ++lineIndex) {
      VisualLine line{};
      if (!nextVisualLine(offset, line) || line.next <= offset) break;
      if (line.length > 0 && copyVisualLine(line)) renderer.drawText(readerFontId, viewportLeft, y, lineBuffer);
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
    snprintf(lineBuffer, sizeof(lineBuffer), "%s — %s %u", currentBook() ? currentBook()->name : tr(STR_BIBLE),
             tr(STR_CHAPTER), currentChapter());
    title = lineBuffer;
  }
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0.0f;
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, std::move(title));
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
}
