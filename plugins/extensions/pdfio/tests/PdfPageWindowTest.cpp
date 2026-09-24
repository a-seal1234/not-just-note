/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfPageWindow.h"
#include "session/PdfSession.h"
#include "session/PdfSessionManifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

/// A notebook of \a pageCount pages whose sizes and rotations vary the way a scanned book does.
/// The size cycle is six real page formats; the rotation cycle puts every /Rotate value in the
/// manifest, because a page window that only ever sees A4 would not exercise the geometry the
/// manifest carries.
PdfSessionManifest mixedSizeManifest(int pageCount)
{
    static const QSizeF sizes[] = {
        QSizeF(595, 842),    // A4 portrait
        QSizeF(842, 595),    // A4 landscape
        QSizeF(420, 595),    // A5 portrait
        QSizeF(612, 792),    // US Letter
        QSizeF(300, 300),    // a square page
        QSizeF(842, 1191),   // A3 portrait
    };
    static const int rotations[] = { 0, 90, 180, 270 };

    PdfSessionManifest manifest;
    manifest.schema = PdfSessionManifest::CurrentSchema;
    manifest.sourceFile = QStringLiteral("source.pdf");
    manifest.sourceSha256 = QByteArrayLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    manifest.sourceByteSize = 1024;

    for (int i = 0; i < pageCount; ++i) {
        PdfPageRecord page;
        page.index = i;
        page.sizePt = sizes[i % 6];
        page.rotation = rotations[i % 4];
        page.kraFile = PdfSession::pageFileName(i);
        page.thumbFile = PdfSession::thumbFileName(i);
        page.generation = 0;
        manifest.pages.append(page);
    }

    return manifest;
}

QString inkFilePath(const QString &projectDir, const PdfSessionManifest &manifest, int page)
{
    return QDir(projectDir).filePath(manifest.pages.at(page).kraFile);
}

bool writeInk(const QString &path, const QByteArray &ink, QString *why)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        fail(why, QStringLiteral("cannot create %1").arg(QFileInfo(path).absolutePath()));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(why, QStringLiteral("cannot write %1").arg(path));
        return false;
    }
    if (file.write(ink) != ink.size()) {
        fail(why, QStringLiteral("short write to %1").arg(path));
        return false;
    }
    file.close();
    return true;
}

QByteArray readInk(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

/// One page's own ink: unique per page and long enough that a short write would be noticed.
QByteArray inkFor(int page, int generation)
{
    QByteArray ink = QByteArrayLiteral("ink page ") + QByteArray::number(page + 1)
        + QByteArrayLiteral(" generation ") + QByteArray::number(generation) + QByteArrayLiteral(": ");
    ink.append(QByteArray(300 + page, char('a' + (page % 26))));
    return ink;
}

} // namespace

/**
 * The bounded page window, and the rule that keeps it from being a way to lose ink.
 *
 * The unit under test is deliberately the same class the plugin's page switch runs, and the
 * fixtures are the same manifest the plugin writes. What is simulated is the document behind an
 * open page -- the ink is bytes in a file rather than a Krita paint layer -- because KraConverter
 * and KisPart want the whole application, which a bare QTest cannot start.
 */
class PdfPageWindowTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testWindowIsBoundedToCapacity();
    void testFiftyMixedSizePagesNavigateWithinTheBound();
    void testCleanPageIsEvictedWithoutASave();
    void testDirtyPageIsSavedBeforeItIsEvicted();
    void testDirtyPagesAreLeftAloneWhileCleanOnesCanGo();
    void testSaveFailureBlocksEvictionAndKeepsThePageOpen();
    void testEditsSurvivePageSwitchAndReopen();
    void testCapacityNeverDropsBelowOnePage();
};

void PdfPageWindowTest::testWindowIsBoundedToCapacity()
{
    PdfPageWindow window(3);

    for (int page = 0; page < 50; ++page) {
        QString why;
        QVERIFY2(window.open(page, &why), qPrintable(why));
        QVERIFY2(window.openCount() <= 3, "the open-page window grew past its bound");
        QCOMPARE(window.openPages().last(), page);
    }

    QCOMPARE(window.openCount(), 3);
    QCOMPARE(window.openPages(), QList<int>({ 47, 48, 49 }));
    QCOMPARE(window.evictionCount(), 47);
    QCOMPARE(window.savedBeforeEvictionCount(), 0);
    QCOMPARE(window.blockedEvictionCount(), 0);
}

