/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfSessionManifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStringList>

namespace {

/// Whether writeTo() has been asked to fail on the way to committing. Set by a test only.
bool &failBeforeCommitForTests()
{
    static bool fail = false;
    return fail;
}

/// The value a rotation normalizes to: 450 is 90, -90 is 270.
int normalizedQuarterTurn(int degrees)
{
    return ((degrees % 360) + 360) % 360;
}

/// Whether \a degrees is one of the four turns this manifest accepts.
bool isQuarterTurn(int degrees)
{
    return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

/// The highest number a "pages/pNNNN.kra" name in \a pages uses, or 0 for none. The names
/// PdfSession::pageFileName() hands out are the only ones this counter has anything to say about;
/// a record whose ink lives somewhere else -- a renamed artifact -- is none of its business.
int highestArtifactNumber(const QList<PdfPageRecord> &pages)
{
    int highest = 0;
    for (const PdfPageRecord &page : pages) {
        const QString name = QFileInfo(page.kraFile).fileName();
        if (!name.startsWith(QLatin1Char('p')) || !name.endsWith(QStringLiteral(".kra"))) {
            continue;
        }
        bool ok = false;
        const int number = name.mid(1, name.size() - 5).toInt(&ok);
        if (ok && number > highest) {
            highest = number;
        }
    }
    return highest;
}

QJsonArray sizeToJson(const QSizeF &size)
{
    return QJsonArray{size.width(), size.height()};
}

QSizeF sizeFromJson(const QJsonValue &value)
{
    const QJsonArray array = value.toArray();
    if (array.size() != 2) {
        return QSizeF();
    }
    return QSizeF(array.at(0).toDouble(), array.at(1).toDouble());
}

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

bool PdfSessionManifest::isSafeRelativePath(const QString &path, QString *why)
{
    if (path.isEmpty()) {
        fail(why, QStringLiteral("it is empty"));
        return false;
    }
    if (QDir::isAbsolutePath(path) || path.startsWith(QLatin1Char('/'))) {
        fail(why, QStringLiteral("it is an absolute path"));
        return false;
    }
    /// Refused before the drive check, and before anything could treat it as one separator: on a
    /// platform where a backslash is a separator this is the same escape as "../", and on this one
    /// it is a name an archive tool or a file manager may still read as a path.
    if (path.contains(QLatin1Char('\\'))) {
        fail(why, QStringLiteral("it uses a backslash as a separator"));
        return false;
    }
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':')) {
        fail(why, QStringLiteral("it names a drive"));
        return false;
    }

    const QStringList components = path.split(QLatin1Char('/'));
    for (const QString &component : components) {
        if (component.isEmpty()) {
            fail(why, QStringLiteral("it has an empty path component"));
            return false;
        }
        if (component == QLatin1String(".")) {
            fail(why, QStringLiteral("it names the project directory itself"));
            return false;
        }
        if (component == QLatin1String("..")) {
            fail(why, QStringLiteral("it escapes the project directory"));
            return false;
        }
    }
    return true;
}

QString PdfSessionManifest::displayName() const
{
    /// A name of its own, or the source's. Not trimmed here: a manifest is written by this code
    /// and the rename path refuses an empty name, so a name that is there is usable as it is.
    if (!name.isEmpty()) {
        return name;
    }
    return QFileInfo(sourceFile).completeBaseName();
}

QSizeF PdfPageRecord::displaySizePt() const
{
    /// A quarter turn swaps the sheet's sides, which is what the reader sees; sizePt stays what
    /// the file declares so the record survives a renderer change.
    return (extraRotation == 90 || extraRotation == 270)
        ? QSizeF(sizePt.height(), sizePt.width())
        : sizePt;
}

int PdfSessionManifest::sourceCount() const
{
    /// One when the list is not there: the three legacy fields are then the whole story, which is
    /// what every notebook made before sources[] existed looks like after the upgrade in fromJson.
    return sources.isEmpty() ? 1 : sources.size();
}

PdfSourceRecord PdfSessionManifest::sourceAt(int index) const
{
    if (index < 0 || index >= sourceCount()) {
        return PdfSourceRecord();
    }
    if (sources.isEmpty()) {
        PdfSourceRecord only;
        only.file = sourceFile;
        only.sha256 = sourceSha256;
        only.byteSize = sourceByteSize;
        return only;
    }
    return sources.at(index);
}

