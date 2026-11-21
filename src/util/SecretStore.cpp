#include "util/SecretStore.h"

#include <QByteArray>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSecret, "mvp.secret")

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <wincrypt.h>
#endif

namespace {

const char kProtectedPrefix[] = "mvp-dpapi:v1:";

#ifdef Q_OS_WIN
DATA_BLOB entropyBlob()
{
    static const QByteArray entropy = QByteArrayLiteral("MusicVideoPlayer/source-credential/v1");
    DATA_BLOB blob;
    blob.cbData = static_cast<DWORD>(entropy.size());
    blob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(entropy.constData()));
    return blob;
}
#endif

} // namespace

namespace SecretStore {

bool isAvailable()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool isProtectedValue(const QString &value)
{
    return value.startsWith(QLatin1String(kProtectedPrefix));
}

QString protectForStorage(const QString &plainText, const QString &label)
{
    if (plainText.isEmpty())
        return QString();

#ifdef Q_OS_WIN
    QByteArray input = plainText.toUtf8();
    DATA_BLOB in;
    in.cbData = static_cast<DWORD>(input.size());
    in.pbData = reinterpret_cast<BYTE *>(input.data());

    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    const BOOL ok = CryptProtectData(&in, L"MusicVideoPlayer source credential",
                                     &entropy, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) {
        qCWarning(lcSecret) << "凭据加密失败" << label << "错误码" << GetLastError();
        return QString();
    }

    const QByteArray cipher(reinterpret_cast<const char *>(out.pbData),
                            static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return QString::fromLatin1(kProtectedPrefix) + QString::fromLatin1(cipher.toBase64());
#else
    Q_UNUSED(label);
    qCWarning(lcSecret) << "当前平台没有凭据加密能力，源凭据不会写入磁盘";
    return QString();
#endif
}

QString revealStored(const QString &storedValue, const QString &label)
{
    if (storedValue.isEmpty())
        return QString();
    if (!isProtectedValue(storedValue)) {
        // 旧版 sources.json 是明文：本次仍可读取，下一次保存会转成密文。
        return storedValue;
    }

#ifdef Q_OS_WIN
    const QByteArray cipher =
        QByteArray::fromBase64(storedValue.mid(int(sizeof(kProtectedPrefix) - 1)).toLatin1());
    if (cipher.isEmpty())
        return QString();

    DATA_BLOB in;
    in.cbData = static_cast<DWORD>(cipher.size());
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()));

    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB out{};
    const BOOL ok = CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                                       CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) {
        // 换机器/换 Windows 用户后旧密文解不开，按空凭据处理，不把密文当 Cookie 发出去。
        qCWarning(lcSecret) << "凭据解密失败，已按空值处理" << label << "错误码" << GetLastError();
        return QString();
    }

    const QByteArray plain(reinterpret_cast<const char *>(out.pbData),
                           static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return QString::fromUtf8(plain);
#else
    Q_UNUSED(label);
    qCWarning(lcSecret) << "当前平台没有凭据解密能力，已忽略加密值";
    return QString();
#endif
}

} // namespace SecretStore
