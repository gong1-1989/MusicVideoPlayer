/// @file DownloadManagerDialog.cpp
/// @brief 下载管理器窗口的数据绑定、分组展示与任务操作实现。
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

/// @brief 把字节数格式化为便于阅读的 KB/MB/GB。
/// @param bytes 字节数；非正数表示未知。
/// @return "—"、KB、MB 或 GB 文本。
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

/// @brief 生成“已下载 / 总大小”文本；总大小未知时只显示已下载量。
QString sizeText(const DownloadJob &job)
{
    if (job.bytesTotal > 0)
        return QStringLiteral("%1 / %2").arg(humanSize(job.bytesReceived),
                                             humanSize(job.bytesTotal));
    return humanSize(job.bytesReceived);
}

/// @brief 把字节/秒格式化为下载速度文本。
QString speedText(qint64 bytesPerSecond)
{
    if (bytesPerSecond <= 0)
        return QStringLiteral("0 B/s");
    if (bytesPerSecond < 1024)
        return QStringLiteral("%1 B/s").arg(bytesPerSecond);
    const double kb = double(bytesPerSecond) / 1024.0;
    if (kb < 1024.0)
        return QStringLiteral("%1 KB/s").arg(kb, 0, 'f', 1);
    return QStringLiteral("%1 MB/s").arg(kb / 1024.0, 0, 'f', 2);
}

} // namespace

/// @brief 构建下载管理器窗口并连接 DownloadManager 的变更信号。
/// @param manager 下载管理器，窗口不拥有它；为空时只展示空界面。
/// @param parent 父窗口。
DownloadManagerDialog::DownloadManagerDialog(DownloadManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
    , ui(new Ui::DownloadManagerDialog)
{
    ui->setupUi(this);
    m_tree = ui->downloadManagerTree;
    m_tree->setHeaderLabels({tr("名称"), tr("状态"), tr("进度"), tr("速度"),
                             tr("大小"), tr("保存位置")});
    m_summary = ui->label_downloadSummary;
    m_statusLabel = ui->label_downloadStatus;
    m_openFile = ui->btn_openFile;
    m_openFolder = ui->btn_openFolder;
    m_retry = ui->btn_retry;
    m_pause = ui->btn_pause;
    m_remove = ui->btn_downloadRemove;
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
    m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(5, QHeaderView::Stretch);
    applyHeaderStyle();

    const QString baseHint =
        MediaRemux::isAvailable()
            ? tr("音乐和影视都先下载到应用暂存区，完成后发布到下载目录；HLS 分片会合并后用内置 FFmpeg 无损转成 MP4。")
            : tr("音乐和影视都先下载到应用暂存区，完成后发布到下载目录；HLS 分片会合并成一个 TS 文件（当前未找到 FFmpeg，不转 MP4）。");
    ui->downloadManagerHint->setText(
        baseHint
        + tr("下载中可暂停并保留进度；“停止下载”会删除未完成部分。没有结束标记的快照任务不会自动续传。"));

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &DownloadManagerDialog::updateButtons);
    connect(m_openFile, &QPushButton::clicked, this,
            &DownloadManagerDialog::openSelectedFile);
    connect(m_openFolder, &QPushButton::clicked, this,
            &DownloadManagerDialog::openSelectedFolder);
    connect(m_retry, &QPushButton::clicked, this,
            &DownloadManagerDialog::retrySelected);
    connect(m_pause, &QPushButton::clicked, this,
            &DownloadManagerDialog::pauseOrResumeSelected);
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

/// @brief 按当前明暗主题重设树表头样式。
/// @note 部分 Windows/Qt 组合下，带祖先选择器的 QSS 无法稳定作用到
///       QHeaderView 的背景，会出现灰白底配白字；这里同时设置样式表和
///       QPalette，保证任何主题下都有足够对比度。
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

/// @brief 每次显示时重新套用原生标题栏配色。
void DownloadManagerDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // 显示后再套一次原生标题栏配色，和主窗口保持一致。
    ThemeManager::instance().applyWindowChrome(this);
}

/// @brief 在窗口底部显示一条过程提示；空文本会隐藏提示行。
void DownloadManagerDialog::setStatusMessage(const QString &text)
{
    if (m_statusLabel == nullptr)
        return;
    m_statusLabel->setText(text);
    m_statusLabel->setToolTip(text);
    m_statusLabel->setVisible(!text.isEmpty());
}

