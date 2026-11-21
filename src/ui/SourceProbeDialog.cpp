#include "ui/SourceProbeDialog.h"

#include "net/HttpClient.h"
#include "settings/AppSettings.h"
#include "ui/ThemeManager.h"
#include "ui_SourceProbeDialog.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QGroupBox>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>

namespace {

QString fieldLabelFor(const QString &key)
{
    if (key == QLatin1String("id"))
        return QStringLiteral("id");
    if (key == QLatin1String("title"))
        return QObject::tr("标题");
    if (key == QLatin1String("artist"))
        return QObject::tr("作者");
    if (key == QLatin1String("streamUrl"))
        return QObject::tr("播放地址");
    if (key == QLatin1String("lyricUrl"))
        return QObject::tr("歌词地址");
    if (key == QLatin1String("webUrl"))
        return QObject::tr("网页地址");
    if (key == QLatin1String("posterUrl"))
        return QObject::tr("海报");
    if (key == QLatin1String("year"))
        return QObject::tr("年份");
    if (key == QLatin1String("area"))
        return QObject::tr("地区");
    if (key == QLatin1String("genre"))
        return QObject::tr("类型");
    if (key == QLatin1String("description"))
        return QObject::tr("简介");
    if (key == QLatin1String("durationMs"))
        return QObject::tr("时长");
    return key;
}

const QStringList kFieldOrder = {
    QStringLiteral("title"),      QStringLiteral("artist"),
    QStringLiteral("streamUrl"),  QStringLiteral("posterUrl"),
    QStringLiteral("year"),       QStringLiteral("area"),
    QStringLiteral("genre"),      QStringLiteral("description"),
    QStringLiteral("webUrl"),     QStringLiteral("lyricUrl"),
    QStringLiteral("durationMs"), QStringLiteral("id")};

} // namespace

SourceProbeDialog::SourceProbeDialog(HttpClient *http, MediaType type, const QString &initialUrl,
                                     const QString &titleText, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SourceProbeDialog)
    , m_http(http)
{
    ui->setupUi(this);
    setModal(true);
    setWindowTitle(titleText);
    const QRect available = screen() != nullptr ? screen()->availableGeometry()
                                                : QRect(0, 0, 1280, 800);
    resize(qMin(900, available.width() - 40), qMin(600, available.height() - 60));

    // 必须在 bindUi() 之后才能碰成员指针：m_type 等在这里才被赋值
    bindUi();

    m_type->addItem(tr("视频"), QStringLiteral("video"));
    m_type->addItem(tr("音乐"), QStringLiteral("music"));

    m_type->setCurrentIndex(type == MediaType::Video ? 0 : 1);
    m_url->setText(initialUrl);

    ThemeManager::instance().applyWindowChrome(this);

    if (!initialUrl.trimmed().isEmpty())
        startProbe();
}

SourceProbeDialog::~SourceProbeDialog()
{
    delete ui;
}

void SourceProbeDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);

    // 窄屏或大字体下窗口按父窗口位置居中，732px 高的探测窗会被放到屏幕顶部
    // 之外，标题栏和“探测类型”不可见。显示后按当前屏幕可用区域重新居中，
    // 并把左上角夹在区域内，保证标题栏和底部按钮始终可达。
    QTimer::singleShot(0, this, [this] {
        const QRect available = screen() != nullptr ? screen()->availableGeometry()
                                                    : QRect(0, 0, 1280, 800);
        const QRect frame = frameGeometry();
        int x = available.center().x() - frame.width() / 2;
        int y = available.center().y() - frame.height() / 2;
        x = qBound(available.left(), x,
                   qMax(available.left(), available.right() - frame.width() + 1));
        y = qBound(available.top(), y,
                   qMax(available.top(), available.bottom() - frame.height() + 1));
        move(x, y);
    });
}

