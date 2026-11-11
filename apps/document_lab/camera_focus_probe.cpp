// The camera focus spike (11 November): what Qt Multimedia lets the Document Lab do about a camera's focus,
// and what each focus setting does to a page's sharpness. Not part of the lab; it lives on its spike branch.
//
//   camera_focus_probe --list [--no-prompt]
//       Every camera: its focus features and modes as Qt reports them, before and after it starts.
//       --no-prompt starts no camera unless the camera may already be used, so nobody is asked; --no-start
//       starts none.
//   camera_focus_probe [--camera <number or part of its name>] [--model <file>] [--out <file>]
//       A window with the camera's picture and the page's outline. Hold the page still and press Space: each
//       focus setting the camera offers is tried for 5 s (2 s to settle, 3 to measure). Then press Space and
//       move the page slowly from near to far for 20 s. Esc ends it. Where the camera's own focus can be set
//       (UVC on macOS, DirectShow on Windows), the probe also focuses on the page itself five times between
//       the two; --search does only that.
//
// It prints aggregates only (how often the page was found, its sharpness and its size), never a frame or
// anything read from the page; the report also goes to --out (by default a file in the system's temporary
// folder).

#include "camera_backend.h"
#include "camera_formats.h"
#include "camera_frame.h"
#include "model_folder.h"
#include "uvc_controls.h"

#include "doc_vision/page_detector.h"
#include "doc_vision/quality_gates.h"
#include "lab_core/processors.h"

#include <QApplication>
#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QPixmap>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWidget>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#endif

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSettleMs = 2000;
constexpr int kMeasureMs = 3000;
constexpr int kSweepMs = 20000;
// Our own focus on the page: trials, focus steps, and how long a step waits before its frames count.
constexpr int kTrials = 5;
constexpr int kCoarseStep = 10;
constexpr int kFineStep = 5;
constexpr int kStepSettleMs = 150;
constexpr int kStepTimeoutMs = 800;
constexpr int kDefocusMs = 800;
constexpr int kHoldMs = 1500;

// Printed at once and kept for the report file.
class Report {
public:
    void say(const QString& line) {
        const QByteArray utf8 = (line + QLatin1Char('\n')).toUtf8();
        std::fputs(utf8.constData(), stdout);
        std::fflush(stdout);
        m_text += utf8;
    }
    bool writeTo(const QString& path) const {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        return file.write(m_text) == m_text.size();
    }

private:
    QByteArray m_text;
};

QString modeName(QCamera::FocusMode mode) {
    switch (mode) {
    case QCamera::FocusModeAuto:
        return QStringLiteral("Auto");
    case QCamera::FocusModeAutoNear:
        return QStringLiteral("AutoNear");
    case QCamera::FocusModeAutoFar:
        return QStringLiteral("AutoFar");
    case QCamera::FocusModeHyperfocal:
        return QStringLiteral("Hyperfocal");
    case QCamera::FocusModeInfinity:
        return QStringLiteral("Infinity");
    case QCamera::FocusModeManual:
        return QStringLiteral("Manual");
    }
    return QStringLiteral("?");
}

const std::vector<QCamera::FocusMode> kModes{QCamera::FocusModeAuto,     QCamera::FocusModeAutoNear,
                                             QCamera::FocusModeAutoFar,  QCamera::FocusModeHyperfocal,
                                             QCamera::FocusModeInfinity, QCamera::FocusModeManual};

QString describe(const QCamera& camera) {
    const QCamera::Features features = camera.supportedFeatures();
    QStringList named;
    const std::vector<std::pair<QCamera::Feature, const char*>> all{
        {QCamera::Feature::CustomFocusPoint, "custom focus point"},
        {QCamera::Feature::FocusDistance, "focus distance"},
        {QCamera::Feature::ColorTemperature, "colour temperature"},
        {QCamera::Feature::ExposureCompensation, "exposure compensation"},
        {QCamera::Feature::IsoSensitivity, "ISO"},
        {QCamera::Feature::ManualExposureTime, "manual exposure time"}};
    for (const auto& [feature, name] : all) {
        if (features.testFlag(feature)) {
            named << QString::fromLatin1(name);
        }
    }
    QStringList modes;
    for (const QCamera::FocusMode mode : kModes) {
        modes << modeName(mode) +
                     (camera.isFocusModeSupported(mode) ? QStringLiteral(" yes") : QStringLiteral(" no"));
    }
    return QStringLiteral(
               "    features: %1\n    focus modes: %2\n    now: mode %3, focus distance %4, focus point "
               "(%5, %6), custom focus point (%7, %8), zoom %9 to %10")
        .arg(named.isEmpty() ? QStringLiteral("none") : named.join(QStringLiteral(", ")),
             modes.join(QStringLiteral(", ")), modeName(camera.focusMode()))
        .arg(camera.focusDistance(), 0, 'f', 2)
        .arg(camera.focusPoint().x(), 0, 'f', 2)
        .arg(camera.focusPoint().y(), 0, 'f', 2)
        .arg(camera.customFocusPoint().x(), 0, 'f', 2)
        .arg(camera.customFocusPoint().y(), 0, 'f', 2)
        .arg(camera.minimumZoomFactor(), 0, 'f', 2)
        .arg(camera.maximumZoomFactor(), 0, 'f', 2);
}

QString positionName(QCameraDevice::Position position) {
    switch (position) {
    case QCameraDevice::FrontFace:
        return QStringLiteral("front");
    case QCameraDevice::BackFace:
        return QStringLiteral("back");
    case QCameraDevice::UnspecifiedPosition:
        break;
    }
    return QStringLiteral("unspecified");
}

