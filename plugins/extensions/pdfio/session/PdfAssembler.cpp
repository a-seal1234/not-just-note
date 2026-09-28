/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfAssembler.h"
#include "PdfLzwDecoder.h"

#include "session/PdfSession.h"

#include <QFile>
#include <QImage>
#include <QSaveFile>
#include <QFileInfo>
#include <QDebug>
#include <QHash>
#include <QList>
#include <QSet>

#include <climits>

#include <cstdio>

/**
 * The assembler's reader, and why it is narrow on purpose.
 *
 * The proven exporter reader already covers classic xref tables, xref streams, hybrid /XRefStm,
 * /ObjStm objects and FlateDecode or LZWDecode with PNG/TIFF predictors. LZW honors /EarlyChange
 * through the shared bounded decoder. This assembler ports the subset needed to make the same decision
 * on the same files; unsupported encryption, structural filters or malformed structures are refused by name. A half-read source written as a file that only looks right is never an
 * acceptable outcome.
 *
 * Source streams are copied byte-for-byte with their original dictionary; only packed object bodies
 * and xref streams are decoded for reading. The text layer, fonts and embedded images remain intact.
 */
namespace {

const QByteArray NL("\n", 1);

/// FlateDecode wants raw zlib; qCompress prepends a four byte length that PDF does not expect. The
/// same call the exporter writes its ink with, so a picture and an ink plane are compressed alike.
QByteArray deflateForPdf(const QByteArray &data)
{
    return qCompress(data, 9).mid(4);
}

/// A picture's pixels the way PDF wants them: eight bits per channel, RGB, row by row.
QByteArray rgbSamplesForPdf(const QImage &image)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    QByteArray samples;
    samples.reserve(int(rgb.width()) * int(rgb.height()) * 3);
    for (int y = 0; y < rgb.height(); ++y) {
        samples.append(reinterpret_cast<const char *>(rgb.constScanLine(y)), rgb.width() * 3);
    }
    return samples;
}

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

bool isWhite(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
}

bool isDelimiter(char c)
{
    return isWhite(c) || c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']'
        || c == '{' || c == '}' || c == '/' || c == '%';
}

int skipWhite(const QByteArray &b, int i)
{
    while (i < b.size() && isWhite(b.at(i))) {
        ++i;
    }
    return i;
}

/// Reads an integer at \a i, whitespace first. Advances \a i only when it read one.
bool readIntAt(const QByteArray &b, int *i, qint64 *out)
{
    int j = skipWhite(b, *i);
    const int start = j;
    if (j < b.size() && (b.at(j) == '-' || b.at(j) == '+')) {
        ++j;
    }
    int digits = 0;
    while (j < b.size() && b.at(j) >= '0' && b.at(j) <= '9') {
        ++j;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    bool ok = false;
    const qint64 value = b.mid(start, j - start).toLongLong(&ok);
    if (!ok) {
        return false;
    }
    *i = j;
    *out = value;
    return true;
}

/// Reads a real at \a i, whitespace first. Advances \a i only when it read one.
bool readRealAt(const QByteArray &b, int *i, double *out)
{
    int j = skipWhite(b, *i);
    const int start = j;
    if (j < b.size() && (b.at(j) == '-' || b.at(j) == '+')) {
        ++j;
    }
    int digits = 0;
    while (j < b.size() && ((b.at(j) >= '0' && b.at(j) <= '9') || b.at(j) == '.')) {
        ++j;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    bool ok = false;
    const double value = b.mid(start, j - start).toDouble(&ok);
    if (!ok) {
        return false;
    }
    *i = j;
    *out = value;
    return true;
}

/// Just past the ')' that closes the literal string starting at the '(' at \a i.
int skipLiteralString(const QByteArray &b, int i)
{
    int depth = 0;
    while (i < b.size()) {
        const char c = b.at(i);
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0) {
                return i + 1;
            }
        }
        ++i;
    }
    return b.size();
}

