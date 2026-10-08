#include "nax5/nax5authstore.h"

#include "nax5/nax5runtime.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QSettings>
#include <QSysInfo>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

constexpr auto kRememberKey = "auth/remember";
constexpr auto kEmailKey = "auth/email";
// Plain-text password written by earlier builds: read once, re-saved encrypted and removed.
constexpr auto kLegacyPasswordKey = "auth/password";
constexpr auto kPasswordKey = "auth/password_enc";
constexpr auto kSessionTokenKey = "auth/session_token";

constexpr int kIvSize = 12;
constexpr int kTagSize = 16;
const QByteArray kFormat = QByteArrayLiteral("v1:");

QSettings authSettings()
{
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope,
        Nax5Runtime::settingsOrganizationName(), Nax5Runtime::settingsApplicationName());
}

// The saved password is encrypted with a key bound to this device and this user account, so the
// settings file alone, copied elsewhere, does not reveal it. It does not protect against software
// running as the same user on this device. Empty when the device has no stable id.
QByteArray deviceKey()
{
    const QByteArray machine = QSysInfo::machineUniqueId();
    if (machine.isEmpty())
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("NAX5 saved password v1\n"));
    hash.addData(machine);
    hash.addData(QByteArrayLiteral("\n"));
    hash.addData(QDir::homePath().toUtf8());
    return hash.result();
}

// AES-256-GCM: "v1:" + base64(iv | ciphertext | tag). Empty on any failure.
QByteArray encryptPassword(const QString &password)
{
    const QByteArray key = deviceKey();
    if (key.size() != 32 || password.isEmpty())
        return {};
    QByteArray plain = password.toUtf8();
    QByteArray iv(kIvSize, Qt::Uninitialized);
    QByteArray out;
    if (RAND_bytes(reinterpret_cast<unsigned char *>(iv.data()), kIvSize) == 1)
    {
        QByteArray cipher(plain.size(), Qt::Uninitialized);
        QByteArray tag(kTagSize, Qt::Uninitialized);
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        int len = 0;
        int total = 0;
        bool ok = ctx
            && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
            && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvSize, nullptr) == 1
            && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                reinterpret_cast<const unsigned char *>(key.constData()),
                reinterpret_cast<const unsigned char *>(iv.constData())) == 1
            && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(cipher.data()), &len,
                reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) == 1;
        total = len;
        ok = ok && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(cipher.data()) + total, &len) == 1;
        total += len;
        ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagSize, tag.data()) == 1;
        EVP_CIPHER_CTX_free(ctx);
        if (ok)
            out = kFormat + (iv + cipher.left(total) + tag).toBase64();
    }
    plain.fill('\0');
    return out;
}

QString decryptPassword(const QByteArray &stored)
{
    const QByteArray key = deviceKey();
    if (key.size() != 32 || !stored.startsWith(kFormat))
        return {};
    const QByteArray blob = QByteArray::fromBase64(stored.mid(kFormat.size()));
    if (blob.size() <= kIvSize + kTagSize)
        return {};
    const QByteArray iv = blob.left(kIvSize);
    const QByteArray tag = blob.right(kTagSize);
    const QByteArray cipher = blob.mid(kIvSize, blob.size() - kIvSize - kTagSize);
    QByteArray plain(cipher.size(), Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    int total = 0;
    bool ok = ctx
        && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvSize, nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
            reinterpret_cast<const unsigned char *>(key.constData()),
            reinterpret_cast<const unsigned char *>(iv.constData())) == 1
        && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &len,
            reinterpret_cast<const unsigned char *>(cipher.constData()), cipher.size()) == 1;
    total = len;
    // A wrong key (another device, another user) or a modified value fails the tag check here.
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagSize,
        const_cast<char *>(tag.constData())) == 1;
    ok = ok && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plain.data()) + total, &len) == 1;
    total += len;
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    const QString password = ok ? QString::fromUtf8(plain.constData(), total) : QString();
    plain.fill('\0');
    return password;
}

void storePassword(QSettings &settings, const QString &password)
{
    settings.remove(kLegacyPasswordKey);
    const QByteArray encrypted = encryptPassword(password);
    if (encrypted.isEmpty())
        settings.remove(kPasswordKey);
    else
        settings.setValue(kPasswordKey, QString::fromLatin1(encrypted));
}

void removePassword(QSettings &settings)
{
    settings.remove(kLegacyPasswordKey);
    settings.remove(kPasswordKey);
}

} // namespace

bool nax5AuthRememberEnabled()
{
    // On by default: a handheld is a personal device and players expect to stay signed in.
    return authSettings().value(kRememberKey, true).toBool();
}

void nax5AuthSetRememberEnabled(bool enabled)
{
    QSettings settings = authSettings();
    settings.setValue(kRememberKey, enabled);
    if(!enabled)
    {
        removePassword(settings);
        settings.remove(kSessionTokenKey);
    }
}

QString nax5AuthSavedEmail()
{
    return authSettings().value(kEmailKey).toString();
}

QString nax5AuthSavedPassword()
{
    QSettings settings = authSettings();
    if(!settings.value(kRememberKey, false).toBool())
        return {};
    if(settings.contains(kLegacyPasswordKey))
    {
        const QString legacy = settings.value(kLegacyPasswordKey).toString();
        storePassword(settings, legacy);
        settings.sync();
        return legacy;
    }
    return decryptPassword(settings.value(kPasswordKey).toString().toLatin1());
}

QString nax5AuthSavedSessionToken()
{
    QSettings settings = authSettings();
    if(!settings.value(kRememberKey, false).toBool())
        return {};
    return settings.value(kSessionTokenKey).toString();
}

void nax5AuthSaveCredentials(const QString &email, const QString &password, const QString &session_token)
{
    QSettings settings = authSettings();
    settings.setValue(kRememberKey, true);
    settings.setValue(kEmailKey, email.trimmed());
    // Empty value means "forget": logout passes empty password and token.
    if(password.isEmpty())
        removePassword(settings);
    else
        storePassword(settings, password);
    if(session_token.isEmpty())
        settings.remove(kSessionTokenKey);
    else
        settings.setValue(kSessionTokenKey, session_token);
    settings.sync();
}

void nax5AuthClearCredentials()
{
    QSettings settings = authSettings();
    settings.remove(kRememberKey);
    settings.remove(kEmailKey);
    removePassword(settings);
    settings.remove(kSessionTokenKey);
    settings.sync();
}
