#include "main_window.h"

#include "card_frame.h"
#include "fake_camera_backend.h"
#include "fake_sender.h"
#include "latch.h"
#include "list_source.h"
#include "owner_only.h"
#include "scratch_folder.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTest>
#include <QTreeWidget>
#include <QWheelEvent>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <visual_debug/endpoint.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
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
    QTreeWidget* resultsTree() const { return window->findChild<QTreeWidget*>("resultsList"); }
    // Each row as "name: value", or the name alone when it has no value.
    QStringList results() const {
        QStringList lines;
        const QTreeWidget* tree = resultsTree();
        if (tree == nullptr) {
            return {QStringLiteral("(no results tree)")};
        }
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            const QTreeWidgetItem* row = tree->topLevelItem(i);
            lines << (row->text(1).isEmpty() ? row->text(0)
                                             : row->text(0) + QStringLiteral(": ") + row->text(1));
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
    lab.choose("processorCombo", QStringLiteral("Outline (simple)"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();
    auto* overlays = lab.window->findChild<QAbstractButton*>("overlaysCheck");
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

// A processor that makes its "edges" picture for the first frame only.
class FirstFrameEdges : public lab_core::FrameProcessor {
public:
    lab_core::ProcessorResult process(const frame_io::Frame& frame) override {
        lab_core::ProcessorResult result;
        if (frame.number == 0) {
            result.images.push_back({.name = "edges", .image = frame.image.clone(), .description = {}});
        }
        return result;
    }
    void reset() override {}
};

// Show keeps a picture the frames no longer have, and says so; nothing is drawn for it meanwhile.
TEST(LabWindow, MissingShowEntryShowsThePlaceholder) {
    Lab lab([](LabDependencies& dependencies) {
        dependencies.makeProcessor = [](lab_core::ProcessorKind) {
            return std::make_unique<FirstFrameEdges>();
        };
    });
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    lab.choose("showCombo", QStringLiteral("edges"));
    EXPECT_FALSE(lab.placeholder()->isVisible());

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

// Show lists the pictures of the processor chosen, not of the ones before it; a choice comes back with its
// picture.
TEST(LabWindow, ProcessorChangeEmptiesShow) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    lab.choose("showCombo", QStringLiteral("edges"));

    lab.choose("processorCombo", QStringLiteral("None"));
    lab.pump();
    EXPECT_EQ(lab.entries("showCombo"), QStringList{QStringLiteral("Frame")});
    EXPECT_FALSE(lab.placeholder()->isVisible());

    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.pump();
    EXPECT_EQ(lab.combo("showCombo")->currentText(), QStringLiteral("edges"));
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

// Findings a test chooses, and an error when asked; it stands in for Edges.
class ScriptedProcessor : public lab_core::FrameProcessor {
public:
    ScriptedProcessor(std::vector<std::string> findings, std::string error,
                      std::vector<std::chrono::nanoseconds>* times)
        : m_findings(std::move(findings)), m_error(std::move(error)), m_times(times) {}

    lab_core::ProcessorResult process(const frame_io::Frame& frame) override {
        if (m_times) {
            m_times->push_back(frame.at);
        }
        if (!m_error.empty()) {
            throw std::runtime_error(m_error);
        }
        return {.overlays = {}, .images = {}, .findings = m_findings};
    }
    void reset() override {}

private:
    std::vector<std::string> m_findings;
    std::string m_error;
    std::vector<std::chrono::nanoseconds>* m_times;
};

// Edges made by `make`; the others as lab_core makes them.
void edgesAre(LabDependencies& dependencies,
              std::function<std::unique_ptr<lab_core::FrameProcessor>()> make) {
    dependencies.makeProcessor = [make = std::move(make)](lab_core::ProcessorKind kind) {
        return kind == lab_core::ProcessorKind::Edges ? make() : lab_core::makeProcessor(kind);
    };
}

// A finding "name: value" is a row of two; one without ": " fills the first column; an error is a row of its
// own.
TEST(LabWindow, ResultsAreNameAndValueRows) {
    Lab lab([](LabDependencies& dependencies) {
        edgesAre(dependencies, [] {
            return std::make_unique<ScriptedProcessor>(
                std::vector<std::string>{"document: not found", "corners: 4: estimated", "plain note"}, "",
                nullptr);
        });
    });
    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();

    const QTreeWidget* tree = lab.resultsTree();
    ASSERT_NE(tree, nullptr);
    ASSERT_EQ(tree->columnCount(), 2);
    EXPECT_TRUE(tree->isHeaderHidden());
    ASSERT_EQ(tree->topLevelItemCount(), 3);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("document"));
    EXPECT_EQ(tree->topLevelItem(0)->text(1), QStringLiteral("not found"));
    EXPECT_EQ(tree->topLevelItem(1)->text(0), QStringLiteral("corners"));
    EXPECT_EQ(tree->topLevelItem(1)->text(1), QStringLiteral("4: estimated"));
    EXPECT_EQ(tree->topLevelItem(2)->text(0), QStringLiteral("plain note"));
    EXPECT_TRUE(tree->topLevelItem(2)->text(1).isEmpty());
}

TEST(LabWindow, ProcessorErrorIsARow) {
    Lab lab([](LabDependencies& dependencies) {
        edgesAre(dependencies, [] {
            return std::make_unique<ScriptedProcessor>(std::vector<std::string>{}, "it broke", nullptr);
        });
    });
    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();

    EXPECT_EQ(lab.results(), QStringList{QStringLiteral("error: it broke")});
}

// View → Results hides the panel; its width, dragged, and whether it shows are remembered.
TEST(LabWindow, ResultsPanelHidesAndRemembersItsWidth) {
    {
        Lab lab;
        auto* results = lab.window->findChild<QAction*>("resultsAction");
        auto* splitter = lab.window->findChild<QSplitter*>("resultsSplitter");
        ASSERT_NE(results, nullptr);
        ASSERT_NE(splitter, nullptr);
        ASSERT_NE(lab.resultsTree(), nullptr);
        EXPECT_TRUE(results->isChecked());
        EXPECT_TRUE(lab.resultsTree()->isVisible());
        EXPECT_EQ(splitter->sizes().at(1), 200); // the first time

        const int total = splitter->sizes().at(0) + splitter->sizes().at(1);
        splitter->setSizes({total - 260, 260});
        emit splitter->splitterMoved(total - 260, 1);
        results->trigger();

        EXPECT_FALSE(lab.resultsTree()->isVisible());
    }
    Lab lab;
    auto* results = lab.window->findChild<QAction*>("resultsAction");
    auto* splitter = lab.window->findChild<QSplitter*>("resultsSplitter");
    EXPECT_FALSE(results->isChecked());
    EXPECT_FALSE(lab.resultsTree()->isVisible());

    results->trigger();

    EXPECT_TRUE(lab.resultsTree()->isVisible());
    EXPECT_EQ(splitter->sizes().at(1), 260);
}

// On Qt before 6.8 the View menu has no appearance in it, but it must still show, or a hidden results panel
// could never come back.
TEST(LabWindow, ViewMenuAlwaysShows) {
    Lab lab;
    auto* view = lab.window->findChild<QMenu*>("viewMenu");
    ASSERT_NE(view, nullptr);

    EXPECT_TRUE(view->menuAction()->isVisible());
    ASSERT_FALSE(view->actions().isEmpty());
    EXPECT_EQ(view->actions().front(), lab.window->findChild<QAction*>("resultsAction"));
}

// The picker says what Outline is; the settings keep lab_core's name, and the choice is found by kind.
TEST(LabWindow, ProcessorLabelsAndRememberedKind) {
    {
        Lab lab;
        EXPECT_EQ(lab.entries("processorCombo"), (QStringList{QStringLiteral("None"), QStringLiteral("Edges"),
                                                              QStringLiteral("Outline (simple)")}));
        lab.choose("processorCombo", QStringLiteral("Outline (simple)"));
        EXPECT_EQ(QSettings().value(QStringLiteral("lab/processor")).toString(), QStringLiteral("Outline"));
    }
    Lab lab;

    EXPECT_EQ(lab.combo("processorCombo")->currentText(), QStringLiteral("Outline (simple)"));
    EXPECT_EQ(lab.combo("processorCombo")->currentData().toInt(),
              static_cast<int>(lab_core::ProcessorKind::Outline));
}

// Loop, As fast as possible and Overlays are buttons whose icon shows whether they are on, whatever the style
// draws for a checked button.
TEST(LabWindow, TogglesShowTheirState) {
    Lab lab;
    for (const char* name : {"loopCheck", "asFastCheck", "overlaysCheck"}) {
        auto* toggle = qobject_cast<QPushButton*>(lab.window->findChild<QAbstractButton*>(name));
        ASSERT_NE(toggle, nullptr) << name;
        EXPECT_TRUE(toggle->isCheckable()) << name;
        ASSERT_FALSE(toggle->icon().isNull()) << name;
        EXPECT_NE(toggle->icon().pixmap(QSize(24, 24), QIcon::Normal, QIcon::On).toImage(),
                  toggle->icon().pixmap(QSize(24, 24), QIcon::Normal, QIcon::Off).toImage())
            << name;
        EXPECT_FALSE(toggle->toolTip().isEmpty()) << name;
    }
    EXPECT_FALSE(lab.window->findChild<QAbstractButton*>("loopCheck")->isChecked());
    EXPECT_TRUE(lab.window->findChild<QAbstractButton*>("overlaysCheck")->isChecked());
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

// --- Cameras
// ------------------------------------------------------------------------------------------------

test_support::FakeCameraBackend& fakeOf(LabDependencies& dependencies) {
    return static_cast<test_support::FakeCameraBackend&>(*dependencies.cameras);
}

document_lab::CameraFormat nv12(int width, int height, float fps = 30) {
    return {.size = QSize(width, height), .maxFps = fps, .pixelFormat = QVideoFrameFormat::Format_NV12};
}

document_lab::CameraInfo camera(const QByteArray& id, const QString& name) {
    return {.id = id, .name = name, .front = false, .formats = {nv12(640, 480), nv12(1280, 720)}};
}

// Two cameras: "a", the FaceTime camera, and "b", a C920.
void twoCameras(LabDependencies& dependencies) {
    fakeOf(dependencies).list = {camera("a", QStringLiteral("FaceTime HD Camera")),
                                 camera("b", QStringLiteral("C920"))};
}

// A 64×48 BGRA frame of one grey.
QVideoFrame cameraFrame(int grey) {
    QVideoFrame frame(QVideoFrameFormat(QSize(64, 48), QVideoFrameFormat::Format_BGRA8888));
    EXPECT_TRUE(frame.map(QVideoFrame::WriteOnly));
    for (int row = 0; row < 48; ++row) {
        std::memset(frame.bits(0) + static_cast<std::ptrdiff_t>(row) * frame.bytesPerLine(0), grey, 64 * 4);
    }
    frame.unmap();
    return frame;
}

QPushButton* startStop(const Lab& lab) { return lab.window->findChild<QPushButton*>("startStopButton"); }

TEST(LabWindow, ListsCamerasAndFollowsChanges) {
    Lab lab(twoCameras);

    EXPECT_EQ(lab.entries("cameraCombo"),
              (QStringList{QStringLiteral("FaceTime HD Camera"), QStringLiteral("C920")}));
    EXPECT_EQ(lab.entries("formatCombo"), (QStringList{QStringLiteral("1280×720, 30 fps, NV12"),
                                                       QStringLiteral("640×480, 30 fps, NV12")}));

    lab.backend->changeCameras({camera("b", QStringLiteral("C920"))});

    EXPECT_EQ(lab.entries("cameraCombo"), QStringList{QStringLiteral("C920")});
}

TEST(LabWindow, NeverStartsAtLaunch) {
    Lab lab(twoCameras);

    EXPECT_TRUE(lab.backend->starts.empty());
    EXPECT_EQ(lab.backend->permissionRequests, 0);
    EXPECT_EQ(startStop(lab)->text(), QStringLiteral("Start"));
    EXPECT_TRUE(startStop(lab)->isEnabled());
}

// Start asks for camera access first, and starts only once it is granted, with the camera's default format.
TEST(LabWindow, StartAsksPermissionFirst) {
    Lab lab([](LabDependencies& dependencies) {
        twoCameras(dependencies);
        fakeOf(dependencies).permission.reset();
    });

    startStop(lab)->click();
    EXPECT_EQ(lab.backend->permissionRequests, 1);
    EXPECT_TRUE(lab.backend->starts.empty());
    lab.backend->pendingAnswer(true);

    ASSERT_EQ(lab.backend->starts.size(), 1U);
    EXPECT_EQ(lab.backend->starts[0].first, QByteArray("a"));
    EXPECT_EQ(lab.backend->starts[0].second, nv12(1280, 720));
}

// A camera that lists no formats (some drivers on Linux) starts in its own default rather than not at all.
TEST(LabWindow, CameraWithoutFormatsStartsInItsDefault) {
    Lab lab([](LabDependencies& dependencies) {
        fakeOf(dependencies).list = {
            {.id = "c", .name = QStringLiteral("USB Camera"), .front = false, .formats = {}}};
    });
    ASSERT_TRUE(startStop(lab)->isEnabled());

    startStop(lab)->click();

    ASSERT_EQ(lab.backend->starts.size(), 1U);
    EXPECT_EQ(lab.backend->starts[0].first, QByteArray("c"));
    EXPECT_EQ(lab.backend->starts[0].second, document_lab::CameraFormat{});
    EXPECT_EQ(startStop(lab)->text(), QStringLiteral("Stop"));
}

TEST(LabWindow, PermissionRefusedSaysHow) {
    Lab lab([](LabDependencies& dependencies) {
        twoCameras(dependencies);
        fakeOf(dependencies).permission = false;
    });

    startStop(lab)->click();

    EXPECT_TRUE(lab.backend->starts.empty());
    EXPECT_TRUE(lab.status().contains(QStringLiteral("privacy"), Qt::CaseInsensitive))
        << lab.status().toStdString();
    EXPECT_EQ(startStop(lab)->text(), QStringLiteral("Start"));
}

TEST(LabWindow, StartShowsFramesOnTheCanvas) {
    Lab lab(twoCameras);
    startStop(lab)->click();

    lab.backend->deliver(cameraFrame(120));
    lab.pump();

    ASSERT_TRUE(lab.canvas()->hasImage());
    EXPECT_EQ(lab.canvas()->visibleArea(), QRect(0, 0, 64, 48));
    EXPECT_TRUE(lab.status().contains(QStringLiteral("Live"))) << lab.status().toStdString();
    EXPECT_EQ(startStop(lab)->text(), QStringLiteral("Stop"));
}

TEST(LabWindow, SwitchingCamerasStopsTheFirst) {
    Lab lab(twoCameras);
    startStop(lab)->click();

    lab.choose("cameraCombo", QStringLiteral("C920"));

    EXPECT_EQ(lab.backend->stops, 1);
    ASSERT_EQ(lab.backend->starts.size(), 2U);
    EXPECT_EQ(lab.backend->starts[1].first, QByteArray("b"));
    EXPECT_TRUE(lab.backend->running);
}

TEST(LabWindow, FormatChangeRestartsTheCamera) {
    Lab lab(twoCameras);
    startStop(lab)->click();

    lab.choose("formatCombo", QStringLiteral("640×480, 30 fps, NV12"));

    ASSERT_EQ(lab.backend->starts.size(), 2U);
    EXPECT_EQ(lab.backend->starts[1].second, nv12(640, 480));
}

TEST(LabWindow, CameraGoneStopsTheRun) {
    Lab lab(twoCameras);
    startStop(lab)->click();

    lab.backend->fail(QStringLiteral("The camera is gone."));

    EXPECT_FALSE(lab.backend->running);
    EXPECT_TRUE(lab.status().contains(QStringLiteral("The camera is gone."))) << lab.status().toStdString();
    EXPECT_EQ(startStop(lab)->text(), QStringLiteral("Start"));
}

TEST(LabWindow, NoFrameAfterStop) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    startStop(lab)->click();

    lab.backend->deliver(cameraFrame(120));
    lab.pump();

    EXPECT_FALSE(lab.canvas()->hasImage());
    EXPECT_FALSE(lab.backend->running);
}

// A frame the old camera had on its way when the user switched belongs to no run: it is dropped.
TEST(LabWindow, OldCamerasFrameNeverReachesTheNewRun) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    const quint64 first = lab.backend->session;
    lab.choose("cameraCombo", QStringLiteral("C920"));

    lab.backend->deliver(cameraFrame(120), first);
    lab.pump();
    EXPECT_FALSE(lab.canvas()->hasImage());

    lab.backend->deliver(cameraFrame(130));
    lab.pump();
    EXPECT_TRUE(lab.canvas()->hasImage());
}

// A camera frame's time counts from the run's first frame, as a replay's does, so a recording or a capture of
// it says how far into the run it was.
TEST(LabWindow, CameraFrameTimeFromTheFirstFrame) {
    std::vector<std::chrono::nanoseconds> times;
    Lab lab([&times](LabDependencies& dependencies) {
        twoCameras(dependencies);
        edgesAre(dependencies, [&times] {
            return std::make_unique<ScriptedProcessor>(std::vector<std::string>{}, "", &times);
        });
    });
    lab.choose("processorCombo", QStringLiteral("Edges"));
    startStop(lab)->click();

    lab.backend->deliver(cameraFrame(120));
    lab.pump();
    lab.advance(40ms);
    lab.backend->deliver(cameraFrame(130));
    lab.pump();

    EXPECT_EQ(times, (std::vector<std::chrono::nanoseconds>{0ms, 40ms}));
}

TEST(LabWindow, RemembersTheCameraAndItsFormat) {
    {
        Lab lab(twoCameras);
        lab.choose("cameraCombo", QStringLiteral("C920"));
        lab.choose("formatCombo", QStringLiteral("640×480, 30 fps, NV12"));
    }
    Lab lab(twoCameras);

    EXPECT_EQ(lab.combo("cameraCombo")->currentText(), QStringLiteral("C920"));
    EXPECT_EQ(lab.combo("formatCombo")->currentText(), QStringLiteral("640×480, 30 fps, NV12"));
}

// --- Replay, Record, Capture, closing
// -----------------------------------------------------------------------

QAbstractButton* button(const Lab& lab, const char* name) {
    return lab.window->findChild<QAbstractButton*>(name);
}

// A folder of three 64×48 PNGs, greys 10, 20 and 30.
std::filesystem::path imageFolder(const test_support::ScratchFolder& scratch) {
    const std::filesystem::path folder = scratch.path() / "images";
    std::filesystem::create_directories(folder);
    for (int i = 0; i < 3; ++i) {
        cv::imwrite((folder / ("frame " + std::to_string(i) + ".png")).string(),
                    cv::Mat(48, 64, CV_8UC3, cv::Scalar::all(10.0 * (i + 1))));
    }
    return folder;
}

std::filesystem::path videoFile(const test_support::ScratchFolder& scratch) {
    const std::filesystem::path path = scratch.path() / "clip.avi";
    cv::VideoWriter writer(path.string(), cv::CAP_OPENCV_MJPEG, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                           30.0, cv::Size(64, 48));
    for (int i = 0; i < 5; ++i) {
        writer.write(cv::Mat(48, 64, CV_8UC3, cv::Scalar::all(40.0 * i)));
    }
    return path;
}

QString qtPath(const std::filesystem::path& path) { return QString::fromStdU16String(path.u16string()); }

// Pumps until a result has been drawn: a video's frames come from a reading thread.
bool pumpUntilShown(Lab& lab, std::uint64_t pictures) {
    return QTest::qWaitFor([&] {
        lab.pump();
        return lab.window->canvasCountsForTests().applied >= pictures;
    });
}

// Folders can step back; so can a video read through FFmpeg, while one read through OpenCV can only start
// again.
TEST(LabWindow, ReplayControlsFollowTheSource) {
    const test_support::ScratchFolder files;
    const std::filesystem::path folder = imageFolder(files);
    const std::filesystem::path video = videoFile(files);
    Lab lab([&](LabDependencies& dependencies) {
        dependencies.chooseFolder = [&folder] { return qtPath(folder); };
        dependencies.chooseFile = [&video] { return qtPath(video); };
    });
    EXPECT_FALSE(button(lab, "playPauseButton")->isEnabled());

    button(lab, "openFolderButton")->click();
    ASSERT_TRUE(pumpUntilShown(lab, 1));
    lab.advance(40ms);
    ASSERT_TRUE(pumpUntilShown(lab, 2));
    EXPECT_TRUE(button(lab, "playPauseButton")->isEnabled());
    EXPECT_TRUE(button(lab, "stepBackButton")->isEnabled());
    EXPECT_TRUE(button(lab, "restartButton")->isEnabled());

    button(lab, "openFileButton")->click();
    ASSERT_TRUE(pumpUntilShown(lab, 3));
    lab.advance(40ms);
    ASSERT_TRUE(pumpUntilShown(lab, 4));
    EXPECT_TRUE(button(lab, "playPauseButton")->isEnabled());
#if defined(FRAME_IO_HAS_FFMPEG)
    EXPECT_TRUE(button(lab, "stepBackButton")->isEnabled()); // FFmpeg seeks in a video with timestamps
#else
    EXPECT_FALSE(button(lab, "stepBackButton")->isEnabled());
#endif
    EXPECT_TRUE(button(lab, "restartButton")->isEnabled());
    EXPECT_TRUE(lab.status().contains(QStringLiteral("clip.avi"))) << lab.status().toStdString();
}

// Space plays and pauses, unless a button has the focus: then it presses that button.
TEST(LabWindow, SpaceTogglesPlay) {
    Lab lab;
    lab.window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(lab.window.get()));
    lab.replay(framesOf(test_support::cardFrame(), 5));
    lab.pump();
    ASSERT_TRUE(lab.status().startsWith(QStringLiteral("Playing"))) << lab.status().toStdString();
    lab.canvas()->setFocus();

    QTest::keyClick(lab.canvas(), Qt::Key_Space);
    lab.pump();
    EXPECT_TRUE(lab.status().startsWith(QStringLiteral("Paused"))) << lab.status().toStdString();

    QTest::keyClick(lab.canvas(), Qt::Key_Space);
    lab.pump();
    EXPECT_TRUE(lab.status().startsWith(QStringLiteral("Playing"))) << lab.status().toStdString();

    QAbstractButton* loop = button(lab, "loopCheck");
    loop->setFocus();
    QTest::keyClick(loop, Qt::Key_Space);
    lab.pump();
    EXPECT_TRUE(loop->isChecked());
    EXPECT_TRUE(lab.status().startsWith(QStringLiteral("Playing"))) << lab.status().toStdString();
}

// The folder of the recording only its owner may open; the lab's folder is left as it was.
std::vector<std::filesystem::path> recordingsIn(const std::filesystem::path& labFolder) {
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : std::filesystem::directory_iterator(labFolder)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "recording.json")) {
            folders.push_back(entry.path());
        }
    }
    return folders;
}

