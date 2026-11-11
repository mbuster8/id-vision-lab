#include "main_window.h"
#include "model_folder.h"
#include "scan_banner.h"

#include "card_frame.h"
#include "fake_camera_backend.h"
#include "fake_focus_control.h"
#include "fake_sender.h"
#include "lab_core/focus_controller.h"
#include "latch.h"
#include "list_source.h"
#include "owner_only.h"
#include "palette_support.h"
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
#include <QSignalSpy>
#include <QSlider>
#include <QSplitter>
#include <QTest>
#include <QTreeWidget>
#include <QWheelEvent>

#include <doc_vision/guidance.h>
#include <doc_vision/quality_gates.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <visual_debug/endpoint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
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
        // Fakes only: no test reaches a real viewer, the owner's on 7139 least of all.
        dependencies.makeSender = [this](const std::string&) -> std::unique_ptr<visual_debug::ImageSender> {
            auto sender = std::make_unique<test_support::RecordingSender>(streamAnswer, holdStream);
            sender->log = &streamLog;
            ++streamSenders;
            return sender;
        };
        dependencies.makeLinkSender =
            [this](const std::string& endpoint) -> std::unique_ptr<visual_debug::ImageSender> {
            auto sender = std::make_unique<test_support::RecordingSender>();
            sender->pingAnswer = [this] { return pingAnswer(); };
            sender->log = &linkLog;
            linkEndpoints.push_back(endpoint);
            linkSender = sender.get();
            return sender;
        };
        dependencies.linkMode = lab_core::ViewerLink::Mode::Immediate;
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
        // A fake focus control for every camera: no test reaches a real camera's focus.
        dependencies.makeFocusControl = [this](const QByteArray&) -> std::unique_ptr<lab_core::FocusControl> {
            return std::make_unique<test_support::FakeFocusControl>(focusLog);
        };
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

    // What the viewer row talks to: the stream's senders, and the link's.
    std::function<visual_debug::SendResult(std::string_view)> streamAnswer = [](std::string_view) {
        return visual_debug::SendResult::Delivered;
    };
    test_support::Latch* holdStream = nullptr;
    test_support::SendLog streamLog;
    int streamSenders = 0;
    std::function<visual_debug::ViewerState()> pingAnswer = [] {
        return visual_debug::ViewerState::Listening;
    };
    test_support::SendLog linkLog;
    std::vector<std::string> linkEndpoints;
    test_support::RecordingSender* linkSender = nullptr; // the newest; the link owns it

    test_support::ScratchFolder folder;
    std::atomic<Clock::rep> ticks = Clock::duration(1000s).count();
    std::atomic<int> conversions = 0;
    test_support::FakeCameraBackend* backend = nullptr;
    std::shared_ptr<test_support::FocusLog> focusLog = std::make_shared<test_support::FocusLog>();
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
    EXPECT_TRUE(lab.results().front().startsWith(QStringLiteral("Edges: ")))
        << lab.results().front().toStdString();
}

