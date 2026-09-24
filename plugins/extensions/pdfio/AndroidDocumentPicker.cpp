/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "AndroidDocumentPicker.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#if defined(Q_OS_ANDROID)

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniEnvironment>
#include <QJniObject>
#else
#include <QAndroidActivityResultReceiver>
#include <QAndroidJniEnvironment>
#include <QAndroidJniObject>
#include <QtAndroid>
using QJniEnvironment = QAndroidJniEnvironment;
using QJniObject = QAndroidJniObject;
#endif

namespace {

const int PickRequestCode = 7001;
const int CreateRequestCode = 7002;

void reportJniException(const char *where)
{
    QJniEnvironment env;
    if (!env->ExceptionCheck()) {
        return;
    }
    env->ExceptionDescribe();
    env->ExceptionClear();
    qWarning() << "[pdfio] JNI exception at" << where;
}

/// Copies what a content:// URI offers into a real file, and returns that path.
QString copyContentToCache(const QString &uri, const QString &cacheFileName)
{
    QJniObject activity = QJniObject::callStaticObjectMethod("org/qtproject/qt5/android/QtNative",
                                                             "activity",
                                                             "()Landroid/app/Activity;");
    if (!activity.isValid()) {
        return QString();
    }

    QJniObject contentResolver = activity.callObjectMethod("getContentResolver",
                                                           "()Landroid/content/ContentResolver;");
    QJniObject juri = QJniObject::callStaticObjectMethod("android/net/Uri", "parse",
                                                         "(Ljava/lang/String;)Landroid/net/Uri;",
                                                         QJniObject::fromString(uri).object<jstring>());
    if (!contentResolver.isValid() || !juri.isValid()) {
        return QString();
    }

    /// openInputStream can throw FileNotFoundException, and a pending exception makes every
    /// later JNI call unreliable, so it is cleared immediately.
    QJniObject stream = contentResolver.callObjectMethod(
        "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;", juri.object());
    reportJniException("ContentResolver.openInputStream");
    if (!stream.isValid()) {
        return QString();
    }

    const QString target =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(cacheFileName);
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        stream.callMethod<void>("close", "()V");
        return QString();
    }

    /// Read the stream through the framework rather than inventing a JNI loop: a small Java
    /// helper on the other side of the bridge does the copying.
    QJniEnvironment env;
    jbyteArray buffer = env->NewByteArray(64 * 1024);
    if (!buffer) {
        file.close();
        stream.callMethod<void>("close", "()V");
        return QString();
    }

    while (true) {
        const jint read = stream.callMethod<jint>("read", "([B)I", buffer);
        if (read <= 0) {
            break;
        }
        QByteArray chunk(int(read), Qt::Uninitialized);
        env->GetByteArrayRegion(buffer, 0, read, reinterpret_cast<jbyte *>(chunk.data()));
        file.write(chunk);
    }

    env->DeleteLocalRef(buffer);
    stream.callMethod<void>("close", "()V");
    file.close();

    return file.size() > 0 ? target : QString();
}

/// Writes a local file into whatever a content:// URI offers, which is how an export reaches the
/// place the user chose. The mirror image of copyContentToCache, and needed for the same reason:
/// the framework speaks in streams, and our renderer and exporter speak in files.
bool writeFileToUri(const QString &localPath, const QString &uri, QString *why)
{
    QJniObject activity = QJniObject::callStaticObjectMethod("org/qtproject/qt5/android/QtNative",
                                                             "activity",
                                                             "()Landroid/app/Activity;");
    if (!activity.isValid()) {
        return false;
    }

    QJniObject contentResolver = activity.callObjectMethod("getContentResolver",
                                                           "()Landroid/content/ContentResolver;");
    QJniObject juri = QJniObject::callStaticObjectMethod("android/net/Uri", "parse",
                                                         "(Ljava/lang/String;)Landroid/net/Uri;",
                                                         QJniObject::fromString(uri).object<jstring>());
    if (!contentResolver.isValid() || !juri.isValid()) {
        return false;
    }

    QJniObject stream = contentResolver.callObjectMethod(
        "openOutputStream", "(Landroid/net/Uri;)Ljava/io/OutputStream;", juri.object());
    reportJniException("ContentResolver.openOutputStream");
    if (!stream.isValid()) {
        if (why) {
            *why = QStringLiteral("the chosen location cannot be written");
        }
        return false;
    }

    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        stream.callMethod<void>("close", "()V");
        if (why) {
            *why = QStringLiteral("cannot read %1").arg(localPath);
        }
        return false;
    }

    QJniEnvironment env;
    jbyteArray buffer = env->NewByteArray(64 * 1024);
    if (!buffer) {
        file.close();
        stream.callMethod<void>("close", "()V");
        return false;
    }

    while (!file.atEnd()) {
        const QByteArray chunk = file.read(64 * 1024);
        env->SetByteArrayRegion(buffer, 0, chunk.size(),
                                reinterpret_cast<const jbyte *>(chunk.constData()));
        stream.callMethod<void>("write", "([BII)V", buffer, jint(0), jint(chunk.size()));
    }

    env->DeleteLocalRef(buffer);
    stream.callMethod<void>("close", "()V");
    file.close();
    return true;
}

} // namespace