nlohmann::json recordingJson(const std::filesystem::path& folder) {
    std::ifstream stream(folder / "recording.json");
    return nlohmann::json::parse(stream);
}

TEST(LabWindow, RecordWritesAPrivateFolder) {
    Lab lab;
    const QFileDevice::Permissions labFolderBefore = QFile::permissions(qtPath(lab.folder.path()));
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();

    button(lab, "recordButton")->click();
    lab.pump(2);
    button(lab, "recordButton")->click();
    lab.pump();

    const std::vector<std::filesystem::path> recordings = recordingsIn(lab.folder.path());
    ASSERT_EQ(recordings.size(), 1U);
    EXPECT_TRUE(test_support::isOwnerOnly(qtPath(recordings[0])));
    EXPECT_EQ(QFile::permissions(qtPath(lab.folder.path())), labFolderBefore);
    const nlohmann::json json = recordingJson(recordings[0]);
    EXPECT_EQ(json["complete"], true);
    EXPECT_EQ(json["frames"].size(), 2U);
    EXPECT_FALSE(button(lab, "recordButton")->isChecked());
}

// Capture keeps what the canvas shows, even while a newer frame's picture is still being made.
TEST(LabWindow, CaptureKeepsTheFrameOnScreen) {
    test_support::Latch latch;
    Lab lab([&latch](LabDependencies& dependencies) {
        dependencies.conversionMode = LiveConverter::Mode::Threaded;
        dependencies.conversion = holdingWhen(latch, [](const LiveJob& job) { return job.frameNumber == 2; });
    });
    const cv::Mat first(48, 64, CV_8UC3, cv::Scalar::all(50));
    const cv::Mat second(48, 64, CV_8UC3, cv::Scalar::all(200));
    lab.replay({{.status = frame_io::ReadStatus::Frame,
                 .frame = test_support::listFrame(0, 0ms, first),
                 .error = {}},
                {.status = frame_io::ReadStatus::Frame,
                 .frame = test_support::listFrame(1, 0ms, second),
                 .error = {}}});
    lab.pump();
    ASSERT_TRUE(waitForPictures(*lab.window, 1));
    lab.pump(); // the second's picture is held
    ASSERT_TRUE(latch.waitUntilEntered());

    button(lab, "captureButton")->click();
    latch.release();

    const std::filesystem::path captures = lab.folder.path() / "Captures";
    std::vector<std::filesystem::path> pngs;
    for (const auto& entry : std::filesystem::directory_iterator(captures)) {
        if (entry.path().extension() == ".png") {
            pngs.push_back(entry.path());
        }
    }
    ASSERT_EQ(pngs.size(), 1U);
    EXPECT_TRUE(pngs[0].filename().string().ends_with(" frame 1.png")) << pngs[0];
    EXPECT_EQ(cv::norm(cv::imread(pngs[0].string()), first, cv::NORM_INF), 0.0);
}

