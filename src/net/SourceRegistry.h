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
    /// 从源配置恢复内存态凭据和源站地址。播放列表落盘时会剥掉 Cookie，
    /// 重启后必须按 sourceId 重新注入，否则需要登录的源会 403。
    MediaItem mediaItemWithCredentials(const MediaItem &item) const;

    bool upsert(const SourceConfig &config);
    bool remove(const QString &id);
    bool setEnabled(const QString &id, bool enabled);
    /// 整表替换：源管理对话框一次提交所有草稿，避免每个源写一次盘、失败到一半。
    bool replaceAll(const QVector<SourceConfig> &sources);

    QString storagePath() const;
    /// 最近一次 load/save 的错误；load 会把损坏文件先备份为 .corrupt-<时间>
    QString lastError() const { return m_lastError; }

signals:
    void changed();

private:
    QVector<SourceConfig> m_sources;
    QString m_lastError;
    /// 实际读写的路径；当前固定为程序目录下的 Data/sources.json。
    QString m_storagePath;
};

#endif // SOURCEREGISTRY_H