// Findings a test chooses, and an error when asked; it stands in for Edges.
class ScriptedProcessor : public lab_core::FrameProcessor {
public:
    ScriptedProcessor(std::vector<lab_core::Finding> findings, std::string error,
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
    std::vector<lab_core::Finding> m_findings;
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

// A finding is a row of its name and its value; one without a value fills the first column; an error is a row
// of its own.
TEST(LabWindow, ResultsAreNameAndValueRows) {
    Lab lab([](LabDependencies& dependencies) {
        edgesAre(dependencies, [] {
            return std::make_unique<ScriptedProcessor>(
                std::vector<lab_core::Finding>{
                    {"Document", "not found"}, {"Corners", "4: estimated"}, {"Plain note", ""}},
                "", nullptr);
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
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("Document"));
    EXPECT_EQ(tree->topLevelItem(0)->text(1), QStringLiteral("not found"));
    EXPECT_EQ(tree->topLevelItem(1)->text(0), QStringLiteral("Corners"));
    EXPECT_EQ(tree->topLevelItem(1)->text(1), QStringLiteral("4: estimated"));
    EXPECT_EQ(tree->topLevelItem(2)->text(0), QStringLiteral("Plain note"));
    EXPECT_TRUE(tree->topLevelItem(2)->text(1).isEmpty());
}

// A finding's tone colours its value: red for a problem, green for good, the palette's text otherwise.
TEST(LabWindow, ToneColoursTheValue) {
    Lab lab([](LabDependencies& dependencies) {
        edgesAre(dependencies, [] {
            return std::make_unique<ScriptedProcessor>(
                std::vector<lab_core::Finding>{{"Sharpness", "41 (min 200)", lab_core::Finding::Tone::Bad},
                                               {"Gates", "all pass", lab_core::Finding::Tone::Good},
                                               {"Fill", "34 %"}},
                "", nullptr);
        });
    });
    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();

    const QTreeWidget* tree = lab.resultsTree();
    ASSERT_EQ(tree->topLevelItemCount(), 3);
    EXPECT_EQ(tree->topLevelItem(0)->foreground(1).color(), QColor(0xC2, 0x39, 0x2E));
    EXPECT_EQ(tree->topLevelItem(1)->foreground(1).color(), QColor(0x1E, 0x7D, 0x45));
    EXPECT_EQ(tree->topLevelItem(2)->foreground(1).style(), Qt::NoBrush);
}

TEST(LabWindow, ProcessorErrorIsARow) {
    Lab lab([](LabDependencies& dependencies) {
        edgesAre(dependencies, [] {
            return std::make_unique<ScriptedProcessor>(std::vector<lab_core::Finding>{}, "it broke", nullptr);
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

        EXPECT_EQ(splitter->sizes().at(1), 0); // shut, its edge left to drag it open again
    }
    Lab lab;
    auto* results = lab.window->findChild<QAction*>("resultsAction");
    auto* splitter = lab.window->findChild<QSplitter*>("resultsSplitter");
    EXPECT_FALSE(results->isChecked());
    EXPECT_EQ(splitter->sizes().at(1), 0);

    results->trigger();

    EXPECT_EQ(splitter->sizes().at(1), 260);
}

// Dragging the panel's edge shut hides it, as View → Results does, and dragging it open shows it again.
TEST(LabWindow, ResultsPanelDraggedShutAndOpen) {
    Lab lab;
    auto* results = lab.window->findChild<QAction*>("resultsAction");
    auto* splitter = lab.window->findChild<QSplitter*>("resultsSplitter");
    const int total = splitter->sizes().at(0) + splitter->sizes().at(1);

    splitter->setSizes({total, 0});
    emit splitter->splitterMoved(total, 1);

    EXPECT_EQ(splitter->sizes().at(1), 0);
    EXPECT_FALSE(results->isChecked());
    EXPECT_FALSE(QSettings().value(QStringLiteral("lab/resultsShown"), true).toBool());

    splitter->setSizes({total - 240, 240});
    emit splitter->splitterMoved(total - 240, 1);

    EXPECT_TRUE(results->isChecked());
    EXPECT_TRUE(QSettings().value(QStringLiteral("lab/resultsShown"), false).toBool());
    EXPECT_EQ(QSettings().value(QStringLiteral("lab/resultsWidth")).toInt(), 240);
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
        EXPECT_EQ(lab.entries("processorCombo"),
                  (QStringList{QStringLiteral("None"), QStringLiteral("Edges"),
                               QStringLiteral("Outline (simple)"), QStringLiteral("Document")}));
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
            return std::make_unique<ScriptedProcessor>(std::vector<lab_core::Finding>{}, "", &times);
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

// --- Seeking
// ------------------------------------------------------------------------------------------------

TEST(LabPosition, Formats) {
    using std::chrono::seconds;
    EXPECT_EQ(document_lab::formatPosition(seconds(16), seconds(25)), QStringLiteral("0:16 / 0:25"));
    EXPECT_EQ(document_lab::formatPosition(seconds(3723), seconds(4200)),
              QStringLiteral("1:02:03 / 1:10:00"));
    EXPECT_EQ(document_lab::formatPosition(seconds(300), seconds(4200)), QStringLiteral("0:05:00 / 1:10:00"));
    EXPECT_EQ(document_lab::formatPosition(16900ms, seconds(25)), QStringLiteral("0:16 / 0:25")); // truncated
}

// `count` 4×3 frames at 30 a second.
std::vector<frame_io::ReadResult> thirtyASecond(int count) {
    return test_support::framesApart(count, std::chrono::nanoseconds(1'000'000'000 / 30));
}

QSlider* slider(const Lab& lab) { return lab.window->findChild<QSlider*>("positionSlider"); }
QString position(const Lab& lab) { return lab.window->findChild<QLabel*>("positionLabel")->text(); }

// A replay that can seek and has frames to seek in, paused on its first frame.
void pausedReplay(Lab& lab, int frames, test_support::ListSource::Counters* counters) {
    lab.window->startReplay(test_support::openerOf(thirtyASecond(frames), true, counters));
    lab.pump();
    button(lab, "playPauseButton")->click();
    lab.pump();
}

// Dragging moves only the time; letting go seeks there.
TEST(LabWindow, SliderFollowsAndSeeksOnRelease) {
    Lab lab;
    test_support::ListSource::Counters counters;
    pausedReplay(lab, 300, &counters);
    ASSERT_NE(slider(lab), nullptr);
    EXPECT_TRUE(slider(lab)->isVisible());
    EXPECT_EQ(slider(lab)->minimum(), 0);
    EXPECT_EQ(slider(lab)->maximum(), 299);
    EXPECT_EQ(position(lab), QStringLiteral("0:00 / 0:10"));

    slider(lab)->setSliderDown(true);
    slider(lab)->setValue(150);
    lab.pump();
    EXPECT_EQ(position(lab), QStringLiteral("0:05 / 0:10"));
    EXPECT_EQ(counters.seeks, 0);

    slider(lab)->setSliderDown(false);
    lab.pump();

    EXPECT_EQ(counters.lastSeek, std::optional<std::uint64_t>(150));
    EXPECT_TRUE(lab.status().contains(QStringLiteral("frame 151 of 300"))) << lab.status().toStdString();
    EXPECT_EQ(slider(lab)->value(), 150);
}

// Ten seconds are ten seconds of the replay's own rate, within its first and last frame.
TEST(LabWindow, TenSecondsBackAndForward) {
    Lab lab;
    test_support::ListSource::Counters counters;
    pausedReplay(lab, 600, &counters);
    slider(lab)->setValue(150); // not dragged: a click or a key seeks at once
    lab.pump();
    ASSERT_EQ(counters.lastSeek, std::optional<std::uint64_t>(150));

    button(lab, "forward10Button")->click();
    lab.pump();
    EXPECT_EQ(counters.lastSeek, std::optional<std::uint64_t>(450));

    button(lab, "back10Button")->click();
    lab.pump();
    EXPECT_EQ(counters.lastSeek, std::optional<std::uint64_t>(150));

    slider(lab)->setValue(590);
    lab.pump();
    button(lab, "forward10Button")->click();
    lab.pump();
    EXPECT_EQ(counters.lastSeek, std::optional<std::uint64_t>(599));
    EXPECT_TRUE(lab.status().contains(QStringLiteral("frame 600 of 600"))) << lab.status().toStdString();
}

#if defined(FRAME_IO_HAS_FFMPEG)
// A video read through FFmpeg steps back to the frame before, found by its time.
TEST(LabWindow, StepBackForAVideo) {
    const test_support::ScratchFolder files;
    const std::filesystem::path video = videoFile(files);
    Lab lab([&video](LabDependencies& dependencies) {
        dependencies.chooseFile = [&video] { return qtPath(video); };
    });
    button(lab, "openFileButton")->click();
    ASSERT_TRUE(pumpUntilShown(lab, 1));
    button(lab, "playPauseButton")->click();
    button(lab, "stepButton")->click();
    ASSERT_TRUE(pumpUntilShown(lab, 2));
    button(lab, "stepButton")->click();
    ASSERT_TRUE(pumpUntilShown(lab, 3));
    ASSERT_TRUE(lab.status().contains(QStringLiteral("frame 3 of 5"))) << lab.status().toStdString();

    button(lab, "stepBackButton")->click();

    ASSERT_TRUE(pumpUntilShown(lab, 4));
    EXPECT_TRUE(lab.status().contains(QStringLiteral("frame 2 of 5"))) << lab.status().toStdString();
}
#endif

// A video's seek can take a second at 4K: the status says so, and the handle waits on the target.
TEST(LabWindow, SeekingShowsInTheStatus) {
    Lab lab;
    bool ready = true;
    lab.window->startReplay([&ready](std::string&) -> std::unique_ptr<frame_io::ReplaySource> {
        auto source = std::make_unique<test_support::ListSource>(thirtyASecond(300), true, nullptr);
        source->readyWhen = [&ready] { return ready; };
        return source;
    });
    lab.pump();
    button(lab, "playPauseButton")->click();
    lab.pump();

    ready = false;
    slider(lab)->setValue(200);
    lab.pump();
    EXPECT_TRUE(lab.status().contains(QStringLiteral("seeking…"))) << lab.status().toStdString();
    EXPECT_EQ(slider(lab)->value(), 200);

    ready = true;
    lab.pump();
    EXPECT_FALSE(lab.status().contains(QStringLiteral("seeking…"))) << lab.status().toStdString();
}

TEST(LabWindow, SliderHiddenForACamera) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    lab.backend->deliver(cameraFrame(120));
    lab.pump();

    EXPECT_FALSE(slider(lab)->isVisible());
    EXPECT_FALSE(button(lab, "back10Button")->isEnabled());
    EXPECT_FALSE(button(lab, "forward10Button")->isEnabled());
}

// The status line and the slider follow each result as it is taken, not only the status's refresh twice a
// second: a step shows the frame it reached at once.
TEST(LabWindow, StatusFollowsEachResult) {
    Lab lab;
    lab.window->startReplay(test_support::openerOf(thirtyASecond(5)));
    lab.pump();
    button(lab, "playPauseButton")->click();
    lab.pump();
    ASSERT_TRUE(lab.status().contains(QStringLiteral("frame 1 of 5"))) << lab.status().toStdString();
    QApplication::processEvents(); // the status hooks queued so far, so that none refreshes it below

    button(lab, "stepButton")->click();
    lab.window->pumpEngineForTests(); // the result arrives through the engine's hook, as in the running lab
    QApplication::processEvents();

    EXPECT_TRUE(lab.status().contains(QStringLiteral("frame 2 of 5"))) << lab.status().toStdString();
    EXPECT_EQ(slider(lab)->value(), 1);
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

// The address field held the focus, so Space typed spaces into it. An address never holds one: there, too,
// Space plays and pauses.
TEST(LabWindow, SpaceTogglesPlayInTheAddressField) {
    Lab lab;
    lab.window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(lab.window.get()));
    lab.replay(framesOf(test_support::cardFrame(), 5));
    lab.pump();
    QLineEdit* address = lab.window->findChild<QLineEdit*>("endpointEdit");
    const QString text = address->text();
    address->setFocus();

    QTest::keyClick(address, Qt::Key_Space);
    lab.pump();

    EXPECT_TRUE(lab.status().startsWith(QStringLiteral("Paused"))) << lab.status().toStdString();
    EXPECT_EQ(address->text(), text);
}

// Closing the window while the address field has the focus and an edit in it: the field's last signals must
// not reach a window already partly destroyed.
TEST(LabWindow, ClosingWithAnAddressBeingEditedIsSafe) {
    Lab lab;
    lab.window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(lab.window.get()));
    QLineEdit* address = lab.window->findChild<QLineEdit*>("endpointEdit");
    address->setFocus();
    QTest::keyClicks(address, QStringLiteral("0"));

    lab.window.reset();

    SUCCEED(); // reaching here, rather than an abort in the window's destructor
}

// The canvas has the keyboard focus from the start, takes it back when a source opens or it is clicked, so
// Space reaches play and pause rather than a field or the button that opened the file.
TEST(LabWindow, CanvasKeepsTheFocus) {
    const test_support::ScratchFolder files;
    const std::filesystem::path folder = imageFolder(files);
    Lab lab([&folder](LabDependencies& dependencies) {
        dependencies.chooseFolder = [&folder] { return qtPath(folder); };
    });
    EXPECT_EQ(lab.window->focusWidget(), lab.canvas());

    QAbstractButton* open = button(lab, "openFolderButton");
    open->setFocus();
    open->click();
    EXPECT_EQ(lab.window->focusWidget(), lab.canvas());

    lab.window->findChild<QLineEdit*>("endpointEdit")->setFocus();
    QTest::mouseClick(lab.canvas(), Qt::LeftButton, Qt::NoModifier, QPoint(10, 10)); // through the window
    EXPECT_EQ(lab.window->focusWidget(), lab.canvas());
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

// capture.json keeps its findings as "Name: value" text, whatever their tone.
TEST(LabWindow, CaptureWritesFindingsAsText) {
    Lab lab([](LabDependencies& dependencies) {
        edgesAre(dependencies, [] {
            return std::make_unique<ScriptedProcessor>(
                std::vector<lab_core::Finding>{{"Sharpness", "41 (min 200)", lab_core::Finding::Tone::Bad}},
                "", nullptr);
        });
    });
    lab.choose("processorCombo", QStringLiteral("Edges"));
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
    EXPECT_EQ(json["findings"], nlohmann::json::array({"Sharpness: 41 (min 200)"})) << json.dump();
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

// --- The viewer row
// -----------------------------------------------------------------------------------------

QLineEdit* endpointEdit(const Lab& lab) { return lab.window->findChild<QLineEdit*>("endpointEdit"); }

void setEndpoint(const Lab& lab, const QString& endpoint) {
    endpointEdit(lab)->setText(endpoint);
    emit endpointEdit(lab)->editingFinished();
}

QString viewerState(const Lab& lab) { return lab.window->findChild<QLabel*>("viewerStateLabel")->text(); }

// The stream's own thread sends: the tests wait for what it logs.
void threadedStream(LabDependencies& dependencies) {
    dependencies.senderMode = lab_core::ResultSender::Mode::Threaded;
}

bool logHas(const test_support::SendLog& log, const std::string& line) {
    const std::vector<std::string> lines = log.lines();
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

std::size_t countOf(const test_support::SendLog& log, const std::string& line) {
    const std::vector<std::string> lines = log.lines();
    return static_cast<std::size_t>(std::count(lines.begin(), lines.end(), line));
}

// Pumps until the condition holds, or ten seconds pass.
bool pumpUntil(Lab& lab, const std::function<bool()>& condition) {
    return QTest::qWaitFor([&] {
        lab.pump();
        return condition();
    });
}

// A stream can show passports: it starts on a click only, never at launch.
TEST(LabWindow, StreamIsOffAtLaunch) {
    Lab lab;
    lab.pump();

    EXPECT_EQ(endpointEdit(lab)->text(), QString::fromStdString(visual_debug::defaultEndpoint()));
    EXPECT_TRUE(button(lab, "streamButton")->isEnabled());
    EXPECT_FALSE(button(lab, "pauseStreamButton")->isEnabled());
    EXPECT_FALSE(button(lab, "stopStreamButton")->isEnabled());
    EXPECT_EQ(lab.streamSenders, 0);
    EXPECT_EQ(viewerState(lab), QStringLiteral("available"));
    EXPECT_LT(endpointEdit(lab)->width(), 300); // about 22 characters, not the row's width
}

// The frame as "lab" with its overlays, each of the processor's pictures as "lab <name>".
TEST(LabWindow, StreamSendsLabChannels) {
    Lab lab(threadedStream);
    setEndpoint(lab, QStringLiteral("tcp://127.0.0.1:7140"));
    lab.choose("processorCombo", QStringLiteral("Edges"));
    button(lab, "streamButton")->click();
    lab.replay(framesOf(test_support::cardFrame()));

    lab.pump();

    EXPECT_TRUE(QTest::qWaitFor([&lab] { return logHas(lab.streamLog, "live lab edges"); }));
    EXPECT_EQ(lab.streamLog.lines().front(), "live lab");
    EXPECT_FALSE(button(lab, "streamButton")->isEnabled());
    EXPECT_TRUE(button(lab, "pauseStreamButton")->isEnabled());
    EXPECT_TRUE(button(lab, "stopStreamButton")->isEnabled());
}

// ⏸ keeps the viewer's channel showing the last frame; ■ ends it.
TEST(LabWindow, PauseKeepsAndStopEndsTheChannels) {
    Lab lab(threadedStream);
    lab.choose("processorCombo", QStringLiteral("None"));
    button(lab, "streamButton")->click();
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    ASSERT_TRUE(QTest::qWaitFor([&lab] { return logHas(lab.streamLog, "live lab"); }));

    button(lab, "pauseStreamButton")->click();
    lab.pump(2);
    EXPECT_EQ(countOf(lab.streamLog, "live lab"), 1U);
    EXPECT_FALSE(logHas(lab.streamLog, "end lab"));
    EXPECT_TRUE(button(lab, "streamButton")->isEnabled()); // resumes

    button(lab, "stopStreamButton")->click();

    EXPECT_TRUE(pumpUntil(lab, [&lab] { return logHas(lab.streamLog, "end lab"); }));
    EXPECT_FALSE(button(lab, "stopStreamButton")->isEnabled());
}

TEST(LabWindow, PausedStreamSaysSo) {
    Lab lab(threadedStream);
    button(lab, "streamButton")->click();
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();

    button(lab, "pauseStreamButton")->click();
    lab.pump();

    EXPECT_TRUE(lab.status().contains(QStringLiteral("stream paused"))) << lab.status().toStdString();
}

// While the old sender ends its channels, ▶ waits: a new stream's "lab" must not be ended by the old one.
TEST(LabWindow, StreamDisabledWhileEnding) {
    test_support::Latch latch;
    Lab lab(threadedStream);
    lab.holdStream = &latch;
    button(lab, "streamButton")->click();
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    ASSERT_TRUE(latch.waitUntilEntered()); // the first frame is held in its send

    button(lab, "stopStreamButton")->click();

    EXPECT_FALSE(button(lab, "streamButton")->isEnabled());
    latch.release();
    EXPECT_TRUE(pumpUntil(lab, [&lab] { return button(lab, "streamButton")->isEnabled(); }));
    EXPECT_TRUE(logHas(lab.streamLog, "end lab"));
}

// The indicator says what the last ping found, asked every two seconds.
TEST(LabWindow, IndicatorFollowsThePing) {
    Lab lab;
    lab.pump();
    EXPECT_EQ(viewerState(lab), QStringLiteral("available"));

    lab.pingAnswer = [] { return visual_debug::ViewerState::Unavailable; };
    lab.advance(2s);
    lab.pump();
    EXPECT_EQ(viewerState(lab), QStringLiteral("not available"));

    lab.pingAnswer = [] { return visual_debug::ViewerState::Paused; };
    lab.advance(2s);
    lab.pump();
    EXPECT_EQ(viewerState(lab), QStringLiteral("paused"));

    lab.pingAnswer = [] { return visual_debug::ViewerState::InvalidEndpoint; };
    lab.advance(2s);
    lab.pump();
    EXPECT_EQ(viewerState(lab), QStringLiteral("invalid address"));
}

// An older viewer cannot be pinged; while streaming, the stream's own results say whether it is there. ■ does
// not end its channels: it would only log the ends as rejected.
TEST(LabWindow, OlderViewerFollowsTheStream) {
    Lab lab(threadedStream);
    lab.pingAnswer = [] { return visual_debug::ViewerState::PredatesPing; };
    lab.pump();
    EXPECT_EQ(viewerState(lab), QStringLiteral("available (older viewer)"));

    lab.streamAnswer = [](std::string_view) { return visual_debug::SendResult::ViewerUnavailable; };
    button(lab, "streamButton")->click();
    lab.replay(framesOf(test_support::cardFrame(), 3));
    EXPECT_TRUE(pumpUntil(lab, [&lab] { return viewerState(lab) == QStringLiteral("not available"); }));

    button(lab, "stopStreamButton")->click();
    EXPECT_TRUE(pumpUntil(lab, [&lab] { return button(lab, "streamButton")->isEnabled(); }));
    EXPECT_FALSE(logHas(lab.streamLog, "end lab"));
    EXPECT_EQ(viewerState(lab), QStringLiteral("available (older viewer)"));
}

// Another computer is not even pinged until the user has said yes to it, this session.
TEST(LabWindow, AnotherComputerIsNotPingedUntilConfirmed) {
    QStringList questions;
    Lab lab([&questions](LabDependencies& dependencies) {
        dependencies.confirm = [&questions](const QString& question) {
            questions << question;
            return true;
        };
    });
    const QString remote = QStringLiteral("tcp://192.0.2.1:7140");
    setEndpoint(lab, remote);
    lab.pump();

    EXPECT_EQ(std::count(lab.linkEndpoints.begin(), lab.linkEndpoints.end(), remote.toStdString()), 0);
    EXPECT_EQ(viewerState(lab), QStringLiteral("another computer: not checked"));
    EXPECT_TRUE(questions.isEmpty());

    button(lab, "streamButton")->click();
    lab.pump();

    ASSERT_EQ(questions.size(), 1);
    EXPECT_TRUE(questions[0].contains(QStringLiteral("192.0.2.1")));
    EXPECT_TRUE(questions[0].contains(QStringLiteral("real passports")));
    EXPECT_EQ(lab.linkEndpoints.back(), remote.toStdString());
    EXPECT_EQ(viewerState(lab), QStringLiteral("available"));
    EXPECT_TRUE(lab.status().contains(QStringLiteral("sending to 192.0.2.1"))) << lab.status().toStdString();

    button(lab, "stopStreamButton")->click();
    button(lab, "streamButton")->click();
    EXPECT_EQ(questions.size(), 1); // once per endpoint and session
}

TEST(LabWindow, RefusedComputerIsNotPingedOrStreamed) {
    Lab lab(
        [](LabDependencies& dependencies) { dependencies.confirm = [](const QString&) { return false; }; });
    setEndpoint(lab, QStringLiteral("tcp://192.0.2.1:7140"));

    button(lab, "streamButton")->click();
    lab.pump();

    EXPECT_EQ(lab.streamSenders, 0);
    EXPECT_EQ(viewerState(lab), QStringLiteral("another computer: not checked"));
    EXPECT_FALSE(button(lab, "stopStreamButton")->isEnabled());
}

TEST(LabWindow, LoopbackNeverAsks) {
    int questions = 0;
    Lab lab([&questions](LabDependencies& dependencies) {
        dependencies.confirm = [&questions](const QString&) {
            ++questions;
            return true;
        };
    });
    setEndpoint(lab, QStringLiteral("tcp://127.0.0.1:7140"));

    button(lab, "streamButton")->click();
    lab.pump();

    EXPECT_EQ(questions, 0);
    EXPECT_EQ(lab.streamSenders, 1);
    EXPECT_EQ(lab.linkEndpoints.back(), "tcp://127.0.0.1:7140");
}

// Send Frame sends what Show shows, into the viewer's history: the frame with the processor's overlays,
// whatever the Overlays toggle (the viewer has its own), or a processor's picture.
TEST(LabWindow, SendFrameSendsTheShownPicture) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Edges"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();
    lab.choose("showCombo", QStringLiteral("edges"));
    lab.pump();

    button(lab, "sendFrameButton")->click();
    lab.pump();
    EXPECT_TRUE(logHas(lab.linkLog, "image lab edges 1"));

    lab.choose("showCombo", QStringLiteral("Frame"));
    lab.pump();
    button(lab, "sendFrameButton")->click();
    lab.pump();
    EXPECT_TRUE(logHas(lab.linkLog, "image lab frame 1"));
    EXPECT_TRUE(lab.status().contains(QStringLiteral("sent lab frame 1 to the viewer")))
        << lab.status().toStdString();
}

TEST(LabWindow, SendFrameKeepsTheOverlaysWhenHidden) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Outline (simple)"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();
    button(lab, "overlaysCheck")->click(); // hidden on the canvas

    button(lab, "sendFrameButton")->click();
    lab.pump();

    ASSERT_NE(lab.linkSender, nullptr);
    const auto calls = lab.linkSender->calls();
    const auto still =
        std::find_if(calls.begin(), calls.end(), [](const test_support::RecordingSender::Call& call) {
            return call.kind == test_support::RecordingSender::Kind::Image;
        });
    ASSERT_NE(still, calls.end());
    EXPECT_EQ(still->name, "lab frame 1");
    EXPECT_FALSE(still->description.overlays.empty());
}

// A new endpoint ends the stream in the old viewer; ▶ starts it on the new one, never by itself.
TEST(LabWindow, EndpointChangeStopsTheStream) {
    Lab lab(threadedStream);
    button(lab, "streamButton")->click();
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    ASSERT_TRUE(QTest::qWaitFor([&lab] { return logHas(lab.streamLog, "live lab"); }));

    setEndpoint(lab, QStringLiteral("tcp://127.0.0.1:7141"));

    EXPECT_TRUE(pumpUntil(lab, [&lab] { return logHas(lab.streamLog, "end lab"); }));
    EXPECT_FALSE(button(lab, "stopStreamButton")->isEnabled());
    EXPECT_EQ(lab.streamSenders, 1);
    EXPECT_EQ(lab.linkEndpoints.back(), "tcp://127.0.0.1:7141");
}

// On macOS a click on a button leaves the address field focused, so its editing never finishes: ▶ and Send
// Frame take the address as typed, and the stream goes on when the field loses the focus later.
TEST(LabWindow, TypedAddressCountsForTheStream) {
    Lab lab;
    lab.pump();
    endpointEdit(lab)->setText(QStringLiteral("tcp://127.0.0.1:7140"));

    button(lab, "streamButton")->click();
    lab.pump();

    EXPECT_EQ(lab.linkEndpoints.back(), "tcp://127.0.0.1:7140");
    emit endpointEdit(lab)->editingFinished();
    EXPECT_TRUE(button(lab, "stopStreamButton")->isEnabled());
}

TEST(LabWindow, TypedAddressCountsForSendFrame) {
    Lab lab;
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();
    endpointEdit(lab)->setText(QStringLiteral("tcp://127.0.0.1:7141"));

    button(lab, "sendFrameButton")->click();
    lab.pump();

    EXPECT_EQ(lab.linkEndpoints.back(), "tcp://127.0.0.1:7141");
    EXPECT_TRUE(logHas(lab.linkLog, "image lab frame 1"));
}

TEST(LabWindow, EmptyEndpointDisablesSending) {
    Lab lab;
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();
    ASSERT_TRUE(button(lab, "sendFrameButton")->isEnabled());

    setEndpoint(lab, QString());
    lab.pump();

    EXPECT_EQ(viewerState(lab), QStringLiteral("no address"));
    EXPECT_FALSE(button(lab, "streamButton")->isEnabled());
    EXPECT_FALSE(button(lab, "sendFrameButton")->isEnabled());
}

// Closing ends the lab's channels in the viewer and waits for a ping in progress rather than leave its
// thread.
TEST(LabWindow, CloseEndsTheChannelsAndWaitsForTheLink) {
    test_support::Latch latch;
    Lab lab([&latch](LabDependencies& dependencies) {
        threadedStream(dependencies);
        dependencies.linkMode = lab_core::ViewerLink::Mode::Threaded;
        dependencies.makeLinkSender =
            [&latch](const std::string&) -> std::unique_ptr<visual_debug::ImageSender> {
            auto sender = std::make_unique<test_support::RecordingSender>();
            sender->pingAnswer = [&latch] {
                latch.hold();
                return visual_debug::ViewerState::Listening;
            };
            return sender;
        };
    });
    ASSERT_TRUE(latch.waitUntilEntered()); // the first ping waits for its answer
    button(lab, "streamButton")->click();
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    ASSERT_TRUE(QTest::qWaitFor([&lab] { return logHas(lab.streamLog, "live lab"); }));

    lab.window->close();
    QApplication::processEvents();
    EXPECT_TRUE(lab.window->isVisible());
    latch.release();

    EXPECT_TRUE(QTest::qWaitFor([&lab] { return !lab.window->isVisible(); }, 10000));
    EXPECT_TRUE(logHas(lab.streamLog, "end lab"));
}

// --- The scan's guidance
// ----------------------------------------------------------------------------------

using doc_vision::Hint;

// The Document processor's results, as a test queues them: each frame takes the next, the last one repeats.
struct GuidanceScript {
    std::vector<lab_core::ProcessorResult> results;
    std::size_t next = 0;
    int made = 0;
};

class ScriptedDocumentProcessor : public lab_core::FrameProcessor {
public:
    explicit ScriptedDocumentProcessor(std::shared_ptr<GuidanceScript> script)
        : m_script(std::move(script)) {}
    lab_core::ProcessorResult process(const frame_io::Frame&) override {
        GuidanceScript& script = *m_script;
        if (script.results.empty()) {
            return {};
        }
        return script.results[std::min(script.next++, script.results.size() - 1)];
    }
    void reset() override {}

private:
    std::shared_ptr<GuidanceScript> m_script;
};

// Document made from `script`; the others as lab_core makes them.
std::function<void(LabDependencies&)> documentIs(const std::shared_ptr<GuidanceScript>& script) {
    return [script](LabDependencies& dependencies) {
        dependencies.makeProcessor =
            [script](lab_core::ProcessorKind kind) -> std::unique_ptr<lab_core::FrameProcessor> {
            if (kind != lab_core::ProcessorKind::Document) {
                return lab_core::makeProcessor(kind);
            }
            ++script->made;
            return std::make_unique<ScriptedDocumentProcessor>(script);
        };
    };
}

struct Guided {
    Hint shown = Hint::NoPage;
    bool outline = true;
    bool page = false;
    lab_core::Guidance::Unavailable unavailable = lab_core::Guidance::Unavailable::None;
    bool facts = false; // the page found in this frame, steady, with sharpness 10: never sharp
};

// A result as the Document processor gives it for a shown hint: the chips (the hint's gate ✗), the outline as
// its only overlay, a "page" picture when asked, and a Bad Sharpness row.
lab_core::ProcessorResult guided(const Guided& wanted) {
    lab_core::ProcessorResult result;
    lab_core::Guidance guidance;
    guidance.frameHint = wanted.shown;
    guidance.shownHint = wanted.shown;
    guidance.unavailable = wanted.unavailable;
    if (wanted.unavailable != lab_core::Guidance::Unavailable::None) {
        guidance.reason = "missing";
    }
    if (wanted.shown != Hint::NoPage) {
        guidance.chips.visible = true;
        guidance.chips.passed.fill(true);
        if (const std::optional<doc_vision::Gate> gate = doc_vision::gateOf(wanted.shown)) {
            guidance.chips.passed[static_cast<std::size_t>(*gate)] = false;
        }
    }
    if (wanted.outline) {
        guidance.outlineOverlay = result.overlays.size();
        result.overlays.push_back(visual_debug::Polygon{
            .points = {{100, 100}, {500, 110}, {490, 380}, {110, 370}},
            .label = "Document",
            .colour = wanted.shown == Hint::Good ? visual_debug::kOverlayGreen : visual_debug::kOverlayRed});
    }
    if (wanted.page) {
        result.images.push_back(
            {.name = "page", .image = cv::Mat(44, 62, CV_8UC3, cv::Scalar::all(180)), .description = {}});
    }
    if (wanted.facts) {
        lab_core::PageFacts facts;
        facts.quad = {{{10, 8}, {54, 8}, {54, 40}, {10, 40}}};
        facts.frameSize = cv::Size(64, 48);
        facts.sharpness = 10.0;
        facts.longerSide = 0.69;
        facts.verdicts.passed.fill(true);
        facts.verdicts.passed[static_cast<std::size_t>(doc_vision::Gate::Sharp)] =
            wanted.shown != Hint::Blurred;
        facts.frameHint = wanted.shown;
        guidance.page = facts;
    }
    result.findings = {{"Sharpness", "41 (min 200)", lab_core::Finding::Tone::Bad}};
    result.guidance = guidance;
    return result;
}

std::shared_ptr<GuidanceScript> scriptOf(std::vector<lab_core::ProcessorResult> results) {
    auto script = std::make_shared<GuidanceScript>();
    script->results = std::move(results);
    return script;
}

document_lab::ScanBanner* banner(const Lab& lab) {
    return lab.window->findChild<document_lab::ScanBanner*>("scanBanner");
}
document_lab::GateChips* chips(const Lab& lab) {
    return lab.window->findChild<document_lab::GateChips*>("gateChips");
}
// The banner's big line, or nothing while it is hidden.
QString bannerLine(const Lab& lab) {
    const document_lab::ScanBanner* shown = banner(lab);
    if (shown == nullptr) {
        return QStringLiteral("(no banner)");
    }
    return shown->isHidden() || !shown->content() ? QString() : shown->content()->bigLine;
}

const QString kShowThePage = QStringLiteral("Show the passport's photo page");

TEST(LabWindow, DocumentIsAProcessor) {
    Lab lab;

    EXPECT_TRUE(lab.entries("processorCombo").contains(QStringLiteral("Document")));
}

TEST(LabWindow, BannerShowsTheDrawnResultsHint) {
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::Blurred})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame()));

    lab.pump();

    EXPECT_EQ(bannerLine(lab), QStringLiteral("Blurred: hold still"));
    ASSERT_NE(chips(lab), nullptr);
    EXPECT_FALSE(chips(lab)->isHidden());
    EXPECT_FALSE(chips(lab)->states().passed[static_cast<std::size_t>(doc_vision::Gate::Sharp)]);
    EXPECT_TRUE(chips(lab)->states().passed[static_cast<std::size_t>(doc_vision::Gate::InView)]);
}

TEST(LabWindow, BannerHiddenForOtherProcessors) {
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::Blurred})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    ASSERT_FALSE(bannerLine(lab).isEmpty());

    lab.choose("processorCombo", QStringLiteral("Edges"));

    EXPECT_TRUE(banner(lab)->isHidden());
    EXPECT_TRUE(chips(lab)->isHidden());
}

// Review Focus 2: away and back, the banner hides at once and starts again from the first hint.
TEST(LabWindow, ProcessorSwitchHidesAndResetsTheBanner) {
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::Blurred})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    pausedReplay(lab, 10, nullptr);
    ASSERT_EQ(bannerLine(lab), QStringLiteral("Blurred: hold still"));

    lab.choose("processorCombo", QStringLiteral("Edges"));
    EXPECT_TRUE(banner(lab)->isHidden());
    lab.choose("processorCombo", QStringLiteral("Document"));

    EXPECT_EQ(bannerLine(lab), kShowThePage);
    EXPECT_TRUE(chips(lab)->isHidden());
}

TEST(LabWindow, StartShowsHintOneBeforeAnyResult) {
    Lab lab(twoCameras);
    lab.choose("processorCombo", QStringLiteral("Document"));

    startStop(lab)->click();

    EXPECT_EQ(bannerLine(lab), kShowThePage);
    EXPECT_EQ(lab.canvas()->emptyText(), QString());
}

TEST(LabWindow, NoCameraBanner) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Document"));
    EXPECT_EQ(bannerLine(lab), QStringLiteral("No camera found"));
    EXPECT_EQ(lab.canvas()->emptyText(), QString());

    lab.choose("processorCombo", QStringLiteral("Edges"));

    EXPECT_TRUE(banner(lab)->isHidden());
    EXPECT_EQ(
        lab.canvas()->emptyText(),
        QStringLiteral("No camera found. Connect one, or open a video, an image or a folder of images."));
}

TEST(LabWindow, AccessOffBanner) {
    Lab lab([](LabDependencies& dependencies) {
        twoCameras(dependencies);
        fakeOf(dependencies).permission = false;
    });
    lab.choose("processorCombo", QStringLiteral("Document"));

    startStop(lab)->click();

    EXPECT_EQ(bannerLine(lab), QStringLiteral("Camera access is off"));
    EXPECT_EQ(lab.canvas()->emptyText(), QString());
}

TEST(LabWindow, CameraStoppedBanner) {
    Lab lab(twoCameras);
    lab.choose("processorCombo", QStringLiteral("Document"));
    startStop(lab)->click();

    lab.backend->fail(QStringLiteral("unplugged"));

    EXPECT_EQ(bannerLine(lab), QStringLiteral("The camera stopped"));
    EXPECT_EQ(banner(lab)->content()->smallLine, QStringLiteral("unplugged"));
    EXPECT_EQ(lab.canvas()->emptyText(), QString());
}

TEST(LabWindow, IdleBanner) {
    Lab lab(twoCameras);

    lab.choose("processorCombo", QStringLiteral("Document"));

    EXPECT_EQ(bannerLine(lab), QStringLiteral("Start the camera"));
    EXPECT_EQ(lab.canvas()->emptyText(), QString());
}

TEST(LabWindow, NoCameraGoesWhenACameraArrives) {
    Lab lab;
    lab.choose("processorCombo", QStringLiteral("Document"));
    ASSERT_EQ(bannerLine(lab), QStringLiteral("No camera found"));

    lab.backend->changeCameras({camera("b", QStringLiteral("C920"))});

    EXPECT_EQ(bannerLine(lab), QStringLiteral("Start the camera"));
}

// Review Focus 4: a model that will not open keeps its banner on every result of the run.
TEST(LabWindow, UnavailableModelBannerStaysForTheRun) {
    Lab lab(documentIs(scriptOf({guided(
        {.shown = Hint::NoPage, .outline = false, .unavailable = lab_core::Guidance::Unavailable::Model})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame(), 3));

    for (int i = 0; i < 3; ++i) {
        lab.pump();
        EXPECT_EQ(bannerLine(lab), QStringLiteral("The document model is missing")) << i;
        EXPECT_TRUE(chips(lab)->isHidden()) << i;
    }
}

// The real processor, from a model folder with no model in it, says so.
TEST(LabWindow, MissingModelFolderSaysTheModelIsMissing) {
    test_support::ScratchFolder empty;
    Lab lab([&empty](LabDependencies& dependencies) {
        dependencies.modelFolder = QString::fromStdU16String(empty.path().u16string());
    });
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame()));

    lab.pump();

    EXPECT_EQ(bannerLine(lab), QStringLiteral("The document model is missing"));
    EXPECT_TRUE(lab.results().contains(QStringLiteral("Detector: unavailable")))
        << lab.results().join('|').toStdString();
}

TEST(LabWindow, DefaultModelFolderIsBesideTheProgram) {
    EXPECT_TRUE(document_lab::defaultModelFolder().endsWith(QStringLiteral("/models")))
        << document_lab::defaultModelFolder().toStdString();
}

// P8: with Show on a picture the frame lacks, the placeholder shows and the banner stays above it, fed by the
// newest result.
TEST(LabWindow, BannerAbovePlaceholder) {
    Lab lab(documentIs(scriptOf(
        {guided({.shown = Hint::Good, .page = true}), guided({.shown = Hint::NoPage, .outline = false})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    lab.choose("showCombo", QStringLiteral("page"));

    lab.pump();

    ASSERT_TRUE(lab.placeholder()->isVisible());
    const QObjectList siblings = lab.placeholder()->parent()->children();
    EXPECT_GT(siblings.indexOf(banner(lab)), siblings.indexOf(lab.placeholder()));
    EXPECT_GT(siblings.indexOf(chips(lab)), siblings.indexOf(lab.placeholder()));
    EXPECT_EQ(bannerLine(lab), kShowThePage);
}

// P15: on a narrow canvas the big line shrinks or elides and the chips wrap onto two rows.
TEST(LabWindow, NarrowCanvasElides) {
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::MoveToMiddle})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame()));
    lab.pump();
    auto* splitter = lab.window->findChild<QSplitter*>("resultsSplitter");
    const QList<int> sizes = splitter->sizes();
    splitter->setSizes({360, sizes.value(0) + sizes.value(1) - 360});
    QApplication::processEvents();
    ASSERT_EQ(lab.canvas()->width(), 360);

    banner(lab)->repaint();

    EXPECT_LE(banner(lab)->width(), 360);
    EXPECT_LT(banner(lab)->fittedBigLine().pixelSize, 32);
    EXPECT_GT(chips(lab)->height(), chips(lab)->heightForWidth(2000));
}

TEST(LabWindow, StatusSaysTheState) {
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::Blurred})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame()));

    lab.pump();

    EXPECT_TRUE(lab.status().contains(QStringLiteral("State: adjust (sharpness)")))
        << lab.status().toStdString();
}

