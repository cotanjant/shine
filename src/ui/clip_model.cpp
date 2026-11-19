#include "ui/clip_model.hpp"

#include <QDateTime>

#include <cmath>

namespace shine::ui {

namespace {

[[nodiscard]] QString format_duration_sec(double seconds) {
    const int total = std::max(0, static_cast<int>(std::llround(seconds)));
    const int mins = total / 60;
    const int secs = total % 60;
    return QStringLiteral("%1:%2")
        .arg(mins)
        .arg(secs, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString format_size(std::uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL) {
        const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    }
    const double kb = static_cast<double>(bytes) / 1024.0;
    return QStringLiteral("%1 KB").arg(kb, 0, 'f', 0);
}

[[nodiscard]] QString format_codec(const std::string& fourcc) {
    if (fourcc == "avc1" || fourcc == "avc3") {
        return QStringLiteral("H.264");
    }
    if (fourcc == "hvc1" || fourcc == "hev1") {
        return QStringLiteral("H.265");
    }
    return QString::fromStdString(fourcc);
}

} // namespace

ClipModel::ClipModel(QObject* parent)
    : QAbstractListModel(parent) {}

int ClipModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(clips_.size());
}

QVariant ClipModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(clips_.size())) {
        return {};
    }

    const auto& c = clips_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case FilePathRole: {
        const std::u8string u8 = c.path.u8string();
        return QString::fromUtf8(reinterpret_cast<const char*>(u8.data()), static_cast<qsizetype>(u8.size()));
    }
    case FileNameRole:
        return QString::fromStdString(c.fileName);
    case TitleRole:
        return QString::fromStdString(c.displayTitle);
    case DurationTextRole:
        return format_duration_sec(c.durationSeconds);
    case SizeTextRole:
        return format_size(c.fileSizeBytes);
    case DateTextRole:
        return QDateTime::fromSecsSinceEpoch(c.modifiedEpochSec).toString(QStringLiteral("MMM d, HH:mm"));
    case CodecRole:
        return format_codec(c.codecFourCC);
    case HasAudioRole:
        return c.hasAudio;
    case IsClipRole:
        return c.isClip;
    default:
        return {};
    }
}

QHash<int, QByteArray> ClipModel::roleNames() const {
    return {
        {FilePathRole, "filePath"},
        {FileNameRole, "fileName"},
        {TitleRole, "title"},
        {DurationTextRole, "durationText"},
        {SizeTextRole, "sizeText"},
        {DateTextRole, "dateText"},
        {CodecRole, "codec"},
        {HasAudioRole, "hasAudio"},
        {IsClipRole, "isClip"}
    };
}

void ClipModel::reload(const std::filesystem::path& directory) {
    beginResetModel();
    clips_ = core::scan_clips(directory);
    endResetModel();
    emit countChanged();
}

} // namespace shine::ui