// Calls `answer` with whether a camera may be used; asks only when `mayAsk` and nobody has answered yet.
void withCameraPermission(bool mayAsk, std::function<void(bool)> answer) {
    Q_UNUSED(mayAsk);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#if QT_CONFIG(permissions)
    const QCameraPermission permission;
    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Granted:
        answer(true);
        return;
    case Qt::PermissionStatus::Denied:
        answer(false);
        return;
    case Qt::PermissionStatus::Undetermined:
        if (!mayAsk) {
            answer(false);
            return;
        }
        qApp->requestPermission(permission, qApp, [answer = std::move(answer)](const QPermission& asked) {
            answer(asked.status() == Qt::PermissionStatus::Granted);
        });
        return;
    }
#endif
#endif
    answer(true);
}

// Runs the event loop until `done` or `ms` have passed.
void waitUntil(int ms, const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

int listCameras(bool mayAsk, bool start, Report& report) {
    const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
    report.say(QStringLiteral("%1 camera(s); Qt %2, platform %3")
                   .arg(devices.size())
                   .arg(QString::fromLatin1(qVersion()), QApplication::platformName()));
    bool answered = false;
    bool permitted = false;
    withCameraPermission(mayAsk, [&](bool granted) {
        permitted = granted;
        answered = true;
    });
    waitUntil(120000, [&] { return answered; });
    for (qsizetype i = 0; i < devices.size(); ++i) {
        const QCameraDevice& device = devices[i];
        QSize largest;
        for (const QCameraFormat& format : device.videoFormats()) {
            if (format.resolution().width() * format.resolution().height() >
                largest.width() * largest.height()) {
                largest = format.resolution();
            }
        }
        report.say(
            QStringLiteral("\n[%1] %2 (%3), %4, %5 formats, largest %6 x %7%8")
                .arg(i)
                .arg(device.description(), QString::fromUtf8(device.id()), positionName(device.position()))
                .arg(device.videoFormats().size())
                .arg(largest.width())
                .arg(largest.height())
                .arg(device.isDefault() ? QStringLiteral(", the default") : QString()));
        QCamera camera(device);
        report.say(QStringLiteral("  before starting:\n") + describe(camera));
        QString why;
        if (const std::unique_ptr<uvc::Camera> controls = uvc::Camera::forQtId(device.id(), why)) {
            report.say(controls->summary());
        } else {
            report.say(QStringLiteral("    native focus control: ") + why);
        }
        if (!start || !permitted) {
            report.say(start ? QStringLiteral("  not started: no camera permission")
                             : QStringLiteral("  not started"));
            continue;
        }
        QVideoSink sink;
        QMediaCaptureSession session; // goes before the sink
        session.setCamera(&camera);
        session.setVideoSink(&sink);
        int frames = 0;
        QString error;
        QObject::connect(&sink, &QVideoSink::videoFrameChanged, &sink, [&frames] { ++frames; });
        QObject::connect(&camera, &QCamera::errorOccurred, &camera,
                         [&error](QCamera::Error, const QString& reason) { error = reason; });
        camera.start();
        waitUntil(6000, [&] { return frames >= 10 || !error.isEmpty(); });
        waitUntil(1000, [] { return false; }); // time to report what changed once it runs
        report.say(QStringLiteral("  after starting (%1 frames%2):\n")
                       .arg(frames)
                       .arg(error.isEmpty() ? QString() : QStringLiteral(", error: ") + error) +
                   describe(camera));
        camera.stop();
    }
    return 0;
}

// One frame's look at the page, made on the analyser's thread.
struct Analysis {
    bool found = false;
    doc_vision::Measures measures{};
    QPointF pageCentre{0.5, 0.5}; // as shares of the frame
    QImage picture;
    QString error;
};

// Finds the page in the newest frame on its own thread; a frame that comes while one is analysed replaces the
// waiting one.
class Analyser {
public:
    Analyser(std::filesystem::path model, std::function<void(Analysis)> deliver)
        : m_model(std::move(model)), m_deliver(std::move(deliver)), m_thread([this] { run(); }) {}
    ~Analyser() {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_one();
        m_thread.join();
    }
    Analyser(const Analyser&) = delete;
    Analyser& operator=(const Analyser&) = delete;

    void offer(const QVideoFrame& frame, bool front) {
        {
            const std::lock_guard lock(m_mutex);
            m_frame = frame;
            m_front = front;
        }
        m_wake.notify_one();
    }

private:
    void run() {
        std::string openError;
        const std::unique_ptr<doc_vision::PageDetector> detector =
            doc_vision::PageDetector::open(m_model, openError);
        if (!detector) {
            Analysis failed;
            failed.error =
                QStringLiteral("The page model did not open: ") + QString::fromStdString(openError);
            m_deliver(std::move(failed));
            return;
        }
        const Clock::time_point start = Clock::now();
        for (;;) {
            QVideoFrame frame;
            bool front = false;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stopping || m_frame.has_value(); });
                if (m_stopping) {
                    return;
                }
                frame = *m_frame;
                m_frame.reset();
                front = m_front;
            }
            m_deliver(analyse(*detector, frame, front, Clock::now() - start));
        }
    }

    static Analysis analyse(doc_vision::PageDetector& detector, const QVideoFrame& frame, bool front,
                            Clock::duration at) {
        Analysis analysis;
        std::string error;
        const std::optional<frame_io::Frame> converted = document_lab::toFrame(
            frame, {.frontCamera = front}, std::chrono::duration_cast<std::chrono::nanoseconds>(at), error);
        if (!converted) {
            analysis.error = QString::fromStdString(error);
            return analysis;
        }
        const cv::Mat bgr = doc_vision::toBgr8(converted->image);
        if (bgr.empty()) {
            analysis.error = QStringLiteral("a frame that is not 8- or 16-bit");
            return analysis;
        }
        const std::optional<doc_vision::PageFound> page = detector.find(bgr, error);
        const double scale = std::min(1.0, 960.0 / bgr.cols);
        cv::Mat shown;
        cv::resize(bgr, shown, cv::Size(), scale, scale, cv::INTER_AREA);
        if (page) {
            analysis.found = true;
            analysis.measures = doc_vision::measure(bgr, page->quad);
            std::vector<cv::Point> outline;
            double x = 0.0;
            double y = 0.0;
            for (const cv::Point2d& corner : page->quad) {
                outline.emplace_back(cvRound(corner.x * scale), cvRound(corner.y * scale));
                x += corner.x / 4.0;
                y += corner.y / 4.0;
            }
            analysis.pageCentre = QPointF(x / bgr.cols, y / bgr.rows);
            cv::polylines(shown, outline, true, cv::Scalar(0, 220, 0), 3, cv::LINE_AA);
        } else if (!error.empty()) {
            analysis.error = QString::fromStdString(error);
        }
        cv::Mat rgb;
        cv::cvtColor(shown, rgb, cv::COLOR_BGR2RGB);
        analysis.picture =
            QImage(rgb.data, rgb.cols, rgb.rows, static_cast<qsizetype>(rgb.step), QImage::Format_RGB888)
                .copy();
        return analysis;
    }

    std::filesystem::path m_model;
    std::function<void(Analysis)> m_deliver;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<QVideoFrame> m_frame;
    bool m_front = false;
    bool m_stopping = false;
    std::thread m_thread; // last: starts once everything it uses is made
};