/// Just past the '>>' that closes the dictionary starting at \a start (which must be "<<").
int dictEndIndex(const QByteArray &b, int start)
{
    int i = start;
    int depth = 0;
    while (i < b.size()) {
        const char c = b.at(i);
        if (c == '%') {
            while (i < b.size() && b.at(i) != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '(') {
            i = skipLiteralString(b, i);
            continue;
        }
        if (c == '<') {
            if (i + 1 < b.size() && b.at(i + 1) == '<') {
                ++depth;
                i += 2;
                continue;
            }
            while (i < b.size() && b.at(i) != '>') {
                ++i;
            }
            ++i;
            continue;
        }
        if (c == '>') {
            if (i + 1 < b.size() && b.at(i + 1) == '>') {
                --depth;
                i += 2;
                if (depth == 0) {
                    return i;
                }
                continue;
            }
            ++i;
            continue;
        }
        ++i;
    }
    return -1;
}

/// The index of \a key's value in a dictionary at the TOP level (a key inside a nested dictionary
/// or a string is not the key). False when it is absent.
bool topLevelKey(const QByteArray &dict, const QByteArray &key, int *valueAt)
{
    if (dict.size() < 2 || dict.at(0) != '<' || dict.at(1) != '<') {
        return false;
    }
    const int end = dictEndIndex(dict, 0);
    const int limit = end < 0 ? dict.size() : end;
    int i = 2;
    int depth = 1;
    while (i < limit) {
        const char c = dict.at(i);
        if (c == '(') {
            i = skipLiteralString(dict, i);
            continue;
        }
        if (c == '<') {
            if (i + 1 < limit && dict.at(i + 1) == '<') {
                ++depth;
                i += 2;
                continue;
            }
            while (i < limit && dict.at(i) != '>') {
                ++i;
            }
            ++i;
            continue;
        }
        if (c == '>') {
            if (i + 1 < limit && dict.at(i + 1) == '>') {
                --depth;
                i += 2;
                continue;
            }
            ++i;
            continue;
        }
        if (c == '%') {
            while (i < limit && dict.at(i) != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '/' && depth == 1) {
            int s = i + 1;
            int e = s;
            while (e < limit && !isDelimiter(dict.at(e))) {
                ++e;
            }
            if (dict.mid(s, e - s) == key) {
                *valueAt = skipWhite(dict, e);
                return true;
            }
            i = e;
            continue;
        }
        ++i;
    }
    return false;
}

/// The name a dictionary's /Type holds, without the slash. Empty when there is none.
///
/// A textual search for "/Pages" would also match a name nested inside an annotation or a content
/// reference, so the key is looked up properly and its value read as a name.
QByteArray typeNameOf(const QByteArray &body)
{
    int valueAt = 0;
    if (!topLevelKey(body, "Type", &valueAt)) {
        return QByteArray();
    }
    if (valueAt < body.size() && body.at(valueAt) == '/') {
        int e = valueAt + 1;
        while (e < body.size() && !isDelimiter(body.at(e))) {
            ++e;
        }
        return body.mid(valueAt + 1, e - valueAt - 1);
    }
    return QByteArray();
}

/// A value at \a at that is "number generation R".
bool valueIsRef(const QByteArray &body, int at, int *number, int *generation = nullptr)
{
    int i = at;
    qint64 n = 0;
    qint64 g = 0;
    if (!readIntAt(body, &i, &n) || !readIntAt(body, &i, &g)) {
        return false;
    }
    i = skipWhite(body, i);
    if (i >= body.size() || body.at(i) != 'R') {
        return false;
    }
    if (n <= 0 || n > INT_MAX || g < 0 || g > 65535 || number == nullptr) {
        return false;
    }
    *number = int(n);
    if (generation) *generation = int(g);
    return true;
}

struct XrefEntry {
    int type = 0;      ///< 1 uncompressed, 2 packed in an object stream
    qint64 first = 0; ///< type 1: byte offset; type 2: object-stream object number
    int second = 0;   ///< type 1: generation; type 2: object index in that stream
};

struct ObjectStream {
    QByteArray decoded;
    int first = 0;
    QList<int> objectNumbers;
    QList<int> offsets;
};

struct SourceDoc {
    QByteArray bytes;
    /// Filled in as the xref chain is walked, through a const SourceDoc&, exactly like the caches
    /// below: reading a source is the one pass that mutates it.
    mutable QHash<int, XrefEntry> xref;
    mutable QHash<int, ObjectStream> objectStreams;
    mutable QHash<int, QByteArray> packedObjectCache;
    mutable QSet<int> resolving;
    int rootNumber = -1;
    int rootGeneration = 0;
    QString file;      ///< the relative name the manifest records, for every message
};

/// One page of a source, with the attributes that are EFFECTIVE for it already resolved to the
/// nearest ancestor that carries them -- which for the user's own files is the /Pages node.
struct SourcePage {
    int object = -1;
    QByteArray resources;      ///< raw value text: "12 0 R" or an inline dictionary
    QByteArray mediaBox;       ///< raw value text: "13 0 R" or an array
    QByteArray cropBox;
    QByteArray rotate;         ///< raw value text, when the page or an ancestor has one
    bool inheritsResources = false;
    bool inheritsMediaBox = false;
    bool hasResources = false;
    bool hasMediaBox = false;
    bool hasCropBox = false;
    bool hasRotate = false;
};

bool readObjectBody(const SourceDoc &doc, int number, QByteArray *body, QString *why, int generation = -1);

bool dictionaryIntValue(const QByteArray &dict, const QByteArray &key, qint64 *value)
{
    int at = 0;
    return topLevelKey(dict, key, &at) && readIntAt(dict, &at, value);
}

bool dictionaryIntArray(const QByteArray &dict, const QByteArray &key, QList<qint64> *values)
{
    int at = 0;
    if (!topLevelKey(dict, key, &at)) {
        return false;
    }
    at = skipWhite(dict, at);
    if (at >= dict.size() || dict.at(at) != '[') {
        return false;
    }
    ++at;
    values->clear();
    while (at < dict.size()) {
        at = skipWhite(dict, at);
        if (at < dict.size() && dict.at(at) == ']') {
            return !values->isEmpty();
        }
        qint64 value = 0;
        if (!readIntAt(dict, &at, &value) || value < 0) {
            return false;
        }
        values->append(value);
        /// An /Index can name many ranges (an incrementally updated file has one pair per
        /// revision), so the guard is only against an unreadable number, not against a valid one.
        if (values->size() > 65536) {
            return false;
        }
    }
    return false;
}

bool readPdfName(const QByteArray &bytes, int *at, QByteArray *name)
{
    *at = skipWhite(bytes, *at);
    if (*at >= bytes.size() || bytes.at(*at) != '/') {
        return false;
    }
    const int start = ++(*at);
    while (*at < bytes.size() && !isDelimiter(bytes.at(*at))) {
        ++(*at);
    }
    if (*at == start) {
        return false;
    }
    *name = bytes.mid(start, *at - start);
    return true;
}

bool dictionaryFilter(const QByteArray &dict, QByteArray *filter, QString *why)
{
    int at = 0;
    if (!topLevelKey(dict, "Filter", &at)) {
        filter->clear();
        return true;
    }
    at = skipWhite(dict, at);
    if (at < dict.size() && dict.at(at) == '/') {
        if (!readPdfName(dict, &at, filter)) {
            fail(why, QStringLiteral("a stream has an unreadable /Filter name"));
            return false;
        }
        return true;
    }
    if (at < dict.size() && dict.at(at) == '[') {
        ++at;
        at = skipWhite(dict, at);
        if (!readPdfName(dict, &at, filter)) {
            fail(why, QStringLiteral("a stream has an unreadable /Filter array"));
            return false;
        }
        at = skipWhite(dict, at);
        if (at >= dict.size() || dict.at(at) != ']') {
            fail(why, QStringLiteral("a stream uses a filter chain the assembler does not support"));
            return false;
        }
        return true;
    }
    fail(why, QStringLiteral("a stream has an unsupported /Filter value"));
    return false;
}

bool dictionaryDecodeParms(const QByteArray &dict, QByteArray *parms, QString *why)
{
    int at = 0;
    if (!topLevelKey(dict, "DecodeParms", &at)) {
        parms->clear();
        return true;
    }
    at = skipWhite(dict, at);
    if (dict.mid(at, 2) == "<<") {
        const int end = dictEndIndex(dict, at);
        if (end < 0) {
            fail(why, QStringLiteral("a stream has unreadable /DecodeParms"));
            return false;
        }
        *parms = dict.mid(at, end - at);
        return true;
    }
    if (dict.mid(at, 4) == "null") {
        parms->clear();
        return true;
    }
    if (at < dict.size() && dict.at(at) == '[') {
        at = skipWhite(dict, at + 1);
        if (dict.mid(at, 4) == "null") {
            parms->clear();
            return true;
        }
        if (dict.mid(at, 2) == "<<") {
            const int end = dictEndIndex(dict, at);
            if (end < 0) {
                fail(why, QStringLiteral("a stream has unreadable /DecodeParms"));
                return false;
            }
            *parms = dict.mid(at, end - at);
            return true;
        }
    }
    fail(why, QStringLiteral("a stream has unsupported /DecodeParms"));
    return false;
}

int paethPredictor(int left, int up, int upperLeft)
{
    const int guess = left + up - upperLeft;
    const int leftDistance = qAbs(guess - left);
    const int upDistance = qAbs(guess - up);
    const int upperLeftDistance = qAbs(guess - upperLeft);
    if (leftDistance <= upDistance && leftDistance <= upperLeftDistance) {
        return left;
    }
    return upDistance <= upperLeftDistance ? up : upperLeft;
}

bool applyPdfPredictor(const QByteArray &input, const QByteArray &parms, QByteArray *output, QString *why)
{
    qint64 predictor = 1;
    qint64 colors = 1;
    qint64 bits = 8;
    qint64 columns = 1;
    dictionaryIntValue(parms, "Predictor", &predictor);
    dictionaryIntValue(parms, "Colors", &colors);
    dictionaryIntValue(parms, "BitsPerComponent", &bits);
    dictionaryIntValue(parms, "Columns", &columns);
    if (predictor <= 1) {
        *output = input;
        return true;
    }
    if (colors <= 0 || columns <= 0 || bits <= 0 || bits > 16) {
        fail(why, QStringLiteral("a stream declares unusable predictor dimensions"));
        return false;
    }
    if (predictor == 2) {
        if (bits != 8) {
            fail(why, QStringLiteral("the assembler only supports 8-bit TIFF predictors"));
            return false;
        }
        const qint64 rowBytes64 = colors * columns;
        if (rowBytes64 <= 0 || rowBytes64 > input.size() || rowBytes64 > INT_MAX
            || input.size() % int(rowBytes64) != 0) {
            fail(why, QStringLiteral("a stream's TIFF predictor rows do not fit its data"));
            return false;
        }
        const int rowBytes = int(rowBytes64);
        *output = input;
        for (int row = 0; row < output->size(); row += rowBytes) {
            for (int i = int(colors); i < rowBytes; ++i) {
                (*output)[row + i] = char(quint8(output->at(row + i))
                                          + quint8(output->at(row + i - int(colors))));
            }
        }
        return true;
    }
    if (predictor < 10 || predictor > 15) {
        fail(why, QStringLiteral("a stream declares the unknown predictor %1").arg(predictor));
        return false;
    }
    const qint64 rowBytes64 = (colors * bits * columns + 7) / 8;
    const qint64 bytesPerPixel64 = qMax<qint64>(1, (colors * bits + 7) / 8);
    if (rowBytes64 <= 0 || rowBytes64 > INT_MAX || bytesPerPixel64 > INT_MAX) {
        fail(why, QStringLiteral("a stream declares predictor dimensions that are too large"));
        return false;
    }
    const int rowBytes = int(rowBytes64);
    const int bytesPerPixel = int(bytesPerPixel64);
    if (input.size() % (rowBytes + 1) != 0) {
        fail(why, QStringLiteral("a stream's PNG predictor rows do not fit its data"));
        return false;
    }
    output->resize(input.size() / (rowBytes + 1) * rowBytes);
    int source = 0;
    int destination = 0;
    while (source < input.size()) {
        const int filter = quint8(input.at(source++));
        if (filter > 4) {
            fail(why, QStringLiteral("a stream uses unknown PNG predictor filter %1").arg(filter));
            return false;
        }
        for (int i = 0; i < rowBytes; ++i) {
            const int raw = quint8(input.at(source++));
            const int left = i >= bytesPerPixel ? quint8(output->at(destination + i - bytesPerPixel)) : 0;
            const int up = destination >= rowBytes ? quint8(output->at(destination + i - rowBytes)) : 0;
            const int upperLeft = i >= bytesPerPixel && destination >= rowBytes
                ? quint8(output->at(destination + i - rowBytes - bytesPerPixel)) : 0;
            int decoded = raw;
            switch (filter) {
            case 1: decoded += left; break;
            case 2: decoded += up; break;
            case 3: decoded += (left + up) / 2; break;
            case 4: decoded += paethPredictor(left, up, upperLeft); break;
            default: break;
            }
            (*output)[destination + i] = char(decoded & 0xff);
        }
        destination += rowBytes;
    }
    return true;
}

bool decodeStream(const QByteArray &dict, const QByteArray &raw, QByteArray *decoded, QString *why)
{
    QByteArray filter;
    if (!dictionaryFilter(dict, &filter, why)) {
        return false;
    }
    if (filter.isEmpty()) {
        *decoded = raw;
        return true;
    }

    const bool flate = filter == "FlateDecode" || filter == "Fl";
    const bool lzw = filter == "LZWDecode" || filter == "LZW";
    if (!flate && !lzw) {
        fail(why, QStringLiteral("the assembler does not support structural stream filter /%1")
                       .arg(QString::fromLatin1(filter)));
        return false;
    }

    QByteArray parms;
    if (!dictionaryDecodeParms(dict, &parms, why)) {
        return false;
    }

    QByteArray decodedData;
    if (flate) {
        decodedData = qUncompress(QByteArray(4, '\0') + raw);
        if (decodedData.isNull()) {
            fail(why, QStringLiteral("a FlateDecode stream could not be decompressed"));
            return false;
        }
    } else {
        qint64 earlyChange = 1;
        dictionaryIntValue(parms, "EarlyChange", &earlyChange);
        if (earlyChange != 0 && earlyChange != 1) {
            fail(why, QStringLiteral("an LZWDecode stream has unsupported /EarlyChange %1")
                           .arg(earlyChange));
            return false;
        }
        if (!PdfLzwDecoder::decode(raw, int(earlyChange), &decodedData, why)) {
            return false;
        }
    }

    if (!applyPdfPredictor(decodedData, parms, decoded, why)) {
        return false;
    }
    return true;
}

bool streamLength(const SourceDoc &doc, const QByteArray &dict, qint64 *length, QString *why)
{
    int at = 0;
    if (!topLevelKey(dict, "Length", &at)) {
        fail(why, QStringLiteral("a stream dictionary has no /Length"));
        return false;
    }
    int referenced = 0;
    int generation = 0;
    if (valueIsRef(dict, at, &referenced, &generation)) {
        QByteArray object;
        if (!readObjectBody(doc, referenced, &object, why, generation)) {
            return false;
        }
        int valueAt = skipWhite(object, 0);
        if (!readIntAt(object, &valueAt, length)) {
            fail(why, QStringLiteral("an indirect stream /Length is not an integer"));
            return false;
        }
    } else if (!readIntAt(dict, &at, length)) {
        fail(why, QStringLiteral("a stream /Length is not an integer or reference"));
        return false;
    }
    if (*length < 0) {
        fail(why, QStringLiteral("a stream has a negative /Length"));
        return false;
    }
    return true;
}

bool streamParts(const SourceDoc &doc, const QByteArray &body, QByteArray *dict, QByteArray *raw,
                 QString *why)
{
    const int dictEnd = dictEndIndex(body, 0);
    if (dictEnd < 0) {
        fail(why, QStringLiteral("a stream object has an unreadable dictionary"));
        return false;
    }
    *dict = body.left(dictEnd);
    const int streamAt = skipWhite(body, dictEnd);
    if (body.mid(streamAt, 6) != "stream") {
        fail(why, QStringLiteral("an object expected to be a stream has no stream keyword"));
        return false;
    }
    int dataStart = streamAt + 6;
    if (body.mid(dataStart, 2) == "\r\n") {
        dataStart += 2;
    } else if (dataStart < body.size() && (body.at(dataStart) == '\r' || body.at(dataStart) == '\n')) {
        ++dataStart;
    } else {
        fail(why, QStringLiteral("a stream keyword is not followed by an end-of-line"));
        return false;
    }
    qint64 length = 0;
    if (!streamLength(doc, *dict, &length, why)) {
        return false;
    }
    if (length > body.size() - dataStart) {
        fail(why, QStringLiteral("a stream's /Length extends past its object"));
        return false;
    }
    const int dataEnd = dataStart + int(length);
    const int endstream = skipWhite(body, dataEnd);
    if (body.mid(endstream, 9) != "endstream") {
        fail(why, QStringLiteral("a stream's /Length does not end at endstream"));
        return false;
    }
    *raw = body.mid(dataStart, int(length));
    return true;
}

bool objectBodyAtOffset(const SourceDoc &doc, qint64 offset, int expectedNumber, int expectedGeneration,
                        QByteArray *body, QString *why)
{
    if (offset < 0 || offset >= doc.bytes.size()) {
        fail(why, QStringLiteral("%1: an xref offset lies outside the file").arg(doc.file));
        return false;
    }
    int i = skipWhite(doc.bytes, int(offset));
    qint64 number = 0;
    qint64 generation = 0;
    if (!readIntAt(doc.bytes, &i, &number) || !readIntAt(doc.bytes, &i, &generation)
        || number <= 0 || number > INT_MAX || generation < 0 || generation > 65535) {
        fail(why, QStringLiteral("%1: an xref offset does not name a valid indirect object")
                       .arg(doc.file));
        return false;
    }
    if (expectedNumber >= 0 && number != expectedNumber) {
        fail(why, QStringLiteral("%1: the xref entry for object %2 points at object %3")
                       .arg(doc.file).arg(expectedNumber).arg(number));
        return false;
    }
    if (expectedGeneration >= 0 && generation != expectedGeneration) {
        fail(why, QStringLiteral("%1: object %2 header generation %3 disagrees with xref generation %4")
                       .arg(doc.file).arg(number).arg(generation).arg(expectedGeneration));
        return false;
    }
    i = skipWhite(doc.bytes, i);
    if (doc.bytes.mid(i, 3) != "obj") {
        fail(why, QStringLiteral("%1: object %2 does not start with obj")
                       .arg(doc.file).arg(number));
        return false;
    }
    const int bodyStart = skipWhite(doc.bytes, i + 3);
    if (doc.bytes.mid(bodyStart, 2) == "<<") {
        const int dictEnd = dictEndIndex(doc.bytes, bodyStart);
        if (dictEnd < 0) {
            fail(why, QStringLiteral("%1: object %2 has an unreadable dictionary")
                           .arg(doc.file).arg(number));
            return false;
        }
        const int after = skipWhite(doc.bytes, dictEnd);
        if (doc.bytes.mid(after, 6) == "stream") {
            const QByteArray dict = doc.bytes.mid(bodyStart, dictEnd - bodyStart);
            int dataStart = after + 6;
            if (doc.bytes.mid(dataStart, 2) == "\r\n") {
                dataStart += 2;
            } else if (dataStart < doc.bytes.size()
                       && (doc.bytes.at(dataStart) == '\r' || doc.bytes.at(dataStart) == '\n')) {
                ++dataStart;
            } else {
                fail(why, QStringLiteral("%1: object %2 has a malformed stream line ending")
                               .arg(doc.file).arg(number));
                return false;
            }
            qint64 length = 0;
            if (!streamLength(doc, dict, &length, why)) {
                return false;
            }
            if (length > doc.bytes.size() - dataStart) {
                fail(why, QStringLiteral("%1: object %2 stream extends past the file")
                               .arg(doc.file).arg(number));
                return false;
            }
            const int endstream = skipWhite(doc.bytes, dataStart + int(length));
            if (doc.bytes.mid(endstream, 9) != "endstream") {
                fail(why, QStringLiteral("%1: object %2 /Length does not end at endstream")
                               .arg(doc.file).arg(number));
                return false;
            }
            *body = doc.bytes.mid(bodyStart, endstream + 9 - bodyStart);
            return true;
        }
        *body = doc.bytes.mid(bodyStart, dictEnd - bodyStart);
        return true;
    }

    const int endobj = int(doc.bytes.indexOf("endobj", bodyStart));
    if (endobj < 0) {
        fail(why, QStringLiteral("%1: object %2 has no endobj").arg(doc.file).arg(number));
        return false;
    }
    *body = doc.bytes.mid(bodyStart, endobj - bodyStart).trimmed();
    return true;
}

bool objectStreamFor(const SourceDoc &doc, int number, QString *why)
{
    if (doc.objectStreams.contains(number)) {
        return true;
    }
    const QHash<int, XrefEntry>::const_iterator entry = doc.xref.constFind(number);
    if (entry == doc.xref.constEnd() || entry->type != 1) {
        fail(why, QStringLiteral("%1: object stream %2 is not an uncompressed object")
                       .arg(doc.file).arg(number));
        return false;
    }
    QByteArray body;
    if (!objectBodyAtOffset(doc, entry->first, number, entry->second, &body, why)) {
        return false;
    }
    QByteArray dict;
    QByteArray raw;
    if (!streamParts(doc, body, &dict, &raw, why)) {
        return false;
    }
    if (typeNameOf(dict) != "ObjStm") {
        fail(why, QStringLiteral("%1: object %2 is not a /Type /ObjStm stream")
                       .arg(doc.file).arg(number));
        return false;
    }
    QByteArray decoded;
    if (!decodeStream(dict, raw, &decoded, why)) {
        /// decodeStream()'s own messages name the filter or predictor, not the file; a refusal has
        /// to name the source it refused, so the file is put in front of whatever it said.
        if (why && !why->contains(doc.file)) {
            *why = doc.file + QStringLiteral(": ") + *why;
        }
        return false;
    }
    qint64 count = 0;
    qint64 first = 0;
    if (!dictionaryIntValue(dict, "N", &count) || !dictionaryIntValue(dict, "First", &first)
        || count <= 0 || count > INT_MAX || first < 0 || first > decoded.size()) {
        fail(why, QStringLiteral("%1: object stream %2 has invalid /N or /First")
                       .arg(doc.file).arg(number));
        return false;
    }
    ObjectStream stream;
    stream.decoded = decoded;
    stream.first = int(first);
    int at = 0;
    for (int i = 0; i < int(count); ++i) {
        qint64 objectNumber = 0;
        qint64 objectOffset = 0;
        if (!readIntAt(decoded, &at, &objectNumber) || !readIntAt(decoded, &at, &objectOffset)
            || objectNumber <= 0 || objectNumber > INT_MAX || objectOffset < 0
            || objectOffset > decoded.size() - first) {
            fail(why, QStringLiteral("%1: object stream %2 has an invalid object index")
                           .arg(doc.file).arg(number));
            return false;
        }
        stream.objectNumbers.append(int(objectNumber));
        stream.offsets.append(int(objectOffset));
    }
    if (skipWhite(decoded, at) > first) {
        fail(why, QStringLiteral("%1: object stream %2 has an invalid /First boundary")
                       .arg(doc.file).arg(number));
        return false;
    }
    for (int i = 0; i < stream.offsets.size(); ++i) {
        const int begin = stream.first + stream.offsets.at(i);
        const int end = i + 1 < stream.offsets.size()
            ? stream.first + stream.offsets.at(i + 1) : stream.decoded.size();
        if (begin < stream.first || end < begin || end > stream.decoded.size()) {
            fail(why, QStringLiteral("%1: object stream %2 has unordered object offsets")
                           .arg(doc.file).arg(number));
            return false;
        }
    }
    doc.objectStreams.insert(number, stream);
    return true;
}

bool readObjectBody(const SourceDoc &doc, int number, QByteArray *body, QString *why, int generation)
{
    const QHash<int, QByteArray>::const_iterator cached = doc.packedObjectCache.constFind(number);
    if (cached != doc.packedObjectCache.constEnd()) {
        *body = cached.value();
        return true;
    }
    if (doc.resolving.contains(number)) {
        fail(why, QStringLiteral("%1: object %2 has a cyclic object-stream reference")
                       .arg(doc.file).arg(number));
        return false;
    }
    const QHash<int, XrefEntry>::const_iterator it = doc.xref.constFind(number);
    if (it == doc.xref.constEnd()) {
        fail(why, QStringLiteral("%1: object %2 is not in the cross-reference data")
                       .arg(doc.file).arg(number));
        return false;
    }
    if (generation >= 0 && (it->type == 2 ? generation != 0 : it->second != generation)) {
        fail(why, QStringLiteral("%1: reference to object %2 has generation %3 but current xref generation is %4")
                       .arg(doc.file).arg(number).arg(generation).arg(it->type == 2 ? 0 : it->second));
        return false;
    }
    doc.resolving.insert(number);
    QByteArray result;
    bool ok = false;
    if (it->type == 1 && it->first > 0 && it->first < doc.bytes.size()) {
        ok = objectBodyAtOffset(doc, it->first, number, it->second, &result, why);
    } else if (it->type == 2 && it->first > 0 && it->first <= INT_MAX && it->second >= 0) {
        const int streamNumber = int(it->first);
        if (streamNumber == number || !objectStreamFor(doc, streamNumber, why)) {
            if (streamNumber == number) {
                fail(why, QStringLiteral("%1: object %2 is packed in itself")
                               .arg(doc.file).arg(number));
            }
        } else {
            const ObjectStream &stream = doc.objectStreams[streamNumber];
            int index = it->second;
            if (index >= stream.objectNumbers.size() || stream.objectNumbers.at(index) != number) {
                index = stream.objectNumbers.indexOf(number);
            }
            if (index < 0 || index >= stream.offsets.size()) {
                fail(why, QStringLiteral("%1: object %2 is not listed in object stream %3")
                               .arg(doc.file).arg(number).arg(streamNumber));
            } else {
                const int begin = stream.first + stream.offsets.at(index);
                const int end = index + 1 < stream.offsets.size()
                    ? stream.first + stream.offsets.at(index + 1) : stream.decoded.size();
                result = stream.decoded.mid(begin, end - begin).trimmed();
                ok = !result.isEmpty();
                if (!ok) {
                    fail(why, QStringLiteral("%1: object %2 has an empty packed body")
                                   .arg(doc.file).arg(number));
                }
            }
        }
    } else {
        fail(why, QStringLiteral("%1: object %2 has an unsupported xref entry type %3")
                       .arg(doc.file).arg(number).arg(it->type));
    }
    doc.resolving.remove(number);
    if (!ok) {
        return false;
    }
    if (it->type == 2) {
        doc.packedObjectCache.insert(number, result);
    }
    *body = result;
    return true;
}

struct XrefTail {
    bool hasPrev = false;
    qint64 prev = 0;
    qint64 xrefStm = -1;
    int rootNumber = -1;
    int rootGeneration = 0;
    bool encrypted = false;
};

bool readTailDictionary(const SourceDoc &doc, const QByteArray &dict, XrefTail *tail, QString *why)
{
    int at = 0;
    if (topLevelKey(dict, "Root", &at)) {
        int root = -1;
        int generation = 0;
        if (!valueIsRef(dict, at, &root, &generation)) {
            fail(why, QStringLiteral("%1: the trailer /Root is not an indirect reference").arg(doc.file));
            return false;
        }
        tail->rootNumber = root;
        tail->rootGeneration = generation;
    }
    if (topLevelKey(dict, "Encrypt", &at)) {
        tail->encrypted = true;
    }
    if (topLevelKey(dict, "Prev", &at)) {
        if (!readIntAt(dict, &at, &tail->prev) || tail->prev <= 0) {
            fail(why, QStringLiteral("%1: the trailer /Prev is not a positive offset").arg(doc.file));
            return false;
        }
        tail->hasPrev = true;
    }
    if (topLevelKey(dict, "XRefStm", &at)) {
        if (!readIntAt(dict, &at, &tail->xrefStm) || tail->xrefStm <= 0) {
            fail(why, QStringLiteral("%1: the trailer /XRefStm is not a positive offset").arg(doc.file));
            return false;
        }
    }
    return true;
}

bool readClassicXref(const SourceDoc &doc, qint64 offset, QSet<int> *sectionTableEntries,
                     XrefTail *tail, QString *why)
{
    if (offset < 0 || offset >= doc.bytes.size() || doc.bytes.mid(int(offset), 4) != "xref") {
        fail(why, QStringLiteral("%1: startxref does not point to a classic xref table").arg(doc.file));
        return false;
    }
    int at = skipWhite(doc.bytes, int(offset) + 4);
    QByteArray trailer;
    bool foundTrailer = false;
    while (at < doc.bytes.size()) {
        at = skipWhite(doc.bytes, at);
        if (doc.bytes.mid(at, 7) == "trailer") {
            const int dictAt = skipWhite(doc.bytes, at + 7);
            if (doc.bytes.mid(dictAt, 2) != "<<") {
                fail(why, QStringLiteral("%1: the xref trailer is not a dictionary").arg(doc.file));
                return false;
            }
            const int end = dictEndIndex(doc.bytes, dictAt);
            if (end < 0) {
                fail(why, QStringLiteral("%1: the xref trailer is unreadable").arg(doc.file));
                return false;
            }
            trailer = doc.bytes.mid(dictAt, end - dictAt);
            foundTrailer = true;
            break;
        }
        qint64 first = 0;
        qint64 count = 0;
        if (!readIntAt(doc.bytes, &at, &first) || !readIntAt(doc.bytes, &at, &count)
            || first < 0 || first > INT_MAX || count < 0 || count > 10000000
            || first + count > qint64(INT_MAX) + 1) {
            fail(why, QStringLiteral("%1: the xref table has an invalid subsection header").arg(doc.file));
            return false;
        }
        for (qint64 row = 0; row < count; ++row) {
            qint64 objectOffset = 0;
            qint64 generation = 0;
            if (!readIntAt(doc.bytes, &at, &objectOffset) || !readIntAt(doc.bytes, &at, &generation)) {
                fail(why, QStringLiteral("%1: the xref table has an incomplete entry").arg(doc.file));
                return false;
            }
            at = skipWhite(doc.bytes, at);
            if (at >= doc.bytes.size() || (doc.bytes.at(at) != 'n' && doc.bytes.at(at) != 'f')) {
                fail(why, QStringLiteral("%1: the xref table has an invalid entry type").arg(doc.file));
                return false;
            }
            const char type = doc.bytes.at(at++);
            const qint64 objectNumber = first + row;
            if (generation < 0 || generation > 65535 || objectOffset < 0
                || (type == 'n' && objectOffset >= doc.bytes.size())) {
                fail(why, QStringLiteral("%1: the xref table has an out-of-range entry").arg(doc.file));
                return false;
            }
            const int key = int(objectNumber);
            if (!doc.xref.contains(key)) {
                sectionTableEntries->insert(key);
                if (type == 'n') {
                    doc.xref.insert(key, XrefEntry{1, objectOffset, int(generation)});
                } else {
                    doc.xref.insert(key, XrefEntry{0, 0, int(generation)});
                }
            }
            while (at < doc.bytes.size() && doc.bytes.at(at) != '\n' && doc.bytes.at(at) != '\r') {
                ++at;
            }
            while (at < doc.bytes.size() && (doc.bytes.at(at) == '\n' || doc.bytes.at(at) == '\r')) {
                ++at;
            }
        }
    }
    if (!foundTrailer || !readTailDictionary(doc, trailer, tail, why)) {
        if (!foundTrailer) {
            fail(why, QStringLiteral("%1: the classic xref table has no trailer").arg(doc.file));
        }
        return false;
    }
    return true;
}

bool readXrefStream(const SourceDoc &doc, qint64 offset,
                     const QSet<int> *overrideExistingEntries,
                    XrefTail *tail, QString *why)
{
    QByteArray body;
    if (!objectBodyAtOffset(doc, offset, -1, -1, &body, why)) {
        return false;
    }
    QByteArray dict;
    QByteArray raw;
    if (!streamParts(doc, body, &dict, &raw, why)) {
        return false;
    }
    if (typeNameOf(dict) != "XRef") {
        fail(why, QStringLiteral("%1: the xref offset does not name a /Type /XRef stream").arg(doc.file));
        return false;
    }
    QByteArray decoded;
    if (!decodeStream(dict, raw, &decoded, why)) {
        /// decodeStream()'s own messages name the filter or predictor, not the file; a refusal has
        /// to name the source it refused, so the file is put in front of whatever it said.
        if (why && !why->contains(doc.file)) {
            *why = doc.file + QStringLiteral(": ") + *why;
        }
        return false;
    }
    QList<qint64> widths;
    if (!dictionaryIntArray(dict, "W", &widths) || widths.size() != 3
        || widths.at(0) > 8 || widths.at(1) > 8 || widths.at(2) > 8) {
        fail(why, QStringLiteral("%1: the xref stream has an invalid /W array").arg(doc.file));
        return false;
    }
    const qint64 rowBytes = widths.at(0) + widths.at(1) + widths.at(2);
    if (rowBytes <= 0 || rowBytes > 24) {
        fail(why, QStringLiteral("%1: the xref stream has unusable entry widths").arg(doc.file));
        return false;
    }
    QList<qint64> ranges;
    int indexAt = 0;
    if (topLevelKey(dict, "Index", &indexAt)) {
        if (!dictionaryIntArray(dict, "Index", &ranges) || ranges.size() % 2 != 0) {
            fail(why, QStringLiteral("%1: the xref stream has an invalid /Index array").arg(doc.file));
            return false;
        }
    } else {
        qint64 size = 0;
        if (!dictionaryIntValue(dict, "Size", &size) || size <= 0 || size > INT_MAX) {
            fail(why, QStringLiteral("%1: the xref stream has no usable /Size").arg(doc.file));
            return false;
        }
        ranges << 0 << size;
    }
    qint64 totalEntries = 0;
    for (int i = 0; i < ranges.size(); i += 2) {
        if (ranges.at(i) > INT_MAX || ranges.at(i + 1) > INT_MAX
            || ranges.at(i) + ranges.at(i + 1) > qint64(INT_MAX) + 1) {
            fail(why, QStringLiteral("%1: the xref stream /Index is out of range").arg(doc.file));
            return false;
        }
        totalEntries += ranges.at(i + 1);
    }
    if (totalEntries > decoded.size() / rowBytes) {
        fail(why, QStringLiteral("%1: the xref stream data is shorter than its /Index").arg(doc.file));
        return false;
    }
    int cursor = 0;
    for (int range = 0; range < ranges.size(); range += 2) {
        const int firstObject = int(ranges.at(range));
        const int count = int(ranges.at(range + 1));
        for (int row = 0; row < count; ++row) {
            quint64 fields[3] = {widths.at(0) == 0 ? 1u : 0u, 0u, 0u};
            for (int field = 0; field < 3; ++field) {
                for (int byte = 0; byte < widths.at(field); ++byte) {
                    if (fields[field] > (quint64(LLONG_MAX) >> 8)) {
                        fail(why, QStringLiteral("%1: an xref stream field overflows").arg(doc.file));
                        return false;
                    }
                    fields[field] = (fields[field] << 8) | quint8(decoded.at(cursor++));
                }
            }
            const int type = widths.at(0) == 0 ? 1 : int(fields[0]);
            const qint64 objectNumber = qint64(firstObject) + row;
            if (type < 0 || type > 2 || fields[1] > quint64(LLONG_MAX)
                || fields[2] > quint64(INT_MAX)) {
                fail(why, QStringLiteral("%1: the xref stream contains an unsupported entry").arg(doc.file));
                return false;
            }
            XrefEntry value;
            value.type = type;
            value.first = qint64(fields[1]);
            value.second = int(fields[2]);
            const int key = int(objectNumber);
            const QHash<int, XrefEntry>::const_iterator existing = doc.xref.constFind(key);
            if (existing == doc.xref.constEnd()
                || (overrideExistingEntries && overrideExistingEntries->contains(key))) {
                doc.xref.insert(key, value);
            }
        }
    }
    return readTailDictionary(doc, dict, tail, why);
}

bool readXrefSection(const SourceDoc &doc, qint64 offset, QSet<int> *sectionTableEntries,
                      XrefTail *tail, bool *isStream,
                     QString *why)
{
    if (offset < 0 || offset >= doc.bytes.size()) {
        fail(why, QStringLiteral("%1: startxref points outside the file").arg(doc.file));
        return false;
    }
    const int at = skipWhite(doc.bytes, int(offset));
    if (doc.bytes.mid(at, 4) == "xref") {
        *isStream = false;
        return readClassicXref(doc, at, sectionTableEntries, tail, why);
    }
    *isStream = true;
    return readXrefStream(doc, at, nullptr, tail, why);
}

bool parseSource(const QString &path, const QString &file, SourceDoc *doc, QString *why)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("%1: %2").arg(file, input.errorString()));
        return false;
    }
    doc->bytes = input.readAll();
    doc->file = file;
    const int marker = int(doc->bytes.lastIndexOf("startxref"));
    if (marker < 0) {
        fail(why, QStringLiteral("%1: no startxref").arg(file));
        return false;
    }
    int at = marker + 9;
    qint64 current = 0;
    if (!readIntAt(doc->bytes, &at, &current) || current <= 0 || current >= doc->bytes.size()) {
        fail(why, QStringLiteral("%1: startxref does not name an in-file offset").arg(file));
        return false;
    }
    QSet<qint64> visited;
    bool finished = false;
    for (int sections = 0; sections < 64; ++sections) {
        if (visited.contains(current)) {
            fail(why, QStringLiteral("%1: the xref chain contains a cycle").arg(file));
            return false;
        }
        visited.insert(current);
        XrefTail tail;
        QSet<int> sectionTableEntries;
        bool sectionIsStream = false;
        if (!readXrefSection(*doc, current, &sectionTableEntries, &tail, &sectionIsStream, why)) {
            return false;
        }
        if (doc->rootNumber < 0 && tail.rootNumber >= 0) {
            doc->rootNumber = tail.rootNumber;
            doc->rootGeneration = tail.rootGeneration;
        }
        bool encrypted = tail.encrypted;
        if (tail.xrefStm >= 0) {
            if (visited.contains(tail.xrefStm)) {
                fail(why, QStringLiteral("%1: the hybrid /XRefStm points to an already-read section").arg(file));
                return false;
            }
            visited.insert(tail.xrefStm);
            XrefTail supplement;
            if (!readXrefStream(*doc, tail.xrefStm, &sectionTableEntries, &supplement, why)) {
                return false;
            }
            encrypted = encrypted || supplement.encrypted;
            if (doc->rootNumber < 0 && supplement.rootNumber >= 0) {
                doc->rootNumber = supplement.rootNumber;
                doc->rootGeneration = supplement.rootGeneration;
            }
        }
        if (encrypted) {
            fail(why, QStringLiteral("%1: encrypted sources cannot be assembled").arg(file));
            return false;
        }
        if (!tail.hasPrev) {
            finished = true;
            break;
        }
        current = tail.prev;
        Q_UNUSED(sectionIsStream);
    }
    if (!finished) {
        fail(why, QStringLiteral("%1: the xref chain is too long").arg(file));
        return false;
    }
    if (doc->rootNumber < 0 || !doc->xref.contains(doc->rootNumber)) {
        fail(why, QStringLiteral("%1: the catalog /Root is not in the xref data").arg(file));
        return false;
    }
    const QHash<int, XrefEntry>::const_iterator rootEntry = doc->xref.constFind(doc->rootNumber);
    if (rootEntry->type == 0 || rootEntry->type == 3
        || (rootEntry->type == 1 && rootEntry->second != doc->rootGeneration)
        || (rootEntry->type == 2 && doc->rootGeneration != 0)) {
        fail(why, QStringLiteral("%1: trailer /Root generation does not match the current xref entry")
                       .arg(file));
        return false;
    }
    return true;
}

