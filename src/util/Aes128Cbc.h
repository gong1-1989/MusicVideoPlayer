#ifndef AES128CBC_H
#define AES128CBC_H

#include <QByteArray>
#include <QString>

/// AES-128-CBC 解密，供 HLS 的 AES-128 分片使用。
///
/// HLS 的 AES-128 使用 PKCS#7 填充；这里只做“16 字节密钥 + 16 字节 IV”
/// 的标准 CBC 解密，不引入第三方加密库。Windows 上走系统 CNG（bcrypt），
/// 其它平台明确失败，避免悄悄把密文当明文写进缓存或下载文件。
namespace Aes128Cbc {

/// 解密一段 AES-128-CBC/PKCS#7 密文。
/// @param ciphertext 密文，长度必须是 16 的整数倍。
/// @param key 16 字节密钥。
/// @param iv 16 字节初始向量。
/// @param plaintext 输出明文。
/// @param error 失败原因，可为空。
/// @return 成功返回 true，并去掉 PKCS#7 填充。
bool decrypt(const QByteArray &ciphertext, const QByteArray &key, const QByteArray &iv,
             QByteArray *plaintext, QString *error = nullptr);

} // namespace Aes128Cbc

#endif // AES128CBC_H
