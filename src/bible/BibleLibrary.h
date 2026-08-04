#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bible {

inline constexpr char BIBLE_ROOT[] = "/bibles";
inline constexpr char DAILY_VERSE_CACHE_PATH[] = "/.crosspoint/daily/verse.json";
inline constexpr char DAILY_VERSE_FIXTURE_PATH[] = "/bibles/verse.json";

inline constexpr size_t MAX_VERSION_COUNT = 12;
inline constexpr size_t MAX_BOOK_COUNT = 80;
inline constexpr size_t MAX_CHAPTER_COUNT = 200;
inline constexpr size_t MAX_PAGE_COUNT = 512;

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
  char date[11]{};
  char reference[96]{};
  char translationAbbreviation[16]{};
  char translationName[96]{};
  char translationLanguage[8]{};
  char text[2048]{};
};

class BibleLibrary final {
 public:
  static bool discoverVersions(std::vector<VersionInfo>& versions);
  static bool loadBooks(const VersionInfo& version, std::vector<BookInfo>& books);
  static bool findBook(const VersionInfo& version, const char* bookId, BookInfo& book);
  static bool loadAvailableChapters(const VersionInfo& version, const BookInfo& book,
                                    std::vector<uint16_t>& chapters);
  static bool loadDailyVerse(DailyVerse& verse);

  // The returned allocation is exactly the chapter file size plus the bounded
  // notes file and a small label allowance. It is owned by the activity and is
  // released whenever the readable chapter view is left.
  static bool loadChapter(const VersionInfo& version, const BookInfo& book, uint16_t chapter,
                          std::unique_ptr<char[]>& text, size_t& textLength, size_t& textCapacity, bool& hasNotes);
};

}  // namespace bible
