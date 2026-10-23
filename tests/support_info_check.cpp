#include "supportinfo.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMetaProperty>
#include <QTemporaryDir>
#include <array>
#include <cstdio>

namespace {
const QString ExampleKey = QStringLiteral("123e4567-e12b-12d1-a456-426655440000");
// Banco Central do Brasil, Manual de Padrões para Iniciação do Pix, section
// 2.6.3 (static BR Code). This public test vector has no real payment details.
const QByteArray ExamplePayload =
    "00020126580014br.gov.bcb.pix0136123e4567-e12b-12d1-a456-426655440000"
    "5204000053039865802BR5913Fulano de Tal6008BRASILIA62070503***63041D3D";
const QString ClipboardSentinel = QStringLiteral("isolated-support-check");

bool check(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "support_info_check: %s\n", message);
    return condition;
}

bool write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// A table-based reference implementation, separate from the production
// bit-at-a-time implementation. Known external vectors anchor both algorithms.
QByteArray crcHex(const QByteArray &bytes)
{
    std::array<unsigned, 256> table{};
    for (unsigned index = 0; index < table.size(); ++index) {
        unsigned value = index << 8;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 0x8000) ? ((value << 1) ^ 0x1021) & 0xffff : (value << 1) & 0xffff;
        table[index] = value;
    }
    unsigned crc = 0xffff;
    for (const char byte : bytes)
        crc = ((crc << 8) ^ table[((crc >> 8) ^ static_cast<unsigned char>(byte)) & 0xff]) & 0xffff;
    return QByteArray::number(crc, 16).rightJustified(4, '0').toUpper();
}

QByteArray field(const QByteArray &id, const QByteArray &value)
{
    return id + QByteArray::number(value.size()).rightJustified(2, '0') + value;
}

QByteArray finish(const QByteArray &body)
{
    const QByteArray withHeader = body + "6304";
    return withHeader + crcHex(withHeader);
}

using FieldList = QList<QPair<QByteArray, QByteArray>>;

FieldList exampleFields()
{
    return {{"00", "01"},
            {"26", field("00", "br.gov.bcb.pix") + field("01", ExampleKey.toUtf8())},
            {"52", "0000"}, {"53", "986"}, {"58", "BR"}, {"59", "Fulano de Tal"},
            {"60", "BRASILIA"}, {"62", field("05", "***")}};
}

QByteArray assemble(const FieldList &fields)
{
    QByteArray body;
    for (const auto &entry : fields) body += field(entry.first, entry.second);
    return finish(body);
}

FieldList changed(const QByteArray &id, const QByteArray &value)
{
    FieldList fields = exampleFields();
    for (auto &entry : fields) {
        if (entry.first == id) {
            entry.second = value;
            return fields;
        }
    }
    fields.append({id, value});
    return fields;
}

FieldList removed(const QByteArray &id)
{
    FieldList fields = exampleFields();
    for (qsizetype index = 0; index < fields.size(); ++index) {
        if (fields[index].first == id) {
            fields.removeAt(index);
            break;
        }
    }
    return fields;
}

QByteArray json(const QByteArray &payload, const QString &key = ExampleKey)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("pixPayload"), QString::fromUtf8(payload)},
                                    {QStringLiteral("pixKey"), key}}).toJson(QJsonDocument::Compact);
}

bool rejected(const QString &path, const QUrl &qr, const QByteArray &configuration, const char *message)
{
    if (!check(write(path, configuration), "não gravou a configuração isolada")) return false;
    SupportInfo support(path, qr);
    return check(!support.available(), message)
        && check(support.pixPayload().isEmpty() && support.pixKey().isEmpty() && support.qrSource().isEmpty(),
                 "configuração inválida expôs dados parciais")
        && check(!support.copyPix(), "configuração inválida permitiu copiar")
        && check(QGuiApplication::clipboard()->text() == ClipboardSentinel,
                 "configuração inválida alterou o clipboard");
}
}