/// The array or dictionary at \a at, following ONE indirect reference. For /MediaBox, /CropBox,
/// /Rotate and /Resources, one level is all a real file needs, and more than one is refused rather
/// than guessed at.
bool resolveValue(const SourceDoc &doc, const QByteArray &body, int at, QByteArray *text, QString *why, int hops = 0)
{
    if (hops >= 64) {
        fail(why, QStringLiteral("%1: indirect value chain is cyclic or exceeds 64 references").arg(doc.file));
        return false;
    }
    int number = 0;
    int generation = 0;
    if (valueIsRef(body, at, &number, &generation)) {
        QByteArray referenced;
        if (!readObjectBody(doc, number, &referenced, why, generation)) {
            return false;
        }
        const int v = skipWhite(referenced, 0);
        return resolveValue(doc, referenced, v, text, why, hops + 1);
    }

    const int v = skipWhite(body, at);
    if (v >= body.size()) {
        fail(why, QStringLiteral("%1: a value is missing where one is required").arg(doc.file));
        return false;
    }
    const char c = body.at(v);
    if (c == '[') {
        int depth = 0;
        int i = v;
        while (i < body.size()) {
            const char d = body.at(i);
            if (d == '(') {
                i = skipLiteralString(body, i);
                continue;
            }
            if (d == '[') {
                ++depth;
            } else if (d == ']') {
                --depth;
                if (depth == 0) {
                    *text = body.mid(v, i + 1 - v);
                    return true;
                }
            }
            ++i;
        }
        fail(why, QStringLiteral("%1: an array is not closed").arg(doc.file));
        return false;
    }
    if (c == '<' && v + 1 < body.size() && body.at(v + 1) == '<') {
        const int end = dictEndIndex(body, v);
        if (end < 0) {
            fail(why, QStringLiteral("%1: a dictionary is not closed").arg(doc.file));
            return false;
        }
        *text = body.mid(v, end - v);
        return true;
    }
    /// A number or a name: run to the next delimiter.
    int e = v;
    while (e < body.size() && !isDelimiter(body.at(e))) {
        ++e;
    }
    if (e == v) {
        fail(why, QStringLiteral("%1: a value at offset %2 cannot be read").arg(doc.file).arg(v));
        return false;
    }
    *text = body.mid(v, e - v);
    return true;
}

