#ifndef SOURCEREGISTRY_H
#define SOURCEREGISTRY_H

#include "net/SourceConfig.h"

#include <QObject>
#include <QVector>

/// 搜索源仓库：Data/sources.json 是唯一事实来源。
/// 首次运行写入预置源（网易云 + 一个 iTunes 示例），之后完全由用户增删改。
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

    int enabledCount() const;

    QString storagePath() const;

signals:
    void changed();

private:
    QVector<SourceConfig> m_sources;
};

#endif // SOURCEREGISTRY_H
