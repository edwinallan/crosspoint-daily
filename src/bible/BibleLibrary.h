#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bible {

inline constexpr char BIBLE_ROOT[] = "/.bibles";
inline constexpr char DAILY_API_BASE_URL[] = "https://my-daily.allan.ch/server";
inline constexpr char DAILY_VERSES_URL[] = "https://my-daily.allan.ch/server/verses";
inline constexpr char DAILY_VERSES_CACHE_PATH[] = "/.crosspoint/daily/verses.json";
inline constexpr char DAILY_VERSES_BACKUP_PATH[] = "/.crosspoint/daily/verses.backup.json";
inline constexpr char DAILY_VERSES_TEMP_PATH[] = "/.crosspoint/daily/verses.download.json";
inline constexpr char DAILY_VERSES_SELECTION_PATH[] = "/.crosspoint/daily/verses.selection";
inline constexpr char DAILY_VERSES_SELECTION_TEMP_PATH[] = "/.crosspoint/daily/verses.selection.tmp";
inline constexpr char DAILY_VERSES_FIXTURE_PATH[] = "/.bibles/verses.json";
inline constexpr char LEGACY_DAILY_VERSE_CACHE_PATH[] = "/.crosspoint/daily/verse.json";
inline constexpr char LEGACY_DAILY_VERSE_BACKUP_PATH[] = "/.crosspoint/daily/verse.backup.json";
inline constexpr char DAILY_VERSE_FIXTURE_PATH[] = "/.bibles/verse.json";

inline constexpr size_t MAX_VERSION_COUNT = 12;
inline constexpr size_t MAX_BOOK_COUNT = 80;
inline constexpr size_t MAX_CHAPTER_COUNT = 200;
inline constexpr size_t MAX_PAGE_COUNT = 512;
inline constexpr size_t MAX_CHAPTER_NOTE_COUNT = 200;
inline constexpr size_t MAX_DAILY_VERSE_COUNT = 128;
inline constexpr char VERSE_NUMBER_END = '\x1d';
inline constexpr char NOTE_MARKER_START = '\x1e';
inline constexpr char NOTE_MARKER_END = '\x1f';

struct VersionInfo {
  char directory[32]{};
  char id[16]{};
  char displayName[48]{};
  char language[8]{};
};

struct BookInfo {
  char id[12]{};
  char name[48]{};
  uint16_t order = 0;
  uint16_t chapterCount = 0;
};

struct DailyVerse {
  bool valid = false;
  bool isSticky = false;
  char date[11]{};
  char reference[96]{};
  char referenceBook[48]{};
  uint16_t referenceChapter = 0;
  uint16_t referenceVerseStart = 0;
  uint16_t referenceVerseEnd = 0;
  char translationAbbreviation[16]{};
  char translationName[96]{};
  char translationLanguage[8]{};
  char text[2048]{};
  uint8_t memorisationLevel = 0;
  uint8_t memorisationScale = 0;
  char personalNote[1024]{};
};

struct ChapterNote {
  uint16_t verse = 0;
  uint16_t number = 0;
  uint32_t markerOffset = 0;
  uint32_t textOffset = 0;
};

class BibleLibrary final {
 public:
  static bool discoverVersions(std::vector<VersionInfo>& versions);
  static bool loadBooks(const VersionInfo& version, std::vector<BookInfo>& books);
  static bool findBook(const VersionInfo& version, const char* bookId, BookInfo& book);
  static bool findBookByName(const VersionInfo& version, const char* bookName, BookInfo& book);
  static bool loadAvailableChapters(const VersionInfo& version, const BookInfo& book, std::vector<uint16_t>& chapters);
  static bool loadDailyVerse(DailyVerse& verse, uint16_t* selectedIndex = nullptr, uint16_t* verseCount = nullptr);
  static bool loadDailyVerseAt(uint16_t index, DailyVerse& verse, uint16_t* verseCount = nullptr);
  static bool refreshDailyVerse();

  // API excerpts are bounded by DailyVerse's fixed buffers. The returned
  // allocation is held for the reader view and released when that view exits.
  static bool loadDailyChapter(const DailyVerse& verse, const char* footnotesLabel, std::unique_ptr<char[]>& text,
                               size_t& textLength, size_t& textCapacity);

  // Note text shares the returned chapter allocation; notes only retain bounded
  // offsets into it, avoiding a second chapter-sized allocation.
  static bool loadChapter(const VersionInfo& version, const BookInfo& book, uint16_t chapter,
                          std::unique_ptr<char[]>& text, size_t& textLength, size_t& textCapacity, ChapterNote* notes,
                          size_t notesCapacity, size_t& noteCount);
};

}  // namespace bible