// Leaving Document takes its state out of the status bar at once, not at the next refresh.
TEST(LabWindow, StatusDropsTheStateWithTheProcessor) {
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::Blurred})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    ASSERT_TRUE(lab.status().contains(QStringLiteral("State: "))) << lab.status().toStdString();

    lab.choose("processorCombo", QStringLiteral("Edges"));

    EXPECT_FALSE(lab.status().contains(QStringLiteral("State: "))) << lab.status().toStdString();
}

const viewer_core::Polygon* guideOn(const Lab& lab) {
    for (const viewer_core::Overlay& overlay : lab.canvas()->overlays()) {
        const auto* polygon = std::get_if<viewer_core::Polygon>(&overlay);
        if (polygon != nullptr && polygon->style == viewer_core::Polygon::Style::Guide) {
            return polygon;
        }
    }
    return nullptr;
}

// P4, P10: the outline is drawn as the canvas's guide, coloured by the shown hint, and hides with Overlays.
TEST(LabWindow, GuideOutlineOnTheCanvas) {
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::Good}), guided({.shown = Hint::Blurred})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame(), 3));

    lab.pump();
    ASSERT_EQ(lab.canvas()->overlays().size(), 1U);
    const auto* good = std::get_if<viewer_core::Polygon>(&lab.canvas()->overlays()[0]);
    ASSERT_NE(good, nullptr);
    EXPECT_EQ(good->style, viewer_core::Polygon::Style::Guide);
    EXPECT_EQ(good->label, "");
    EXPECT_EQ(good->colour, (viewer_core::Colour{0x3D, 0xDC, 0x84}));

    lab.pump();
    ASSERT_NE(guideOn(lab), nullptr);
    EXPECT_EQ(guideOn(lab)->colour, (viewer_core::Colour{0xFF, 0x6B, 0x5E}));

    button(lab, "overlaysCheck")->click();
    EXPECT_FALSE(lab.canvas()->overlaysShown());
}