// A capture keeps where the processor found what it found, not only its findings.
TEST(LabWindow, CaptureKeepsTheOverlays) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Outline (simple)"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();

    button(lab, "captureButton")->click();

    std::vector<std::filesystem::path> jsons;
    for (const auto& entry : std::filesystem::directory_iterator(lab.folder.path() / "Captures")) {
        if (entry.path().extension() == ".json") {
            jsons.push_back(entry.path());
        }
    }
    ASSERT_EQ(jsons.size(), 1U);
    std::ifstream stream(jsons[0]);
    const nlohmann::json json = nlohmann::json::parse(stream);
    ASSERT_TRUE(json.contains("overlays")) << json.dump();
    ASSERT_FALSE(json["overlays"].empty());
    EXPECT_EQ(json["overlays"][0]["label"], "Document");
}

TEST(LabWindow, CapturesFolderIsPrivate) {
    Lab lab;
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();

    button(lab, "captureButton")->click();

    EXPECT_TRUE(test_support::isOwnerOnly(qtPath(lab.folder.path() / "Captures")));
    EXPECT_TRUE(lab.status().contains(QStringLiteral("frame 1.png"))) << lab.status().toStdString();
}

TEST(LabWindow, OtherFolderShowsTheReminder) {
    const test_support::ScratchFolder other;
    Lab lab([&other](LabDependencies& dependencies) {
        dependencies.chooseLabFolder = [&other] { return qtPath(other.path()); };
    });

    button(lab, "chooseLabFolderButton")->click();

    EXPECT_TRUE(lab.status().contains(
        QStringLiteral("Recordings may show real passports; avoid synced or shared folders.")))
        << lab.status().toStdString();
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();
    button(lab, "captureButton")->click();
    EXPECT_TRUE(std::filesystem::exists(other.path() / "Captures"));
}

