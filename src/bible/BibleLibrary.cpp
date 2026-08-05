#include "BibleLibrary.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bible {
namespace {

constexpr char LOG_TAG[] = "BIBLE";
constexpr size_t MAX_MANIFEST_BYTES = 32 * 1024;
constexpr size_t MAX_DAILY_VERSE_BYTES = 8 * 1024;
constexpr size_t MAX_CHAPTER_FILE_BYTES = 32 * 1024;
constexpr size_t MAX_NOTES_FILE_BYTES = 16 * 1024;
constexpr size_t NOTES_LABEL_ALLOWANCE = 2048;
constexpr size_t MAX_PATH_LENGTH = 160;

bool copyExact(char* destination, const size_t destinationSize, const char* source) {
  if (!destination || destinationSize == 0 || !source || source[0] == '\0') return false;
  const size_t length = strlen(source);
  if (length >= destinationSize) return false;
  memcpy(destination, source, length + 1);
  return true;
}

const char* firstString(const JsonObjectConst object, const char* first, const char* second = nullptr,
                        const char* third = nullptr) {
  const char* value = object[first].as<const char*>();
  if ((!value || value[0] == '\0') && second) value = object[second].as<const char*>();
  if ((!value || value[0] == '\0') && third) value = object[third].as<const char*>();
  return value;
}

uint16_t firstUint16(const JsonObjectConst object, const char* first, const char* second = nullptr,
                     const char* third = nullptr) {
  if (!object[first].isNull()) return object[first].as<uint16_t>();
  if (second && !object[second].isNull()) return object[second].as<uint16_t>();
  if (third && !object[third].isNull()) return object[third].as<uint16_t>();
  return 0;
}

const char* baseName(char* path) {
  if (!path) return nullptr;
  size_t length = strlen(path);
  while (length > 0 && path[length - 1] == '/') path[--length] = '\0';
  char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

bool parseJsonFile(const char* path, const size_t maxBytes, JsonDocument& document) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  const size_t size = file.size();
  if (size == 0 || size > maxBytes) {
    LOG_ERR(LOG_TAG, "JSON file has unsupported size: %s (%u bytes)", path, static_cast<unsigned>(size));
    return false;
  }

  const DeserializationError error = deserializeJson(document, file);
  if (error) {
    LOG_ERR(LOG_TAG, "Invalid JSON in %s: %s", path, error.c_str());
    return false;
  }
  return true;
}

bool manifestPath(const VersionInfo& version, char* path, const size_t pathSize) {
  const int written = snprintf(path, pathSize, "%s/%s/manifest.json", BIBLE_ROOT, version.directory);
  return written > 0 && static_cast<size_t>(written) < pathSize;
}

bool parseVersion(const char* directory, VersionInfo& version) {
  VersionInfo candidate{};
  if (!copyExact(candidate.directory, sizeof(candidate.directory), directory)) return false;

  char path[MAX_PATH_LENGTH];
  if (!manifestPath(candidate, path, sizeof(path))) return false;

  JsonDocument document;
  if (!parseJsonFile(path, MAX_MANIFEST_BYTES, document)) return false;
  const JsonObjectConst root = document.as<JsonObjectConst>();
  if (root.isNull() || !root["books"].is<JsonArrayConst>()) return false;

  const char* id = firstString(root, "id", "versionId", "version");
  if (!id || id[0] == '\0') id = root["abbreviation"].as<const char*>();

  const char* name = firstString(root, "name", "displayName", "display_name");

  const char* language = root["language"].as<const char*>();
  if (!language || language[0] == '\0') {
    const JsonObjectConst languageObject = root["language"].as<JsonObjectConst>();
    language = firstString(languageObject, "iso_639_1", "tag", "iso_639_3");
  }
  if (!copyExact(candidate.id, sizeof(candidate.id), id) ||
      !copyExact(candidate.displayName, sizeof(candidate.displayName), name) ||
      !copyExact(candidate.language, sizeof(candidate.language), language)) {
    LOG_ERR(LOG_TAG, "Manifest metadata is missing or too long: %s", path);
    return false;
  }

  version = candidate;
  return true;
}

bool parseBook(const JsonObjectConst object, BookInfo& book) {
  BookInfo candidate{};
  const char* id = firstString(object, "id", "bookId");
  const char* name = firstString(object, "name", "displayName");
  candidate.order = firstUint16(object, "order", "bookOrder");
  candidate.chapterCount = firstUint16(object, "chapters", "chapterCount", "chapter_count");
  if (!copyExact(candidate.id, sizeof(candidate.id), id) || !copyExact(candidate.name, sizeof(candidate.name), name) ||
      candidate.order == 0 || candidate.chapterCount == 0 || candidate.chapterCount > MAX_CHAPTER_COUNT) {
    return false;
  }
  book = candidate;
  return true;
}

bool endsWithTxt(const char* name) {
  const size_t length = name ? strlen(name) : 0;
  if (length < 5) return false;
  const char* extension = name + length - 4;
  return extension[0] == '.' && std::tolower(static_cast<unsigned char>(extension[1])) == 't' &&
         std::tolower(static_cast<unsigned char>(extension[2])) == 'x' &&
         std::tolower(static_cast<unsigned char>(extension[3])) == 't';
}

uint16_t noteVerseNumber(const JsonVariantConst value) {
  if (value.is<uint16_t>()) return value.as<uint16_t>();
  const char* text = value.as<const char*>();
  if (!text || text[0] == '\0') return 0;
  char* end = nullptr;
  const long number = strtol(text, &end, 10);
  return end && end != text && *end == '\0' && number > 0 && number <= UINT16_MAX ? static_cast<uint16_t>(number) : 0;
}

bool findVerseMarkerOffset(const char* buffer, const size_t length, const uint16_t verse, size_t& offset) {
  size_t lineStart = 0;
  while (lineStart < length) {
    char* end = nullptr;
    const long number = strtol(buffer + lineStart, &end, 10);
    if (end != buffer + lineStart && number == verse && (*end == ' ' || *end == NOTE_MARKER_START)) {
      offset = static_cast<size_t>(end - buffer);
      while (offset < length && buffer[offset] == NOTE_MARKER_START) {
        while (offset < length && buffer[offset] != NOTE_MARKER_END) ++offset;
        if (offset < length) ++offset;
      }
      return true;
    }
    const char* newline = static_cast<const char*>(memchr(buffer + lineStart, '\n', length - lineStart));
    if (!newline) break;
    lineStart = static_cast<size_t>(newline - buffer) + 1;
  }
  return false;
}

void loadNotes(const char* path, const BookInfo& book, const uint16_t chapter, char* buffer, size_t& length,
               const size_t capacity, ChapterNote* output, const size_t outputCapacity, size_t& outputCount) {
  outputCount = 0;
  if (!output || outputCapacity == 0) return;
  JsonDocument document;
  if (!parseJsonFile(path, MAX_NOTES_FILE_BYTES, document)) return;

  const JsonObjectConst root = document.as<JsonObjectConst>();
  const char* bookId = root["book_id"].as<const char*>();
  const JsonArrayConst notes = root["notes"].as<JsonArrayConst>();
  if (root.isNull() || !bookId || strcmp(bookId, book.id) != 0 || root["chapter"].as<uint16_t>() != chapter ||
      notes.isNull()) {
    LOG_ERR(LOG_TAG, "Ignoring mismatched notes file: %s", path);
    return;
  }

  size_t textTail = capacity;
  uint16_t sourceNumber = 0;
  for (const JsonObjectConst note : notes) {
    if (++sourceNumber > MAX_CHAPTER_NOTE_COUNT || outputCount >= outputCapacity) break;
    const char* noteText = note["text"].as<const char*>();
    if (!noteText || noteText[0] == '\0') continue;
    const uint16_t verse = noteVerseNumber(note["verse"]);
    const size_t noteLength = strlen(noteText);
    if (verse == 0 || noteLength + 1 >= textTail || textTail - noteLength - 1 <= length) continue;
    textTail -= noteLength + 1;
    memcpy(buffer + textTail, noteText, noteLength + 1);
    output[outputCount++] = ChapterNote{verse, sourceNumber, 0, static_cast<uint32_t>(textTail)};
  }

  std::sort(output, output + outputCount, [](const ChapterNote& left, const ChapterNote& right) {
    if (left.verse != right.verse) return left.verse < right.verse;
    return left.number < right.number;
  });

  size_t validCount = 0;
  for (size_t i = 0; i < outputCount; ++i) {
    size_t markerOffset = 0;
    if (!findVerseMarkerOffset(buffer, length, output[i].verse, markerOffset)) continue;
    char marker[8]{};
    const int markerLength = snprintf(marker, sizeof(marker), "%c%u%c", NOTE_MARKER_START, output[i].number,
                                      NOTE_MARKER_END);
    if (markerLength <= 0 || length + static_cast<size_t>(markerLength) + 1 > textTail) {
      LOG_ERR(LOG_TAG, "Note markers exceed chapter buffer: %s", path);
      break;
    }
    memmove(buffer + markerOffset + markerLength, buffer + markerOffset, length - markerOffset + 1);
    memcpy(buffer + markerOffset, marker, markerLength);
    length += static_cast<size_t>(markerLength);
    output[i].markerOffset = static_cast<uint32_t>(markerOffset);
    output[validCount++] = output[i];
  }
  outputCount = validCount;
}

bool loadDailyVerseFile(const char* path, DailyVerse& verse) {
  JsonDocument document;
  if (!parseJsonFile(path, MAX_DAILY_VERSE_BYTES, document)) return false;

  const JsonObjectConst root = document.as<JsonObjectConst>();
  const JsonObjectConst verseObject = root["verse"].as<JsonObjectConst>();
  const JsonObjectConst translation = verseObject["translation"].as<JsonObjectConst>();
  if (!root["success"].as<bool>() || verseObject.isNull() || translation.isNull()) return false;

  DailyVerse candidate{};
  const char* date = root["date"].as<const char*>();
  const char* reference = verseObject["reference"].as<const char*>();
  const char* abbreviation = translation["abbreviation"].as<const char*>();
  const char* name = translation["name"].as<const char*>();
  const char* language = translation["language"].as<const char*>();
  const char* text = verseObject["text"].as<const char*>();

  if (!copyExact(candidate.date, sizeof(candidate.date), date) ||
      !copyExact(candidate.reference, sizeof(candidate.reference), reference) ||
      !copyExact(candidate.translationAbbreviation, sizeof(candidate.translationAbbreviation), abbreviation) ||
      !copyExact(candidate.translationName, sizeof(candidate.translationName), name) ||
      !copyExact(candidate.translationLanguage, sizeof(candidate.translationLanguage), language) ||
      !copyExact(candidate.text, sizeof(candidate.text), text)) {
    LOG_ERR(LOG_TAG, "Daily verse fields are missing or too long: %s", path);
    return false;
  }

  candidate.valid = true;
  verse = candidate;
  return true;
}

}  // namespace

