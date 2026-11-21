#include "ui/DownloadManagerDialog.h"

#include "download/DownloadManager.h"
#include "download/MediaRemux.h"

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QString humanSize(qint64 bytes)
{
    if (bytes <= 0)
        return QStringLiteral("—");
    const double mb = double(bytes) / (1024.0 * 1024.0);
    if (mb < 1.0)
        return QStringLiteral("%1 KB").arg(qMax<qint64>(1, bytes / 1024));
    if (mb < 1024.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
}

QString sizeText(const DownloadJob &job)
{
    if (job.bytesTotal > 0)
        return QStringLiteral("%1 / %2").arg(humanSize(job.bytesReceived),
                                             humanSize(job.bytesTotal));
    return humanSize(job.bytesReceived);
}

} // namespace

DownloadManagerDialog::DownloadManagerDialog(DownloadManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setObjectName(QStringLiteral("DownloadManagerDialog"));
    setWindowTitle(tr("下载管理"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(920, 560);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(12);

    auto *title = new QLabel(tr("下载任务"), this);
    title->setObjectName(QStringLiteral("downloadManagerTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("downloadManagerTree"));
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("名称"), tr("状态"), tr("进度"), tr("大小"),
                             tr("保存位置")});
    m_tree->setRootIsDecorated(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(false);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_tree->setMinimumHeight(260);
    root->addWidget(m_tree, 1);

    const QString hint =
        MediaRemux::isAvailable()
            ? tr("HLS 分片会合并成一个文件，再用内置 FFmpeg 无损转成 MP4；退出程序时未完成任务会保留，下次启动自动续传。")
            : tr("HLS 分片会合并成一个 TS 文件；当前未找到 FFmpeg，因此不转 MP4。退出程序时未完成任务会保留，下次启动自动续传。");
    auto *hintLabel = new QLabel(hint, this);
    hintLabel->setWordWrap(true);
    hintLabel->setObjectName(QStringLiteral("downloadManagerHint"));
    root->addWidget(hintLabel);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_openFile = new QPushButton(tr("打开文件"), this);
    m_openFolder = new QPushButton(tr("打开目录"), this);
    m_retry = new QPushButton(tr("重试 / 续传"), this);
    m_remove = new QPushButton(tr("移除记录"), this);
    m_clearCompleted = new QPushButton(tr("清空已完成"), this);
    auto *closeButton = new QPushButton(tr("关闭"), this);
    closeButton->setDefault(true);
    buttons->addWidget(m_openFile);
    buttons->addWidget(m_openFolder);
    buttons->addWidget(m_retry);
    buttons->addWidget(m_remove);
    buttons->addStretch(1);
    buttons->addWidget(m_clearCompleted);
    buttons->addWidget(closeButton);
    root->addLayout(buttons);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &DownloadManagerDialog::updateButtons);
    connect(m_openFile, &QPushButton::clicked, this,
            &DownloadManagerDialog::openSelectedFile);
    connect(m_openFolder, &QPushButton::clicked, this,
            &DownloadManagerDialog::openSelectedFolder);
    connect(m_retry, &QPushButton::clicked, this,
            &DownloadManagerDialog::retrySelected);
    connect(m_remove, &QPushButton::clicked, this,
            &DownloadManagerDialog::removeSelected);
    connect(m_clearCompleted, &QPushButton::clicked, this,
            &DownloadManagerDialog::clearCompleted);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    if (m_manager != nullptr)
        connect(m_manager, &DownloadManager::jobsChanged,
                this, &DownloadManagerDialog::rebuild);

    rebuild();
}

void DownloadManagerDialog::rebuild()
{
    if (m_manager == nullptr)
        return;

    const QTreeWidgetItem *selected = m_tree->currentItem();
    if (selected != nullptr && !selected->data(0, Qt::UserRole).toString().isEmpty())
        m_selectedId = selected->data(0, Qt::UserRole).toString();

    m_tree->clear();

    QVector<DownloadJob> active;
    QVector<DownloadJob> completed;
    for (const DownloadJob &job : m_manager->jobs()) {
        if (job.status == DownloadJob::Status::Completed)
            completed.append(job);
        else
            active.append(job);
    }

    m_summary->setText(tr("未完成 %1 个 · 已完成 %2 个 · 退出程序后未完成任务会自动续传")
                            .arg(active.size())
                            .arg(completed.size()));

    auto makeGroup = [this](const QString &text) {
        auto *group = new QTreeWidgetItem(m_tree);
        group->setFirstColumnSpanned(true);
        group->setFlags(Qt::ItemIsEnabled);
        QFont font = group->font(0);
        font.setBold(true);
        group->setFont(0, font);
        group->setText(0, text);
        group->setExpanded(true);
        return group;
    };

    auto addJob = [this](QTreeWidgetItem *group, const DownloadJob &job) {
        auto *row = new QTreeWidgetItem(group);
        row->setData(0, Qt::UserRole, job.id);
        row->setText(0, job.item.title);
        row->setToolTip(0, job.item.title);
        row->setText(1, job.statusText());
        row->setToolTip(1, job.statusText());
        row->setText(3, sizeText(job));
        row->setText(4, QFileInfo(job.filePath).absoluteFilePath());
        row->setToolTip(4, QFileInfo(job.filePath).absoluteFilePath());

        auto *bar = new QProgressBar(m_tree);
        bar->setRange(0, 100);
        bar->setValue(job.percent());
        bar->setTextVisible(true);
        bar->setFormat(QStringLiteral("%1%").arg(job.percent()));
        bar->setMinimumWidth(150);
        m_tree->setItemWidget(row, 2, bar);

        if (job.id == m_selectedId)
            m_tree->setCurrentItem(row);
    };

    if (active.isEmpty()) {
        auto *group = makeGroup(tr("正在下载 / 未完成 (0)"));
        auto *empty = new QTreeWidgetItem(group);
        empty->setText(0, tr("暂无未完成的下载"));
        empty->setFlags(Qt::ItemIsEnabled);
        empty->setFirstColumnSpanned(true);
    } else {
        auto *group = makeGroup(tr("正在下载 / 未完成 (%1)").arg(active.size()));
        for (const DownloadJob &job : active)
            addJob(group, job);
    }

    if (!completed.isEmpty()) {
        auto *group = makeGroup(tr("下载完成 (%1)").arg(completed.size()));
        for (const DownloadJob &job : completed)
            addJob(group, job);
    }

    updateButtons();
}

void DownloadManagerDialog::updateButtons()
{
    const DownloadJob *job = selectedJob();
    const bool hasJob = job != nullptr;
    const bool completed = hasJob && job->status == DownloadJob::Status::Completed;
    const bool canRetry = hasJob
                          && (job->status == DownloadJob::Status::Failed
                              || job->status == DownloadJob::Status::Paused);

    m_openFile->setEnabled(completed && QFileInfo::exists(job->filePath));
    m_openFolder->setEnabled(hasJob && !job->filePath.isEmpty());
    m_retry->setEnabled(canRetry);
    m_remove->setEnabled(hasJob);

    bool hasCompleted = false;
    for (const DownloadJob &entry : m_manager->jobs()) {
        if (entry.status == DownloadJob::Status::Completed) {
            hasCompleted = true;
            break;
        }
    }
    m_clearCompleted->setEnabled(hasCompleted);
}

const DownloadJob *DownloadManagerDialog::selectedJob() const
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    if (item == nullptr || m_manager == nullptr)
        return nullptr;
    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty())
        return nullptr;
    for (const DownloadJob &job : m_manager->jobs()) {
        if (job.id == id)
            return &job;
    }
    return nullptr;
}