/// Walks the page tree carrying the inherited attributes down. THIS is the rule the user's own
/// files depend on: their pages carry no /Resources and no /MediaBox at all.
bool walkPages(const SourceDoc &doc, int node, int nodeGeneration, const SourcePage &inherited,
               QList<SourcePage> *pages, QString *why, int depth = 0)
{
    if (depth > 64) {
        fail(why, QStringLiteral("%1: the page tree is deeper than 64 levels").arg(doc.file));
        return false;
    }
    QByteArray body;
    if (!readObjectBody(doc, node, &body, why, nodeGeneration)) {
        return false;
    }

    SourcePage here = inherited;
    int valueAt = 0;
    if (topLevelKey(body, "Resources", &valueAt)) {
        QByteArray text;
        if (!resolveValue(doc, body, valueAt, &text, why)) {
            return false;
        }
        here.resources = text;
        here.hasResources = true;
        here.inheritsResources = false;
    }
    if (topLevelKey(body, "MediaBox", &valueAt)) {
        QByteArray text;
        if (!resolveValue(doc, body, valueAt, &text, why)) {
            return false;
        }
        here.mediaBox = text;
        here.hasMediaBox = true;
        here.inheritsMediaBox = false;
    }
    if (topLevelKey(body, "CropBox", &valueAt)) {
        QByteArray text;
        if (!resolveValue(doc, body, valueAt, &text, why)) {
            return false;
        }
        here.cropBox = text;
        here.hasCropBox = true;
    }
    if (topLevelKey(body, "Rotate", &valueAt)) {
        QByteArray text;
        if (!resolveValue(doc, body, valueAt, &text, why)) {
            return false;
        }
        here.rotate = text;
        here.hasRotate = true;
    }

    const QByteArray type = typeNameOf(body);
    const bool isPages = type == "Pages";
    const bool isPage = type == "Page";
    if (isPages) {
        if (!topLevelKey(body, "Kids", &valueAt)) {
            fail(why, QStringLiteral("%1: a /Pages node has no /Kids").arg(doc.file));
            return false;
        }
        QByteArray kids;
        if (!resolveValue(doc, body, valueAt, &kids, why)) {
            return false;
        }
        if (kids.isEmpty() || kids.at(0) != '[') {
            fail(why, QStringLiteral("%1: a /Pages node's /Kids is not an array").arg(doc.file));
            return false;
        }
        int i = 1;
        while (i < kids.size()) {
            i = skipWhite(kids, i);
            if (i >= kids.size() || kids.at(i) == ']') {
                break;
            }
            int child = 0;
            int childGeneration = 0;
            if (!valueIsRef(kids, i, &child, &childGeneration)) {
                fail(why, QStringLiteral("%1: a /Kids entry is not a page reference").arg(doc.file));
                return false;
            }
            SourcePage childInherited = here;
            childInherited.object = child;
            childInherited.inheritsResources = true;
            childInherited.inheritsMediaBox = true;
            if (!walkPages(doc, child, childGeneration, childInherited, pages, why, depth + 1)) {
                return false;
            }
            /// Past the reference.
            int j = i;
            qint64 n = 0;
            qint64 g = 0;
            readIntAt(kids, &j, &n);
            readIntAt(kids, &j, &g);
            j = skipWhite(kids, j);
            i = j + 1;
        }
        return true;
    }

    if (isPage) {
        SourcePage page = here;
        page.object = node;
        pages->append(page);
        return true;
    }

    fail(why, QStringLiteral("%1: object %2 is neither /Page nor /Pages").arg(doc.file).arg(node));
    return false;
}