void SourceProbeDialog::bindUi()
{
    m_url = ui->edit_url;
    m_type = ui->combo_type;
    m_btnProbe = ui->btn_probe;
    m_status = ui->label_status;

    m_resultGroup = ui->group_result;
    m_nameLabel = ui->label_name;
    m_interfaceLabel = ui->label_interface;
    m_pathLabel = ui->label_path;
    m_kindLabel = ui->label_kind;
    m_fieldsLabel = ui->label_fields;
    m_samples = ui->list_samples;

    m_attempts = ui->text_attempts;
    m_error = ui->text_error;

    m_paths = ui->list_paths;
    m_customPath = ui->edit_customPath;

    m_btnSave = ui->btn_save;

    connect(m_btnProbe, &QPushButton::clicked, this, &SourceProbeDialog::startProbe);
    connect(m_btnSave, &QPushButton::clicked, this, &SourceProbeDialog::accept);
    connect(ui->btn_cancel, &QPushButton::clicked, this, &SourceProbeDialog::reject);
    connect(m_url, &QLineEdit::returnPressed, this, &SourceProbeDialog::startProbe);
    connect(ui->btn_addPath, &QPushButton::clicked, this, &SourceProbeDialog::addCustomPath);
    connect(ui->btn_removePath, &QPushButton::clicked, this,
            &SourceProbeDialog::removeCustomPath);
    connect(m_customPath, &QLineEdit::returnPressed, this,
            &SourceProbeDialog::addCustomPath);
    connect(m_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { reloadPathList(); });

    reloadPathList();
}

void SourceProbeDialog::startProbe()
{
    if (m_busy)
        return;

    const QString text = m_url->text().trimmed();
    if (text.isEmpty()) {
        m_status->setText(tr("先填一个网站地址。"));
        return;
    }

    QUrl url(text);
    if (url.scheme().isEmpty())
        url = QUrl(QStringLiteral("https://") + text);
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
        m_status->setText(tr("网址必须以 http:// 或 https:// 开头。"));
        return;
    }

    m_busy = true;
    m_result = SourceProbe::Result();
    m_samples->clear();
    m_attempts->clear();
    m_error->clear();
    m_error->setVisible(false);
    m_nameLabel->clear();
    m_interfaceLabel->clear();
    m_pathLabel->clear();
    m_kindLabel->clear();
    m_fieldsLabel->clear();
    m_btnSave->setEnabled(false);
    m_btnProbe->setEnabled(false);
    m_status->setText(tr("正在探测，会依次试几条常见接口路径…"));

    if (m_prober != nullptr)
        m_prober->deleteLater();
    m_prober = new SourceProbe::Prober(m_http, this);
    connect(m_prober, &SourceProbe::Prober::finished, this,
            &SourceProbeDialog::onProbeFinished);
    // 不管选没选类型都开着自动识别：类型选错时按返回数据纠正
    m_prober->start(url, currentType(), customPaths(), true);
}

MediaType SourceProbeDialog::currentType() const
{
    return m_type->currentIndex() == 1 ? MediaType::Music : MediaType::Video;
}

QStringList SourceProbeDialog::customPaths() const
{
    return AppSettings::probePaths(currentType());
}

void SourceProbeDialog::reloadPathList()
{
    if (m_paths == nullptr)
        return;

    m_paths->clear();

    auto addBuiltin = [&](MediaType type) {
        for (const QString &path : SourceProbe::defaultPaths(type)) {
            auto *item = new QListWidgetItem(tr("内置 · %1").arg(path), m_paths);
            item->setData(Qt::UserRole, false);
            item->setToolTip(tr("内置路径，不能修改或删除\n%1").arg(path));
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        }
    };
    auto addCustom = [&](MediaType type) {
        for (const QString &path : AppSettings::probePaths(type)) {
            auto *item = new QListWidgetItem(tr("自定义 · %1").arg(path), m_paths);
            item->setData(Qt::UserRole, true);
            item->setData(Qt::UserRole + 1,
                          type == MediaType::Video ? QStringLiteral("video")
                                                   : QStringLiteral("music"));
            item->setData(Qt::UserRole + 2, path);
            item->setToolTip(path);
        }
    };

    addBuiltin(currentType());
    addCustom(currentType());

    // 清掉默认选中，免得只读的内置行看着像被选中
    m_paths->setCurrentRow(-1);
    m_paths->clearSelection();
}