bool BibleLibrary::discoverVersions(std::vector<VersionInfo>& versions) {
  versions.clear();
  versions.reserve(MAX_VERSION_COUNT);

  HalFile root = Storage.open(BIBLE_ROOT);
  if (!root || !root.isDirectory()) {
    LOG_ERR(LOG_TAG, "Bible directory not found: %s", BIBLE_ROOT);
    return false;
  }

  while (versions.size() < MAX_VERSION_COUNT) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) continue;

    char entryName[MAX_PATH_LENGTH]{};
    if (entry.getName(entryName, sizeof(entryName)) == 0) continue;
    const char* directory = baseName(entryName);
    if (!directory || directory[0] == '\0' || directory[0] == '.') continue;

    VersionInfo version{};
    if (parseVersion(directory, version)) versions.push_back(version);
  }

  std::sort(versions.begin(), versions.end(), [](const VersionInfo& left, const VersionInfo& right) {
    return strcmp(left.displayName, right.displayName) < 0;
  });
  return !versions.empty();
}

bool BibleLibrary::loadBooks(const VersionInfo& version, std::vector<BookInfo>& books) {
  books.clear();
  books.reserve(MAX_BOOK_COUNT);

  char path[MAX_PATH_LENGTH];
  if (!manifestPath(version, path, sizeof(path))) return false;

  JsonDocument document;
  if (!parseJsonFile(path, MAX_MANIFEST_BYTES, document)) return false;
  const JsonArrayConst bookArray = document["books"].as<JsonArrayConst>();
  if (bookArray.isNull()) return false;

  for (const JsonObjectConst object : bookArray) {
    if (books.size() >= MAX_BOOK_COUNT) break;
    BookInfo book{};
    if (!parseBook(object, book)) {
      LOG_ERR(LOG_TAG, "Ignoring invalid book in %s", path);
      continue;
    }
    books.push_back(book);
  }

  std::sort(books.begin(), books.end(), [](const BookInfo& left, const BookInfo& right) {
    if (left.order != right.order) return left.order < right.order;
    return strcmp(left.name, right.name) < 0;
  });
  return !books.empty();
}

