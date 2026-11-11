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
        return {doc_vision::Tone::Neutral, true, tr("Show the passport's photo page"),
                tr("Hold it so the whole page is in view")};
    case Hint::CutByEdge:
        return bad("Move back", "the whole page must be in view");
    case Hint::MoveToMiddle:
        return bad("Move the page to the middle", "the whole page must be in view");
    case Hint::TooFar:
        return bad("Move closer");
    case Hint::TooClose:
        return bad("Move a little further away");
    case Hint::Tilted:
        return bad("Hold the page flat to the camera");
    case Hint::Blurred:
        return bad("Blurred: hold still", "or move a little closer or further away");
    case Hint::Glare:
        return bad("Glare on the page", "tilt it slightly away from the light");
    case Hint::TooDark:
        return bad("Too dark", "add more light");
    case Hint::TooBright:
        return bad("Too bright", "move out of direct light");
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
        return bad("This camera can't focus this close", "Move the page a little further away");
    case FocusBanner::CannotMakeSharp:
        return bad("This camera can't make the page sharp", "Try more light, or another camera");
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
    case Hint::Good:
        break;
    }
    return tr("good");
}

} // namespace document_lab
