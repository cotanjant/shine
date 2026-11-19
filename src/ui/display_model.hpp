#pragma once

#include "shine/core/types.hpp"

#include <QAbstractListModel>

#include <vector>

namespace shine::ui {

class DisplayModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        IndexRole = Qt::UserRole + 1,
        NameRole,
        WidthRole,
        HeightRole,
        RefreshHzRole,
        IsPrimaryRole,
        LabelRole
    };

    explicit DisplayModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_displays(std::vector<core::DisplayInfo> displays);

private:
    std::vector<core::DisplayInfo> displays_;
};

} // namespace shine::ui
