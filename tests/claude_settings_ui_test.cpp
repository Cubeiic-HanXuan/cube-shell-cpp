// 设置页表单几何回归；离屏运行，不加载或写入真实 Claude 设置。
#include "claude_code/ClaudeCodeBackend.h"
#include "claude_code/SettingsPanel.h"
#include "util/ThemeManager.h"
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QFormLayout>
#include <QTemporaryDir>
#include <QThread>

using namespace cubeshell;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { qWarning() << "FAIL:" << #c << "line" << __LINE__; ++failures; } } while (0)
static void settle()
{
    for (int i = 0; i < 12; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
}
static void checkAlignment(SettingsPanel &panel)
{
    int left = -1, width = -1, count = 0;
    const auto forms = panel.findChildren<QFormLayout *>();
    CHECK(forms.size() == 3);
    for (auto *form : forms) {
        for (int row = 0; row < form->rowCount(); ++row) {
            auto *item = form->itemAt(row, QFormLayout::FieldRole);
            auto *field = item ? item->widget() : nullptr;
            CHECK(field);
            if (!field) continue;
            const int fieldLeft = field->mapTo(&panel, QPoint()).x();
            if (count++ == 0) { left = fieldLeft; width = field->width(); }
            CHECK(fieldLeft == left);
            CHECK(field->width() == width);
            CHECK(field->isVisible());
            CHECK(fieldLeft + field->width() <= panel.width());
        }
    }
    CHECK(count == 8);
    CHECK(width > 200);
}
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir temp;
    CHECK(temp.isValid());
    qputenv("CLAUDE_CONFIG_DIR", temp.path().toUtf8());
    ClaudeCodeBackend backend;
    SettingsPanel panel(&backend);
    panel.resize(1000, 900);
    panel.show();
    settle();
    checkAlignment(panel);
    const auto originalFont = panel.font();
    for (auto theme : {ThemeManager::Appearance::Dark, ThemeManager::Appearance::Light}) {
        ThemeManager::applyTheme(&app, theme);
        for (double scale : {1.0, 1.5, 1.0}) {
            auto font = originalFont;
            if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * scale));
            else font.setPointSizeF(font.pointSizeF() * scale);
            panel.setFont(font);
            for (int width : {720, 1000, 1280}) {
                panel.resize(width, 1100);
                settle();
                CHECK(panel.width() == width);
                checkAlignment(panel);
            }
        }
    }
    // 提供商切换会启用输入框并更新模型选项，宽度仍应一致。
    for (auto *combo : panel.findChildren<QComboBox *>())
        if (combo->count() > 0) combo->setCurrentIndex(combo->count() - 1);
    settle();
    checkAlignment(panel);
    qInfo() << "claude_settings_ui_test failures:" << failures;
    return failures ? 1 : 0;
}
