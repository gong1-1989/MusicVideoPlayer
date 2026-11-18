#ifndef SEARCHPROVIDERBASE_H
#define SEARCHPROVIDERBASE_H

#include "media/MediaItem.h"

#include <QObject>
#include <QVector>

/// 所有搜索源的共同契约：三种实现（配置化 / 内置 / 外部解析器）都继承它，
/// 上层只认这一个信号，不关心底层怎么拿到数据。
class SearchProviderBase : public QObject
{
    Q_OBJECT

public:
    explicit SearchProviderBase(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    virtual void search(const QString &keyword, int limit) = 0;

signals:
    /// error 为空表示成功；失败时 items 为空
    void finished(const QString &sourceId, const QVector<MediaItem> &items, int filtered,
                  const QString &error);
};

#endif // SEARCHPROVIDERBASE_H