PdfSourceRecord PdfSessionManifest::sourceForPage(const PdfPageRecord &page) const
{
    return sourceAt(page.source);
}

int PdfSessionManifest::sourceIndexForSha(const QByteArray &sha256) const
{
    if (sha256.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < sourceCount(); ++i) {
        if (sourceAt(i).sha256 == sha256) {
            return i;
        }
    }
    return -1;
}

int PdfSessionManifest::allocatePageNumber()
{
    /// Self-healing rather than trusting the field: a manifest written before the counter existed,
    /// or one whose counter a hand edit lowered, must not hand out a number a page already names.
    refreshNextPageNumber();
    return nextPageNumber++;
}

int PdfSessionManifest::effectiveNextPageNumber() const
{
    const int next = qMax(nextPageNumber, highestArtifactNumber(pages) + 1);
    return qMax(next, 1);
}

void PdfSessionManifest::refreshNextPageNumber()
{
    nextPageNumber = effectiveNextPageNumber();
}

void PdfSessionManifest::setFailBeforeCommitForTests(bool fail)
{
    failBeforeCommitForTests() = fail;
}

/// How many times writeTo() has committed, for the seam above. A file-scope counter rather than a
/// static member so the header does not have to name a storage. 
static int &committedManifests()
{
    static int count = 0;
    return count;
}

int PdfSessionManifest::writeCountForTests()
{
    return committedManifests();
}

void PdfSessionManifest::resetWriteCountForTests()
{
    committedManifests() = 0;
}

bool PdfSessionManifest::isValid(QString *why) const
{
    if (schema != CurrentSchema) {
        fail(why, QStringLiteral("unsupported manifest schema %1").arg(schema));
        return false;
    }
    if (sourceFile.isEmpty()) {
        fail(why, QStringLiteral("no source file recorded"));
        return false;
    }
    if (sourceSha256.isEmpty()) {
        fail(why, QStringLiteral("no source checksum recorded"));
        return false;
    }
    if (pages.isEmpty()) {
        fail(why, QStringLiteral("no pages recorded"));
        return false;
    }
    /// Every field that names a file is checked here, once, where the manifest enters the session.
    /// readFrom(), fromJson() and PdfSession::openProject() all come through isValid(), and every
    /// consumer joins these names onto the project directory afterwards -- so this is the one place
    /// a hand-placed project directory, or a manifest that has been edited, cannot get past.
    QString reason;
    if (!isSafeRelativePath(sourceFile, &reason)) {
        fail(why, QStringLiteral("the manifest's source file \"%1\" is not a file inside the project: %2")
                      .arg(sourceFile, reason));
        return false;
    }

    /// The sources, when the list is there: each one a file inside the project with a checksum of
    /// its own, and the first agreeing with the three fields above, which older readers still use.
    for (int i = 0; i < sources.size(); ++i) {
        const PdfSourceRecord &source = sources.at(i);
        if (source.file.isEmpty() || source.sha256.isEmpty()) {
            fail(why, QStringLiteral("source %1 is incomplete").arg(i + 1));
            return false;
        }
        if (!isSafeRelativePath(source.file, &reason)) {
            fail(why, QStringLiteral("the manifest's source %1 \"%2\" is not a file inside the project: %3")
                          .arg(i + 1).arg(source.file, reason));
            return false;
        }
    }
    if (!sources.isEmpty()
        && (sources.first().file != sourceFile || sources.first().sha256 != sourceSha256
            || sources.first().byteSize != sourceByteSize)) {
        fail(why, QStringLiteral("the manifest's first source \"%1\" does not agree with the source it records")
                      .arg(sources.first().file));
        return false;
    }

    for (const PdfPageRecord &page : pages) {
        if (page.index < 0 || !page.sizePt.isValid()) {
            fail(why, QStringLiteral("page %1 is incomplete").arg(page.index));
            return false;
        }
        if (page.source < 0 || page.source >= sourceCount()) {
            fail(why, QStringLiteral("page %1 names source %2, which the manifest does not have")
                          .arg(page.index + 1).arg(page.source));
            return false;
        }
        if (!isQuarterTurn(page.extraRotation)) {
            fail(why, QStringLiteral("page %1 has an extra rotation of %2 degrees, which is not a quarter turn")
                          .arg(page.index + 1).arg(page.extraRotation));
            return false;
        }
        if (page.kraFile.isEmpty()) {
            fail(why, QStringLiteral("the manifest's page %1 ink file is not recorded").arg(page.index + 1));
            return false;
        }
        if (!isSafeRelativePath(page.kraFile, &reason)) {
            fail(why, QStringLiteral("the manifest's page %1 ink file \"%2\" is not a file inside the project: %3")
                          .arg(page.index + 1).arg(page.kraFile, reason));
            return false;
        }

        /// An empty thumbnail stays legal -- see PdfPageRecord::thumbFile. A name that is there is
        /// checked like any other, because the docker and the strip decoration join it.
        if (!page.thumbFile.isEmpty() && !isSafeRelativePath(page.thumbFile, &reason)) {
            fail(why, QStringLiteral("the manifest's page %1 thumbnail \"%2\" is not a file inside the project: %3")
                          .arg(page.index + 1).arg(page.thumbFile, reason));
            return false;
        }
    }
    return true;
}

