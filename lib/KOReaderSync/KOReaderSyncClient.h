#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * Optional document metadata sent alongside progress sync requests.
 * Mirrors the metadata object added in KOReader PR #15306 and adds bounded
 * bibliographic fields for compatible custom servers.
 */
struct KOReaderMetadata {
  std::string filename;  // e.g. "my_book.epub"
  std::string title;     // Document title from EPUB metadata
  std::string authors;   // Author(s) from EPUB metadata
  // Raw JSON of the book's plugin sidecar ("<book>.meta.json"): e.g. a
  // service book id a plugin recorded at download time, so a custom sync
  // server can route progress to that service. Its flat fields are sent
  // verbatim (JSON types kept) inside the metadata object; the firmware
  // assigns no meaning to the keys.
  std::string extraJson;
  std::string isbn;    // ISBN from package metadata when available
  std::string asin;    // Amazon ASIN from package metadata when available
  std::string series;  // Series/collection title when available
  std::optional<float> seriesIndex;
};

/**
 * Rich CrossPoint position sent alongside progress uploads. Maps 1:1 onto the
 * crosspoint-sync extended `position` object (see crosspoint-sync docs/API.md).
 * It is transmitted only when the selected server type supports rich progress.
 * These fields remain layout-dependent compatibility hints; the standard XPath
 * is the content anchor.
 */
struct KOReaderRichPosition {
  uint32_t pctQ = 0;                       // Percentage quantized 0..1,000,000 (metadata/fallback)
  uint16_t spineIndex = 0;                 // Spine (chapter) index
  uint16_t pageNumber = 0;                 // Page within spine (layout-dependent hint)
  uint16_t totalPages = 1;                 // Spine page count (layout-dependent hint)
  std::optional<uint16_t> paragraphIndex;  // Synthetic 1-based paragraph index
  std::string xpath;                       // KOReader-style xpath (server cap: 120 bytes)
};

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;                          // Document hash
  std::string progress;                          // XPath-like progress string
  float percentage;                              // Progress percentage (0.0 to 1.0)
  std::string device;                            // Device name
  std::string deviceId;                          // Device ID
  int64_t timestamp;                             // Unix timestamp of last update
  std::optional<KOReaderMetadata> metadata;      // Optional document metadata
  std::optional<KOReaderRichPosition> position;  // Optional rich position
};

// WaddleOS keeps only the position types above; there is no sync client.

