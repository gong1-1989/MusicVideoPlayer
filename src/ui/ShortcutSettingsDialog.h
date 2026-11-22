#ifndef SHORTCUTSETTINGSDIALOG_H
#define SHORTCUTSETTINGSDIALOG_H

#include <QDialog>
#include <QHash>
#include <QString>

class QLabel;
class QKeySequenceEdit;
class QTableWidget;

/// 快捷键草稿对话框：只编辑并返回绑定，不直接写 AppSettings。
/// 这样系统设置页点“取消”时能连同快捷键一起整体回滚。
class ShortcutSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ShortcutSettingsDialog(const QHash<QString, QString> &bindings,
                                    QWidget *parent = nullptr);

    QHash<QString, QString> bindings() const { return m_bindings; }

protected:
    void accept() override;

private:
    void buildRows();
    void resetAllToDefaults();
    QHash<QString, QString> collectBindings() const;
    bool validateAndWarn();
    void refreshConflictHint();

    QTableWidget *m_table = nullptr;
    QLabel *m_hint = nullptr;
    QHash<QString, QKeySequenceEdit *> m_editors;
    QHash<QString, QString> m_bindings;
    QHash<QString, QString> m_defaults;
};

#endif // SHORTCUTSETTINGSDIALOG_H
