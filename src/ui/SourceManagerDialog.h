#ifndef SOURCEMANAGERDIALOG_H
#define SOURCEMANAGERDIALOG_H

#include "net/SourceConfig.h"

#include <QDialog>
#include <QPoint>
#include <QVector>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;
class SourceRegistry;
struct SourceParserSettings;

QT_BEGIN_NAMESPACE
namespace Ui {
class SourceManagerDialog;
}
QT_END_NAMESPACE

/// 搜索源管理：只负责源列表、新增/更新，以及新增/更新时调用的外部解析器配置。
/// 源字段不再在界面里手工编辑；新增和更新都先走解析器探测，确认后才回填列表。
class SourceManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SourceManagerDialog(SourceRegistry *registry, QWidget *parent = nullptr,
                                 bool deferCommit = false);
    ~SourceManagerDialog() override;

    void setDraftSources(const QVector<SourceConfig> &sources);
    QVector<SourceConfig> committedSources() const { return m_committedSources; }

protected:
    void accept() override;

private:
    void bindUi();
    void reloadList(int targetRow);
    void updateActions();
    void onAdd();
    void onRefreshSelected();
    void onRemoveSelected();
    void onListContextMenu(const QPoint &pos);
    void onItemChanged(QListWidgetItem *item);
    void selectRow(int row);
    int currentIndex() const;
    QString uniqueId(const QString &preferred) const;
    SourceParserSettings parserSettings() const;
    void saveParserSettings() const;
    bool saveAll();

    Ui::SourceManagerDialog *ui;
    SourceRegistry *m_registry = nullptr;
    bool m_deferCommit = false;
    bool m_updating = false;

    QListWidget *m_list = nullptr;
    QLineEdit *m_command = nullptr;
    QLineEdit *m_arguments = nullptr;
    QSpinBox *m_timeout = nullptr;
    QPushButton *m_btnAdd = nullptr;
    QPushButton *m_btnRefresh = nullptr;

    QVector<SourceConfig> m_drafts;
    QVector<SourceConfig> m_committedSources;
};

#endif // SOURCEMANAGERDIALOG_H