void PdfPageWindowTest::testFiftyMixedSizePagesNavigateWithinTheBound()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const PdfSessionManifest written = mixedSizeManifest(50);
    QString why;
    QVERIFY2(written.writeTo(PdfSession::manifestPath(dir.path()), &why), qPrintable(why));

    /// The navigation runs off the manifest as it comes back from disk, not off the object in
    /// memory: page sizes and the page-to-file map are read from the file the plugin reads.
    const PdfSessionManifest manifest = PdfSessionManifest::readFrom(PdfSession::manifestPath(dir.path()), &why);
    QVERIFY2(manifest.isValid(&why), qPrintable(why));
    QCOMPARE(manifest.pages.size(), 50);

    /// Genuinely mixed: six distinct sizes and all four rotations.
    QSet<QString> sizes;
    QSet<int> rotations;
    for (const PdfPageRecord &page : manifest.pages) {
        sizes.insert(QStringLiteral("%1x%2").arg(page.sizePt.width()).arg(page.sizePt.height()));
        rotations.insert(page.rotation);
    }
    QCOMPARE(sizes.size(), 6);
    QCOMPARE(rotations.size(), 4);

    PdfPageWindow window(3);
    QSet<int> visited;
    for (int page = 0; page < manifest.pages.size(); ++page) {
        QVERIFY2(window.open(page, &why), qPrintable(why));
        QVERIFY2(window.openCount() <= window.capacity(), "the window exceeded its bound mid-navigation");
        QVERIFY(window.isOpen(page));
        visited.insert(page);
    }

    QCOMPARE(visited.size(), 50);
    QCOMPARE(window.evictionCount(), 47);
    QCOMPARE(window.openPages(), QList<int>({ 47, 48, 49 }));

    qInfo("50 mixed-size pages, window bound 3: open %d, evictions %d, saved-before-evict %d, blocked %d",
          window.openCount(), window.evictionCount(), window.savedBeforeEvictionCount(),
          window.blockedEvictionCount());
}

void PdfPageWindowTest::testCleanPageIsEvictedWithoutASave()
{
    PdfPageWindow window(2);

    int saves = 0;
    window.setSaver([&saves](int, QString *) {
        ++saves;
        return true;
    });

    QVERIFY(window.open(0));
    QVERIFY(window.open(1));
    QCOMPARE(window.openCount(), 2);

    QVERIFY(window.open(2));
    QCOMPARE(saves, 0);
    QCOMPARE(window.evictionCount(), 1);
    QVERIFY2(!window.isOpen(0), "the least recently used page should have gone");
    QVERIFY(window.isOpen(1));
    QVERIFY(window.isOpen(2));
}

void PdfPageWindowTest::testDirtyPageIsSavedBeforeItIsEvicted()
{
    PdfPageWindow window(1);

    int saves = 0;
    int savedPage = -1;
    QByteArray savedInk;
    window.setSaver([&](int page, QString *) {
        ++saves;
        savedPage = page;
        savedInk = inkFor(page, 1);
        return true;
    });

    QVERIFY(window.open(0));
    window.setDirty(0);
    QVERIFY(window.isDirty(0));
    QCOMPARE(window.dirtyPages(), QList<int>({ 0 }));

    QVERIFY(window.open(1));
    QCOMPARE(saves, 1);
    QCOMPARE(savedPage, 0);
    QCOMPARE(savedInk, inkFor(0, 1));
    QCOMPARE(window.savedBeforeEvictionCount(), 1);
    QCOMPARE(window.evictionCount(), 1);
    QCOMPARE(window.blockedEvictionCount(), 0);
    QVERIFY(!window.isOpen(0));
    QVERIFY(window.isOpen(1));
    QVERIFY(!window.isOpen(1) || !window.isDirty(1));

    /// The write that bought the eviction is also what clears the mark: what the idle save
    /// walks is exactly the ink that has not reached the disk.
    QCOMPARE(window.dirtyPages(), QList<int>());
}

