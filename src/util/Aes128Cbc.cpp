#include "util/Aes128Cbc.h"

#include <QObject>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace {

bool isBlockAligned(const QByteArray &data)
{
    return !data.isEmpty() && data.size() % 16 == 0;
}

#ifdef Q_OS_WIN
QString bcryptError(const QString &stage, NTSTATUS status)
{
    return QObject::tr("%1失败（系统错误 0x%2）")
        .arg(stage)
        .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
}

struct BCryptHandles
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;

    ~BCryptHandles()
    {
        if (key != nullptr)
            BCryptDestroyKey(key);
        if (algorithm != nullptr)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};
#endif

} // namespace

bool Aes128Cbc::decrypt(const QByteArray &ciphertext, const QByteArray &key,
                        const QByteArray &iv, QByteArray *plaintext, QString *error)
{
    if (plaintext == nullptr) {
        if (error != nullptr)
            *error = QObject::tr("AES 解密输出参数为空");
        return false;
    }
    plaintext->clear();

    if (key.size() != 16) {
        if (error != nullptr)
            *error = QObject::tr("AES-128 密钥必须是 16 字节");
        return false;
    }
    if (iv.size() != 16) {
        if (error != nullptr)
            *error = QObject::tr("AES-128 初始向量必须是 16 字节");
        return false;
    }
    if (!isBlockAligned(ciphertext)) {
        if (error != nullptr)
            *error = QObject::tr("AES 密文长度不是 16 字节的整数倍");
        return false;
    }

#ifdef Q_OS_WIN
    BCryptHandles handles;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&handles.algorithm,
                                                  BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (status < 0) {
        if (error != nullptr)
            *error = bcryptError(QObject::tr("打开 AES 算法"), status);
        return false;
    }

    status = BCryptSetProperty(handles.algorithm, BCRYPT_CHAINING_MODE,
                               reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(
                                   BCRYPT_CHAIN_MODE_CBC)),
                               static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)), 0);
    if (status < 0) {
        if (error != nullptr)
            *error = bcryptError(QObject::tr("设置 CBC 模式"), status);
        return false;
    }

    DWORD objectLength = 0;
    DWORD resultLength = 0;
    status = BCryptGetProperty(handles.algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&objectLength),
                               sizeof(objectLength), &resultLength, 0);
    if (status < 0 || objectLength == 0) {
        if (error != nullptr)
            *error = bcryptError(QObject::tr("读取 AES 对象长度"), status);
        return false;
    }

    QByteArray keyObject(static_cast<int>(objectLength), '\0');
    status = BCryptGenerateSymmetricKey(
        handles.algorithm, &handles.key, reinterpret_cast<PUCHAR>(keyObject.data()),
        objectLength, reinterpret_cast<PUCHAR>(const_cast<char *>(key.constData())),
        static_cast<ULONG>(key.size()), 0);
    if (status < 0) {
        keyObject.fill('\0');
        if (error != nullptr)
            *error = bcryptError(QObject::tr("导入 AES 密钥"), status);
        return false;
    }

    // CNG 的密钥句柄内部引用 pbKeyObject 缓冲区，必须保持其内容直到
    // 解密完成；只有在销毁密钥后才能清零，否则 BCryptDecrypt 会返回
    // STATUS_INVALID_HANDLE（0xC0000008）。
    auto releaseKey = [&handles, &keyObject]() {
        if (handles.key != nullptr) {
            BCryptDestroyKey(handles.key);
            handles.key = nullptr;
        }
        keyObject.fill('\0');
    };

    // 输出最多等于密文长度；多留一个块，避免部分 CNG 实现把填充余量算进
    // 所需输出长度时返回缓冲区不足。
    QByteArray output(ciphertext.size() + 16, '\0');
    status = BCryptDecrypt(
        handles.key, reinterpret_cast<PUCHAR>(const_cast<char *>(ciphertext.constData())),
        static_cast<ULONG>(ciphertext.size()), nullptr,
        reinterpret_cast<PUCHAR>(const_cast<char *>(iv.constData())),
        static_cast<ULONG>(iv.size()), reinterpret_cast<PUCHAR>(output.data()),
        static_cast<ULONG>(output.size()), &resultLength, BCRYPT_BLOCK_PADDING);
    if (status < 0) {
        releaseKey();
        if (error != nullptr)
            *error = bcryptError(QObject::tr("AES 解密"), status);
        return false;
    }
    if (resultLength == 0 || resultLength > static_cast<DWORD>(output.size())) {
        releaseKey();
        if (error != nullptr)
            *error = QObject::tr("AES 解密结果长度异常");
        return false;
    }

    output.resize(static_cast<int>(resultLength));
    *plaintext = output;
    releaseKey();
    return true;
#else
    Q_UNUSED(ciphertext);
    Q_UNUSED(iv);
    if (error != nullptr)
        *error = QObject::tr("当前平台没有可用的 AES-128 解密实现");
    return false;
#endif
}
