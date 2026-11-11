// The README's labelled picture of the Document Lab, made by hand: document_lab_picture [out.png]
//
// The real window and the real page model, on the drawn page of procedural_page.h (no real document), with
// fake cameras and fake senders, so nothing reaches a camera or a viewer. Each part is outlined and labelled
// where the window actually put it; README.md says what each number is for. Run it on the platform whose look
// the picture should have (macOS for the README), not offscreen; it takes the system's light or dark
// appearance.

#include "fake_camera_backend.h"
#include "fake_sender.h"
#include "list_source.h"
#include "main_window.h"
#include "procedural_page.h"

#include "application_identity.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct Part {
    int number;
    QString name;
    QRect area;             // in the window's coordinates
    QPoint anchor;          // where the label's top-left corner goes
    bool fromRight = false; // the anchor is the label's top-right corner instead
};

// The drawn page, named as a file would be in the status bar.
class DrawnPage final : public test_support::ListSource {
public:
    explicit DrawnPage(std::vector<frame_io::ReadResult> reads)
        : ListSource(std::move(reads), true, nullptr) {}
    std::string describe() const override { return "drawn page"; }
};

QRect areaOf(const QWidget* window, const QWidget* widget) {
    return {widget->mapTo(window, QPoint(0, 0)), widget->size()};
}

// The union of a row's widgets, with the label in front of them when there is one.
QRect rowOf(const QMainWindow& window, const QString& label, const std::vector<const char*>& names) {
    QRect area;
    for (const char* name : names) {
        if (const auto* widget = window.findChild<QWidget*>(QLatin1String(name));
            widget && widget->isVisible()) {
            area = area.united(areaOf(&window, widget));
        }
    }
    for (const QLabel* text : window.findChildren<QLabel*>()) {
        if (!label.isEmpty() && text->text() == label) {
            area = area.united(areaOf(&window, text));
        }
    }
    return area;
}

// Where an image point falls on a canvas that fits the whole image, centred, as the canvas does at first.
QPointF fittedPoint(const QRect& canvas, QSize image, cv::Point2d point) {
    const double scale = std::min(static_cast<double>(canvas.width()) / image.width(),
                                  static_cast<double>(canvas.height()) / image.height());
    const double left = canvas.left() + (canvas.width() - image.width() * scale) / 2.0;
    const double top = canvas.top() + (canvas.height() - image.height() * scale) / 2.0;
    return {left + point.x * scale, top + point.y * scale};
}

void label(QPainter& painter, const QFont& font, const Part& part) {
    const QColor accent(0xF7, 0x67, 0x07);
    painter.setPen(QPen(accent, 2.5));
    painter.setBrush(Qt::NoBrush);
    if (!part.area.isEmpty()) {
        painter.drawRoundedRect(QRectF(part.area).adjusted(-3, -3, 3, 3), 6, 6);
    }
    painter.setFont(font);
    const QString text = QStringLiteral("%1  %2").arg(part.number).arg(part.name);
    const QFontMetrics metrics(font);
    const QSizeF size(metrics.horizontalAdvance(text) + 16, metrics.height() + 6);
    const QPointF topLeft =
        part.fromRight ? QPointF(part.anchor) - QPointF(size.width(), 0) : QPointF(part.anchor);
    const QRectF box(topLeft, size);
    QPainterPath pill;
    pill.addRoundedRect(box, box.height() / 2, box.height() / 2);
    painter.fillPath(pill, accent);
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, text);
}

} // namespace