TEST(LabWindow, GuideOnlyOnTheFrameEntry) {
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::Good, .page = true})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();
    ASSERT_NE(guideOn(lab), nullptr);

    lab.choose("showCombo", QStringLiteral("page"));
    lab.pump();

    EXPECT_EQ(guideOn(lab), nullptr);
}

// Review R1: the outline's colour is fixed with its picture's job, so a picture and its outline arrive
// together.
TEST(LabWindow, OutlineTravelsWithItsPicture) {
    test_support::Latch latch;
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, viewer_core::Colour>> made;
    Lab lab([&](LabDependencies& dependencies) {
        documentIs(scriptOf({guided({.shown = Hint::Good}), guided({.shown = Hint::Blurred})}))(dependencies);
        dependencies.conversionMode = LiveConverter::Mode::Threaded;
        dependencies.conversion = [&](const LiveJob& job) {
            if (job.frameNumber == 1) {
                latch.hold();
            }
            for (const viewer_core::Overlay& overlay : job.tags.overlayList()) {
                if (const auto* polygon = std::get_if<viewer_core::Polygon>(&overlay)) {
                    const std::lock_guard lock(mutex);
                    made.emplace_back(job.frameNumber, polygon->colour);
                }
            }
            return convertLiveFrame(job);
        };
    });
    lab.choose("processorCombo", QStringLiteral("Document"));
    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump(); // the first picture is held converting
    ASSERT_TRUE(latch.waitUntilEntered());
    lab.pump(); // the second waits
    latch.release();
    ASSERT_TRUE(waitForPictures(*lab.window, 2));
    QApplication::processEvents();

    const std::lock_guard lock(mutex);
    ASSERT_EQ(made.size(), 2U);
    EXPECT_EQ(made[0], (std::pair{std::uint64_t{1}, viewer_core::Colour{0x3D, 0xDC, 0x84}}));
    EXPECT_EQ(made[1], (std::pair{std::uint64_t{2}, viewer_core::Colour{0xFF, 0x6B, 0x5E}}));
    ASSERT_NE(guideOn(lab), nullptr);
    EXPECT_EQ(guideOn(lab)->colour, (viewer_core::Colour{0xFF, 0x6B, 0x5E}));
    EXPECT_EQ(bannerLine(lab), QStringLiteral("Blurred: hold still"));
}