// Closing waits until the recording has written every frame, and says so meanwhile; nothing new can start in
// the meantime, or the window would wait for that run too.
TEST(LabWindow, CloseWaitsForTheRecording) {
    test_support::Latch latch;
    Lab lab([&latch](LabDependencies& dependencies) {
        twoCameras(dependencies);
        dependencies.engineMode = lab_core::Engine::Mode::Threaded;
        dependencies.recorderMode = frame_io::RecordingWriter::Mode::Threaded;
        dependencies.recorderBeforeWrite = [&latch](std::uint64_t) { latch.hold(); };
    });
    // Frames a second apart on the test's clock: frame 1 shows at once, the others when the clock moves.
    std::vector<frame_io::ReadResult> reads;
    for (std::uint64_t i = 0; i < 3; ++i) {
        reads.push_back(
            {.status = frame_io::ReadStatus::Frame,
             .frame = test_support::listFrame(i, std::chrono::seconds(i), test_support::cardFrame()),
             .error = {}});
    }
    lab.replay(std::move(reads));
    ASSERT_TRUE(QTest::qWaitFor([&lab] { return lab.status().contains(QStringLiteral("frame 1 of 3")); }))
        << lab.status().toStdString();
    button(lab, "recordButton")->click();
    lab.advance(1s);
    ASSERT_TRUE(latch.waitUntilEntered()); // frame 2 is being written
    lab.advance(1s);
    ASSERT_TRUE(QTest::qWaitFor([&lab] { return lab.status().contains(QStringLiteral("frame 3 of 3")); }))
        << lab.status().toStdString();

    lab.window->close();
    QApplication::processEvents();
    EXPECT_TRUE(lab.window->isVisible());
    EXPECT_TRUE(QTest::qWaitFor([&lab] {
        return lab.status().contains(QStringLiteral("writing recording"));
    })) << lab.status().toStdString();
    EXPECT_FALSE(startStop(lab)->isEnabled());
    EXPECT_FALSE(button(lab, "openFileButton")->isEnabled());
    EXPECT_FALSE(button(lab, "openFolderButton")->isEnabled());
    latch.release();

    EXPECT_TRUE(QTest::qWaitFor([&lab] { return !lab.window->isVisible(); }, 10000));
    const std::vector<std::filesystem::path> recordings = recordingsIn(lab.folder.path());
    ASSERT_EQ(recordings.size(), 1U);
    EXPECT_EQ(recordingJson(recordings[0])["complete"], true);
    EXPECT_EQ(recordingJson(recordings[0])["frames"].size(), 2U);
}