bool collectSourcePages(const SourceDoc &doc, QList<SourcePage> *pages, QString *why)
{
    /// The /Type test above is textual, so read the catalog properly and start from its /Pages.
    QByteArray rootBody;
    if (!readObjectBody(doc, doc.rootNumber, &rootBody, why, doc.rootGeneration)) {
        return false;
    }
    int valueAt = 0;
    if (!topLevelKey(rootBody, "Pages", &valueAt)) {
        fail(why, QStringLiteral("%1: the /Root catalog has no /Pages").arg(doc.file));
        return false;
    }
    int pagesNode = 0;
    int pagesGeneration = 0;
    if (!valueIsRef(rootBody, valueAt, &pagesNode, &pagesGeneration)) {
        fail(why, QStringLiteral("%1: the catalog's /Pages is not a reference").arg(doc.file));
        return false;
    }
    SourcePage inherited;
    inherited.inheritsResources = true;
    inherited.inheritsMediaBox = true;
    return walkPages(doc, pagesNode, pagesGeneration, inherited, pages, why);
}

/// Copies objects into the output, renumbering every reference. Streams are copied byte for byte:
/// only a stream's DICTIONARY is rewritten, never its data.
class Copier
{
public:
    struct PageReference {
        int selectedCount = 0;
        int outputNumber = 0;
    };

    static quint64 objectKey(int sourceIndex, int sourceNumber)
    {
        return (quint64(quint32(sourceIndex)) << 32) | quint32(sourceNumber);
    }

    Copier(const QHash<int, SourceDoc> *docs, int firstNumber)
        : m_docs(docs)
        , m_next(firstNumber)
    {
    }

    /// A number of our own, for an object this code builds itself -- a page dictionary. The caller
    /// inserts the finished body: its references are already OUTPUT numbers, and running them
    /// through rewriteReferences() again would look for those numbers in a SOURCE.
    int allocate() { return m_next++; }
    void insert(int number, const QByteArray &body) { m_bodies.insert(number, body); }

    void registerSourcePages(int sourceIndex, const QList<SourcePage> &pages)
    {
        for (const SourcePage &page : pages) {
            const quint64 key = objectKey(sourceIndex, page.object);
            if (!m_pageReferences.contains(key)) {
                m_pageReferences.insert(key, PageReference());
            }
        }
    }

    bool selectPage(int sourceIndex, int sourceNumber, int outputNumber)
    {
        auto it = m_pageReferences.find(objectKey(sourceIndex, sourceNumber));
        if (it == m_pageReferences.end()) {
            return false;
        }
        ++it->selectedCount;
        it->outputNumber = it->selectedCount == 1 ? outputNumber : 0;
        return true;
    }

    int selectedPageCount(int sourceIndex, int sourceNumber) const
    {
        const auto it = m_pageReferences.constFind(objectKey(sourceIndex, sourceNumber));
        return it == m_pageReferences.constEnd() ? -1 : it->selectedCount;
    }

    bool isUniqueSelectedPage(int sourceIndex, int sourceNumber) const
    {
        const auto it = m_pageReferences.constFind(objectKey(sourceIndex, sourceNumber));
        return it != m_pageReferences.constEnd() && it->selectedCount == 1 && it->outputNumber > 0;
    }

    /// Copies object a sourceNumber of a sourceIndex. The key is the PAIR: object 5 of the first
    /// source and object 5 of the second are different objects, and numbering them by number alone
    /// is how two sources would collide.
    bool copy(int sourceIndex, int sourceNumber, int sourceGeneration, int *outNumber, QString *why)
    {
        const SourceDoc &doc = m_docs->value(sourceIndex);
        QByteArray validationBody;
        if (!readObjectBody(doc, sourceNumber, &validationBody, why, sourceGeneration)) {
            /// readObjectBody validates the source generation before any output mapping is made.
            return false;
        }
        const quint64 key = objectKey(sourceIndex, sourceNumber);
        const QHash<quint64, PageReference>::const_iterator page = m_pageReferences.constFind(key);
        if (page != m_pageReferences.constEnd()) {
            if (page->selectedCount == 0) {
                fail(why, QStringLiteral("%1: a reference points to source page object %2, which is not included in the notebook")
                              .arg(doc.file).arg(sourceNumber));
                return false;
            }
            if (page->selectedCount != 1 || page->outputNumber <= 0) {
                fail(why, QStringLiteral("%1: a reference points to source page object %2, which occurs more than once in the notebook; refusing ambiguous remapping")
                              .arg(doc.file).arg(sourceNumber));
                return false;
            }
            *outNumber = page->outputNumber;
            return true;
        }
        const QHash<quint64, int>::const_iterator known = m_map.constFind(key);
        if (known != m_map.constEnd()) {
            *outNumber = known.value();
            return true;
        }
        if (!doc.xref.contains(sourceNumber)) {
            fail(why, QStringLiteral("%1: object %2 is referenced but not in the xref table")
                          .arg(doc.file).arg(sourceNumber));
            return false;
        }

        /// The number is mapped before references are rewritten, so graph cycles cannot recurse
        /// forever.
        const int number = m_next++;
        m_map.insert(key, number);
        m_bodies.insert(number, QByteArray());

        QByteArray body = validationBody;
        if (!rewriteReferences(sourceIndex, &body, why)) {
            return false;
        }
        m_bodies.insert(number, body);
        *outNumber = number;
        return true;
    }