int main(int argc, char* argv[]) {
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("document-lab.png");
    QApplication application(argc, argv);
    // Settings in a folder of their own: the picture shows the defaults, and the owner's own settings are
    // neither read nor changed.
    const QTemporaryDir settingsFolder;
    const QTemporaryDir labFolder;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsFolder.path());
    qt_common::setApplicationIdentity(QStringLiteral("Document Lab Picture"));

    document_lab::LabDependencies dependencies;
    auto cameras = std::make_unique<test_support::FakeCameraBackend>();
    cameras->list = {
        {.id = "camera",
         .name = QStringLiteral("USB Camera"),
         .front = false,
         .formats = {
             {.size = QSize(1280, 720), .maxFps = 30, .pixelFormat = QVideoFrameFormat::Format_NV12}}}};
    dependencies.cameras = std::move(cameras);
    dependencies.makeSender = [](const std::string&) -> std::unique_ptr<visual_debug::ImageSender> {
        return std::make_unique<test_support::RecordingSender>();
    };
    dependencies.makeLinkSender = [](const std::string&) -> std::unique_ptr<visual_debug::ImageSender> {
        auto sender = std::make_unique<test_support::RecordingSender>();
        sender->pingAnswer = [] { return visual_debug::ViewerState::Listening; };
        return sender;
    };
    dependencies.makeFocusControl = [](const QByteArray&) {
        return std::unique_ptr<lab_core::FocusControl>{};
    };
    dependencies.linkMode = lab_core::ViewerLink::Mode::Immediate;
    dependencies.engineMode = lab_core::Engine::Mode::Immediate;
    dependencies.conversionMode = LiveConverter::Mode::Immediate;
    dependencies.senderMode = lab_core::ResultSender::Mode::Immediate;
    dependencies.recorderMode = frame_io::RecordingWriter::Mode::Immediate;
    dependencies.immediateCapture = true;
    dependencies.confirm = [](const QString&) { return true; };
    dependencies.labFolder = labFolder.path();
    dependencies.modelFolder =
        QString::fromStdU16String(std::filesystem::path(ID_VISION_LAB_LC100_MODEL).parent_path().u16string());

    document_lab::LabWindow window(std::move(dependencies));
    window.resize(1360, 860);
    window.show();
    QApplication::processEvents();

    auto* processor = window.findChild<QComboBox*>(QStringLiteral("processorCombo"));
    processor->setCurrentIndex(processor->findText(QStringLiteral("Document")));
    window.findChild<QAbstractButton*>(QStringLiteral("asFastCheck"))->setChecked(true);

    // A second and a half of the drawn page, 33 ms apart by the frames' own times: long enough for every
    // check to pass and the banner to say so.
    const cv::Size frameSize(1280, 720);
    const cv::Mat frame =
        test_support::pageInFrame(test_support::proceduralPage(), test_support::kPageQuad, frameSize);
    std::vector<frame_io::ReadResult> reads;
    constexpr int kFrames = 45;
    for (int i = 0; i < kFrames; ++i) {
        reads.push_back({.status = frame_io::ReadStatus::Frame,
                         .frame = test_support::listFrame(static_cast<std::uint64_t>(i), i * 33ms, frame),
                         .error = {}});
    }
    window.startReplay([reads = std::move(reads)](std::string&) -> std::unique_ptr<frame_io::ReplaySource> {
        return std::make_unique<DrawnPage>(reads);
    });
    for (int i = 0; i < 4 * kFrames; ++i) {
        window.pumpForTests();
        QApplication::processEvents();
    }

    const QRect canvas = areaOf(&window, window.findChild<QWidget*>(QStringLiteral("canvas")));
    const QSize image(frameSize.width, frameSize.height);
    const QPointF corner = fittedPoint(canvas, image, test_support::kPageQuad[0]);
    QRectF outline;
    for (const cv::Point2d& point : test_support::kPageQuad) {
        const QPointF at = fittedPoint(canvas, image, point);
        outline = outline.isNull() ? QRectF(at, QSizeF(1, 1)) : outline.united(QRectF(at, QSizeF(1, 1)));
    }

    const QRect source =
        rowOf(window, QStringLiteral("Camera:"),
              {"cameraCombo", "formatCombo", "startStopButton", "openFileButton", "openFolderButton"});
    const QRect replay =
        rowOf(window, {},
              {"playPauseButton", "stepBackButton", "stepButton", "back10Button", "forward10Button",
               "restartButton", "loopCheck", "asFastCheck", "positionSlider", "positionLabel"});
    const QRect processing =
        rowOf(window, QStringLiteral("Processor:"), {"processorCombo", "showCombo", "overlaysCheck"});
    const QRect recording =
        rowOf(window, {}, {"recordButton", "captureButton", "openLabFolderButton", "chooseLabFolderButton"});
    const QRect viewer = rowOf(window, QStringLiteral("Viewer:"),
                               {"endpointEdit", "viewerIndicator", "viewerStateLabel", "streamButton",
                                "pauseStreamButton", "stopStreamButton", "sendFrameButton"});
    const QRect banner = areaOf(&window, window.findChild<QWidget*>(QStringLiteral("scanBanner")));
    const QRect chips = areaOf(&window, window.findChild<QWidget*>(QStringLiteral("gateChips")));
    const QRect results = areaOf(&window, window.findChild<QWidget*>(QStringLiteral("resultsList")));
    const QRect status = areaOf(&window, window.statusBar());

    // Each label beside its part, where it covers least of what it names.
    const std::vector<Part> parts{
        {1, QStringLiteral("Source"), source, source.topRight() + QPoint(16, 0)},
        {2, QStringLiteral("Replay"), replay, replay.topRight() + QPoint(16, 0)},
        {3, QStringLiteral("Processor"), processing, processing.topRight() + QPoint(16, 0)},
        {4, QStringLiteral("Record and capture"), recording, recording.topLeft() - QPoint(190, 0)},
        {5, QStringLiteral("Viewer"), viewer, viewer.topRight() + QPoint(16, 0)},
        {6, QStringLiteral("Canvas"), canvas, canvas.bottomLeft() + QPoint(12, -64)},
        {7, QStringLiteral("Banner"), banner, banner.topRight() + QPoint(14, 8)},
        {8, QStringLiteral("Guide outline"), outline.toAlignedRect(), corner.toPoint() + QPoint(16, 16)},
        {9, QStringLiteral("Chips"), chips, chips.topRight() + QPoint(14, 6)},
        {10, QStringLiteral("Results"), results, results.topLeft() + QPoint(12, results.height() / 2)},
        {11, QStringLiteral("Status bar"), status, status.topRight() + QPoint(-8, 2), true},
    };

    QPixmap picture = window.grab();
    QPainter painter(&picture);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font = QApplication::font();
    font.setBold(true);
    for (const Part& part : parts) {
        label(painter, font, part);
    }
    painter.end();
    if (!picture.save(out)) {
        std::fprintf(stderr, "could not write %s\n", out.toLocal8Bit().constData());
        return 1;
    }
    std::printf("wrote %s (%d x %d)\n", out.toLocal8Bit().constData(), picture.width(), picture.height());
    return 0;
}
