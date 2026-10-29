#include "NewsPaper.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>

#include "core/activities/ActivityManager.h"
#include "apps/reader/EpubReaderUtils.h"
#include "core/network/HttpDownloader.h"

namespace waddle::news {

namespace {

constexpr const char* DIR = "/Nyheter";
constexpr const char* LIST_PATH = "/Nyheter/nrk.json";
constexpr const char* NEW_LIST_PATH = "/Nyheter/nrk.new.json";
constexpr const char* READ_PATH = "/Nyheter/lest.txt";
constexpr const char* LIST_URL = "https://arnee.me/api/nrk.json";
constexpr const char* EPUB_URL = "https://arnee.me/api/nrk.epub";

// Reads the "bygd" (build time) field of a headline list.
std::string builtOf(const char* path) {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path, doc)) return "";
  return doc["bygd"] | "";
}

}  // namespace

bool load(Paper& paper) {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(LIST_PATH, doc)) return false;
  paper.edition = doc["tittel"] | "NRK";
  paper.built = doc["bygd"] | "";
  paper.stories.clear();
  JsonArray list = doc["liste"].as<JsonArray>();
  paper.stories.reserve(list.size());
  for (JsonObject obj : list) {
    paper.stories.push_back({obj["id"] | "", obj["t"] | "", obj["kl"] | ""});
  }
  return true;
}

// lest.txt: one article id per line.
std::vector<std::string> loadReadIds() {
  std::vector<std::string> ids;
  if (!Storage.exists(READ_PATH)) return ids;
  const String text = Storage.readFile(READ_PATH);
  int start = 0;
  while (start < static_cast<int>(text.length())) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    if (end > start) ids.emplace_back(text.substring(start, end).c_str());
    start = end + 1;
  }
  return ids;
}

bool isRead(const std::vector<std::string>& readIds, const std::string& id) {
  return std::find(readIds.begin(), readIds.end(), id) != readIds.end();
}

bool sync() {
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("NEWS", "low heap for download (%u free)", ESP.getFreeHeap());
    return false;
  }
  Storage.mkdir(DIR);
  if (HttpDownloader::downloadToFile(LIST_URL, NEW_LIST_PATH) != HttpDownloader::OK) return false;
  const std::string newBuilt = builtOf(NEW_LIST_PATH);
  if (newBuilt.empty()) {
    Storage.remove(NEW_LIST_PATH);
    return false;
  }
  if (newBuilt == builtOf(LIST_PATH) && Storage.exists(EPUB_PATH)) {
    Storage.remove(NEW_LIST_PATH);
    return true;
  }
  if (HttpDownloader::downloadToFile(EPUB_URL, EPUB_PATH) != HttpDownloader::OK) {
    Storage.remove(NEW_LIST_PATH);
    return false;
  }
  // Same file name as the old paper, so drop the old paper's layout cache.
  Epub(EPUB_PATH, "/.crosspoint").clearCache();
  return Storage.replaceFile(NEW_LIST_PATH, LIST_PATH);
}

// Only ids in today's paper are written, so the file stays small.
void saveReadIds(const Paper& paper, const std::vector<std::string>& readIds) {
  String text;
  for (const auto& story : paper.stories) {
    if (isRead(readIds, story.id)) {
      text += story.id.c_str();
      text += '\n';
    }
  }
  Storage.writeFile(READ_PATH, text);
}

void setRead(const Paper& paper, std::vector<std::string>& readIds, const int index, const bool read) {
  if (index < 0 || index >= static_cast<int>(paper.stories.size())) return;
  const std::string& id = paper.stories[index].id;
  if (read == isRead(readIds, id)) return;
  if (read) {
    readIds.push_back(id);
  } else {
    readIds.erase(std::find(readIds.begin(), readIds.end(), id));
  }
  saveReadIds(paper, readIds);
}

int enterChapter(int spine, const bool skipRead, const int spineCount) {
  Paper paper;
  if (!load(paper)) return spine;
  auto readIds = loadReadIds();
  const int stories = static_cast<int>(paper.stories.size());
  if (skipRead) {
    while (spine >= 1 && spine <= stories && isRead(readIds, paper.stories[spine - 1].id)) spine++;
    if (spine > stories) return spineCount;  // nothing unread after this: end of the paper
  }
  if (spine >= 1 && spine <= stories) setRead(paper, readIds, spine - 1, true);
  return spine;
}

namespace {
bool fromHome_ = false;
}

bool openedFromHome() { return fromHome_; }

void openStory(const Paper& paper, std::vector<std::string>& readIds, const int index, const bool fromHome) {
  if (index < 0 || index >= static_cast<int>(paper.stories.size()) || !Storage.exists(EPUB_PATH)) return;
  fromHome_ = fromHome;
  setRead(paper, readIds, index, true);

  // Chapter 0 is the front page, so story i is chapter i + 1. Saving it as the
  // reading position makes the reader open there.
  Epub epub(EPUB_PATH, "/.crosspoint");
  epub.setupCacheDir();
  EpubReaderUtils::saveProgress(epub, index + 1, 0, 0);
  activityManager.goToReader(EPUB_PATH);
}

}  // namespace waddle::news