    /// A dictionary that lives INLINE inside another object (an inline /Resources, say): it is given
    /// an object of its own in the output so that its references are rewritten with everything else.
    bool copyInline(int sourceIndex, const QByteArray &text, int *outNumber, QString *why)
    {
        const int number = m_next++;
        m_bodies.insert(number, QByteArray());
        QByteArray body = text;
        if (!rewriteReferences(sourceIndex, &body, why)) {
            return false;
        }
        m_bodies.insert(number, body);
        *outNumber = number;
        return true;
    }

    QHash<int, QByteArray> bodies() const { return m_bodies; }
    int nextNumber() const { return m_next; }

private:
    bool rewriteReferences(int sourceIndex, QByteArray *body, QString *why)
    {
        QByteArray head = *body;
        int streamAt = -1;
        if (head.startsWith("<<")) {
            const int dictEnd = dictEndIndex(head, 0);
            if (dictEnd > 0) {
                const int after = skipWhite(head, dictEnd);
                if (head.mid(after, 6) == "stream") {
                    streamAt = after;
                }
            }
        }
        const int limit = streamAt >= 0 ? streamAt : head.size();

        QByteArray rewritten;
        rewritten.reserve(head.size() + 64);
        int i = 0;
        while (i < limit) {
            const char c = head.at(i);
            if (c == '(') {
                const int end = skipLiteralString(head, i);
                rewritten += head.mid(i, end - i);
                i = end;
                continue;
            }
            if (c == '<') {
                /// "<<" opens a DICTIONARY and has to be stepped over as a pair; a single "<" opens
                /// a hex string. Getting this wrong swallows the whole nested dictionary as if it
                /// were a hex string -- which is how the /Font reference inside an inherited
                /// /Resources went uncopied and the page came out blank.
                if (i + 1 < limit && head.at(i + 1) == '<') {
                    rewritten += "<<";
                    i += 2;
                    continue;
                }
                int end = i + 1;
                while (end < limit && head.at(end) != '>') {
                    ++end;
                }
                rewritten += head.mid(i, qMin(end + 1, limit) - i);
                i = end + 1;
                continue;
            }
            if (c == '%') {
                int end = i;
                while (end < limit && head.at(end) != '\n') {
                    ++end;
                }
                rewritten += head.mid(i, end - i);
                i = end;
                continue;
            }
            if (c >= '0' && c <= '9' && (i == 0 || isDelimiter(head.at(i - 1)))) {
                int j = i;
                qint64 n = 0;
                qint64 g = 0;
                if (readIntAt(head, &j, &n) && readIntAt(head, &j, &g)) {
                    const int r = skipWhite(head, j);
                    if (r < limit && head.at(r) == 'R'
                        && (r + 1 >= limit || isDelimiter(head.at(r + 1)))) {
                        int outNumber = 0;
                        if (n <= 0 || n > INT_MAX || g < 0 || g > 65535
                            || !copy(sourceIndex, int(n), int(g), &outNumber, why)) {
                            return false;
                        }
                        rewritten += QByteArray::number(outNumber) + " 0 R";
                        i = r + 1;
                        continue;
                    }
                }
            }
            rewritten += c;
            ++i;
        }

        if (streamAt >= 0) {
            rewritten += head.mid(streamAt);
        }
        *body = rewritten;
        return true;
    }

    const QHash<int, SourceDoc> *m_docs;
    int m_next;
    QHash<quint64, PageReference> m_pageReferences;
    QHash<quint64, int> m_map;
    QHash<int, QByteArray> m_bodies;
};

bool dictionaryNameValue(const QByteArray &dictionary, const QByteArray &key, QByteArray *name)
{
    int valueAt = 0;
    return topLevelKey(dictionary, key, &valueAt) && readPdfName(dictionary, &valueAt, name);
}

bool validateLocalDestination(const SourceDoc &doc, const QByteArray &container, int valueAt,
                              int sourceIndex, const Copier &copier, const QString &prefix,
                              QString *why)
{
    QByteArray destination;
    if (!resolveValue(doc, container, valueAt, &destination, why)) {
        return false;
    }
    if (destination.isEmpty() || destination.at(0) != '[') {
        fail(why, QStringLiteral("%1 uses a named or non-array destination that is not safely remappable")
                      .arg(prefix));
        return false;
    }
    int pageAt = skipWhite(destination, 1);
    int pageObject = 0;
    int generation = 0;
    if (!valueIsRef(destination, pageAt, &pageObject, &generation)) {
        fail(why, QStringLiteral("%1 destination does not start with a page reference")
                      .arg(prefix));
        return false;
    }
    if (!copier.isUniqueSelectedPage(sourceIndex, pageObject)) {
        fail(why, QStringLiteral("%1 destination page is absent from the notebook or occurs more than once; refusing ambiguous remapping")
                      .arg(prefix));
        return false;
    }
    return true;
}

/// Tagged link annotations point into a structure tree and parent tree that this assembler does not
/// carry. The user chose to discard those links rather than block an otherwise valid export.
bool isTaggedLinkAnnotation(const QByteArray &annotation)
{
    QByteArray subtype;
    int structParentAt = 0;
    return dictionaryNameValue(annotation, "Subtype", &subtype) && subtype == "Link"
        && topLevelKey(annotation, "StructParent", &structParentAt);
}

bool validateLinkAnnotation(const SourceDoc &doc, int sourceIndex, int sourcePageObject,
                            const QByteArray &annotation, const Copier &copier,
                            const QString &prefix, QString *why)
{
    QByteArray subtype;
    if (!dictionaryNameValue(annotation, "Subtype", &subtype) || subtype != "Link") {
        fail(why, QStringLiteral("%1 has an annotation subtype other than /Link; refusing partial /Annots remapping")
                      .arg(prefix));
        return false;
    }
    int unsupportedAt = 0;
    if (topLevelKey(annotation, "AA", &unsupportedAt)) {
        fail(why, QStringLiteral("%1 has /AA actions outside the supported local-link subset")
                      .arg(prefix));
        return false;
    }
    if (topLevelKey(annotation, "OC", &unsupportedAt)) {
        fail(why, QStringLiteral("%1 has /OC optional-content data not carried by the assembler")
                      .arg(prefix));
        return false;
    }

    int pageAt = 0;
    if (topLevelKey(annotation, "P", &pageAt)) {
        int pageObject = 0;
        int generation = 0;
        if (!valueIsRef(annotation, pageAt, &pageObject, &generation)
            || pageObject != sourcePageObject
            || !copier.isUniqueSelectedPage(sourceIndex, pageObject)) {
            fail(why, QStringLiteral("%1 /P does not identify one unique notebook page")
                          .arg(prefix));
            return false;
        }
    }

    int destinationAt = 0;
    int actionAt = 0;
    const bool hasDestination = topLevelKey(annotation, "Dest", &destinationAt);
    const bool hasAction = topLevelKey(annotation, "A", &actionAt);
    if (hasDestination == hasAction) {
        fail(why, QStringLiteral("%1 must have exactly one of /Dest or /A to be safely remappable")
                      .arg(prefix));
        return false;
    }
    if (hasDestination) {
        return validateLocalDestination(doc, annotation, destinationAt, sourceIndex, copier, prefix, why);
    }

    QByteArray action;
    if (!resolveValue(doc, annotation, actionAt, &action, why)
        || action.size() < 4 || action.at(0) != '<' || action.at(1) != '<') {
        fail(why, QStringLiteral("%1 /A is not a readable action dictionary")
                      .arg(prefix));
        return false;
    }
    int nextAt = 0;
    if (topLevelKey(action, "Next", &nextAt)) {
        fail(why, QStringLiteral("%1 has a chained /A action that is not safely remappable")
                      .arg(prefix));
        return false;
    }
    QByteArray actionType;
    int destinationInAction = 0;
    if (!dictionaryNameValue(action, "S", &actionType) || actionType != "GoTo"
        || !topLevelKey(action, "D", &destinationInAction)) {
        fail(why, QStringLiteral("%1 action is not a simple local /GoTo destination")
                      .arg(prefix));
        return false;
    }
    return validateLocalDestination(doc, action, destinationInAction, sourceIndex, copier, prefix, why);
}