void DownloadManagerDialog::openSelectedFile()
{
    const DownloadJob *job = selectedJob();
    if (job == nullptr)
        return;
    if (!QFileInfo::exists(job->filePath)) {
        QMessageBox::warning(this, tr("文件不存在"),
                             tr("下载文件已被移动或删除：\n%1").arg(job->filePath));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(job->filePath));
}

void DownloadManagerDialog::openSelectedFolder()
{
    const DownloadJob *job = selectedJob();
    if (job == nullptr || job->filePath.isEmpty())
        return;
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(job->filePath).absolutePath()));
}

void DownloadManagerDialog::retrySelected()
{
    const DownloadJob *job = selectedJob();
    if (job == nullptr || m_manager == nullptr)
        return;
    m_manager->retryJob(job->id);
    rebuild();
}

void DownloadManagerDialog::removeSelected()
{
    const DownloadJob *job = selectedJob();
    if (job == nullptr || m_manager == nullptr)
        return;

    const QString text = job->status == DownloadJob::Status::Completed
        ? tr("只移除这条下载记录，磁盘上的文件不会删除。确定继续？")
        : tr("该任务尚未完成，移除时会删除它的临时文件。确定继续？");
    if (QMessageBox::question(this, tr("移除下载记录"), text)
        != QMessageBox::Yes) {
        return;
    }

    m_selectedId.clear();
    m_manager->removeJob(job->id);
    rebuild();
}

void DownloadManagerDialog::clearCompleted()
{
    if (m_manager == nullptr)
        return;
    if (QMessageBox::question(
            this, tr("清空已完成"),
            tr("只清空下载记录，已经下载的文件不会删除。确定继续？"))
        != QMessageBox::Yes) {
        return;
    }
    m_manager->clearCompleted();
    rebuild();
}
