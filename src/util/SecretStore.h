#ifndef SECRETSTORE_H
#define SECRETSTORE_H

#include <QString>

/// 源配置里的 Cookie/Authorization 等凭据不能明文落盘。
/// Windows 使用 DPAPI（当前用户范围）加密；其他平台不提供加密时，
/// 写盘阶段会直接丢弃这些字段，而不是退回明文。
namespace SecretStore {

bool isAvailable();
bool isProtectedValue(const QString &value);

/// 把明文凭据转成可写入 JSON 的字符串；失败返回空。
QString protectForStorage(const QString &plainText, const QString &label);

/// 读取配置：加密值自动解密；兼容旧版明文；解密失败返回空。
QString revealStored(const QString &storedValue, const QString &label);

} // namespace SecretStore

#endif // SECRETSTORE_H
