#pragma once

#include <string>
#include <vector>

// The NRK paper from arnee.me, as saved on the SD card: the book, its headline
// list and which stories have been read. Used by the Nyheter screen and Hjem.
namespace waddle::news {

// Leaving the reader from this book returns to the Nyheter list.
constexpr const char* EPUB_PATH = "/Nyheter/NRK.epub";

struct Story {
  std::string id;  // NRK's article id
  std::string title;
  std::string time;  // "08:05"
};

struct Paper {
  std::string edition;  // "NRK lør. 24. okt. kl 08"
  std::string built;    // build time, to skip downloading the same paper twice
  std::vector<Story> stories;
};

// Reads the saved headline list. False when there is none.
bool load(Paper& paper);

// Ids of the stories that have been opened.
std::vector<std::string> loadReadIds();
bool isRead(const std::vector<std::string>& readIds, const std::string& id);

// Downloads the headline list, and the book when the paper is new. Needs
// Wi-Fi. The old files stay until the new ones are complete.
bool sync();

// Marks a story read and opens the reader at it.
void openStory(const Paper& paper, std::vector<std::string>& readIds, int index);

}  // namespace waddle::news