bool BibleLibrary::findBook(const VersionInfo& version, const char* bookId, BookInfo& book) {
  if (!bookId || bookId[0] == '\0') return false;
  char path[MAX_PATH_LENGTH];
  if (!manifestPath(version, path, sizeof(path))) return false;

  JsonDocument document;
  if (!parseJsonFile(path, MAX_MANIFEST_BYTES, document)) return false;
  const JsonArrayConst bookArray = document["books"].as<JsonArrayConst>();
  for (const JsonObjectConst object : bookArray) {
    const char* id = firstString(object, "id", "bookId");
    if (id && strcmp(id, bookId) == 0) return parseBook(object, book);
  }
  return false;
}

bool BibleLibrary::loadAvailableChapters(const VersionInfo& version, const BookInfo& book,
                                         std::vector<uint16_t>& chapters) {
  chapters.clear();
  chapters.reserve(std::min(static_cast<size_t>(book.chapterCount), MAX_CHAPTER_COUNT));

  char path[MAX_PATH_LENGTH];
  const int written = snprintf(path, sizeof(path), "%s/%s/%s", BIBLE_ROOT, version.directory, book.id);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(path)) return false;

  HalFile directory = Storage.open(path);
  if (!directory || !directory.isDirectory()) return false;

  while (chapters.size() < MAX_CHAPTER_COUNT) {
    HalFile entry = directory.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) continue;

    char entryName[MAX_PATH_LENGTH]{};
    if (entry.getName(entryName, sizeof(entryName)) == 0) continue;
    char* filename = const_cast<char*>(baseName(entryName));
    if (!filename || !endsWithTxt(filename)) continue;

    char* extension = strrchr(filename, '.');
    if (!extension) continue;
    *extension = '\0';
    char* parseEnd = nullptr;
    const long number = strtol(filename, &parseEnd, 10);
    if (!parseEnd || parseEnd == filename || *parseEnd != '\0' || number <= 0 || number > book.chapterCount) continue;
    chapters.push_back(static_cast<uint16_t>(number));
  }

  std::sort(chapters.begin(), chapters.end());
  chapters.erase(std::unique(chapters.begin(), chapters.end()), chapters.end());
  return !chapters.empty();
}