/// @brief 依据 DownloadManager 的当前任务快照重建整棵树。
/// @note 重建前保存选中任务 id，重建后按 id 恢复选中，避免下载进度刷新
///       导致用户选中的行被清掉。任务按“未完成 / 已完成”分组展示。
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

    qint64 totalSpeed = 0;
    for (const DownloadJob &job : active) {
        if (job.status == DownloadJob::Status::Downloading)
            totalSpeed += job.bytesPerSecond;
    }
    m_summary->setText(tr("未完成 %1 个 · 已完成 %2 个 · 总速度 %3 · 普通任务退出后自动续传，快照任务不会")
                            .arg(active.size())
                            .arg(completed.size())
                            .arg(speedText(totalSpeed)));

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
        const QString speed = job.status == DownloadJob::Status::Downloading
            ? speedText(job.bytesPerSecond)
            : QStringLiteral("—");
        row->setText(3, speed);
        row->setToolTip(3, speed);
        row->setText(4, sizeText(job));
        const QString path = QFileInfo(job.filePath).absoluteFilePath();
        row->setText(5, path);
        row->setToolTip(5, path);

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

/// @brief 按当前选中任务的状态启用/禁用操作按钮。
/// @note 打开文件要求任务已完成且最终文件仍存在；重试只对失败任务开放；
///       暂停/继续按钮在“正在下载”和“已暂停”之间切换；未完成任务的
///       “停止下载”会同时删除半成品；“清空已完成”只在有完成记录时可用。
void DownloadManagerDialog::updateButtons()
{
    const DownloadJob *job = selectedJob();
    const bool hasJob = job != nullptr;
    const bool completed = hasJob && job->status == DownloadJob::Status::Completed;
    const bool downloading = hasJob && job->status == DownloadJob::Status::Downloading;
    const bool paused = hasJob && job->status == DownloadJob::Status::Paused;
    const bool canRetry = hasJob && job->status == DownloadJob::Status::Failed;

    m_openFile->setEnabled(completed && QFileInfo::exists(job->filePath));
    m_openFolder->setEnabled(hasJob && !job->filePath.isEmpty());
    m_retry->setEnabled(canRetry);
    m_pause->setEnabled(downloading || paused);
    // 没有选中任务时默认展示“暂停下载”，否则用户会误以为暂停入口不存在；
    // 选中已暂停任务后才切换成“继续下载”。
    m_pause->setText(paused ? tr("继续下载") : tr("暂停下载"));
    m_remove->setEnabled(hasJob);
    m_remove->setText(completed || !hasJob ? tr("移除记录") : tr("停止下载"));

    bool hasCompleted = false;
    for (const DownloadJob &entry : m_manager->jobs()) {
        if (entry.status == DownloadJob::Status::Completed) {
            hasCompleted = true;
            break;
        }
    }
    m_clearCompleted->setEnabled(hasCompleted);
}

/// @brief 取当前选中行对应的任务。
/// @return 任务指针指向 DownloadManager 内部快照，仅在本次调用期间有效；
///         选中分组行或记录已消失时返回 nullptr。
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

/// @brief 用系统默认播放器打开选中的已完成文件。
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

/// @brief 在资源管理器中打开选中文件所在目录。
void DownloadManagerDialog::openSelectedFolder()
{
    const DownloadJob *job = selectedJob();
    if (job == nullptr || job->filePath.isEmpty())
        return;
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(job->filePath).absolutePath()));
}

/// @brief 重试选中的失败/暂停任务。
/// @note 快照型 HLS 任务（无 ENDLIST）必须先征得用户同意：结果可能不完整，
///       且不跨启动续传；用户拒绝时不做任何操作。
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

/// @brief 在“正在下载”和“已暂停”之间切换，保留已下载字节。
void DownloadManagerDialog::pauseOrResumeSelected()
{
    const DownloadJob *job = selectedJob();
    if (job == nullptr || m_manager == nullptr)
        return;

    const QString id = job->id;
    const DownloadJob::Status status = job->status;
    if (status == DownloadJob::Status::Downloading)
        m_manager->pauseJob(id);
    else if (status == DownloadJob::Status::Paused)
        m_manager->resumeJob(id);
    rebuild();
}

/// @brief 移除选中的下载记录。
/// @note 已完成任务只移除记录、不删磁盘文件；未完成任务会同时删除已下载
///       部分和 .mvpdownload 临时文件，因此必须二次确认。
void DownloadManagerDialog::removeSelected()
{
    const DownloadJob *job = selectedJob();
    if (job == nullptr || m_manager == nullptr)
        return;

    const QString id = job->id;
    const bool completed = job->status == DownloadJob::Status::Completed;
    const QString text = completed
        ? tr("只移除这条下载记录，磁盘上的文件不会删除。确定继续？")
        : tr("停止下载会删除尚未完成的部分和暂存文件，且不能继续。确定停止？");
    if (QMessageBox::question(this,
                              completed ? tr("移除下载记录") : tr("停止下载"),
                              text)
        != QMessageBox::Yes) {
        return;
    }

    m_selectedId.clear();
    // 确认框会跑嵌套事件循环，期间 DownloadManager 可能刷新任务列表，
    // 上面保存的 job 指针已经失效，后续只能使用提前复制的 id。
    m_manager->removeJob(id);
    rebuild();
}

/// @brief 清空全部已完成记录，不删除磁盘文件。
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
