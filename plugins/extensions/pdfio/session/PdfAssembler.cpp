/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfAssembler.h"

#include "session/PdfSession.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <cstdio>

/**
 * The assembler's reader, and why it is narrow on purpose.
 *
 * docs/PDFIO-DESIGN-ASSEMBLE.md measured the PDFs the user actually imports: classic PDF 1.4 with
 * an xref TABLE, plain objects, no object streams (the real manual: 0 objects in object streams;
 * the modern forms appear only in our own synthetic fixtures). This reads exactly that, and REFUSES
 * -- naming the file -- everything else: cross-reference streams, /ObjStm, hybrid /XRefStm,
 * encryption. A refusal is cheap and the per-source export is the fallback; a half-read source
 * written out as a file that only looks right is the one outcome this file must never produce.
 *
 * Streams are never decoded: a copied stream is its bytes and its dictionary, byte for byte, so the
 * text layer, the fonts and the embedded images survive untouched.
 */
namespace {

const QByteArray NL("\n", 1);

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
bool valueIsRef(const QByteArray &body, int at, int *number)
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
    if (n <= 0 || number == nullptr) {
        return false;
    }
    *number = int(n);
    return true;
}

struct XrefEntry {
    int type = 0;      ///< 1 uncompressed. Nothing else is read.
    qint64 offset = 0;
    int generation = 0;
};