// Another camera format is another run: the recording ends with the one it belongs to.
TEST(LabWindow, FormatChangeStopsTheRecording) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    lab.backend->deliver(cameraFrame(120));
    lab.pump();
    button(lab, "recordButton")->click();
    lab.backend->deliver(cameraFrame(130));
    lab.pump();

    lab.choose("formatCombo", QStringLiteral("640×480, 30 fps, NV12"));
    lab.pump();

    EXPECT_FALSE(button(lab, "recordButton")->isChecked());
    const std::vector<std::filesystem::path> recordings = recordingsIn(lab.folder.path());
    ASSERT_EQ(recordings.size(), 1U);
    EXPECT_EQ(recordingJson(recordings[0])["complete"], true);
    EXPECT_EQ(recordingJson(recordings[0])["source"]["kind"], "camera");
}

// --- Send to viewer
// -----------------------------------------------------------------------------------------

QLineEdit* endpointEdit(const Lab& lab) { return lab.window->findChild<QLineEdit*>("endpointEdit"); }

void setEndpoint(const Lab& lab, const QString& endpoint) {
    endpointEdit(lab)->setText(endpoint);
    emit endpointEdit(lab)->editingFinished();
}

TEST(LabWindow, SendIsOffByDefault) {
    Lab lab;

    EXPECT_FALSE(button(lab, "sendCheck")->isChecked());
    EXPECT_EQ(endpointEdit(lab)->text(), QString::fromStdString(visual_debug::defaultEndpoint()));
}