bool copyPageAnnotations(const SourceDoc &doc, int sourceIndex, int sourcePageObject,
                         const QByteArray &pageBody, int annotsAt, const QString &prefix,
                         bool geometryPreserved, Copier &copier,
                         QSet<quint64> *copiedAnnotationObjects, QByteArray *copiedArray,
                         QString *why)
{
    QByteArray annotations;
    if (!resolveValue(doc, pageBody, annotsAt, &annotations, why)) {
        return false;
    }
    if (annotations.isEmpty() || annotations.at(0) != '[') {
        fail(why, QStringLiteral("%1 /Annots is not an array")
                      .arg(prefix));
        return false;
    }

    struct KeptAnnotation {
        int object;
        int generation;
        QByteArray body;
    };
    QList<KeptAnnotation> kept;
    int droppedTaggedLinks = 0;

    int at = skipWhite(annotations, 1);
    if (at >= annotations.size()) {
        fail(why, QStringLiteral("%1 /Annots array is not closed")
                      .arg(prefix));
        return false;
    }
    while (at < annotations.size()) {
        at = skipWhite(annotations, at);
        if (at >= annotations.size() || annotations.at(at) == ']') {
            break;
        }
        int annotationObject = 0;
        int generation = 0;
        if (!valueIsRef(annotations, at, &annotationObject, &generation)) {
            fail(why, QStringLiteral("%1 contains an /Annots entry that is not an indirect reference")
                          .arg(prefix));
            return false;
        }

        QByteArray annotation;
        if (!readObjectBody(doc, annotationObject, &annotation, why, generation)) {
            return false;
        }
        if (isTaggedLinkAnnotation(annotation)) {
            ++droppedTaggedLinks;
        } else {
            kept.append({annotationObject, generation, annotation});
        }

        int afterReference = at;
        qint64 ignoredNumber = 0;
        qint64 ignoredGeneration = 0;
        readIntAt(annotations, &afterReference, &ignoredNumber);
        readIntAt(annotations, &afterReference, &ignoredGeneration);
        afterReference = skipWhite(annotations, afterReference);
        if (afterReference >= annotations.size() || annotations.at(afterReference) != 'R') {
            fail(why, QStringLiteral("%1 contains a malformed /Annots reference")
                          .arg(prefix));
            return false;
        }
        at = afterReference + 1;
    }

    /// If the only annotations were tagged links, the user's lossy policy leaves no coordinates to
    /// remap, so export remains valid even when the notebook transformed or duplicated the page.
    if (kept.isEmpty()) {
        if (droppedTaggedLinks > 0) {
            qWarning("[pdfio] dropped %d tagged link annotation(s) with /StructParent from %s",
                     droppedTaggedLinks, qPrintable(prefix));
        }
        *copiedArray = "[]";
        return true;
    }
    if (!geometryPreserved) {
        fail(why, QStringLiteral("%1 has link annotations but the notebook transforms the page; annotation coordinates are not yet transformed")
                      .arg(prefix));
        return false;
    }
    if (copier.selectedPageCount(sourceIndex, sourcePageObject) != 1) {
        fail(why, QStringLiteral("%1 has annotations but the source page occurs more than once in the notebook")
                      .arg(prefix));
        return false;
    }

    QByteArray rebuilt("[");
    bool first = true;
    for (const KeptAnnotation &entry : kept) {
        const quint64 key = Copier::objectKey(sourceIndex, entry.object);
        if (copiedAnnotationObjects->contains(key)) {
            fail(why, QStringLiteral("%1 annotation object %2 is shared by pages; refusing to attach it more than once")
                          .arg(prefix).arg(entry.object));
            return false;
        }
        copiedAnnotationObjects->insert(key);

        if (!validateLinkAnnotation(doc, sourceIndex, sourcePageObject, entry.body,
                                    copier, prefix, why)) {
            return false;
        }
        int outputObject = 0;
        if (!copier.copy(sourceIndex, entry.object, entry.generation, &outputObject, why)) {
            return false;
        }
        if (!first) {
            rebuilt += ' ';
        }
        rebuilt += QByteArray::number(outputObject) + " 0 R";
        first = false;
    }
    rebuilt += ']';
    *copiedArray = rebuilt;
    if (droppedTaggedLinks > 0) {
        qWarning("[pdfio] dropped %d tagged link annotation(s) with /StructParent from %s",
                 droppedTaggedLinks, qPrintable(prefix));
    }
    return true;
}

