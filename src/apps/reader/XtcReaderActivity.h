#pragma once

#include <Xtc.h>

#include <memory>
#include <string>

#include "ReaderActivity.h"

class XtcReaderActivity final : public ReaderActivity {
  std::shared_ptr<Xtc> xtc;
  uint32_t currentPage = 0;

  void renderPage();
  void openChapterSelection();
  void saveProgress() const;
  void loadProgress();

  bool loadBook() override;
  std::string getBookTitle() const override { return xtc ? xtc->getTitle() : ""; }
  std::string getBookAuthor() const override { return xtc ? xtc->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return xtc ? xtc->getThumbBmpPath() : ""; }
  bool handleFormatInput() override;
  void renderBook() override;
  void applyInitialOrientation() override;

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("XtcReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~XtcReaderActivity() override = default;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  ScreenshotInfo getScreenshotInfo() const override;
};
