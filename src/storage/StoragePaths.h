#ifndef STORAGEPATHS_H
#define STORAGEPATHS_H

#include <QString>

/// 应用数据目录的唯一入口。
/// 默认放在程序目录下的 Data/；测试或便携部署可用 MVP_DATA_DIR 覆盖。
namespace StoragePaths {

QString dataDir();
QString dataFile(const QString &fileName);

} // namespace StoragePaths

#endif // STORAGEPATHS_H