bool BibleLibrary::loadDailyVerse(DailyVerse& verse) {
  verse = DailyVerse{};
  if (Storage.exists(DAILY_VERSE_CACHE_PATH) && loadDailyVerseFile(DAILY_VERSE_CACHE_PATH, verse)) return true;
  if (Storage.exists(DAILY_VERSE_FIXTURE_PATH) && loadDailyVerseFile(DAILY_VERSE_FIXTURE_PATH, verse)) return true;
  return false;
}

bool BibleLibrary::loadChapter(const VersionInfo& version, const BookInfo& book, const uint16_t chapter,
                               std::unique_ptr<char[]>& text, size_t& textLength, size_t& textCapacity,
                               ChapterNote* notes, const size_t notesCapacity, size_t& noteCount) {
  text.reset();
  textLength = 0;
  textCapacity = 0;
  noteCount = 0;

  char chapterPath[MAX_PATH_LENGTH];
  char notesPath[MAX_PATH_LENGTH];
  const int chapterWritten = snprintf(chapterPath, sizeof(chapterPath), "%s/%s/%s/%03u.txt", BIBLE_ROOT,
                                      version.directory, book.id, chapter);
  const int notesWritten = snprintf(notesPath, sizeof(notesPath), "%s/%s/%s/%03u.notes.json", BIBLE_ROOT,
                                    version.directory, book.id, chapter);
  if (chapterWritten <= 0 || static_cast<size_t>(chapterWritten) >= sizeof(chapterPath) || notesWritten <= 0 ||
      static_cast<size_t>(notesWritten) >= sizeof(notesPath)) {
    return false;
  }

  HalFile chapterFile;
  if (!Storage.openFileForRead(LOG_TAG, chapterPath, chapterFile)) return false;
  const size_t chapterSize = chapterFile.size();
  if (chapterSize == 0 || chapterSize > MAX_CHAPTER_FILE_BYTES) {
    LOG_ERR(LOG_TAG, "Chapter has unsupported size: %s (%u bytes)", chapterPath,
            static_cast<unsigned>(chapterSize));
    return false;
  }

  size_t notesSize = 0;
  if (Storage.exists(notesPath)) {
    HalFile notesFile;
    if (Storage.openFileForRead(LOG_TAG, notesPath, notesFile)) {
      const size_t candidateSize = notesFile.size();
      if (candidateSize > 0 && candidateSize <= MAX_NOTES_FILE_BYTES) {
        notesSize = candidateSize;
      } else {
        LOG_ERR(LOG_TAG, "Ignoring oversized notes file: %s", notesPath);
      }
    }
  }

  // A chapter can exceed the task stack, and its size is only known from the
  // SD file. Allocate exactly once for the readable view, using nothrow, then
  // reuse it for every page until the view is left.
  const size_t capacity = chapterSize + notesSize + NOTES_LABEL_ALLOWANCE + 1;
  auto chapterText = makeUniqueNoThrow<char[]>(capacity);
  if (!chapterText) {
    LOG_ERR(LOG_TAG, "OOM: chapter buffer (%u bytes)", static_cast<unsigned>(capacity));
    return false;
  }

  const int bytesRead = chapterFile.read(chapterText.get(), chapterSize);
  if (bytesRead != static_cast<int>(chapterSize)) {
    LOG_ERR(LOG_TAG, "Failed to read complete chapter: %s", chapterPath);
    return false;
  }

  // Normalize CRLF and turn the manifest's TAB separator into a printable
  // space without allocating a second copy of the chapter.
  size_t normalizedLength = 0;
  for (size_t i = 0; i < chapterSize; ++i) {
    char value = chapterText[i];
    if (value == '\r') continue;
    if (value == '\t') value = ' ';
    chapterText[normalizedLength++] = value;
  }
  chapterText[normalizedLength] = '\0';

  if (notesSize > 0) {
    loadNotes(notesPath, book, chapter, chapterText.get(), normalizedLength, capacity, notes, notesCapacity, noteCount);
  }

  text = std::move(chapterText);
  textLength = normalizedLength;
  textCapacity = capacity;
  return true;
}

}  // namespace bible
