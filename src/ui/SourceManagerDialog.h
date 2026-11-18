#ifndef SOURCEMANAGERDIALOG_H
#define SOURCEMANAGERDIALOG_H

#include "net/SourceConfig.h"

#include <QDialog>
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

/// 搜索源管理：左侧源列表，右侧编辑当前源，底部可试搜、导入导出。
/// 改动先留在草稿里，点“保存”才写回 registry。
class SourceManagerDialog : public QDialog
{
    Q_OBJECT

public:
    SourceManagerDialog(SourceRegistry *registry, HttpClient *http, QWidget *parent = nullptr);

protected:
    void accept() override;

public:
    /// 导出/导入用，便于在多机之间搬配置
    static bool writeSourcesToFile(const QVector<SourceConfig> &sources, const QString &path);
    static bool readSourcesFromFile(QVector<SourceConfig> *sources, const QString &path,
                                    QString *error);

private:
    void buildUi();
    void reloadList(int targetRow);
    void selectRow(int row);
    void commitFormToDraft();
    void loadDraftToForm(const SourceConfig &config);
    SourceConfig readForm() const;
    void updateActions();
    void updateKindUi();

    void onAdd();
    void onDuplicate();
    void onRemove();
    void onTest();
    void onImport();
    void onExport();
    void saveAll();

    SourceRegistry *m_registry;
    HttpClient *m_http;

    QVector<SourceConfig> m_drafts;
    int m_currentRow = -1;
    bool m_updating = false;

    QListWidget *m_list = nullptr;
    QListWidget *m_testList = nullptr;
    QLineEdit *m_testKeyword = nullptr;
    QLineEdit *m_name = nullptr;
    QComboBox *m_type = nullptr;
    QComboBox *m_kind = nullptr;
    QComboBox *m_provider = nullptr;
    QWidget *m_builtinRow = nullptr;
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
    QPlainTextEdit *m_skipIf = nullptr;
    QPushButton *m_btnRemove = nullptr;
    QPushButton *m_btnDuplicate = nullptr;
    QPushButton *m_btnTest = nullptr;
};

#endif // SOURCEMANAGERDIALOG_H
