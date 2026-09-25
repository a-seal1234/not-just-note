/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfExporter.h"

#include "session/PdfNotebookOps.h"

#include <QDebug>

#include <QFile>
#include <QHash>
#include <QSet>
#include <QTransform>

namespace {

/// A QByteArray, so that NL + "text" concatenates instead of doing pointer arithmetic.
const QByteArray NL("\n", 1);

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

// ---------------------------------------------------------------------------------------------
// A small, self-contained PDF reader.
//
// The writer is an incremental update, so it has to read exactly three things out of the source:
// where the page objects are, what the page dictionaries say, and where the previous xref lives.
// That is small enough to do here, in pure C++, with no renderer dependency -- which is the point,
// since the same code runs on Android where there is no Poppler.
//
// It understands both cross reference forms: the classic "xref" table and the PDF 1.5
// cross reference *stream* (/Type /XRef), plus objects packed into object streams (/Type /ObjStm)
// and the FlateDecode + PNG/TIFF predictors those streams usually carry. Anything it cannot read
// is refused with a precise message instead of being written out as a file that only looks right.
// ---------------------------------------------------------------------------------------------

bool isWhite(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
}

bool isDelimiter(char c)
{
    return isWhite(c) || c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']'
        || c == '{' || c == '}' || c == '/' || c == '%';
}

int skipWhite(const QByteArray &bytes, int i)
{
    while (i < bytes.size() && isWhite(bytes.at(i))) {
        ++i;
    }
    return i;
}

bool readInt(const QByteArray &bytes, int *i, qint64 *value)
{
    int p = skipWhite(bytes, *i);
    const int start = p;
    if (p < bytes.size() && (bytes.at(p) == '+' || bytes.at(p) == '-')) {
        ++p;
    }
    const int digits = p;
    while (p < bytes.size() && bytes.at(p) >= '0' && bytes.at(p) <= '9') {
        ++p;
    }
    if (p == digits) {
        return false;
    }
    *value = bytes.mid(start, p - start).toLongLong();
    *i = p;
    return true;
}

/// Index just past the ">>" closing the dictionary that starts at \a start. Handles nested
/// dictionaries, literal strings (with escapes and nested parens), hex strings and comments, so
/// that a ">>" inside a string does not end the dictionary early.
int dictEndIndex(const QByteArray &bytes, int start)
{
    if (start < 0 || bytes.mid(start, 2) != "<<") {
        return -1;
    }
    int depth = 0;
    int i = start;
    while (i < bytes.size()) {
        const char c = bytes.at(i);
        if (c == '(') {
            ++i;
            int parens = 1;
            while (i < bytes.size() && parens > 0) {
                const char d = bytes.at(i);
                if (d == '\\') {
                    i += 2;
                    continue;
                }
                if (d == '(') {
                    ++parens;
                } else if (d == ')') {
                    --parens;
                }
                ++i;
            }
            continue;
        }
        if (c == '<') {
            if (i + 1 < bytes.size() && bytes.at(i + 1) == '<') {
                ++depth;
                i += 2;
                continue;
            }
            ++i;
            while (i < bytes.size() && bytes.at(i) != '>') {
                ++i;
            }
            if (i < bytes.size()) {
                ++i;
            }
            continue;
        }
        if (c == '>' && i + 1 < bytes.size() && bytes.at(i + 1) == '>') {
            --depth;
            i += 2;
            if (depth == 0) {
                return i;
            }
            continue;
        }
        if (c == '%') {
            while (i < bytes.size() && bytes.at(i) != '\n' && bytes.at(i) != '\r') {
                ++i;
            }
            continue;
        }
        ++i;
    }
    return -1;
}

/// Index just past the "]" closing the array that starts at \a start. Like dictEndIndex it walks
/// nested arrays and dictionaries, literal strings (with escapes and nested parens), hex strings and
/// comments, so a "]" inside a string does not end the array early.
int arrayEndIndex(const QByteArray &bytes, int start)
{
    if (start < 0 || start >= bytes.size() || bytes.at(start) != '[') {
        return -1;
    }
    int depth = 0;
    int dictDepth = 0;
    int i = start;
    while (i < bytes.size()) {
        const char c = bytes.at(i);
        if (c == '(') {
            ++i;
            int parens = 1;
            while (i < bytes.size() && parens > 0) {
                const char d = bytes.at(i);
                if (d == '\\') {
                    i += 2;
                    continue;
                }
                if (d == '(') {
                    ++parens;
                } else if (d == ')') {
                    --parens;
                }
                ++i;
            }
            continue;
        }
        if (c == '<') {
            if (i + 1 < bytes.size() && bytes.at(i + 1) == '<') {
                ++dictDepth;
                i += 2;
                continue;
            }
            ++i;
            while (i < bytes.size() && bytes.at(i) != '>') {
                ++i;
            }
            if (i < bytes.size()) {
                ++i;
            }
            continue;
        }
        if (c == '>' && i + 1 < bytes.size() && bytes.at(i + 1) == '>') {
            if (dictDepth > 0) {
                --dictDepth;
            }
            i += 2;
            continue;
        }
        if (c == '[') {
            ++depth;
            ++i;
            continue;
        }
        if (c == ']') {
            --depth;
            ++i;
            if (depth == 0) {
                return i;
            }
            continue;
        }
        if (c == '%') {
            while (i < bytes.size() && bytes.at(i) != '\n' && bytes.at(i) != '\r') {
                ++i;
            }
            continue;
        }
        ++i;
    }
    return -1;
}

/// Index of the value that follows "/Key" inside the dictionary [dictStart, dictEnd), or -1.
/// The key has to end on a delimiter, so "/Size" never matches inside "/SizeX".
int dictValueAt(const QByteArray &bytes, int dictStart, int dictEnd, const char *key)
{
    const QByteArray needle = QByteArray("/") + key;
    int at = dictStart;
    while (at >= 0 && at < dictEnd) {
        at = bytes.indexOf(needle, at);
        if (at < 0 || at >= dictEnd) {
            return -1;
        }
        const int after = at + needle.size();
        if (after < bytes.size() && !isDelimiter(bytes.at(after))) {
            at = after;
            continue;
        }
        return after;
    }
    return -1;
}

bool dictIntValue(const QByteArray &bytes, int dictStart, int dictEnd, const char *key, qint64 *value)
{
    const int v = dictValueAt(bytes, dictStart, dictEnd, key);
    if (v < 0) {
        return false;
    }
    int i = v;
    return readInt(bytes, &i, value);
}

bool dictRefValue(const QByteArray &bytes, int dictStart, int dictEnd, const char *key,
                  int *number, int *generation)
{
    const int v = dictValueAt(bytes, dictStart, dictEnd, key);
    if (v < 0) {
        return false;
    }
    int i = v;
    qint64 n = 0;
    qint64 g = 0;
    if (!readInt(bytes, &i, &n) || !readInt(bytes, &i, &g)) {
        return false;
    }
    i = skipWhite(bytes, i);
    if (i >= bytes.size() || bytes.at(i) != 'R') {
        return false;
    }
    *number = int(n);
    *generation = int(g);
    return true;
}

/// A slice of a dictionary-valued value: [start, end) with start/end inside \a bytes.
struct DictSlice {
    int start = -1;
    int end = -1;
    bool valid() const { return start >= 0 && end > start; }
};

/// The slice of "Key << ... >>" if the value is a dictionary, or of the n-th dictionary when the
/// value is an array of dictionaries. Used for /DecodeParms.
bool dictDictValue(const QByteArray &bytes, int dictStart, int dictEnd, const char *key, int index,
                   DictSlice *out)
{
    const int v = dictValueAt(bytes, dictStart, dictEnd, key);
    if (v < 0) {
        return false;
    }
    int i = skipWhite(bytes, v);
    if (bytes.mid(i, 2) == "<<") {
        if (index > 0) {
            return false;
        }
        const int e = dictEndIndex(bytes, i);
        if (e < 0) {
            return false;
        }
        out->start = i;
        out->end = e;
        return true;
    }
    if (i < bytes.size() && bytes.at(i) == '[') {
        ++i;
        int seen = 0;
        while (i < bytes.size() && bytes.at(i) != ']') {
            i = skipWhite(bytes, i);
            if (bytes.mid(i, 2) == "<<") {
                const int e = dictEndIndex(bytes, i);
                if (e < 0) {
                    return false;
                }
                if (seen == index) {
                    out->start = i;
                    out->end = e;
                    return true;
                }
                ++seen;
                i = e;
                continue;
            }
            ++i;
        }
    }
    return false;
}

/// Names of a value that is a name or an array of names. An empty list means the key is absent;
/// an unreadable value yields an empty list too and the caller refuses later if that matters.
QList<QByteArray> dictNames(const QByteArray &bytes, int dictStart, int dictEnd, const char *key)
{
    QList<QByteArray> names;
    const int v = dictValueAt(bytes, dictStart, dictEnd, key);
    if (v < 0) {
        return names;
    }
    auto readName = [&bytes](int *i, QByteArray *name) {
        *i = skipWhite(bytes, *i);
        if (*i >= bytes.size() || bytes.at(*i) != '/') {
            return false;
        }
        int s = *i + 1;
        int e = s;
        while (e < bytes.size() && !isDelimiter(bytes.at(e))) {
            ++e;
        }
        *name = bytes.mid(s, e - s);
        *i = e;
        return true;
    };

    int i = skipWhite(bytes, v);
    if (i < bytes.size() && bytes.at(i) == '[') {
        ++i;
        while (i < bytes.size() && bytes.at(i) != ']') {
            QByteArray name;
            if (!readName(&i, &name)) {
                ++i;
                continue;
            }
            names.append(name);
        }
        return names;
    }
    QByteArray name;
    if (readName(&i, &name)) {
        names.append(name);
    }
    return names;
}

bool dictInts(const QByteArray &bytes, int dictStart, int dictEnd, const char *key, QList<qint64> *out)
{
    const int v = dictValueAt(bytes, dictStart, dictEnd, key);
    if (v < 0) {
        return false;
    }
    int i = skipWhite(bytes, v);
    if (i >= bytes.size() || bytes.at(i) != '[') {
        qint64 single = 0;
        if (readInt(bytes, &i, &single)) {
            out->append(single);
            return true;
        }
        return false;
    }
    ++i;
    while (i < bytes.size() && bytes.at(i) != ']') {
        i = skipWhite(bytes, i);
        if (i < bytes.size() && bytes.at(i) == ']') {
            break;
        }
        qint64 n = 0;
        if (!readInt(bytes, &i, &n)) {
            ++i;
            continue;
        }
        out->append(n);
    }
    return !out->isEmpty();
}

/// Indirect references of an array value, generation included.
QList<QPair<int, int>> dictRefs(const QByteArray &bytes, int dictStart, int dictEnd, const char *key)
{
    QList<QPair<int, int>> refs;
    const int v = dictValueAt(bytes, dictStart, dictEnd, key);
    if (v < 0) {
        return refs;
    }
    int i = skipWhite(bytes, v);
    if (i >= bytes.size() || bytes.at(i) != '[') {
        return refs;
    }
    ++i;
    while (i < bytes.size() && bytes.at(i) != ']') {
        i = skipWhite(bytes, i);
        if (i < bytes.size() && bytes.at(i) == ']') {
            break;
        }
        const int save = i;
        qint64 n = 0;
        qint64 g = 0;
        if (readInt(bytes, &i, &n) && readInt(bytes, &i, &g)) {
            const int p = skipWhite(bytes, i);
            if (p < bytes.size() && bytes.at(p) == 'R') {
                refs.append(QPair<int, int>(int(n), int(g)));
                i = p + 1;
                continue;
            }
        }
        i = save + 1;
    }
    return refs;
}

// ---------------------------------------------------------------------------------------------
// Stream decoding: FlateDecode with the PNG and TIFF predictors real producers use.
// ---------------------------------------------------------------------------------------------

int paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = qAbs(p - a);
    const int pb = qAbs(p - b);
    const int pc = qAbs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

QByteArray applyPredictor(const QByteArray &data, int predictor, int colors, int bits, int columns,
                          QString *why)
{
    if (predictor <= 1) {
        return data;
    }
    if (columns <= 0 || colors <= 0 || bits <= 0) {
        fail(why, QStringLiteral("a stream declares an unusable /DecodeParms predictor"));
        return {};
    }

    const int bpp = qMax(1, (colors * bits) / 8);
    const int rowBytes = (colors * bits * columns + 7) / 8;

    if (predictor == 2) {
        if (bits != 8) {
            fail(why, QStringLiteral("a stream uses the TIFF predictor with %1 bits per "
                                     "component, which the exporter does not implement")
                          .arg(bits));
            return {};
        }
        QByteArray out = data;
        const int rows = out.size() / rowBytes;
        for (int r = 0; r < rows; ++r) {
            char *row = out.data() + r * rowBytes;
            for (int j = bpp; j < rowBytes; ++j) {
                row[j] = char((static_cast<unsigned char>(row[j])
                               + static_cast<unsigned char>(row[j - bpp])) & 0xff);
            }
        }
        return out;
    }

    if (predictor < 10) {
        fail(why, QStringLiteral("a stream declares the unknown predictor %1").arg(predictor));
        return {};
    }

    if (rowBytes <= 0 || data.size() % (rowBytes + 1) != 0) {
        fail(why, QStringLiteral("a PNG-predicted stream does not divide into %1 byte rows")
                      .arg(rowBytes));
        return {};
    }

    QByteArray out;
    out.reserve(data.size());
    QByteArray previous(rowBytes, '\0');
    int i = 0;
    while (i < data.size()) {
        const int filter = static_cast<unsigned char>(data.at(i));
        ++i;
        QByteArray row = data.mid(i, rowBytes);
        i += rowBytes;
        for (int j = 0; j < rowBytes; ++j) {
            const int raw = static_cast<unsigned char>(row.at(j));
            const int left = j >= bpp ? static_cast<unsigned char>(row.at(j - bpp)) : 0;
            const int up = static_cast<unsigned char>(previous.at(j));
            const int upLeft = j >= bpp ? static_cast<unsigned char>(previous.at(j - bpp)) : 0;
            int value = 0;
            switch (filter) {
            case 0:
                value = raw;
                break;
            case 1:
                value = raw + left;
                break;
            case 2:
                value = raw + up;
                break;
            case 3:
                value = raw + ((left + up) / 2);
                break;
            case 4:
                value = raw + paeth(left, up, upLeft);
                break;
            default:
                fail(why, QStringLiteral("a PNG-predicted stream uses the unknown filter type %1")
                              .arg(filter));
                return {};
            }
            row[j] = char(value & 0xff);
        }
        out += row;
        previous = row;
    }
    return out;
}

bool decodeStream(const QByteArray &bytes, int dictStart, int dictEnd, const QByteArray &raw,
                  QByteArray *out, QString *why)
{
    const QList<QByteArray> filters = dictNames(bytes, dictStart, dictEnd, "Filter");
    if (filters.isEmpty()) {
        *out = raw;
        return true;
    }

    QByteArray data = raw;
    for (int fi = 0; fi < filters.size(); ++fi) {
        const QByteArray filter = filters.at(fi);
        if (filter != "FlateDecode" && filter != "Fl") {
            fail(why, QStringLiteral("the PDF uses the unsupported stream filter /%1")
                          .arg(QString::fromLatin1(filter)));
            return false;
        }
        if (data.isEmpty()) {
            *out = QByteArray();
            return true;
        }
        /// qUncompress wants the four byte length header qCompress writes. Qt inflates into a
        /// correctly sized buffer regardless of the number in the header, so a zero header is a
        /// safe placeholder -- verified against the Qt this plugin builds with.
        const QByteArray inflated = qUncompress(QByteArray(4, '\0') + data);
        if (inflated.isNull()) {
            fail(why, QStringLiteral("a FlateDecode stream in the PDF could not be inflated"));
            return false;
        }
        data = inflated;

        DictSlice parms;
        if (dictDictValue(bytes, dictStart, dictEnd, "DecodeParms", fi, &parms)) {
            qint64 predictor = 1;
            qint64 colors = 1;
            qint64 bits = 8;
            qint64 columns = 1;
            dictIntValue(bytes, parms.start, parms.end, "Predictor", &predictor);
            dictIntValue(bytes, parms.start, parms.end, "Colors", &colors);
            dictIntValue(bytes, parms.start, parms.end, "BitsPerComponent", &bits);
            const bool haveColumns = dictIntValue(bytes, parms.start, parms.end, "Columns", &columns);
            if (predictor > 1 && !haveColumns) {
                fail(why, QStringLiteral("a predicted stream has no /Columns"));
                return false;
            }
            data = applyPredictor(data, int(predictor), int(colors), int(bits), int(columns), why);
            if (data.isNull()) {
                return false;
            }
        }
    }
    *out = data;
    return true;
}

/// A stream's raw bytes plus the extent of the dictionary that describes it.
struct StreamSlice {
    int dictStart = -1;
    int dictEnd = -1;
    QByteArray raw;
};

/// Reads the indirect object at \a offset and reports its stream, if it is a stream object.
bool streamAt(const QByteArray &bytes, qint64 offset, StreamSlice *slice, int *objectEnd)
{
    if (offset < 0 || offset >= bytes.size()) {
        return false;
    }
    int i = int(offset);
    qint64 number = 0;
    qint64 generation = 0;
    if (!readInt(bytes, &i, &number) || !readInt(bytes, &i, &generation)) {
        return false;
    }
    i = skipWhite(bytes, i);
    if (bytes.mid(i, 3) != "obj") {
        return false;
    }
    const int dictStart = skipWhite(bytes, i + 3);
    if (bytes.mid(dictStart, 2) != "<<") {
        return false;
    }
    const int dictEnd = dictEndIndex(bytes, dictStart);
    if (dictEnd < 0) {
        return false;
    }
    int s = skipWhite(bytes, dictEnd);
    if (bytes.mid(s, 6) != "stream") {
        return false;
    }
    int dataStart = s + 6;
    if (dataStart < bytes.size() && bytes.at(dataStart) == '\r') {
        ++dataStart;
    }
    if (dataStart < bytes.size() && bytes.at(dataStart) == '\n') {
        ++dataStart;
    }

    int dataEnd = -1;
    qint64 length = -1;
    if (dictIntValue(bytes, dictStart, dictEnd, "Length", &length)
        && length >= 0 && dataStart + length <= bytes.size()) {
        dataEnd = int(dataStart + length);
    }
    if (dataEnd < 0) {
        /// /Length may be an indirect reference; fall back to the endstream keyword.
        dataEnd = int(bytes.indexOf("endstream", dataStart));
        if (dataEnd < 0) {
            return false;
        }
        if (dataEnd > dataStart && bytes.at(dataEnd - 1) == '\n') {
            --dataEnd;
        }
        if (dataEnd > dataStart && bytes.at(dataEnd - 1) == '\r') {
            --dataEnd;
        }
    }

    int after = skipWhite(bytes, dataEnd);
    if (bytes.mid(after, 9) != "endstream") {
        const int at = int(bytes.indexOf("endstream", dataEnd));
        if (at < 0) {
            return false;
        }
        after = at;
    }
    after = skipWhite(bytes, after + 9);
    *objectEnd = bytes.mid(after, 6) == "endobj" ? after + 6 : -1;

    slice->dictStart = dictStart;
    slice->dictEnd = dictEnd;
    slice->raw = bytes.mid(dataStart, dataEnd - dataStart);
    return true;
}

/// Body of the indirect object at \a offset: its dictionary and stream if it has one, and for an
/// object that is neither -- the array a page's /Contents may point at, for instance -- the plain
/// value. The body never includes the trailing "endobj".
bool objectBodyAt(const QByteArray &bytes, qint64 offset, QByteArray *body)
{
    if (offset < 0 || offset >= bytes.size()) {
        return false;
    }
    int i = int(offset);
    qint64 number = 0;
    qint64 generation = 0;
    if (!readInt(bytes, &i, &number) || !readInt(bytes, &i, &generation)) {
        return false;
    }
    i = skipWhite(bytes, i);
    if (bytes.mid(i, 3) != "obj") {
        return false;
    }
    const int bodyStart = skipWhite(bytes, i + 3);

    /// Only a dictionary can start a stream, so the stream test is worth doing before the plain
    /// object below -- and an object whose body is not a dictionary at all is still readable: an
    /// array object holds the content streams of a page that writes its /Contents indirectly, and
    /// refusing to read it is what used to leave that page with no background in the export.
    if (bytes.mid(bodyStart, 2) == "<<") {
        const int dictEnd = dictEndIndex(bytes, bodyStart);
        if (dictEnd < 0) {
            return false;
        }

        const int afterDict = skipWhite(bytes, dictEnd);
        if (bytes.mid(afterDict, 6) == "stream") {
            StreamSlice slice;
            int objectEnd = -1;
            if (!streamAt(bytes, offset, &slice, &objectEnd) || objectEnd < 0) {
                return false;
            }
            *body = bytes.mid(bodyStart, objectEnd - 6 - bodyStart);
            return true;
        }

        const int endobj = int(bytes.indexOf("endobj", dictEnd));
        if (endobj < 0) {
            return false;
        }
        *body = bytes.mid(bodyStart, endobj - bodyStart);
        return true;
    }

    const int endobj = int(bytes.indexOf("endobj", bodyStart));
    if (endobj < 0 || endobj == bodyStart) {
        return false;
    }
    *body = bytes.mid(bodyStart, endobj - bodyStart);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Cross reference chain and object access.
// ---------------------------------------------------------------------------------------------

struct XrefEntry {
    int type = 0;     ///< 0 unused, 1 uncompressed, 2 inside an object stream
    qint64 first = 0; ///< type 1: byte offset; type 2: object stream number
    int second = 0;   ///< type 1: generation; type 2: index inside the object stream
};

struct ObjectStream {
    QByteArray data;
    int first = 0;
    QList<int> numbers;
    QList<int> offsets;
};

struct XrefTail {
    bool hasPrev = false;
    qint64 prev = 0;
    qint64 xrefStm = -1;
    int root = -1;
    int rootGeneration = 0;
    bool hasRoot = false;
    bool encrypted = false;
    qint64 size = -1;
    bool hasInfo = false;
    int infoNumber = 0;
    int infoGeneration = 0;
    bool hasId = false;
    QByteArray id;
};

struct PdfDocument {
    QByteArray bytes;
    QHash<int, XrefEntry> xref;
    QHash<int, QByteArray> cache;
    QHash<int, ObjectStream> objectStreams;
    QSet<int> resolving;
    int size = 0;
    int rootNumber = -1;
    int rootGeneration = 0;
    bool encrypted = false;
    bool hasInfo = false;
    int infoNumber = 0;
    int infoGeneration = 0;
    bool hasId = false;
    QByteArray id;
    qint64 newestXrefOffset = 0;
};

void tailFromDict(PdfDocument *doc, const QByteArray &bytes, int dictStart, int dictEnd,
                  XrefTail *tail)
{
    int number = 0;
    int generation = 0;
    if (dictRefValue(bytes, dictStart, dictEnd, "Root", &number, &generation)) {
        tail->root = number;
        tail->rootGeneration = generation;
        tail->hasRoot = true;
    }
    qint64 size = -1;
    if (dictIntValue(bytes, dictStart, dictEnd, "Size", &size)) {
        tail->size = size;
    }
    qint64 prev = 0;
    if (dictIntValue(bytes, dictStart, dictEnd, "Prev", &prev)) {
        tail->hasPrev = true;
        tail->prev = prev;
    }
    qint64 xrefStm = 0;
    if (dictIntValue(bytes, dictStart, dictEnd, "XRefStm", &xrefStm)) {
        tail->xrefStm = xrefStm;
    }
    if (dictValueAt(bytes, dictStart, dictEnd, "Encrypt") >= 0) {
        tail->encrypted = true;
    }
    if (dictRefValue(bytes, dictStart, dictEnd, "Info", &number, &generation)) {
        tail->hasInfo = true;
        tail->infoNumber = number;
        tail->infoGeneration = generation;
    }
    const int idAt = dictValueAt(bytes, dictStart, dictEnd, "ID");
    if (idAt >= 0) {
        int i = skipWhite(bytes, idAt);
        if (i < bytes.size() && bytes.at(i) == '[') {
            const int close = int(bytes.indexOf(']', i));
            if (close > i) {
                tail->hasId = true;
                tail->id = bytes.mid(i, close - i + 1);
            }
        }
    }
    Q_UNUSED(doc);
}

bool readClassicXref(PdfDocument *doc, int offset, XrefTail *tail, QString *why)
{
    const QByteArray &bytes = doc->bytes;
    int i = offset + 4; ///< past "xref"
    while (true) {
        i = skipWhite(bytes, i);
        if (i >= bytes.size()) {
            fail(why, QStringLiteral("a classic xref table ends without a trailer"));
            return false;
        }
        if (bytes.mid(i, 7) == "trailer") {
            break;
        }
        qint64 start = 0;
        qint64 count = 0;
        if (!readInt(bytes, &i, &start) || !readInt(bytes, &i, &count) || count < 0) {
            fail(why, QStringLiteral("a classic xref subsection header is unreadable"));
            return false;
        }
        for (qint64 k = 0; k < count; ++k) {
            i = skipWhite(bytes, i);
            if (i + 18 > bytes.size()) {
                fail(why, QStringLiteral("a classic xref table is truncated"));
                return false;
            }
            const qint64 entryOffset = bytes.mid(i, 10).trimmed().toLongLong();
            i += 10;
            i = skipWhite(bytes, i);
            const qint64 generation = bytes.mid(i, 5).trimmed().toLongLong();
            i += 5;
            i = skipWhite(bytes, i);
            const char type = bytes.at(i);
            ++i;
            const int number = int(start + k);
            if (type == 'n' && !doc->xref.contains(number)) {
                doc->xref.insert(number, XrefEntry{1, entryOffset, int(generation)});
            }
        }
    }

    const int dictStart = skipWhite(bytes, i + 7);
    const int dictEnd = dictEndIndex(bytes, dictStart);
    if (dictEnd < 0) {
        fail(why, QStringLiteral("the xref trailer is not a dictionary"));
        return false;
    }
    tailFromDict(doc, bytes, dictStart, dictEnd, tail);
    return true;
}

bool readXrefStream(PdfDocument *doc, int offset, XrefTail *tail, QString *why)
{
    const QByteArray &bytes = doc->bytes;
    StreamSlice slice;
    int objectEnd = -1;
    if (!streamAt(bytes, offset, &slice, &objectEnd)) {
        fail(why, QStringLiteral("the offset %1 in startxref points at neither a cross reference "
                                 "table nor a cross reference stream")
                      .arg(offset));
        return false;
    }
    if (!bytes.mid(slice.dictStart, slice.dictEnd - slice.dictStart).contains("/XRef")) {
        fail(why, QStringLiteral("the object at the startxref offset is not an /XRef stream"));
        return false;
    }

    QByteArray data;
    if (!decodeStream(bytes, slice.dictStart, slice.dictEnd, slice.raw, &data, why)) {
        return false;
    }

    QList<qint64> widths;
    if (!dictInts(bytes, slice.dictStart, slice.dictEnd, "W", &widths) || widths.isEmpty()) {
        fail(why, QStringLiteral("a cross reference stream has no readable /W"));
        return false;
    }
    while (widths.size() < 3) {
        widths.append(0);
    }
    const int w0 = int(widths.at(0));
    const int w1 = int(widths.at(1));
    const int w2 = int(widths.at(2));
    const int rowBytes = w0 + w1 + w2;
    if (rowBytes <= 0 || rowBytes > 64) {
        fail(why, QStringLiteral("a cross reference stream declares an unusable /W"));
        return false;
    }

    QList<qint64> index;
    if (!dictInts(bytes, slice.dictStart, slice.dictEnd, "Index", &index)) {
        qint64 size = 0;
        dictIntValue(bytes, slice.dictStart, slice.dictEnd, "Size", &size);
        index = {0, size};
    }

    int position = 0;
    for (int p = 0; p + 1 < index.size(); p += 2) {
        const int start = int(index.at(p));
        const qint64 count = index.at(p + 1);
        for (qint64 k = 0; k < count; ++k) {
            if (position + rowBytes > data.size()) {
                fail(why, QStringLiteral("a cross reference stream is shorter than its /W and "
                                         "/Index describe"));
                return false;
            }
            qint64 fields[3] = {1, 0, 0};
            const int fieldWidths[3] = {w0, w1, w2};
            for (int f = 0; f < 3; ++f) {
                qint64 value = 0;
                for (int byte = 0; byte < fieldWidths[f]; ++byte) {
                    value = (value << 8) | static_cast<unsigned char>(data.at(position++));
                }
                fields[f] = value;
            }
            const int type = w0 > 0 ? int(fields[0]) : 1;
            const int number = start + int(k);
            if (!doc->xref.contains(number)) {
                if (type == 1) {
                    doc->xref.insert(number, XrefEntry{1, fields[1], int(fields[2])});
                } else if (type == 2) {
                    doc->xref.insert(number, XrefEntry{2, fields[1], int(fields[2])});
                }
            }
        }
    }

    tailFromDict(doc, bytes, slice.dictStart, slice.dictEnd, tail);
    return true;
}

bool readXrefSection(PdfDocument *doc, qint64 offset, XrefTail *tail, QString *why)
{
    const QByteArray &bytes = doc->bytes;
    if (offset < 0 || offset >= bytes.size()) {
        fail(why, QStringLiteral("a cross reference offset lies outside the PDF"));
        return false;
    }
    const int i = skipWhite(bytes, int(offset));
    if (bytes.mid(i, 4) == "xref") {
        return readClassicXref(doc, i, tail, why);
    }
    return readXrefStream(doc, int(offset), tail, why);
}

bool parseDocument(const QByteArray &pdf, PdfDocument *doc, QString *why)
{
    doc->bytes = pdf;

    const int startxrefAt = int(pdf.lastIndexOf("startxref"));
    if (startxrefAt < 0) {
        fail(why, QStringLiteral("the PDF has no startxref"));
        return false;
    }
    const QString tailText = QString::fromLatin1(pdf.mid(startxrefAt + 9, 64)).simplified();
    bool ok = false;
    const qint64 offset = tailText.section(QLatin1Char(' '), 0, 0).toLongLong(&ok);
    if (!ok) {
        fail(why, QStringLiteral("the startxref offset is not a number"));
        return false;
    }
    doc->newestXrefOffset = offset;

    qint64 current = offset;
    QSet<qint64> seen;
    for (int guard = 0; guard < 64; ++guard) {
        if (current <= 0 || current >= pdf.size() || seen.contains(current)) {
            break;
        }
        seen.insert(current);
        XrefTail tail;
        if (!readXrefSection(doc, current, &tail, why)) {
            return false;
        }
        if (!doc->encrypted && tail.encrypted) {
            doc->encrypted = true;
        }
        if (doc->rootNumber < 0 && tail.hasRoot) {
            doc->rootNumber = tail.root;
            doc->rootGeneration = tail.rootGeneration;
        }
        if (!doc->hasInfo && tail.hasInfo) {
            doc->hasInfo = true;
            doc->infoNumber = tail.infoNumber;
            doc->infoGeneration = tail.infoGeneration;
        }
        if (!doc->hasId && tail.hasId) {
            doc->hasId = true;
            doc->id = tail.id;
        }
        if (tail.size > doc->size) {
            doc->size = int(tail.size);
        }
        if (tail.xrefStm >= 0 && !seen.contains(tail.xrefStm)) {
            seen.insert(tail.xrefStm);
            XrefTail hybrid;
            if (!readXrefSection(doc, tail.xrefStm, &hybrid, why)) {
                return false;
            }
            if (hybrid.encrypted) {
                doc->encrypted = true;
            }
            if (hybrid.size > doc->size) {
                doc->size = int(hybrid.size);
            }
        }
        if (!tail.hasPrev) {
            break;
        }
        current = tail.prev;
    }

    /// /Encrypt is checked before anything is decoded: an encrypted file's object streams look
    /// like garbage, and a precise refusal beats a corrupt export.
    if (doc->encrypted) {
        fail(why, QStringLiteral("the PDF is encrypted, so its pages cannot be read and the "
                                 "notebook cannot be overlaid on it"));
        return false;
    }
    if (doc->xref.isEmpty()) {
        fail(why, QStringLiteral("no cross reference entries were found in the PDF"));
        return false;
    }
    if (doc->rootNumber < 0) {
        fail(why, QStringLiteral("the trailer has no /Root"));
        return false;
    }
    for (auto it = doc->xref.constBegin(); it != doc->xref.constEnd(); ++it) {
        if (it.key() + 1 > doc->size) {
            doc->size = it.key() + 1;
        }
    }
    return true;
}

bool objectByNumber(PdfDocument *doc, int number, QByteArray *body, QString *why);

QByteArray objectStreamData(PdfDocument *doc, int objectStreamNumber, QString *why)
{
    if (doc->objectStreams.contains(objectStreamNumber)) {
        return doc->objectStreams.value(objectStreamNumber).data;
    }
    QByteArray body;
    if (!objectByNumber(doc, objectStreamNumber, &body, why)) {
        return QByteArray();
    }
    const int dictStart = skipWhite(body, 0);
    const int dictEnd = dictEndIndex(body, dictStart);
    if (dictEnd < 0) {
        fail(why, QStringLiteral("object %1 is not an object stream").arg(objectStreamNumber));
        return QByteArray();
    }
    const int afterDict = skipWhite(body, dictEnd);
    if (body.mid(afterDict, 6) != "stream") {
        fail(why, QStringLiteral("object %1 is not a stream, so its objects cannot be read")
                      .arg(objectStreamNumber));
        return QByteArray();
    }

    /// \a body starts at the dictionary already (objectBodyAt stripped the "N 0 obj" header),
    /// so the stream is sliced here rather than through streamAt(), which expects a header.
    int dataStart = afterDict + 6;
    if (dataStart < body.size() && body.at(dataStart) == '\r') {
        ++dataStart;
    }
    if (dataStart < body.size() && body.at(dataStart) == '\n') {
        ++dataStart;
    }
    int dataEnd = -1;
    qint64 length = -1;
    if (dictIntValue(body, dictStart, dictEnd, "Length", &length)
        && length >= 0 && dataStart + length <= body.size()) {
        dataEnd = int(dataStart + length);
    }
    if (dataEnd < 0) {
        dataEnd = int(body.indexOf("endstream", dataStart));
        if (dataEnd < 0) {
            fail(why, QStringLiteral("object stream %1 has no endstream").arg(objectStreamNumber));
            return QByteArray();
        }
        if (dataEnd > dataStart && body.at(dataEnd - 1) == '\n') {
            --dataEnd;
        }
        if (dataEnd > dataStart && body.at(dataEnd - 1) == '\r') {
            --dataEnd;
        }
    }

    const StreamSlice slice{dictStart, dictEnd, body.mid(dataStart, dataEnd - dataStart)};
    QByteArray decoded;
    if (!decodeStream(body, slice.dictStart, slice.dictEnd, slice.raw, &decoded, why)) {
        return QByteArray();
    }

    ObjectStream stream;
    stream.data = decoded;
    qint64 first = 0;
    qint64 count = 0;
    dictIntValue(body, slice.dictStart, slice.dictEnd, "First", &first);
    dictIntValue(body, slice.dictStart, slice.dictEnd, "N", &count);
    stream.first = int(first);
    if (stream.first < 0 || stream.first > decoded.size() || count < 0) {
        fail(why, QStringLiteral("object stream %1 has no usable /First and /N")
                      .arg(objectStreamNumber));
        return QByteArray();
    }
    int i = 0;
    for (qint64 k = 0; k < count; ++k) {
        qint64 objectNumber = 0;
        qint64 relative = 0;
        if (!readInt(decoded, &i, &objectNumber) || !readInt(decoded, &i, &relative)) {
            fail(why, QStringLiteral("object stream %1 has a truncated header")
                          .arg(objectStreamNumber));
            return QByteArray();
        }
        if (i > stream.first) {
            fail(why, QStringLiteral("object stream %1 declares /First inside its own header")
                          .arg(objectStreamNumber));
            return QByteArray();
        }
        stream.numbers.append(int(objectNumber));
        stream.offsets.append(int(relative));
    }
    doc->objectStreams.insert(objectStreamNumber, stream);
    return stream.data;
}

bool objectByNumber(PdfDocument *doc, int number, QByteArray *body, QString *why)
{
    if (doc->cache.contains(number)) {
        *body = doc->cache.value(number);
        return true;
    }
    const XrefEntry entry = doc->xref.value(number, XrefEntry{});
    if (entry.type == 0) {
        fail(why, QStringLiteral("object %1 is missing from the cross reference").arg(number));
        return false;
    }
    if (doc->resolving.contains(number)) {
        fail(why, QStringLiteral("the PDF's objects form a reference cycle at object %1").arg(number));
        return false;
    }
    doc->resolving.insert(number);

    QByteArray result;
    if (entry.type == 1) {
        if (!objectBodyAt(doc->bytes, entry.first, &result)) {
            fail(why, QStringLiteral("object %1 (at byte %2) could not be read")
                          .arg(number).arg(entry.first));
            doc->resolving.remove(number);
            return false;
        }
    } else if (entry.type == 2) {
        const QByteArray data = objectStreamData(doc, int(entry.first), why);
        if (data.isNull()) {
            doc->resolving.remove(number);
            return false;
        }
        const ObjectStream &stream = doc->objectStreams.value(int(entry.first));
        int slot = entry.second;
        if (slot < 0 || slot >= stream.numbers.size() || stream.numbers.at(slot) != number) {
            slot = stream.numbers.indexOf(number);
        }
        if (slot < 0) {
            fail(why, QStringLiteral("object %1 is not inside object stream %2")
                          .arg(number).arg(entry.first));
            doc->resolving.remove(number);
            return false;
        }
        const int from = stream.first + stream.offsets.at(slot);
        const int to = slot + 1 < stream.offsets.size()
            ? stream.first + stream.offsets.at(slot + 1)
            : data.size();
        if (from < 0 || to > data.size() || to < from) {
            fail(why, QStringLiteral("object %1 inside object stream %2 has a bad offset")
                          .arg(number).arg(entry.first));
            doc->resolving.remove(number);
            return false;
        }
        result = data.mid(from, to - from);
    } else {
        fail(why, QStringLiteral("object %1 has an unknown cross reference type").arg(number));
        doc->resolving.remove(number);
        return false;
    }

    doc->cache.insert(number, result);
    doc->resolving.remove(number);
    *body = result;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Pages and inheritance.
// ---------------------------------------------------------------------------------------------

struct PageEntry {
    int number = -1;
    int generation = 0;
    QList<int> ancestors; ///< parent, grandparent, ... for /MediaBox, /Rotate and /Resources
};

bool collectPages(PdfDocument *doc, QList<PageEntry> *pages, QString *why)
{
    QByteArray catalog;
    if (!objectByNumber(doc, doc->rootNumber, &catalog, why)) {
        return false;
    }
    const int catalogStart = skipWhite(catalog, 0);
    const int catalogEnd = dictEndIndex(catalog, catalogStart);
    int pagesNumber = -1;
    int pagesGeneration = 0;
    if (catalogEnd < 0
        || !dictRefValue(catalog, catalogStart, catalogEnd, "Pages", &pagesNumber,
                         &pagesGeneration)) {
        fail(why, QStringLiteral("the catalog has no /Pages"));
        return false;
    }

    QList<QPair<int, QList<int>>> pending;
    pending.append(QPair<int, QList<int>>(pagesNumber, QList<int>()));
    QSet<int> visitedInternal;
    for (int guard = 0; !pending.isEmpty() && guard < 100000; ++guard) {
        const QPair<int, QList<int>> node = pending.takeFirst();
        QByteArray body;
        if (!objectByNumber(doc, node.first, &body, why)) {
            return false;
        }
        const int dictStart = skipWhite(body, 0);
        const int dictEnd = dictEndIndex(body, dictStart);
        if (dictEnd < 0) {
            fail(why, QStringLiteral("object %1 in the page tree is not a dictionary")
                          .arg(node.first));
            return false;
        }
        if (dictValueAt(body, dictStart, dictEnd, "Kids") >= 0) {
            if (visitedInternal.contains(node.first)) {
                continue;
            }
            visitedInternal.insert(node.first);
            QList<QPair<int, int>> kids = dictRefs(body, dictStart, dictEnd, "Kids");
            if (kids.isEmpty()) {
                fail(why, QStringLiteral("a /Pages node in the page tree has no readable /Kids"));
                return false;
            }
            QList<int> lineage = node.second;
            lineage.prepend(node.first);
            for (const QPair<int, int> &kid : kids) {
                pending.append(QPair<int, QList<int>>(kid.first, lineage));
            }
            continue;
        }
        PageEntry page;
        page.number = node.first;
        page.ancestors = node.second;
        page.generation = 0;
        pages->append(page);
    }

    if (pages->isEmpty()) {
        fail(why, QStringLiteral("no pages were found in the page tree"));
        return false;
    }
    return true;
}

/// The value of \a key on the page or, failing that, on its nearest ancestor that has it.
bool inheritedValue(PdfDocument *doc, const PageEntry &page, const char *key, QByteArray *body,
                    int *dictStart, int *dictEnd, int *valueAt, QString *why)
{
    QList<int> candidates;
    candidates.append(page.number);
    candidates += page.ancestors;
    for (int number : candidates) {
        QByteArray candidate;
        if (!objectByNumber(doc, number, &candidate, why)) {
            return false;
        }
        const int start = skipWhite(candidate, 0);
        const int end = dictEndIndex(candidate, start);
        if (end < 0) {
            continue;
        }
        const int value = dictValueAt(candidate, start, end, key);
        if (value < 0) {
            continue;
        }
        *body = candidate;
        *dictStart = start;
        *dictEnd = end;
        *valueAt = value;
        return true;
    }
    return false;
}

bool effectiveRect(PdfDocument *doc, const PageEntry &page, QString *why,
                   double *x0, double *y0, double *x1, double *y1)
{
    QByteArray body;
    int dictStart = 0;
    int dictEnd = 0;
    int value = 0;
    if (!inheritedValue(doc, page, "MediaBox", &body, &dictStart, &dictEnd, &value, why)) {
        fail(why, QStringLiteral("page %1 has no /MediaBox, not even an inherited one")
                      .arg(page.number));
        return false;
    }
    int i = skipWhite(body, value);
    if (i >= body.size() || body.at(i) != '[') {
        fail(why, QStringLiteral("page %1 has an unreadable /MediaBox").arg(page.number));
        return false;
    }
    const int close = int(body.indexOf(']', i));
    if (close < 0) {
        fail(why, QStringLiteral("page %1 has an unreadable /MediaBox").arg(page.number));
        return false;
    }
    const QByteArray slice = body.mid(i + 1, close - i - 1);
    const QStringList parts = QString::fromLatin1(slice).simplified().split(QLatin1Char(' '));
    if (parts.size() != 4) {
        fail(why, QStringLiteral("page %1 has a /MediaBox that is not a rectangle")
                      .arg(page.number));
        return false;
    }

    /// The four numbers are held apart and compared as numbers. The check used to read
    /// "if (!(x1 > x0) || !(y1 > y0))" on the out-parameters themselves, which are double
    /// *pointers*: it compared their addresses, a question whose answer depends on how the
    /// platform lays the arguments out. x86-64 put x1 above x0 and accepted every box; the
    /// tablet's arm64 build put it the other way, so every readable /MediaBox came back "empty or
    /// inverted" and the export silently fell back to the notebook's displayed size -- the whole
    /// reason this task exists, and invisible to every desktop test by construction.
    const double values[4] = {parts.at(0).toDouble(), parts.at(1).toDouble(),
                              parts.at(2).toDouble(), parts.at(3).toDouble()};
    if (!(values[2] > values[0]) || !(values[3] > values[1])) {
        fail(why, QStringLiteral("page %1 has an empty or inverted /MediaBox").arg(page.number));
        return false;
    }
    *x0 = values[0];
    *y0 = values[1];
    *x1 = values[2];
    *y1 = values[3];
    return true;
}

bool effectiveRotation(PdfDocument *doc, const PageEntry &page, QString *why, int *rotation)
{
    *rotation = 0;
    QByteArray body;
    int dictStart = 0;
    int dictEnd = 0;
    int value = 0;
    if (!inheritedValue(doc, page, "Rotate", &body, &dictStart, &dictEnd, &value, why)) {
        return true;
    }
    int i = value;
    qint64 raw = 0;
    if (!readInt(body, &i, &raw)) {
        fail(why, QStringLiteral("page %1 has an unreadable /Rotate").arg(page.number));
        return false;
    }
    int normalized = int(((raw % 360) + 360) % 360);
    if (normalized % 90 != 0) {
        fail(why, QStringLiteral("page %1 has /Rotate %2, which is not a multiple of 90")
                      .arg(page.number).arg(raw));
        return false;
    }
    *rotation = normalized;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Writing.
// ---------------------------------------------------------------------------------------------

/// FlateDecode wants raw zlib; qCompress prepends a four byte length that PDF does not expect.
QByteArray deflate(const QByteArray &data)
{
    return qCompress(data, 9).mid(4);
}

/// The ink as it has to sit in the page's own user space, which is not the space the user drew
/// in once /Rotate is not zero.
QImage inkInPageSpace(const QImage &displayInk, int rotation)
{
    const QImage source = displayInk.convertToFormat(QImage::Format_ARGB32);
    if (rotation == 0) {
        return source;
    }

    /// /Rotate turns the page clockwise for display, so the drawn image has to come back the
    /// other way to line up with the page's own coordinates.
    QTransform transform;
    transform.rotate(-rotation);
    return source.transformed(transform);
}

QByteArray rgbSamples(const QImage &image)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    QByteArray samples;
    samples.reserve(rgb.width() * rgb.height() * 3);
    for (int y = 0; y < rgb.height(); ++y) {
        samples.append(reinterpret_cast<const char *>(rgb.constScanLine(y)), rgb.width() * 3);
    }
    return samples;
}

QByteArray alphaSamples(const QImage &image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    QByteArray samples;
    samples.reserve(argb.width() * argb.height());
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            samples.append(char(qAlpha(line[x])));
        }
    }
    return samples;
}

QByteArray imageObject(int width, int height, const QByteArray &colorSpace, const QByteArray &body,
                       bool withMask, int maskNumber)
{
    QByteArray object = "<< /Type /XObject /Subtype /Image /Width ";
    object += QByteArray::number(width) + " /Height " + QByteArray::number(height);
    object += " /ColorSpace /";
    object += colorSpace;
    object += " /BitsPerComponent 8 /Filter /FlateDecode";
    if (withMask) {
        object += " /SMask " + QByteArray::number(maskNumber) + " 0 R";
    }
    object += " /Length " + QByteArray::number(body.size()) + " >>" + NL + "stream" + NL;
    object += body;
    object += NL + "endstream";
    return object;
}

struct WrittenObject {
    int number = 0;
    int generation = 0;
    QByteArray body;
};

/// Inserts "/pdfioInk N 0 R" into the /XObject sub-dictionary of \a resources, creating the
/// sub-dictionary if needed and never adding a duplicate /XObject key (a second key would hide
/// the page's original images from every reader that keeps the last one).
bool mergeInkXObject(PdfDocument *doc, const QByteArray &resources, int imageNumber,
                     int *nextObject, QList<WrittenObject> *appended, QByteArray *merged,
                     QString *why)
{
    const int dictStart = skipWhite(resources, 0);
    const int dictEnd = dictEndIndex(resources, dictStart);
    if (dictEnd < 0) {
        fail(why, QStringLiteral("a page's /Resources is not a dictionary"));
        return false;
    }
    const QByteArray entry = "/pdfioInk " + QByteArray::number(imageNumber) + " 0 R";
    const int at = dictValueAt(resources, dictStart, dictEnd, "XObject");
    if (at < 0) {
        *merged = resources.left(dictStart + 2) + " /XObject << " + entry + " >>"
                  + resources.mid(dictStart + 2);
        return true;
    }

    const int value = skipWhite(resources, at);
    if (resources.mid(value, 2) == "<<") {
        const int xEnd = dictEndIndex(resources, value);
        if (xEnd < 0) {
            fail(why, QStringLiteral("a page's /XObject is not a readable dictionary"));
            return false;
        }
        *merged = resources.left(value + 2) + " " + entry + " " + resources.mid(value + 2);
        return true;
    }

    int number = 0;
    int generation = 0;
    if (!dictRefValue(resources, dictStart, dictEnd, "XObject", &number, &generation)) {
        fail(why, QStringLiteral("a page's /XObject is neither a dictionary nor a reference"));
        return false;
    }
    QByteArray sub;
    if (!objectByNumber(doc, number, &sub, why)) {
        return false;
    }
    const int subStart = skipWhite(sub, 0);
    const int subEnd = dictEndIndex(sub, subStart);
    if (subEnd < 0) {
        fail(why, QStringLiteral("the object a page's /XObject points at is not a dictionary"));
        return false;
    }
    QByteArray replacement = sub.left(subStart + 2) + " " + entry + " " + sub.mid(subStart + 2);
    const int newNumber = (*nextObject)++;
    appended->append(WrittenObject{newNumber, 0, replacement});

    int i = skipWhite(resources, at);
    qint64 refNumber = 0;
    qint64 refGeneration = 0;
    if (!readInt(resources, &i, &refNumber) || !readInt(resources, &i, &refGeneration)) {
        fail(why, QStringLiteral("a page's /XObject reference is unreadable"));
        return false;
    }
    i = skipWhite(resources, i);
    if (i >= resources.size() || resources.at(i) != 'R') {
        fail(why, QStringLiteral("a page's /XObject reference is unreadable"));
        return false;
    }
    *merged = resources.left(at) + " " + QByteArray::number(newNumber) + " 0 R "
              + resources.mid(i + 1);
    return true;
}

/// Slice of the /Resources dictionary of a page, wherever it actually lives.
struct ResourcesSlice {
    QByteArray dict;
    bool inlineInPage = false; ///< true when the dictionary is inside the page object itself
    int valueAt = -1;          ///< index of the /Resources value in the page body, when inline
    int valueEnd = -1;
};

bool findResources(PdfDocument *doc, const PageEntry &page, const QByteArray &pageBody,
                   ResourcesSlice *out, QString *why)
{
    const int pageStart = skipWhite(pageBody, 0);
    const int pageEnd = dictEndIndex(pageBody, pageStart);
    if (pageEnd < 0) {
        fail(why, QStringLiteral("page %1 is not a dictionary").arg(page.number));
        return false;
    }

    QList<int> candidates;
    candidates.append(page.number);
    candidates += page.ancestors;
    for (int candidate : candidates) {
        const QByteArray body = candidate == page.number ? pageBody : QByteArray();
        QByteArray owner = body;
        if (owner.isEmpty()) {
            if (!objectByNumber(doc, candidate, &owner, why)) {
                return false;
            }
        }
        const int start = skipWhite(owner, 0);
        const int end = dictEndIndex(owner, start);
        if (end < 0) {
            continue;
        }
        const int value = dictValueAt(owner, start, end, "Resources");
        if (value < 0) {
            continue;
        }
        const int p = skipWhite(owner, value);
        if (owner.mid(p, 2) == "<<") {
            const int e = dictEndIndex(owner, p);
            if (e < 0) {
                fail(why, QStringLiteral("page %1 has an unreadable /Resources")
                              .arg(page.number));
                return false;
            }
            out->dict = owner.mid(p, e - p);
            out->inlineInPage = candidate == page.number;
            if (out->inlineInPage) {
                out->valueAt = p;
                out->valueEnd = e;
            }
            return true;
        }
        int number = 0;
        int generation = 0;
        if (!dictRefValue(owner, start, end, "Resources", &number, &generation)) {
            fail(why, QStringLiteral("page %1 has a /Resources that is neither a dictionary nor "
                                     "a reference")
                          .arg(page.number));
            return false;
        }
        QByteArray resolved;
        if (!objectByNumber(doc, number, &resolved, why)) {
            return false;
        }
        const int rs = skipWhite(resolved, 0);
        const int re = dictEndIndex(resolved, rs);
        if (re < 0) {
            fail(why, QStringLiteral("the /Resources object %1 of page %2 is not a dictionary")
                          .arg(number).arg(page.number));
            return false;
        }
        out->dict = resolved.mid(rs, re - rs);
        out->inlineInPage = false;
        return true;
    }

    /// No /Resources anywhere: a page can legitimately have none. Synthesize one.
    out->dict = "<< >>";
    out->inlineInPage = false;
    return true;
}

/// Replaces the object reference that starts at \a valueAt with \a replacement.
bool replaceRef(const QByteArray &body, int valueAt, const QByteArray &replacement, QByteArray *out,
                QString *why)
{
    int i = valueAt;
    qint64 number = 0;
    qint64 generation = 0;
    if (!readInt(body, &i, &number) || !readInt(body, &i, &generation)) {
        fail(why, QStringLiteral("an indirect reference in a page dictionary is unreadable"));
        return false;
    }
    i = skipWhite(body, i);
    if (i >= body.size() || body.at(i) != 'R') {
        fail(why, QStringLiteral("an indirect reference in a page dictionary is unreadable"));
        return false;
    }
    *out = body.left(valueAt) + " " + replacement + body.mid(i + 1);
    return true;
}

/// Index just past the value of /Contents in \a body, which is either a "[ ... ]" array or an
/// "N G R" reference, or -1 when neither is readable there.
int contentsValueEnd(const QByteArray &body, int valueAt)
{
    const int v = skipWhite(body, valueAt);
    if (v >= body.size()) {
        return -1;
    }
    if (body.at(v) == '[') {
        return arrayEndIndex(body, v);
    }
    int i = v;
    qint64 number = 0;
    qint64 generation = 0;
    if (!readInt(body, &i, &number) || !readInt(body, &i, &generation)) {
        return -1;
    }
    const int p = skipWhite(body, i);
    if (p >= body.size() || body.at(p) != 'R') {
        return -1;
    }
    return p + 1;
}

bool contentEntries(PdfDocument *doc, const QByteArray &owner, int valueAt, int depth,
                    QByteArray *out, int *count, QString *why);

/// Appends one content stream reference, splicing the entries of an array object instead of writing
/// the reference itself.
///
/// This is the whole point of the flattening below: "/Contents 6 0 R" with 6 = "[4 0 R 5 0 R]" is
/// legal PDF, and the obvious update -- "[ save 6 0 R ink ]" -- nests one array inside another, which
/// is not a content list. Poppler answers "Weird page contents" and renders the page blank; Android's
/// PdfRenderer (PDFium) draws only the streams it can see directly, so the page's own content is lost
/// and the ink is left floating on blank paper -- which is exactly the Mi Pad 8 report. Splicing the
/// array's entries in flat keeps the original streams addressable while remaining one array.
bool appendContentEntry(PdfDocument *doc, int number, int generation, int depth, QByteArray *out,
                        int *count, QString *why)
{
    const QByteArray reference =
        QByteArray::number(number) + " " + QByteArray::number(generation) + " R";
    QByteArray target;
    QString unreadable;
    if (!objectByNumber(doc, number, &target, &unreadable)) {
        /// The reference cannot be resolved, but the file still names it. Keeping it verbatim is
        /// what the update always did, and refusing here would turn a page that used to export into
        /// a failed notebook.
        out->append(reference + " ");
        ++(*count);
        return true;
    }
    const int start = skipWhite(target, 0);
    if (start < target.size() && target.at(start) == '[') {
        if (depth >= 4) {
            fail(why, QStringLiteral("object %1 is a /Contents array nested more than four deep")
                          .arg(number));
            return false;
        }
        if (!contentEntries(doc, target, start, depth + 1, out, count, why)) {
            return false;
        }
        return true;
    }
    out->append(reference + " ");
    ++(*count);
    return true;
}

/// The content stream references of a /Contents value -- an array or a single reference -- as one
/// flat, space separated list that can be put inside a new array. \a count receives the number of
/// streams named, which is what the caller logs: a page that keeps zero streams is a page whose
/// content is about to be lost.
bool contentEntries(PdfDocument *doc, const QByteArray &owner, int valueAt, int depth,
                    QByteArray *out, int *count, QString *why)
{
    const int v = skipWhite(owner, valueAt);
    if (v >= owner.size()) {
        fail(why, QStringLiteral("a /Contents value is empty"));
        return false;
    }

    if (owner.at(v) == '[') {
        const int end = arrayEndIndex(owner, v);
        if (end < 0) {
            fail(why, QStringLiteral("a /Contents array is not closed"));
            return false;
        }
        int p = v + 1;
        while (p < end) {
            p = skipWhite(owner, p);
            if (p >= end) {
                break;
            }
            if (owner.at(p) == '[') {
                /// A nested array written out directly in the file: spliced like an indirect one,
                /// so what comes back is one flat content list either way.
                if (depth >= 4) {
                    fail(why, QStringLiteral("a /Contents array holds arrays nested more than four "
                                             "deep"));
                    return false;
                }
                if (!contentEntries(doc, owner, p, depth + 1, out, count, why)) {
                    return false;
                }
                p = arrayEndIndex(owner, p);
                if (p < 0) {
                    fail(why, QStringLiteral("a nested /Contents array is not closed"));
                    return false;
                }
                continue;
            }
            const int element = p;
            int i = p;
            qint64 number = 0;
            qint64 generation = 0;
            if (readInt(owner, &i, &number) && readInt(owner, &i, &generation)) {
                const int r = skipWhite(owner, i);
                if (r < owner.size() && owner.at(r) == 'R') {
                    if (!appendContentEntry(doc, int(number), int(generation), depth, out, count,
                                            why)) {
                        return false;
                    }
                    p = r + 1;
                    continue;
                }
            }
            /// Not a reference and not an array: not a content stream, but it is what the file
            /// says. A bare token is copied through; anything that is only a delimiter -- the ")"
            /// that closes this array, most importantly -- is stepped over, so the walk always
            /// advances and the array's own bracket is never copied into the new list.
            int e = element;
            while (e < end && !isWhite(owner.at(e)) && !isDelimiter(owner.at(e))) {
                ++e;
            }
            if (e == element) {
                ++p;
                continue;
            }
            out->append(owner.mid(element, e - element) + " ");
            p = e;
        }
        return true;
    }

    int i = v;
    qint64 number = 0;
    qint64 generation = 0;
    if (!readInt(owner, &i, &number) || !readInt(owner, &i, &generation)) {
        fail(why, QStringLiteral("a /Contents reference is unreadable"));
        return false;
    }
    i = skipWhite(owner, i);
    if (i >= owner.size() || owner.at(i) != 'R') {
        fail(why, QStringLiteral("a /Contents reference is unreadable"));
        return false;
    }
    return appendContentEntry(doc, int(number), int(generation), depth, out, count, why);
}

/// The page body with the ink content stream appended to /Contents and /pdfioInk added to the
/// page's (possibly inherited) resources.
bool preparePage(PdfDocument *doc, const PageEntry &page, int imageNumber, int contentNumber,
                 int saveContentNumber, int *nextObject, QList<WrittenObject> *appended,
                 QByteArray *newBody, int *originalContents, QString *why)
{
    QByteArray body;
    if (!objectByNumber(doc, page.number, &body, why)) {
        return false;
    }
    int dictStart = skipWhite(body, 0);
    int dictEnd = dictEndIndex(body, dictStart);
    if (dictEnd < 0) {
        fail(why, QStringLiteral("page %1 is not a dictionary").arg(page.number));
        return false;
    }

    /// /Contents becomes [save, original..., ink]: the save stream opens the graphics state
    /// before the page's own content and the ink stream closes it again, so the overlay is not
    /// subject to a transform or clip the source forgot to restore. A page that legitimately had
    /// no contents at all gets [save, ink].
    const int contentsAt = dictValueAt(body, dictStart, dictEnd, "Contents");
    const QByteArray contentRef = QByteArray::number(contentNumber) + " 0 R";
    const QByteArray saveRef = QByteArray::number(saveContentNumber) + " 0 R";
    *originalContents = 0;
    if (contentsAt < 0) {
        body = body.left(dictEnd - 2) + " /Contents [ " + saveRef + " " + contentRef + " ] "
               + body.mid(dictEnd - 2);
    } else {
        /// The original content has to sit in the same array as the save and the ink streams, as
        /// stream references. A /Contents that is an indirect reference to an array is resolved and
        /// its entries are spliced in flat -- wrapping the reference instead would nest one array
        /// inside another, which no reader accepts as a content list, and the page's own content
        /// would silently disappear from the export.
        QByteArray entries;
        QString contentWhy;
        if (!contentEntries(doc, body, contentsAt, 0, &entries, originalContents, &contentWhy)) {
            fail(why, QStringLiteral("page %1: %2").arg(page.number).arg(contentWhy));
            return false;
        }
        if (*originalContents == 0) {
            fail(why, QStringLiteral("page %1 has a /Contents that names no content stream")
                          .arg(page.number));
            return false;
        }
        const int valueEnd = contentsValueEnd(body, contentsAt);
        if (valueEnd < 0) {
            fail(why, QStringLiteral("page %1 has an unreadable /Contents").arg(page.number));
            return false;
        }
        body = body.left(contentsAt) + " [ " + saveRef + " " + entries + contentRef + " ]"
               + body.mid(valueEnd);
    }

    dictStart = skipWhite(body, 0);
    dictEnd = dictEndIndex(body, dictStart);

    ResourcesSlice resources;
    if (!findResources(doc, page, body, &resources, why)) {
        return false;
    }
    QByteArray merged;
    if (!mergeInkXObject(doc, resources.dict, imageNumber, nextObject, appended, &merged, why)) {
        return false;
    }

    if (resources.inlineInPage) {
        body = body.left(resources.valueAt) + merged + body.mid(resources.valueEnd);
    } else {
        const int newNumber = (*nextObject)++;
        appended->append(WrittenObject{newNumber, 0, merged});
        const QByteArray reference = QByteArray::number(newNumber) + " 0 R";

        dictStart = skipWhite(body, 0);
        dictEnd = dictEndIndex(body, dictStart);
        const int own = dictValueAt(body, dictStart, dictEnd, "Resources");
        if (own >= 0) {
            QByteArray repointed;
            if (!replaceRef(body, own, reference, &repointed, why)) {
                return false;
            }
            body = repointed;
        } else {
            dictEnd = dictEndIndex(body, dictStart);
            body = body.left(dictEnd - 2) + " /Resources " + reference + " " + body.mid(dictEnd - 2);
        }
    }

    *newBody = body;
    return true;
}

} // namespace

