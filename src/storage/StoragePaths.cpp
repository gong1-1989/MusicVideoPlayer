#include "storage/StoragePaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

QString StoragePaths::dataDir()
{
    // 测试用独立临时目录，避免碰用户真实的 Data/；不设置时行为与原来完全一致
    const QString override = qEnvironmentVariable("MVP_DATA_DIR").trimmed();
    if (!override.isEmpty())
        return QDir::cleanPath(QFileInfo(override).absoluteFilePath());

    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Data"));
}

QString StoragePaths::dataFile(const QString &fileName)
{
    return QDir(dataDir()).filePath(fileName);
}
