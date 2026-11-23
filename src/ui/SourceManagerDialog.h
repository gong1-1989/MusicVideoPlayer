#ifndef SOURCEMANAGERDIALOG_H
#define SOURCEMANAGERDIALOG_H

#include "net/SourceConfig.h"

#include <QDialog>
#include <QHash>
#include <QVector>

class HttpClient;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class SourceRegistry;

QT_BEGIN_NAMESPACE
namespace Ui {
class SourceManagerDialog;
}
QT_END_NAMESPACE

/// 搜索源管理：左侧源列表，右侧编辑当前源，底部可试搜、导入导出。
/// 改动先留在草稿里，点“保存”才写回 registry；延迟提交模式下只把草稿交给调用方。
class SourceManagerDialog : public QDialog
{
    Q_OBJECT

public:
    SourceManagerDialog(SourceRegistry *registry, HttpClient *http, QWidget *parent = nullptr,
                        bool deferCommit = false);
    ~SourceManagerDialog() override;

    /// 延迟提交模式：accept() 只把最终草稿交给调用方，不写 sources.json
    QVector<SourceConfig> committedSources() const { return m_committedSources; }
    /// 延迟提交模式下重新打开时，用上一次未提交的草稿继续编辑
    void setDraftSources(const QVector<SourceConfig> &sources);

protected:
    void accept() override;

public:
    /// 导出/导入用，便于在多机之间搬配置
    static bool writeSourcesToFile(const QVector<SourceConfig> &sources, const QString &path);
    static bool readSourcesFromFile(QVector<SourceConfig> *sources, const QString &path,
                                    QString *error);

private:
    void bindUi();
    /// 给表单控件补鼠标悬停说明：字段含义、可填格式和留空后的行为
    void configureHoverHelp();
    void reloadList(int targetRow);
    void selectRow(int row);
    void commitFormToDraft();
    void loadDraftToForm(const SourceConfig &config);
    SourceConfig readForm();
    void updateActions();
    void updateKindUi();

    void onAdd();
    void onAddExample();
    void onAddFromSite();
    void onRefreshSelected();
    /// 预置配置源（如网易云音乐）字段由程序维护，更新改为按当前配置试搜验证
    void verifyProtectedSource(const SourceConfig &draft);
    /// 内置网页型影视源的“一键更新”：探测地址跳转 + 试搜验真，不写死字段映射
    void refreshWebSource(const SourceConfig &draft, const QString &previousUrl);
    void onDuplicate();
    void onRemove();
    void onTest();
    void onImport();
    void onExport();
    bool saveAll();

    Ui::SourceManagerDialog *ui;
    SourceRegistry *m_registry;
    HttpClient *m_http;

    QVector<SourceConfig> m_drafts;
    QVector<SourceConfig> m_committedSources;
    bool m_deferCommit = false;
    int m_currentRow = -1;
    bool m_updating = false;
    bool m_probeBusy = false;
    /// 试搜请求在途标记：按钮禁用之外还要防回车/重复点击造成多个 provider 抢写列表。
    bool m_testBusy = false;
    /// 站点体检失败的原因（按源 id 存），列表里据此标红并做悬停提示；
    /// 只反映本次运行期的检查结果，不写进 sources.json
    QHash<QString, QString> m_healthIssues;

    QListWidget *m_list = nullptr;
    QListWidget *m_testList = nullptr;
    QLineEdit *m_testKeyword = nullptr;
    QLineEdit *m_name = nullptr;
    QComboBox *m_type = nullptr;
    QComboBox *m_kind = nullptr;
    QComboBox *m_provider = nullptr;
    QWidget *m_builtinRow = nullptr;
    QWidget *m_siteUrlRow = nullptr;
    QLineEdit *m_siteUrl = nullptr;
    QGroupBox *m_requestGroup = nullptr;
    QGroupBox *m_fieldsGroup = nullptr;
    QGroupBox *m_filterGroup = nullptr;
    QGroupBox *m_externalGroup = nullptr;
    QLineEdit *m_command = nullptr;
    QLineEdit *m_arguments = nullptr;
    QSpinBox *m_timeout = nullptr;
    QComboBox *m_method = nullptr;
    QLineEdit *m_url = nullptr;
    QPlainTextEdit *m_body = nullptr;
    QPlainTextEdit *m_headers = nullptr;
    QLineEdit *m_cookie = nullptr;
    QLineEdit *m_itemsPath = nullptr;
    QLineEdit *m_idField = nullptr;
    QLineEdit *m_titleField = nullptr;
    QLineEdit *m_artistField = nullptr;
    QLineEdit *m_durationField = nullptr;
    QLineEdit *m_streamField = nullptr;
    QLineEdit *m_lyricField = nullptr;
    QLineEdit *m_webField = nullptr;
    QLineEdit *m_posterField = nullptr;
    QLineEdit *m_yearField = nullptr;
    QLineEdit *m_areaField = nullptr;
    QLineEdit *m_genreField = nullptr;
    QLineEdit *m_descriptionField = nullptr;
    QComboBox *m_streamKind = nullptr;
    QPlainTextEdit *m_skipIf = nullptr;
    // 二次解析：搜索无直链时再请求详情换直链
    QGroupBox *m_detailGroup = nullptr;
    QLineEdit *m_detailUrl = nullptr;
    QLineEdit *m_detailItemsPath = nullptr;
    QLineEdit *m_detailStreamField = nullptr;
    QPushButton *m_btnRemove = nullptr;
    QPushButton *m_btnDuplicate = nullptr;
    QPushButton *m_btnTest = nullptr;
};

#endif // SOURCEMANAGERDIALOG_H
