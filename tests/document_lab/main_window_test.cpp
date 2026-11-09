#include "main_window.h"

#include "card_frame.h"
#include "fake_camera_backend.h"
#include "fake_sender.h"
#include "latch.h"
#include "list_source.h"
#include "scratch_folder.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QTest>
#include <QWheelEvent>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using document_lab::Clock;
using document_lab::LabDependencies;
using document_lab::LabWindow;

// A window with every part immediate, a clock the test moves, fake cameras, and conversions counted.
struct Lab {
    explicit Lab(const std::function<void(LabDependencies&)>& adjust = {}) {
        LabDependencies dependencies;
        auto cameras = std::make_unique<test_support::FakeCameraBackend>();
        backend = cameras.get();
        dependencies.cameras = std::move(cameras);
        dependencies.makeSender = [](const std::string&) {
            return std::make_unique<test_support::RecordingSender>();
        };
        dependencies.engineMode = lab_core::Engine::Mode::Immediate;
        dependencies.conversionMode = LiveConverter::Mode::Immediate;
        dependencies.conversion = [this](const LiveJob& job) {
            ++conversions;
            return convertLiveFrame(job);
        };
        dependencies.senderMode = lab_core::ResultSender::Mode::Immediate;
        dependencies.recorderMode = frame_io::RecordingWriter::Mode::Immediate;
        dependencies.immediateCapture = true;
        dependencies.freeSpace = [](const std::filesystem::path&) { return std::uintmax_t{100'000'000'000}; };
        dependencies.confirm = [](const QString&) { return true; };
        dependencies.labFolder = QString::fromStdString(folder.path().string());
        dependencies.clock = [this] { return Clock::time_point(Clock::duration(ticks.load())); };
        if (adjust) {
            adjust(dependencies);
        }
        window = std::make_unique<LabWindow>(std::move(dependencies));
        window->resize(800, 600);
        window->show();
        QApplication::processEvents();
    }

    void advance(Clock::duration by) { ticks += by.count(); }
    void replay(std::vector<frame_io::ReadResult> reads) {
        window->startReplay(test_support::openerOf(std::move(reads)));
    }
    void pump(int times = 1) {
        for (int i = 0; i < times; ++i) {
            window->pumpForTests();
        }
    }

    ImageView* canvas() const { return window->findChild<ImageView*>(); }
    QComboBox* combo(const char* name) const { return window->findChild<QComboBox*>(name); }
    void choose(const char* comboName, const QString& text) const {
        QComboBox* box = combo(comboName);
        ASSERT_NE(box->findText(text), -1) << text.toStdString();
        box->setCurrentIndex(box->findText(text));
    }
    QStringList entries(const char* comboName) const {
        QStringList texts;
        for (int i = 0; i < combo(comboName)->count(); ++i) {
            texts << combo(comboName)->itemText(i);
        }
        return texts;
    }
    QString status() const { return window->findChild<QLabel*>("statusLabel")->text(); }
    QLabel* placeholder() const { return window->findChild<QLabel*>("placeholder"); }
    QStringList results() const {
        QStringList lines;
        const auto* list = window->findChild<QListWidget*>("resultsList");
        for (int i = 0; i < list->count(); ++i) {
            lines << list->item(i)->text();
        }
        return lines;
    }

    test_support::ScratchFolder folder;
    std::atomic<Clock::rep> ticks = Clock::duration(1000s).count();
    std::atomic<int> conversions = 0;
    test_support::FakeCameraBackend* backend = nullptr;
    std::unique_ptr<LabWindow> window;
};

// `count` frames of `image`, all due at once.
std::vector<frame_io::ReadResult> framesOf(const cv::Mat& image, int count = 1) {
    std::vector<frame_io::ReadResult> reads;
    for (int i = 0; i < count; ++i) {
        reads.push_back({.status = frame_io::ReadStatus::Frame,
                         .frame = test_support::listFrame(static_cast<std::uint64_t>(i), 0ms, image),
                         .error = {}});
    }
    return reads;
}

cv::Mat fourK() { return cv::Mat(3000, 4000, CV_8UC3, cv::Scalar::all(90)); }

LiveConverter::Conversion holdingWhen(test_support::Latch& latch, std::function<bool(const LiveJob&)> when) {
    return [&latch, when = std::move(when)](const LiveJob& job) {
        if (when(job)) {
            latch.hold();
        }
        return convertLiveFrame(job);
    };
}

void zoomIn(QWidget& view, QPointF position) {
    QWheelEvent event(position, view.mapToGlobal(position), QPoint(), QPoint(0, 4 * 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&view, &event);
}

void sendMouse(QWidget& view, QEvent::Type type, QPointF position) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, position, view.mapToGlobal(position), button,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
}

void pan(QWidget& view, QPointF from, QPointF to) {
    sendMouse(view, QEvent::MouseButtonPress, from);
    sendMouse(view, QEvent::MouseMove, to);
    sendMouse(view, QEvent::MouseButtonRelease, to);
}

bool waitForPictures(const LabWindow& window, std::uint64_t received) {
    return QTest::qWaitFor(
        [&window, received] { return window.canvasCountsForTests().received >= received; });
}

// --- The canvas
// ---------------------------------------------------------------------------------------------

TEST(LabWindow, ShowsTheResultOnTheCanvas) {
    Lab lab;
    lab.replay(framesOf(test_support::cardFrame()));

    lab.pump();

    ASSERT_TRUE(lab.canvas()->hasImage());
    EXPECT_EQ(lab.canvas()->visibleArea(), QRect(0, 0, 640, 480));
    EXPECT_FALSE(lab.canvas()->regionsEnabled());
}

// The processor's overlays are drawn over the frame from the start; hiding them only repaints.
TEST(LabWindow, OverlaysOnByDefaultAndCanBeHidden) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Outline"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();
    auto* overlays = lab.window->findChild<QCheckBox*>("overlaysCheck");
    ASSERT_TRUE(overlays->isChecked());
    EXPECT_FALSE(lab.canvas()->overlays().empty());
    EXPECT_TRUE(lab.canvas()->overlaysShown());
    const int conversions = lab.conversions;

    overlays->setChecked(false);

    EXPECT_FALSE(lab.canvas()->overlaysShown());
    EXPECT_EQ(lab.conversions, conversions);
}

TEST(LabWindow, ShowListsDebugImages) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.replay(framesOf(test_support::cardFrame()));

