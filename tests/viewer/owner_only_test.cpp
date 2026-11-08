#include "owner_only.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

// The check the privacy tests rely on must be able to fail, or they would pass whatever the viewer did.
TEST(OwnerOnly, NoticesAFileEveryoneCanRead) {
    const QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    const QString path = QDir(folder.path()).filePath(QStringLiteral("shared.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    ASSERT_TRUE(test_support::isOwnerOnly(path));

    ASSERT_TRUE(test_support::letEveryoneRead(path));

    EXPECT_FALSE(test_support::isOwnerOnly(path));
}

TEST(OwnerOnly, MissingFileIsNotOwnerOnly) {
    EXPECT_FALSE(test_support::isOwnerOnly(QStringLiteral("/nonexistent/missing.txt")));
}

} // namespace