// The frame as "lab" with its overlays, each of the processor's pictures as "lab <name>".
TEST(LabWindow, SendsLabChannels) {
    std::atomic<test_support::RecordingSender*> made = nullptr;
    Lab lab([&made](LabDependencies& dependencies) {
        dependencies.senderMode = lab_core::ResultSender::Mode::Threaded;
        dependencies.makeSender = [&made](const std::string&) {
            auto sender = std::make_unique<test_support::RecordingSender>();
            made = sender.get();
            return sender;
        };
    });
    setEndpoint(lab, QStringLiteral("tcp://127.0.0.1:7140"));
    lab.choose("processorCombo", QStringLiteral("Edges"));
    button(lab, "sendCheck")->click();
    lab.replay(framesOf(test_support::cardFrame()));

    lab.pump();

    ASSERT_NE(made.load(), nullptr);
    ASSERT_TRUE(made.load()->waitForCalls(2));
    const auto calls = made.load()->calls();
    EXPECT_EQ(calls[0].name, "lab");
    EXPECT_EQ(calls[1].name, "lab edges");
}

// Another computer's viewer gets frames that may show real passports, unencrypted: asked once per endpoint.
TEST(LabWindow, RemoteEndpointAsksOnce) {
    QStringList questions;
    Lab lab([&questions](LabDependencies& dependencies) {
        dependencies.confirm = [&questions](const QString& question) {
            questions << question;
            return true;
        };
    });
    setEndpoint(lab, QStringLiteral("tcp://192.168.1.20:7140"));

    button(lab, "sendCheck")->click();
    lab.pump();
    EXPECT_TRUE(lab.status().contains(QStringLiteral("sending to 192.168.1.20")))
        << lab.status().toStdString();
    button(lab, "sendCheck")->click();
    button(lab, "sendCheck")->click();

    ASSERT_EQ(questions.size(), 1);
    EXPECT_TRUE(questions[0].contains(QStringLiteral("192.168.1.20")));
    EXPECT_TRUE(questions[0].contains(QStringLiteral("real passports")));
    EXPECT_TRUE(button(lab, "sendCheck")->isChecked());
}