    lab.pump();

    EXPECT_EQ(lab.entries("showCombo"), (QStringList{QStringLiteral("Frame"), QStringLiteral("edges")}));
}

// Show keeps a picture the frames no longer have, and says so; nothing is drawn for it meanwhile.
TEST(LabWindow, MissingShowEntryShowsThePlaceholder) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    lab.choose("showCombo", QStringLiteral("edges"));
    EXPECT_FALSE(lab.placeholder()->isVisible());

    lab.choose("processorCombo", QStringLiteral("None"));
    lab.pump();

    EXPECT_EQ(lab.combo("showCombo")->currentText(), QStringLiteral("edges"));
    EXPECT_TRUE(lab.placeholder()->isVisible());
    EXPECT_TRUE(lab.placeholder()->text().contains(QStringLiteral("edges")));
    const int conversions = lab.conversions;
    const QPointF centre = QRectF(lab.canvas()->rect()).center();
    zoomIn(*lab.canvas(), centre);
    pan(*lab.canvas(), centre, centre - QPointF(150, 0));
    EXPECT_EQ(lab.conversions, conversions);
}

TEST(LabWindow, FindingsInTheResultsPanel) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.replay(framesOf(test_support::cardFrame()));

    lab.pump();

    ASSERT_EQ(lab.results().size(), 1);
    EXPECT_TRUE(lab.results().front().startsWith(QStringLiteral("edges: ")))
        << lab.results().front().toStdString();
}

