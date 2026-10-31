#include "display_text.h"

#include <QCoreApplication>
#include <QDateTime>

#include <QStringList>

#include <cstdint>

namespace {

constexpr int kNumberPrecision = 10; // enough for a 32-bit integer and a float's digits

QString withTags(const QString& summary, const viewer_core::ImageTags& tags) {
    const QString tagged = tagsText(tags);
    return tagged.isEmpty() ? summary : summary + QStringLiteral(" · ") + tagged;
}

} // namespace

QString formattedNumber(double value) { return QLocale::c().toString(value, 'g', kNumberPrecision); }

// Standard names, not translated.
QString tagsText(const viewer_core::ImageTags& tags) {
    QStringList parts;
    if (tags.transfer) {
        parts << (*tags.transfer == viewer_core::Transfer::Hlg  ? QStringLiteral("HLG")
                  : *tags.transfer == viewer_core::Transfer::Pq ? QStringLiteral("PQ")
                                                                : QStringLiteral("SDR"));
    }
    if (tags.primaries) {
        parts << (*tags.primaries == viewer_core::Primaries::Bt2020 ? QStringLiteral("BT.2020")
                                                                    : QStringLiteral("BT.709"));
    }
    if (tags.hasValueRange()) {
        parts << formattedNumber(tags.valueRange.min) + u'–' + formattedNumber(tags.valueRange.max);
    }
    return parts.join(QStringLiteral(" · "));
}

QString imageDetails(const viewer_core::ReceivedImage& image, const QLocale& locale) {
    const auto size = [&locale](std::uint64_t bytes) {
        return locale.formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeSIFormat);
    };
    const QString summary = withTags(QString::fromStdString(image.summary), image.origin.tags);
    if (image.origin.source == viewer_core::ImageSource::OpenedFile && image.origin.fileBytes) {
        return QCoreApplication::translate("ImageInfoBar", "%1 · %2 file")
            .arg(summary, size(*image.origin.fileBytes));
    }
    return QStringLiteral("%1 · %2").arg(summary, size(image.pixelDataBytes()));
}

QString liveDetails(const QString& summary, const viewer_core::LiveChannel& channel,
                    const viewer_core::LiveStats& stats) {
    const QString state = channel.frozen ? QCoreApplication::translate("ImageInfoBar", "frozen")
                          : channel.idle ? QCoreApplication::translate("ImageInfoBar", "idle")
                                         : QCoreApplication::translate("ImageInfoBar", "live");
    QString text = QCoreApplication::translate("ImageInfoBar", "%1 · %2 · %3 fps in · %4 drawn · %5 dropped")
                       .arg(state, withTags(summary, channel.shownTags))
                       .arg(stats.arrivedPerSecond)
                       .arg(stats.drawnPerSecond)
                       .arg(channel.dropped);
    if (stats.age) {
        text += QCoreApplication::translate("ImageInfoBar", " · %1 ms").arg(stats.age->count());
    }
    return text;
}

QString snapshotName(const QString& channel, qint64 epochMilliseconds) {
    return channel + u' ' +
           QDateTime::fromMSecsSinceEpoch(epochMilliseconds).toString(QStringLiteral("HH:mm:ss.zzz"));
}

QString clockTime(qint64 epochMicroseconds) {
    return QDateTime::fromMSecsSinceEpoch(epochMicroseconds / 1000).toString(QStringLiteral("HH:mm:ss"));
}
