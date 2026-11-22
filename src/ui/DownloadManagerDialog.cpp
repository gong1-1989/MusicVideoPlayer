#include "ui/DownloadManagerDialog.h"
#include "ui_DownloadManagerDialog.h"

#include "download/DownloadManager.h"
#include "download/MediaRemux.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>

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
    , ui(new Ui::DownloadManagerDialog)
{
    ui->setupUi(this);
    m_tree = ui->downloadManagerTree;
    m_tree->setHeaderLabels({tr("名称"), tr("状态"), tr("进度"), tr("大小"),
                             tr("保存位置")});
    m_summary = ui->label_downloadSummary;
    m_statusLabel = ui->label_downloadStatus;
    m_openFile = ui->btn_openFile;
    m_openFolder = ui->btn_openFolder;
    m_retry = ui->btn_retry;
    m_remove = ui->btn_remove;
    m_clearCompleted = ui->btn_clearCompleted;

    // 标题字号仍按当前字体比例动态加粗，避免在 .ui 里写死字号后无法跟随字体缩放。
    QFont titleFont = ui->downloadManagerTitle->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    ui->downloadManagerTitle->setFont(titleFont);

    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowIcon(QApplication::windowIcon());
    ThemeManager::instance().applyWindowChrome(this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] {
                ThemeManager::instance().applyWindowChrome(this);
                applyHeaderStyle();
            });

    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    applyHeaderStyle();

    const QString baseHint =
        MediaRemux::isAvailable()
            ? tr("HLS 分片会合并成一个文件，再用内置 FFmpeg 无损转成 MP4；普通任务退出后会自动续传。")
            : tr("HLS 分片会合并成一个 TS 文件；当前未找到 FFmpeg，因此不转 MP4；普通任务退出后会自动续传。");
    ui->downloadManagerHint->setText(
        baseHint + tr("没有结束标记的快照任务只按当前分片下载，不会自动续传。"));

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
    connect(ui->btn_close, &QPushButton::clicked, this, &QDialog::close);

    if (m_manager != nullptr)
        connect(m_manager, &DownloadManager::jobsChanged,
                this, &DownloadManagerDialog::rebuild);

    rebuild();
}

DownloadManagerDialog::~DownloadManagerDialog()
{
    delete ui;
}

void DownloadManagerDialog::applyHeaderStyle()
{
    if (m_tree == nullptr || m_tree->header() == nullptr)
        return;

    // 表头在部分 Windows/Qt 组合下拿不到带祖先选择器的 QSS 背景，
    // 会出现灰白底 + 白字看不清。这里直接给 QHeaderView 设置完整样式，
    // 同时写 palette 兜底，确保文字和底色始终有足够对比度。
    const bool dark =
        ThemeManager::instance().current().fadeColor.lightness() < 128;
    const QColor background = dark ? QColor(0x1b, 0x21, 0x40)
                                   : QColor(0xe7, 0xec, 0xff);
    const QColor text = dark ? QColor(0xef, 0xf3, 0xff)
                             : QColor(0x22, 0x28, 0x4a);
    const QColor border = dark ? QColor(160, 190, 255, 52)
                               : QColor(80, 110, 200, 38);

    auto *header = m_tree->header();
    header->setStyleSheet(
        QStringLiteral(
            "QHeaderView { background-color: %1; border: none; }"
            "QHeaderView::section {"
            " background-color: %1; color: %2; border: none;"
            " border-right: 1px solid %3; border-bottom: 1px solid %3;"
            " padding: 8px 10px; font-weight: 600; }")
            .arg(background.name(), text.name(), border.name(QColor::HexArgb)));
    QPalette palette = header->palette();
    palette.setColor(QPalette::Button, background);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, text);
    header->setPalette(palette);
    header->setAutoFillBackground(true);
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header->setHighlightSections(false);
}

void DownloadManagerDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // 显示后再套一次原生标题栏配色，和主窗口保持一致。
    ThemeManager::instance().applyWindowChrome(this);
}

void DownloadManagerDialog::setStatusMessage(const QString &text)
{
    if (m_statusLabel == nullptr)
        return;
    m_statusLabel->setText(text);
    m_statusLabel->setToolTip(text);
    m_statusLabel->setVisible(!text.isEmpty());
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

    m_summary->setText(tr("未完成 %1 个 · 已完成 %2 个 · 普通任务退出后自动续传，快照任务不会")
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

    const bool forceSnapshot = job->needsSnapshotConsent;
    const QString id = job->id;
    if (forceSnapshot) {
        const QString text =
            tr("该播放列表没有 #EXT-X-ENDLIST，可能是直播或仍在更新。\n\n"
               "普通重试仍会失败。是否按当前可见分片强制下载？\n"
               "结果只是快照，可能不完整，且不会跨启动续传。");
        if (QMessageBox::question(this, tr("按当前分片强制下载"), text,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }
    }
    m_manager->retryJob(id, forceSnapshot);
    rebuild();
}

void DownloadManagerDialog::removeSelected()
{
    const DownloadJob *job = selectedJob();
    if (job == nullptr || m_manager == nullptr)
        return;

    const QString text = job->status == DownloadJob::Status::Completed
        ? tr("只移除这条下载记录，磁盘上的文件不会删除。确定继续？")
        : tr("该任务尚未完成，移除时已下载的部分和临时文件会一起删除。确定继续？");
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
