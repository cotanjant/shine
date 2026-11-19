#include "ui/display_model.hpp"

#include <utility>

namespace shine::ui {

DisplayModel::DisplayModel(QObject* parent)
    : QAbstractListModel(parent) {}

int DisplayModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(displays_.size());
}

QVariant DisplayModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(displays_.size())) {
        return {};
    }

    const auto& d = displays_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IndexRole:
        return static_cast<int>(d.index);
    case NameRole:
        return QString::fromStdString(d.name);
    case WidthRole:
        return static_cast<int>(d.width);
    case HeightRole:
        return static_cast<int>(d.height);
    case RefreshHzRole:
        return static_cast<int>(d.refreshHz);
    case IsPrimaryRole:
        return d.isPrimary;
    case LabelRole:
        return QStringLiteral("%1 · %2×%3 %4Hz")
            .arg(d.index + 1)
            .arg(d.width)
            .arg(d.height)
            .arg(d.refreshHz);
    default:
        return {};
    }
}

QHash<int, QByteArray> DisplayModel::roleNames() const {
    return {
        {IndexRole, "displayIndex"},
        {NameRole, "displayName"},
        {WidthRole, "displayWidth"},
        {HeightRole, "displayHeight"},
        {RefreshHzRole, "refreshHz"},
        {IsPrimaryRole, "isPrimary"},
        {LabelRole, "label"}
    };
}

void DisplayModel::set_displays(std::vector<core::DisplayInfo> displays) {
    beginResetModel();
    displays_ = std::move(displays);
    endResetModel();
}

} // namespace shine::ui
