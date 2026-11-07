#include "application_identity.h"

#include <QApplication>
#include <QByteArray>
#include <QFont>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

// Every test starts from the defaults, as on a first start, whether ctest runs it alone or the binary runs
// them all in one process: a sort or a width one test chose never reaches the next. The viewer's settings
// hold the appearance the example sender shares.
class ClearSettingsBeforeEachTest : public testing::EmptyTestEventListener {
    void OnTestStart(const testing::TestInfo& /*test*/) override {
        QSettings().clear();
        QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                  QLatin1String(qt_common::kOrganisationDomain), QLatin1String(qt_common::kViewerName))
            .clear();
    }
};

} // namespace

// Widgets need a QApplication. The offscreen platform lets these tests run without a display, from ctest,
// Qt Creator or a CI runner alike. Settings go to INI files in a folder that lives for the whole run, so a
// test never reads or changes the owner's real viewer settings.
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
#ifdef _WIN32
    // The offscreen platform looks for fonts only in QT_QPA_FONTDIR, by default Qt's own lib/fonts, which a
    // Windows installation does not have. With no font it measures every character as a box as wide as the
    // font is high, and layouts grow far wider than in the real viewer. Windows' own fonts fix that.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR") + QStringLiteral("\\Fonts")).toLocal8Bit());
    }
#endif
    testing::InitGoogleTest(&argc, argv);
    const QTemporaryDir settingsFolder;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsFolder.path());
    QApplication application(argc, argv);
#ifdef _WIN32
    // Offscreen names no family, so Qt would take the first font it finds (Agency FB); every Windows app gets
    // the system's message font, Segoe UI 9 pt, instead.
    QApplication::setFont(QFont(QStringLiteral("Segoe UI"), 9));
#endif
    qt_common::setApplicationIdentity(QStringLiteral("Visual Debug Tests"));
    testing::UnitTest::GetInstance()->listeners().Append(new ClearSettingsBeforeEachTest);
    return RUN_ALL_TESTS();
}