QJsonObject PdfSessionManifest::toJson() const
{
    QJsonArray pageArray;
    for (const PdfPageRecord &page : pages) {
        QJsonObject object;
        object.insert(QStringLiteral("index"), page.index);
        object.insert(QStringLiteral("sizePt"), sizeToJson(page.sizePt));
        object.insert(QStringLiteral("rotation"), page.rotation);
        object.insert(QStringLiteral("kra"), page.kraFile);
        object.insert(QStringLiteral("thumb"), page.thumbFile);
        object.insert(QStringLiteral("generation"), page.generation);
        object.insert(QStringLiteral("source"), page.source);
        object.insert(QStringLiteral("extraRotation"), page.extraRotation);
        pageArray.append(object);
    }

    QJsonObject source;
    source.insert(QStringLiteral("file"), sourceFile);
    source.insert(QStringLiteral("sha256"), QString::fromLatin1(sourceSha256));
    source.insert(QStringLiteral("bytes"), double(sourceByteSize));

    /// The list is always written, even when the manifest was built without one: a reader then
    /// never has to work out whether the single source in the legacy object is the whole story.
    QJsonArray sourceArray;
    if (sources.isEmpty()) {
        sourceArray.append(source);
    } else {
        for (const PdfSourceRecord &record : sources) {
            QJsonObject entry;
            entry.insert(QStringLiteral("file"), record.file);
            entry.insert(QStringLiteral("sha256"), QString::fromLatin1(record.sha256));
            entry.insert(QStringLiteral("bytes"), double(record.byteSize));
            sourceArray.append(entry);
        }
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema"), schema);
    root.insert(QStringLiteral("name"), name);
    root.insert(QStringLiteral("source"), source);
    root.insert(QStringLiteral("sources"), sourceArray);
    /// The effective value, not the raw field: a manifest built in code that never refreshed its
    /// counter still records one that is past every page, so reading it back gives the same answer
    /// as writing it -- and no later write can hand out a number a page already names.
    root.insert(QStringLiteral("nextPageNumber"), effectiveNextPageNumber());
    root.insert(QStringLiteral("pages"), pageArray);
    return root;
}

PdfSessionManifest PdfSessionManifest::fromJson(const QJsonObject &object, QString *why)
{
    PdfSessionManifest manifest;

    if (!object.contains(QStringLiteral("schema"))) {
        fail(why, QStringLiteral("not a pdfio manifest: no schema field"));
        return manifest;
    }

    manifest.schema = object.value(QStringLiteral("schema")).toInt();

    /// A schema 1 manifest is this shape plus nothing: one source in the legacy object, no
    /// sources[] list, no per-page source and no page-number counter. It is upgraded here, in
    /// memory, which is what keeps every notebook made until now opening.
    if (manifest.schema == 1) {
        manifest.schema = CurrentSchema;
    }

    /// Absent in a manifest written before the field existed, and every notebook made then must
    /// keep opening: a missing name is simply no name of its own.
    manifest.name = object.value(QStringLiteral("name")).toString();

    const QJsonObject source = object.value(QStringLiteral("source")).toObject();
    manifest.sourceFile = source.value(QStringLiteral("file")).toString();
    manifest.sourceSha256 = source.value(QStringLiteral("sha256")).toString().toLatin1();
    manifest.sourceByteSize = qint64(source.value(QStringLiteral("bytes")).toDouble());

    const QJsonArray sourceArray = object.value(QStringLiteral("sources")).toArray();
    for (const QJsonValue &value : sourceArray) {
        const QJsonObject entry = value.toObject();
        PdfSourceRecord record;
        record.file = entry.value(QStringLiteral("file")).toString();
        record.sha256 = entry.value(QStringLiteral("sha256")).toString().toLatin1();
        record.byteSize = qint64(entry.value(QStringLiteral("bytes")).toDouble());
        manifest.sources.append(record);
    }

    /// The one source a notebook made before sources[] existed records IS the whole list, and
    /// every one of its pages comes from it; the three legacy fields and the list then agree, which
    /// is what isValid() asks of a manifest. The other way round -- a list with no legacy object --
    /// is filled in rather than refused, so an older reader gets the same answer as this one.
    if (manifest.sources.isEmpty() && !manifest.sourceFile.isEmpty()) {
        PdfSourceRecord only;
        only.file = manifest.sourceFile;
        only.sha256 = manifest.sourceSha256;
        only.byteSize = manifest.sourceByteSize;
        manifest.sources.append(only);
    } else if (!manifest.sources.isEmpty() && manifest.sourceFile.isEmpty()) {
        manifest.sourceFile = manifest.sources.first().file;
        manifest.sourceSha256 = manifest.sources.first().sha256;
        manifest.sourceByteSize = manifest.sources.first().byteSize;
    }

    const QJsonArray pageArray = object.value(QStringLiteral("pages")).toArray();
    for (const QJsonValue &value : pageArray) {
        const QJsonObject pageObject = value.toObject();
        PdfPageRecord page;
        page.index = pageObject.value(QStringLiteral("index")).toInt(-1);
        page.sizePt = sizeFromJson(pageObject.value(QStringLiteral("sizePt")));
        page.rotation = pageObject.value(QStringLiteral("rotation")).toInt();
        page.kraFile = pageObject.value(QStringLiteral("kra")).toString();
        page.thumbFile = pageObject.value(QStringLiteral("thumb")).toString();
        page.generation = pageObject.value(QStringLiteral("generation")).toInt();
        page.source = pageObject.value(QStringLiteral("source")).toInt();
        page.extraRotation = normalizedQuarterTurn(pageObject.value(QStringLiteral("extraRotation")).toInt());
        manifest.pages.append(page);
    }

    /// The counter, raised past every number the page list already names: a manifest written before
    /// it existed, or one a hand edit lowered, must not hand out a number in use.
    manifest.nextPageNumber = object.value(QStringLiteral("nextPageNumber")).toInt();
    manifest.refreshNextPageNumber();

    if (!manifest.isValid(why)) {
        return manifest;
    }

    /// Clear a why that isValid() may have set on an earlier, discarded attempt.
    return manifest;
}

bool PdfSessionManifest::writeTo(const QString &path, QString *why) const
{
    if (!isValid(why)) {
        return false;
    }

    /// Written beside the destination and renamed onto it. This file is the one durable
    /// description of the notebook: a crash or a full disk in the middle of a plain
    /// truncate-and-write leaves something that is neither the old manifest nor the new one, with
    /// the page list and every file name it held gone.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        fail(why, QStringLiteral("cannot write %1").arg(path));
        return false;
    }

    const QByteArray bytes = QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        fail(why, QStringLiteral("cannot write %1").arg(path));
        file.cancelWriting();
        return false;
    }

    /// The seam a test uses to stand where a crash between the write and the rename would: the
    /// manifest that was already there has to survive it untouched.
    if (failBeforeCommitForTests()) {
        fail(why, QStringLiteral("refused before putting %1 in place (test)").arg(path));
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        fail(why, QStringLiteral("cannot put %1 in place: %2").arg(path, file.errorString()));
        return false;
    }

    ++committedManifests();
    return true;
}

PdfSessionManifest PdfSessionManifest::readFrom(const QString &path, QString *why)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(path));
        return PdfSessionManifest();
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        fail(why, QStringLiteral("%1 is not valid JSON: %2").arg(path, error.errorString()));
        return PdfSessionManifest();
    }

    return fromJson(document.object(), why);
}

QByteArray PdfSessionManifest::sha256OfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return QByteArray();
    }
    return hash.result().toHex();
}
