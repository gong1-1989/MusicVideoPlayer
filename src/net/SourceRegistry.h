#ifndef SOURCEREGISTRY_H
#define SOURCEREGISTRY_H

#include "net/SourceConfig.h"

#include <QObject>
#include <QVector>

/// 搜索源仓库：优先使用 Data/sources.json，安装目录不可写时退回用户数据目录。
/// 首次运行写入预置源，之后完全由用户增删改。
class SourceRegistry : public QObject
{
    Q_OBJECT

public:
    explicit SourceRegistry(QObject *parent = nullptr);

    bool load();
    bool save();

    QVector<SourceConfig> sources() const { return m_sources; }
    QVector<SourceConfig> enabledSources() const;
    SourceConfig sourceById(const QString &id) const;

    bool upsert(const SourceConfig &config);
    bool remove(const QString &id);
    bool setEnabled(const QString &id, bool enabled);
    /// 整表替换：源管理对话框一次提交所有草稿，避免每个源写一次盘、失败到一半。
    bool replaceAll(const QVector<SourceConfig> &sources);

    int enabledCount() const;

    QString storagePath() const;
    /// 最近一次 load/save 的错误；load 会把损坏文件先备份为 .corrupt-<时间>
    QString lastError() const { return m_lastError; }

signals:
    void changed();

private:
    QVector<SourceConfig> m_sources;
    QString m_lastError;
    /// 本次实际读写的路径：正常是 Data/sources.json，主目录不可写时是用户数据目录。
    QString m_storagePath;
};

#endif // SOURCEREGISTRY_H
