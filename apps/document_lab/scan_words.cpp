#include "scan_words.h"

#include <QCoreApplication>
#include <QtGlobal>

namespace document_lab {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("document_lab::ScanWords", text); }

BannerContent bad(const char* big, const char* small = "") {
    return {doc_vision::Tone::Bad, false, tr(big), *small != '\0' ? tr(small) : QString()};
}

QString reinstall() {
    return tr("Reinstall the Document Lab, or rebuild it with the models: see the build guide");
}

} // namespace

BannerContent bannerFor(doc_vision::Hint hint) {
    using doc_vision::Hint;
    switch (hint) {
    case Hint::NoPage:
        // "Document", not "page": an ID card has no page (the owner, A3.3); the passport's own words wait
        // for the MRZ to say what the document is (A4).
        return {doc_vision::Tone::Neutral, true, tr("Show the document"),
                tr("the side with the photo, all of it in view")};
    case Hint::CutByEdge:
        return bad("Move back", "the whole document must be in view");
    case Hint::MoveToMiddle:
        return bad("Move the document to the middle", "the whole document must be in view");
    case Hint::TooFar:
        return bad("Move closer");
    case Hint::TooClose:
        return bad("Move a little further away");
    case Hint::Tilted:
        return bad("Hold the document flat to the camera");
    case Hint::Blurred:
        return bad("Blurred: hold still", "or move a little closer or further away");
    case Hint::Glare:
        return bad("Glare on the document", "tilt it slightly away from the light");
    case Hint::TooDark:
        return bad("Too dark", "add more light");
    case Hint::TooBright:
        return bad("Too bright", "move out of direct light");
    case Hint::NoMrzBand:
        return bad("Keep your fingers off the two lines at the bottom");
    case Hint::CannotReadMrz:
        // Wider than screen 07's (the owner, A4's D2): an ID card's front has no MRZ to read.
        return bad("Can't read the lines at the bottom",
                   "check for glare or fingers; only passports are read for now");
    case Hint::Good:
        break;
    }
    return {doc_vision::Tone::Good, false, tr("Good: hold still"), {}};
}

BannerContent bannerFor(FocusBanner banner) {
    switch (banner) {
    case FocusBanner::Focusing:
        break;
    case FocusBanner::CannotFocusThisClose:
        return bad("This camera can't focus this close", "Move the document a little further away");
    case FocusBanner::CannotMakeSharp:
        return bad("This camera can't make the document sharp", "Try more light, or another camera");
    }
    return {doc_vision::Tone::Neutral, false, tr("Hold still: focusing"), {}};
}

BannerContent bannerFor(WindowState state, const QString& detail) {
    switch (state) {
    case WindowState::NoCamera:
        return bad("No camera found", "Connect a camera, or open a video, an image or a folder");
    case WindowState::AccessOff:
#if defined(Q_OS_MACOS)
        return bad("Camera access is off", "Allow it in System Settings → Privacy & Security → Camera");
#elif defined(Q_OS_WIN)
        return bad("Camera access is off", "Allow it in Settings → Privacy & security → Camera");
#else
        return bad("Camera access is off", "Allow it in the system's privacy settings");
#endif
    case WindowState::CameraStopped:
        return {doc_vision::Tone::Bad, false, tr("The camera stopped"), detail};
    case WindowState::Idle:
        return {doc_vision::Tone::Neutral, true, tr("Start the camera"),
                tr("or open a video, an image or a folder")};
    case WindowState::ModelMissing:
        return {doc_vision::Tone::Bad, false, tr("The document model is missing"), reinstall()};
    case WindowState::RuntimeMissing:
        break;
    }
    return {doc_vision::Tone::Bad, false, tr("ONNX Runtime could not be loaded"), reinstall()};
}

QString readingLine(int agreeing) { return tr("Reading the lines at the bottom · %1 of 3").arg(agreeing); }

QString readingNeedsMotion() { return tr("MRZ reading needs a camera or a video"); }

QString readerUnavailableLine() {
    return tr("The MRZ reader could not be opened · reinstall the Document Lab");
}

QString mrzDate(const std::string& yymmdd, const std::optional<doc_vision::MrzDate>& date) {
    // A fixed table, never QLocale: Review shows English month names whatever the locale (spec §3.4).
    static constexpr const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (!date) {
        return QString::fromStdString(yymmdd);
    }
    if (date->month < 1 || date->month > 12) {
        return QString::number(date->year);
    }
    const QString month = QString::fromLatin1(kMonths[date->month - 1]);
    if (date->day < 1) {
        return QStringLiteral("%1 %2").arg(month).arg(date->year);
    }
    return QStringLiteral("%1 %2 %3").arg(date->day).arg(month).arg(date->year);
}

QString expiredWarning(const doc_vision::MrzDate& expiry) {
    return tr("Expired on %1. A5 and later steps flag this; the scan itself still succeeded.")
        .arg(mrzDate({}, expiry));
}

QString stillName(std::uint64_t frameNumber) {
    return QStringLiteral("lab passport frame %1").arg(frameNumber + 1);
}

QString chipLabel(doc_vision::Gate gate) {
    using doc_vision::Gate;
    switch (gate) {
    case Gate::InView:
        return tr("In view");
    case Gate::Size:
        return tr("Size");
    case Gate::Flat:
        return tr("Flat");
    case Gate::Sharp:
        return tr("Sharp");
    case Gate::Glare:
        return tr("No glare");
    case Gate::Light:
        break;
    }
    return tr("Light");
}

QString stateWord(doc_vision::Hint shown) {
    using doc_vision::Hint;
    switch (shown) {
    case Hint::NoPage:
        return tr("searching");
    case Hint::CutByEdge:
    case Hint::MoveToMiddle:
        return tr("adjust (in view)");
    case Hint::TooFar:
    case Hint::TooClose:
        return tr("adjust (size)");
    case Hint::Tilted:
        return tr("adjust (flat)");
    case Hint::Blurred:
        return tr("adjust (sharpness)");
    case Hint::Glare:
        return tr("adjust (glare)");
    case Hint::TooDark:
    case Hint::TooBright:
        return tr("adjust (light)");
    case Hint::NoMrzBand:
    case Hint::CannotReadMrz:
        return tr("reading");
    case Hint::Good:
        break;
    }
    return tr("good");
}

} // namespace document_lab