// --- The status bar
// -----------------------------------------------------------------------------------------

// A picture replaced before it was drawn counts as not drawn.
TEST(LabWindow, StatusShowsTheCounts) {
    test_support::Latch latch;
    Lab lab([&latch](LabDependencies& dependencies) {
        dependencies.conversionMode = LiveConverter::Mode::Threaded;
        dependencies.conversion = holdingWhen(latch, [](const LiveJob& job) { return job.frameNumber == 1; });
    });
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump(); // the first picture is held converting
    ASSERT_TRUE(latch.waitUntilEntered());
    lab.pump(); // the second waits
    lab.pump(); // the third replaces it
    latch.release();
    ASSERT_TRUE(waitForPictures(*lab.window, 2));
    lab.pump();

    EXPECT_TRUE(lab.status().contains(QStringLiteral("frame 3 of 3"))) << lab.status().toStdString();
    EXPECT_TRUE(
        lab.status().contains(QStringLiteral("dropped 0 · not drawn 1 · not sent 0 · not recorded 0")))
        << lab.status().toStdString();
}

// A processor that takes long says on which frame, and for how long.
class HeldProcessor : public lab_core::FrameProcessor {
public:
    explicit HeldProcessor(test_support::Latch& latch) : m_latch(latch) {}
    lab_core::ProcessorResult process(const frame_io::Frame& frame) override {
        if (frame.number == 1) {
            m_latch.hold();
        }
        return {};
    }

private:
    test_support::Latch& m_latch;
};

TEST(LabWindow, StatusShowsTheBusyFrame) {
    test_support::Latch latch;
    Lab lab([&latch](LabDependencies& dependencies) {
        dependencies.engineMode = lab_core::Engine::Mode::Threaded;
        dependencies.makeProcessor = [&latch](lab_core::ProcessorKind) {
            return std::make_unique<HeldProcessor>(latch);
        };
    });
    lab.replay(framesOf(test_support::cardFrame(), 3));
    ASSERT_TRUE(latch.waitUntilEntered());

    lab.advance(3s);

    EXPECT_TRUE(QTest::qWaitFor([&lab] {
        return lab.status().contains(QStringLiteral("processing frame 2 for 3 s"));
    })) << lab.status().toStdString();
    latch.release();
}

// --- What reaches the canvas
// --------------------------------------------------------------------------------

TEST(LabWindow, StaleResultAfterAShowChangeIsDropped) {
    test_support::Latch latch;
    Lab lab([&latch](LabDependencies& dependencies) {
        dependencies.conversionMode = LiveConverter::Mode::Threaded;
        dependencies.conversion =
            holdingWhen(latch, [](const LiveJob& job) { return job.channel == "Frame"; });
    });
    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump(); // the frame's picture is held converting
    ASSERT_TRUE(latch.waitUntilEntered());

    lab.choose("showCombo", QStringLiteral("edges"));
    latch.release();
    ASSERT_TRUE(waitForPictures(*lab.window, 2));

    EXPECT_EQ(lab.window->canvasCountsForTests().applied, 1U);
    // Inside the card: no edges there (0), where the frame is light (220).
    const QImage& picture = lab.canvas()->picture();
    EXPECT_EQ(qGray(picture.pixel(picture.width() / 2, picture.height() / 2)), 0);
}

TEST(LabWindow, StaleResultAfterStopIsDropped) {
    test_support::Latch latch;
    Lab lab([&latch](LabDependencies& dependencies) {
        dependencies.conversionMode = LiveConverter::Mode::Threaded;
        dependencies.conversion = holdingWhen(latch, [](const LiveJob&) { return true; });
    });
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    ASSERT_TRUE(latch.waitUntilEntered());

    lab.window->findChild<QPushButton*>("startStopButton")->click();
    latch.release();
    ASSERT_TRUE(waitForPictures(*lab.window, 1));

    EXPECT_EQ(lab.window->canvasCountsForTests().applied, 0U);
    EXPECT_FALSE(lab.canvas()->hasImage());
}

