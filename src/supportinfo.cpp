#include "supportinfo.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace {
using Fields = QMap<QByteArray, QByteArray>;

bool digits(const QByteArray &value)
{
    for (const char character : value)
        if (character < '0' || character > '9') return false;
    return !value.isEmpty();
}

bool parseFields(const QByteArray &source, Fields *fields, QByteArray *lastId = nullptr)
{
    Fields parsed;
    qsizetype position = 0;
    while (position < source.size()) {
        if (source.size() - position < 4) return false;
        const QByteArray id = source.mid(position, 2);
        const QByteArray length = source.mid(position + 2, 2);
        if (!digits(id) || !digits(length) || parsed.contains(id)) return false;
        const int count = (length[0] - '0') * 10 + length[1] - '0';
        position += 4;
        if (source.size() - position < count) return false;
        parsed.insert(id, source.mid(position, count));
        position += count;
        if (lastId) *lastId = id;
    }
    *fields = parsed;
    return !parsed.isEmpty();
}

quint16 checksum(const QByteArray &source)
{
    quint16 crc = 0xffff;
    for (const char character : source) {
        crc ^= quint16(static_cast<unsigned char>(character)) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
    }
    return crc;
}

bool validPayload(const QString &payload, const QString &configuredKey)
{
    static const QRegularExpression randomKey(QStringLiteral(
        "\\A[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\z"));
    static const QRegularExpression transactionId(QStringLiteral("\\A(?:\\*\\*\\*|[A-Za-z0-9]{1,25})\\z"));
    static const QRegularExpression crcText(QStringLiteral("\\A[0-9a-fA-F]{4}\\z"));
    if (!randomKey.match(configuredKey).hasMatch()) return false;

    const QByteArray bytes = payload.toUtf8();
    if (bytes.isEmpty() || bytes.size() > 512) return false;
    Fields fields, account, additional;
    QByteArray lastId;
    if (!parseFields(bytes, &fields, &lastId) || lastId != "63"
        || fields.value("00") != "01"
        || (fields.contains("01") && fields.value("01") != "11")
        || fields.value("52").size() != 4 || !digits(fields.value("52"))
        || fields.value("53") != "986" || fields.contains("54")
        || fields.value("58") != "BR"
        || fields.value("59").trimmed().isEmpty() || fields.value("59").size() > 25
        || fields.value("60").trimmed().isEmpty() || fields.value("60").size() > 15
        || !parseFields(fields.value("26"), &account)
        || account.value("00") != "br.gov.bcb.pix" || account.contains("25")
        || account.value("01") != configuredKey.toUtf8()
        || !parseFields(fields.value("62"), &additional)
        || !transactionId.match(QString::fromLatin1(additional.value("05"))).hasMatch()
        || !crcText.match(QString::fromLatin1(fields.value("63"))).hasMatch()) return false;

    // The checksum includes the final 6304 header, excluding its four hex
    // digits. Duplicate TLVs and any data after tag 63 were rejected above.
    bool converted = false;
    const quint16 expected = fields.value("63").toUShort(&converted, 16);
    return converted && checksum(bytes.first(bytes.size() - 4)) == expected;
}

QString localPath(const QUrl &source)
{
    if (!source.isValid() || source.hasQuery() || source.hasFragment()) return {};
    if (source.scheme() == QStringLiteral("qrc") && source.host().isEmpty())
        return QStringLiteral(":") + source.path();
    if (source.isLocalFile() && source.host().isEmpty()) return source.toLocalFile();
    return {};
}

bool hasLocalSvg(const QUrl &source)
{
    const QString path = localPath(source);
    if (path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 1024 * 1024) return false;
    QXmlStreamReader xml(file.readAll());
    if (!xml.readNextStartElement() || xml.name() != QStringLiteral("svg")
        || xml.namespaceUri() != QStringLiteral("http://www.w3.org/2000/svg")) return false;
    while (!xml.atEnd()) xml.readNext();
    return !xml.hasError();
}
}

SupportInfo::SupportInfo(QObject *parent)
    : SupportInfo(QStringLiteral(":/mandarina/assets/support-pix.json"),
                  QUrl(QStringLiteral("qrc:/mandarina/assets/support-pix-qr.svg")), parent)
{
}

SupportInfo::SupportInfo(const QString &configurationPath, const QUrl &qrSource, QObject *parent)
    : QObject(parent)
{
    QFile file(configurationPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 16 * 1024) return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return;
    const QJsonObject object = document.object();
    if (!object.value(QStringLiteral("pixPayload")).isString()
        || !object.value(QStringLiteral("pixKey")).isString()) return;
    const QString payload = object.value(QStringLiteral("pixPayload")).toString();
    const QString key = object.value(QStringLiteral("pixKey")).toString();
    if (!validPayload(payload, key) || !hasLocalSvg(qrSource)) return;
    m_payload = payload;
    m_key = key;
    m_qrSource = qrSource;
    m_available = true;
}

bool SupportInfo::copyPix() const
{
    if (!m_available || !qobject_cast<QGuiApplication *>(QCoreApplication::instance())) return false;
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) return false;
    clipboard->setText(m_payload, QClipboard::Clipboard);
    return clipboard->text(QClipboard::Clipboard) == m_payload;
}
