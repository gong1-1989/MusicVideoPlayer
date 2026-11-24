#ifndef SOURCEMANAGERDIALOG_H
#define SOURCEMANAGERDIALOG_H

#include "net/SourceConfig.h"
#include "ui/SourceProbeDialog.h"

#include <QDialog>
#include <QPoint>
#include <QVector>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class SourceRegistry;

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
    /// 设置页延迟提交时，用它把当前草稿解析器配置带进源管理窗口。
    void setParserSettings(const SourceParserSettings &settings);
    /// 延迟提交时，返回用户本次是否改过解析器配置。
    SourceParserSettings parserSettings() const { return m_parserSettings; }
    bool parserSettingsDirty() const { return m_parserDirty; }

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
    void openParserConfig();
    void refreshParserSummary();
    void selectRow(int row);
    int currentIndex() const;
    QString uniqueId(const QString &preferred) const;
    void saveParserSettings() const;
    bool saveAll();

    Ui::SourceManagerDialog *ui;
    SourceRegistry *m_registry = nullptr;
    bool m_deferCommit = false;
    bool m_updating = false;

    QListWidget *m_list = nullptr;
    QLabel *m_parserLabel = nullptr;
    QPushButton *m_btnParserConfig = nullptr;
    QPushButton *m_btnAdd = nullptr;
    QPushButton *m_btnRefresh = nullptr;

    SourceParserSettings m_parserSettings;
    bool m_parserDirty = false;
    QVector<SourceConfig> m_drafts;
    QVector<SourceConfig> m_committedSources;
};

#endif // SOURCEMANAGERDIALOG_H
