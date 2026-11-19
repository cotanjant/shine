#include "ui/recorder_controller.hpp"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSystemTrayIcon>
#include <QWindow>

#include <memory>

namespace {

[[nodiscard]] QIcon build_tray_icon(bool recording) {
    QPixmap pix(64, 64);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.setPen(QPen(QColor(QStringLiteral("#2D3448")), 3.0));
    p.setBrush(QColor(QStringLiteral("#111520")));
    p.drawEllipse(6, 6, 52, 52);

    const QColor dotColor = recording
        ? QColor(QStringLiteral("#FF4D5E"))
        : QColor(QStringLiteral("#E2E8F0"));
    p.setPen(Qt::NoPen);
    p.setBrush(dotColor);
    p.drawEllipse(20, 20, 24, 24);

    return QIcon(pix);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Shine"));
    app.setOrganizationName(QStringLiteral("Shine"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const bool trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
    app.setQuitOnLastWindowClosed(!trayAvailable);

    shine::ui::RecorderController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("recorder"), &controller);
    engine.loadFromModule(QStringLiteral("Shine"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    auto* mainWindow = qobject_cast<QWindow*>(engine.rootObjects().constFirst());

    std::unique_ptr<QSystemTrayIcon> trayIcon;
    std::unique_ptr<QMenu> trayMenu;
    if (trayAvailable) {
        trayIcon = std::make_unique<QSystemTrayIcon>(build_tray_icon(controller.recording()));
        trayIcon->setToolTip(QStringLiteral("Shine Screen Recorder"));

        trayMenu = std::make_unique<QMenu>();
        QAction* toggleAction = trayMenu->addAction(QStringLiteral("Start / Stop Recording"));
        QAction* showAction = trayMenu->addAction(QStringLiteral("Show / Hide"));
        trayMenu->addSeparator();
        QAction* quitAction = trayMenu->addAction(QStringLiteral("Quit"));

        QObject::connect(toggleAction, &QAction::triggered, &controller,
                         &shine::ui::RecorderController::toggleRecording);

        QObject::connect(showAction, &QAction::triggered, [mainWindow]() {
            if (mainWindow == nullptr) {
                return;
            }
            if (mainWindow->isVisible()) {
                mainWindow->hide();
            } else {
                mainWindow->show();
                mainWindow->raise();
                mainWindow->requestActivate();
            }
        });

        QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);

        QObject::connect(trayIcon.get(), &QSystemTrayIcon::activated,
                         [mainWindow](QSystemTrayIcon::ActivationReason reason) {
                             if (reason == QSystemTrayIcon::Trigger && mainWindow != nullptr) {
                                 mainWindow->show();
                                 mainWindow->raise();
                                 mainWindow->requestActivate();
                             }
                         });

        QObject::connect(&controller, &shine::ui::RecorderController::stateChanged,
                         [&trayIcon, &controller]() {
                             if (trayIcon) {
                                 trayIcon->setIcon(build_tray_icon(controller.recording()));
                             }
                         });

        trayIcon->setContextMenu(trayMenu.get());
        trayIcon->show();
    }

    return app.exec();
}