struct Sample {
    bool found = false;
    double sharpness = 0.0;
    double longerSide = 0.0;
};

double quantile(std::vector<double> values, double q) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(q * static_cast<double>(values.size() - 1))];
}

class ProbeWindow final : public QWidget {
public:
    ProbeWindow(const QCameraDevice& device, std::filesystem::path model, QString outPath, bool searchOnly,
                Report& report)
        : m_outPath(std::move(outPath)), m_report(report), m_searchOnly(searchOnly) {
        setWindowTitle(QStringLiteral("Camera Focus Probe: ") + device.description());
        auto* layout = new QVBoxLayout(this);
        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        QFont font = m_status->font();
        font.setPointSizeF(font.pointSizeF() * 1.4);
        m_status->setFont(font);
        m_picture = new QLabel(this);
        m_picture->setMinimumSize(960, 540);
        m_picture->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_status);
        layout->addWidget(m_picture, 1);

        const doc_vision::Thresholds thresholds;
        m_report.say(
            QStringLiteral("\nCamera: %1 (%2), %3; the Sharp gate fails under %4 and passes again from %5")
                .arg(device.description(), QString::fromUtf8(device.id()), positionName(device.position()))
                .arg(thresholds.sharpness.failsAt)
                .arg(thresholds.sharpness.passesAgainAt));

        m_camera = std::make_unique<QCamera>(device);
        std::vector<document_lab::CameraFormat> formats;
        for (const QCameraFormat& format : device.videoFormats()) {
            formats.push_back({.size = format.resolution(),
                               .maxFps = format.maxFrameRate(),
                               .pixelFormat = format.pixelFormat()});
        }
        if (const std::optional<document_lab::CameraFormat> chosen = document_lab::defaultFormat(formats)) {
            for (const QCameraFormat& format : device.videoFormats()) {
                if (format.resolution() == chosen->size && format.maxFrameRate() == chosen->maxFps &&
                    format.pixelFormat() == chosen->pixelFormat) {
                    m_camera->setCameraFormat(format);
                    m_report.say(QStringLiteral("Format: %1 x %2 at up to %3 fps")
                                     .arg(chosen->size.width())
                                     .arg(chosen->size.height())
                                     .arg(chosen->maxFps, 0, 'f', 1));
                    break;
                }
            }
        }
        m_report.say(QStringLiteral("  before starting:\n") + describe(*m_camera));
        QString why;
        m_uvc = uvc::Camera::forQtId(device.id(), why);
        if (m_uvc) {
            m_report.say(m_uvc->summary());
            m_uvcAutoAtStart = m_uvc->autoFocus();
            m_uvcFocusAtStart = m_uvc->focus();
        } else {
            m_report.say(QStringLiteral("    native focus control: ") + why);
        }
        m_session.setCamera(m_camera.get());
        m_session.setVideoSink(&m_sink);
        const bool front = device.position() == QCameraDevice::FrontFace;
        connect(&m_sink, &QVideoSink::videoFrameChanged, this, [this, front](const QVideoFrame& frame) {
            if (m_analyser) {
                m_analyser->offer(frame, front);
            }
        });
        connect(m_camera.get(), &QCamera::errorOccurred, this, [this](QCamera::Error, const QString& reason) {
            m_report.say(QStringLiteral("Camera error: ") + reason);
        });
        connect(m_camera.get(), &QCamera::supportedFeaturesChanged, this,
                [this] { m_report.say(QStringLiteral("  features changed:\n") + describe(*m_camera)); });
        m_analyser = std::make_unique<Analyser>(std::move(model), [this](Analysis analysis) {
            QMetaObject::invokeMethod(
                this, [this, analysis = std::move(analysis)]() mutable { take(std::move(analysis)); },
                Qt::QueuedConnection);
        });
        m_camera->start();
        m_clock.start();
        connect(&m_ticker, &QTimer::timeout, this, [this] { tick(); });
        m_ticker.start(100);
        QTimer::singleShot(3000, this,
                           [this] { m_report.say(QStringLiteral("  running:\n") + describe(*m_camera)); });
    }

    ~ProbeWindow() override {
        m_analyser.reset(); // no analysis arrives once the window starts to go
        restoreUvc();
        m_camera->stop();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            finish();
            close();
            return;
        }
        if (event->key() != Qt::Key_Space) {
            QWidget::keyPressEvent(event);
            return;
        }
        if (m_phase == Phase::Aim && m_searchOnly) {
            if (canSearch()) {
                startSearch();
            } else {
                m_report.say(QStringLiteral("No native focus control to search with."));
                finish();
            }
        } else if (m_phase == Phase::Aim) {
            startSettings();
        } else if (m_phase == Phase::SearchReady) {
            startSearch();
        } else if (m_phase == Phase::SweepReady) {
            m_report.say(QStringLiteral("\nSweep: near to far for %1 s, focus mode %2")
                             .arg(kSweepMs / 1000)
                             .arg(modeName(m_camera->focusMode())));
            m_phase = Phase::Sweep;
            m_samples.clear();
            m_phaseClock.start();
        }
    }

