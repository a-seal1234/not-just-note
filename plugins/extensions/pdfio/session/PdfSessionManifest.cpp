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
#include <QStringList>

const int PdfSessionManifest::CurrentSchema = 1;

namespace {

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

    for (const PdfPageRecord &page : pages) {
        if (page.index < 0 || !page.sizePt.isValid()) {
            fail(why, QStringLiteral("page %1 is incomplete").arg(page.index));
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
        pageArray.append(object);
    }

    QJsonObject source;
    source.insert(QStringLiteral("file"), sourceFile);
    source.insert(QStringLiteral("sha256"), QString::fromLatin1(sourceSha256));
    source.insert(QStringLiteral("bytes"), double(sourceByteSize));

    QJsonObject root;
    root.insert(QStringLiteral("schema"), schema);
    root.insert(QStringLiteral("name"), name);
    root.insert(QStringLiteral("source"), source);
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

    /// Absent in a manifest written before the field existed, and every notebook made then must
    /// keep opening: a missing name is simply no name of its own.
    manifest.name = object.value(QStringLiteral("name")).toString();

    const QJsonObject source = object.value(QStringLiteral("source")).toObject();
    manifest.sourceFile = source.value(QStringLiteral("file")).toString();
    manifest.sourceSha256 = source.value(QStringLiteral("sha256")).toString().toLatin1();
    manifest.sourceByteSize = qint64(source.value(QStringLiteral("bytes")).toDouble());

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
        manifest.pages.append(page);
    }

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

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(why, QStringLiteral("cannot write %1").arg(path));
        return false;
    }

    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
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