void PdfPageWindowTest::testDirtyPagesAreLeftAloneWhileCleanOnesCanGo()
{
    /// A window with room for three, where the oldest page carries unsaved ink: the page that
    /// goes has to be the clean one, even though it is newer. The dirty page keeps its slot.
    PdfPageWindow window(3);

    int saves = 0;
    window.setSaver([&saves](int, QString *) {
        ++saves;
        return true;
    });

    QVERIFY(window.open(0));
    window.setDirty(0);
    QVERIFY(window.open(1));
    QVERIFY(window.open(2));

    QVERIFY(window.open(3));
    QCOMPARE(saves, 0);
    QVERIFY2(window.isOpen(0), "a dirty page was evicted while a clean one was available");
    QVERIFY(!window.isOpen(1));
    QCOMPARE(window.openPages(), QList<int>({ 0, 2, 3 }));

    /// And when nothing but dirty pages is left, one save buys one eviction -- the window still
    /// does not have to give up an unsaved page to grow.
    window.setDirty(2);
    window.setDirty(3);
    QVERIFY(window.open(4));
    QCOMPARE(saves, 1);
    QCOMPARE(window.savedBeforeEvictionCount(), 1);
    QVERIFY(window.isOpen(2));
    QVERIFY(window.isOpen(3));
    QVERIFY(window.isOpen(4));
}

void PdfPageWindowTest::testSaveFailureBlocksEvictionAndKeepsThePageOpen()
{
    PdfPageWindow window(1);

    bool saveWorks = false;
    int attempts = 0;
    window.setSaver([&](int, QString *why) {
        ++attempts;
        if (!saveWorks) {
            fail(why, QStringLiteral("no space left on device"));
            return false;
        }
        return true;
    });

    QVERIFY(window.open(0));
    window.setDirty(0);

    QString why;
    QVERIFY2(!window.open(1, &why), "a page turn that would discard unsaved ink has to fail");
    QVERIFY2(!why.isEmpty(), "the refusal has to say why");
    QVERIFY2(why.contains(QStringLiteral("no space left on device")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("stays open")), qPrintable(why));

    QCOMPARE(attempts, 1);
    QCOMPARE(window.evictionCount(), 0);
    QCOMPARE(window.blockedEvictionCount(), 1);
    QCOMPARE(window.openCount(), 1);
    QVERIFY2(window.isOpen(0), "the page that could not be saved must still be open");
    QVERIFY(window.isDirty(0));
    QVERIFY2(!window.isOpen(1), "the new page must not be opened when the eviction was refused");

    /// The same turn goes through once the save can be made; the refusal was not sticky.
    saveWorks = true;
    QVERIFY2(window.open(1, &why), qPrintable(why));
    QCOMPARE(window.evictionCount(), 1);
    QCOMPARE(window.savedBeforeEvictionCount(), 1);
    QVERIFY(!window.isOpen(0));
    QVERIFY(window.isOpen(1));

    /// Without a saver at all the same rule holds: unsaved ink is not evicted.
    PdfPageWindow noSaver(1);
    QVERIFY(noSaver.open(0));
    noSaver.setDirty(0);
    QString noSaverWhy;
    QVERIFY(!noSaver.open(1, &noSaverWhy));
    QVERIFY2(noSaverWhy.contains(QStringLiteral("no saver")), qPrintable(noSaverWhy));
    QVERIFY(noSaver.isOpen(0));
}

