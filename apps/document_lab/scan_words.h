#pragma once

#include <doc_vision/guidance.h>
#include <doc_vision/mrz.h>
#include <doc_vision/quality_gates.h>

#include <QString>

#include <cstdint>
#include <optional>
#include <string>

namespace document_lab {

// What the banner says while the Document processor is chosen and no run gives results, or its model is
// unavailable: the window's own states, in the banner's place.
enum class WindowState { NoCamera, AccessOff, CameraStopped, Idle, ModelMissing, RuntimeMissing };

// The banner's two lines, its tone, and whether its icon is the ID card rather than the tone's ✓ or ✗.
struct BannerContent {
    doc_vision::Tone tone = doc_vision::Tone::Neutral;
    bool cardIcon = false;
    QString bigLine;
    QString smallLine; // empty: one line only

    bool operator==(const BannerContent&) const = default;
};

// A hint's banner (ADR-004's table, A3.1's rows).
BannerContent bannerFor(doc_vision::Hint hint);

// The focus banners (A3.3, spec §3.5): a search running; a search that found nothing sharp; a camera the lab
// cannot focus that stayed blurred.
enum class FocusBanner { Focusing, CannotFocusThisClose, CannotMakeSharp };
BannerContent bannerFor(FocusBanner banner);

// A window state's banner. `detail` is CameraStopped's small line, the failure's reason; ignored otherwise.
// AccessOff's small line is the running platform's.
BannerContent bannerFor(WindowState state, const QString& detail = {});

// A4: Reading's small line, "Reading the lines at the bottom · 2 of 3".
QString readingLine(int agreeing);
// In its place: a one-frame source (three reads of one picture are no evidence), or a reader that could not
// be opened.
QString readingNeedsMotion();
QString readerUnavailableLine();
// A date of the MRZ as Review shows it: "12 Aug 1974", "Aug 1974" for an unknown day, "1974" for an unknown
// month, or the six characters when it is no date. English month names whatever the locale.
QString mrzDate(const std::string& yymmdd, const std::optional<doc_vision::MrzDate>& date);
// "Expired on 15 Apr 2012. A5 and later steps flag this; the scan itself still succeeded."
QString expiredWarning(const doc_vision::MrzDate& expiry);
// The still Review sends: fixed words and the frame's number as the user sees it, never MRZ text.
QString stillName(std::uint64_t frameNumber);

// A gate's chip: "In view", "Size", "Flat", "Sharp", "No glare", "Light".
QString chipLabel(doc_vision::Gate gate);

// The status bar's "State:" word for the shown hint: "searching", "adjust (sharpness)" (the gate to fix) or
// "good".
QString stateWord(doc_vision::Hint shown);

} // namespace document_lab