int main(int argc, char **argv)
{
    // This test must never access the user's session clipboard.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!check(temporary.isValid(), "diretório temporário indisponível")) return 1;
    const QString config = temporary.filePath(QStringLiteral("support.json"));
    const QString qrPath = temporary.filePath(QStringLiteral("fixture.svg"));
    const QUrl qr = QUrl::fromLocalFile(qrPath);
    const QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1 1\">"
                           "<path d=\"M0 0h1v1H0z\"/></svg>";
    if (!check(write(qrPath, svg), "não gravou o SVG isolado")
        || !check(crcHex("123456789") == "29B1", "referência CRC-16/CCITT-FALSE divergiu")
        || !check(crcHex(ExamplePayload.first(ExamplePayload.size() - 4)) == "1D3D",
                  "vetor publicado pelo Banco Central divergiu")
        || !check(assemble(exampleFields()) == ExamplePayload, "montagem do vetor estático divergiu")) return 1;

    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(ClipboardSentinel);
    if (!check(write(config, json(ExamplePayload)), "não gravou o vetor público")) return 1;
    SupportInfo valid(config, qr);
    if (!check(valid.available(), "vetor estático público foi recusado")
        || !check(valid.pixKey() == ExampleKey && valid.pixPayload() == QString::fromLatin1(ExamplePayload)
                      && valid.qrSource() == qr, "dados validados foram alterados")
        || !check(clipboard->text() == ClipboardSentinel, "construção acessou o clipboard")
        || !check(valid.copyPix() && clipboard->text() == QString::fromLatin1(ExamplePayload),
                  "cópia explícita não preservou o código Pix")) return 1;
    for (const char *property : {"available", "pixPayload", "pixKey", "qrSource"}) {
        const int index = valid.metaObject()->indexOfProperty(property);
        if (!check(index >= 0 && valid.metaObject()->property(index).isConstant(),
                   "dados de apoio não são constantes")) return 1;
    }
    clipboard->setText(ClipboardSentinel);

    const QList<QByteArray> validPayloads = {
        assemble(changed("01", "11")),
        assemble(changed("62", field("05", "ABc0123456789012345678901"))),
        assemble(changed("62", field("05", "0"))),
        assemble(changed("59", QByteArray(25, 'A'))),
        assemble(changed("60", QByteArray(15, 'B')))
    };
    for (const auto &payload : validPayloads) {
        if (!check(write(config, json(payload)), "não gravou uma variante estática válida")) return 1;
        SupportInfo support(config, qr);
        if (!check(support.available(), "variante estática válida foi recusada")
            || !check(clipboard->text() == ClipboardSentinel, "validação alterou o clipboard")) return 1;
    }

    const QList<QPair<QByteArray, const char *>> invalidPayloads = {
        {assemble(changed("00", "02")), "versão inválida foi aceita"},
        {assemble(changed("01", "12")), "QR dinâmico foi aceito"},
        {assemble(changed("52", "123")), "categoria truncada foi aceita"},
        {assemble(changed("52", "00A0")), "categoria não numérica foi aceita"},
        {assemble(changed("53", "840")), "moeda diferente de real foi aceita"},
        {assemble(changed("54", "1.00")), "valor fixo foi aceito"},
        {assemble(changed("54", "")), "tag de valor vazia foi aceita"},
        {assemble(changed("58", "US")), "país diferente de BR foi aceito"},
        {assemble(changed("59", "   ")), "nome vazio foi aceito"},
        {assemble(changed("59", QByteArray(26, 'A'))), "nome acima do limite foi aceito"},
        {assemble(changed("60", "")), "cidade vazia foi aceita"},
        {assemble(changed("60", QByteArray(16, 'B'))), "cidade acima do limite foi aceita"},
        {assemble(changed("26", field("00", "other.pix") + field("01", ExampleKey.toUtf8()))),
         "GUI diferente de Pix foi aceito"},
        {assemble(changed("26", field("00", "br.gov.bcb.pix") + field("01", ExampleKey.toUtf8())
                                           + field("25", "https://invalid.example/charge"))),
         "endereço de cobrança dinâmica foi aceito"},
        {assemble(changed("26", field("00", "br.gov.bcb.pix") + field("01", "+5561999999999"))),
         "chave que não é aleatória foi aceita"},
        {assemble(changed("26", field("00", "br.gov.bcb.pix") + field("01", ExampleKey.toUtf8())
                                           + field("01", ExampleKey.toUtf8()))),
         "tag aninhada duplicada foi aceita"},
        {assemble(changed("26", "0014br.gov.bcb.pix0137" + ExampleKey.toUtf8())),
         "comprimento aninhado incorreto foi aceito"},
        {assemble(changed("62", field("05", ""))), "txid vazio foi aceito"},
        {assemble(changed("62", field("05", "A_B"))), "txid com símbolo foi aceito"},
        {assemble(changed("62", field("05", "AB\n"))), "txid com quebra de linha foi aceito"},
        {assemble(changed("62", field("05", QByteArray(26, 'A')))), "txid acima do limite foi aceito"},
        {assemble(changed("62", field("05", "***") + field("05", "OTHER"))), "txid duplicado foi aceito"},
        {ExamplePayload.first(ExamplePayload.size() - 1) + "0", "checksum incorreto foi aceito"},
        {ExamplePayload + field("80", "trailing"), "dados após o checksum foram aceitos"},
        {finish("000201000201"), "tag externa duplicada foi aceita"},
        {finish("0A0201"), "id não numérico foi aceito"},
        {finish("000X01"), "comprimento não numérico foi aceito"},
        {finish("00"), "cabeçalho truncado foi aceito"},
        {finish("009901"), "campo truncado foi aceito"},
        {QByteArray(513, '0'), "payload acima do limite foi aceito"}
    };
    for (const auto &entry : invalidPayloads)
        if (!rejected(config, qr, json(entry.first), entry.second)) return 1;
    for (const QByteArray &id : {QByteArray("00"), QByteArray("26"), QByteArray("52"), QByteArray("53"),
                                QByteArray("58"), QByteArray("59"), QByteArray("60"), QByteArray("62")})
        if (!rejected(config, qr, json(assemble(removed(id))), "campo obrigatório ausente foi aceito")) return 1;

    const QList<QByteArray> invalidJson = {
        "{}", "[]", "not-json", QByteArray(16 * 1024 + 1, ' '),
        "{\"pixPayload\":123,\"pixKey\":false}",
        json(ExamplePayload, QStringLiteral("123e4567-e12b-12d1-a456-426655440001")),
        json(ExamplePayload, QStringLiteral("{123e4567-e12b-12d1-a456-426655440000}"))
    };
    for (const QByteArray &configuration : invalidJson)
        if (!rejected(config, qr, configuration, "JSON inválido ou chave divergente foi aceito")) return 1;
    for (const QUrl &invalidQr : {QUrl(QStringLiteral("https://invalid.example/qr.svg")),
                                 QUrl::fromLocalFile(temporary.filePath(QStringLiteral("missing.svg"))), QUrl()})
        if (!rejected(config, invalidQr, json(ExamplePayload), "QR ausente ou remoto foi aceito")) return 1;
    for (const QByteArray &invalidSvg : {QByteArray(""), QByteArray("not-svg"),
                                       QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\"><path>"),
                                       QByteArray("<svg></svg>"), QByteArray(1024 * 1024 + 1, ' ')}) {
        if (!check(write(qrPath, invalidSvg), "não gravou SVG inválido isolado")
            || !rejected(config, qr, json(ExamplePayload), "SVG inválido foi aceito")) return 1;
    }
    SupportInfo missing(temporary.filePath(QStringLiteral("missing.json")), qr);
    if (!check(!missing.available() && !missing.copyPix() && clipboard->text() == ClipboardSentinel,
               "configuração ausente não falhou de modo seguro")) return 1;
    std::puts("support_info_check: static Pix, official CRC vector, local-only QR, malformed configuration and explicit clipboard copy");
    return 0;
}
