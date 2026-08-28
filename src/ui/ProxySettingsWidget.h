#pragma once

// ProxySettingsWidget.h — 代理配置控件，覆盖 6 种代理类型。
//
// 一个控件**同时服务两处**：设备对话框里的「代理」行（每台设备各自的代理）
// 与设置页里的「代理」标签页（那份全局代理）。做成两套独立表单的话，
// 6 种类型的字段显隐逻辑就得维护两遍，早晚不一致。
//
// 内部写法沿用对话框里已确立的那套：类型 QComboBox（一律读 currentData()
// 而非 currentText()，理由见 add_device_dialog.h）+ QStackedWidget 按类型
// 切换参数页——即 m_authStack 的模式。

#include <QList>
#include <QWidget>

#include "net/ProxyConfig.h"

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace cubeshell {

// 跳板机下拉框的候选项。只放 id + 显示名：这个控件不认识 DeviceConfigStore
//（它在 ui/ 层被两个互不相识的宿主复用），设备目录由宿主喂进来。
struct ProxyDeviceItem {
    QString id;
    QString name;
};

class ProxySettingsWidget : public QWidget {
    Q_OBJECT
public:
    explicit ProxySettingsWidget(QWidget *parent = nullptr);

    // 要不要在类型下拉里提供「全局代理」。
    // 设置页里的那一份**自己就是**全局代理，必须关掉——否则用户能选出
    // Global→Global，那是唯一的无限递归入口（resolveGlobalProxy 会把它
    // 当直连兜住，但让用户选得出来本身就是错的）。
    void setGlobalOptionEnabled(bool on);

    // 跳板机下拉的候选设备。excludeId 是「自己」（设备对话框传当前设备 id），
    // 用于把自己从候选里摘掉——把自己当跳板是成环的最短路径。
    //
    // 可以在 setConfig() 之前或之后调用，两种顺序都对：目录存在成员里，
    // 两个函数都会拿它重建下拉。
    void setDeviceCatalog(const QList<ProxyDeviceItem> &devices,
                          const QString &excludeId = QString());

    // 当前配置。password 为空的语义与 DeviceEntry::password 一致：
    // 「用户没有输入新口令」，**不是**「口令被清空」。要判断用户是否真的
    // 动过口令框，用 passwordEdited()。
    ProxyConfig config() const;
    void setConfig(const ProxyConfig &cfg);

    // 钥匙串里已存有这份配置的代理口令 → 口令框可留空，占位符改成
    // 「已保存，留空则不修改」。与 AddDeviceDialog::setHasStoredPassword 同义。
    void setHasStoredPassword(bool has);
    bool passwordEdited() const { return m_passwordEdited; }

    // 必填校验。失败时填 err 并返回 false。宿主在 accept() 里调。
    bool validate(QString *err) const;

    // 让宿主把本控件内部的表单纳入它的统一排版：返回内部所有标签控件，
    // 供宿主计算统一的标签列宽（见 AddDeviceDialog 构造末尾那段）。
    QList<QWidget *> formLabels() const;

signals:
    // 类型切换后行数变了，宿主可能要重算窗口高度（见 AddDeviceDialog
    // ::onProtocolChanged 末尾那段关于 QDialog 不自动收缩的注释）。
    void typeChanged();

private:
    void onTypeChanged();
    ProxyType selectedType() const;

    // --- 跳板机行 ---
    struct HopRow {
        QWidget   *row = nullptr;
        QLabel    *label = nullptr;
        QComboBox *combo = nullptr;
    };
    void addHopRow(const QString &deviceId);
    void removeHopRow(QWidget *row);
    void relabelHops();
    // 用当前目录重填一个下拉，并尽量选中 selectedId。
    // selectedId 不在目录里（被引用的设备已删除）时会**保留**它并显示成
    // 「已删除」，而不是静默丢掉——丢掉会让用户以为配置还好着，
    // 保留则由 validate() 当场拦下来要求修正。
    void fillHopCombo(QComboBox *combo, const QString &selectedId) const;

    QComboBox      *m_type = nullptr;
    QStackedWidget *m_stack = nullptr;
    QFormLayout    *m_form = nullptr;      // 类型行 + 堆叠页

    // page: 无参数（直连 / 全局 / 系统），只放一句说明
    QLabel *m_noParamHint = nullptr;

    // page: host/port（HTTP 与 SOCKS5 共用——字段完全一样）
    QFormLayout *m_hostForm = nullptr;
    QLineEdit *m_host = nullptr;
    QLineEdit *m_port = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    // 端口框里当前放的是哪种代理的默认端口。切类型时据此判断「用户没改过端口」，
    // 从而可以安全换成新类型的默认端口。同 AddDeviceDialog::m_portDefaultFor。
    ProxyType m_portDefaultFor = ProxyType::None;

    // page: 代理命令
    QFormLayout *m_commandForm = nullptr;
    QLineEdit *m_command = nullptr;

    // page: 跳转服务器
    QVBoxLayout *m_hopLayout = nullptr;    // 承载 HopRow
    QPushButton *m_addHop = nullptr;
    QLabel      *m_hopHint = nullptr;
    QList<HopRow> m_hops;

    QList<ProxyDeviceItem> m_catalog;
    QString m_excludeId;

    bool m_hasStoredPassword = false;
    bool m_passwordEdited = false;
};

} // namespace cubeshell