private:
    enum class Phase { Aim, Settings, SearchReady, Search, SweepReady, Sweep, Done };
    enum class SearchStage { Defocus, Coarse, Fine, Hold };

    struct Step {
        int position = 0;
        double sharpness = 0.0;
    };

    struct Setting {
        QString name;
        std::function<void(QCamera&)> apply;
    };

    void take(Analysis analysis) {
        if (!analysis.error.isEmpty() && analysis.error != m_lastError) {
            m_lastError = analysis.error;
            m_report.say(QStringLiteral("Analysis: ") + analysis.error);
        }
        if (!analysis.picture.isNull()) {
            m_picture->setPixmap(QPixmap::fromImage(analysis.picture));
        }
        ++m_framesAnalysed;
        if (analysis.found) {
            m_pageCentre = analysis.pageCentre;
        }
        m_live = analysis.found ? QStringLiteral("Page found: sharpness %1, longer side %2% of the frame")
                                      .arg(analysis.measures.sharpness, 0, 'f', 0)
                                      .arg(analysis.measures.longerSide * 100.0, 0, 'f', 0)
                                : QStringLiteral("No page found");
        const Sample sample{analysis.found, analysis.measures.sharpness, analysis.measures.longerSide};
        if (m_phase == Phase::Settings && m_phaseClock.elapsed() >= kSettleMs) {
            m_samples.push_back(sample);
        } else if (m_phase == Phase::Sweep) {
            m_samples.push_back(sample);
        } else if (m_phase == Phase::Search) {
            searchTake(analysis);
        }
        showStatus();
    }

    void startSettings() {
        QCamera& camera = *m_camera;
        m_startMode = camera.focusMode();
        m_report.say(QStringLiteral("  at the start of the settings:\n") + describe(camera));
        m_settings.clear();
        m_settings.push_back({QStringLiteral("as started (%1)").arg(modeName(m_startMode)), [](QCamera&) {}});
        for (const QCamera::FocusMode mode : kModes) {
            if (mode != QCamera::FocusModeManual && camera.isFocusModeSupported(mode)) {
                m_settings.push_back({QStringLiteral("mode %1").arg(modeName(mode)),
                                      [mode](QCamera& c) { c.setFocusMode(mode); }});
            }
        }
        const QCamera::Features features = camera.supportedFeatures();
        if (features.testFlag(QCamera::Feature::CustomFocusPoint)) {
            m_settings.push_back({QStringLiteral("Auto, point at the frame centre"), [](QCamera& c) {
                                      if (c.isFocusModeSupported(QCamera::FocusModeAuto)) {
                                          c.setFocusMode(QCamera::FocusModeAuto);
                                      }
                                      c.setCustomFocusPoint(QPointF(0.5, 0.5));
                                  }});
            m_settings.push_back({QStringLiteral("Auto, point at the page centre"), [this](QCamera& c) {
                                      if (c.isFocusModeSupported(QCamera::FocusModeAuto)) {
                                          c.setFocusMode(QCamera::FocusModeAuto);
                                      }
                                      c.setCustomFocusPoint(m_pageCentre);
                                  }});
        }
        if (features.testFlag(QCamera::Feature::FocusDistance) &&
            camera.isFocusModeSupported(QCamera::FocusModeManual)) {
            for (int tenth = 0; tenth <= 10; ++tenth) {
                const float distance = static_cast<float>(tenth) / 10.0f;
                m_settings.push_back(
                    {QStringLiteral("Manual, distance %1").arg(distance, 0, 'f', 1), [distance](QCamera& c) {
                         c.setFocusMode(QCamera::FocusModeManual);
                         c.setFocusDistance(distance);
                     }});
            }
        }
        if (m_uvc && m_uvc->hasAutoFocus()) {
            m_settings.push_back({QStringLiteral("native auto focus on"), [this](QCamera&) {
                                      m_uvcChanged = true;
                                      m_uvc->setAutoFocus(true);
                                  }});
        }
        if (m_uvc && m_uvc->hasAbsoluteFocus()) {
            if (const std::optional<uvc::FocusRange> range = m_uvc->focusRange();
                range && range->maximum > range->minimum) {
                const int step = std::max(1, range->resolution);
                for (int k = 0; k <= 10; ++k) {
                    const int raw = range->minimum + (range->maximum - range->minimum) * k / 10;
                    const int position = range->minimum + (raw - range->minimum) / step * step;
                    m_settings.push_back(
                        {QStringLiteral("native manual focus %1").arg(position), [this, position](QCamera&) {
                             m_uvcChanged = true;
                             if (m_uvc->hasAutoFocus()) {
                                 m_uvc->setAutoFocus(false);
                             }
                             m_uvc->setFocus(position);
                         }});
                }
            }
        }
        m_report.say(QStringLiteral("\nSettings, the page held still: %1 s to settle, %2 s measured each")
                         .arg(kSettleMs / 1000)
                         .arg(kMeasureMs / 1000));
        m_report.say(QStringLiteral("  %1 %2 %3 %4 %5 %6 %7 %8   %9")
                         .arg(QStringLiteral("setting"), -34)
                         .arg(QStringLiteral("frames"), 6)
                         .arg(QStringLiteral("found"), 6)
                         .arg(QStringLiteral("p10"), 6)
                         .arg(QStringLiteral("p50"), 6)
                         .arg(QStringLiteral("p90"), 6)
                         .arg(QStringLiteral("max"), 6)
                         .arg(QStringLiteral("side"), 5)
                         .arg(QStringLiteral("reported (mode, distance, focus point)")));
        m_settingIndex = 0;
        m_phase = Phase::Settings;
        applySetting();
    }

    void applySetting() {
        m_samples.clear();
        m_settings[m_settingIndex].apply(*m_camera);
        m_phaseClock.start();
    }

    void tick() {
        if (m_phase == Phase::Settings && m_phaseClock.elapsed() >= kSettleMs + kMeasureMs) {
            reportSetting();
            if (++m_settingIndex < m_settings.size()) {
                applySetting();
            } else {
                m_camera->setFocusMode(m_startMode);
                restoreUvc();
                m_phase = canSearch() ? Phase::SearchReady : Phase::SweepReady;
            }
        } else if (m_phase == Phase::Search) {
            searchTick();
        } else if (m_phase == Phase::Sweep && m_phaseClock.elapsed() >= kSweepMs) {
            reportSweep();
            finish();
        }
        showStatus();
    }

    bool canSearch() {
        if (!m_uvc || !m_uvc->hasAbsoluteFocus()) {
            return false;
        }
        const std::optional<uvc::FocusRange> range = m_uvc->focusRange();
        return range && range->maximum > range->minimum;
    }

    // Our own focus on the page: the camera's focus set by hand, every kCoarseStep from one end, then every
    // kFineStep around the best, judged by the page's sharpness in the first frame kStepSettleMs after each
    // move.
    void startSearch() {
        m_range = *m_uvc->focusRange();
        m_report.say(
            QStringLiteral("\nFocus on the page, %1 trials, the page held still: every %2 from one end, then "
                           "every %3 around the best; a step's frame counts from %4 ms after the move")
                .arg(kTrials)
                .arg(std::max(kCoarseStep, m_range.resolution))
                .arg(std::max(kFineStep, m_range.resolution))
                .arg(kStepSettleMs));
        m_report.say(QStringLiteral("  %1 %2 %3 %4 %5 %6 %7 %8 %9")
                         .arg(QStringLiteral("trial"), -6)
                         .arg(QStringLiteral("from"), 5)
                         .arg(QStringLiteral("steps"), 6)
                         .arg(QStringLiteral("best"), 5)
                         .arg(QStringLiteral("peak"), 6)
                         .arg(QStringLiteral("time s"), 7)
                         .arg(QStringLiteral("held p10"), 9)
                         .arg(QStringLiteral("held p50"), 9)
                         .arg(QStringLiteral("held found"), 11));
        m_trial = 0;
        m_phase = Phase::Search;
        startTrial();
    }

    void startTrial() {
        m_curve.clear();
        m_uvcChanged = true;
        if (m_uvc->hasAutoFocus()) {
            m_uvc->setAutoFocus(false);
        }
        m_trialStart = m_trial % 2 == 0 ? m_range.maximum : m_range.minimum;
        moveTo(m_trialStart);
        m_stage = SearchStage::Defocus;
    }

    void moveTo(int position) {
        m_uvc->setFocus(position);
        m_position = position;
        m_stepClock.start();
    }

    void beginStage(SearchStage stage, std::vector<int> positions) {
        m_stage = stage;
        m_positions = std::move(positions);
        m_positionIndex = 0;
        if (m_positions.empty()) {
            holdBest();
        } else {
            moveTo(m_positions.front());
        }
    }

    void searchTake(const Analysis& analysis) {
        if (m_stepClock.elapsed() < kStepSettleMs) {
            return;
        }
        if ((m_stage == SearchStage::Coarse || m_stage == SearchStage::Fine) && analysis.found) {
            recordStep(analysis.measures.sharpness);
        } else if (m_stage == SearchStage::Hold) {
            ++m_heldFrames;
            if (analysis.found) {
                m_held.push_back(analysis.measures.sharpness);
            }
        }
    }

    void searchTick() {
        const qint64 waited = m_stepClock.elapsed();
        if (m_stage == SearchStage::Defocus && waited >= kDefocusMs) {
            m_trialClock.start();
            const int step = std::max(kCoarseStep, m_range.resolution);
            std::vector<int> coarse;
            for (int position = m_range.minimum; position <= m_range.maximum; position += step) {
                coarse.push_back(position);
            }
            if (m_trialStart == m_range.maximum) {
                std::reverse(coarse.begin(), coarse.end());
            }
            beginStage(SearchStage::Coarse, std::move(coarse));
        } else if ((m_stage == SearchStage::Coarse || m_stage == SearchStage::Fine) &&
                   waited >= kStepTimeoutMs) {
            recordStep(0.0); // no page in time
        } else if (m_stage == SearchStage::Hold && waited >= kStepSettleMs + kHoldMs) {
            finishTrial();
        }
    }

    void recordStep(double sharpness) {
        m_curve.push_back({m_position, sharpness});
        if (++m_positionIndex < m_positions.size()) {
            moveTo(m_positions[m_positionIndex]);
            return;
        }
        if (m_stage != SearchStage::Coarse) {
            holdBest();
            return;
        }
        const int best = bestStep().position;
        const int coarse = std::max(kCoarseStep, m_range.resolution);
        const int fine = std::max(kFineStep, m_range.resolution);
        std::vector<int> around;
        for (int position = best - coarse + fine; position < best + coarse; position += fine) {
            const bool measured = std::any_of(m_curve.begin(), m_curve.end(), [position](const Step& step) {
                return step.position == position;
            });
            if (!measured && position >= m_range.minimum && position <= m_range.maximum) {
                around.push_back(position);
            }
        }
        beginStage(SearchStage::Fine, std::move(around));
    }

    Step bestStep() const {
        Step best;
        for (const Step& step : m_curve) {
            if (step.sharpness > best.sharpness) {
                best = step;
            }
        }
        return best;
    }

    void holdBest() {
        m_focusMs = m_trialClock.elapsed();
        const Step best = bestStep();
        m_best = best.position;
        m_peak = best.sharpness;
        m_held.clear();
        m_heldFrames = 0;
        moveTo(m_best);
        m_stage = SearchStage::Hold;
    }

    void finishTrial() {
        m_report.say(QStringLiteral("  %1 %2 %3 %4 %5 %6 %7 %8 %9%")
                         .arg(m_trial + 1, -6)
                         .arg(m_trialStart, 5)
                         .arg(m_curve.size(), 6)
                         .arg(m_best, 5)
                         .arg(m_peak, 6, 'f', 0)
                         .arg(static_cast<double>(m_focusMs) / 1000.0, 7, 'f', 2)
                         .arg(quantile(m_held, 0.1), 9, 'f', 0)
                         .arg(quantile(m_held, 0.5), 9, 'f', 0)
                         .arg(m_heldFrames == 0 ? 0.0
                                                : 100.0 * static_cast<double>(m_held.size()) /
                                                      static_cast<double>(m_heldFrames),
                              10, 'f', 0));
        std::vector<Step> curve = m_curve;
        std::sort(curve.begin(), curve.end(),
                  [](const Step& a, const Step& b) { return a.position < b.position; });
        QStringList points;
        for (const Step& step : curve) {
            points << QStringLiteral("%1:%2").arg(step.position).arg(step.sharpness, 0, 'f', 0);
        }
        m_report.say(QStringLiteral("        curve (focus:sharpness) ") + points.join(QLatin1Char(' ')));
        if (++m_trial < kTrials) {
            startTrial();
            return;
        }
        restoreUvc();
        if (m_searchOnly) {
            finish();
        } else {
            m_phase = Phase::SweepReady;
        }
    }

    // The camera's own focus as it was when the probe started: UVC settings outlast the program.
    void restoreUvc() {
        if (!m_uvc || !m_uvcChanged) {
            return;
        }
        m_uvcChanged = false;
        if (m_uvcAutoAtStart && !*m_uvcAutoAtStart && m_uvcFocusAtStart) {
            m_uvc->setAutoFocus(false);
            m_uvc->setFocus(*m_uvcFocusAtStart);
        } else if (m_uvcAutoAtStart) {
            m_uvc->setAutoFocus(true);
        }
        m_report.say(QStringLiteral("Native focus restored:\n") + m_uvc->summary());
    }

    QString uvcNow() {
        if (!m_uvc) {
            return QString();
        }
        const std::optional<bool> on = m_uvc->autoFocus();
        const std::optional<int> at = m_uvc->focus();
        return QStringLiteral(", native auto %1, focus %2")
            .arg(on ? (*on ? QStringLiteral("on") : QStringLiteral("off")) : QStringLiteral("?"))
            .arg(at ? QString::number(*at) : QStringLiteral("?"));
    }

    void reportSetting() {
        std::vector<double> sharpness;
        std::vector<double> side;
        for (const Sample& sample : m_samples) {
            if (sample.found) {
                sharpness.push_back(sample.sharpness);
                side.push_back(sample.longerSide);
            }
        }
        const double foundShare = m_samples.empty() ? 0.0
                                                    : 100.0 * static_cast<double>(sharpness.size()) /
                                                          static_cast<double>(m_samples.size());
        m_report.say(QStringLiteral("  %1 %2 %3% %4 %5 %6 %7 %8%   %9, %10, (%11, %12)")
                         .arg(m_settings[m_settingIndex].name, -34)
                         .arg(m_samples.size(), 6)
                         .arg(foundShare, 5, 'f', 0)
                         .arg(quantile(sharpness, 0.1), 6, 'f', 0)
                         .arg(quantile(sharpness, 0.5), 6, 'f', 0)
                         .arg(quantile(sharpness, 0.9), 6, 'f', 0)
                         .arg(quantile(sharpness, 1.0), 6, 'f', 0)
                         .arg(quantile(side, 0.5) * 100.0, 4, 'f', 0)
                         .arg(modeName(m_camera->focusMode()))
                         .arg(m_camera->focusDistance(), 0, 'f', 2)
                         .arg(m_camera->focusPoint().x(), 0, 'f', 2)
                         .arg(m_camera->focusPoint().y(), 0, 'f', 2) +
                     uvcNow());
    }

    void reportSweep() {
        const doc_vision::Thresholds thresholds;
        m_report.say(QStringLiteral("  %1 %2 %3 %4 %5 %6")
                         .arg(QStringLiteral("longer side"), -14)
                         .arg(QStringLiteral("frames"), 6)
                         .arg(QStringLiteral("p50"), 6)
                         .arg(QStringLiteral("p90"), 6)
                         .arg(QStringLiteral("max"), 6)
                         .arg(QStringLiteral("passing Sharp"), 14));
        int notFound = 0;
        for (const Sample& sample : m_samples) {
            notFound += sample.found ? 0 : 1;
        }
        for (int bin = 1; bin <= 9; ++bin) {
            const double low = bin == 1 ? 0.0 : bin / 10.0;
            const double high = (bin + 1) / 10.0;
            std::vector<double> sharpness;
            int passing = 0;
            for (const Sample& sample : m_samples) {
                if (sample.found && sample.longerSide >= low && (sample.longerSide < high || bin == 9)) {
                    sharpness.push_back(sample.sharpness);
                    passing += sample.sharpness >= thresholds.sharpness.failsAt ? 1 : 0;
                }
            }
            const QString name =
                bin == 1 ? QStringLiteral("under 20%")
                         : QStringLiteral("%1-%2%").arg(bin * 10).arg(bin == 9 ? 100 : (bin + 1) * 10);
            m_report.say(
                QStringLiteral("  %1 %2 %3 %4 %5 %6%")
                    .arg(name, -14)
                    .arg(sharpness.size(), 6)
                    .arg(quantile(sharpness, 0.5), 6, 'f', 0)
                    .arg(quantile(sharpness, 0.9), 6, 'f', 0)
                    .arg(quantile(sharpness, 1.0), 6, 'f', 0)
                    .arg(sharpness.empty() ? 0.0 : 100.0 * passing / static_cast<double>(sharpness.size()),
                         13, 'f', 0));
        }
        m_report.say(QStringLiteral("  frames without a page: %1 of %2").arg(notFound).arg(m_samples.size()));
    }

    void finish() {
        if (m_phase == Phase::Done) {
            return;
        }
        restoreUvc();
        m_phase = Phase::Done;
        m_report.say(QStringLiteral("\nFrames analysed: %1 in %2 s")
                         .arg(m_framesAnalysed)
                         .arg(m_clock.elapsed() / 1000));
        m_written = m_report.writeTo(m_outPath);
        m_report.say(m_written ? QStringLiteral("Report: ") + QDir::toNativeSeparators(m_outPath)
                               : QStringLiteral("Could not write the report to ") + m_outPath);
    }

    void showStatus() {
        QString instruction;
        switch (m_phase) {
        case Phase::Aim:
            instruction =
                QStringLiteral("Hold the page still at a comfortable reading distance, filling about half "
                               "the picture, then press Space. Esc ends.");
            break;
        case Phase::Settings:
            instruction =
                QStringLiteral("Keep the page still. Setting %1 of %2: %3 (%4 s)")
                    .arg(m_settingIndex + 1)
                    .arg(m_settings.size())
                    .arg(m_settings[m_settingIndex].name)
                    .arg(std::max<qint64>(0, (kSettleMs + kMeasureMs - m_phaseClock.elapsed()) / 1000 + 1));
            break;
        case Phase::SearchReady:
            instruction =
                QStringLiteral("Settings done. Hold the page still again and press Space: the probe "
                               "focuses on it %1 times (about 40 s).")
                    .arg(kTrials);
            break;
        case Phase::Search: {
            static const char* const kStages[] = {"defocusing", "coarse search", "fine search", "holding"};
            instruction = QStringLiteral("Keep the page still: focusing, trial %1 of %2, %3 at %4")
                              .arg(m_trial + 1)
                              .arg(kTrials)
                              .arg(QLatin1String(kStages[static_cast<int>(m_stage)]))
                              .arg(m_position);
            break;
        }
        case Phase::SweepReady:
            instruction =
                QStringLiteral("Now the sweep. Bring the page as close as the camera shows it whole, press "
                               "Space, then move it slowly to arm's length over 20 s.");
            break;
        case Phase::Sweep:
            instruction = QStringLiteral("Move the page slowly away: %1 s left")
                              .arg(std::max<qint64>(0, (kSweepMs - m_phaseClock.elapsed()) / 1000 + 1));
            break;
        case Phase::Done:
            instruction =
                m_written
                    ? QStringLiteral("Done. Report: %1. Esc closes.").arg(QDir::toNativeSeparators(m_outPath))
                    : QStringLiteral("Done. Esc closes.");
            break;
        }
        m_status->setText(instruction + QStringLiteral("\n") + m_live +
                          QStringLiteral(" · focus %1").arg(modeName(m_camera->focusMode())));
    }

    QString m_outPath;
    Report& m_report;
    QLabel* m_status = nullptr;
    QLabel* m_picture = nullptr;
    std::unique_ptr<QCamera> m_camera;
    QVideoSink m_sink;
    QMediaCaptureSession m_session; // after the sink and the camera, so it goes first
    std::unique_ptr<Analyser> m_analyser;
    std::unique_ptr<uvc::Camera> m_uvc;
    std::optional<bool> m_uvcAutoAtStart;
    std::optional<int> m_uvcFocusAtStart;
    bool m_uvcChanged = false; // since the start or the last restore
    bool m_searchOnly = false;
    uvc::FocusRange m_range{};
    int m_trial = 0;
    int m_trialStart = 0;
    int m_position = 0;
    int m_best = 0;
    double m_peak = 0.0;
    qint64 m_focusMs = 0;
    SearchStage m_stage = SearchStage::Defocus;
    std::vector<int> m_positions;
    std::size_t m_positionIndex = 0;
    std::vector<Step> m_curve;
    std::vector<double> m_held;
    int m_heldFrames = 0;
    QElapsedTimer m_stepClock;
    QElapsedTimer m_trialClock;
    QTimer m_ticker;
    QElapsedTimer m_clock;
    QElapsedTimer m_phaseClock;
    Phase m_phase = Phase::Aim;
    std::vector<Setting> m_settings;
    std::size_t m_settingIndex = 0;
    std::vector<Sample> m_samples;
    QCamera::FocusMode m_startMode = QCamera::FocusModeAuto;
    QPointF m_pageCentre{0.5, 0.5};
    QString m_live = QStringLiteral("Starting the camera");
    QString m_lastError;
    int m_framesAnalysed = 0;
    bool m_written = false;
};

