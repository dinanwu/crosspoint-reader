#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;

  // Subscription series marker. Detected by the presence of sub_watermark.bin next
  // to progress.bin. The watermark is rewritten on onExit to the user's current
  // reading position (in chapter-count space) so the inbox shows honest "unread
  // chapters," not "chapters added since last open."
  bool isSubscription = false;
  uint16_t watermarkSpineCount = 0;
  // lastKnownSpineCount captured at onEnter, used by auto-advance to detect when
  // the current series grew during this reading session (mid-session sync).
  uint16_t lastKnownSpineCountAtOpen = 0;

  // Deferred until after the first successful page render so the background
  // OPF/TOC parse doesn't compete with section-0 rendering for heap (see
  // render() for the start call).
  bool backgroundIndexerStarted = false;

  // End-of-book hand-off: if this is a subscription EPUB, transitions to the
  // most-recently-synced other subscription that still has unread chapters.
  // Returns true if a transition was kicked off (caller should not call onGoHome).
  bool tryAutoAdvanceToNextSubscription();

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
};
