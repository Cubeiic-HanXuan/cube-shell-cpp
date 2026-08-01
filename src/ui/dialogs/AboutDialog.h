#pragma once

// AboutDialog.h — 关于对话框（Logo/版本/作者/简介/许可证）。
// 对应Python: function/about.py::AboutDialog

#include <QDialog>

namespace cubeshell {

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget *parent = nullptr);

signals:
    // "检查更新" 按钮被点击（更新逻辑在 Phase 5，由主窗口决定如何处理）。
    // 对应Python: function/about.py::AboutDialog._check_update
    void checkUpdateRequested();
};

} // namespace cubeshell