// The analyser on drawn frames, with no camera and no window: the model opens, frames convert and are
// measured.
int selfTest(const std::filesystem::path& model, Report& report) {
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
    Q_UNUSED(model);
    report.say(QStringLiteral("self-test needs Qt 6.8 or newer (a video frame made from an image)"));
    return 1;
#else
    QImage drawn(1280, 720, QImage::Format_RGB32);
    drawn.fill(QColor(70, 70, 70));
    for (int y = 160; y < 560; ++y) {
        for (int x = 290; x < 990; ++x) {
            const bool ink = y > 440 && y < 520 && (x / 9) % 2 == 0 && x > 330 && x < 950;
            drawn.setPixelColor(x, y, ink ? QColor(20, 20, 20) : QColor(235, 235, 230));
        }
    }
    std::vector<Analysis> got;
    {
        Analyser analyser(model, [&](Analysis analysis) {
            QMetaObject::invokeMethod(
                qApp,
                [&got, analysis = std::move(analysis)]() mutable { got.push_back(std::move(analysis)); },
                Qt::QueuedConnection);
        });
        for (int i = 0; i < 3; ++i) {
            analyser.offer(QVideoFrame(drawn), false);
            waitUntil(5000, [&] { return got.size() > static_cast<std::size_t>(i); });
        }
    }
    bool ok = got.size() == 3;
    for (const Analysis& analysis : got) {
        report.say(
            QStringLiteral("self-test frame: %1%2%3")
                .arg(analysis.found ? QStringLiteral("page found, sharpness %1")
                                          .arg(analysis.measures.sharpness, 0, 'f', 0)
                                    : QStringLiteral("no page"))
                .arg(analysis.picture.isNull() ? QStringLiteral(", no picture") : QString())
                .arg(analysis.error.isEmpty() ? QString() : QStringLiteral(", error: ") + analysis.error));
        ok = ok && analysis.error.isEmpty() && !analysis.picture.isNull();
    }
    report.say(ok ? QStringLiteral("self-test passed")
                  : QStringLiteral("self-test FAILED (%1 analyses)").arg(got.size()));
    return ok ? 0 : 1;
#endif
}