/// Writes a complete classic PDF: a header, the objects in number order, an xref table and a
/// trailer. Every object in it is ours, so no object stream is needed whatever the sources used.
bool writeFile(const QString &outPath, const QHash<int, QByteArray> &bodies, int maxNumber,
               int rootNumber, QString *why)
{
    QByteArray out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    QList<qint64> offsets;
    for (int n = 0; n <= maxNumber; ++n) {
        offsets.append(-1);
    }

    for (int n = 1; n <= maxNumber; ++n) {
        const QHash<int, QByteArray>::const_iterator it = bodies.constFind(n);
        if (it == bodies.constEnd()) {
            continue;
        }
        offsets[n] = out.size();
        out += QByteArray::number(n) + " 0 obj\n" + it.value() + "\nendobj\n";
    }

    const qint64 xrefAt = out.size();
    out += "xref\n0 " + QByteArray::number(maxNumber + 1) + "\n";
    out += "0000000000 65535 f \n";
    for (int n = 1; n <= maxNumber; ++n) {
        char entry[32];
        const qint64 at = offsets.at(n) < 0 ? 0 : offsets.at(n);
        snprintf(entry, sizeof entry, "%010lld 00000 n \n", static_cast<long long>(at));
        out += entry;
    }
    out += "trailer\n<< /Size " + QByteArray::number(maxNumber + 1) + " /Root "
        + QByteArray::number(rootNumber) + " 0 R >>\nstartxref\n" + QByteArray::number(xrefAt)
        + "\n%%EOF\n";

    QSaveFile file(outPath);
    if (!file.open(QIODevice::WriteOnly)) {
        fail(why, QStringLiteral("cannot write %1: %2").arg(outPath, file.errorString()));
        return false;
    }
    if (file.write(out) != out.size()) {
        fail(why, QStringLiteral("%1 was not written completely: %2").arg(outPath, file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        fail(why, QStringLiteral("cannot atomically replace %1: %2").arg(outPath, file.errorString()));
        return false;
    }
    return true;
}

/// The numbers of a /MediaBox array, checked to be four and non-degenerate.
bool mediaBoxNumbers(const QByteArray &text, double *values, QString *why)
{
    if (text.isEmpty() || text.at(0) != '[') {
        fail(why, QStringLiteral("a /MediaBox is not an array"));
        return false;
    }
    int i = 1;
    for (int k = 0; k < 4; ++k) {
        if (!readRealAt(text, &i, &values[k])) {
            fail(why, QStringLiteral("a /MediaBox does not hold four numbers"));
            return false;
        }
    }
    if (qFuzzyCompare(values[2], values[0]) || qFuzzyCompare(values[3], values[1])) {
        fail(why, QStringLiteral("a /MediaBox has no area"));
        return false;
    }
    return true;
}

/// Normalises /Rotate to 0/90/180/270, or refuses an angle PDF cannot name.
bool normaliseRotation(const QByteArray &text, int *degrees, QString *why)
{
    int i = 0;
    qint64 value = 0;
    if (!readIntAt(text, &i, &value)) {
        fail(why, QStringLiteral("a /Rotate is not a number"));
        return false;
    }
    int r = int(value % 360);
    if (r < 0) {
        r += 360;
    }
    if (r % 90 != 0) {
        fail(why, QStringLiteral("a source page is rotated %1 degrees, which PDF /Rotate cannot "
                                 "name").arg(r));
        return false;
    }
    *degrees = r;
    return true;
}

} // namespace

bool PdfAssembler::plan(const QString &projectDir, const PdfSessionManifest &manifest,
                        QList<PagePlan> *pages, QString *why)
{
    if (!manifest.isValid(why)) {
        return false;
    }
    if (pages) {
        pages->clear();
    }

    QHash<int, QList<SourcePage>> bySource;
    QHash<int, QString> sourceNames;
    for (const PdfPageRecord &record : manifest.pages) {
        /// A BLANK page has no source at all: there is no file to read and no page object to copy,
        /// and the assembly writes its sheet itself. A page drawn from a PICTURE is the same in this
        /// respect -- the file is not a PDF and nothing here can parse it.
        if (record.isBlank()) {
            continue;
        }
        if (record.source < 0 || record.source >= manifest.sourceCount()) {
            fail(why, QStringLiteral("notebook page names source %1, which the manifest does not "
                                     "hold").arg(record.source));
            return false;
        }
        if (manifest.sourceAt(record.source).isImage()) {
            continue;
        }
        if (bySource.contains(record.source)) {
            continue;
        }
        const QString file = manifest.sourceAt(record.source).file;
        sourceNames.insert(record.source, file);

        SourceDoc doc;
        const QString path = PdfSession::sourcePath(projectDir, file);
        if (!parseSource(path, file, &doc, why)) {
            return false;
        }
        QList<SourcePage> sourcePages;
        if (!collectSourcePages(doc, &sourcePages, why)) {
            return false;
        }
        bySource.insert(record.source, sourcePages);
    }

    for (const PdfPageRecord &record : manifest.pages) {
        /// The two page kinds that are WRITTEN rather than copied. Both describe a sheet of their own
        /// (sizePt), and both leave the copy of an object behind: a blank page has none to copy, and a
        /// picture is not a page of a PDF.
        if (record.isBlank() || manifest.sourceAt(record.source).isImage()) {
            PagePlan out;
            out.synthesized = true;
            out.sourceIndex = record.isBlank() ? -1 : record.source;
            out.sizePt = record.sizePt;
            /// A picture has no /Rotate of its own and a blank sheet has no file to declare one; the
            /// notebook's own turn is recorded in the manifest and applied by the exporter.
            out.rotation = 0;
            if (pages) {
                pages->append(out);
            }
            continue;
        }

        const QList<SourcePage> &sourcePages = bySource.value(record.source);
        if (record.index < 0 || record.index >= sourcePages.size()) {
            fail(why, QStringLiteral("%1 has %2 page(s), and the notebook names page %3 of it")
                          .arg(sourceNames.value(record.source))
                          .arg(sourcePages.size())
                          .arg(record.index + 1));
            return false;
        }
        const SourcePage &page = sourcePages.at(record.index);
        if (page.object < 0 || !page.hasMediaBox || page.mediaBox.isEmpty()) {
            fail(why, QStringLiteral("%1 page %2 has no /MediaBox, not even on an ancestor")
                          .arg(sourceNames.value(record.source))
                          .arg(record.index + 1));
            return false;
        }

        PagePlan out;
        out.sourceIndex = record.source;
        out.sourcePage = record.index;
        out.pageObject = page.object;
        double box[4] = { 0, 0, 0, 0 };
        if (!mediaBoxNumbers(page.mediaBox, box, why)) {
            fail(why, QStringLiteral("%1 page %2: %3").arg(sourceNames.value(record.source))
                          .arg(record.index + 1).arg(why ? *why : QString()));
            return false;
        }
        out.sizePt = QSizeF(qAbs(box[2] - box[0]), qAbs(box[3] - box[1]));
        if (page.hasRotate && !normaliseRotation(page.rotate, &out.rotation, why)) {
            fail(why, QStringLiteral("%1 page %2: %3").arg(sourceNames.value(record.source))
                          .arg(record.index + 1).arg(why ? *why : QString()));
            return false;
        }
        out.inheritsResources = page.inheritsResources;
        out.inheritsMediaBox = page.inheritsMediaBox;
        if (pages) {
            pages->append(out);
        }
    }
    return true;
}

bool PdfAssembler::assemble(const QString &projectDir, const PdfSessionManifest &manifest,
                            const QString &outPath, QList<PagePlan> *written, QString *why)
{
    QList<PagePlan> plans;
    if (!plan(projectDir, manifest, &plans, why)) {
        return false;
    }

    /// The sources are parsed again for the copy -- plan() answered the page TREE question; this is
    /// the object question, and both need the same reader.
    QHash<int, SourceDoc> docs;
    QHash<int, QList<SourcePage>> sourcePages;
    for (const PdfPageRecord &record : manifest.pages) {
        /// The same two kinds plan() skips: nothing to parse, nothing to copy.
        if (record.isBlank() || manifest.sourceAt(record.source).isImage()) {
            continue;
        }
        if (docs.contains(record.source)) {
            continue;
        }
        SourceDoc doc;
        const QString file = manifest.sourceAt(record.source).file;
        if (!parseSource(PdfSession::sourcePath(projectDir, file), file, &doc, why)) {
            return false;
        }
        QList<SourcePage> pages;
        if (!collectSourcePages(doc, &pages, why)) {
            return false;
        }
        docs.insert(record.source, doc);
        sourcePages.insert(record.source, pages);
    }

    /// Object 1 is the catalog, object 2 the page tree; everything else is copied in.
    const int catalogNumber = 1;
    const int pagesNumber = 2;
    Copier copier(&docs, 3);
    for (auto it = sourcePages.constBegin(); it != sourcePages.constEnd(); ++it) {
        copier.registerSourcePages(it.key(), it.value());
    }

    QList<int> pageNumbers;
    pageNumbers.reserve(plans.size());
    for (const PagePlan &plan : plans) {
        const int pageObject = copier.allocate();
        pageNumbers.append(pageObject);
        copier.insert(pageObject, QByteArray());
        /// A synthesized page has no object in any source to be selected: the dictionary written for
        /// it below IS the page, and its number is already its own.
        if (plan.synthesized) {
            continue;
        }
        if (!copier.selectPage(plan.sourceIndex, plan.pageObject, pageObject)) {
            fail(why, QStringLiteral("notebook page %1 does not map to a source page object")
                          .arg(pageNumbers.size()));
            return false;
        }
    }
    QSet<quint64> copiedAnnotationObjects;

    for (int i = 0; i < plans.size(); ++i) {
        const PagePlan &plan = plans.at(i);
        const PdfPageRecord &record = manifest.pages.at(i);

        /// A page the assembly writes itself: a sheet of the page's own size, with the picture drawn
        /// over it when there is one, and nothing else -- no /Resources to copy, no /Contents to
        /// remap, no annotations, no /Group.
        if (plan.synthesized) {
            const QSizeF sheet = plan.sizePt;
            QByteArray dict = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "
                + QByteArray::number(sheet.width(), 'f', 3) + " "
                + QByteArray::number(sheet.height(), 'f', 3) + "]";

            const bool picture = plan.sourceIndex >= 0
                && manifest.sourceAt(plan.sourceIndex).isImage();
            if (picture) {
                const QString file = manifest.sourceAt(plan.sourceIndex).file;
                const QImage loaded(PdfSession::sourcePath(projectDir, file));
                if (loaded.isNull() || loaded.width() <= 0 || loaded.height() <= 0) {
                    fail(why, QStringLiteral("notebook page %1 draws on %2, which cannot be read as "
                                             "a picture").arg(i + 1).arg(file));
                    return false;
                }

                /// The pixels are written at the picture's own size and placed over the whole sheet:
                /// one pixel to one point is the size a picture page is given when it is brought in,
                /// and the notebook's Scale and Box are the exporter's business, applied after this.
                const QByteArray samples = rgbSamplesForPdf(loaded);
                const int imageObject = copier.allocate();
                QByteArray image =
                    "<< /Type /XObject /Subtype /Image /Width " + QByteArray::number(loaded.width())
                    + " /Height " + QByteArray::number(loaded.height())
                    + " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length "
                    + QByteArray::number(deflateForPdf(samples).size()) + " >>\nstream\n";
                image += deflateForPdf(samples);
                image += "\nendstream";
                copier.insert(imageObject, image);

                const QByteArray content =
                    "q " + QByteArray::number(sheet.width(), 'f', 3) + " 0 0 "
                    + QByteArray::number(sheet.height(), 'f', 3) + " 0 0 cm /pdfioPicture Do Q\n";
                const int contentObject = copier.allocate();
                copier.insert(contentObject,
                              "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n"
                                  + content + "endstream");

                dict += " /Resources << /XObject << /pdfioPicture " + QByteArray::number(imageObject)
                    + " 0 R >> >> /Contents " + QByteArray::number(contentObject) + " 0 R";
            }

            dict += " >>";
            copier.insert(pageNumbers.at(i), dict);
            continue;
        }

        if (record.source != plan.sourceIndex || record.index != plan.sourcePage) {
            /// plan() and the copy walk the manifest in the same order; if they ever disagree the
            /// page written would not be the page named, so it is refused rather than written.
            fail(why, QStringLiteral("notebook page %1 was planned from %2 page %3 but names "
                                     "source %4 page %5")
                          .arg(i + 1)
                          .arg(plan.sourceIndex)
                          .arg(plan.sourcePage + 1)
                          .arg(record.source)
                          .arg(record.index + 1));
            return false;
        }
        const SourceDoc &doc = docs.value(plan.sourceIndex);
        /// QHash::value() hands back a temporary QList; bind the LIST (whose lifetime is extended)
        /// and take the element from that, or the SourcePage& would dangle past this line.
        const QList<SourcePage> &sourceList = sourcePages.value(plan.sourceIndex);
        const SourcePage &source = sourceList.at(plan.sourcePage);
        const QByteArray prefix = QStringLiteral("%1 page %2: ")
                                      .arg(manifest.sourceAt(plan.sourceIndex).file)
                                      .arg(plan.sourcePage + 1)
                                      .toUtf8();

        QByteArray dict = "<< /Type /Page /Parent 2 0 R /MediaBox " + source.mediaBox;
        if (source.hasCropBox && !source.cropBox.isEmpty()) {
            dict += " /CropBox " + source.cropBox;
        }
        if (plan.rotation != 0) {
            dict += " /Rotate " + QByteArray::number(plan.rotation);
        }

        if (source.hasResources && !source.resources.isEmpty()) {
            int resourcesObject = 0;
            if (source.resources.startsWith("<<")) {
                if (!copier.copyInline(plan.sourceIndex, source.resources, &resourcesObject, why)) {
                    return false;
                }
            } else {
                int referenced = 0;
                int generation = 0;
                if (!valueIsRef(source.resources, 0, &referenced, &generation)) {
                    fail(why, QStringLiteral("%1 the /Resources value cannot be copied").arg(QString::fromUtf8(prefix)));
                    return false;
                }
                if (!copier.copy(plan.sourceIndex, referenced, generation, &resourcesObject, why)) {
                    return false;
                }
            }
            dict += " /Resources " + QByteArray::number(resourcesObject) + " 0 R";
        }

        /// /Contents: one stream, or an array of them. Anything else is refused rather than guessed.
        QByteArray pageBody;
        if (!readObjectBody(doc, source.object, &pageBody, why)) {
            return false;
        }
        {
            int annotsAt = 0;
            if (topLevelKey(pageBody, "Annots", &annotsAt)) {
                QByteArray annotations;
                if (!copyPageAnnotations(doc, plan.sourceIndex, source.object, pageBody, annotsAt,
                                         QString::fromUtf8(prefix), record.rotation == plan.rotation
                                             && record.extraRotation == 0
                                             && record.extraScale == 1.0
                                             && !record.boxPt.isValid(),
                                         copier, &copiedAnnotationObjects, &annotations, why)) {
                    return false;
                }
                dict += " /Annots " + annotations;
            }
        }
        {
            int valueAt = 0;
            if (topLevelKey(pageBody, "Contents", &valueAt)) {
                int referenced = 0;
                int generation = 0;
                if (valueIsRef(pageBody, valueAt, &referenced, &generation)) {
                    int out = 0;
                    if (!copier.copy(plan.sourceIndex, referenced, generation, &out, why)) {
                        return false;
                    }
                    dict += " /Contents " + QByteArray::number(out) + " 0 R";
                } else {
                    QByteArray array;
                    if (!resolveValue(doc, pageBody, valueAt, &array, why)) {
                        return false;
                    }
                    if (array.isEmpty() || array.at(0) != '[') {
                        fail(why, QStringLiteral("%1 the /Contents is neither a stream nor an array")
                                      .arg(QString::fromUtf8(prefix)));
                        return false;
                    }
                    QByteArray rebuilt = "[";
                    int k = 1;
                    bool first = true;
                    while (k < array.size()) {
                        k = skipWhite(array, k);
                        if (k >= array.size() || array.at(k) == ']') {
                            break;
                        }
                        int item = 0;
                        int generation = 0;
                        if (!valueIsRef(array, k, &item, &generation)) {
                            fail(why, QStringLiteral("%1 a /Contents array entry is not a stream "
                                                     "reference").arg(QString::fromUtf8(prefix)));
                            return false;
                        }
                        int out = 0;
                        if (!copier.copy(plan.sourceIndex, item, generation, &out, why)) {
                            return false;
                        }
                        if (!first) {
                            rebuilt += " ";
                        }
                        rebuilt += QByteArray::number(out) + " 0 R";
                        first = false;
                        int j = k;
                        qint64 n = 0;
                        qint64 g = 0;
                        readIntAt(array, &j, &n);
                        readIntAt(array, &j, &g);
                        j = skipWhite(array, j);
                        k = j + 1;
                    }
                    rebuilt += "]";
                    dict += " /Contents " + rebuilt;
                }
            }
        }

        int groupAt = 0;
        if (topLevelKey(pageBody, "Group", &groupAt)) {
            int groupRef = 0;
            int generation = 0;
            int groupObject = 0;
            if (valueIsRef(pageBody, groupAt, &groupRef, &generation)) {
                if (!copier.copy(plan.sourceIndex, groupRef, generation, &groupObject, why)) {
                    return false;
                }
            } else {
                QByteArray groupText;
                if (!resolveValue(doc, pageBody, groupAt, &groupText, why)) {
                    return false;
                }
                if (!copier.copyInline(plan.sourceIndex, groupText, &groupObject, why)) {
                    return false;
                }
            }
            dict += " /Group " + QByteArray::number(groupObject) + " 0 R";
        }

        dict += " >>";
        /// The page dictionary is an object of OUR making: every reference in it is already an
        /// OUTPUT number (the parent, copied resources, contents, annotations), so it is inserted
        /// as it stands. Sending it back through the reference rewriter would look those numbers up
        /// in a source and copy the wrong objects.
        copier.insert(pageNumbers.at(i), dict);
    }

    QByteArray kids = "[";
    for (int i = 0; i < pageNumbers.size(); ++i) {
        if (i > 0) {
            kids += " ";
        }
        kids += QByteArray::number(pageNumbers.at(i)) + " 0 R";
    }
    kids += "]";

    QHash<int, QByteArray> bodies = copier.bodies();
    bodies.insert(catalogNumber, "<< /Type /Catalog /Pages " + QByteArray::number(pagesNumber) + " 0 R >>");
    bodies.insert(pagesNumber, "<< /Type /Pages /Kids " + kids + " /Count "
                                   + QByteArray::number(pageNumbers.size()) + " >>");

    int maxNumber = 0;
    for (auto it = bodies.constBegin(); it != bodies.constEnd(); ++it) {
        maxNumber = qMax(maxNumber, it.key());
    }
    if (!writeFile(outPath, bodies, maxNumber, catalogNumber, why)) {
        return false;
    }

    if (written) {
        *written = plans;
    }
    return true;
}
