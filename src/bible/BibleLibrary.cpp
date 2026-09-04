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

#include "network/HttpDownloader.h"

namespace bible {
namespace {

constexpr char LOG_TAG[] = "BIBLE";
constexpr size_t MAX_MANIFEST_BYTES = 32 * 1024;
constexpr size_t MAX_DAILY_VERSE_BYTES = 8 * 1024;
constexpr size_t MAX_DAILY_VERSES_BYTES = 512 * 1024;
constexpr size_t MAX_DAILY_VERSE_OBJECT_BYTES = 24 * 1024;
constexpr size_t MAX_DAILY_VERSE_SELECTION_BYTES = 24 * 1024;
constexpr size_t MAX_CHAPTER_FILE_BYTES = 32 * 1024;
constexpr size_t MAX_NOTES_FILE_BYTES = 16 * 1024;
constexpr size_t NOTES_LABEL_ALLOWANCE = 2048;
constexpr size_t MAX_PATH_LENGTH = 160;
constexpr char DAILY_CACHE_DIRECTORY[] = "/.crosspoint/daily";
constexpr size_t JSON_SCAN_BUFFER_BYTES = 192;
constexpr uint32_t FNV1A_OFFSET_BASIS = 2166136261U;
constexpr uint32_t FNV1A_PRIME = 16777619U;
constexpr uint8_t DAILY_VERSE_SELECTION_SCHEMA_VERSION = 1;

struct DailyVersesInfo {
  uint32_t contentHash = FNV1A_OFFSET_BASIS;
  uint32_t arrayEnd = 0;
  uint16_t count = 0;
};

bool copyExact(char* destination, const size_t destinationSize, const char* source) {
  if (!destination || destinationSize == 0 || !source || source[0] == '\0') return false;
  const size_t length = strlen(source);
  if (length >= destinationSize) return false;
  memcpy(destination, source, length + 1);
  return true;
}

bool copyTruncatedUtf8(char* destination, const size_t destinationSize, const char* source) {
  if (!destination || destinationSize == 0 || !source || source[0] == '\0') return false;
  size_t length = std::min(strlen(source), destinationSize - 1);
  while (length > 0 && (static_cast<unsigned char>(source[length]) & 0xC0) == 0x80) --length;
  memcpy(destination, source, length);
  destination[length] = '\0';
  return length > 0;
}

void appendTruncated(char* destination, const size_t destinationSize, const char* source) {
  if (!destination || destinationSize == 0 || !source || source[0] == '\0') return;
  const size_t used = strlen(destination);
  if (used + 1 >= destinationSize) return;
  size_t length = std::min(strlen(source), destinationSize - used - 1);
  while (length > 0 && (static_cast<unsigned char>(source[length]) & 0xC0) == 0x80) --length;
  memcpy(destination + used, source, length);
  destination[used + length] = '\0';
}

bool parseReference(DailyVerse& verse) {
  const char* colon = strrchr(verse.reference, ':');
  if (!colon || colon == verse.reference || !std::isdigit(static_cast<unsigned char>(colon[1]))) return false;

  const char* chapterStart = colon;
  while (chapterStart > verse.reference && std::isdigit(static_cast<unsigned char>(chapterStart[-1]))) --chapterStart;
  if (chapterStart == verse.reference || chapterStart[-1] != ' ') return false;

  char* parseEnd = nullptr;
  const long chapter = strtol(chapterStart, &parseEnd, 10);
  if (parseEnd != colon || chapter <= 0 || chapter > MAX_CHAPTER_COUNT) return false;

  const long verseStart = strtol(colon + 1, &parseEnd, 10);
  if (parseEnd == colon + 1 || verseStart <= 0 || verseStart > UINT16_MAX) return false;
  long verseEnd = verseStart;
  if (*parseEnd == '-') {
    char* rangeEnd = nullptr;
    const long parsedEnd = strtol(parseEnd + 1, &rangeEnd, 10);
    if (rangeEnd != parseEnd + 1 && parsedEnd >= verseStart && parsedEnd <= UINT16_MAX) {
      verseEnd = parsedEnd;
      parseEnd = rangeEnd;
    }
  }
  if (*parseEnd != '\0') return false;

  size_t bookLength = static_cast<size_t>(chapterStart - verse.reference - 1);
  while (bookLength > 0 && verse.reference[bookLength - 1] == ' ') --bookLength;
  if (bookLength == 0 || bookLength >= sizeof(verse.referenceBook)) return false;
  memcpy(verse.referenceBook, verse.reference, bookLength);
  verse.referenceBook[bookLength] = '\0';
  verse.referenceChapter = static_cast<uint16_t>(chapter);
  verse.referenceVerseStart = static_cast<uint16_t>(verseStart);
  verse.referenceVerseEnd = static_cast<uint16_t>(verseEnd);
  return true;
}

const char* personalNoteText(const JsonVariantConst note) {
  if (note.is<const char*>()) return note.as<const char*>();
  if (note.is<JsonObjectConst>()) return note["text"].as<const char*>();
  return nullptr;
}

void parsePersonalNotes(const JsonVariantConst personal, char* destination, const size_t destinationSize) {
  destination[0] = '\0';
  if (personal.is<JsonArrayConst>()) {
    for (const JsonVariantConst note : personal.as<JsonArrayConst>()) {
      const char* text = personalNoteText(note);
      if (!text || text[0] == '\0') continue;
      if (destination[0] != '\0') appendTruncated(destination, destinationSize, "\n");
      appendTruncated(destination, destinationSize, text);
    }
    return;
  }
  const char* text = personalNoteText(personal);
  if (text && text[0] != '\0') copyTruncatedUtf8(destination, destinationSize, text);
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
    if (end != buffer + lineStart && number == verse && (*end == VERSE_NUMBER_END || *end == NOTE_MARKER_START)) {
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
    const int markerLength =
        snprintf(marker, sizeof(marker), "%c%u%c", NOTE_MARKER_START, output[i].number, NOTE_MARKER_END);
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

bool parseDailyVerseObject(const JsonObjectConst verseObject, const char* date, DailyVerse& verse, const char* path) {
  verse = DailyVerse{};
  const JsonObjectConst translation = verseObject["translation"].as<JsonObjectConst>();
  if (verseObject.isNull() || translation.isNull()) return false;

  const char* reference = verseObject["reference"].as<const char*>();
  const char* abbreviation = translation["abbreviation"].as<const char*>();
  const char* name = translation["name"].as<const char*>();
  const char* language = translation["language"].as<const char*>();
  const char* text = verseObject["text"].as<const char*>();

  if ((date && date[0] != '\0' && !copyExact(verse.date, sizeof(verse.date), date)) ||
      !copyExact(verse.reference, sizeof(verse.reference), reference) ||
      !copyExact(verse.translationAbbreviation, sizeof(verse.translationAbbreviation), abbreviation) ||
      !copyExact(verse.translationName, sizeof(verse.translationName), name) ||
      !copyExact(verse.translationLanguage, sizeof(verse.translationLanguage), language) ||
      !copyTruncatedUtf8(verse.text, sizeof(verse.text), text) || !parseReference(verse)) {
    LOG_ERR(LOG_TAG, "Daily verse fields are missing or too long: %s", path);
    verse = DailyVerse{};
    return false;
  }

  const JsonObjectConst memorisation = verseObject["memorisation"].as<JsonObjectConst>();
  if (!memorisation.isNull()) {
    verse.memorisationLevel = memorisation["level"] | 0;
    verse.memorisationScale = memorisation["scale"] | 0;
  }
  const JsonVariantConst sticky = verseObject["is_sticky"];
  if (!sticky.isNull() && !sticky.is<bool>()) {
    LOG_ERR(LOG_TAG, "Daily verse is_sticky must be boolean: %s", path);
    verse = DailyVerse{};
    return false;
  }
  verse.isSticky = sticky.as<bool>();
  parsePersonalNotes(verseObject["notes"]["personal"], verse.personalNote, sizeof(verse.personalNote));
  verse.valid = true;
  return true;
}

bool loadDailyVerseFile(const char* path, DailyVerse& verse) {
  JsonDocument document;
  if (!parseJsonFile(path, MAX_DAILY_VERSE_BYTES, document)) return false;

  const JsonObjectConst root = document.as<JsonObjectConst>();
  if (!root["success"].as<bool>()) return false;
  return parseDailyVerseObject(root["verse"].as<JsonObjectConst>(), root["date"].as<const char*>(), verse, path);
}

bool validateDailyVersesRoot(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  const size_t size = file.size();
  if (size == 0 || size > MAX_DAILY_VERSES_BYTES) {
    LOG_ERR(LOG_TAG, "Verses JSON has unsupported size: %s (%u bytes)", path, static_cast<unsigned>(size));
    return false;
  }

  // Filtering validates the complete JSON syntax while retaining only the
  // small top-level success flag; verse bodies remain on SD.
  JsonDocument filter;
  filter["success"] = true;
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, file, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR(LOG_TAG, "Invalid verses JSON in %s: %s", path, error.c_str());
    return false;
  }
  return document["success"].is<bool>() && document["success"].as<bool>();
}

bool collectDailyVerseOffsets(const char* path, DailyVersesInfo& info, uint32_t* offsets,
                              const size_t offsetsCapacity) {
  info = DailyVersesInfo{};
  if (!validateDailyVersesRoot(path)) return false;

  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;

  char buffer[JSON_SCAN_BUFFER_BYTES];
  bool inString = false;
  bool escaped = false;
  bool captureRootKey = false;
  bool rootKeyOverflow = false;
  bool expectingRootKey = false;
  bool versesKey = false;
  bool waitingForColon = false;
  bool waitingForVersesArray = false;
  bool inVersesArray = false;
  bool foundVersesArray = false;
  size_t rootKeyLength = 0;
  char rootKey[8]{};
  uint8_t depth = 0;
  uint8_t versesDepth = 0;
  uint32_t absoluteOffset = 0;

  while (file.available()) {
    const int bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;
    for (int i = 0; i < bytesRead; ++i, ++absoluteOffset) {
      const char value = buffer[i];
      info.contentHash = (info.contentHash ^ static_cast<uint8_t>(value)) * FNV1A_PRIME;

      if (inString) {
        if (escaped) {
          escaped = false;
          if (captureRootKey) rootKeyOverflow = true;
          continue;
        }
        if (value == '\\') {
          escaped = true;
          continue;
        }
        if (value == '"') {
          inString = false;
          if (captureRootKey) {
            rootKey[rootKeyLength] = '\0';
            versesKey = !rootKeyOverflow && strcmp(rootKey, "verses") == 0;
            waitingForColon = versesKey;
            expectingRootKey = false;
          }
          captureRootKey = false;
          continue;
        }
        if (captureRootKey) {
          if (rootKeyLength + 1 < sizeof(rootKey)) {
            rootKey[rootKeyLength++] = value;
          } else {
            rootKeyOverflow = true;
          }
        }
        continue;
      }

      if (value == '"') {
        inString = true;
        captureRootKey = !inVersesArray && depth == 1 && expectingRootKey;
        rootKeyLength = 0;
        rootKeyOverflow = false;
        continue;
      }

      if (waitingForColon) {
        if (std::isspace(static_cast<unsigned char>(value))) continue;
        if (value != ':') return false;
        waitingForColon = false;
        waitingForVersesArray = true;
        continue;
      }
      if (waitingForVersesArray) {
        if (std::isspace(static_cast<unsigned char>(value))) continue;
        if (value != '[' || depth != 1) return false;
        waitingForVersesArray = false;
        inVersesArray = true;
        foundVersesArray = true;
        versesDepth = ++depth;
        continue;
      }

      if (inVersesArray) {
        if (depth == versesDepth) {
          if (value == '{') {
            if (info.count >= offsetsCapacity || info.count >= MAX_DAILY_VERSE_COUNT) {
              LOG_ERR(LOG_TAG, "Too many Daily verses in %s", path);
              return false;
            }
            if (offsets) offsets[info.count] = absoluteOffset;
            ++info.count;
            ++depth;
            continue;
          }
          if (value == ']') {
            info.arrayEnd = absoluteOffset;
            inVersesArray = false;
            --depth;
            continue;
          }
          if (value == ',' || std::isspace(static_cast<unsigned char>(value))) continue;
          LOG_ERR(LOG_TAG, "Daily verses array contains a non-object value: %s", path);
          return false;
        }
        if (value == '{' || value == '[') {
          ++depth;
        } else if (value == '}' || value == ']') {
          if (depth == 0) return false;
          --depth;
        }
        continue;
      }

      if (value == '{' || value == '[') {
        ++depth;
        if (depth == 1 && value == '{') expectingRootKey = true;
      } else if (value == '}' || value == ']') {
        if (depth == 0) return false;
        --depth;
      } else if (value == ',' && depth == 1) {
        expectingRootKey = true;
        versesKey = false;
      }
    }
  }

  return foundVersesArray && !inVersesArray && info.count > 0 && info.arrayEnd > 0;
}

void buildDailyVerseFilter(JsonDocument& filter) {
  filter["reference"] = true;
  filter["translation"]["abbreviation"] = true;
  filter["translation"]["name"] = true;
  filter["translation"]["language"] = true;
  filter["text"] = true;
  filter["memorisation"]["level"] = true;
  filter["memorisation"]["scale"] = true;
  filter["notes"]["personal"] = true;
  filter["is_sticky"] = true;
}

bool parseDailyVerseAtOffset(HalFile& file, const uint32_t offset, JsonDocument& document, const JsonDocument& filter,
                             DailyVerse& verse, const char* path) {
  if (!file.seek(offset)) return false;
  document.clear();
  const DeserializationError error =
      deserializeJson(document, file, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(12));
  if (error) {
    LOG_ERR(LOG_TAG, "Invalid verse object in %s: %s", path, error.c_str());
    return false;
  }
  return parseDailyVerseObject(document.as<JsonObjectConst>(), nullptr, verse, path);
}

uint16_t memorisationWeight(const DailyVerse& verse) {
  if (verse.memorisationScale == 0) return 1;
  const uint8_t level = std::min(verse.memorisationLevel, verse.memorisationScale);
  return static_cast<uint16_t>(verse.memorisationScale - level + 1);
}

bool validateAndChooseDailyVerse(const char* path, const DailyVersesInfo& info, const uint32_t* offsets,
                                 uint16_t& selectedIndex) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;

  // DailyVerse is several KB and the offset table is 512 bytes at its bound;
  // both are reused for the whole validation pass instead of living on stack.
  auto candidate = makeUniqueNoThrow<DailyVerse>();
  if (!candidate) {
    LOG_ERR(LOG_TAG, "OOM: Daily verse validation model (%u bytes)", static_cast<unsigned>(sizeof(DailyVerse)));
    return false;
  }

  JsonDocument filter;
  buildDailyVerseFilter(filter);
  JsonDocument document;
  bool stickyFound = false;
  uint32_t totalWeight = 0;
  selectedIndex = 0;
  for (uint16_t index = 0; index < info.count; ++index) {
    const uint32_t objectEnd = index + 1 < info.count ? offsets[index + 1] : info.arrayEnd;
    if (objectEnd <= offsets[index] || objectEnd - offsets[index] > MAX_DAILY_VERSE_OBJECT_BYTES) {
      LOG_ERR(LOG_TAG, "Daily verse object is too large: %s", path);
      return false;
    }
    if (!parseDailyVerseAtOffset(file, offsets[index], document, filter, *candidate, path)) return false;

    if (candidate->isSticky) {
      if (stickyFound) {
        LOG_ERR(LOG_TAG, "Daily verses response contains more than one sticky verse");
        return false;
      }
      stickyFound = true;
      selectedIndex = index;
      continue;
    }
    if (stickyFound) continue;

    // Linear inverse weighting keeps every verse eligible while making a
    // lower memorisation level proportionally more likely.
    const uint16_t weight = memorisationWeight(*candidate);
    totalWeight += weight;
    if (random(static_cast<long>(totalWeight)) < weight) selectedIndex = index;
  }
  return true;
}

bool readDailyVerseSelection(DailyVerse& verse, uint16_t& index, uint16_t& count) {
  // The snapshot is variable data, so a short-lived JSON document is required;
  // decoded verse fields remain bounded by DailyVerse's fixed buffers.
  JsonDocument document;
  if (!parseJsonFile(DAILY_VERSES_SELECTION_PATH, MAX_DAILY_VERSE_SELECTION_BYTES, document)) return false;

  const JsonObjectConst root = document.as<JsonObjectConst>();
  const uint32_t parsedIndex = root["selectedIndex"].as<uint32_t>();
  const uint32_t parsedCount = root["count"].as<uint32_t>();
  if (root["schemaVersion"].as<uint8_t>() != DAILY_VERSE_SELECTION_SCHEMA_VERSION || parsedCount == 0 ||
      parsedCount > MAX_DAILY_VERSE_COUNT || parsedIndex >= parsedCount ||
      !parseDailyVerseObject(root["verse"].as<JsonObjectConst>(), nullptr, verse, DAILY_VERSES_SELECTION_PATH)) {
    return false;
  }

  index = static_cast<uint16_t>(parsedIndex);
  count = static_cast<uint16_t>(parsedCount);
  return true;
}

bool writeDailyVerseSelection(const char* path, const DailyVersesInfo& info, const uint16_t index,
                              const DailyVerse& verse) {
  // This sync-only document is short-lived and contains only fields already
  // bounded by DailyVerse; persistent/static storage would retain scarce DRAM.
  JsonDocument document;
  document["schemaVersion"] = DAILY_VERSE_SELECTION_SCHEMA_VERSION;
  document["contentHash"] = info.contentHash;
  document["count"] = info.count;
  document["selectedIndex"] = index;
  JsonObject selected = document["verse"].to<JsonObject>();
  selected["reference"] = verse.reference;
  JsonObject translation = selected["translation"].to<JsonObject>();
  translation["abbreviation"] = verse.translationAbbreviation;
  translation["name"] = verse.translationName;
  translation["language"] = verse.translationLanguage;
  selected["text"] = verse.text;
  JsonObject memorisation = selected["memorisation"].to<JsonObject>();
  memorisation["level"] = verse.memorisationLevel;
  memorisation["scale"] = verse.memorisationScale;
  selected["notes"]["personal"] = verse.personalNote;
  selected["is_sticky"] = verse.isSticky;

  const size_t serializedSize = measureJson(document);
  if (serializedSize == 0 || serializedSize > MAX_DAILY_VERSE_SELECTION_BYTES) {
    LOG_ERR(LOG_TAG, "Daily verse selection is too large (%u bytes)", static_cast<unsigned>(serializedSize));
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite(LOG_TAG, path, file)) return false;
  if (serializeJson(document, file) == 0) {
    LOG_ERR(LOG_TAG, "Could not write Daily verse selection");
    return false;
  }
  return true;
}

bool loadDailyVerseFromCollection(const char* path, const bool useSavedSelection, DailyVerse& verse,
                                  uint16_t& selectedIndex, uint16_t& verseCount) {
  // The selection cache contains the already-validated display model. Reading
  // it keeps Bible startup independent of the full verses payload size.
  if (useSavedSelection && readDailyVerseSelection(verse, selectedIndex, verseCount)) return true;

  // The bounded table avoids retaining every multi-KB verse in RAM.
  auto offsets = makeUniqueNoThrow<uint32_t[]>(MAX_DAILY_VERSE_COUNT);
  if (!offsets) {
    LOG_ERR(LOG_TAG, "OOM: Daily verse offsets (%u bytes)",
            static_cast<unsigned>(MAX_DAILY_VERSE_COUNT * sizeof(uint32_t)));
    return false;
  }
  DailyVersesInfo info;
  if (!collectDailyVerseOffsets(path, info, offsets.get(), MAX_DAILY_VERSE_COUNT)) return false;
  if (!validateAndChooseDailyVerse(path, info, offsets.get(), selectedIndex)) return false;

  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  JsonDocument filter;
  buildDailyVerseFilter(filter);
  JsonDocument document;
  if (!parseDailyVerseAtOffset(file, offsets[selectedIndex], document, filter, verse, path)) return false;
  verseCount = info.count;
  if (useSavedSelection &&
      !writeDailyVerseSelection(DAILY_VERSES_SELECTION_PATH, info, selectedIndex, verse)) {
    LOG_ERR(LOG_TAG, "Could not upgrade Daily verse selection cache");
  }
  return true;
}

bool loadDailyVerseAtFromCollection(const char* path, const uint16_t index, DailyVerse& verse, uint16_t& verseCount) {
  auto offsets = makeUniqueNoThrow<uint32_t[]>(MAX_DAILY_VERSE_COUNT);
  if (!offsets) {
    LOG_ERR(LOG_TAG, "OOM: Daily verse offsets (%u bytes)",
            static_cast<unsigned>(MAX_DAILY_VERSE_COUNT * sizeof(uint32_t)));
    return false;
  }
  DailyVersesInfo info;
  if (!collectDailyVerseOffsets(path, info, offsets.get(), MAX_DAILY_VERSE_COUNT) || index >= info.count) return false;

  HalFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;
  JsonDocument filter;
  buildDailyVerseFilter(filter);
  JsonDocument document;
  if (!parseDailyVerseAtOffset(file, offsets[index], document, filter, verse, path)) return false;
  verseCount = info.count;
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

bool BibleLibrary::findBookByName(const VersionInfo& version, const char* bookName, BookInfo& book) {
  if (!bookName || bookName[0] == '\0') return false;
  char path[MAX_PATH_LENGTH];
  if (!manifestPath(version, path, sizeof(path))) return false;

  JsonDocument document;
  if (!parseJsonFile(path, MAX_MANIFEST_BYTES, document)) return false;
  const JsonArrayConst bookArray = document["books"].as<JsonArrayConst>();
  for (const JsonObjectConst object : bookArray) {
    const char* name = firstString(object, "name", "displayName");
    if (!name) continue;

    size_t leftLength = strlen(name);
    size_t rightLength = strlen(bookName);
    while (leftLength > 0 && std::tolower(static_cast<unsigned char>(name[leftLength - 1])) == 's') --leftLength;
    while (rightLength > 0 && std::tolower(static_cast<unsigned char>(bookName[rightLength - 1])) == 's') --rightLength;
    if (leftLength != rightLength) continue;

    bool matches = true;
    for (size_t i = 0; i < leftLength; ++i) {
      if (std::tolower(static_cast<unsigned char>(name[i])) != std::tolower(static_cast<unsigned char>(bookName[i]))) {
        matches = false;
        break;
      }
    }
    if (matches) return parseBook(object, book);
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

bool BibleLibrary::loadDailyVerse(DailyVerse& verse, uint16_t* selectedIndex, uint16_t* verseCount) {
  verse = DailyVerse{};
  uint16_t index = 0;
  uint16_t count = 0;
  if (Storage.exists(DAILY_VERSES_CACHE_PATH) &&
      loadDailyVerseFromCollection(DAILY_VERSES_CACHE_PATH, true, verse, index, count)) {
    if (selectedIndex) *selectedIndex = index;
    if (verseCount) *verseCount = count;
    return true;
  }
  if (Storage.exists(DAILY_VERSES_BACKUP_PATH) &&
      loadDailyVerseFromCollection(DAILY_VERSES_BACKUP_PATH, false, verse, index, count)) {
    if (selectedIndex) *selectedIndex = index;
    if (verseCount) *verseCount = count;
    return true;
  }
  if (Storage.exists(DAILY_VERSES_FIXTURE_PATH) &&
      loadDailyVerseFromCollection(DAILY_VERSES_FIXTURE_PATH, false, verse, index, count)) {
    if (selectedIndex) *selectedIndex = index;
    if (verseCount) *verseCount = count;
    return true;
  }
  if (Storage.exists(LEGACY_DAILY_VERSE_CACHE_PATH) && loadDailyVerseFile(LEGACY_DAILY_VERSE_CACHE_PATH, verse)) {
    if (selectedIndex) *selectedIndex = 0;
    if (verseCount) *verseCount = 1;
    return true;
  }
  if (Storage.exists(LEGACY_DAILY_VERSE_BACKUP_PATH) && loadDailyVerseFile(LEGACY_DAILY_VERSE_BACKUP_PATH, verse)) {
    if (selectedIndex) *selectedIndex = 0;
    if (verseCount) *verseCount = 1;
    return true;
  }
  if (Storage.exists(DAILY_VERSE_FIXTURE_PATH) && loadDailyVerseFile(DAILY_VERSE_FIXTURE_PATH, verse)) return true;
  return false;
}

bool BibleLibrary::loadDailyVerseAt(const uint16_t index, DailyVerse& verse, uint16_t* verseCount) {
  // Navigation must leave the displayed verse intact if SD parsing fails, so
  // use one bounded fallible model on this cold long-press path.
  auto candidate = makeUniqueNoThrow<DailyVerse>();
  if (!candidate) {
    LOG_ERR(LOG_TAG, "OOM: Daily verse navigation model (%u bytes)", static_cast<unsigned>(sizeof(DailyVerse)));
    return false;
  }
  uint16_t count = 0;
  if (Storage.exists(DAILY_VERSES_CACHE_PATH) &&
      loadDailyVerseAtFromCollection(DAILY_VERSES_CACHE_PATH, index, *candidate, count)) {
    verse = *candidate;
    if (verseCount) *verseCount = count;
    return true;
  }
  if (Storage.exists(DAILY_VERSES_BACKUP_PATH) &&
      loadDailyVerseAtFromCollection(DAILY_VERSES_BACKUP_PATH, index, *candidate, count)) {
    verse = *candidate;
    if (verseCount) *verseCount = count;
    return true;
  }
  if (Storage.exists(DAILY_VERSES_FIXTURE_PATH) &&
      loadDailyVerseAtFromCollection(DAILY_VERSES_FIXTURE_PATH, index, *candidate, count)) {
    verse = *candidate;
    if (verseCount) *verseCount = count;
    return true;
  }
  return false;
}

namespace {

bool refreshDailyVerseCache(DailyVerse* output) {
  if (!Storage.ensureDirectoryExists(DAILY_CACHE_DIRECTORY)) {
    LOG_ERR(LOG_TAG, "Could not create Daily cache directory");
    return false;
  }
  if (Storage.exists(DAILY_VERSES_TEMP_PATH)) Storage.remove(DAILY_VERSES_TEMP_PATH);
  if (Storage.exists(DAILY_VERSES_SELECTION_TEMP_PATH)) Storage.remove(DAILY_VERSES_SELECTION_TEMP_PATH);

  HalFile download;
  if (!Storage.openFileForWrite(LOG_TAG, DAILY_VERSES_TEMP_PATH, download)) return false;
  size_t downloaded = 0;
  const bool fetched =
      HttpDownloader::fetchUrl(DAILY_VERSES_URL, [&download, &downloaded](const uint8_t* data, const size_t length) {
        if (length > MAX_DAILY_VERSES_BYTES - downloaded) return false;
        if (download.write(data, length) != length) return false;
        downloaded += length;
        return true;
      });
  download.close();  // Reopened for validation and renamed below.
  if (!fetched || downloaded == 0) {
    Storage.remove(DAILY_VERSES_TEMP_PATH);
    return false;
  }

  auto offsets = makeUniqueNoThrow<uint32_t[]>(MAX_DAILY_VERSE_COUNT);
  if (!offsets) {
    LOG_ERR(LOG_TAG, "OOM: Daily verse offsets (%u bytes)",
            static_cast<unsigned>(MAX_DAILY_VERSE_COUNT * sizeof(uint32_t)));
    Storage.remove(DAILY_VERSES_TEMP_PATH);
    return false;
  }
  DailyVersesInfo info;
  uint16_t selectedIndex = 0;
  if (!collectDailyVerseOffsets(DAILY_VERSES_TEMP_PATH, info, offsets.get(), MAX_DAILY_VERSE_COUNT) ||
      !validateAndChooseDailyVerse(DAILY_VERSES_TEMP_PATH, info, offsets.get(), selectedIndex)) {
    Storage.remove(DAILY_VERSES_TEMP_PATH);
    Storage.remove(DAILY_VERSES_SELECTION_TEMP_PATH);
    return false;
  }

  // DailyVerse is too large for the small network-task stack. Keep one
  // fallible model long enough to create the compact startup cache and, when
  // requested, return the selected verse without parsing the collection again.
  auto selectedVerse = makeUniqueNoThrow<DailyVerse>();
  if (!selectedVerse) {
    LOG_ERR(LOG_TAG, "OOM: selected Daily verse model (%u bytes)", static_cast<unsigned>(sizeof(DailyVerse)));
    Storage.remove(DAILY_VERSES_TEMP_PATH);
    return false;
  }
  bool selectedVerseParsed = false;
  {
    HalFile source;
    JsonDocument filter;
    JsonDocument document;
    buildDailyVerseFilter(filter);
    selectedVerseParsed =
        Storage.openFileForRead(LOG_TAG, DAILY_VERSES_TEMP_PATH, source) &&
        parseDailyVerseAtOffset(source, offsets[selectedIndex], document, filter, *selectedVerse,
                                DAILY_VERSES_TEMP_PATH);
  }
  if (!selectedVerseParsed ||
      !writeDailyVerseSelection(DAILY_VERSES_SELECTION_TEMP_PATH, info, selectedIndex, *selectedVerse)) {
    Storage.remove(DAILY_VERSES_TEMP_PATH);
    Storage.remove(DAILY_VERSES_SELECTION_TEMP_PATH);
    return false;
  }

  const bool hadCache = Storage.exists(DAILY_VERSES_CACHE_PATH);
  const bool hadBackup = Storage.exists(DAILY_VERSES_BACKUP_PATH);
  if (hadCache) {
    // A leftover backup means the previous install was interrupted. Preserve
    // that known fallback and discard only the cache file it supersedes.
    const bool preserved = hadBackup ? Storage.remove(DAILY_VERSES_CACHE_PATH)
                                     : Storage.rename(DAILY_VERSES_CACHE_PATH, DAILY_VERSES_BACKUP_PATH);
    if (!preserved) {
      LOG_ERR(LOG_TAG, "Could not preserve previous Daily verse cache");
      Storage.remove(DAILY_VERSES_TEMP_PATH);
      Storage.remove(DAILY_VERSES_SELECTION_TEMP_PATH);
      return false;
    }
  }
  if (!Storage.rename(DAILY_VERSES_TEMP_PATH, DAILY_VERSES_CACHE_PATH)) {
    LOG_ERR(LOG_TAG, "Could not install Daily verse cache");
    if (Storage.exists(DAILY_VERSES_BACKUP_PATH)) {
      Storage.rename(DAILY_VERSES_BACKUP_PATH, DAILY_VERSES_CACHE_PATH);
    }
    Storage.remove(DAILY_VERSES_TEMP_PATH);
    Storage.remove(DAILY_VERSES_SELECTION_TEMP_PATH);
    return false;
  }

  if (Storage.exists(DAILY_VERSES_SELECTION_PATH) && !Storage.remove(DAILY_VERSES_SELECTION_PATH)) {
    LOG_ERR(LOG_TAG, "Could not replace Daily verse selection");
    Storage.remove(DAILY_VERSES_CACHE_PATH);
    if (Storage.exists(DAILY_VERSES_BACKUP_PATH)) {
      Storage.rename(DAILY_VERSES_BACKUP_PATH, DAILY_VERSES_CACHE_PATH);
    }
    Storage.remove(DAILY_VERSES_SELECTION_TEMP_PATH);
    return false;
  }
  if (!Storage.rename(DAILY_VERSES_SELECTION_TEMP_PATH, DAILY_VERSES_SELECTION_PATH)) {
    LOG_ERR(LOG_TAG, "Could not install Daily verse selection");
    Storage.remove(DAILY_VERSES_CACHE_PATH);
    if (Storage.exists(DAILY_VERSES_BACKUP_PATH)) {
      Storage.rename(DAILY_VERSES_BACKUP_PATH, DAILY_VERSES_CACHE_PATH);
    }
    return false;
  }
  if (Storage.exists(DAILY_VERSES_BACKUP_PATH)) Storage.remove(DAILY_VERSES_BACKUP_PATH);
  if (output) *output = *selectedVerse;
  return true;
}

}  // namespace

bool BibleLibrary::refreshDailyVerse() { return refreshDailyVerseCache(nullptr); }

bool BibleLibrary::loadDailyChapter(const DailyVerse& verse, const char* footnotesLabel, std::unique_ptr<char[]>& text,
                                    size_t& textLength, size_t& textCapacity) {
  text.reset();
  textLength = 0;
  textCapacity = 0;
  if (!verse.valid || verse.text[0] == '\0') return false;

  const size_t sourceLength = strlen(verse.text);
  const size_t noteLength = strlen(verse.personalNote);
  const size_t labelLength = footnotesLabel ? strlen(footnotesLabel) : 0;
  const size_t capacity = sourceLength + noteLength + labelLength + 32;
  auto dailyText = makeUniqueNoThrow<char[]>(capacity);
  if (!dailyText) {
    LOG_ERR(LOG_TAG, "OOM: Daily chapter buffer (%u bytes)", static_cast<unsigned>(capacity));
    return false;
  }

  size_t input = 0;
  size_t output = 0;
  bool lineStart = true;
  while (input < sourceLength) {
    if (verse.text[input] == '\r') {
      ++input;
      continue;
    }
    if (lineStart && std::isdigit(static_cast<unsigned char>(verse.text[input]))) {
      while (input < sourceLength && std::isdigit(static_cast<unsigned char>(verse.text[input]))) {
        dailyText[output++] = verse.text[input++];
      }
      if (input < sourceLength && (verse.text[input] == ' ' || verse.text[input] == '\t')) {
        dailyText[output++] = VERSE_NUMBER_END;
        while (input < sourceLength && (verse.text[input] == ' ' || verse.text[input] == '\t')) ++input;
      }
      lineStart = false;
      continue;
    }
    const char value = verse.text[input++];
    dailyText[output++] = value;
    lineStart = value == '\n';
  }

  if (noteLength > 0) {
    const int written = snprintf(dailyText.get() + output, capacity - output, "\n\n%s\n[1] %s",
                                 footnotesLabel ? footnotesLabel : "", verse.personalNote);
    if (written > 0) output += std::min(static_cast<size_t>(written), capacity - output - 1);
  }
  dailyText[output] = '\0';
  text = std::move(dailyText);
  textLength = output;
  textCapacity = capacity;
  return true;
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
  const int chapterWritten =
      snprintf(chapterPath, sizeof(chapterPath), "%s/%s/%s/%03u.txt", BIBLE_ROOT, version.directory, book.id, chapter);
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
    LOG_ERR(LOG_TAG, "Chapter has unsupported size: %s (%u bytes)", chapterPath, static_cast<unsigned>(chapterSize));
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

  // Normalize CRLF and retain the manifest's TAB separator as an internal
  // marker so the reader can style verse numbers without a second copy.
  size_t normalizedLength = 0;
  for (size_t i = 0; i < chapterSize; ++i) {
    char value = chapterText[i];
    if (value == '\r') continue;
    if (value == '\t') value = VERSE_NUMBER_END;
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
