#pragma once

#include <string>

// Saved reading percentage, or -1 when unavailable. Never builds reading caches.
// chapter, when given, gets the chapter number (1-based) of an EPUB, else 0.
int loadBookProgress(const std::string& path, int* chapter = nullptr);