struct AndroidDocumentPicker::Private
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
{
};
#else
    : public QAndroidActivityResultReceiver
{
    AndroidDocumentPicker *q = nullptr;
    std::function<void(const QString &, const QString &)> callback;
    std::function<void(bool, const QString &)> written;
    QString pendingLocalFile;

    /// What the picked content is copied to while it is worked on. Set by pickFile() before the
    /// activity starts, because the result callback only ever sees the URI.
    QString pendingCacheName;

    void handleActivityResult(int receiverRequestCode, int resultCode, const QAndroidJniObject &data) override
    {
        if (receiverRequestCode == CreateRequestCode && written) {
            auto done = written;
            written = nullptr;
            const QString local = pendingLocalFile;
            pendingLocalFile.clear();

            if (resultCode != -1 || !data.isValid()) {
                done(false, QStringLiteral("no destination was chosen"));
                return;
            }

            QJniObject uri = data.callObjectMethod("getData", "()Landroid/net/Uri;");
            if (!uri.isValid()) {
                done(false, QStringLiteral("the chosen destination has no URI"));
                return;
            }

            QJniObject text = uri.callObjectMethod("toString", "()Ljava/lang/String;");
            qWarning("[pdfio] writing to %s", qPrintable(text.toString()));

            QString why;
            const bool ok = writeFileToUri(local, text.toString(), &why);
            done(ok, why);
            return;
        }

        if (receiverRequestCode != PickRequestCode || !callback) {
            return;
        }

        auto done = callback;
        callback = nullptr;

        qWarning("[pdfio] picker returned: request %d result %d data %d",
                 receiverRequestCode, resultCode, int(data.isValid()));

        /// Activity.RESULT_OK is -1.
        if (resultCode != -1 || !data.isValid()) {
            done(QString(), QStringLiteral("no file was chosen"));
            return;
        }

        QJniObject uri = data.callObjectMethod("getData", "()Landroid/net/Uri;");
        if (!uri.isValid()) {
            done(QString(), QStringLiteral("the chosen file has no URI"));
            return;
        }

        QJniObject text = uri.callObjectMethod("toString", "()Ljava/lang/String;");
        qWarning("[pdfio] picked uri: %s", qPrintable(text.toString()));

        const QString local = copyContentToCache(text.toString(), pendingCacheName);
        qWarning("[pdfio] copied to %s (%lld bytes)", qPrintable(local),
                 qint64(local.isEmpty() ? 0 : QFileInfo(local).size()));
        if (local.isEmpty()) {
            done(QString(), QStringLiteral("the chosen file could not be copied"));
            return;
        }

        done(local, QString());
    }
};
#endif

AndroidDocumentPicker::AndroidDocumentPicker(QObject *parent)
    : QObject(parent)
    , d(new Private)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    d->q = this;
#endif
}

AndroidDocumentPicker::~AndroidDocumentPicker()
{
    delete d;
}

void AndroidDocumentPicker::pickPdf(std::function<void(const QString &, const QString &)> onPicked)
{
    pickFile(QStringLiteral("application/pdf"), QStringLiteral("pdfio-picked.pdf"), onPicked);
}

void AndroidDocumentPicker::pickBundle(std::function<void(const QString &, const QString &)> onPicked)
{
    /// application/zip rather than a type invented for .pnb: the file really is a zip, so a file
    /// manager that has never heard of .pnb still offers it.
    pickFile(QStringLiteral("application/zip"), QStringLiteral("pdfio-picked.pnb"), onPicked);
}

void AndroidDocumentPicker::pickFile(const QString &mimeType,
                                     const QString &cacheFileName,
                                     std::function<void(const QString &, const QString &)> onPicked)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    Q_UNUSED(mimeType);
    Q_UNUSED(cacheFileName);
    onPicked(QString(), QStringLiteral("the document picker is only wired for the Qt5 Android build"));