TEST(LabWindow, PausedReplayKeepsTheLastHint) {
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::Blurred})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    pausedReplay(lab, 10, nullptr);

    QApplication::processEvents();
    lab.pump();

    EXPECT_EQ(bannerLine(lab), QStringLiteral("Blurred: hold still"));
}

// P17: a value's tone follows the appearance when it changes, as the running app sees it.
TEST(LabWindow, ToneFollowsAPaletteSwitch) {
    const test_support::PaletteGuard guard;
    Lab lab(documentIs(scriptOf({guided({.shown = Hint::Blurred})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    pausedReplay(lab, 10, nullptr);
    QTreeWidgetItem* sharpness = lab.resultsTree()->topLevelItem(0);
    ASSERT_NE(sharpness, nullptr);

    QGuiApplication::setPalette(
        test_support::paletteWith(QColor(0x1E, 0x21, 0x25), QColor(0xE8, 0xEA, 0xED)));
    QEvent changed(QEvent::PaletteChange);
    QCoreApplication::sendEvent(lab.window.get(), &changed);
    EXPECT_EQ(lab.resultsTree()->topLevelItem(0)->foreground(1).color(), QColor(0xFF, 0x6B, 0x5E));

    test_support::applyPalette(test_support::paletteWith(QColor(236, 236, 236), QColor(0, 0, 0)));
    EXPECT_EQ(lab.resultsTree()->topLevelItem(0)->foreground(1).color(), QColor(0xC2, 0x39, 0x2E));
}

// --- A3.3: focus on the page
// ------------------------------------------------------------------------------------------------

// A camera run's frames: delivered 33 ms apart on the window's clock, each pumped.
void cameraFrames(Lab& lab, int count) {
    for (int i = 0; i < count; ++i) {
        lab.advance(33ms);
        lab.backend->deliver(cameraFrame(120));
        lab.pump();
    }
}

// A single click at the canvas's centre, as the user makes one. Waits for the click itself rather than a
// fixed time, which a slow paint on a busy runner can use up.
void clickCanvas(Lab& lab) {
    lab.canvas()->setClickDelay(0);
    QSignalSpy clicked(lab.canvas(), &ImageView::clicked);
    const QPointF centre = QRectF(lab.canvas()->rect()).center();
    sendMouse(*lab.canvas(), QEvent::MouseButtonPress, centre);
    sendMouse(*lab.canvas(), QEvent::MouseButtonRelease, centre);
    EXPECT_TRUE(!clicked.isEmpty() || clicked.wait(5000));
}

std::function<void(LabDependencies&)> camerasAndDocument(const std::shared_ptr<GuidanceScript>& script) {
    return [script](LabDependencies& dependencies) {
        twoCameras(dependencies);
        documentIs(script)(dependencies);
    };
}

// Review Focus 5: while the lens sweeps, never Good: the focusing banner, Sharp ✗ and the Bad outline.
TEST(LabWindow, SearchingShowsFocusingOverGood) {
    Lab lab(camerasAndDocument(scriptOf({guided({.shown = Hint::Good})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    startStop(lab)->click();
    cameraFrames(lab, 1);
    ASSERT_EQ(bannerLine(lab), QStringLiteral("Good: hold still"));

    clickCanvas(lab);
    cameraFrames(lab, 1);

    EXPECT_EQ(bannerLine(lab), QStringLiteral("Hold still: focusing"));
    EXPECT_FALSE(chips(lab)->states().passed[static_cast<std::size_t>(doc_vision::Gate::Sharp)]);
    EXPECT_TRUE(chips(lab)->states().passed[static_cast<std::size_t>(doc_vision::Gate::InView)]);
    ASSERT_NE(guideOn(lab), nullptr);
    EXPECT_EQ(guideOn(lab)->colour, (viewer_core::Colour{0xFF, 0x6B, 0x5E}));
}

TEST(LabWindow, CannotFocusWithoutControlWords) {
    Lab lab([](LabDependencies& dependencies) {
        camerasAndDocument(scriptOf({guided({.shown = Hint::Blurred, .facts = true})}))(dependencies);
        dependencies.makeFocusControl = nullptr;
    });
    lab.choose("processorCombo", QStringLiteral("Document"));
    startStop(lab)->click();

    cameraFrames(lab, 95); // steady and blurred for more than 3 s

    EXPECT_EQ(bannerLine(lab), QStringLiteral("This camera can't make the page sharp"));
    EXPECT_EQ(banner(lab)->content()->smallLine, QStringLiteral("Try more light, or another camera"));
}

TEST(LabWindow, CannotFocusAfterSearchWords) {
    Lab lab(camerasAndDocument(scriptOf({guided({.shown = Hint::Blurred, .facts = true})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    startStop(lab)->click();

    for (int i = 0; i < 500 && bannerLine(lab) != QStringLiteral("This camera can't focus this close"); ++i) {
        cameraFrames(lab, 1);
    }

    EXPECT_EQ(bannerLine(lab), QStringLiteral("This camera can't focus this close"));
    EXPECT_EQ(banner(lab)->content()->smallLine, QStringLiteral("Move the page a little further away"));
    EXPECT_TRUE(lab.focusLog->autoFocus); // put back
}

// "Can't focus" shows only while the shown hint is Blurred: an earlier hint keeps its own banner.
TEST(LabWindow, TooFarKeepsItsBanner) {
    std::vector<lab_core::ProcessorResult> results(95, guided({.shown = Hint::Blurred, .facts = true}));
    results.push_back(guided({.shown = Hint::TooFar, .facts = true}));
    Lab lab([&results](LabDependencies& dependencies) {
        camerasAndDocument(scriptOf(results))(dependencies);
        dependencies.makeFocusControl = nullptr;
    });
    lab.choose("processorCombo", QStringLiteral("Document"));
    startStop(lab)->click();
    cameraFrames(lab, 95);
    ASSERT_EQ(bannerLine(lab), QStringLiteral("This camera can't make the page sharp"));

    cameraFrames(lab, 3);

    EXPECT_EQ(bannerLine(lab), QStringLiteral("Move closer"));
}

TEST(LabWindow, ClickFocusesOnlyInACameraRunOnTheFrame) {
    { // a replay: nothing reaches a camera
        Lab lab;
        lab.replay(framesOf(test_support::cardFrame(), 3));
        lab.pump();
        clickCanvas(lab);
        lab.pump();
        EXPECT_TRUE(lab.focusLog->calls.empty());
    }
    Lab lab(camerasAndDocument(scriptOf({guided({.shown = Hint::Blurred, .page = true})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    startStop(lab)->click();
    cameraFrames(lab, 1);

    lab.choose("showCombo", QStringLiteral("page"));
    cameraFrames(lab, 1);
    clickCanvas(lab);
    cameraFrames(lab, 1);
    EXPECT_TRUE(lab.status().contains(QStringLiteral("Show the Frame to focus by clicking")))
        << lab.status().toStdString();
    EXPECT_EQ(lab.focusLog->count("auto off"), 0);

    lab.choose("showCombo", QStringLiteral("Frame"));
    cameraFrames(lab, 1);
    clickCanvas(lab);
    cameraFrames(lab, 1);
    EXPECT_EQ(lab.focusLog->count("auto off"), 1);
}

TEST(LabWindow, FKeyFocusesTheCentre) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    cameraFrames(lab, 1);

    QTest::keyClick(lab.canvas(), Qt::Key_F);
    cameraFrames(lab, 1);

    EXPECT_EQ(lab.focusLog->count("auto off"), 1);
}

TEST(LabWindow, FocusRowShowsInTheResults) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    cameraFrames(lab, 1);

    EXPECT_TRUE(lab.results().contains(QStringLiteral("Focus: camera")))
        << lab.results().join(" | ").toStdString();
}

TEST(LabWindow, NullMakeFocusControlMeansNone) {
    Lab lab([](LabDependencies& dependencies) {
        twoCameras(dependencies);
        dependencies.makeFocusControl = nullptr;
    });
    startStop(lab)->click();
    cameraFrames(lab, 1);

    EXPECT_TRUE(lab.results().contains(QStringLiteral("Focus: camera (lab can't set it)")))
        << lab.results().join(" | ").toStdString();
}

TEST(LabWindow, FocusNoteInTheStatusLine) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    cameraFrames(lab, 1);
    clickCanvas(lab);
    cameraFrames(lab, 1);
    EXPECT_TRUE(lab.status().contains(QStringLiteral("Focusing…"))) << lab.status().toStdString();

    lab.advance(5100ms);
    lab.pump();

    EXPECT_FALSE(lab.status().contains(QStringLiteral("Focusing…"))) << lab.status().toStdString();
}

// A new run's controller numbers its notes from 1 again: its first note still shows.
TEST(LabWindow, NoteOfANewRunShows) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    cameraFrames(lab, 1);
    clickCanvas(lab);
    cameraFrames(lab, 1);
    lab.advance(6s);
    lab.pump();
    startStop(lab)->click(); // stop
    startStop(lab)->click(); // and start again
    cameraFrames(lab, 1);

    clickCanvas(lab);
    cameraFrames(lab, 1);

    EXPECT_TRUE(lab.status().contains(QStringLiteral("Focusing…"))) << lab.status().toStdString();
}

// Final review F1, as the user meets it: the window stops before it starts another camera or a replay.
TEST(LabWindow, FailedRestoreShowsAfterACameraChange) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    cameraFrames(lab, 1);
    clickCanvas(lab);
    cameraFrames(lab, 1);
    ASSERT_FALSE(lab.focusLog->autoFocus);
    lab.focusLog->failSetAuto = true;

    lab.choose("cameraCombo", QStringLiteral("C920"));
    lab.pump();

    EXPECT_TRUE(lab.status().contains(QString::fromUtf8(lab_core::kFocusRestoreFailed)))
        << lab.status().toStdString();
}

TEST(LabWindow, FailedRestoreShowsAfterOpeningAFile) {
    Lab lab(twoCameras);
    startStop(lab)->click();
    cameraFrames(lab, 1);
    clickCanvas(lab);
    cameraFrames(lab, 1);
    lab.focusLog->failSetAuto = true;

    lab.replay(framesOf(test_support::cardFrame(), 3));
    lab.pump();

    EXPECT_TRUE(lab.status().contains(QString::fromUtf8(lab_core::kFocusRestoreFailed)))
        << lab.status().toStdString();
}

// The Legion's hand check: a window 2000 px wide cut the focus note off the status line's end. News comes
// straight after the run's state, before the source and the counters, so a narrow window keeps it.
TEST(LabWindow, NewsComesBeforeTheCounters) {
    Lab lab(camerasAndDocument(scriptOf({guided({.shown = Hint::Blurred})})));
    lab.choose("processorCombo", QStringLiteral("Document"));
    startStop(lab)->click();
    cameraFrames(lab, 1);
    clickCanvas(lab);
    cameraFrames(lab, 1);
    lab.focusLog->failSetAuto = true;
    lab.choose("cameraCombo", QStringLiteral("C920")); // the restore fails: the engine's message
    lab.pump();
    lab.focusLog->failSetAuto = false; // the next camera's focus can be set: a click gives a note
    cameraFrames(lab, 1);
    clickCanvas(lab);
    cameraFrames(lab, 1);

    const QString status = lab.status();
    const qsizetype counters = status.indexOf(QStringLiteral("fps processed"));
    ASSERT_GE(counters, 0) << status.toStdString();
    EXPECT_TRUE(status.startsWith(QStringLiteral("Live"))) << status.toStdString();
    for (const QString& news : {QStringLiteral("State: adjust (sharpness)"), QStringLiteral("Focusing…"),
                                QString::fromUtf8(lab_core::kFocusRestoreFailed)}) {
        const qsizetype at = status.indexOf(news);
        EXPECT_GE(at, 0) << news.toStdString() << " missing: " << status.toStdString();
        EXPECT_LT(at, counters) << news.toStdString() << " after the counters: " << status.toStdString();
    }
}

// Final review F8: F is the focus key only while a camera runs; otherwise the window's lists keep it.
TEST(LabWindow, FKeyOnlyDuringACameraRun) {
    Lab lab(twoCameras);
    QAction* focus = lab.window->findChild<QAction*>(QStringLiteral("focusAction"));
    ASSERT_NE(focus, nullptr);
    EXPECT_FALSE(focus->isEnabled());

    startStop(lab)->click();
    EXPECT_TRUE(focus->isEnabled());

    startStop(lab)->click();
    EXPECT_FALSE(focus->isEnabled());
}

} // namespace