QList<int> PdfExporter::pageObjectNumbers(const QByteArray &pdf, QString *why)
{
    PdfDocument doc;
    if (!parseDocument(pdf, &doc, why)) {
        return {};
    }
    QList<PageEntry> pages;
    if (!collectPages(&doc, &pages, why)) {
        return {};
    }
    QList<int> numbers;
    numbers.reserve(pages.size());
    for (const PageEntry &page : pages) {
        numbers.append(page.number);
    }
    return numbers;
}

bool PdfExporter::exportWithInk(const QString &sourcePdf,
                                const PdfSessionManifest &manifest,
                                const QHash<int, QImage> &ink,
                                const QString &outPath,
                                QString *why)
{
    if (!manifest.isValid(why)) {
        return false;
    }

    /// F3's guard, and it is the whole reason it is here rather than in the menu. The overlay below
    /// is attached to the PDF page at the SAME POSITION as the notebook page, so it is only correct
    /// while the notebook's page N is the PDF's page N. A move, a delete, a duplicate, an insert or
    /// a page from another PDF breaks that, and the file that would come out has every mark on the
    /// wrong sheet -- with nothing to show for it but a PDF that looks fine. Refusing with the
    /// reason is the only safe answer until the writer can rebuild the page tree in notebook order.
    if (!PdfNotebookOps::exportIsOrderPreserving(manifest, why)) {
        return false;
    }

    QFile source(sourcePdf);
    if (!source.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(sourcePdf));
        return false;
    }
    const QByteArray pdf = source.readAll();
    source.close();

    PdfDocument doc;
    if (!parseDocument(pdf, &doc, why)) {
        return false;
    }
    QList<PageEntry> pages;
    if (!collectPages(&doc, &pages, why)) {
        return false;
    }

    int nextObject = qMax(1, doc.size);
    for (auto it = doc.xref.constBegin(); it != doc.xref.constEnd(); ++it) {
        nextObject = qMax(nextObject, it.key() + 1);
    }

    QByteArray out = pdf;
    if (!out.endsWith(NL)) {
        out += NL;
    }

    QList<WrittenObject> replacements;
    QList<WrittenObject> appended;
    QSet<int> rewritten;
    int inkedPages = 0;

    qWarning("[pdfio] export: source %s (%d bytes), %d PDF page(s), %d notebook page(s)",
             qPrintable(sourcePdf), int(pdf.size()), int(pages.size()),
             int(manifest.pages.size()));

    for (int i = 0; i < pages.size() && i < manifest.pages.size(); ++i) {
        const PageEntry &page = pages.at(i);
        QHash<int, QImage>::const_iterator found = ink.constFind(i);
        if (found == ink.constEnd() || found->isNull()) {
            continue;
        }
        if (rewritten.contains(page.number)) {
            fail(why, QStringLiteral("notebook pages %1 and %2 both point at PDF page object %3, "
                                     "so an overlay cannot be attached to each separately")
                          .arg(i).arg(i + 1).arg(page.number));
            return false;
        }
        rewritten.insert(page.number);
        ++inkedPages;

        /// The rotation comes first: the fallback below has to know it, because the notebook's
        /// recorded size is the *displayed* size and a quarter turn has to be undone before it can
        /// stand in for a page-space box.
        int rotation = 0;
        if (!effectiveRotation(&doc, page, why, &rotation)) {
            return false;
        }

        double x0 = 0;
        double y0 = 0;
        double x1 = 0;
        double y1 = 0;
        QString rectWhy;
        if (!effectiveRect(&doc, page, &rectWhy, &x0, &y0, &x1, &y1)) {
            /// The notebook measured this page from the same source when it was made, and the
            /// measurement is kept in manifest.json, so it is what the overlay is placed against
            /// when the source's own /MediaBox cannot be read.
            ///
            /// Measured on the tablet: a real 19-page notebook failed the whole export with
            /// "page 3875 has an empty or inverted /MediaBox", and the page that failed was not
            /// the first one -- so pressing Export did nothing at all, because the export had
            /// already returned before the document picker could open. A page whose box cannot be
            /// read is not a reason to refuse the other eighteen.
            const QSizeF pageSize = manifest.pages.at(i).sizePt;
            if (pageSize.isEmpty()) {
                fail(why, QStringLiteral("page %1 (PDF object %2): %3, and the notebook has no "
                                         "recorded size for it either")
                              .arg(i + 1).arg(page.number).arg(rectWhy));
                return false;
            }

            /// Both renderers report the size the reader shows, with /Rotate already applied, so a
            /// page turned a quarter turn is recorded as tall-by-wide when its own user space is
            /// wide-by-tall. Using it as it stands drew a 595x842 ink plane into an 842x595 box:
            /// measured on ex-mediabox-cases.pdf, a 50x50 mark came back 35x71 on the /Rotate 90
            /// page and the /Rotate 270 page's mark fell off the page entirely (0 red pixels).
            const bool quarterTurn = rotation == 90 || rotation == 270;
            x0 = 0;
            y0 = 0;
            x1 = quarterTurn ? pageSize.height() : pageSize.width();
            y1 = quarterTurn ? pageSize.width() : pageSize.height();

            qWarning("[pdfio] page %d (PDF object %d): %s; using the notebook's own %.2fx%.2f "
                     "points%s",
                     i + 1, page.number, qPrintable(rectWhy),
                     pageSize.width(), pageSize.height(),
                     quarterTurn ? ", turned back into page space for the /Rotate" : "");
        }

        /// The overlay is composited with the plane's own alpha as its mask, so a plane that has
        /// no alpha channel is not an overlay at all: it is an opaque rectangle, and drawing it
        /// hides the page underneath. Measured on text-fixture.pdf: the page's 1758 dark pixels
        /// came back as exactly the 2500 of the mark, the original text gone. That is the reported
        /// "ink floating on blank paper", made by the export itself, so it is refused with a
        /// message the menu shows instead of being written.
        if (!found->hasAlphaChannel()) {
            fail(why, QStringLiteral("page %1 (PDF object %2): the ink plane has no alpha channel, "
                                     "so it would be drawn as an opaque sheet over the page; the "
                                     "page's artifact must carry transparency")
                          .arg(i + 1).arg(page.number));
            return false;
        }

        /// The image is stored the way the viewer will rotate it, so it has to be turned back
        /// into the page's own coordinates first.
        const QImage placed = inkInPageSpace(*found, rotation);
        if (placed.isNull()) {
            fail(why, QStringLiteral("page %1 produced no image").arg(i + 1));
            return false;
        }

        const int maskNumber = nextObject++;
        const int imageNumber = nextObject++;
        appended.append(WrittenObject{maskNumber, 0,
                                      imageObject(placed.width(), placed.height(), "DeviceGray",
                                                  deflate(alphaSamples(placed)), false, 0)});
        appended.append(WrittenObject{imageNumber, 0,
                                      imageObject(placed.width(), placed.height(), "DeviceRGB",
                                                  deflate(rgbSamples(placed)), true, maskNumber)});

        /// Appended content inherits the graphics state the page's own stream left behind, and
        /// real writers do not always restore it: Chromium/Skia ends its page stream with a
        /// scaled, y-flipped CTM applied outside any q. A one byte "q" stream runs *before* the
        /// page content and saves the page's default user space; the ink stream then pops back to
        /// it with Q, so the overlay lands where the user drew it no matter what the source left.
        const int saveContentNumber = nextObject++;
        const QByteArray saveContent = "q" + NL;
        appended.append(WrittenObject{saveContentNumber, 0,
                                      "<< /Length " + QByteArray::number(saveContent.size())
                                          + " >>" + NL + "stream" + NL + saveContent + "endstream"});

        /// Draw the ink over the page, in the page's own user space.
        const double widthPt = x1 - x0;
        const double heightPt = y1 - y0;
        /// "Q " first, to leave whatever state the page content left. "q " and not "q": without
        /// the separator a parser reads "q595.0000" as one token.
        const QByteArray content = "Q q " + QByteArray::number(widthPt, 'f', 4) + " 0 0 "
                                   + QByteArray::number(heightPt, 'f', 4) + " "
                                   + QByteArray::number(x0, 'f', 4) + " "
                                   + QByteArray::number(y0, 'f', 4)
                                   + " cm /pdfioInk Do Q" + NL;

        const int contentNumber = nextObject++;
        appended.append(WrittenObject{contentNumber, 0,
                                      "<< /Length " + QByteArray::number(content.size()) + " >>"
                                          + NL + "stream" + NL + content + "endstream"});

        QByteArray newBody;
        int originalContents = 0;
        if (!preparePage(&doc, page, imageNumber, contentNumber, saveContentNumber, &nextObject,
                         &appended, &newBody, &originalContents, why)) {
            return false;
        }
        replacements.append(WrittenObject{page.number, page.generation, newBody});

        /// One line per inked page, so a log from a device that cannot be inspected directly says
        /// what the file was built from: which box placed the ink, whether that box came from the
        /// notebook because the page's own /MediaBox could not be read, the plane's size and whether
        /// it carries alpha, and how many of the page's own content streams survived into the
        /// update. Zero surviving streams is a page whose background is gone.
        qWarning("[pdfio] export page %d/%d (PDF object %d): box %.2f,%.2f..%.2f,%.2f%s, rotate %d, "
                 "ink %dx%d %s, page contents kept: %d stream(s)",
                 i + 1, int(qMin(pages.size(), manifest.pages.size())), page.number,
                 x0, y0, x1, y1, rectWhy.isEmpty() ? " from /MediaBox" : " from the notebook",
                 rotation, placed.width(), placed.height(),
                 found->hasAlphaChannel() ? "with alpha" : "NO ALPHA", originalContents);
    }

    qWarning("[pdfio] export: %d of %d notebook page(s) inked, %d page object(s) rewritten, "
             "%d object(s) appended",
             inkedPages, int(qMin(pages.size(), manifest.pages.size())), int(replacements.size()),
             int(appended.size()));

    if (replacements.isEmpty() && appended.isEmpty()) {
        /// Nothing to add: the honest answer is a clean copy, not an incremental update with an
        /// empty body.
        QFile copy(outPath);
        if (!copy.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fail(why, QStringLiteral("cannot write %1").arg(outPath));
            return false;
        }
        copy.write(pdf);
        return true;
    }

    QHash<int, qint64> offsets;
    QList<int> numbers;
    auto writeObject = [&out, &offsets, &numbers](const WrittenObject &object) {
        offsets.insert(object.number, out.size());
        numbers.append(object.number);
        out += QByteArray::number(object.number) + " " + QByteArray::number(object.generation)
               + " obj" + NL + object.body + NL + "endobj" + NL;
    };
    for (const WrittenObject &object : replacements) {
        writeObject(object);
    }
    for (const WrittenObject &object : appended) {
        writeObject(object);
    }

    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());

    int size = nextObject;
    for (int number : numbers) {
        size = qMax(size, number + 1);
    }

    const qint64 xrefAt = out.size();
    out += "xref" + QByteArray(NL);
    /// The free head entry, then one subsection per written object, in ascending order.
    out += "0 1" + QByteArray(NL) + "0000000000 65535 f " + QByteArray(NL);
    for (int number : numbers) {
        out += QByteArray::number(number) + " 1" + QByteArray(NL);
        out += QByteArray::number(offsets.value(number)).rightJustified(10, '0') + " 00000 n "
               + QByteArray(NL);
    }

    out += "trailer" + QByteArray(NL) + "<< /Size " + QByteArray::number(size)
           + " /Root " + QByteArray::number(doc.rootNumber) + " "
           + QByteArray::number(doc.rootGeneration) + " R /Prev "
           + QByteArray::number(doc.newestXrefOffset);
    if (doc.hasInfo) {
        out += " /Info " + QByteArray::number(doc.infoNumber) + " "
               + QByteArray::number(doc.infoGeneration) + " R";
    }
    if (doc.hasId) {
        out += " /ID " + doc.id;
    }
    out += " >>" + QByteArray(NL) + "startxref" + QByteArray(NL) + QByteArray::number(xrefAt)
           + QByteArray(NL) + "%%EOF" + QByteArray(NL);

    QFile target(outPath);
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(why, QStringLiteral("cannot write %1").arg(outPath));
        return false;
    }
    target.write(out);
    return true;
}
