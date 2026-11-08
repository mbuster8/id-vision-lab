#include "data_folder.h"

#include "image_store.h"
#include "owner_only.h"
#include "private_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

namespace {

const QString kEndpoint = QStringLiteral("tcp://127.0.0.1:7139");

TEST(DataFolder, NamesFolderAfterEndpoint) {
    EXPECT_EQ(DataFolder::folderNameFor(kEndpoint), QStringLiteral("tcp_127.0.0.1_7139"));
    // The scheme, "_", then the address with every character outside [A-Za-z0-9.] as "_".
    EXPECT_EQ(DataFolder::folderNameFor(QStringLiteral("tcp://[::1]:7140")),
              QStringLiteral("tcp____1__7140"));
    EXPECT_EQ(DataFolder::folderNameFor(QStringLiteral("tcp://127.0.0.1:*")), QString());
    // Not an endpoint: as a folder name it would point outside the root.
    EXPECT_EQ(DataFolder::folderNameFor(QStringLiteral("..")), QString());
}

TEST(DataFolder, CreatesOwnerOnlyFolders) {
    const QTemporaryDir root;

    const auto folder = DataFolder::open(root.path(), kEndpoint);

    ASSERT_NE(folder, nullptr);
    EXPECT_EQ(folder->path(), QDir(root.path()).filePath(QStringLiteral("tcp_127.0.0.1_7139")));
    EXPECT_EQ(folder->logFile(), folder->path() + QStringLiteral("/activity.log"));
    EXPECT_EQ(folder->imagesFolder(), folder->path() + QStringLiteral("/images"));
    for (const QString& path : {folder->path(), folder->imagesFolder()}) {
        SCOPED_TRACE(path.toStdString());
        EXPECT_TRUE(QFileInfo(path).isDir());
        EXPECT_TRUE(test_support::isOwnerOnly(path));
    }
}

// Private whatever the root lets others do: on Windows a shared or redirected profile folder can pass wider
// access on to everything made in it, so the folders get an access list of their own, which the files they
// hold inherit.
TEST(DataFolder, StaysOwnerOnlyUnderAWideOpenRoot) {
    const QTemporaryDir root;
    ASSERT_TRUE(test_support::letEveryoneReadWhatItHolds(root.path()));

    const auto folder = DataFolder::open(root.path(), kEndpoint);

    ASSERT_NE(folder, nullptr);
    ImageStore(folder->imagesFolder()).write("stored", cv::Mat(2, 2, CV_8UC1, cv::Scalar(1)));
    ASSERT_TRUE(
        writeFileAtomically(folder->logFile(), [](QIODevice& device) { return device.write("x") == 1; }));
    QStringList paths{folder->path(), folder->imagesFolder(), folder->logFile()};
    for (const QString& image : QDir(folder->imagesFolder()).entryList(QDir::Files)) {
        paths.append(QDir(folder->imagesFolder()).filePath(image));
    }
    ASSERT_GT(paths.size(), 3); // the stored image is among them
    for (const QString& path : paths) {
        SCOPED_TRACE(path.toStdString());
        EXPECT_TRUE(test_support::isOwnerOnly(path));
    }
}

// A folder an earlier viewer made under a wide-open root is private once a viewer opens it. On Windows its
// files too, which inherited the root's access; on macOS and Linux every viewer made its files 0600 itself.
TEST(DataFolder, MakesAnEarlierFolderPrivate) {
    const QTemporaryDir root;
    ASSERT_TRUE(test_support::letEveryoneReadWhatItHolds(root.path()));
    const QString earlier = QDir(root.path()).filePath(DataFolder::folderNameFor(kEndpoint));
    ASSERT_TRUE(QDir().mkpath(QDir(earlier).filePath(QStringLiteral("images"))));
    const QString log = QDir(earlier).filePath(QStringLiteral("activity.log"));
    QFile file(log);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
#ifdef _WIN32
    ASSERT_FALSE(test_support::isOwnerOnly(log)); // it inherited the root's access
#endif

    const auto folder = DataFolder::open(root.path(), kEndpoint);

    ASSERT_NE(folder, nullptr);
    QStringList paths{folder->path(), folder->imagesFolder()};
#ifdef _WIN32
    paths.append(log);
#endif
    for (const QString& path : paths) {
        SCOPED_TRACE(path.toStdString());
        EXPECT_TRUE(test_support::isOwnerOnly(path));
    }
}

TEST(DataFolder, SecondOpenFailsWhileLocked) {
    const QTemporaryDir root;
    auto first = DataFolder::open(root.path(), kEndpoint);
    ASSERT_NE(first, nullptr);

    DataFolder::Problem problem = DataFolder::Problem::None;
    EXPECT_EQ(DataFolder::open(root.path(), kEndpoint, &problem), nullptr);
    EXPECT_EQ(problem, DataFolder::Problem::Locked);

    first.reset();
    EXPECT_NE(DataFolder::open(root.path(), kEndpoint), nullptr);
}

TEST(DataFolder, WildcardPortKeepsNothing) {
    const QTemporaryDir root;

    DataFolder::Problem problem = DataFolder::Problem::None;
    EXPECT_EQ(DataFolder::open(root.path(), QStringLiteral("tcp://127.0.0.1:*"), &problem), nullptr);
    EXPECT_EQ(problem, DataFolder::Problem::NoFolderName);
    EXPECT_TRUE(QDir(root.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

// Turning storage off counts and deletes every endpoint's images; starting with it off deletes only those no
// running viewer holds.
TEST(DataFolder, CountsAndRemovesStoredImagesEverywhere) {
    const QTemporaryDir root;
    const auto held = DataFolder::open(root.path(), kEndpoint);
    ASSERT_NE(held, nullptr);
    ImageStore(held->imagesFolder()).write("held", cv::Mat(2, 2, CV_8UC1, cv::Scalar(1)));
    QString releasedImages;
    {
        const auto released = DataFolder::open(root.path(), QStringLiteral("tcp://127.0.0.1:7140"));
        ASSERT_NE(released, nullptr);
        releasedImages = released->imagesFolder();
        ImageStore store(releasedImages);
        store.write("released 1", cv::Mat(2, 2, CV_8UC1, cv::Scalar(1)));
        store.write("released 2", cv::Mat(2, 2, CV_8UC1, cv::Scalar(1)));
    }
    EXPECT_EQ(DataFolder::storedImageCountAll(root.path()), 3);

    DataFolder::removeStoredImagesEverywhere(root.path(), true);
    EXPECT_EQ(ImageStore(held->imagesFolder()).storedImageCount(), 1);
    EXPECT_EQ(ImageStore(releasedImages).storedImageCount(), 0);

    DataFolder::removeStoredImagesEverywhere(root.path(), false);
    EXPECT_EQ(DataFolder::storedImageCountAll(root.path()), 0);
}

} // namespace
