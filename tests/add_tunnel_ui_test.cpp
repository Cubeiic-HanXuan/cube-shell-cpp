// SSH 隧道表单回归：真实 qdarktheme 深/浅主题、字体变化、窗口缩放、配置往返。
// 离屏运行，不建立 SSH 连接、不修改用户配置。
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLayout>
#include <QLineEdit>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTemporaryDir>

#include "dialogs/AddTunnelDialog.h"
#include "util/ThemeManager.h"

using namespace cubeshell;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

template <typename T> static T *field(AddTunnelDialog &dialog, const char *name)
{
    auto *widget = dialog.findChild<T *>(QString::fromLatin1(name));
    Q_ASSERT(widget);
    return widget;
}

static void settle(AddTunnelDialog &dialog)
{
    QCoreApplication::processEvents();
    dialog.layout()->activate();
    QCoreApplication::processEvents();
}

static void checkGeometry(AddTunnelDialog &dialog)
{
    auto *type = field<QComboBox>(dialog, "tunnelType");
    auto *device = field<QComboBox>(dialog, "tunnelDevice");
    CHECK(type->x() == device->x());
    CHECK(type->width() == device->width());
    CHECK(type->geometry().right() <= dialog.width() - 16);
    for (const char *name : {"tunnelName", "remoteBindAddress", "localBindAddress", "browserOpen"}) {
        const auto *edit = field<QLineEdit>(dialog, name);
        CHECK(edit->x() == type->x());
        CHECK(edit->width() == type->width());
    }
    QStyleOptionComboBox option;
    option.initFrom(type);
    option.currentText = type->currentText();
    option.editable = type->isEditable();
    option.iconSize = type->iconSize();
    const QRect textRect = type->style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, type);
    const QRect arrowRect = type->style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, type);
    const int availableWidth = arrowRect.isEmpty() ? textRect.width()
        : qMin(textRect.right() + 1, arrowRect.left()) - textRect.left();
    const int textWidth = type->fontMetrics().horizontalAdvance(type->currentText());
    if (availableWidth < textWidth || textRect.height() < type->fontMetrics().height())
        qWarning() << "Clipped mode:" << type->currentText() << "text field" << textRect
                   << "arrow" << arrowRect << "text width" << textWidth
                   << "font height" << type->fontMetrics().height();
    CHECK(availableWidth >= textWidth);
    CHECK(textRect.height() >= type->fontMetrics().height());
}

static void testLayout()
{
    const QFont original = QApplication::font();
    const QString longName = QStringLiteral("生产服务器-") + QString(180, QChar(0x670d));
    // 原生样式也要覆盖，避免修复只在某一主题下有效。
    for (int theme : {0, 1, 2}) {
        if (theme == 0) qApp->setStyleSheet(QString());
        else ThemeManager::applyTheme(qApp, theme == 1 ? ThemeManager::Appearance::Light : ThemeManager::Appearance::Dark);
        AddTunnelDialog dialog({QStringLiteral("dev"), longName});
        auto *type = field<QComboBox>(dialog, "tunnelType");
        auto *device = field<QComboBox>(dialog, "tunnelDevice");
        CHECK(device->currentText() == QStringLiteral("dev"));
        CHECK(dialog.findChild<QFormLayout *>()->fieldGrowthPolicy() == QFormLayout::AllNonFixedFieldsGrow);
        dialog.show();
        for (double scale : {1.0, 1.5, 2.0}) {
            QFont font = original;
            if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * scale));
            else font.setPointSizeF(font.pointSizeF() * scale);
            dialog.setFont(font);
            for (int width : {480, 560, 820}) {
                dialog.resize(width, 420);
                settle(dialog);
                const QSize initialSize = dialog.size();
                const int initialFieldWidth = type->width();
                for (int index = 0; index < type->count(); ++index) {
                    type->setCurrentIndex(index);
                    settle(dialog);
                    checkGeometry(dialog);
                    CHECK(type->width() == initialFieldWidth);
                    CHECK(dialog.size() == initialSize);
                    CHECK(dialog.entry().tunnelType == type->itemData(index).toString());
                }
                device->setCurrentIndex(1);
                settle(dialog);
                checkGeometry(dialog);
                CHECK(device->toolTip() == longName);
                CHECK(device->itemData(1, Qt::ToolTipRole).toString() == longName);
                CHECK(dialog.entry().deviceName == longName);
                CHECK(dialog.size() == initialSize); // 长设备名不把整个窗口撑出屏幕
                device->setCurrentIndex(0);
            }
        }
    }
    qApp->setStyleSheet(QString());
}

static void testEntryRoundTrip()
{
    for (const QString &mode : {QStringLiteral("本地"), QStringLiteral("远程"), QStringLiteral("动态")}) {
        AddTunnelDialog dialog({QStringLiteral("dev"), QStringLiteral("prod")});
        TunnelEntry entry;
        entry.tunnelType = mode;
        entry.deviceName = QStringLiteral("prod");
        entry.localBindAddress = QStringLiteral("127.0.0.1:1080");
        entry.remoteBindAddress = mode == QStringLiteral("动态") ? QString() : QStringLiteral("localhost:8080");
        entry.browserOpen = QStringLiteral("http://127.0.0.1:1080");
        dialog.setEntry(QStringLiteral("nginx"), entry);
        CHECK(dialog.tunnelName() == QStringLiteral("nginx"));
        CHECK(field<QLineEdit>(dialog, "tunnelName")->isReadOnly());
        CHECK(dialog.entry().toJson() == entry.toJson());
        CHECK(field<QLineEdit>(dialog, "remoteBindAddress")->isEnabled() == (mode != QStringLiteral("动态")));
        dialog.accept();
        CHECK(dialog.result() == QDialog::Accepted);
    }
    AddTunnelDialog dialog({QStringLiteral("dev")});
    auto *remote = field<QLineEdit>(dialog, "remoteBindAddress");
    remote->setText(QStringLiteral("localhost:9000"));
    field<QComboBox>(dialog, "tunnelType")->setCurrentIndex(2);
    CHECK(!remote->isEnabled());
    CHECK(remote->text().isEmpty());
    CHECK(remote->placeholderText().contains(QStringLiteral("无需填写")));
    field<QComboBox>(dialog, "tunnelType")->setCurrentIndex(0);
    CHECK(remote->isEnabled());
    CHECK(remote->placeholderText().contains(QStringLiteral("localhost:8080")));

    AddTunnelDialog empty(QStringList{});
    empty.show();
    settle(empty);
    checkGeometry(empty);
    CHECK(field<QComboBox>(empty, "tunnelDevice")->count() == 0);
    CHECK(empty.entry().deviceName.isEmpty());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    QApplication app(argc, argv);
    testLayout();
    testEntryRoundTrip();
    qInfo() << "add_tunnel_ui_test:" << failures << "failure(s)";
    return failures ? 1 : 0;
}