struct SourceDoc {
    QByteArray bytes;
    QHash<int, XrefEntry> xref;
    int rootNumber = -1;
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

bool readObjectBody(const SourceDoc &doc, int number, QByteArray *body, QString *why)
{
    const QHash<int, XrefEntry>::const_iterator it = doc.xref.constFind(number);
    if (it == doc.xref.constEnd() || it->type != 1 || it->offset <= 0
        || it->offset >= doc.bytes.size()) {
        fail(why, QStringLiteral("%1: object %2 is not in the xref table")
                      .arg(doc.file).arg(number));
        return false;
    }

    int i = skipWhite(doc.bytes, int(it->offset));
    qint64 n = 0;
    qint64 g = 0;
    if (!readIntAt(doc.bytes, &i, &n) || n != number) {
        fail(why, QStringLiteral("%1: the xref table points at object %2 but the file says "
                                 "something else there")
                      .arg(doc.file).arg(number));
        return false;
    }
    if (!readIntAt(doc.bytes, &i, &g)) {
        fail(why, QStringLiteral("%1: object %2 has no generation number")
                      .arg(doc.file).arg(number));
        return false;
    }
    i = skipWhite(doc.bytes, i);
    if (doc.bytes.mid(i, 3) != "obj") {
        fail(why, QStringLiteral("%1: object %2 does not start with \"obj\"")
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
            int dataStart = after + 6;
            if (doc.bytes.mid(dataStart, 2) == "\r\n") {
                dataStart += 2;
            } else if (dataStart < doc.bytes.size()
                       && (doc.bytes.at(dataStart) == '\n' || doc.bytes.at(dataStart) == '\r')) {
                ++dataStart;
            }
            const int endstream = int(doc.bytes.indexOf("endstream", dataStart));
            if (endstream < 0) {
                fail(why, QStringLiteral("%1: the stream of object %2 has no endstream")
                              .arg(doc.file).arg(number));
                return false;
            }
            /// The dictionary AND the stream, so the copier can copy the data verbatim and rewrite
            /// only the dictionary.
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

/// Parses the classic xref chain from the LAST startxref. Everything else is refused by name.
bool parseSource(const QString &path, const QString &file, SourceDoc *doc, QString *why)
{
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("%1: cannot be read").arg(file));
        return false;
    }
    doc->bytes = source.readAll();
    doc->file = file;
    const QByteArray &b = doc->bytes;

    const int startxref = int(b.lastIndexOf("startxref"));
    if (startxref < 0) {
        fail(why, QStringLiteral("%1: no startxref").arg(file));
        return false;
    }
    int i = skipWhite(b, startxref + 9);
    qint64 offset = 0;
    if (!readIntAt(b, &i, &offset)) {
        fail(why, QStringLiteral("%1: startxref does not name an offset").arg(file));
        return false;
    }

    QSet<qint64> seen;
    while (offset > 0 && !seen.contains(offset)) {
        seen.insert(offset);
        if (offset >= b.size()) {
            fail(why, QStringLiteral("%1: startxref points past the end of the file").arg(file));
            return false;
        }
        if (b.mid(int(offset), 4) != "xref") {
            fail(why, QStringLiteral("%1: this source does not use a classic xref table "
                                     "(cross-reference streams and object streams are refused)")
                          .arg(file));
            return false;
        }

        int p = int(offset) + 4;
        bool readSubsection = true;
        while (readSubsection) {
            qint64 first = 0;
            qint64 count = 0;
            int q = p;
            if (!readIntAt(b, &q, &first) || !readIntAt(b, &q, &count) || count <= 0) {
                readSubsection = false;
                break;
            }
            p = q;
            for (qint64 k = 0; k < count; ++k) {
                const int at = p + int(k) * 20;
                const QByteArray entry = b.mid(at, 20);
                int t = 0;
                qint64 entryOffset = 0;
                qint64 entryGeneration = 0;
                if (readIntAt(entry, &t, &entryOffset) && readIntAt(entry, &t, &entryGeneration)) {
                    const int typeAt = skipWhite(entry, t);
                    const char type = typeAt < entry.size() ? entry.at(typeAt) : 'f';
                    const int number = int(first + k);
                    if (type == 'n' && !doc->xref.contains(number)) {
                        doc->xref.insert(number, XrefEntry{1, entryOffset, int(entryGeneration)});
                    }
                }
            }
            p += int(count) * 20;
        }

        const int trailerAt = int(b.indexOf("<<", p));
        if (trailerAt < 0) {
            fail(why, QStringLiteral("%1: the trailer dictionary is missing").arg(file));
            return false;
        }
        const int trailerEnd = dictEndIndex(b, trailerAt);
        if (trailerEnd < 0) {
            fail(why, QStringLiteral("%1: the trailer dictionary is unreadable").arg(file));
            return false;
        }
        const QByteArray trailer = b.mid(trailerAt, trailerEnd - trailerAt);

        int valueAt = 0;
        if (topLevelKey(trailer, "Encrypt", &valueAt)) {
            fail(why, QStringLiteral("%1: this source is encrypted").arg(file));
            return false;
        }
        if (topLevelKey(trailer, "XRefStm", &valueAt)) {
            fail(why, QStringLiteral("%1: this source is a hybrid file with an xref stream").arg(file));
            return false;
        }
        if (doc->rootNumber < 0 && topLevelKey(trailer, "Root", &valueAt)) {
            int root = 0;
            if (valueIsRef(trailer, valueAt, &root)) {
                doc->rootNumber = root;
            }
        }

        qint64 prev = 0;
        int prevAt = 0;
        if (topLevelKey(trailer, "Prev", &prevAt) && readIntAt(trailer, &prevAt, &prev) && prev > 0) {
            offset = prev;
        } else {
            offset = -1;
        }
    }

    if (doc->rootNumber < 0) {
        fail(why, QStringLiteral("%1: the trailer does not name a /Root catalog").arg(file));
        return false;
    }
    return true;
}

/// The array or dictionary at \a at, following ONE indirect reference. For /MediaBox, /CropBox,
/// /Rotate and /Resources, one level is all a real file needs, and more than one is refused rather
/// than guessed at.
bool resolveValue(const SourceDoc &doc, const QByteArray &body, int at, QByteArray *text, QString *why)
{
    int number = 0;
    if (valueIsRef(body, at, &number)) {
        QByteArray referenced;
        if (!readObjectBody(doc, number, &referenced, why)) {
            return false;
        }
        const int v = skipWhite(referenced, 0);
        return resolveValue(doc, referenced, v, text, why);
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
bool walkPages(const SourceDoc &doc, int node, const SourcePage &inherited, QList<SourcePage> *pages,
               QString *why, int depth = 0)
{
    if (depth > 64) {
        fail(why, QStringLiteral("%1: the page tree is deeper than 64 levels").arg(doc.file));
        return false;
    }
    QByteArray body;
    if (!readObjectBody(doc, node, &body, why)) {
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
            if (!valueIsRef(kids, i, &child)) {
                fail(why, QStringLiteral("%1: a /Kids entry is not a page reference").arg(doc.file));
                return false;
            }
            SourcePage childInherited = here;
            childInherited.object = child;
            childInherited.inheritsResources = true;
            childInherited.inheritsMediaBox = true;
            if (!walkPages(doc, child, childInherited, pages, why, depth + 1)) {
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
    if (!readObjectBody(doc, doc.rootNumber, &rootBody, why)) {
        return false;
    }
    int valueAt = 0;
    if (!topLevelKey(rootBody, "Pages", &valueAt)) {
        fail(why, QStringLiteral("%1: the /Root catalog has no /Pages").arg(doc.file));
        return false;
    }
    int pagesNode = 0;
    if (!valueIsRef(rootBody, valueAt, &pagesNode)) {
        fail(why, QStringLiteral("%1: the catalog's /Pages is not a reference").arg(doc.file));
        return false;
    }
    SourcePage inherited;
    inherited.inheritsResources = true;
    inherited.inheritsMediaBox = true;
    return walkPages(doc, pagesNode, inherited, pages, why);
}

/// Copies objects into the output, renumbering every reference. Streams are copied byte for byte:
/// only a stream's DICTIONARY is rewritten, never its data.
class Copier
{
public:
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

    /// Copies object a sourceNumber of a sourceIndex. The key is the PAIR: object 5 of the first
    /// source and object 5 of the second are different objects, and numbering them by number alone
    /// is how two sources would collide.
    bool copy(int sourceIndex, int sourceNumber, int *outNumber, QString *why)
    {
        const quint64 key = (quint64(quint32(sourceIndex)) << 32) | quint32(sourceNumber);
        const QHash<quint64, int>::const_iterator known = m_map.constFind(key);
        if (known != m_map.constEnd()) {
            *outNumber = known.value();
            return true;
        }
        const SourceDoc &doc = m_docs->value(sourceIndex);
        if (!doc.xref.contains(sourceNumber)) {
            fail(why, QStringLiteral("%1: object %2 is referenced but not in the xref table")
                          .arg(doc.file).arg(sourceNumber));
            return false;
        }

        /// The number is MAPPED BEFORE the body is read, so a cycle cannot recurse for ever.
        const int number = m_next++;
        m_map.insert(key, number);
        m_bodies.insert(number, QByteArray());

        QByteArray body;
        if (!readObjectBody(doc, sourceNumber, &body, why)) {
            return false;
        }
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
                        if (!copy(sourceIndex, int(n), &outNumber, why)) {
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
    QHash<quint64, int> m_map;
    QHash<int, QByteArray> m_bodies;
};

/// Writes a complete classic PDF: a header, the objects in number order, an xref table and a
/// trailer. Every object in it is ours, so no object stream is needed whatever the sources used.
bool writeFile(const QString &outPath, const QHash<int, QByteArray> &bodies, int maxNumber,
               int rootNumber, QString *why)
{
    QByteArray out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    QList<qint64> offsets;
    offsets.resize(maxNumber + 1);
    for (int n = 0; n <= maxNumber; ++n) {
        offsets[n] = -1;
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

    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(why, QStringLiteral("cannot write %1").arg(outPath));
        return false;
    }
    if (file.write(out) != out.size()) {
        fail(why, QStringLiteral("%1 was not written completely").arg(outPath));
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
        if (record.source < 0 || record.source >= manifest.sourceCount()) {
            fail(why, QStringLiteral("notebook page names source %1, which the manifest does not "
                                     "hold").arg(record.source));
            return false;
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

    QList<int> pageNumbers;
    for (int i = 0; i < plans.size(); ++i) {
        const PagePlan &plan = plans.at(i);
        const PdfPageRecord &record = manifest.pages.at(i);
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
        const SourcePage &source = sourcePages.value(plan.sourceIndex).at(plan.sourcePage);
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
                if (!valueIsRef(source.resources, 0, &referenced)) {
                    fail(why, QStringLiteral("%1 the /Resources value cannot be copied").arg(QString::fromUtf8(prefix)));
                    return false;
                }
                if (!copier.copy(plan.sourceIndex, referenced, &resourcesObject, why)) {
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
            int valueAt = 0;
            if (topLevelKey(pageBody, "Contents", &valueAt)) {
                int referenced = 0;
                if (valueIsRef(pageBody, valueAt, &referenced)) {
                    int out = 0;
                    if (!copier.copy(plan.sourceIndex, referenced, &out, why)) {
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
                        if (!valueIsRef(array, k, &item)) {
                            fail(why, QStringLiteral("%1 a /Contents array entry is not a stream "
                                                     "reference").arg(QString::fromUtf8(prefix)));
                            return false;
                        }
                        int out = 0;
                        if (!copier.copy(plan.sourceIndex, item, &out, why)) {
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
            int groupObject = 0;
            if (valueIsRef(pageBody, groupAt, &groupRef)) {
                if (!copier.copy(plan.sourceIndex, groupRef, &groupObject, why)) {
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
        /// OUTPUT number (the parent, the copied resources, the copied contents), so it is inserted
        /// as it stands. Sending it back through the reference rewriter would look those numbers up
        /// in a source and copy the wrong objects.
        const int pageObject = copier.allocate();
        copier.insert(pageObject, dict);
        pageNumbers.append(pageObject);
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