void PdfPageWindowTest::testEditsSurvivePageSwitchAndReopen()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString why;
    QVERIFY2(mixedSizeManifest(50).writeTo(PdfSession::manifestPath(dir.path()), &why), qPrintable(why));
    const PdfSessionManifest manifest = PdfSessionManifest::readFrom(PdfSession::manifestPath(dir.path()), &why);
    QVERIFY2(manifest.isValid(&why), qPrintable(why));

    /// The page window is the one the plugin runs: bound one, which is the shipped design A.
    PdfPageWindow window(1);

    QHash<int, QByteArray> drawn;
    int saves = 0;
    window.setSaver([&](int page, QString *savewhy) {
        const QString path = inkFilePath(dir.path(), manifest, page);
        if (!writeInk(path, drawn.value(page), savewhy)) {
            return false;
        }
        ++saves;
        return true;
    });

    const auto draw = [&](int page, int generation) {
        drawn.insert(page, inkFor(page, generation));
        window.setDirty(page);
    };

    /// Forward through the notebook, drawing a different thing on every page. Every page but the
    /// last has to be written on the way out.
    for (int page = 0; page < manifest.pages.size(); ++page) {
        draw(page, 1);
        QVERIFY2(window.open(page, &why), qPrintable(why));
    }
    QCOMPARE(window.openCount(), 1);
    QCOMPARE(window.evictionCount(), 49);
    QCOMPARE(window.savedBeforeEvictionCount(), 49);
    QCOMPARE(saves, 49);
    for (int page = 0; page < 49; ++page) {
        QCOMPARE(readInk(inkFilePath(dir.path(), manifest, page)), drawn.value(page));
    }

    /// The page that is open was never evicted, so it is still only in memory. That is the honest
    /// boundary of the policy: it protects a dirty page from eviction, it does not checkpoint one
    /// that has not been left yet.
    QVERIFY(window.isOpen(49));
    QVERIFY(window.isDirty(49));
    QVERIFY2(!QFileInfo::exists(inkFilePath(dir.path(), manifest, 49)),
             "a page that was never evicted should not have been written yet");

    /// Back through it, and the ink comes back from the file each page was evicted onto. This is
    /// the reopen the plugin does in buildForSinglePage: the page is rebuilt from the source and
    /// PdfInkLoader puts the saved ink back.
    for (int page = manifest.pages.size() - 2; page >= 0; --page) {
        QVERIFY2(window.open(page, &why), qPrintable(why));
        QCOMPARE(readInk(inkFilePath(dir.path(), manifest, page)), drawn.value(page));
    }
    /// Ninety-eight evictions, but only fifty saves: the pages walked back through were written on
    /// the way out the first time, so they are clean now and go for free. The one page still
    /// carrying unsaved ink when the walk turned around is the one that is written here.
    QCOMPARE(window.evictionCount(), 98);
    QCOMPARE(window.savedBeforeEvictionCount(), 50);
    QCOMPARE(saves, 50);

    qInfo("50 pages drawn on and turned: evictions %d, saves before eviction %d, files on disk %d",
          window.evictionCount(), window.savedBeforeEvictionCount(), int(drawn.size()));
    for (int page = 0; page < manifest.pages.size(); ++page) {
        QVERIFY(QFileInfo::exists(inkFilePath(dir.path(), manifest, page)));
    }

    /// Process death: nothing from the running session is kept but the project directory, and the
    /// manifest is read again from disk the way PdfSession::openProject reads it.
    const PdfSessionManifest reopened = PdfSessionManifest::readFrom(PdfSession::manifestPath(dir.path()), &why);
    QVERIFY2(reopened.isValid(&why), qPrintable(why));
    PdfPageWindow freshWindow(1);
    for (int page = 0; page < reopened.pages.size(); ++page) {
        QVERIFY2(freshWindow.open(page, &why), qPrintable(why));
        QCOMPARE(readInk(inkFilePath(dir.path(), reopened, page)), drawn.value(page));
    }
    QCOMPARE(freshWindow.evictionCount(), 49);

    /// A save that cannot be made leaves the last edit on disk untouched rather than truncating it.
    QByteArray lastEdit = drawn.value(0);
    PdfPageWindow refusingWindow(1);
    refusingWindow.setSaver([](int, QString *refusewhy) {
        fail(refusewhy, QStringLiteral("read-only file system"));
        return false;
    });
    QVERIFY(refusingWindow.open(0));
    refusingWindow.setDirty(0);
    QVERIFY(!refusingWindow.open(1, &why));
    QCOMPARE(readInk(inkFilePath(dir.path(), manifest, 0)), lastEdit);
}

void PdfPageWindowTest::testCapacityNeverDropsBelowOnePage()
{
    PdfPageWindow window(0);
    QCOMPARE(window.capacity(), 1);

    window.setCapacity(-5);
    QCOMPARE(window.capacity(), 1);

    QVERIFY(window.open(0));
    QVERIFY(window.open(1));
    QCOMPARE(window.openCount(), 1);
    QVERIFY(window.isOpen(1));

    /// Shrinking keeps what is open until the next page needs the room; it does not evict from
    /// under the caller.
    PdfPageWindow three(3);
    QVERIFY(three.open(0));
    QVERIFY(three.open(1));
    QVERIFY(three.open(2));
    three.setCapacity(1);
    QCOMPARE(three.openCount(), 3);
    QCOMPARE(three.evictionCount(), 0);
    QVERIFY(three.open(3));
    QCOMPARE(three.openCount(), 1);
    QVERIFY(three.isOpen(3));
}

QTEST_GUILESS_MAIN(PdfPageWindowTest)
#include "PdfPageWindowTest.moc"
