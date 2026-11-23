/// @file StoragePaths.cpp
/// @brief 应用数据目录的唯一解析入口实现。
#include "storage/StoragePaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

/// @brief 返回应用数据根目录。
/// @return MVP_DATA_DIR 非空时返回其绝对路径；否则返回可执行文件旁的 Data/。
/// @note 测试与便携部署用环境变量隔离，避免读写用户真实数据目录。
QString StoragePaths::dataDir()
{
    // 测试用独立临时目录，避免碰用户真实的 Data/；未设置时使用程序目录下的 Data/。
    const QString override = qEnvironmentVariable("MVP_DATA_DIR").trimmed();
    if (!override.isEmpty())
        return QDir::cleanPath(QFileInfo(override).absoluteFilePath());

    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Data"));
}

/// @brief 返回数据根目录下的具体文件路径。
/// @param fileName 相对文件名。
/// @return 拼接后的绝对路径；不创建目录、不检查文件是否存在。
QString StoragePaths::dataFile(const QString &fileName)
{
    return QDir(dataDir()).filePath(fileName);
}