void SourceProbeDialog::addCustomPath()
{
    const QString raw = m_customPath->text().trimmed();
    if (raw.isEmpty()) {
        m_status->setText(tr("先填一个接口路径再点“添加”。"));
        return;
    }

    QString path = raw;
    if (!path.contains(QLatin1String("{keyword}"), Qt::CaseInsensitive)) {
        // 没写占位符就补一个常见的搜索参数，免得试出来是静态列表
        path += path.contains(QLatin1Char('?')) ? QStringLiteral("&wd={keyword}")
                                                : QStringLiteral("?wd={keyword}");
    }

    QStringList paths = customPaths();
    if (paths.contains(path)) {
        m_status->setText(tr("这个路径已经在列表里了。"));
        return;
    }
    paths.append(path);
    AppSettings::setProbePaths(currentType(), paths);
    m_customPath->clear();
    reloadPathList();
}

void SourceProbeDialog::removeCustomPath()
{
    QListWidgetItem *item = m_paths->currentItem();
    if (item == nullptr) {
        m_status->setText(tr("先在列表里选一条自定义路径。"));
        return;
    }
    if (!item->data(Qt::UserRole).toBool()) {
        m_status->setText(tr("内置路径不能删除。"));
        return;
    }

    const QString typeTag = item->data(Qt::UserRole + 1).toString();
    const MediaType ownerType = typeTag == QLatin1String("music") ? MediaType::Music
                                                                  : MediaType::Video;
    const QString target = item->data(Qt::UserRole + 2).toString();

    QStringList paths = AppSettings::probePaths(ownerType);
    paths.removeAll(target);
    AppSettings::setProbePaths(ownerType, paths);
    reloadPathList();
}

void SourceProbeDialog::onProbeFinished(const SourceProbe::Result &result)
{
    m_busy = false;
    m_btnProbe->setEnabled(true);
    m_result = result;

    QStringList attemptLines;
    for (const SourceProbe::Attempt &attempt : result.attempts) {
        attemptLines.append(QStringLiteral("[%1] %2 — %3")
                                .arg(attempt.status)
                                .arg(attempt.note, attempt.url));
    }
    m_attempts->setPlainText(attemptLines.join(QLatin1Char('\n')));

    if (!result.ok) {
        m_status->setText(tr("探测失败：没有拿到可用的接口，不能保存。"));
        m_error->setVisible(true);
        m_error->setPlainText(result.error);
        m_btnSave->setEnabled(false);
        return;
    }

    // 探测结果说了算：用户选错了也按识别结果切换下拉
    const int detectedIndex = result.mediaType == MediaType::Video ? 0 : 1;
    const bool switched = m_type->currentIndex() != detectedIndex;
    if (switched)
        m_type->setCurrentIndex(detectedIndex);

    QString status = tr("探测成功，确认无误后点“保存这个源”。");
    if (switched) {
        status += tr("已自动识别为%1，类型已切换。")
                      .arg(result.mediaType == MediaType::Video ? tr("视频") : tr("音乐"));
    } else if (result.autoDetected) {
        status += tr("已识别为%1，与当前选择一致。")
                      .arg(result.mediaType == MediaType::Video ? tr("视频") : tr("音乐"));
    }
    m_status->setText(status);
    m_nameLabel->setText(result.config.name);
    m_interfaceLabel->setText(result.config.urlTemplate);

    QString path = result.config.itemsPath;
    if (path.isEmpty())
        path = tr("（根就是数组）");
    m_pathLabel->setText(path);
    m_kindLabel->setText(result.config.streamKind == QLatin1String("episodes")
                             ? tr("剧集列表（会按集拆成多条）")
                             : tr("单条直链"));

    QStringList fieldLines;
    for (const QString &key : kFieldOrder) {
        const QString value = result.config.fields.value(key);
        if (value.trimmed().isEmpty())
            continue;
        fieldLines.append(QStringLiteral("%1 → %2").arg(fieldLabelFor(key), value));
    }
    m_fieldsLabel->setText(fieldLines.join(QStringLiteral("    ")));

    m_samples->clear();
    for (const QString &title : result.sampleTitles)
        m_samples->addItem(title);
    m_samples->addItem(tr("…共 %1 条").arg(result.sampleCount));

    m_btnSave->setEnabled(true);
}

void SourceProbeDialog::updateSaveState()
{
    m_btnSave->setEnabled(m_result.ok);
}

void SourceProbeDialog::closeEvent(QCloseEvent *event)
{
    if (m_prober != nullptr)
        m_prober->cancel();
    QDialog::closeEvent(event);
}