QString argumentAfter(const QStringList& arguments, const QString& name) {
    const qsizetype at = arguments.indexOf(name);
    return at >= 0 && at + 1 < arguments.size() ? arguments[at + 1] : QString();
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    const QStringList arguments = QApplication::arguments();
    Report report;
    if (arguments.contains(QStringLiteral("--help"))) {
        report.say(QStringLiteral(
            "usage: camera_focus_probe --list [--no-prompt] [--no-start]\n"
            "       camera_focus_probe --self-test [--model <file>]\n"
            "       camera_focus_probe [--camera <number or part of its name>] [--search] [--model <file>] "
            "[--out <file>]"));
        return 0;
    }
    if (arguments.contains(QStringLiteral("--list"))) {
        return listCameras(!arguments.contains(QStringLiteral("--no-prompt")),
                           !arguments.contains(QStringLiteral("--no-start")), report);
    }

    QString model = argumentAfter(arguments, QStringLiteral("--model"));
    if (model.isEmpty()) {
        model = QDir(document_lab::defaultModelFolder()).filePath(QLatin1String(lab_core::kLc100ModelFile));
    }
    if (arguments.contains(QStringLiteral("--self-test"))) {
        return selfTest(std::filesystem::path(model.toStdU16String()), report);
    }

    const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
    std::optional<QCameraDevice> device;
    const QString wanted = argumentAfter(arguments, QStringLiteral("--camera"));
    bool isNumber = false;
    const int number = wanted.toInt(&isNumber);
    for (qsizetype i = 0; i < devices.size() && !device; ++i) {
        if (wanted.isEmpty()
                ? devices[i].isDefault()
                : (isNumber ? i == number : devices[i].description().contains(wanted, Qt::CaseInsensitive))) {
            device = devices[i];
        }
    }
    if (!device) {
        report.say(QStringLiteral("No camera matches \"%1\"; --list shows them.").arg(wanted));
        return 2;
    }
    QString out = argumentAfter(arguments, QStringLiteral("--out"));
    if (out.isEmpty()) {
        out =
            QDir(QDir::tempPath())
                .filePath(QStringLiteral("camera-focus-probe-%1.txt")
                              .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    }

    std::unique_ptr<ProbeWindow> window;
    withCameraPermission(true, [&](bool granted) {
        if (!granted) {
            report.say(
                QStringLiteral("The camera may not be used: allow it in the system's privacy settings."));
            // Later: the answer may come before the event loop runs, and exit() does nothing then.
            QTimer::singleShot(0, qApp, [] { QApplication::exit(3); });
            return;
        }
        window = std::make_unique<ProbeWindow>(*device, std::filesystem::path(model.toStdU16String()), out,
                                               arguments.contains(QStringLiteral("--search")), report);
        window->show();
    });
    return QApplication::exec();
}
