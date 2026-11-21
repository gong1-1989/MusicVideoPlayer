#ifndef DOWNLOADMANAGERDIALOG_H
#define DOWNLOADMANAGERDIALOG_H

#include "download/DownloadJob.h"

#include <QDialog>
#include <QPointer>

class DownloadManager;
class QLabel;
class QPushButton;
class QShowEvent;
class QTreeWidget;
class QTreeWidgetItem;

QT_BEGIN_NAMESPACE
namespace Ui {
class DownloadManagerDialog;
}
QT_END_NAMESPACE

/// 下载管理器窗口：上半区是正在下载/暂停/失败任务，下半区是已完成历史。
/// 非模态窗口，下载过程中可以继续用播放器；任务状态变化时自动刷新。
/// 静态布局由 DownloadManagerDialog.ui 描述，这里只做数据绑定和运行时状态处理。
class DownloadManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DownloadManagerDialog(DownloadManager *manager, QWidget *parent = nullptr);
    ~DownloadManagerDialog() override;
    /// 下载入队、完成、失败等过程提示在下载管理器内显示。
    void setStatusMessage(const QString &text);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void applyHeaderStyle();
    void rebuild();
    void updateButtons();
    const DownloadJob *selectedJob() const;
    void openSelectedFile();
    void openSelectedFolder();
    void retrySelected();
    void removeSelected();
    void clearCompleted();

    DownloadManager *m_manager = nullptr;
    Ui::DownloadManagerDialog *ui;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_openFile = nullptr;
    QPushButton *m_openFolder = nullptr;
    QPushButton *m_retry = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_clearCompleted = nullptr;
    QString m_selectedId;
};

#endif // DOWNLOADMANAGERDIALOG_H
