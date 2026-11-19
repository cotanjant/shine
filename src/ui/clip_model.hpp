#pragma once

#include "shine/core/clip_library.hpp"

#include <QAbstractListModel>

#include <filesystem>
#include <vector>

namespace shine::ui {

class ClipModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        FilePathRole = Qt::UserRole + 1,
        FileNameRole,
        TitleRole,
        DurationTextRole,
        SizeTextRole,
        DateTextRole,
        CodecRole,
        HasAudioRole,
        IsClipRole
    };

    explicit ClipModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void reload(const std::filesystem::path& directory);

signals:
    void countChanged();

private:
    std::vector<core::ClipEntry> clips_;
};

} // namespace shine::ui