TEST(LabWindow, RemoteRefusedTurnsSendOff) {
    int senders = 0;
    Lab lab([&senders](LabDependencies& dependencies) {
        dependencies.confirm = [](const QString&) { return false; };
        dependencies.makeSender = [&senders](const std::string&) {
            ++senders;
            return std::make_unique<test_support::RecordingSender>();
        };
    });
    setEndpoint(lab, QStringLiteral("tcp://192.168.1.20:7140"));

    button(lab, "sendCheck")->click();

    EXPECT_FALSE(button(lab, "sendCheck")->isChecked());
    EXPECT_EQ(senders, 0);
}

TEST(LabWindow, LoopbackNeverAsks) {
    int questions = 0;
    int senders = 0;
    Lab lab([&](LabDependencies& dependencies) {
        dependencies.confirm = [&questions](const QString&) {
            ++questions;
            return true;
        };
        dependencies.makeSender = [&senders](const std::string& endpoint) {
            EXPECT_EQ(endpoint, "tcp://127.0.0.1:7140");
            ++senders;
            return std::make_unique<test_support::RecordingSender>();
        };
    });
    setEndpoint(lab, QStringLiteral("tcp://127.0.0.1:7140"));

    button(lab, "sendCheck")->click();

    EXPECT_EQ(questions, 0);
    EXPECT_EQ(senders, 1);
    EXPECT_TRUE(button(lab, "sendCheck")->isChecked());
}

} // namespace