#else
    d->callback = onPicked;
    d->pendingCacheName = cacheFileName;

    QJniObject action = QJniObject::fromString(QStringLiteral("android.intent.action.OPEN_DOCUMENT"));
    QJniObject type = QJniObject::fromString(mimeType);

    QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V", action.object<jstring>());
    intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;", type.object<jstring>());
    intent.callObjectMethod("addCategory", "(Ljava/lang/String;)Landroid/content/Intent;",
                            QJniObject::fromString(QStringLiteral("android.intent.category.OPENABLE")).object<jstring>());

    /// Qt hands the result back to the receiver, so no Java of ours is needed to receive it.
    qWarning("[pdfio] launching the document picker");
    QtAndroid::startActivity(intent, PickRequestCode, d);
    reportJniException("startActivity(OPEN_DOCUMENT)");
#endif
}

void AndroidDocumentPicker::createPdf(const QString &suggestedName,
                                      const QString &localFile,
                                      std::function<void(bool, const QString &)> onWritten)
{
    createFile(QStringLiteral("application/pdf"), suggestedName, localFile, onWritten);
}

void AndroidDocumentPicker::createBundle(const QString &suggestedName,
                                         const QString &localFile,
                                         std::function<void(bool, const QString &)> onWritten)
{
    createFile(QStringLiteral("application/zip"), suggestedName, localFile, onWritten);
}

void AndroidDocumentPicker::createFile(const QString &mimeType,
                                       const QString &suggestedName,
                                       const QString &localFile,
                                       std::function<void(bool, const QString &)> onWritten)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    Q_UNUSED(mimeType);
    Q_UNUSED(suggestedName);
    Q_UNUSED(localFile);
    onWritten(false, QStringLiteral("the document writer is only wired for the Qt5 Android build"));
#else
    d->written = onWritten;
    d->pendingLocalFile = localFile;

    QJniObject action = QJniObject::fromString(QStringLiteral("android.intent.action.CREATE_DOCUMENT"));
    QJniObject type = QJniObject::fromString(mimeType);
    QJniObject titleKey = QJniObject::fromString(QStringLiteral("android.intent.extra.TITLE"));
    QJniObject title = QJniObject::fromString(suggestedName);

    QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V", action.object<jstring>());
    intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;", type.object<jstring>());
    intent.callObjectMethod("putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
                            titleKey.object<jstring>(), title.object<jstring>());

    qWarning("[pdfio] launching the document creator");
    QtAndroid::startActivity(intent, CreateRequestCode, d);
    reportJniException("startActivity(CREATE_DOCUMENT)");
#endif
}

#else

struct AndroidDocumentPicker::Private
{
};

AndroidDocumentPicker::AndroidDocumentPicker(QObject *parent)
    : QObject(parent)
    , d(new Private)
{
}

AndroidDocumentPicker::~AndroidDocumentPicker()
{
    delete d;
}

void AndroidDocumentPicker::pickPdf(std::function<void(const QString &, const QString &)> onPicked)
{
    pickFile(QStringLiteral("application/pdf"), QStringLiteral("pdfio-picked.pdf"), onPicked);
}

void AndroidDocumentPicker::createPdf(const QString &suggestedName,
                                      const QString &localFile,
                                      std::function<void(bool, const QString &)> onWritten)
{
    createFile(QStringLiteral("application/pdf"), suggestedName, localFile, onWritten);
}

void AndroidDocumentPicker::pickBundle(std::function<void(const QString &, const QString &)> onPicked)
{
    pickFile(QStringLiteral("application/zip"), QStringLiteral("pdfio-picked.pnb"), onPicked);
}

void AndroidDocumentPicker::createBundle(const QString &suggestedName,
                                         const QString &localFile,
                                         std::function<void(bool, const QString &)> onWritten)
{
    createFile(QStringLiteral("application/zip"), suggestedName, localFile, onWritten);
}

void AndroidDocumentPicker::pickFile(const QString &mimeType,
                                     const QString &cacheFileName,
                                     std::function<void(const QString &, const QString &)> onPicked)
{
    Q_UNUSED(mimeType);
    Q_UNUSED(cacheFileName);
    onPicked(QString(), QStringLiteral("no document picker on this platform"));
}

void AndroidDocumentPicker::createFile(const QString &mimeType,
                                       const QString &suggestedName,
                                       const QString &localFile,
                                       std::function<void(bool, const QString &)> onWritten)
{
    Q_UNUSED(mimeType);
    Q_UNUSED(suggestedName);
    Q_UNUSED(localFile);
    onWritten(false, QStringLiteral("no document writer on this platform"));
}

#endif // Q_OS_ANDROID