// A frame whose picture is made while a new source's first picture, a fit, still converts is drawn for the
// fitted view too, not for the zoom on screen, which is about to go.
TEST(LabWindow, JobsDuringAFitFitToo) {
    test_support::Latch latch;
    std::atomic<bool> holdFits = false;
    Lab lab([&](LabDependencies& dependencies) {
        dependencies.conversionMode = LiveConverter::Mode::Threaded;
        dependencies.conversion = holdingWhen(latch, [&holdFits](const LiveJob& job) {
            return holdFits && job.keep == ImageView::KeepView::No;
        });
    });
    lab.replay(framesOf(fourK()));
    lab.pump();
    ASSERT_TRUE(waitForPictures(*lab.window, 1));
    zoomIn(*lab.canvas(), QRectF(lab.canvas()->rect()).center());
    ASSERT_TRUE(waitForPictures(*lab.window, 2));
    ASSERT_NE(lab.canvas()->pictureArea(), QRect(0, 0, 4000, 3000));

    holdFits = true;
    lab.replay(framesOf(fourK(), 2));
    lab.pump(); // the new source's first picture, a fit, is held
    ASSERT_TRUE(latch.waitUntilEntered());
    lab.pump(); // the next frame's waits behind it
    latch.release();
    ASSERT_TRUE(waitForPictures(*lab.window, 4));

    EXPECT_TRUE(lab.canvas()->pictureArea().contains(lab.canvas()->visibleArea()));
    EXPECT_EQ(lab.canvas()->visibleArea(), QRect(0, 0, 4000, 3000));
}

// A fit's picture was made for the view as it was; resized meanwhile, the view needs another, which is made.
TEST(LabWindow, FitResultResubmitsWhenTheAreaDiffers) {
    test_support::Latch latch;
    Lab lab([&latch](LabDependencies& dependencies) {
        dependencies.conversionMode = LiveConverter::Mode::Threaded;
        dependencies.conversion =
            holdingWhen(latch, [](const LiveJob& job) { return job.keep == ImageView::KeepView::No; });
    });
    lab.replay(framesOf(fourK()));
    lab.pump();
    ASSERT_TRUE(latch.waitUntilEntered());
    const int widthBefore = lab.canvas()->width();

    lab.window->resize(1400, 1000);
    QApplication::processEvents();
    ASSERT_GT(lab.canvas()->width(), widthBefore);
    latch.release();
    ASSERT_TRUE(waitForPictures(*lab.window, 2));

    EXPECT_GT(lab.canvas()->picture().width(), widthBefore);
}

TEST(LabWindow, PanWithinTheMarginDoesNotResubmit) {
    Lab lab;
    lab.replay(framesOf(fourK()));
    lab.pump();
    const QPointF centre = QRectF(lab.canvas()->rect()).center();
    zoomIn(*lab.canvas(), centre);
    ASSERT_NE(lab.canvas()->pictureArea(), QRect(0, 0, 4000, 3000));
    const int conversions = lab.conversions;

    pan(*lab.canvas(), centre, centre - QPointF(10, 0));

    EXPECT_EQ(lab.conversions, conversions);
}

TEST(LabWindow, PanOutOfTheMarginResubmits) {
    Lab lab;
    lab.replay(framesOf(fourK()));
    lab.pump();
    const QPointF centre = QRectF(lab.canvas()->rect()).center();
    zoomIn(*lab.canvas(), centre);
    const int conversions = lab.conversions;

    pan(*lab.canvas(), centre, centre - QPointF(lab.canvas()->width() / 2.0, 0));

    EXPECT_GT(lab.conversions, conversions);
    EXPECT_TRUE(lab.canvas()->pictureArea().contains(lab.canvas()->visibleArea()));
}

} // namespace
