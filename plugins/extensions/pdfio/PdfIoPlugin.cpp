/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QCoreApplication>
#include "AndroidDocumentPicker.h"
#include "PdfIoDocker.h"
#include "PdfIoNotebookActions.h"
#include "PdfIoPlugin.h"

#include "backends/image/ImageRenderBackend.h"
#include "PdfIoProbe.h"
#include "PdfNotebookOpsDialog.h"
#include "PdfNotebookResizeDialog.h"
#include "PdfPageNavigator.h"
#include "PdfRendererSpike.h"

#include <cstdio>
#include <functional>
#include <limits>
#include <unistd.h>

#include <QActionGroup>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>

/// Not behind PDFIO_HAVE_POPPLER: the session, the saver, the ink loader and the exporter are all
/// plain C++ and are built on every platform. Only the renderer differs, and that is chosen by
/// PdfRenderBackend::create.
#include "backend/PdfRenderBackend.h"
#include "session/PdfExporter.h"
#include "session/PdfInkLoader.h"
#include "session/PdfNotebookBundle.h"
#include "session/PdfNotebookOps.h"
#include "session/PdfPageRotator.h"
#include "session/PdfPageSaver.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfStripBuilder.h"
#include "session/PdfStripLayout.h"
#include "session/PdfSession.h"

#include <QHash>
#include <QImage>
#include <QPixmap>

#include <KoDocumentInfo.h>

#include <kis_canvas2.h>
#include <kis_coordinates_converter.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <KisView.h>

#include <KoColor.h>

#include <QDateTime>
#include <QFileDialog>
#include <QStandardPaths>

#include <kis_group_layer.h>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisViewManager.h>
#include <KisWelcomePageWidget.h>
#include <kis_canvas_controller.h>
#include <kis_node_manager.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <klocalizedstring.h>
#include <kpluginfactory.h>

K_PLUGIN_FACTORY_WITH_JSON(PdfIoPluginFactory, "kritapdfio.json", registerPlugin<PdfIoPlugin>();)

namespace {

void say(const QString &message)
{
    /// Both sinks: see the note in PdfIoProbe about desktop versus Android.
    fprintf(stderr, "[pdfio] %s\n", qPrintable(message));
    fflush(stderr);
    qWarning("[pdfio] %s", qPrintable(message));
}

/// Resident set size in kilobytes, from /proc: the number that decides whether a notebook can
/// stay open on a tablet.
qint64 residentKb()
{
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (!statm.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
    if (fields.size() < 2) {
        return -1;
    }
    return fields.at(1).toLongLong() * (sysconf(_SC_PAGESIZE) / 1024);
}

/// Puts the notebook list on the Start screen; defined with the open helpers far below, and
/// declared here because the plugin constructor uses it.
void refreshWelcomePageEntries();

/// Shared entry point for pixel resizing, also called from the Manage Pages screen.
void resizeNotebookByPixels(bool batch);

/// The notebook's name as it is on disk right now. Read from the manifest rather than from the
/// navigator: the navigator holds the copy it loaded when the notebook was opened, and a rename is
/// written after that, so its copy is one rename behind.
QString notebookNameFromDisk(const QString &projectDir)
{
    if (projectDir.isEmpty()) {
        return QString();
    }
    QString why;
    const PdfSessionManifest manifest =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(projectDir), &why);
    if (!manifest.isValid()) {
        return QString();
    }

    /// Through the one rule, never through displayName() alone: that falls back to the source file's
    /// base name, which on Android is the picker's cache copy -- the name this used to put on the
    /// Start screen, in the tab and in every export suggestion.
    return PdfPageNavigator::usableNotebookName(manifest.name);
}

/// A name that can also be offered as a file name: printable, without path separators, bounded.
/// The manifest may have been written by another tool or edited by hand, so the rule is applied on
/// the way out rather than trusted.
QString nameForFile(const QString &name)
{
    /// 92 is the backslash, written as a code so the intent cannot be lost in an escape.
    const QChar backslash = QLatin1Char(92);

    QString clean;
    for (const QChar character : name) {
        if (character.isPrint() && character != QLatin1Char('/') && character != backslash) {
            clean.append(character);
        }
    }
    clean = clean.trimmed();
    if (clean.size() > 120) {
        clean.truncate(120);
    }
    return clean;
}

/// The name the export dialog should offer: the notebook's own name, and the human default when the
/// manifest cannot be read. Never the source file's name -- on Android that is the picker's cache
/// copy, and suggesting "pdfio-picked-notes.pdf" is how a name nobody chose reaches a file the user
/// hands to someone else.
QString exportSuggestion(PdfPageNavigator *navigator)
{
    QString base = notebookNameFromDisk(navigator->projectDir());
    if (base.isEmpty()) {
        base = PdfPageNavigator::defaultNotebookName();
    }
    base = nameForFile(base);
    if (base.isEmpty()) {
        base = QStringLiteral("notebook");
    }
    return base + QStringLiteral("-notes.pdf");
}

/// Puts the open notebook's name on every docker. The docker reads the manifest itself; this is
/// what makes a rename visible without waiting for the next page turn.
void reloadDockerNames()
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    if (!window) {
        return;
    }
    const QList<PdfIoDocker *> dockers = window->findChildren<PdfIoDocker *>();
    for (PdfIoDocker *docker : dockers) {
        docker->reloadNotebookName();
    }
}

/// Puts the notebook panel up when a notebook is opened.
///
/// The panel is where the notebook's pages are picked from: a notebook with no way to choose a page
/// is a canvas with a title on it. A user who has not been to Settings > Dockers > Notebook would
/// otherwise open a notebook and see none of it. Shown only while it is hidden, so closing it stays
/// a choice -- it comes back when the next notebook is opened, the way the canvas does.
void showNotebookPanel()
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    if (!window) {
        return;
    }
    const QList<PdfIoDocker *> dockers = window->findChildren<PdfIoDocker *>();
    for (PdfIoDocker *docker : dockers) {
        if (!docker->isVisible()) {
            docker->show();
            docker->raise();
        }
    }
}

/// The recent list lives with the notebook store rather than here: removing a notebook has to
/// forget its entry at the same moment, and one place has to know where the list is kept. What is
/// here is the policy -- what an entry's name is, which entries are dead, when to refresh the Start
/// screen -- and the menus around it.
QStringList recentNotebookEntries()
{
    return PdfPageNavigator::recentNotebooks();
}

void writeRecentNotebooks(const QStringList &entries)
{
    PdfPageNavigator::setRecentNotebooks(entries);
}

QString recentNotebookDir(const QString &entry)
{
    return PdfPageNavigator::recentNotebookDir(entry);
}

/// Remembers the open notebook as the most recent one. An older entry for the same project
/// directory is dropped, so a renamed notebook appears once, at the top, under its new name.
void rememberRecentNotebook()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook() || navigator->projectDir().isEmpty()) {
        return;
    }

    const QString dir = QFileInfo(navigator->projectDir()).absoluteFilePath();
    QString name = notebookNameFromDisk(navigator->projectDir());
    if (name.isEmpty()) {
        /// An unreadable manifest. The human default, never the source file's name: on Android that
        /// is the picker's cache copy, and a Recent entry reading "pdfio-picked" is the bug this
        /// rule exists to stop.
        name = PdfPageNavigator::defaultNotebookName();
    }

    QStringList entries = recentNotebookEntries();
    for (int i = entries.size() - 1; i >= 0; --i) {
        const QString existing = recentNotebookDir(entries.at(i));
        if (existing.isEmpty()
            || QFileInfo(existing).absoluteFilePath() == dir) {
            entries.removeAt(i);
        }
    }
    entries.prepend(dir + PdfPageNavigator::recentNotebookSeparator() + name);
    writeRecentNotebooks(entries);
    refreshWelcomePageEntries();
}

/// Puts the notebook's name on the open document's tab; defined with the other name helpers below.
/// Declared HERE, above notebookOpened(), which calls it: the declaration used to sit below that call
/// and the compiler was right to refuse it.
void applyNotebookNameToTab();

/// What every successful open does, and nothing more: settle the notebook's name, remember it, and
/// put its name on the docker and the tab. Deliberately called after the open -- the picker's copy
/// step stays exactly where it is, and nothing here runs inside an activity callback.
///
/// \a defaultTheName is false only on Android's picker path, where the provider's own name is asked
/// for straight afterwards: a default written first would make the notebook look named and the
/// provider's answer would then be refused. The rule itself -- what counts as a name a person chose,
/// and what a notebook is called when it has none -- lives in the navigator, one place for every
/// reader and writer: PdfPageNavigator::ensureNotebookName().
void notebookOpened(bool defaultTheName = true)
{
    QString namedWhy;
    if (!PdfPageNavigator::instance()->ensureNotebookName(defaultTheName, &namedWhy)) {
        say(QStringLiteral("the notebook's name could not be settled: %1").arg(namedWhy));
    }
    rememberRecentNotebook();
    reloadDockerNames();
    applyNotebookNameToTab();
    showNotebookPanel();
}

/// Puts the notebook list on the Start screen; defined with the open helpers below, and declared
/// here because the recent-list bookkeeping above it refreshes that screen.
void refreshWelcomePageEntries();

#if defined(Q_OS_ANDROID)
/// Asks the provider for the name of the PDF that was just imported, on the event loop and after
/// the notebook is open -- never inside the activity callback, which is where that query crashed.
///
/// It is written only while the notebook still has no name a person chose, so a name the user set
/// with "Rename notebook..." is never overwritten -- and when the provider cannot answer (no URI, no
/// provider, no column, no value) the notebook is given the human default rather than the cache name
/// the picker copied the PDF under, which is what it used to keep.
void adoptProviderName(const QString &contentUri)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        return;
    }

    const QString providerName = contentUri.isEmpty()
        ? QString()
        : AndroidDocumentPicker::displayNameForContentUri(contentUri);
    if (providerName.isEmpty()) {
        /// Nothing to borrow: settle a name a person can live with, through the one rule.
        QString namedWhy;
        if (!navigator->ensureNotebookName(true, &namedWhy)) {
            say(QStringLiteral("the provider gave no name and the default could not be written: %1")
                    .arg(namedWhy));
        }
        return;
    }

    const QString path = PdfSession::manifestPath(navigator->projectDir());
    QString why;
    PdfSessionManifest manifest = PdfSessionManifest::readFrom(path, &why);
    if (!manifest.isValid()) {
        say(QStringLiteral("cannot read the notebook's manifest: %1").arg(why));
        return;
    }

    /// A name a provider gave may replace one the application made up for a cache copy -- that is not
    /// a name the user chose -- and never a name typed with "Rename notebook...". The rule is the
    /// navigator's, one place, so this cannot drift into a string comparison of its own.
    if (!manifest.name.isEmpty() && !PdfPageNavigator::isInternalNotebookName(manifest.name)) {
        say(QStringLiteral("the notebook already has the name \"%1\"; the provider's \"%2\" was left alone")
                .arg(manifest.name, providerName));
        return;
    }

    QString clean = nameForFile(providerName);
    /// The provider usually offers the name with its extension. The notebook is named without it,
    /// the way the desktop path names one, so the export suggestion does not read
    /// "<name>.pdf-notes.pdf".
    if (clean.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        clean.chop(4);
    }
    if (clean.isEmpty()) {
        /// The provider answered, but with nothing a file name can carry. Same rule as no answer.
        QString namedWhy;
        if (!navigator->ensureNotebookName(true, &namedWhy)) {
            say(QStringLiteral("the provider's name was unusable and the default could not be written: %1")
                    .arg(namedWhy));
        }
        return;
    }

    manifest.name = clean;
    if (!manifest.writeTo(path, &why)) {
        say(QStringLiteral("cannot write the notebook's name: %1").arg(why));
        return;
    }

    say(QStringLiteral("the notebook is named \"%1\" after the PDF that was imported").arg(clean));
    reloadDockerNames();
    applyNotebookNameToTab();
    rememberRecentNotebook();
}
#endif

/**
 * The one range dialog: from page and to page, inside the pages that are available.
 *
 * Both entries that need a range use it -- inserting pages asks within the PDF being inserted,
 * extracting asks within the notebook -- so the two cannot drift into two UIs with two different
 * ideas of what a range is.
 *
 * \a first comes back zero-based, the way the operations count pages, and \a count is how many.
 */
bool askForPageRange(const QString &title, const QString &label, int available, int *first, int *count)
{
    if (available < 1 || !first || !count) {
        return false;
    }

    QDialog dialog(nullptr);
    dialog.setWindowTitle(title);
    auto *layout = new QVBoxLayout(&dialog);
    auto *question = new QLabel(label, &dialog);
    question->setWordWrap(true);
    layout->addWidget(question);

    auto *from = new QSpinBox(&dialog);
    from->setRange(1, available);
    from->setValue(1);
    auto *to = new QSpinBox(&dialog);
    to->setRange(1, available);
    to->setValue(available);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(i18n("From page"), &dialog));
    row->addWidget(from);
    row->addWidget(new QLabel(i18n("to"), &dialog));
    row->addWidget(to);
    layout->addLayout(row);

    /// Two boxes describe one range, so they are kept consistent: moving one past the other takes
    /// the other with it rather than leaving a range that reads backwards.
    QObject::connect(from, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [from, to](int value) {
        if (to->value() < value) {
            to->setValue(value);
        }
    });
    QObject::connect(to, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [from](int value) {
        if (from->value() > value) {
            from->setValue(value);
        }
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    *first = from->value() - 1;
    *count = to->value() - from->value() + 1;
    return true;
}

/// Opens \a pdfPath, replacing whatever notebook is open.
///
/// A page that is already open is closed first and the open is deferred until that close has
/// happened -- the same shape rebuildForScope() uses, and for the same reason: opening the new
/// notebook while the old document's view is still alive leaves the import with nothing on screen,
/// which is what "importing while a page is open opens nothing" was. \a contentUri is the Android
/// picker's URI when the open came from the picker, and empty otherwise; when it is there the
/// provider's own name is adopted instead of the cache-name default.
void openNotebookReplacing(const QString &pdfPath,
                           const QString &contentUri,
                           int attemptsLeft,
                           std::function<void()> then = nullptr)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();

    /// The first screen may have no main window yet, and the view a notebook opens into is made on
    /// it. From the first screen the import then had nowhere to appear; waiting for the window is
    /// the same retry the menu registration already does, rather than importing into nothing.
    if (!KisPart::instance()->currentMainwindow() && attemptsLeft > 0) {
        say(QStringLiteral("no main window yet; opening the notebook when one arrives"));
        QTimer::singleShot(500, navigator, [pdfPath, contentUri, attemptsLeft, then]() {
            openNotebookReplacing(pdfPath, contentUri, attemptsLeft - 1, then);
        });
        return;
    }

    /// The document that is open is still in the way while Krita closes it, and the close is
    /// deferred, so this waits rather than spinning: each attempt gives the event loop 700 ms.
    if (navigator->currentDocument() && attemptsLeft > 0) {
        /// WHAT IS STILL UNSAVED IS WRITTEN HERE, before the document is allowed to close.
        ///
        /// This used to clear the modified flag and leave it at that, with a comment saying the ink
        /// had been written already. It had not: the view's own pre-close handler asks
        /// prepareForClose(), sees a clean document and returns without touching the notebook, so
        /// importing straight after drawing silently dropped the stroke. The gate is the
        /// navigator's own -- every page the open document holds, waited for -- and a write that
        /// cannot be made REFUSES the open and leaves the notebook where it is, which is the one
        /// outcome that cannot lose ink. With nothing unsaved the gate is not asked at all, so a
        /// clean notebook still closes as cheaply as it did.
        KisDocument *document = navigator->currentDocument();
        if (document->isModified()) {
            QString saveWhy;
            if (!navigator->prepareForNotebookChange(&saveWhy)) {
                say(QStringLiteral("refusing to replace the open notebook: it could not be written "
                                   "(%1)").arg(saveWhy));
                QMessageBox::warning(nullptr, i18n("Import PDF as notebook"),
                                     i18n("The notebook that is open could not be written, so it was "
                                          "left as it is and %1 was not opened: %2", pdfPath, saveWhy));
                return;
            }
        }
        if (KisView *view = navigator->currentView()) {
            view->closeView();
        }
        QTimer::singleShot(700, navigator, [pdfPath, contentUri, attemptsLeft, then]() {
            openNotebookReplacing(pdfPath, contentUri, attemptsLeft - 1, then);
        });
        return;
    }

    QString why;
    if (!navigator->openNotebook(pdfPath, &why)) {
        say(QStringLiteral("could not open the notebook: %1").arg(why));
        return;
    }

#if defined(Q_OS_ANDROID)
    /// From the picker the cache name is not written as the notebook's name: the provider's own
    /// name is asked for below, and the manifest reader already falls back to the cache name.
    const bool fromPicker = !contentUri.isEmpty();
    notebookOpened(!fromPicker);
    if (fromPicker) {
        adoptProviderName(contentUri);
    }
#else
    Q_UNUSED(contentUri);
    notebookOpened();
#endif

    if (then) {
        then();
    }
}

/// Opens the notebook that lives at \a projectDir, replacing whatever notebook is open.
///
/// By DIRECTORY, not by source: a range extracted from a notebook carries the same PDF as the
/// notebook it came from, and openNotebook() keys a project by the source's own hash -- so a source
/// path would find the notebook that already exists instead of the one that was just made. Every
/// path that remembers a notebook (Recent notebooks, the Start screen, an extraction) stores the
/// directory, and this is the door that opens what it stored.
///
/// The close and the deferral are openNotebookReplacing()'s, for the same reason.
void openProjectDirReplacing(const QString &projectDir, int attemptsLeft,
                              std::function<void()> then = nullptr)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();

    if (!KisPart::instance()->currentMainwindow() && attemptsLeft > 0) {
        say(QStringLiteral("no main window yet; opening the notebook when one arrives"));
        QTimer::singleShot(500, navigator, [projectDir, attemptsLeft, then]() {
            openProjectDirReplacing(projectDir, attemptsLeft - 1, then);
        });
        return;
    }

    if (navigator->currentDocument() && attemptsLeft > 0) {
        /// The same write-before-close gate openNotebookReplacing() uses, and for the same reason:
        /// the ops paths run prepareForNotebookChange() before they ask for an open, but Recent
        /// notebooks and the Start screen come straight here, and clearing the modified flag
        /// without writing is how a notebook closed on ink that had never reached disk.
        KisDocument *document = navigator->currentDocument();
        if (document->isModified()) {
            QString saveWhy;
            if (!navigator->prepareForNotebookChange(&saveWhy)) {
                say(QStringLiteral("refusing to replace the open notebook: it could not be written "
                                   "(%1)").arg(saveWhy));
                QMessageBox::warning(nullptr, i18n("Open a notebook"),
                                     i18n("The notebook that is open could not be written, so it was "
                                          "left as it is and %1 was not opened: %2",
                                          projectDir, saveWhy));
                return;
            }
        }
        if (KisView *view = navigator->currentView()) {
            view->closeView();
        }
        QTimer::singleShot(700, navigator, [projectDir, attemptsLeft, then]() {
            openProjectDirReplacing(projectDir, attemptsLeft - 1, then);
        });
        return;
    }

    QString why;
    if (!navigator->openNotebookDir(projectDir, &why)) {
        say(QStringLiteral("could not open the notebook at %1: %2").arg(projectDir, why));
        QMessageBox::warning(nullptr, i18n("Open a notebook"),
                             i18n("%1 could not be opened as a notebook: %2", projectDir, why));
        return;
    }

    /// The same bookkeeping every open does: name it if it has none, remember it, title the docker.
    notebookOpened();
    if (then) {
        then();
    }
}

/// Puts the notebook's name on the open document's tab as well, so the tab agrees with the
/// docker, the Recent entry and the export suggestion the moment a name is adopted or changed.
void applyNotebookNameToTab()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    KisDocument *document = navigator->currentDocument();
    if (!document || !navigator->hasNotebook()) {
        return;
    }

    const QString name = notebookNameFromDisk(navigator->projectDir());
    const QString shown = name.isEmpty() ? PdfPageNavigator::defaultNotebookName() : name;
    document->setUntitledCaption(QStringLiteral("%1 - page %2/%3")
                                     .arg(shown)
                                     .arg(navigator->currentIndex() + 1)
                                     .arg(navigator->pageCount()));
}

/// The reason last logged for a notebook's missing page-1 preview, so one state is said once and
/// not on every rebuild of the entries.
QHash<QString, QString> &lastStartScreenReason()
{
    static QHash<QString, QString> reasons;
    return reasons;
}

/// Asks for the one preview a Start-screen entry shows, and only for the notebook that is OPEN.
///
/// A recent entry is usually a notebook the navigator does not have open, and making its preview
/// would mean opening its source PDF and rendering a page as a side effect of building the Start
/// screen -- once per recent entry, on the startup path, which is not a cost a screen may impose.
/// The folder icon is the honest answer there, and refreshWelcomePageEntries() says in the log
/// which page it was and which of the two reasons it was.
///
/// The notebook that IS open needs no such machinery: its sources are parsed, one page is a
/// bounded render, and the ask goes through the notebook's own queue -- the same ask the docker
/// and the ops screen already make -- so a missing page-1 preview comes back without the user
/// having to do anything. The caller asks only when the file is MISSING, so this queues exactly one
/// render for the one page the entry shows.
void askForStartScreenPreview(const QString &dir)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook() || navigator->projectDir().isEmpty()) {
        return;
    }
    if (QFileInfo(navigator->projectDir()).absoluteFilePath()
        != QFileInfo(dir).absoluteFilePath()) {
        return;
    }
    navigator->ensureThumbnail(0);
}

/// Once per process: the Start screen can be built while a page's preview is missing -- the entries
/// are rebuilt on the way out of the very open that asks for the new one -- and page 1 is the only
/// page a recent entry shows. So a page-1 preview landing is the moment to rebuild the entries.
///
/// The rebuild only asks when the file is MISSING, so an ask cannot answer immediately and the
/// signal cannot come straight back here; refreshWelcomePageEntries() drops a nested call as well,
/// for the case where the navigator's own record differs from the one on disk.
void hookStartScreenPreviewOnce()
{
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;

    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QObject::connect(navigator, &PdfPageNavigator::thumbnailReady, navigator, [](int index) {
        if (index == 0) {
            refreshWelcomePageEntries();
        }
    });
}

/// Puts the notebook list on the Start screen's Recent Images list.
///
/// Krita's own recent documents are left alone: the welcome page appends these beside them through
/// a proxy of its own, and a click comes back here as the project directory. A notebook whose
/// directory or manifest is gone is dropped on this pass, so it disappears from the screen on the
/// next refresh rather than lingering as a dead row; one with no page-1 preview gets a plain icon,
/// and that plain icon has two causes which only the log can tell apart -- see below.
void refreshWelcomePageEntries()
{
    /// A nested call is dropped: asking the navigator for a preview can answer immediately when the
    /// file is already there, and that answer arrives back here as a refresh of the list this call
    /// is still building. The outer call's entries are the ones that describe the disk.
    static bool refreshing = false;
    if (refreshing) {
        return;
    }
    refreshing = true;

    hookStartScreenPreviewOnce();

    QList<KisWelcomePageWidget::ExtraRecentEntry> entries;
    /// The directories this pass really looked at, so a reason remembered for a notebook that has
    /// left the recent list is forgotten with it.
    QStringList looked;
    for (const QString &entry : recentNotebookEntries()) {
        const QString dir = recentNotebookDir(entry);
        looked << dir;
        QString why;
        const PdfSessionManifest manifest =
            PdfSessionManifest::readFrom(PdfSession::manifestPath(dir), &why);
        if (!manifest.isValid()) {
            continue;
        }

        KisWelcomePageWidget::ExtraRecentEntry extra;
        extra.name = entry.section(PdfPageNavigator::recentNotebookSeparator(), 1);
        if (extra.name.isEmpty()) {
            extra.name = manifest.displayName();
        }

        if (manifest.pages.isEmpty()) {
            /// Keep an empty notebook in Recents without inventing a page-one preview.
            lastStartScreenReason().remove(dir);
            extra.thumbnailPath = QString();
        } else {
            /// The picture comes from PAGE 1'S OWN RECORD, never from a name guessed out of the page's
            /// position. A preview's name is handed out by the notebook's allocator and travels with
            /// the page, so "thumbs/p0001.png" is the shape of the names this build happens to write and
            /// not a rule the manifest obeys. Worse, a guessed path that is absent and a page that
            /// records no preview are the SAME folder icon on screen -- which is how a notebook whose
            /// preview was dropped came to look like a notebook that never had one. The two are told
            /// apart here, in the log, because the icon cannot say it.
            const PdfPageRecord &first = manifest.pages.first();
            const QString thumbnail = PdfPageNavigator::thumbnailPathFor(dir, first);
            const bool present = !thumbnail.isEmpty() && QFileInfo::exists(thumbnail);
            if (!present) {
                /// Which of the two causes it is, said ONCE per state rather than on every rebuild: a
                /// page-1 save rebuilds these entries too, and a line repeated for every rebuild is a
                /// line nobody reads. The state is the pair (this notebook, this reason), and it is
                /// cleared when the picture is there, so losing it again is said again.
                const QString reason = first.thumbFile.isEmpty()
                    ? QStringLiteral("page 1 records no preview name yet")
                    : QStringLiteral("page 1's recorded preview %1 is not on disk").arg(first.thumbFile);
                if (lastStartScreenReason().value(dir) != reason) {
                    lastStartScreenReason().insert(dir, reason);
                    say(QStringLiteral("Start screen: %1 shows the plain icon: %2").arg(dir, reason));
                }
                askForStartScreenPreview(dir);
            } else {
                lastStartScreenReason().remove(dir);
            }
            extra.thumbnailPath = present ? thumbnail : QString();
        }
        extra.token = dir;
        entries.append(extra);
    }

    for (const QString &known : lastStartScreenReason().keys()) {
        if (!looked.contains(known)) {
            lastStartScreenReason().remove(known);
        }
    }

    KisWelcomePageWidget::setExtraRecentEntries(
        entries, [](const QString &projectDir) {
            QString why;
            const PdfSessionManifest manifest =
                PdfSessionManifest::readFrom(PdfSession::manifestPath(projectDir), &why);
            if (!manifest.isValid()) {
                say(QStringLiteral("that recent notebook can no longer be read: %1").arg(why));
                return;
            }
            /// The same close-first path the Recent notebooks menu uses -- and by DIRECTORY, so a
            /// notebook made by extracting a range opens as itself rather than as the notebook it
            /// was extracted from.
            openProjectDirReplacing(projectDir, 6);
        });

    refreshing = false;
}

/// Writes \a entered into the open notebook's manifest as its name, with the same rejection the
/// dialog path has, and puts it on the docker. Shared so the menu action and the probe do not
/// drift apart.
bool applyNotebookName(const QString &entered)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        return false;
    }

    const QString clean = nameForFile(entered);
    if (clean.isEmpty()) {
        say(QStringLiteral("that name was refused: it has no printable characters"));
        return false;
    }

    const QString path = PdfSession::manifestPath(navigator->projectDir());
    QString why;
    PdfSessionManifest manifest = PdfSessionManifest::readFrom(path, &why);
    if (!manifest.isValid()) {
        say(QStringLiteral("cannot read the notebook's manifest: %1").arg(why));
        return false;
    }

    manifest.name = clean;
    if (!manifest.writeTo(path, &why)) {
        say(QStringLiteral("cannot write the notebook's name: %1").arg(why));
        return false;
    }

    say(QStringLiteral("the notebook is now named \"%1\"").arg(clean));
    reloadDockerNames();
    applyNotebookNameToTab();
    rememberRecentNotebook();
    refreshWelcomePageEntries();
    return true;
}

void renameNotebook()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        say(QStringLiteral("rename: no notebook is open"));
        QMessageBox::information(nullptr, i18n("Rename notebook"),
                                 i18n("No notebook is open. Import a PDF as a notebook first."));
        return;
    }

    bool accepted = false;
    const QString entered = QInputDialog::getText(nullptr,
                                                  i18n("Rename notebook"),
                                                  i18n("Notebook name:"),
                                                  QLineEdit::Normal,
                                                  notebookNameFromDisk(navigator->projectDir()),
                                                  &accepted);
    if (!accepted) {
        return;
    }

    if (!applyNotebookName(entered)) {
        QMessageBox::warning(nullptr, i18n("Rename notebook"),
                             i18n("The name could not be saved. The notebook keeps the name it had."));
    }
}

/// Opens a notebook from the list, by the directory the list remembered.
///
/// Not by source: two notebooks can draw on one PDF -- a range extracted from a notebook carries
/// the same source as the notebook it came from -- and a source path can only find the one that was
/// made first. The directory is what tells them apart, and it is what Recent has always stored.
void openRecentNotebook(const QString &projectDir)
{
    QString why;
    const PdfSessionManifest manifest =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(projectDir), &why);
    if (!manifest.isValid()) {
        say(QStringLiteral("that recent notebook can no longer be read: %1").arg(why));
        return;
    }

    /// Deferred out of the menu action, and close-first like every other open: opening the new
    /// notebook while the old document and its strip are still alive leaves the OLD strip on
    /// screen, because showPage() then finds the page it is asked for already inside the strip it
    /// is holding and unlocks that slot instead of building the new one. That is the "the tab
    /// changes but the strip does not" seen when switching notebooks with Recent notebooks.
    QTimer::singleShot(0, PdfPageNavigator::instance(), [projectDir]() {
        openProjectDirReplacing(projectDir, 6);
    });
}

void rebuildRecentNotebooks(QMenu *menu)
{
    menu->clear();

    QStringList entries = recentNotebookEntries();
    QStringList alive;
    for (const QString &entry : entries) {
        const QString dir = recentNotebookDir(entry);
        /// A notebook whose directory has gone is dropped rather than offered as a dead click.
        if (dir.isEmpty() || !QFileInfo::exists(PdfSession::manifestPath(dir))) {
            continue;
        }
        alive.append(entry);
    }
    if (alive != entries) {
        writeRecentNotebooks(alive);
    }

    if (alive.isEmpty()) {
        QAction *empty = menu->addAction(i18n("(no notebooks yet)"));
        empty->setEnabled(false);
        return;
    }

    for (const QString &entry : alive) {
        const QString dir = recentNotebookDir(entry);
        const QString name = entry.section(PdfPageNavigator::recentNotebookSeparator(), 1);
        QAction *open = menu->addAction(name.isEmpty() ? dir : name);
        QObject::connect(open, &QAction::triggered, menu, [dir]() { openRecentNotebook(dir); });
    }
}

/// A separator that is removed before it is re-added, like the entries around it: registerActions()
/// runs again for a second view and is retried while the first screen has no window, so an
/// unconditional separator would leave one more behind on every call.
void addMenuSeparator(QMenu *menu, const QString &objectName)
{
    if (!menu) {
        return;
    }
    if (QAction *previous = menu->findChild<QAction *>(objectName)) {
        menu->removeAction(previous);
        previous->deleteLater();
    }
    QAction *separator = menu->addSeparator();
    separator->setObjectName(objectName);
}

/// The runner every entry goes through, defined below; declared here because the insert path,
/// which picks its file first, is written above it.
bool applyNotebookOperation(const QString &title,
                            const std::function<PdfNotebookOps::Outcome(const QString &, int)> &operation);

/// Inserts a range of \a picked after the page that is open.
///
/// After, not before: the page the reader is on does not move, which is what makes the result
/// predictable. Which pages is asked with the one range dialog, the same one the extract entry
/// uses, so "a range" means the same thing in both.
void insertPickedPdf(const QString &picked, const QString &why)
{
    if (picked.isEmpty()) {
        if (!why.isEmpty()) {
            say(QStringLiteral("no PDF was inserted: %1").arg(why));
        }
        return;
    }

    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend || !backend->open(picked)) {
        say(QStringLiteral("%1 could not be opened as a PDF; nothing was inserted").arg(picked));
        QMessageBox::warning(nullptr, i18n("Insert pages from a PDF"),
                             i18n("%1 could not be opened as a PDF, so nothing was inserted.", picked));
        return;
    }

    int first = 0;
    int count = 0;
    if (!askForPageRange(i18n("Insert pages from a PDF"),
                         i18n("%1 has %2 page(s). Which of them should go after page %3 of this "
                              "notebook?",
                              QFileInfo(picked).fileName(), backend->pageCount(),
                              navigator->currentIndex() + 1),
                         backend->pageCount(), &first, &count)) {
        return;
    }

    PdfRenderBackend *renderer = backend.data();
    applyNotebookOperation(i18n("Insert pages from a PDF"),
                           [picked, renderer, first, count](const QString &dir, int page) {
                               return PdfNotebookOps::insertPages(dir, page + 1, picked, *renderer,
                                                                  first, count, page);
                           });
}

/// Writes a range of the open notebook out as a notebook of its own, beside it, and opens it.
///
/// The new notebook is opened BY DIRECTORY: it may share its source PDF with the notebook it came
/// from, and the source path cannot tell the two apart.
void extractPageRange()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        QMessageBox::information(nullptr, i18n("Extract a page range"),
                                 i18n("No notebook is open."));
        return;
    }

    int first = 0;
    int count = 0;
    if (!askForPageRange(
            i18n("Extract a page range"),
            i18n("Notebook \"%1\" has %2 page(s). Which of them should the new notebook hold?",
                 notebookNameFromDisk(navigator->projectDir()), navigator->pageCount()),
            navigator->pageCount(), &first, &count)) {
        return;
    }

    bool accepted = false;
    const QString entered = QInputDialog::getText(
        nullptr, i18n("Extract a page range"), i18n("Name for the new notebook:"), QLineEdit::Normal,
        QStringLiteral("%1 - pages %2-%3")
            .arg(notebookNameFromDisk(navigator->projectDir()))
            .arg(first + 1)
            .arg(first + count),
        &accepted);
    if (!accepted) {
        return;
    }

    const QString name = nameForFile(entered);
    if (name.isEmpty()) {
        QMessageBox::warning(nullptr, i18n("Extract a page range"),
                             i18n("That name has no printable characters, so no notebook was made."));
        return;
    }

    /// The directory is the new notebook's identity -- it is what Recent and the Start screen
    /// remember -- so the name the user typed is the name it is stored under, inside the one
    /// notebook folder.
    const QString destination = QDir(PdfSession::projectRoot()).filePath(name);
    PdfNotebookOps::ExtractOptions options;
    if (QFileInfo::exists(destination)) {
        /// Two notebooks made from the same pages is a decision for a person, and the one already
        /// there may be the one they are working in.
        if (QMessageBox::question(
                nullptr, i18n("Extract a page range"),
                i18n("A notebook called \"%1\" is already there. Replace it?", name))
            != QMessageBox::Yes) {
            return;
        }
        options.replaceExisting = true;
    }

    /// The open pages are written first, exactly as every other operation does, so the range that
    /// is read off the manifest is the one that is on screen. The refusal names the page that could
    /// not be written and what the writer said, in the log and to the user.
    QString why;
    if (!navigator->prepareForNotebookChange(&why)) {
        say(QStringLiteral("extract refused before it started: %1").arg(why));
        QMessageBox::warning(nullptr, i18n("Extract a page range"),
                             i18n("The notebook could not be written, so nothing was extracted: %1",
                                  why));
        return;
    }

    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::extractRange(
        navigator->projectDir(), first, count, destination, options);
    if (!outcome.ok) {
        say(QStringLiteral("extract refused: %1").arg(outcome.why));
        QMessageBox::warning(nullptr, i18n("Extract a page range"), outcome.why);
        return;
    }

    say(outcome.summary);
    /// Deferred, and close-first: the notebook that is open has to go before the new one is built.
    QTimer::singleShot(0, PdfPageNavigator::instance(),
                       [destination]() { openProjectDirReplacing(destination, 6); });
}

/// Merges every page of the notebook at \a sourceDir in, after the page that is open.
///
/// Through the same runner as the other operations, so the open pages are written first and the
/// notebook is reloaded afterwards. What arrives goes AFTER the page the reader is on, so that page
/// does not move -- the outcome's anchor, which the runner opens, says so.
void mergeNotebookFrom(const QString &sourceDir)
{
    applyNotebookOperation(i18n("Merge a notebook in"),
                           [sourceDir](const QString &dir, int page) {
                               return PdfNotebookOps::mergeNotebook(dir, page + 1, sourceDir, page);
                           });
}

/**
 * What the merge chooser answered.
 */
struct MergeTarget {
    enum Kind {
        None,      ///< the chooser was dismissed
        Notebook,  ///< a recent notebook, by its own directory
        File,      ///< a notebook file, to be unpacked
        Folder,    ///< ask for a folder on this device
    };
    Kind kind = None;
    /// The recent notebook's directory, for Kind::Notebook. Empty otherwise.
    QString dir;
};

/**
 * The one merge chooser, for the menu entry and for the Notebook ops screen's merge button.
 *
 * The recent notebooks come first, under their own names. That is what the user asked for ("when
 * merging a note, why not look in the recent notes?") and on a tablet it is the only way a notebook
 * can arrive whole: there is no filesystem to browse. "From a file..." sits beside them for a .pnb,
 * and a folder picker is offered only where folders mean something -- never on Android, where the
 * entry would open a dialog with nothing to show.
 *
 * A list with nothing in it says so on a disabled line of its own rather than standing empty: the
 * file entry below it still works, and the reason the list is bare is visible rather than implied.
 */
MergeTarget chooseMergeTarget()
{
    MergeTarget target;

    QDialog chooser(nullptr);
    chooser.setWindowTitle(i18n("Merge a notebook in"));
    auto *layout = new QVBoxLayout(&chooser);

    auto *intro = new QLabel(
        i18n("Merge the pages of another notebook into this one, after the page you are on."),
        &chooser);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *list = new QListWidget(&chooser);
    list->setObjectName(QStringLiteral("pdfio_merge_chooser_list"));
    list->setMinimumWidth(420);

    /// A notebook whose directory has gone is dropped rather than offered as a dead click, the same
    /// rule the Recent notebooks menu follows.
    const QStringList entries = recentNotebookEntries();
    QStringList alive;
    for (const QString &entry : entries) {
        const QString dir = recentNotebookDir(entry);
        if (dir.isEmpty() || !QFileInfo::exists(PdfSession::manifestPath(dir))) {
            continue;
        }
        alive.append(entry);
    }
    if (alive != entries) {
        writeRecentNotebooks(alive);
    }

    QListWidgetItem *defaultChoice = nullptr;
    if (alive.isEmpty()) {
        auto *empty = new QListWidgetItem(i18n("(no recent notebooks yet)"), list);
        empty->setFlags(Qt::NoItemFlags);
    } else {
        for (const QString &entry : alive) {
            const QString dir = recentNotebookDir(entry);
            const QString name = entry.section(PdfPageNavigator::recentNotebookSeparator(), 1);
            auto *item = new QListWidgetItem(name.isEmpty() ? dir : name, list);
            item->setData(Qt::UserRole, int(MergeTarget::Notebook));
            item->setData(Qt::UserRole + 1, dir);
            item->setToolTip(dir);
            if (!defaultChoice) {
                defaultChoice = item;
            }
        }
    }

    auto *file = new QListWidgetItem(i18n("From a file..."), list);
    file->setData(Qt::UserRole, int(MergeTarget::File));
    if (!defaultChoice) {
        defaultChoice = file;
    }

#if !defined(Q_OS_ANDROID)
    auto *folder = new QListWidgetItem(i18n("From a folder on this device..."), list);
    folder->setData(Qt::UserRole, int(MergeTarget::Folder));
#endif

    layout->addWidget(list, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &chooser);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &chooser, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &chooser, &QDialog::reject);
    layout->addWidget(buttons);
    /// A double click is a choice, the way it is in every file dialog.
    QObject::connect(list, &QListWidget::itemDoubleClicked, &chooser, &QDialog::accept);
    list->setCurrentItem(defaultChoice);

    if (chooser.exec() != QDialog::Accepted) {
        return target;
    }

    QListWidgetItem *chosen = list->currentItem();
    if (!chosen || chosen->flags() == Qt::NoItemFlags) {
        return target;
    }

    target.kind = MergeTarget::Kind(chosen->data(Qt::UserRole).toInt());
    target.dir = chosen->data(Qt::UserRole + 1).toString();
    return target;
}

#if defined(Q_OS_ANDROID)
/**
 * Waits for the Android picker and hands back the local copy it made, or an empty string.
 *
 * The picker answers through an activity-result callback, and the screens that ask for a file ask
 * the way a desktop file dialog answers: synchronously. The wait is a local event loop, which is
 * exactly what a modal QFileDialog is on the desktop. What the ACTIVITY CALLBACK itself does stays
 * the one thing it may do: it copies, stores what it was handed, and posts the wake-up. Opening the
 * file, reading its pages and running the operation all happen after the callback has returned --
 * the rule the crash in that callback taught us.
 */
QString pickedFileOnAndroid(const QString &mimeType, const QString &cacheFileName)
{
    QEventLoop loop;
    QString picked;
    QString why;

    auto *picker = new AndroidDocumentPicker();
    picker->pickFile(mimeType, cacheFileName,
                     [&loop, &picked, &why](const QString &localPath, const QString &reason) {
                         picked = localPath;
                         why = reason;
                         /// Woken from the event loop rather than from here, so that what follows
                         /// the wait cannot run on the activity's own stack.
                         QTimer::singleShot(0, &loop, [&loop]() { loop.quit(); });
                     });
    loop.exec();
    picker->deleteLater();

    if (picked.isEmpty()) {
        say(QStringLiteral("the picker brought nothing back%1")
                .arg(why.isEmpty() ? QString() : QStringLiteral(": ") + why));
    }
    return picked;
}
#endif

/// The path of a PDF the user chose: the file dialog where there is one, the Android picker where
/// there is not. The same terms slotInsertPages() uses -- a PDF is filtered, and what the picker
/// copied is a real file the renderer can open.
QString pickPdfFilePath()
{
#if defined(Q_OS_ANDROID)
    return pickedFileOnAndroid(QStringLiteral("application/pdf"),
                               QStringLiteral("pdfio-picked-pages.pdf"));
#else
    return QFileDialog::getOpenFileName(
        nullptr, i18n("Insert pages from a PDF"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        i18n("PDF documents (*.pdf)"));
#endif
}

/// The path of a notebook FILE the user chose.
///
/// A .pnb has no mime type worth filtering on, so the Android picker is asked for anything at all
/// and what comes back is inspected: a file that is not a notebook is refused with a clear reason
/// rather than half-read.
QString pickNotebookFilePath()
{
#if defined(Q_OS_ANDROID)
    return pickedFileOnAndroid(
        QStringLiteral("*/*"),
        QStringLiteral("pdfio-picked-notebook.") + PdfNotebookBundle::extension());
#else
    return QFileDialog::getOpenFileName(
        nullptr, i18n("Merge a notebook in"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        PdfNotebookBundle::fileFilter());
#endif
}

/// A notebook FOLDER on this device, or false when there is nothing to browse.
///
/// Android has no filesystem to browse and the chooser does not offer a folder there; this says so
/// rather than opening a dialog that cannot work if it is somehow reached.
bool pickNotebookFolderPath(QString *dir)
{
#if defined(Q_OS_ANDROID)
    Q_UNUSED(dir);
    QMessageBox::information(nullptr, i18n("Merge a notebook folder"),
                             i18n("Merging a folder is not available on this device. Merge a "
                                  "recent notebook, or a notebook file."));
    return false;
#else
    const QString picked = QFileDialog::getExistingDirectory(
        nullptr, i18n("Merge a notebook folder"), PdfSession::projectRoot());
    if (picked.isEmpty()) {
        return false;
    }
    *dir = picked;
    return true;
#endif
}

/// Unpacks the notebook file at \a picked into a directory of its own under the notebook folder.
///
/// Read before anything is written: inspect() says what the file holds and refuses what is not a
/// notebook, without unpacking a byte. \a dir is set to the unpacked copy, which the caller owns and
/// removes once the pages it carried are in the notebook's own files -- the .pnb is transport, and
/// the merge itself works on a notebook directory.
bool unpackNotebookFile(const QString &picked, QString *dir, QString *why)
{
    const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(picked, why);
    if (!info.isValid()) {
        return false;
    }

    const QString unpacked =
        QDir(QDir(PdfSession::projectRoot())
                 .filePath(QStringLiteral(".merging-%1").arg(QCoreApplication::applicationPid())))
            .filePath(PdfNotebookBundle::extractDirName(info.manifest));
    QDir(unpacked).removeRecursively();
    if (!PdfNotebookBundle::extract(picked, unpacked, why)) {
        return false;
    }

    *dir = unpacked;
    return true;
}

/// The picker's own copy, forgotten once it has been read.
///
/// Only Android makes one: the chosen content is copied into the application cache because a content
/// URI is a stream and the formats here need a file. On the desktop \a path is the user's own file
/// and is never touched.
void forgetPickedCopy(const QString &path)
{
#if defined(Q_OS_ANDROID)
    QFile::remove(path);
#else
    Q_UNUSED(path);
#endif
}

/// Picks a notebook FILE and merges what is inside it in.
void mergeNotebookFile()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        QMessageBox::information(nullptr, i18n("Merge a notebook in"),
                                 i18n("No notebook is open."));
        return;
    }

    const QString picked = pickNotebookFilePath();
    if (picked.isEmpty()) {
        return;
    }

    QString why;
    QString unpacked;
    if (!unpackNotebookFile(picked, &unpacked, &why)) {
        say(QStringLiteral("that file cannot be merged in: %1").arg(why));
        QMessageBox::warning(nullptr, i18n("Merge a notebook in"), why);
        forgetPickedCopy(picked);
        return;
    }
    forgetPickedCopy(picked);

    /// The copy is removed afterwards whether or not the merge worked: by then the pages it carried
    /// are in the notebook's own files.
    mergeNotebookFrom(unpacked);
    QDir(unpacked).removeRecursively();
}

/// Picks a notebook FOLDER on this device and merges it in.
void mergeNotebookFolder()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        QMessageBox::information(nullptr, i18n("Merge a notebook folder"),
                                 i18n("No notebook is open."));
        return;
    }

    QString dir;
    if (!pickNotebookFolderPath(&dir)) {
        return;
    }

    mergeNotebookFrom(dir);
}

/// The menu's merge entry: the one chooser, for a recent notebook or for a file.
///
/// The entry used to be the .pnb file dialog alone, which is a dead end on a tablet and misses the
/// notebooks the user already has. It is now the same chooser the Notebook ops screen's merge button
/// opens, so the two cannot drift into two different ideas of where a notebook comes from.
void mergeNotebookChosen()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        QMessageBox::information(nullptr, i18n("Merge a notebook in"),
                                 i18n("No notebook is open."));
        return;
    }

    const MergeTarget target = chooseMergeTarget();
    switch (target.kind) {
    case MergeTarget::None:
        return;
    case MergeTarget::Notebook:
        /// A notebook already on this device is merged where it is: no unpacking, no copy.
        mergeNotebookFrom(target.dir);
        return;
    case MergeTarget::File:
        mergeNotebookFile();
        return;
    case MergeTarget::Folder:
        mergeNotebookFolder();
        return;
    }
}

/// Deletes the notebook that is open: closes it, removes its folder, and forgets it everywhere.
///
/// The confirmation is the whole of the safety here, so it says what goes and what does not: the
/// notebook's own folder -- its pages, the ink drawn on them, their previews, and the copies of every
/// PDF it was made from or given pages from -- and then the original PDF or notebook file the user
/// opened, which is their own and is not touched. On Android the notebook opened from a bundle IS
/// that working copy in the store, which is exactly what deleting it removes; the wording says so by
/// naming the copies and the untouched original, without a second string for the platform.
///
/// The order is deliberate. The folder is checked against the store before the dialog, so a notebook
/// that is not ours to delete says so instead of asking; the ink is written before anything is
/// removed, so a removal that fails leaves the notebook complete and usable; the folder is removed
/// while the notebook is still open, so a refusal can leave it open as it was; and only once the
/// folder is gone is the notebook closed, so nothing is left describing a folder that does not exist.
void deleteNotebook()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        QMessageBox::information(nullptr, i18n("Delete notebook"), i18n("No notebook is open."));
        return;
    }

    const QString projectDir = navigator->projectDir();
    const QString folder = QDir::toNativeSeparators(projectDir);

    /// Asked before the dialog, not after: a folder that is not one this store made is not ours to
    /// remove, and saying that is better than confirming a deletion that cannot happen.
    if (!PdfPageNavigator::isInsideNotebookStore(projectDir)) {
        say(QStringLiteral("notebook not deleted: %1 is not inside the notebook store").arg(projectDir));
        QMessageBox::warning(nullptr, i18n("Delete notebook"),
                             i18n("%1 is not inside the notebook store, so nothing was deleted.",
                                  folder));
        return;
    }

    QString name = notebookNameFromDisk(projectDir);
    if (name.isEmpty()) {
        name = QFileInfo(projectDir).fileName();
    }

    QMessageBox confirm(QMessageBox::Warning, i18n("Delete notebook"),
                        i18n("Delete the notebook \"%1\"?", name), QMessageBox::Cancel);
    confirm.setInformativeText(
        i18n("This removes %1 and everything in it: the notebook's pages, the ink drawn on them, "
             "their previews, and the copies of every PDF it was made from or given pages from. "
             "The original PDF or notebook file you opened is your own file and is not touched.",
             folder));
    QPushButton *remove = confirm.addButton(i18n("Delete"), QMessageBox::DestructiveRole);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != remove) {
        say(QStringLiteral("the notebook was not deleted"));
        return;
    }

    /// The ink first: if what follows fails, the notebook is exactly as it was and goes on working.
    if (!navigator->prepareForClose()) {
        say(QStringLiteral("the notebook was not deleted: its open pages could not be written"));
        QMessageBox::warning(nullptr, i18n("Delete notebook"),
                             i18n("The open pages could not be written, so the notebook was not "
                                  "deleted and is still open."));
        return;
    }

    /// The folder goes while the notebook is still open, so a removal that fails leaves the notebook
    /// open and usable rather than closed over a folder that is still there.
    QString why;
    if (!PdfPageNavigator::removeNotebookStore(projectDir, &why)) {
        say(QStringLiteral("the notebook was not deleted: %1").arg(why));
        QMessageBox::warning(nullptr, i18n("Delete notebook"),
                             i18n("The notebook was not deleted: %1", why));
        return;
    }

    /// And now it is closed: its folder is gone, so nothing may go on describing it. The ink was
    /// written a moment ago, so this close has nothing of its own to refuse over.
    QString closeWhy;
    if (!navigator->closeNotebook(&closeWhy)) {
        say(QStringLiteral("the notebook folder was removed but the notebook was not closed: %1")
                .arg(closeWhy));
    }

    /// The store's entry went with the folder (removeNotebookStore() forgets it); the Start screen
    /// shows the same list, and the "Recent notebooks" menu rebuilds itself when it opens.
    refreshWelcomePageEntries();

    say(QStringLiteral("notebook deleted: %1").arg(folder));
}

/// Picks a PDF for the screen's insert button and reads what the screen needs: the path and every
/// page's displayed size, which is what the notebook records.
///
/// The pick is the same on both platforms -- pickPdfFilePath() is a file dialog on the desktop and
/// the Android picker on the tablet -- so the screen's insert works the way the menu's does instead
/// of opening nothing.
bool pickPdfForScreen(PdfNotebookOpsDialog::PdfToAdd *pdf)
{
    const QString picked = pickPdfFilePath();
    if (picked.isEmpty()) {
        return false;
    }

    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend || !backend->open(picked)) {
        say(QStringLiteral("%1 could not be opened as a PDF").arg(picked));
        QMessageBox::warning(nullptr, i18n("Insert pages from a PDF"),
                             i18n("%1 could not be opened as a PDF.", picked));
        return false;
    }

    pdf->path = picked;
    pdf->displayedSizes.clear();
    for (int i = 0; i < backend->pageCount(); ++i) {
        pdf->displayedSizes.append(backend->pageInfo(i).sizePt);
    }
    return !pdf->displayedSizes.isEmpty();
}

/// The path of a PICTURE the user chose, for the screen's "insert a picture as a page".
///
/// On Android the picker copies what was chosen into its cache under the suggested name, so that name
/// carries the extension the notebook records. The EXTENSION IS ONLY A NAME here: every reader in
/// this plugin is chosen by what the manifest says the source is (`kind`) and reads the bytes, never
/// the suffix -- which is what lets a picture that arrived as a JPEG keep a name that says so little.
QString pickImageFilePath()
{
#if defined(Q_OS_ANDROID)
    return pickedFileOnAndroid(QStringLiteral("image/*"),
                               QStringLiteral("pdfio-picked-picture.png"));
#else
    return QFileDialog::getOpenFileName(
        nullptr, i18n("Insert a picture as a page"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        i18n("Pictures (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
#endif
}

/// The name of a sheet a size is, when it is one: "A4", "A5", "Letter". Empty for anything else.
///
/// Only a convenience for the preview text -- a page is its own size either way -- but it is the word
/// a person recognises: a 300 dpi scan of A4 comes back as 595 x 842 points, and "A4" says what
/// 595 x 842 means.
QString knownSheetName(const QSizeF &sizePt)
{
    struct Sheet {
        const char *name;
        QSizeF size;
    };
    const QList<Sheet> sheets = {
        { "A4 portrait", QSizeF(595.28, 841.89) },
        { "A4 landscape", QSizeF(841.89, 595.28) },
        { "A5 portrait", QSizeF(419.53, 595.28) },
        { "Letter portrait", QSizeF(612.0, 792.0) },
        { "Letter landscape", QSizeF(792.0, 612.0) },
    };
    for (const Sheet &sheet : sheets) {
        if (qAbs(sizePt.width() - sheet.size.width()) <= 3.0
            && qAbs(sizePt.height() - sheet.size.height()) <= 3.0) {
            return QString::fromLatin1(sheet.name);
        }
    }
    return QString();
}

/// A size as a person reads it: "595 x 842 pt (210 x 297 mm)" and the sheet's name when it is one.
QString describePageSize(const QSizeF &sizePt)
{
    if (!sizePt.isValid()) {
        return i18n("no size");
    }
    const auto mm = [](qreal points) { return qRound(points * 25.4 / 72.0); };
    const QString sheet = knownSheetName(sizePt);
    return i18n("%1 x %2 pt (%3 x %4 mm)%5",
                qRound(sizePt.width()), qRound(sizePt.height()),
                mm(sizePt.width()), mm(sizePt.height()),
                sheet.isEmpty() ? QString() : QStringLiteral(" -- ") + sheet);
}

/// Asks how big the page should be, as a SCALE of the picture's own pixels. Zero means the user
/// backed out.
///
/// A picture has no size a person can see until something says how big its pixels are: the same scan
/// is a page of A4 at 300 dpi and a poster at 72. So the question is asked at the moment the picture
/// comes in, with the answer shown in points and millimetres AND as the sheet it works out to -- and
/// with two answers that cover what is actually being asked for: "I scanned this at N dpi", which is
/// the true size of the paper, and "make it N% of what it would otherwise be", which is a choice about
/// the page rather than about the scan.
///
/// The last answer is remembered: a notebook is usually a run of pages from one scanner, and retyping
/// 300 for every picture is the kind of thing a dialog should not ask twice.
qreal askImagePageScale(const QSize &pixels)
{
    QDialog dialog;
    dialog.setWindowTitle(i18n("Insert a picture as a page"));

    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *mode = new QComboBox(&dialog);
    mode->setObjectName(QStringLiteral("pdfio_image_size_mode"));
    mode->addItem(i18n("The resolution it was scanned at"));
    mode->addItem(i18n("A share of its natural size"));

    static int lastDpi = 300;
    static int lastPercent = 100;

    auto *dpi = new QSpinBox(&dialog);
    dpi->setObjectName(QStringLiteral("pdfio_image_dpi"));
    dpi->setRange(36, 2400);
    dpi->setValue(lastDpi);
    dpi->setSuffix(i18n(" dpi"));

    auto *percent = new QSpinBox(&dialog);
    percent->setObjectName(QStringLiteral("pdfio_image_percent"));
    percent->setRange(5, 1000);
    percent->setValue(lastPercent);
    percent->setSuffix(QStringLiteral(" %"));

    form->addRow(i18n("Size from:"), mode);
    form->addRow(i18n("Scanned at:"), dpi);
    form->addRow(i18n("Scale:"), percent);
    layout->addLayout(form);

    auto *preview = new QLabel(&dialog);
    preview->setObjectName(QStringLiteral("pdfio_image_size_preview"));
    preview->setWordWrap(true);
    layout->addWidget(preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);

    const auto chosenScale = [&]() -> qreal {
        return mode->currentIndex() == 0 ? ImageRenderBackend::scaleForScannedDpi(dpi->value())
                                        : ImageRenderBackend::scaleForNaturalShare(percent->value());
    };
    const auto chosenSize = [&]() -> QSizeF {
        const qreal scale = chosenScale();
        return QSizeF(pixels.width() * scale, pixels.height() * scale);
    };
    const auto refresh = [&]() {
        const bool scanned = mode->currentIndex() == 0;
        dpi->setEnabled(scanned);
        percent->setEnabled(!scanned);
        preview->setText(i18n("The page will be %1.", describePageSize(chosenSize())));
    };
    /// The int overload spelled out, the way the spin boxes below are: which of a signal's overloads a
    /// connect means has to be said, and the Android build refuses the shorthand the desktop one lets
    /// through.
    QObject::connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
                     [&refresh](int) { refresh(); });
    QObject::connect(dpi, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, [&refresh](int) { refresh(); });
    QObject::connect(percent, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                     [&refresh](int) { refresh(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    refresh();

    if (dialog.exec() != QDialog::Accepted) {
        return 0.0;
    }
    lastDpi = dpi->value();
    lastPercent = percent->value();
    return chosenScale();
}

/// Picks a picture for the screen's own entry and says what size page it makes.
///
/// The file is read here, once, for two reasons: a file that is not a picture is refused BEFORE it
/// joins the screen's working copy, and the page's size comes from the picture's own pixels -- one
/// pixel to one point, the rule ImageRenderBackend reads it by -- so the page is the picture's shape
/// the moment it appears in the list.
bool pickImageForScreen(PdfNotebookOpsDialog::ImageToAdd *picture)
{
    const QString picked = pickImageFilePath();
    if (picked.isEmpty()) {
        return false;
    }

    const QImage loaded(picked);
    if (loaded.isNull() || loaded.width() <= 0 || loaded.height() <= 0) {
        say(QStringLiteral("%1 could not be read as a picture").arg(picked));
        QMessageBox::warning(nullptr, i18n("Insert a picture as a page"),
                             i18n("%1 could not be read as a picture.", picked));
        return false;
    }

    /// And how big a page it becomes, which a picture cannot answer for itself: the same scan is A4
    /// at 300 dpi and a poster at 72. Asked here rather than in the ops screen, because it is a
    /// question about the FILE -- what its pixels mean -- and the screen is about the page list.
    ///
    /// The answer is a SCALE, not a size: the page keeps the picture's own pixels as its size and
    /// says how big it is shown, exactly as Scale mode does for a PDF page -- so an imported picture
    /// can be rescaled afterwards the same way, and the page is rendered at its own resolution when
    /// the reader is at it.
    const qreal scale = askImagePageScale(loaded.size());
    if (scale <= 0.0) {
        return false;
    }

    picture->path = picked;
    picture->sizePt = QSizeF(loaded.width(), loaded.height());
    picture->scale = scale;
    return true;
}

/// Picks a notebook for the screen's merge button and hands over where its files are.
///
/// It opens the SAME chooser the menu's merge entry does -- the recent notebooks first, then a
/// notebook file, and a folder where the platform has them -- so the screen and the menu cannot offer
/// two different ideas of where a notebook comes from. A notebook already on this device is used
/// where it is and \a temporary stays as it was: there is nothing of ours to remove. A .pnb is
/// unpacked into a directory of its own under the notebook folder, and \a temporary is set to it so
/// the caller can remove it once Apply has read every file out of it.
bool pickNotebookForScreen(PdfNotebookOpsDialog::NotebookToMerge *notebook, QString *temporary)
{
    const MergeTarget target = chooseMergeTarget();

    QString dir;
    QString unpackedHere;
    QString why;
    switch (target.kind) {
    case MergeTarget::None:
        return false;
    case MergeTarget::Notebook:
        dir = target.dir;
        break;
    case MergeTarget::Folder:
        if (!pickNotebookFolderPath(&dir)) {
            return false;
        }
        break;
    case MergeTarget::File: {
        const QString picked = pickNotebookFilePath();
        if (picked.isEmpty()) {
            return false;
        }
        if (!unpackNotebookFile(picked, &unpackedHere, &why)) {
            say(QStringLiteral("that file cannot be merged in: %1").arg(why));
            QMessageBox::warning(nullptr, i18n("Merge a notebook in"), why);
            forgetPickedCopy(picked);
            return false;
        }
        forgetPickedCopy(picked);
        dir = unpackedHere;
        break;
    }
    }

    notebook->dir = dir;
    notebook->manifest = PdfSession::openProject(dir, &why);
    if (!notebook->manifest.isValid()) {
        say(QStringLiteral("that notebook cannot be read: %1").arg(why));
        QMessageBox::warning(nullptr, i18n("Merge a notebook in"), why);
        /// Only the transport copy this call made: an earlier pick's directory may still be what the
        /// pending change is reading from.
        if (!unpackedHere.isEmpty()) {
            QDir(unpackedHere).removeRecursively();
        }
        return false;
    }

    if (!unpackedHere.isEmpty()) {
        *temporary = unpackedHere;
    }
    return true;
}

/// Opens the Notebook ops screen: the notebook's pages as one list, applied as one change.
///
/// The screen reads the list as it is on disk, so the pages that are open are written first -- the
/// rule every operation follows -- and then it works on a copy. On Apply the engine commits the
/// whole change once (one journal entry, one manifest write) and the notebook is reloaded once, so
/// the reader ends up on the page they were on rather than wherever the last edit happened to land.
/// Shows the Notebook ops screen as a PAGE of the window rather than as a dialog on top of it.
///
/// It is given the rectangle the canvas had -- the main window's central area -- and a frame with no
/// title bar, so opening it reads as the window turning to another page and Apply or Cancel reads as
/// turning back. The menu, the toolbars, the status bar and the panels stay exactly where they were,
/// and the notebook panel beside it keeps showing the notebook the list is describing.
///
/// It is still exec(): the page is a decision about the notebook's page list, and the canvas
/// underneath must not be poked while that decision is being made.
void showNotebookOpsPage(PdfNotebookOpsDialog &dialog)
{
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);

    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    if (QWidget *central = window ? window->centralWidget() : nullptr) {
        dialog.move(central->mapToGlobal(QPoint(0, 0)));
        dialog.resize(central->size());
    }

    dialog.exec();
}

void openNotebookOpsScreen(PdfIoPlugin *plugin)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        QMessageBox::information(nullptr, i18n("Notebook ops"), i18n("No notebook is open."));
        return;
    }

    /// NO WRITE GATE BEFORE SHOWING THE SCREEN, deliberately.
    ///
    /// The screen changes nothing until Apply, and the manifest it lists the pages from does not
    /// depend on the ink at all. A preflight save before merely SHOWING it is what refused to open
    /// the screen after an import -- "the notebook could not be written" -- so the user could not
    /// reach a screen that had nothing to do with the write. The gate belongs where the change is
    /// APPLIED: applyNotebookOperation() still runs it, and there it names the page and the reason.
    QString why;
    const PdfSessionManifest manifest = PdfSession::openProject(navigator->projectDir(), &why);
    if (!manifest.isValid(&why)) {
        say(QStringLiteral("the notebook ops screen could not read the notebook: %1").arg(why));
        QMessageBox::warning(nullptr, i18n("Notebook ops"),
                             i18n("The notebook could not be read: %1", why));
        return;
    }

    PdfNotebookOpsDialog dialog(navigator->projectDir(), manifest, navigator->currentIndex());

    /// Insert and merge are edits the screen builds itself: it is handed only the picking and the
    /// reading, so a PDF added here and a notebook merged in join the same Apply as everything else.
    PdfNotebookOpsDialog::SourceAdder adder;
    adder.pickAndRead = [](PdfNotebookOpsDialog::PdfToAdd *pdf) { return pickPdfForScreen(pdf); };
    adder.askRange = [](int available, int *first, int *count) {
        return askForPageRange(
            i18n("Insert pages from a PDF"),
            i18n("That PDF has %1 page(s). Which of them should be inserted?", available), available,
            first, count);
    };
    adder.pickImage = [](PdfNotebookOpsDialog::ImageToAdd *picture) {
        return pickImageForScreen(picture);
    };
    dialog.setSourceAdder(adder);

    QString unpackedNotebook;
    PdfNotebookOpsDialog::NotebookMerger merger;
    merger.pickNotebook = [&unpackedNotebook](PdfNotebookOpsDialog::NotebookToMerge *notebook) {
        return pickNotebookForScreen(notebook, &unpackedNotebook);
    };
    dialog.setNotebookMerger(merger);

    showNotebookOpsPage(dialog);

    if (dialog.result() != QDialog::Accepted) {
        /// Cancel discards the pending page-list edits and removes any unpacked transport copy.
        /// Nothing was written for this screen to begin with: the write gate runs when Apply does,
        /// in applyNotebookOperation(), and not before the screen is shown.
        if (!unpackedNotebook.isEmpty()) {
            QDir(unpackedNotebook).removeRecursively();
        }
        return;
    }

    /// Extract writes a notebook of its own, so it stays its own operation and waits for the page
    /// list to be settled rather than being folded into this one.
    if (dialog.requestedAction() == PdfNotebookOpsDialog::ExtractRangeAction) {
        if (!unpackedNotebook.isEmpty()) {
            QDir(unpackedNotebook).removeRecursively();
        }
        extractPageRange();
        return;
    }

    if (dialog.requestedAction() == PdfNotebookOpsDialog::BatchPixelResizeAction) {
        if (!unpackedNotebook.isEmpty()) {
            QDir(unpackedNotebook).removeRecursively();
        }
        resizeNotebookByPixels(true);
        return;
    }

    Q_UNUSED(plugin);

    /// The write gate every other operation runs before it changes anything, and the one this path
    /// was missing.
    ///
    /// Without it the open page's ink is still in the air while the manifest and the artifacts change
    /// under it, and the reload below is then REFUSED -- "the open page still carries ink that is not
    /// on disk; write the notebook before changing it". The notebook IS cropped on disk by then, and
    /// the page on screen goes on showing the sheet it had: the user's own report of a crop that
    /// only took effect after closing the tab and opening the notebook again. The comment above
    /// saying "applyNotebookOperation() still runs it" was about a path this screen does not take.
    if (!navigator->prepareForNotebookChange(&why)) {
        if (!unpackedNotebook.isEmpty()) {
            QDir(unpackedNotebook).removeRecursively();
        }
        say(QStringLiteral("the notebook ops screen was refused before it started: %1").arg(why));
        QMessageBox::warning(nullptr, i18n("Notebook ops"),
                             i18n("The notebook could not be written, so it was not changed: %1", why));
        return;
    }

    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::applyPageEdits(
        navigator->projectDir(), dialog.edits(), PdfPageRotator::rotateInto);
    if (!outcome.ok) {
        /// The .pnb was only unpacked so this change could read its files. It never became part of
        /// the notebook, so an unsuccessful Apply must discard that transport copy as well.
        if (!unpackedNotebook.isEmpty()) {
            QDir(unpackedNotebook).removeRecursively();
        }
        say(QStringLiteral("the notebook ops screen was refused: %1").arg(outcome.why));
        QMessageBox::warning(nullptr, i18n("Notebook ops"), outcome.why);
        return;
    }

    /// The unpacked copy has been read from; the notebook keeps its own files now.
    if (!unpackedNotebook.isEmpty()) {
        QDir(unpackedNotebook).removeRecursively();
    }

    say(outcome.summary);
    if (!navigator->reloadNotebook(dialog.anchorPage(), &why)) {
        say(QStringLiteral("the page list changed, but the notebook could not be reopened: %1")
                .arg(why));
        QMessageBox::warning(nullptr, i18n("Notebook ops"),
                             i18n("The page list was changed, but the notebook could not be opened "
                                  "again: %1",
                                  why));
    }
}

/// Runs one notebook-level operation the way its invariants require: write the open pages first,
/// change the notebook, then reload it and open the page the operation answers with.
///
/// Every entry goes through here so that order cannot drift. The flush is what keeps an operation
/// from being applied to the manifest while ink is still in the air; the reload is what keeps the
/// open document and the strip describing the notebook that is now on disk.
bool applyNotebookOperation(const QString &title,
                            const std::function<PdfNotebookOps::Outcome(const QString &, int)> &operation)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        QMessageBox::information(nullptr, title, i18n("No notebook is open."));
        return false;
    }

    QString why;
    if (!navigator->prepareForNotebookChange(&why)) {
        say(QStringLiteral("notebook operation refused before it started: %1").arg(why));
        QMessageBox::warning(nullptr, title,
                             i18n("The notebook could not be written, so it was not changed: %1", why));
        return false;
    }

    const PdfNotebookOps::Outcome outcome = operation(navigator->projectDir(), navigator->currentIndex());
    if (!outcome.ok) {
        say(QStringLiteral("notebook operation refused: %1").arg(outcome.why));
        QMessageBox::warning(nullptr, title, outcome.why);
        return false;
    }

    say(outcome.summary);
    if (!navigator->reloadNotebook(outcome.anchorPage, &why)) {
        say(QStringLiteral("notebook operation applied, but the notebook could not be reopened: %1").arg(why));
        QMessageBox::warning(nullptr, title,
                             i18n("The notebook was changed, but it could not be opened again: %1", why));
        return false;
    }
    return true;
}

/// The pixel-target actions are intentionally separate from Scale and Box in the page-management
/// canvas: a pixel box is a requested output size, while those modes edit the page's geometry.
void resizeNotebookByPixels(bool batch)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        return;
    }

    const QString title = batch ? i18n("Batch resize pages by pixels")
                                : i18n("Resize page by pixels");
    PdfNotebookResizeDialog dialog(navigator->manifest().pages, navigator->currentIndex(), batch);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const PdfNotebookResizeDialog::Request request = dialog.request();
    if (request.pageIndices.isEmpty()) {
        return;
    }
    applyNotebookOperation(title, [request](const QString &dir, int currentPage) {
        return PdfNotebookOps::resizePagesToPixelBounds(dir, request.pageIndices, request.boundsPx,
                                                        request.dpi, currentPage,
                                                        PdfPageRotator::rotateInto);
    });
}

/// The confirmation before a page leaves the notebook, said the same way whether the operation was
/// asked for from the submenu or from the panel: what happens to the page's notes, and what brings
/// them back.
bool confirmPageDelete(const QString &title)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    return QMessageBox::question(
               nullptr, title,
               i18n("Delete page %1 of %2? The page's notes are kept and can be brought back with "
                    "\"Undo the last notebook change\".",
                    navigator->currentIndex() + 1, navigator->pageCount()))
        == QMessageBox::Yes;
}

/// Enables what the notebook can actually do right now.
///
/// This is why the entries are plain QActions and not rows in PdfIoPlugin.action: "move up" means
/// nothing on the first page, "delete" would empty a one-page notebook, and "undo" depends on
/// whether a change is waiting -- none of which the .action file's activationFlags can see.
void updateNotebookOpsActions(QMenu *ops)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    const bool open = navigator->hasNotebook();
    const int index = navigator->currentIndex();
    const int pages = navigator->pageCount();

    const auto set = [ops, open](const char *name, bool possible) {
        QAction *action = ops->findChild<QAction *>(QString::fromLatin1(name));
        if (!action) {
            /// A name that is not there is a mistake in this file rather than a state: said out
            /// loud, so renaming an entry cannot quietly leave it enabled or disabled for good.
            qWarning("[pdfio] the Notebook ops submenu has no entry called %s", name);
            return;
        }
        action->setEnabled(open && possible);
    };

    /// The six page operations answer to the rule the panel's own buttons ask, so the submenu entry
    /// and the button cannot disagree about what is possible on the page that is open. Together
    /// with the runner above, that leaves one definition of each operation and one of each rule.
    const auto quick = [index, pages](PdfNotebookQuicks::Action action) {
        return PdfNotebookQuicks::available(action, pages, index, nullptr);
    };

    set("pdfio_rename_notebook", true);
    set("pdfio_ops_insert", true);
    set("pdfio_ops_move_up", quick(PdfNotebookQuicks::Action::MoveUp));
    set("pdfio_ops_move_down", quick(PdfNotebookQuicks::Action::MoveDown));
    set("pdfio_ops_move_to", pages > 1);
    set("pdfio_ops_duplicate", quick(PdfNotebookQuicks::Action::Duplicate));
    set("pdfio_ops_rotate_right", quick(PdfNotebookQuicks::Action::TurnRight));
    set("pdfio_ops_rotate_left", quick(PdfNotebookQuicks::Action::TurnLeft));
    set("pdfio_ops_resize_current", pages >= 1);
    set("pdfio_ops_resize_batch", pages >= 1);
    if (QMenu *resize = ops->findChild<QMenu *>(QStringLiteral("pdfio_ops_resize_menu"))) {
        resize->setEnabled(open && pages >= 1);
    }
    /// The quick-action gate accepts deleting the final page too; the resulting empty notebook can be
    /// repopulated with Insert pages or Merge notebook.
    set("pdfio_ops_delete", quick(PdfNotebookQuicks::Action::Delete));
    set("pdfio_ops_extract_range", pages >= 1);
    set("pdfio_ops_screen", open);
    set("pdfio_ops_merge", true);
    set("pdfio_ops_merge_folder", true);
    set("pdfio_ops_undo", PdfNotebookOps::canUndo(navigator->projectDir()));
    /// Deleting the notebook is possible exactly when there is one open, which is what a open is.
    set("pdfio_ops_delete_notebook", true);
}

/// The "Notebook ops" submenu: everything that changes the notebook itself rather than the page on
/// screen, under one entry. Deduped like the entries around it, because registerActions() runs
/// again for a second view and is retried while the first screen has no window.
void addNotebookOpsMenu(QMenu *menu, PdfIoPlugin *plugin)
{
    if (!menu) {
        return;
    }

    if (QMenu *previous = menu->findChild<QMenu *>(QStringLiteral("pdfio_notebook_ops"))) {
        menu->removeAction(previous->menuAction());
        previous->deleteLater();
    }

    /// One home for the notebook-level entries. A registration from before the submenu existed put
    /// "Rename notebook..." straight on the menu, and the separator it brought with it; both are
    /// taken off here rather than left as a second place the same action appears.
    const QStringList replaced = { QStringLiteral("pdfio_rename_notebook"),
                                   QStringLiteral("pdfio_rename_separator") };
    for (const QString &name : replaced) {
        if (QAction *previous = menu->findChild<QAction *>(name, Qt::FindDirectChildrenOnly)) {
            menu->removeAction(previous);
            previous->deleteLater();
        }
    }

    QMenu *ops = menu->addMenu(i18n("Notebook ops"));
    ops->setObjectName(QStringLiteral("pdfio_notebook_ops"));

    /// Rename first: it is the one notebook-level entry that was already there, and the one the
    /// submenu is a new home for.
    /// The screen first: one place where the whole page list can be seen and changed in one go, and
    /// the entries below stay for the one-click cases on the page that is open.
    QAction *screen = ops->addAction(i18n("Notebook ops..."));
    screen->setObjectName(QStringLiteral("pdfio_ops_screen"));
    QObject::connect(screen, &QAction::triggered, ops,
                     [plugin]() { openNotebookOpsScreen(plugin); });

    ops->addSeparator();

    QAction *rename = ops->addAction(i18n("Rename notebook..."));
    rename->setObjectName(QStringLiteral("pdfio_rename_notebook"));
    QObject::connect(rename, &QAction::triggered, ops, []() { renameNotebook(); });

    ops->addSeparator();

    /// Insert comes first of the page operations: it is the one that makes a notebook out of more
    /// than one PDF, and the entries below it act on pages one at a time.
    QAction *insert = ops->addAction(i18n("Insert pages from a PDF..."));
    insert->setObjectName(QStringLiteral("pdfio_ops_insert"));
    if (plugin) {
        QObject::connect(insert, &QAction::triggered, plugin, &PdfIoPlugin::slotInsertPages);
    }

    QAction *moveUp = ops->addAction(i18n("Move page up"));
    moveUp->setObjectName(QStringLiteral("pdfio_ops_move_up"));
    QObject::connect(moveUp, &QAction::triggered, ops, []() {
        runPdfIoQuickAction(PdfNotebookQuicks::Action::MoveUp);
    });

    QAction *moveDown = ops->addAction(i18n("Move page down"));
    moveDown->setObjectName(QStringLiteral("pdfio_ops_move_down"));
    QObject::connect(moveDown, &QAction::triggered, ops, []() {
        runPdfIoQuickAction(PdfNotebookQuicks::Action::MoveDown);
    });

    QAction *moveTo = ops->addAction(i18n("Move to page..."));
    moveTo->setObjectName(QStringLiteral("pdfio_ops_move_to"));
    QObject::connect(moveTo, &QAction::triggered, ops, []() {
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        if (!navigator->hasNotebook()) {
            return;
        }
        bool accepted = false;
        const int target =
            QInputDialog::getInt(nullptr, i18n("Move page"),
                                 i18n("Move page %1 to which position?", navigator->currentIndex() + 1),
                                 navigator->currentIndex() + 1, 1, navigator->pageCount(), 1, &accepted);
        if (!accepted || target - 1 == navigator->currentIndex()) {
            return;
        }
        applyNotebookOperation(i18n("Move page"), [target](const QString &dir, int page) {
            return PdfNotebookOps::movePage(dir, page, target - 1, page);
        });
    });

    QAction *duplicate = ops->addAction(i18n("Duplicate page"));
    duplicate->setObjectName(QStringLiteral("pdfio_ops_duplicate"));
    QObject::connect(duplicate, &QAction::triggered, ops, []() {
        runPdfIoQuickAction(PdfNotebookQuicks::Action::Duplicate);
    });

    /// Turning a page turns the paper AND the ink: the artifact is rotated through the same
    /// journal and commit protocol as every other operation, so a page that could not be turned
    /// fails whole rather than leaving a turned sheet under an upright stroke.
    QAction *rotateRight = ops->addAction(i18n("Rotate page right"));
    rotateRight->setObjectName(QStringLiteral("pdfio_ops_rotate_right"));
    QObject::connect(rotateRight, &QAction::triggered, ops, []() {
        runPdfIoQuickAction(PdfNotebookQuicks::Action::TurnRight);
    });

    QAction *rotateLeft = ops->addAction(i18n("Rotate page left"));
    rotateLeft->setObjectName(QStringLiteral("pdfio_ops_rotate_left"));
    QObject::connect(rotateLeft, &QAction::triggered, ops, []() {
        runPdfIoQuickAction(PdfNotebookQuicks::Action::TurnLeft);
    });

    /// Pixel dimensions and batch resize are separate from the Scale/Box gestures in the page manager.
    QMenu *resizeMenu = ops->addMenu(i18n("Resize"));
    resizeMenu->setObjectName(QStringLiteral("pdfio_ops_resize_menu"));
    QAction *resizeCurrent = resizeMenu->addAction(i18n("Resize current page by pixel size..."));
    resizeCurrent->setObjectName(QStringLiteral("pdfio_ops_resize_current"));
    QObject::connect(resizeCurrent, &QAction::triggered, resizeMenu, []() {
        resizeNotebookByPixels(false);
    });
    QAction *resizeBatch = resizeMenu->addAction(i18n("Batch resize pages by pixel size..."));
    resizeBatch->setObjectName(QStringLiteral("pdfio_ops_resize_batch"));
    QObject::connect(resizeBatch, &QAction::triggered, resizeMenu, []() {
        resizeNotebookByPixels(true);
    });

    QAction *removePage = ops->addAction(i18n("Delete page..."));
    removePage->setObjectName(QStringLiteral("pdfio_ops_delete"));
    QObject::connect(removePage, &QAction::triggered, ops, []() {
        runPdfIoQuickAction(PdfNotebookQuicks::Action::Delete);
    });

    /// Extracting takes pages OUT of the notebook and into a new one, so it sits with the page
    /// operations rather than with the whole-notebook ones below the separator.
    QAction *extract = ops->addAction(i18n("Extract a page range..."));
    extract->setObjectName(QStringLiteral("pdfio_ops_extract_range"));
    QObject::connect(extract, &QAction::triggered, ops, []() { extractPageRange(); });

    /// Merging is extraction's other direction: pages arrive from another notebook instead of
    /// leaving for one. One chooser serves it -- the recent notebooks first, then a notebook file --
    /// because that is what the user asked for, and on a tablet the recent list is the only place a
    /// whole notebook can come from.
    QAction *merge = ops->addAction(i18n("Merge a notebook in..."));
    merge->setObjectName(QStringLiteral("pdfio_ops_merge"));
    QObject::connect(merge, &QAction::triggered, ops, []() { mergeNotebookChosen(); });

    /// The folder form is for a notebook already on this device, which means a desktop: Android has
    /// no filesystem to browse, so the entry is not offered there at all rather than opening a
    /// dialog that cannot work. The chooser above leaves it out there as well.
    QAction *mergeFolder = ops->addAction(i18n("Merge a notebook folder..."));
    mergeFolder->setObjectName(QStringLiteral("pdfio_ops_merge_folder"));
    QObject::connect(mergeFolder, &QAction::triggered, ops, []() { mergeNotebookFolder(); });
#if defined(Q_OS_ANDROID)
    mergeFolder->setVisible(false);
#endif

    ops->addSeparator();

    QAction *undo = ops->addAction(i18n("Undo the last notebook change"));
    undo->setObjectName(QStringLiteral("pdfio_ops_undo"));
    QObject::connect(undo, &QAction::triggered, ops, []() {
        /// Stroke-level undo is Krita's, and it covers the page that is open. Notebook operations
        /// are separate undo steps; this action walks the bounded notebook history one step at a time.
        applyNotebookOperation(i18n("Undo the last notebook change"), [](const QString &dir, int) {
            return PdfNotebookOps::undoLast(dir);
        });
    });

    /// Last, below the separator, and the only entry here that takes something away for good: the
    /// notebook and its folder. It is the counterpart of "Import PDF as notebook", which is how one
    /// arrives -- until this existed the store only ever grew.
    ops->addSeparator();
    QAction *removeNotebook = ops->addAction(i18n("Delete notebook..."));
    removeNotebook->setObjectName(QStringLiteral("pdfio_ops_delete_notebook"));
    QObject::connect(removeNotebook, &QAction::triggered, ops, []() { deleteNotebook(); });

    /// Enabled from the state the notebook is in when the user reaches for the menu, not from the
    /// state it was in when the menu was built.
    QObject::connect(ops, &QMenu::aboutToShow, ops, [ops]() { updateNotebookOpsActions(ops); });
    updateNotebookOpsActions(ops);
}

/// The two sentences every entry of the memory-budget menu and the typed dialog carries: what the
/// budget is spent on, and what it costs the ink. One string, so the menu and the dialog cannot drift
/// into telling the user two different things about the same setting.
QString memoryBudgetCaveat()
{
    return i18n(
        "This is spent on the whole window that is up -- every page in it plus the gaps, across "
        "every full-size layer the strip has -- so the page size it buys depends on how many pages "
        "are open and how big they are: a window of five slides buys a smaller page than a window of "
        "five A4 sheets.\n\n"
        "The budget is the pages' rendered resolution, which is also the resolution the pen's ink is "
        "stored at from then on, and lowering it is not freely reversible. The source PDF and "
        "everything already stored are untouched.");
}

/// The typed memory budget: a number of megabytes, and under it, live, what that number buys for the
/// notebook that is open.
///
/// The live line is the whole reason a typed value is worth having. The presets name round numbers
/// and the menu names the page size each of them buys; a dialog that only took a number would ask the
/// user to guess what it costs in quality and show them afterwards. So every change asks the
/// navigator the same question the menu label asks -- longestPagePixelsForBudget() -- through the
/// same derivation, and shows the memory the window really costs beside it, which is NOT always the
/// number typed: a budget above what 200 dpi already costs does not spend itself.
///
/// No Q_OBJECT: it has no signals or slots of its own, so it needs no moc.
class PdfMemoryBudgetDialog : public QDialog
{
public:
    PdfMemoryBudgetDialog(int current, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(i18n("Rendered page memory"));

        auto *intro = new QLabel(i18n("What the strip's rendered pages may cost, in megabytes:"), this);
        intro->setWordWrap(true);

        m_value = new QSpinBox(this);
        m_value->setObjectName(QStringLiteral("pdfio_memory_budget_value"));
        /// The range the setter holds a typed value to, asked of the navigator rather than written
        /// down again here, so the spin box cannot offer what the setter would refuse.
        m_value->setRange(PdfPageNavigator::minMemoryBudgetMb(),
                          PdfPageNavigator::maxMemoryBudgetMb());
        m_value->setSuffix(QStringLiteral(" MB"));
        /// A spin box on purpose: it is tapped on a tablet, and the +/- buttons are how the value is
        /// raised as well as lowered -- the knob the user wants is finer than three presets and goes
        /// in both directions. 50 MB is a visible step at this scale: ten taps cover 200 to 700 MB.
        m_value->setSingleStep(50);
        /// The platform draws the step buttons a few pixels wide, which a finger cannot hit: measured
        /// on the tablet at about 6x11 logical pixels. The +/- stepping is the whole reason this is a
        /// spin box rather than a line edit, so the buttons are given the room of a touch target.
        const int stepWidth = qMax(32, m_value->fontMetrics().height() * 2);
        m_value->setStyleSheet(QStringLiteral("QSpinBox::up-button, QSpinBox::down-button { width: %1px; }")
                                   .arg(stepWidth));
        m_value->setValue(qBound(PdfPageNavigator::minMemoryBudgetMb(), current,
                                 PdfPageNavigator::maxMemoryBudgetMb()));
        m_value->setToolTip(memoryBudgetCaveat());

        m_preview = new QLabel(this);
        m_preview->setObjectName(QStringLiteral("pdfio_memory_budget_preview"));
        m_preview->setWordWrap(true);

        auto *sentences = new QLabel(memoryBudgetCaveat(), this);
        sentences->setWordWrap(true);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(intro);
        layout->addWidget(m_value);
        layout->addWidget(m_preview);
        layout->addWidget(sentences);
        layout->addWidget(buttons);

        const auto update = [this]() {
            PdfPageNavigator *navigator = PdfPageNavigator::instance();
            if (!navigator->hasNotebook()) {
                m_preview->setText(i18n("No notebook is open: the page size a budget buys depends on "
                                        "the window that is up."));
                return;
            }

            const int megabytes = m_value->value();
            m_preview->setText(i18n("About %1 px on the longest page of the window that is open, in "
                                    "a window that costs about %2 MB.",
                                    navigator->longestPagePixelsForBudget(megabytes),
                                    navigator->windowCostMbForBudget(megabytes)));
        };
        QObject::connect(m_value, QOverload<int>::of(&QSpinBox::valueChanged), this,
                         [update](int) { update(); });
        update();
    }

    int megabytes() const { return m_value->value(); }

private:
    QSpinBox *m_value = nullptr;
    QLabel *m_preview = nullptr;
};

/// What the strip's rendered pages may cost, and the resolution that buys.
///
/// A page can be LANDSCAPE, so a bound on the longest side buys a different amount of memory for
/// every shape: the same pixel target is 1.8 Mpx on a 16:9 page and 2.5 Mpx on an A4 portrait, and a
/// window of five slides costs several times a window of five A4 sheets. The knob is therefore the
/// memory, and the resolution follows from the window that is up (see
/// PdfPageNavigator::memoryBudgetMb()).
///
/// The labels name the budget AND, when a notebook is open, the page size it produces for the window
/// that is up: a budget must not hide what it costs in quality. The tooltip carries the two things
/// neither can:
///
///  - the budget is spent on the WHOLE window that is up -- every page in it plus the gaps, across
///    every full-size layer the strip has -- so the page size it buys depends on how many pages are
///    open and how big they are: a window of five slides buys a smaller page than five A4 sheets;
///  - the budget is the pages' rendered resolution, which is also the resolution the pen's ink is
///    stored at from then on, and lowering it is not freely reversible. The source PDF and
///    everything already stored are untouched.
void addMemoryBudgetMenu(QMenu *menu)
{
    if (!menu) {
        return;
    }

    /// Deduped like the entries around it: registerActions() runs again for a second view and is
    /// retried while the first screen has no window.
    if (QMenu *previous = menu->findChild<QMenu *>(QStringLiteral("pdfio_memory_budget_menu"))) {
        menu->removeAction(previous->menuAction());
        previous->deleteLater();
    }

    QMenu *budgets = menu->addMenu(i18n("Rendered page memory"));
    budgets->setObjectName(QStringLiteral("pdfio_memory_budget_menu"));

    auto *group = new QActionGroup(budgets);
    const auto addBudget = [budgets, group](int megabytes, const QString &name) {
        QAction *action = budgets->addAction(QString());
        action->setObjectName(name);
        action->setCheckable(true);
        action->setData(megabytes);
        group->addAction(action);
        QObject::connect(action, &QAction::triggered, budgets, [megabytes]() {
            PdfPageNavigator::instance()->setMemoryBudgetMb(megabytes);
        });
        return action;
    };

    QAction *unlimited = addBudget(0, QStringLiteral("pdfio_memory_budget_unlimited"));
    QAction *mb200 = addBudget(200, QStringLiteral("pdfio_memory_budget_200"));
    QAction *mb400 = addBudget(400, QStringLiteral("pdfio_memory_budget_400"));
    QAction *mb800 = addBudget(800, QStringLiteral("pdfio_memory_budget_800"));

    /// The typed value. Its own action rather than a preset with a sentinel: it is not a budget that
    /// can be applied, it is a dialog, so it must not go through the "set this number" connection.
    QAction *custom = budgets->addAction(QString());
    custom->setObjectName(QStringLiteral("pdfio_memory_budget_custom"));
    custom->setCheckable(true);
    custom->setData(-1);
    group->addAction(custom);
    QObject::connect(custom, &QAction::triggered, budgets, [budgets]() {
        PdfPageNavigator *navigator = PdfPageNavigator::instance();

        /// Opens on the value in force. "No limit" has no number to open on, so it opens on what the
        /// window costs at 200 dpi -- the no-limit case, which is the most the dialog can usefully
        /// offer -- and a notebook with nothing open opens on the first preset.
        int current = navigator->memoryBudgetMb();
        if (current <= 0) {
            current = navigator->windowCostMbForBudget(0);
        }
        if (current <= 0) {
            current = 200;
        }

        PdfMemoryBudgetDialog dialog(current, budgets->parentWidget());
        if (dialog.exec() == QDialog::Accepted) {
            /// The same setter the presets use: the persistence, the resize through the roll, the
            /// modified flag and the one derivation are shared, not re-implemented here.
            navigator->setMemoryBudgetMb(dialog.megabytes());
        }
    });

    /// Filled every time the submenu opens: the mark is the navigator's own setting, and the page
    /// size beside each budget is what THAT budget would produce for the notebook that is open now.
    /// With nothing open there is no page size to name, and the label is the budget alone.
    const auto refresh = [unlimited, mb200, mb400, mb800, custom]() {
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        const bool open = navigator->hasNotebook();
        const QString caveat = memoryBudgetCaveat();

        const auto fill = [navigator, open, &caveat](QAction *action) {
            const int megabytes = action->data().toInt();

            if (megabytes < 0) {
                /// The typed entry names the value in force when it is not one of the presets, so a
                /// value the user typed is visible on the menu rather than only in the document's
                /// title, and it is the one that gets the mark.
                const int current = navigator->memoryBudgetMb();
                const bool typed = current > 0 && current != 200 && current != 400 && current != 800;
                action->setText(typed ? i18n("Custom... (%1 MB)", current) : i18n("Custom..."));
                action->setToolTip(caveat);
                action->setChecked(typed);
                return;
            }

            const QString label = megabytes > 0 ? i18n("Up to %1 MB", megabytes)
                                                : i18n("No limit (the 200 dpi it always used)");

            const int pixels = open ? navigator->longestPagePixelsForBudget(megabytes) : 0;
            action->setText(pixels > 0
                                ? i18n("%1 — about %2 px on the longest side", label, pixels)
                                : label);
            action->setToolTip(caveat);
            action->setChecked(megabytes == navigator->memoryBudgetMb());
        };

        fill(unlimited);
        fill(mb200);
        fill(mb400);
        fill(mb800);
        fill(custom);
    };

    QObject::connect(budgets, &QMenu::aboutToShow, budgets, refresh);
    refresh();
}


/// The two sentences the "Strip pages" submenu and its typed dialog carry: what more pages cost, and
/// why the count is bounded by the notebook rather than by a constant of this build's.
QString stripPagesCaveat()
{
    return i18n(
        "The strip is the active page with the same number of pages either side of it, all in one "
        "document. More pages at a fixed memory budget spend the same memory at a lower resolution "
        "each; with no limit they simply cost more.\n\n"
        "The window cannot extend past the first and last page, so a notebook with fewer pages holds "
        "what it has. This is the strip's \"how many\" half: whether it is on at all is the switch "
        "beside it, which keeps this count when it is turned off.");
}

/// The typed strip page count: a number of pages, and under it, live, what that number costs for the
/// notebook that is open.
///
/// The live line is the whole reason a typed count is worth having. The presets name round numbers
/// and the menu names what each of them buys; a dialog that only took a number would ask the user to
/// guess what it costs and show them afterwards. So every change asks the navigator the same question
/// the menu label asks -- longestPagePixelsForScope() and windowCostMbForScope() -- through the same
/// derivation the build would use.
///
/// No Q_OBJECT: it has no signals or slots of its own, so it needs no moc.
class PdfStripPagesDialog : public QDialog
{
public:
    PdfStripPagesDialog(int current, int maximum, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(i18n("Strip pages"));

        auto *intro = new QLabel(i18n("How many pages the strip holds, the active page included. The "
                                      "strip is symmetric, so an even count is laid out as the next "
                                      "odd one:"), this);
        intro->setWordWrap(true);

        m_value = new QSpinBox(this);
        m_value->setObjectName(QStringLiteral("pdfio_strip_pages_value"));
        /// Bounded by the notebook, asked of it rather than written down here: a window cannot extend
        /// past the first and last page. With nothing open there is nothing to bound it by, so the
        /// range is wide enough for the "several tens of pages" a powerful machine was asked for, and
        /// the notebook clamps it when one is open.
        const int bound = qMax(3, maximum);
        m_value->setRange(3, bound);
        m_value->setSuffix(i18n(" pages"));
        /// A spin box on purpose: it is tapped on a tablet, and +/- is how the value is raised as
        /// well as lowered. One step is one page, so the step a press makes stays visible however
        /// large the count is.
        m_value->setSingleStep(1);
        m_value->setValue(qBound(3, current, bound));
        m_value->setToolTip(stripPagesCaveat());

        m_preview = new QLabel(this);
        m_preview->setObjectName(QStringLiteral("pdfio_strip_pages_preview"));
        m_preview->setWordWrap(true);

        auto *sentences = new QLabel(stripPagesCaveat(), this);
        sentences->setWordWrap(true);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(intro);
        layout->addWidget(m_value);
        layout->addWidget(m_preview);
        layout->addWidget(sentences);
        layout->addWidget(buttons);

        const auto update = [this]() {
            PdfPageNavigator *navigator = PdfPageNavigator::instance();
            const int chosen = m_value->value();
            /// The strip is symmetric, so an even value is laid out as the next odd one: the number
            /// shown is the number that will really be on screen rather than the one typed.
            const int laidOut = chosen + (chosen % 2);
            const int pages =
                navigator->hasNotebook() ? qMin(laidOut, navigator->pageCount()) : laidOut;

            if (!navigator->hasNotebook()) {
                m_preview->setText(i18n("%1 pages (active ±%2). No notebook is open: the page size and "
                                        "the memory a count costs depend on the notebook's own pages.",
                                        pages, (pages - 1) / 2));
                return;
            }

            m_preview->setText(i18n("%1 pages (active ±%2) — about %3 px on the longest page, about "
                                    "%4 MB.",
                                    pages,
                                    (pages - 1) / 2,
                                    navigator->longestPagePixelsForScope(chosen),
                                    navigator->windowCostMbForScope(chosen)));
        };
        QObject::connect(m_value, QOverload<int>::of(&QSpinBox::valueChanged), this,
                         [update](int) { update(); });
        update();
    }

    int pages() const { return m_value->value(); }

private:
    QSpinBox *m_value = nullptr;
    QLabel *m_preview = nullptr;
};

/// Applies the strip's page count to the navigator AND to the screen.
///
/// The navigator owns the setting: setScope() persists the count and the on/off state apart, applies
/// a new count to a strip that is already up through the roll's own resize path, and leaves a
/// single-page document alone. What it cannot do is the part that needs a DOCUMENT rather than a
/// window -- going to one page at a time is design A, a different document, and bringing a strip up
/// where a single page was is design B's build. Those two are what \a rebuildPage is for, and the
/// switch's own label is what \a refreshSwitch is for.
///
/// \a pages of 1 is OFF, the switch's own state, and anything above it is a count; the navigator
/// reads the same two settings either way.
void applyStripPages(int pages, const std::function<void()> &refreshSwitch,
                     const std::function<void(int)> &rebuildPage)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    const bool turningOff = pages <= 1;
    const bool stripWasOpen = navigator->stripIsOpen();

    navigator->setScope(pages);
    refreshSwitch();

    if (!navigator->hasNotebook()) {
        say(QStringLiteral("strip: %1 set; it applies when a notebook is opened")
                .arg(turningOff ? QStringLiteral("one page at a time")
                                : QStringLiteral("%1 pages").arg(navigator->scope())));
        return;
    }

    /// Rebuilt only where the change is a different DOCUMENT: taking a strip down to one page, or
    /// bringing one up where a single page was. A count change WITHIN a strip is neither, and the
    /// navigator's setScope has already re-cut the bands and resized the document through the roll
    /// -- rebuilding then would throw away exactly the path this control exists to use.
    const bool needsBuild = turningOff ? stripWasOpen : !stripWasOpen;
    if (!needsBuild) {
        say(QStringLiteral("strip: the window is now %1 page(s), re-cut in place")
                .arg(navigator->scope()));
        return;
    }

    /// The pages the document still holds are written before it is taken down, exactly as the
    /// switch's own path writes them, and the rebuild happens on the event loop because Krita's close
    /// of the old document is deferred.
    navigator->saveStripPages();
    rebuildPage(navigator->currentIndex());
}

/// The strip's page count: the switch's "how many" half, as a submenu of presets and a typed value.
///
/// The count is bounded by the NOTEBOOK rather than by a constant of this build's -- the user asked
/// to be able to load several tens of pages on a powerful machine, so seven is not a ceiling. Every
/// entry names what that count costs for the notebook that is open, the longest page's pixels and the
/// window's megabytes, which is the discouragement a large value needs: at a fixed budget more pages
/// spend the same memory coarser, and with no limit they cost more.
///
/// \a applyPages is the navigator-and-screen path, provided by the caller because the two things a
/// count change can need beyond the navigator -- relabelling the switch and rebuilding the document
/// where its shape changes -- are the plugin's own private members.
void addStripPagesMenu(QMenu *menu, const std::function<void(int)> &applyPages)
{
    if (!menu) {
        return;
    }

    /// Deduped like the entries around it: registerActions() runs again for a second view and is
    /// retried while the first screen has no window.
    if (QMenu *previous = menu->findChild<QMenu *>(QStringLiteral("pdfio_strip_pages_menu"))) {
        menu->removeAction(previous->menuAction());
        previous->deleteLater();
    }

    QMenu *pages = menu->addMenu(i18n("Strip pages"));
    pages->setObjectName(QStringLiteral("pdfio_strip_pages_menu"));

    auto *group = new QActionGroup(pages);
    const auto addPages = [pages, group, applyPages](int count, const QString &name) {
        QAction *action = pages->addAction(QString());
        action->setObjectName(name);
        action->setCheckable(true);
        action->setData(count);
        group->addAction(action);
        QObject::connect(action, &QAction::triggered, pages,
                         [applyPages, count]() { applyPages(count); });
        return action;
    };

    QAction *three = addPages(3, QStringLiteral("pdfio_strip_pages_3"));
    QAction *five = addPages(5, QStringLiteral("pdfio_strip_pages_5"));
    QAction *seven = addPages(7, QStringLiteral("pdfio_strip_pages_7"));
    QAction *nine = addPages(9, QStringLiteral("pdfio_strip_pages_9"));

    /// The typed value. Its own action rather than a preset with a sentinel: it is not a count that
    /// can be applied, it is a dialog.
    QAction *custom = pages->addAction(QString());
    custom->setObjectName(QStringLiteral("pdfio_strip_pages_custom"));
    custom->setCheckable(true);
    custom->setData(-1);
    group->addAction(custom);
    QObject::connect(custom, &QAction::triggered, pages, [pages, applyPages]() {
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        /// The bound is the notebook's own page count; with nothing open the dialog offers the wide
        /// range and the notebook clamps it when one is.
        const int maximum = navigator->hasNotebook() ? navigator->pageCount() : 99;
        PdfStripPagesDialog dialog(navigator->stripPageCount(), maximum, pages->parentWidget());
        if (dialog.exec() == QDialog::Accepted) {
            applyPages(dialog.pages());
        }
    });

    /// Filled every time the submenu opens: the mark is the navigator's own count, and the pixels and
    /// megabytes beside each count are what THAT count would produce for the notebook that is open
    /// now. With nothing open there is no size or cost to name, and the label is the count alone.
    const auto refresh = [three, five, seven, nine, custom]() {
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        const bool open = navigator->hasNotebook();
        const int current = navigator->stripPageCount();
        const QString caveat = stripPagesCaveat();

        const auto fill = [navigator, open, current, &caveat](QAction *action) {
            const int count = action->data().toInt();

            if (count < 0) {
                /// The typed entry names the count in force when it is not one of the presets, so a
                /// count the user typed is visible on the menu and is the one that gets the mark.
                const bool typed = current != 3 && current != 5 && current != 7 && current != 9;
                action->setText(typed ? i18n("Custom... (%1 pages)", current) : i18n("Custom..."));
                action->setToolTip(caveat);
                action->setChecked(typed);
                return;
            }

            /// What the notebook can really show: the window cannot extend past the first and last
            /// page, so a choice larger than the notebook is labelled with the pages it will hold.
            const int held = open ? qMin(count, navigator->pageCount()) : count;
            const QString label = i18n("%1 pages (active ±%2)", held, (held - 1) / 2);
            const int pixels = open ? navigator->longestPagePixelsForScope(count) : 0;
            const int megabytes = open ? navigator->windowCostMbForScope(count) : 0;
            action->setText(open
                                ? i18n("%1 — about %2 px on the longest page, about %3 MB",
                                       label, pixels, megabytes)
                                : label);
            action->setToolTip(caveat);
            action->setChecked(count == current);
        };

        fill(three);
        fill(five);
        fill(seven);
        fill(nine);
        fill(custom);
    };

    QObject::connect(pages, &QMenu::aboutToShow, pages, refresh);
    refresh();
}


/// The page-loading trigger's two sentences, on every entry of its menus and in its dialogs: what
/// the settle delay changes and what the reading share changes.
QString settleDelayCaveat()
{
    return i18n(
        "How long the page under the middle of the viewport has to stay there before the strip turns "
        "to it.\n\n"
        "A longer wait means a burst of scrolling does not drag the strip through every page it "
        "passes: the candidate page is reset every time a different page comes under the middle, so "
        "only the page the scrolling RESTS on is opened, which is fewer writes and less churn. A "
        "shorter one means the strip follows your finger.\n\n"
        "Zero is allowed: the strip then turns on the first 150 ms tick after the page under the "
        "middle changes. There is no upper limit, because no value breaks anything -- a wait so long "
        "that it never fires is the same thing as turning \"Turn pages by panning\" off, and that "
        "switch is the honest way to say it.");
}

QString readingShareCaveat()
{
    return i18n(
        "How much of the viewport height the active page takes when the strip is fitted. Three fifths "
        "(60%) is what the notebook has always used.\n\n"
        "It is deliberately not full screen: a page fitted exactly filled the viewport and the page "
        "that comes next was then not on screen at all -- \"even at page three you cannot see "
        "four\". A smaller share shows more of the next and previous pages, and less scrolling turns "
        "the reading page; a larger one fills more of the screen and needs more scrolling to change "
        "it.\n\n"
        "Held between 1% and 100%: a share of zero is a page with no height, and above 100% the page "
        "is taller than the viewport, so the neighbour this rule exists to show is gone again.");
}

/// What a settle delay buys, said the same way in every label: short is "follows your finger" and
/// long is "a burst settles before anything turns". A function rather than a chain in the lambda so
/// the preset and a typed value are described by the same rule.
QString settleDelayMeaning(int milliseconds)
{
    if (milliseconds <= 0) {
        return i18n("turn on the next tick");
    }
    if (milliseconds <= 150) {
        return i18n("follows your finger");
    }
    if (milliseconds <= 300) {
        return i18n("a short beat");
    }
    if (milliseconds <= 450) {
        return i18n("today's value: a burst settles first");
    }
    return i18n("only a deliberate pause turns");
}

/// The typed settle delay, in milliseconds.
///
/// No maximum: the declaration of scrollSettleMs() says why -- a value that never fires is the
/// switch's job to say, not a ceiling's. The step is 50 ms because that is a visible change to a
/// 450 ms pause and still fine enough for a tablet's +/- buttons.
///
/// No Q_OBJECT: it has no signals or slots of its own, so it needs no moc.
class PdfSettleDelayDialog : public QDialog
{
public:
    PdfSettleDelayDialog(int current, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(i18n("Turn after a scroll"));

        auto *intro = new QLabel(i18n("How long the view has to rest on a page before the strip turns "
                                      "to it, in milliseconds:"), this);
        intro->setWordWrap(true);

        m_value = new QSpinBox(this);
        m_value->setObjectName(QStringLiteral("pdfio_scroll_settle_value"));
        m_value->setRange(0, std::numeric_limits<int>::max());
        m_value->setSuffix(QStringLiteral(" ms"));
        m_value->setSingleStep(50);
        m_value->setValue(qMax(0, current));
        m_value->setToolTip(settleDelayCaveat());

        auto *sentences = new QLabel(settleDelayCaveat(), this);
        sentences->setWordWrap(true);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(intro);
        layout->addWidget(m_value);
        layout->addWidget(sentences);
        layout->addWidget(buttons);
    }

    int milliseconds() const { return m_value->value(); }

private:
    QSpinBox *m_value = nullptr;
};

/// What a reading share buys, in the words a menu can carry.
QString readingShareMeaning(int percent)
{
    if (percent <= 40) {
        return i18n("shows the most of the neighbours");
    }
    if (percent <= 50) {
        return i18n("half the height, half for the neighbours");
    }
    if (percent <= 60) {
        return i18n("today's: three fifths");
    }
    if (percent < 100) {
        return i18n("a bigger page, turns after more scrolling");
    }
    return i18n("full screen: the next page is off screen");
}

/// The typed reading share, in percent, with what it leaves for the pages either side under it.
///
/// No Q_OBJECT: it has no signals or slots of its own, so it needs no moc.
class PdfReadingShareDialog : public QDialog
{
public:
    PdfReadingShareDialog(int current, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(i18n("Reading page share"));

        auto *intro = new QLabel(i18n("How much of the viewport height the active page takes when the "
                                      "strip is fitted, in percent:"), this);
        intro->setWordWrap(true);

        m_value = new QSpinBox(this);
        m_value->setObjectName(QStringLiteral("pdfio_reading_share_value"));
        /// The range the setter holds a typed value to, asked of the navigator rather than written
        /// down again here, so the spin box cannot offer what the setter would refuse.
        m_value->setRange(PdfPageNavigator::minReadingSharePercent(),
                          PdfPageNavigator::maxReadingSharePercent());
        m_value->setSuffix(QStringLiteral(" %"));
        m_value->setSingleStep(5);
        m_value->setValue(qBound(PdfPageNavigator::minReadingSharePercent(), current,
                                 PdfPageNavigator::maxReadingSharePercent()));
        m_value->setToolTip(readingShareCaveat());

        m_leaves = new QLabel(this);
        m_leaves->setObjectName(QStringLiteral("pdfio_reading_share_leaves"));
        m_leaves->setWordWrap(true);

        auto *sentences = new QLabel(readingShareCaveat(), this);
        sentences->setWordWrap(true);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(intro);
        layout->addWidget(m_value);
        layout->addWidget(m_leaves);
        layout->addWidget(sentences);
        layout->addWidget(buttons);

        const auto update = [this]() {
            const int share = m_value->value();
            m_leaves->setText(i18n("The page takes %1% of the height and %2% is left for the pages "
                                   "either side of it.", share, 100 - share));
        };
        QObject::connect(m_value, QOverload<int>::of(&QSpinBox::valueChanged), this,
                         [update](int) { update(); });
        update();
    }

    int percent() const { return m_value->value(); }

private:
    QSpinBox *m_value = nullptr;
    QLabel *m_leaves = nullptr;
};

/// How long the strip waits after the view settles on a page before it turns to it: the switch's
/// own companion, one submenu of presets and a typed value.
///
/// The value is in milliseconds; 450 is what the notebook has always used and is the default, so
/// nobody who does not touch this sees a change. Zero turns on the next 150 ms tick. There is no
/// maximum: the declaration of PdfPageNavigator::scrollSettleMs() says why.
void addScrollSettleMenu(QMenu *menu)
{
    if (!menu) {
        return;
    }

    /// Deduped like the entries around it: registerActions() runs again for a second view and is
    /// retried while the first screen has no window.
    if (QMenu *previous = menu->findChild<QMenu *>(QStringLiteral("pdfio_scroll_settle_menu"))) {
        menu->removeAction(previous->menuAction());
        previous->deleteLater();
    }

    QMenu *settle = menu->addMenu(i18n("Turn after a scroll"));
    settle->setObjectName(QStringLiteral("pdfio_scroll_settle_menu"));

    auto *group = new QActionGroup(settle);
    const auto addPreset = [settle, group](int milliseconds, const QString &name) {
        QAction *action = settle->addAction(QString());
        action->setObjectName(name);
        action->setCheckable(true);
        action->setData(milliseconds);
        group->addAction(action);
        QObject::connect(action, &QAction::triggered, settle, [milliseconds]() {
            PdfPageNavigator::instance()->setScrollSettleMs(milliseconds);
        });
        return action;
    };

    QAction *immediate = addPreset(0, QStringLiteral("pdfio_scroll_settle_0"));
    QAction *finger = addPreset(150, QStringLiteral("pdfio_scroll_settle_150"));
    QAction *beat = addPreset(300, QStringLiteral("pdfio_scroll_settle_300"));
    QAction *shipped = addPreset(450, QStringLiteral("pdfio_scroll_settle_450"));
    QAction *pause = addPreset(900, QStringLiteral("pdfio_scroll_settle_900"));

    /// The typed value. Its own action rather than a preset with a sentinel: it is not a delay that
    /// can be applied, it is a dialog.
    QAction *custom = settle->addAction(QString());
    custom->setObjectName(QStringLiteral("pdfio_scroll_settle_custom"));
    custom->setCheckable(true);
    custom->setData(-1);
    group->addAction(custom);
    QObject::connect(custom, &QAction::triggered, settle, [settle]() {
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        PdfSettleDelayDialog dialog(navigator->scrollSettleMs(), settle->parentWidget());
        if (dialog.exec() == QDialog::Accepted) {
            navigator->setScrollSettleMs(dialog.milliseconds());
        }
    });

    /// Filled every time the submenu opens: the mark is the navigator's own value and every label
    /// says what that value buys, because the number alone does not.
    const auto refresh = [immediate, finger, beat, shipped, pause, custom]() {
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        const int current = navigator->scrollSettleMs();
        const QString caveat = settleDelayCaveat();

        const auto fill = [current, &caveat](QAction *action) {
            const int milliseconds = action->data().toInt();

            if (milliseconds < 0) {
                const bool typed = current != 0 && current != 150 && current != 300 && current != 450
                    && current != 900;
                action->setText(typed ? i18n("Custom... (%1 ms)", current) : i18n("Custom..."));
                action->setToolTip(caveat);
                action->setChecked(typed);
                return;
            }

            action->setText(i18n("%1 ms — %2", milliseconds, settleDelayMeaning(milliseconds)));
            action->setToolTip(caveat);
            action->setChecked(milliseconds == current);
        };

        fill(immediate);
        fill(finger);
        fill(beat);
        fill(shipped);
        fill(pause);
        fill(custom);
    };

    QObject::connect(settle, &QMenu::aboutToShow, settle, refresh);
    refresh();
}

/// How much of the viewport the active page takes when the strip is fitted: the three fifths the fit
/// has always used, as a share the user can move.
///
/// Smaller shows more of the neighbours and turns the reading page after less scrolling; 100% fills
/// the viewport, which is the state the three fifths exists to avoid. The default is 60 and changes
/// nothing for anyone who does not touch it.
void addReadingShareMenu(QMenu *menu)
{
    if (!menu) {
        return;
    }

    if (QMenu *previous = menu->findChild<QMenu *>(QStringLiteral("pdfio_reading_share_menu"))) {
        menu->removeAction(previous->menuAction());
        previous->deleteLater();
    }

    QMenu *share = menu->addMenu(i18n("Reading page share"));
    share->setObjectName(QStringLiteral("pdfio_reading_share_menu"));

    auto *group = new QActionGroup(share);
    const auto addPreset = [share, group](int percent, const QString &name) {
        QAction *action = share->addAction(QString());
        action->setObjectName(name);
        action->setCheckable(true);
        action->setData(percent);
        group->addAction(action);
        QObject::connect(action, &QAction::triggered, share, [percent]() {
            PdfPageNavigator::instance()->setReadingSharePercent(percent);
        });
        return action;
    };

    QAction *twoFifths = addPreset(40, QStringLiteral("pdfio_reading_share_40"));
    QAction *half = addPreset(50, QStringLiteral("pdfio_reading_share_50"));
    QAction *threeFifths = addPreset(60, QStringLiteral("pdfio_reading_share_60"));
    QAction *fourFifths = addPreset(80, QStringLiteral("pdfio_reading_share_80"));
    QAction *full = addPreset(100, QStringLiteral("pdfio_reading_share_100"));

    QAction *custom = share->addAction(QString());
    custom->setObjectName(QStringLiteral("pdfio_reading_share_custom"));
    custom->setCheckable(true);
    custom->setData(-1);
    group->addAction(custom);
    QObject::connect(custom, &QAction::triggered, share, [share]() {
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        PdfReadingShareDialog dialog(navigator->readingSharePercent(), share->parentWidget());
        if (dialog.exec() == QDialog::Accepted) {
            navigator->setReadingSharePercent(dialog.percent());
        }
    });

    const auto refresh = [twoFifths, half, threeFifths, fourFifths, full, custom]() {
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        const int current = navigator->readingSharePercent();
        const QString caveat = readingShareCaveat();

        const auto fill = [current, &caveat](QAction *action) {
            const int percent = action->data().toInt();

            if (percent < 0) {
                const bool typed = current != 40 && current != 50 && current != 60 && current != 80
                    && current != 100;
                action->setText(typed ? i18n("Custom... (%1%)", current) : i18n("Custom..."));
                action->setToolTip(caveat);
                action->setChecked(typed);
                return;
            }

            action->setText(i18n("%1% — %2", percent, readingShareMeaning(percent)));
            action->setToolTip(caveat);
            action->setChecked(percent == current);
        };

        fill(twoFifths);
        fill(half);
        fill(threeFifths);
        fill(fourFifths);
        fill(full);
        fill(custom);
    };

    QObject::connect(share, &QMenu::aboutToShow, share, refresh);
    refresh();
}

/// Puts the "Recent notebooks" submenu at the top of \a menu: the entry the user reaches for
/// first, open document or not. Rebuilt every time it opens, so it is never stale; deduped like the
/// entries around it.
void addRecentNotebooksMenu(QMenu *menu)
{
    if (!menu) {
        return;
    }

    if (QMenu *previous = menu->findChild<QMenu *>(QStringLiteral("pdfio_recent_notebooks"))) {
        menu->removeAction(previous->menuAction());
        previous->deleteLater();
    }

    QMenu *recent = menu->addMenu(i18n("Recent notebooks"));
    recent->setObjectName(QStringLiteral("pdfio_recent_notebooks"));
    QObject::connect(recent, &QMenu::aboutToShow, recent,
                     [recent]() { rebuildRecentNotebooks(recent); });
}

} // namespace

/// The application half of the pieces of the notebook panel and the submenu that are not widgets.
/// Here rather than in the anonymous namespace above, which is where everything else in this file
/// lives: these three are what PdfIoDocker.cpp calls, so they have to have external linkage.
QString pdfIoQuickTitle(PdfNotebookQuicks::Action action)
{
    switch (action) {
    case PdfNotebookQuicks::Action::MoveUp:
        return i18n("Move page up");
    case PdfNotebookQuicks::Action::MoveDown:
        return i18n("Move page down");
    case PdfNotebookQuicks::Action::Duplicate:
        return i18n("Duplicate page");
    case PdfNotebookQuicks::Action::Delete:
        return i18n("Delete page");
    case PdfNotebookQuicks::Action::TurnLeft:
        return i18n("Rotate page left");
    case PdfNotebookQuicks::Action::TurnRight:
        return i18n("Rotate page right");
    }

    /// Not reachable, and empty rather than a guess: PdfNotebookQuicks::all() is the only source of
    /// these values, and this switch covers every one of them.
    return QString();
}

void runPdfIoQuickAction(PdfNotebookQuicks::Action action)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    const QString title = pdfIoQuickTitle(action);

    /// Asked only for an operation the page can actually take, and once more underneath by
    /// PdfNotebookQuicks::run. Both of this file's callers disable what is impossible, so a refusal
    /// here is the application's own last word rather than the way a user finds out -- and it is
    /// never a question about a page that does not exist.
    const bool possible = PdfNotebookQuicks::available(
        action, navigator->pageCount(), navigator->currentIndex(), nullptr);
    if (possible && action == PdfNotebookQuicks::Action::Delete && !confirmPageDelete(title)) {
        return;
    }

    /// One call into the same gate the rest of the notebook operations use, so the panel button and
    /// the submenu entry cannot do different things: the open pages are written, the operation is
    /// applied as one change, and the notebook is reloaded onto the page it answers with.
    applyNotebookOperation(title, [action](const QString &dir, int page) {
        return PdfNotebookQuicks::run(dir, action, page, PdfPageRotator::rotateInto);
    });
}

void openPdfIoNotebookOpsScreen()
{
    openNotebookOpsScreen(nullptr);
}

bool pdfIoCanUndoNotebookChange()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    return navigator->hasNotebook() && PdfNotebookOps::canUndo(navigator->projectDir());
}

void pdfIoUndoNotebookChange()
{
    /// One change deep, which is what the notebook's undo has always been: an operation closes the
    /// page it was made on, so it is its own unit of undo, and this is that unit. Inserting pages
    /// and merging a notebook are operations like any other, so both come back whole.
    applyNotebookOperation(i18n("Undo the last notebook change"), [](const QString &dir, int) {
        return PdfNotebookOps::undoLast(dir);
    });
}

void PdfIoPlugin::slotInsertPages()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        QMessageBox::information(nullptr, i18n("Insert pages from a PDF"),
                                 i18n("No notebook is open."));
        return;
    }

#if defined(Q_OS_ANDROID)
    /// The same picker the import path uses, told to filter PDFs. The copy happens in the activity
    /// callback and nothing else does: opening the PDF and the operation itself run on the event
    /// loop afterwards, which is the rule a crash in that callback taught us.
    auto *picker = new AndroidDocumentPicker(this);
    picker->pickFile(QStringLiteral("application/pdf"), QStringLiteral("pdfio-picked-pages.pdf"),
                     [this](const QString &localPath, const QString &why) {
                         QTimer::singleShot(0, this, [localPath, why]() {
                             insertPickedPdf(localPath, why);
                         });
                     });
#else
    const QString picked = QFileDialog::getOpenFileName(
        nullptr, i18n("Insert pages from a PDF"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        i18n("PDF documents (*.pdf)"));
    insertPickedPdf(picked, QString());
#endif
}

void PdfIoPlugin::slotInsertImage()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    KisDocument *document = navigator->currentDocument();
    if (!document || !document->image()) {
        say(QStringLiteral("open a notebook and a page before inserting an image"));
        return;
    }
    const QString projectDir = navigator->projectDir();
    if (projectDir.isEmpty()) {
        say(QStringLiteral("this page is not part of a notebook, so there is nowhere to put the image"));
        return;
    }

#if defined(Q_OS_ANDROID)
    /// The same picker the PDF and bundle paths use, told to filter images. The copy happens in the
    /// activity callback and nothing else does: loading, placing and saving all run on the event
    /// loop afterwards, which is the rule the crash taught us.
    auto *picker = new AndroidDocumentPicker(this);
    picker->pickFile(QStringLiteral("image/*"), QStringLiteral("pdfio-picked-image.png"),
                     [this, projectDir](const QString &localPath, const QString &why) {
                         QTimer::singleShot(0, this, [this, localPath, why, projectDir]() {
                             placeInsertedImage(localPath, why, projectDir);
                         });
                     });
#else
    const QString picked = QFileDialog::getOpenFileName(
        nullptr, i18n("Insert image"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        i18n("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
    placeInsertedImage(picked, QString(), projectDir);
#endif
}

/// Puts the picked image on the page that is open as a raster layer of its own.
///
/// A paint layer, not a file layer. A KisFileLayer built over the navigator's page image crashes in
/// ctor" and then SIGSEGV, with the image already read and the page open -- and the raster copy is
/// the more robust shape anyway: the pixels live in the artifact, so nothing depends on an outside
/// file staying where it was, and the notebook needs no second copy to keep in sync.
///
/// An abandoned pick leaves nothing behind: a cancelled dialog, an unreadable file, or a page that
/// went away while the picker was up all end here.
void PdfIoPlugin::placeInsertedImage(const QString &picked, const QString &why,
                                     const QString &projectDir)
{
    Q_UNUSED(projectDir);

    if (picked.isEmpty()) {
        say(QStringLiteral("no image was inserted%1")
                .arg(why.isEmpty() ? QString() : QStringLiteral(": ") + why));
        return;
    }

    QImage picture(picked);
    /// The picker's cache copy is only ever the way in; it goes whether or not this works out.
    QFile::remove(picked);
    if (picture.isNull()) {
        say(QStringLiteral("that file could not be read as an image; nothing was inserted"));
        return;
    }

    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    KisDocument *document = navigator->currentDocument();
    if (!document || !document->image()) {
        say(QStringLiteral("the page was closed while the picker was open; nothing was inserted"));
        return;
    }
    KisImageSP page = document->image();

    /// Scaled to sit comfortably on the page and centred on it.
    const int side = qMax(1, qMin(page->width(), page->height()) * 3 / 5);
    const QSize wanted = picture.size().scaled(side, side, Qt::KeepAspectRatio);
    const QImage scaled = picture.scaled(wanted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    auto *layer = new KisPaintLayer(page, QStringLiteral("Inserted image"), OPACITY_OPAQUE_U8);
    layer->paintDevice()->convertFromQImage(scaled, nullptr, 0, 0);
    layer->setX((page->width() - wanted.width()) / 2);
    layer->setY((page->height() - wanted.height()) / 2);
    /// Inside the page's Ink group, not beside it at the root: the picture is part of what the page
    /// is made of, and the group is what the layer panel and the strip work with. Still its own
    /// layer, so the no-merge rule stands and a stroke and the picture stay separate children.
    ///
    /// A page with no Ink group -- one made before the group existed, say -- takes the layer at the
    /// root with a line saying so, rather than losing the picture.
    KisNodeSP parent = page->rootLayer();
    for (quint32 i = 0; parent.data() == page->rootLayer().data() && i < page->rootLayer()->childCount(); ++i) {
        KisNodeSP child = page->rootLayer()->at(i);
        if (child->name() == PdfProjectBuilder::inkLayerName()) {
            parent = child;
        }
    }
    if (parent.data() == page->rootLayer().data()) {
        say(QStringLiteral("this page has no Ink group; the image went to the page root"));
    }

    if (!page->addNode(layer, parent)) {
        say(QStringLiteral("the image layer could not be added to the page"));
        return;
    }

    page->refreshGraphAsync(page->root(), { page->bounds() }, page->bounds());
    page->waitForDone();

    say(QStringLiteral("inserted a %1x%2 image onto page %3 as a content layer")
            .arg(wanted.width()).arg(wanted.height()).arg(navigator->currentIndex() + 1));

    /// And written into the page's artifact straight away, so the notebook carries it whether or not
    /// the user saves again.
    slotSavePage();
}



namespace {

/**
 * Opens the "Notebook ops" submenu for a screenshot run, retrying while the window is still being
 * built.
 *
 * The container's screenshot script has no way to click: no xdotool, no window manager, only Xvfb
 * and one capture. So the menu opens itself when PDFIO_PROBE_MENU is set, on the same terms as the
 * other PDFIO_PROBE_* hooks -- a run-time switch a screenshot needs, not a code path a user takes.
 */
void showNotebookOpsMenuForShot(int attemptsLeft)
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    QMenu *notebook = (window && window->menuBar())
                          ? window->menuBar()->findChild<QMenu *>(QStringLiteral("pdfio_menu"))
                          : nullptr;
    QMenu *ops =
        notebook ? notebook->findChild<QMenu *>(QStringLiteral("pdfio_notebook_ops")) : nullptr;
    if (!ops) {
        if (attemptsLeft > 0) {
            QTimer::singleShot(500, qApp,
                               [attemptsLeft]() { showNotebookOpsMenuForShot(attemptsLeft - 1); });
        } else {
            qWarning("[pdfio] PDFIO_PROBE_MENU: no Notebook ops submenu was found to show");
        }
        return;
    }

    ops->popup(QPoint(320, 220));
    qWarning("[pdfio] PDFIO_PROBE_MENU: the Notebook ops submenu is up");
}

} // namespace

PdfIoPlugin::PdfIoPlugin(QObject *parent, const QVariantList &)
    : KisActionPlugin(parent)
{
    registerActions();

    /// A screenshot run asks for the submenu to be put on screen; once per process, because the
    /// plugin is constructed for every view.
    if (qEnvironmentVariableIntValue("PDFIO_PROBE_MENU") > 0) {
        static bool scheduled = false;
        if (!scheduled) {
            scheduled = true;
            QTimer::singleShot(4000, qApp, []() { showNotebookOpsMenuForShot(20); });
        }
    }


    /// Once per process: a view plugin is created for every view.
    registerPdfIoDocker();

    /// The Start screen asks for its notebook entries; the welcome page applies them when it is
    /// built, whenever that happens, because the registration is kept statically.
    refreshWelcomePageEntries();

    /// Temporary: answers whether the Android render backend can be pure C++.
    PdfRendererSpike::run();

    QString probePath = qEnvironmentVariable("PDFIO_PROBE");
#if defined(Q_OS_ANDROID)
    /// Temporary: adb cannot hand an environment variable to an Android application, so on
    /// Android the probe always runs, against the fixture it writes for itself.
    probePath = QStringLiteral("__builtin__");
#endif
    if (probePath.isEmpty()) {
        return;
    }

#if !defined(Q_OS_ANDROID)
    /// On desktop Krita's own message handler swallows plugin output during startup, so the probe
    /// routes everything to stderr. Not on Android, where stderr goes nowhere and Krita's Android
    /// log handler is the thing that reaches logcat -- installing this there swallowed the very
    /// output it was meant to reveal.
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        fprintf(stderr, "[probe] %s\n", qPrintable(message));
        fflush(stderr);
    });
#endif

    PdfIoProbe::runIfRequested();


    QTimer::singleShot(0, this, [this, probePath]() {
        /// Android is driven by the menu action. The unattended route that opened a file from the
        /// cache at startup is gone: it existed to reproduce the open path crash, and it found it.
        /// Opening a document automatically on every launch would only surprise the user now.
        if (qEnvironmentVariableIntValue("PDFIO_PROBE_STRIP") > 0) {
            runStripProbe();
            return;
        }
        if (qEnvironmentVariableIntValue("PDFIO_PROBE_THUMBS") > 0) {
            runThumbnailProbe();
            return;
        }
        if (qEnvironmentVariableIntValue("PDFIO_PROBE_PAN") > 0) {
            runPanProbe();
            return;
        }
        if (qEnvironmentVariableIntValue("PDFIO_PROBE_RESTORE") > 0) {
            runRestoreProbe();
            return;
        }
        const int scale = qEnvironmentVariableIntValue("PDFIO_PROBE_SCALE");
        if (scale > 0) {
            runScaleProbe(scale);
            return;
        }
        const bool opened = openNotebook(probePath);
        say(QStringLiteral("openNotebook(%1) = %2").arg(probePath).arg(opened));
    });
}

PdfIoPlugin::~PdfIoPlugin()
{
}

void PdfIoPlugin::registerActions()
{
    /// The menu is made even when there is no view manager, because that is the state of the first
    /// screen: the plugin is created once per view, and before a document exists there is no view,
    /// so the whole "PDF Notebook" menu used to be missing exactly where "Open PDF as notebook" is
    /// needed. Everything else in the menu means nothing without a notebook and arrives with the
    /// first view.
    KisMainWindow *window = viewManager() ? viewManager()->mainWindow()
                                          : KisPart::instance()->currentMainwindow();
    if (!window || !window->menuBar()) {
        /// Start-up: the main window is still being built when the plugin is constructed, which is
        /// what the probe path had to defer for. Retried a bounded number of times; past that
        /// there is no window to hang a menu on, and the first view brings one.
        if (m_menuTries < 20) {
            ++m_menuTries;
            QTimer::singleShot(500, this, [this]() { registerActions(); });
        }
        return;
    }

    if (!viewManager() || !viewManager()->actionManager()) {
        QMenu *alone = window->menuBar()->findChild<QMenu *>(QStringLiteral("pdfio_menu"));
        if (!alone) {
            alone = window->menuBar()->addMenu(i18n("PDF Notebook"));
            alone->setObjectName(QStringLiteral("pdfio_menu"));
        }

        /// Recent first, the same order as the full menu: it is the entry the user reaches for
        /// first with nothing open.
        addRecentNotebooksMenu(alone);
        addMenuSeparator(alone, QStringLiteral("pdfio_open_separator"));

        /// A plain action: the KisAction that carries the icon and the shortcut is the action
        /// manager's to make, and there is none yet. The registration that does have a view
        /// manager takes it off the menu again, so the entry is never doubled.
        if (!alone->findChild<QAction *>(QStringLiteral("pdfio_open_notebook_alone"))) {
            QAction *open = alone->addAction(i18n("Import PDF as notebook..."));
            open->setObjectName(QStringLiteral("pdfio_open_notebook_alone"));
            connect(open, &QAction::triggered, this, &PdfIoPlugin::slotOpenNotebook);
        }

        /// The bundle's inverse needs a stand-in here too: opening a notebook that came from
        /// another device is one of the three things a person looks for before any document is
        /// open, and without this it was reachable only once a view existed. Removed the same way
        /// the import stand-in is when the action manager arrives.
        if (!alone->findChild<QAction *>(QStringLiteral("pdfio_open_bundle_alone"))) {
            QAction *openBundle = alone->addAction(i18n("Open a notebook file..."));
            openBundle->setObjectName(QStringLiteral("pdfio_open_bundle_alone"));
            connect(openBundle, &QAction::triggered, this, &PdfIoPlugin::slotOpenNotebookBundle);
        }

        /// Everything that changes the notebook itself, Rename included, lives under one entry
        /// here as it does in the full menu -- greyed while there is no notebook to act on.
        addNotebookOpsMenu(alone, this);
        return;
    }

    struct Entry {
        const char *name;
        void (PdfIoPlugin::*slot)();
    };

    const Entry entries[] = {
        { "pdfio_open_notebook", &PdfIoPlugin::slotOpenNotebook },
        { "pdfio_open_bundle", &PdfIoPlugin::slotOpenNotebookBundle },
        /// The image goes onto the page that is open, so it leads the document block rather than
        /// sitting with the two that create a notebook.
        { "pdfio_insert_image", &PdfIoPlugin::slotInsertImage },
        { "pdfio_save_page", &PdfIoPlugin::slotSavePage },
        { "pdfio_next_page", &PdfIoPlugin::slotNextPage },
        { "pdfio_previous_page", &PdfIoPlugin::slotPreviousPage },
        { "pdfio_export_pdf", &PdfIoPlugin::slotExportPdf },
        { "pdfio_save_notebook", &PdfIoPlugin::slotSaveNotebook },
        { "pdfio_save_bundle", &PdfIoPlugin::slotSaveNotebookAsBundle },
    };

    QMenu *menu = nullptr;
    if (window->menuBar()) {
        menu = window->menuBar()->findChild<QMenu *>(QStringLiteral("pdfio_menu"));
        if (!menu) {
            menu = window->menuBar()->addMenu(i18n("PDF Notebook"));
            menu->setObjectName(QStringLiteral("pdfio_menu"));
        }

        /// The Start screen's two notebook buttons use the very same handlers as the menu below.
        /// The registration is cleared when this instance goes, so a welcome page that outlives it
        /// does not call into a dead object.
        KisWelcomePageWidget::setNotebookActions([this]() { slotOpenNotebook(); },
                                                 [this]() { slotOpenNotebookBundle(); });
        connect(this, &QObject::destroyed, []() {
            KisWelcomePageWidget::setNotebookActions(nullptr, nullptr);
        });

        /// And the first-screen stand-ins go before the real actions are added: two entries that
        /// do the same thing is how a menu starts looking broken.
        if (QAction *standin = menu->findChild<QAction *>(QStringLiteral("pdfio_open_notebook_alone"))) {
            menu->removeAction(standin);
            standin->deleteLater();
        }
        if (QAction *standin = menu->findChild<QAction *>(QStringLiteral("pdfio_open_bundle_alone"))) {
            menu->removeAction(standin);
            standin->deleteLater();
        }
    }

    auto addEntry = [this, menu](const Entry &entry) {
        KisAction *action = viewManager()->actionManager()->createAction(QString::fromLatin1(entry.name));
        if (!action) {
            return;
        }
        connect(action, &KisAction::triggered, this, entry.slot);

        /// Creating an action does not put it anywhere. Without this the plugin is invisible.
        if (menu) {
            menu->addAction(action);
        }
    };

    const int entryCount = int(sizeof(entries) / sizeof(entries[0]));

    /// Recent first: the entry the user reaches for first, open document or not.
    addRecentNotebooksMenu(menu);
    addMenuSeparator(menu, QStringLiteral("pdfio_open_separator"));

    /// Import and Open next. They are what creates the first document, so they stay enabled with
    /// nothing open (their activationFlags are NONE in the .action file) and they are not part of
    /// the block below that only means something once a page is on screen.
    for (int i = 0; i < 2 && i < entryCount; ++i) {
        addEntry(entries[i]);
    }
    addMenuSeparator(menu, QStringLiteral("pdfio_document_separator"));

    /// The document's own entries: save, turn, export, and the two ways of writing the notebook out.
    for (int i = 2; i < entryCount; ++i) {
        addEntry(entries[i]);
    }

    /// The notebook's own operations: move, duplicate, delete, undo and rename. They act on the
    /// notebook that is open, so they follow the document block rather than sitting above it.
    addNotebookOpsMenu(menu, this);

    /// The strip switch. The strip existed behind PDFIO_PROBE_STRIP only, which nobody can set on
    /// a tablet; a checkable action is both the way in and the indicator of which mode is in force.
    KisAction *stripAction =
        viewManager()->actionManager()->createAction(QStringLiteral("pdfio_strip_mode"));
    if (stripAction) {
        stripAction->setCheckable(true);
        m_stripAction = stripAction;
        connect(stripAction, &KisAction::toggled, this, &PdfIoPlugin::slotToggleStripMode);
        if (menu) {
            addMenuSeparator(menu, QStringLiteral("pdfio_strip_separator"));
            menu->addAction(stripAction);
        }
    }
    updateStripAction();

    /// The strip's "how many" half, immediately under the switch that is its on/off half. The switch
    /// keeps its own way out -- unchecking it is one page at a time -- and does not forget the count:
    /// that is the navigator's pdfio/stripPages, which this control owns.
    addStripPagesMenu(menu, [this](int pages) {
        applyStripPages(pages,
                        [this]() { updateStripAction(); },
                        [this](int page) {
                            QTimer::singleShot(0, this, [this, page]() { rebuildForScope(page, 6); });
                        });
    });

    /// The page-loading trigger, beside the two strip controls: WHEN the follow is allowed to turn
    /// and how much of a page is on screen when the strip is fitted. One addXxxMenu() each and one
    /// line each here, so a later setting cannot collide with either by editing a shared block.
    addScrollSettleMenu(menu);
    addReadingShareMenu(menu);

    /// A switch rather than a plain action: turning pages by panning is the same gesture as
    /// looking at the bottom of a page, and whoever reads that way will want it off.
    KisAction *followAction =
        viewManager()->actionManager()->createAction(QStringLiteral("pdfio_follow_scrolling"));
    if (followAction) {
        followAction->setCheckable(true);
        followAction->setChecked(PdfPageNavigator::instance()->scrollFollowEnabled());
        connect(followAction, &KisAction::toggled, this, [](bool enabled) {
            PdfPageNavigator::instance()->setScrollFollowEnabled(enabled);
        });
        if (menu) {
            addMenuSeparator(menu, QStringLiteral("pdfio_follow_separator"));
            menu->addAction(followAction);
        }
    }

    /// The rendered page memory: the one knob the strip's memory answers to, next to the two
    /// switches above because it is a display setting and not an operation on the notebook.
    addMemoryBudgetMenu(menu);
}

void PdfIoPlugin::updateStripAction()
{
    if (!m_stripAction) {
        return;
    }

    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    const bool strip = navigator->scope() > 1;

    /// The count the strip is SET TO, rather than the five this used to be hard-coded at: the label
    /// says how many pages the switch is on at, and "Strip pages" is where that number comes from.
    /// The count is remembered while the strip is off, so the label still names it.
    ///
    /// What it NAMES for an open notebook is what that notebook can show: the count is not clamped
    /// when it is set (it is set before the notebook it is meant for as often as after), and a
    /// "nine page" label on a five page notebook would promise pages that are not there. With nothing
    /// open there is nothing to bound it, and the setting itself is named.
    const int asked = navigator->stripPageCount();
    const int pages = navigator->hasNotebook()
        ? qMin(asked, qMax(1, navigator->pageCount()))
        : asked;
    const int side = (pages - 1) / 2;
    const QString label = i18n("%1-page strip (active ±%2)", pages, side);

    /// The check mark is the mode indicator, and the text says what is on without a menu open.
    m_stripAction->setChecked(strip);
    m_stripAction->setText(strip ? i18n("%1 is on", label) : label);
    m_stripAction->setToolTip(strip
        ? i18n("The page above and the page below are shown in the same document. "
               "Choose again to go back to one page at a time.")
        : i18n("Show the pages above and below the open one as well, in one document. "
               "Choose again to go back to one page at a time."));
}

void PdfIoPlugin::rebuildForScope(int pageIndex, int attemptsLeft)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();

    /// The document that is open is still in the way while Krita is closing it. Its close is
    /// deferred, so this waits rather than spinning: each attempt gives the event loop 700 ms.
    if (navigator->currentDocument() && attemptsLeft > 0) {
        if (KisDocument *document = navigator->currentDocument()) {
            /// Its ink was written just before this was called; leaving it modified would make
            /// Krita ask whether to save it while the close is already under way.
            document->setModified(false);
        }
        if (KisView *view = navigator->currentView()) {
            view->closeView();
        }

        QTimer::singleShot(700, this, [this, pageIndex, attemptsLeft]() {
            rebuildForScope(pageIndex, attemptsLeft - 1);
        });
        return;
    }

    QString why;
    if (!navigator->showPage(pageIndex, &why)) {
        say(QStringLiteral("strip: cannot re-open page %1: %2").arg(pageIndex + 1).arg(why));
        return;
    }

    say(QStringLiteral("strip: page %1 is open with %2 page(s) in the document, scope %3")
            .arg(pageIndex + 1)
            .arg(navigator->scope())
            .arg(navigator->scope()));
}

void PdfIoPlugin::slotToggleStripMode()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();

    /// The switch is the ON/OFF half: on restores the count the strip last had -- five unless the
    /// "Strip pages" menu changed it -- and off is one page at a time. Which count that is belongs to
    /// the navigator, so turning the strip off cannot forget it, and the same apply path the submenu
    /// uses is what puts the change on screen.
    const bool on = m_stripAction && m_stripAction->isChecked();
    const int pages = on ? navigator->stripPageCount() : 1;

    applyStripPages(pages,
                    [this]() { updateStripAction(); },
                    [this](int page) {
                        QTimer::singleShot(0, this, [this, page]() { rebuildForScope(page, 6); });
                    });
}

void PdfIoPlugin::slotOpenNotebook()
{
#if defined(Q_OS_ANDROID)
    /// QFileDialog is not usable here: the application has no broad filesystem access, so the
    /// file arrives as a content URI and has to be copied to a path the renderer can open.
    auto *picker = new AndroidDocumentPicker(this);
    say(QStringLiteral("the document picker is opening"));
    picker->pickPdf([this, picker](const QString &localPath, const QString &why) {
        /// Read before the picker is released, and used only on the event loop below: the URI is
        /// what the provider's display name can be asked for, and asking for it here is what
        /// crashed the application. The copy above this point is untouched.
        const QString contentUri = picker->pickedContentUri();
        picker->deleteLater();
        say(QStringLiteral("picker finished: path \"%1\" reason \"%2\"").arg(localPath, why));

        if (localPath.isEmpty()) {
            say(QStringLiteral("nothing was opened: %1").arg(why));
            return;
        }

        /// Deferred out of the activity result callback. Opening a document builds a view and
        /// walks the resource system, and doing that while the activity transition is still
        /// unwinding crashed inside Qt's own hash tables.
        QTimer::singleShot(0, this, [this, localPath, contentUri]() {
            /// Replaces whatever is open: the picker route has the same rule as the menu route,
            /// because importing while a page is open is exactly the case that opened nothing.
            openNotebookReplacing(localPath, contentUri, 6);
        });
    });
#else
    const QString path = QFileDialog::getOpenFileName(nullptr,
                                                      i18n("Import PDF as notebook"),
                                                      QString(),
                                                      i18n("PDF documents (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }

    /// Replaces whatever is open, closing the page that is there first: importing from a state
    /// that already has a page open is the case that used to end with nothing on screen.
    openNotebookReplacing(path, QString(), 6);
#endif
}

void PdfIoPlugin::slotNextPage()
{
    QString why;
    if (!PdfPageNavigator::instance()->next(&why)) {
        qWarning() << "pdfio:" << why;
    }
}

void PdfIoPlugin::slotPreviousPage()
{
    QString why;
    if (!PdfPageNavigator::instance()->previous(&why)) {
        qWarning() << "pdfio:" << why;
    }
}

void PdfIoPlugin::slotSaveNotebook()
{
    /// Every page in the strip, not only the one that is open. Their ink is all in one layer and
    /// each page is picked out by the rectangle it occupies, so the cropping can be done whenever
    /// rather than only on the way out of a page.
    if (!PdfPageNavigator::instance()->saveStripPages()) {
        qWarning() << "pdfio: could not save the notebook";
    }
}

QString PdfIoPlugin::bundleSuggestion() const
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();

    /// The notebook's own name, the same one the tab and the export suggestion use. The source file
    /// is the cache copy the PDF arrived in, so its base name is the "pdfio-picked" the user never
    /// chose -- which is exactly what this used to suggest. Only a notebook with no name of its own
    /// falls back to it, and only a notebook at all falls back to "notebook".
    QString base;
    if (navigator->hasNotebook()) {
        /// From disk, not from the navigator's copy: a rename is written out and the copy the
        /// navigator holds is not updated with it, so a suggestion read from memory kept the old
        /// name -- measured on the desktop, "bundle suggestion after rename: text-fixture.pnb".
        base = notebookNameFromDisk(navigator->projectDir());
        if (base.isEmpty()) {
            base = PdfPageNavigator::defaultNotebookName();
        }
    }
    if (base.isEmpty()) {
        base = QStringLiteral("notebook");
    }
    return base + QLatin1Char('.') + PdfNotebookBundle::extension();
}

void PdfIoPlugin::slotSaveNotebookAsBundle()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        say(QStringLiteral("save as one file: no notebook is open"));
        return;
    }

    /// The ink that is on screen lives in the document until a save writes it out, so the notebook
    /// is written first. Krita saves in the background and the navigator hands out no completion
    /// callback, so on the desktop the file dialog -- which takes the user a moment -- is what lets
    /// that save finish, and on Android a short timer is. A stroke made in the last instant before
    /// this action can therefore still miss the bundle; docs/verify/BUNDLE-CORE.md says so plainly
    /// rather than pretending the chain is airtight.
    navigator->saveStripPages();

    const QString suggested = bundleSuggestion();

#if defined(Q_OS_ANDROID)
    /// Android has no useful file dialog: the bundle is written to a temporary file and handed to
    /// the system's document creator, which is where the user picks the real destination -- the
    /// same route the PDF export takes.
    const QString staged = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                               .filePath(QStringLiteral("pdfio-notebook.pnb"));

    QTimer::singleShot(600, this, [this, navigator, staged, suggested]() {
        QString why;
        if (!PdfNotebookBundle::save(navigator->projectDir(), staged, &why)) {
            say(QStringLiteral("the notebook could not be written as one file: %1").arg(why));
            return;
        }
        say(QStringLiteral("staged %1 (%2 bytes)").arg(staged).arg(QFileInfo(staged).size()));

        auto *writer = new AndroidDocumentPicker(this);
        writer->createBundle(suggested, staged, [this, writer](bool written, const QString &why) {
            writer->deleteLater();
            if (!written) {
                say(QStringLiteral("the notebook was not saved: %1").arg(why));
                return;
            }
            say(QStringLiteral("the notebook was saved to the location that was chosen"));
        });
    });
#else
    const QString target = QFileDialog::getSaveFileName(nullptr,
                                                        i18n("Save the notebook as one file"),
                                                        suggested,
                                                        PdfNotebookBundle::fileFilter());
    if (target.isEmpty()) {
        return;
    }

    /// The dialog's own filter is a convenience, not a rule, so the suffix is added when the user
    /// typed a name without one: every file manager then knows what the file is.
    QString destination = target;
    if (!destination.endsWith(QLatin1Char('.') + PdfNotebookBundle::extension(), Qt::CaseInsensitive)) {
        destination += QLatin1Char('.') + PdfNotebookBundle::extension();
    }

    QString why;
    if (!PdfNotebookBundle::save(navigator->projectDir(), destination, &why)) {
        say(QStringLiteral("the notebook could not be written as one file: %1").arg(why));
        return;
    }
    say(QStringLiteral("saved the notebook as %1 (%2 bytes)")
            .arg(destination).arg(QFileInfo(destination).size()));
#endif
}

void PdfIoPlugin::slotOpenNotebookBundle()
{
#if defined(Q_OS_ANDROID)
    /// The same route as opening a PDF: QFileDialog is not usable, the file arrives as a content
    /// URI, and it has to be copied somewhere the archive can be read from.
    auto *picker = new AndroidDocumentPicker(this);
    say(QStringLiteral("the notebook picker is opening"));
    picker->pickBundle([this, picker](const QString &localPath, const QString &why) {
        picker->deleteLater();
        say(QStringLiteral("notebook picker finished: path \"%1\" reason \"%2\"").arg(localPath, why));
        if (localPath.isEmpty()) {
            say(QStringLiteral("nothing was opened: %1").arg(why));
            return;
        }

        /// Deferred out of the activity result callback, for the reason the PDF open is: opening a
        /// document builds a view and walks the resource system, and doing that while the activity
        /// transition unwinds crashed inside Qt's own hash tables.
        QTimer::singleShot(0, this, [this, localPath]() { openBundleFile(localPath, true); });
    });
#else
    const QString path = QFileDialog::getOpenFileName(nullptr,
                                                      i18n("Open a notebook file"),
                                                      QString(),
                                                      PdfNotebookBundle::fileFilter());
    if (path.isEmpty()) {
        return;
    }
    openBundleFile(path, false);
#endif
}

void PdfIoPlugin::openBundleFile(const QString &bundlePath, bool replaceWithoutAsking)
{
    /// Read before writing: inspect() makes every check extract() makes and touches nothing, so a
    /// file that is not a notebook is refused before a directory is created for it.
    QString why;
    const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(bundlePath, &why);
    if (!info.isValid()) {
        say(QStringLiteral("this file is not a notebook: %1").arg(why));
        return;
    }

    say(QStringLiteral("bundle: %1 pages, %2 files, %3 withheld, %4 bytes%5")
            .arg(info.manifest.pages.size())
            .arg(info.entries.size())
            .arg(info.withheld.size())
            .arg(info.bundleBytes)
            .arg(info.unknown.isEmpty()
                     ? QString()
                     : QStringLiteral(", ignoring %1 entries that are not part of a notebook")
                           .arg(info.unknown.size())));

    const QString root = PdfNotebookBundle::defaultProjectRoot();
    if (!QDir().mkpath(root)) {
        say(QStringLiteral("cannot create %1").arg(root));
        return;
    }

    /// The name the navigator gives this notebook -- source base name and source hash -- so that
    /// opening the source below finds the project that was just unpacked instead of making a
    /// second, empty one beside it.
    const QString destination = QDir(root).filePath(PdfNotebookBundle::extractDirName(info.manifest));

    bool replace = replaceWithoutAsking;
    if (!replace && QFileInfo::exists(destination)) {
        const auto answer = QMessageBox::question(
            nullptr,
            i18n("A notebook for this source is already here"),
            i18n("This device already has a notebook for %1. Replace it with the one in the file?",
                 info.manifest.sourceFile));
        if (answer != QMessageBox::Yes) {
            say(QStringLiteral("the notebook already on the device was left alone"));
            return;
        }
        replace = true;
    }

    PdfNotebookBundle::ExtractOptions options;
    options.replaceExisting = replace;

    QStringList ignored;
    why.clear();
    if (!PdfNotebookBundle::extract(bundlePath, destination, options, &why, &ignored)) {
        say(QStringLiteral("the notebook could not be unpacked: %1").arg(why));
        return;
    }
    say(QStringLiteral("unpacked the notebook to %1").arg(destination));

    /// Deferred, for the same reason the PDF open is, and on Android also to get out of the
    /// activity result callback.
    QTimer::singleShot(0, this, [this, destination, info]() {
        const QString source = QDir(destination).filePath(info.manifest.sourceFile);

        /// Replaces whatever is open, exactly as the PDF import does: a notebook opened from a
        /// file has to end up open even when another notebook was already there. The check below
        /// runs after the open has actually happened, deferral and all.
        openNotebookReplacing(source, QString(), 6, [destination]() {
            /// Opening goes through the navigator, which keys a project by the source's own hash.
            /// It finds the directory just written only while its root is the one assumed here, so
            /// a drift between the two is said out loud instead of leaving an empty notebook and no
            /// reason.
            if (QFileInfo(PdfPageNavigator::instance()->projectDir()).absoluteFilePath()
                != QFileInfo(destination).absoluteFilePath()) {
                say(QStringLiteral("WARNING: the notebook opened from %1, not from %2: the project root "
                                   "assumed by PdfNotebookBundle::defaultProjectRoot() no longer "
                                   "matches the navigator's, and the ink that came in the file was "
                                   "not used")
                        .arg(PdfPageNavigator::instance()->projectDir(), destination));
            }
        });
    });
}

void PdfIoPlugin::slotExportPdf()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        qWarning() << "pdfio: no notebook is open";
        QMessageBox::warning(nullptr, i18n("Export to PDF"),
                             i18n("No notebook is open. Open a PDF as a notebook first."));
        return;
    }

#if defined(Q_OS_ANDROID)
    /// Android has no useful file dialog: the export is written to a temporary file and then handed
    /// to the system's document creator, which is where the user picks the real destination.
    const QString target = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                               .filePath(QStringLiteral("pdfio-export.pdf"));
#else
    const QString target = QFileDialog::getSaveFileName(nullptr,
                                                        i18n("Export the notebook to PDF"),
                                                        exportSuggestion(navigator),
                                                        i18n("PDF documents (*.pdf)"));
    if (target.isEmpty()) {
        return;
    }
#endif

    /// One image per page that was ever drawn on. The ink is read straight out of the saved
    /// artifacts, so exporting does not have to open a document per page.
    QHash<int, QImage> ink;
    const QDir project(navigator->projectDir());
    const QList<PdfPageRecord> pages = navigator->manifest().pages;
    for (int i = 0; i < pages.size(); ++i) {
        const QImage pageInk = PdfInkLoader::loadInk(project.filePath(pages.at(i).kraFile), nullptr);
        if (!pageInk.isNull()) {
            /// Keyed by the page's place in the NOTEBOOK, which is what the export walks: the ink of
            /// notebook page i belongs on the i-th page of the exported file, whatever page of the
            /// source PDF that page came from and whatever order the notebook is in.
            ink.insert(i, pageInk);
        }
    }

    QString why;
    if (!PdfExporter::exportProjectWithInk(navigator->projectDir(), navigator->manifest(),
                                           ink, target, &why)) {
        say(QStringLiteral("export failed: %1").arg(why));
        /// Said to the user, not only to the log. Measured on the tablet: the export can be
        /// refused before the document picker opens, and then pressing Export does nothing
        /// visible at all -- which is indistinguishable from a dead menu item.
        QMessageBox::warning(nullptr, i18n("Export to PDF"), why);
        return;
    }

    say(QStringLiteral("exported %1 of %2 pages with ink to %3")
            .arg(ink.size()).arg(navigator->pageCount()).arg(target));

#if defined(Q_OS_ANDROID)
    /// Off to wherever the user chooses. The temporary file is left behind on purpose: it is what
    /// the content resolver reads from, and the system may take its time getting there.
    auto *writer = new AndroidDocumentPicker(this);
    const QString suggested = exportSuggestion(navigator);
    writer->createPdf(suggested, target, [writer, this](bool written, const QString &why) {
        writer->deleteLater();
        if (!written) {
            say(QStringLiteral("the export was not saved: %1").arg(why));
            return;
        }
        say(QStringLiteral("the export was saved to the location that was chosen"));
    });
#endif
}

void PdfIoPlugin::slotSavePage()
{
    /// No platform guard: the saver is plain C++ and works wherever a render backend does.
    KisDocument *document = viewManager() ? viewManager()->document() : nullptr;
    if (!document || !document->image()) {
        qWarning() << "pdfio: no page is open";
        return;
    }

    const QString projectDir = document->property("pdfioProjectDir").toString();
    const int pageIndex = document->property("pdfioPageIndex").toInt();
    if (projectDir.isEmpty()) {
        qWarning() << "pdfio: this document is not a note page";
        return;
    }

    /// Cropped to the page the notebook has open, by the same rectangle the roll writes through.
    ///
    /// This used to build the document from the whole image, so on a strip page 1's artifact was
    /// the whole strip until the next roll rewrote it -- and a close, a reopen or an export right
    /// after an insert read a page-sized rectangle full of strip.
    QString why;
    KisDocument *pageDocument =
        PdfPageNavigator::instance()->pageLayersDocument(document, pageIndex, &why);
    if (!pageDocument) {
        qWarning() << "pdfio: cannot prepare the page:" << why;
        return;
    }

    const QString path = QDir(projectDir).filePath(PdfSession::pageFileName(pageIndex));
    QDir().mkpath(QFileInfo(path).absolutePath());

    /// Krita saves in the background, so the copy has to outlive this call. It is deleted when the
    /// save reports back, rather than by waiting here: a nested event loop around
    /// sigSavingFinished wedged on the second save.
    connect(pageDocument, &KisDocument::sigSavingFinished, this, [pageDocument, path](const QString &) {
        say(QStringLiteral("saved %1 (%2 bytes)").arg(path).arg(QFileInfo(path).size()));
        KisPart::instance()->removeDocument(pageDocument, true);
    });

    if (!PdfPageSaver::saveDocument(pageDocument, path, &why)) {
        qWarning() << "pdfio: cannot save:" << why;
        KisPart::instance()->removeDocument(pageDocument, true);
    }
}

bool PdfIoPlugin::openNotebook(const QString &pdfPath)
{
    QString why;
    const bool opened = PdfPageNavigator::instance()->openNotebook(pdfPath, &why);
    if (!opened) {
        say(QStringLiteral("openNotebook failed: %1").arg(why));
        return false;
    }
    notebookOpened();
    return true;
}

void PdfIoPlugin::runRestoreProbe()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;

    /// Pinned to one page before anything is opened: PdfProjectBuilder::inkStrokeLayer() only
    /// answers for a page-shaped image, and the notebook ships with a five-page strip, so without
    /// this the probe reaches "no paintable Ink layer" before it has drawn and the page path it
    /// exists to check is never exercised.
    navigator->setScope(1);

    /// The docker is put up first, because the crash being chased happened with it visible and a
    /// page clicked in it. Without it the probe is not reproducing the same thing.
    if (KisMainWindow *window = KisPart::instance()->currentMainwindow()) {
        auto *docker = new PdfIoDocker();
        window->addDockWidget(Qt::RightDockWidgetArea, docker);
        docker->show();
        say(QStringLiteral("restore: the notebook docker is up"));
    }

    if (!navigator->openNotebook(qEnvironmentVariable("PDFIO_PROBE"), &why)) {
        say(QStringLiteral("restore: cannot open the notebook: %1").arg(why));
        return;
    }

    /// Draw a mark into the Ink layer of whatever page is open.
    KisDocument *opened = navigator->currentDocument();
    if (!opened) {
        say(QStringLiteral("restore: no document is open"));
        return;
    }

    KisImageSP image = opened->image();
    KisPaintLayer *stroke = qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(image).data());
    if (!stroke) {
        say(QStringLiteral("restore: no paintable Ink layer"));
        return;
    }
    stroke->paintDevice()->fill(QRect(100, 100, 200, 40), KoColor(Qt::black, image->colorSpace()));

    /// The window only writes a page it believes is dirty, and a direct write into a paint device
    /// does not set that. Measured: without this the turn away from the page wrote no artifact, the
    /// reopen found nothing to restore, and this probe reported "ink bounds 0,0 0x0" -- testing the
    /// probe's setup rather than the restore it exists for. A stroke in the application arrives at
    /// the same flag through the undo system, which is what this stands in for.
    if (KisDocument *page = navigator->currentDocument()) {
        page->setModified(true);
    }
    say(QStringLiteral("restore: drew a mark, ink bounds now %1,%2 %3x%4")
            .arg(stroke->paintDevice()->exactBounds().x())
            .arg(stroke->paintDevice()->exactBounds().y())
            .arg(stroke->paintDevice()->exactBounds().width())
            .arg(stroke->paintDevice()->exactBounds().height()));

    Q_UNUSED(why);

    /// Each turn on a tick of its own, the way a person takes them -- and not only for realism:
    /// the page left behind is closed on a later turn of the event loop, so turning twice inside
    /// one call stacks three views and Krita's window handling wedges. The probe wedged there
    /// twice before this was understood.
    QTimer::singleShot(600, this, [this, navigator]() {
        QString why;
        if (!navigator->next(&why)) {
            say(QStringLiteral("restore: cannot turn forward: %1").arg(why));
            return;
        }
        say(QStringLiteral("restore: turned to page %1").arg(navigator->currentIndex() + 1));

        QTimer::singleShot(600, this, [this, navigator]() {
            QString why;
            if (!navigator->previous(&why)) {
                say(QStringLiteral("restore: cannot turn back: %1").arg(why));
                return;
            }
            say(QStringLiteral("restore: turned back to page %1").arg(navigator->currentIndex() + 1));

            QTimer::singleShot(600, this, [navigator]() {
                /// Not a ternary: document->image() hands back a weak pointer, and a conditional
                /// cannot mix that with a strong one.
                KisDocument *returned = navigator->currentDocument();
                if (!returned) {
                    say(QStringLiteral("restore: nothing is open after turning back"));
                    return;
                }
                KisImageSP back = returned->image();
                KisPaintLayer *backStroke =
                    qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(back).data());
                if (!backStroke) {
                    say(QStringLiteral("restore: the returned page has no Ink layer"));
                    return;
                }

                const QRect bounds = backStroke->paintDevice()->exactBounds();
                say(QStringLiteral("restore: back on page %1, ink bounds %2,%3 %4x%5  (expected 100,100 200x40)")
                        .arg(navigator->currentIndex() + 1)
                        .arg(bounds.x()).arg(bounds.y())
                        .arg(bounds.width()).arg(bounds.height()));
            });
        });
    });
}

void PdfIoPlugin::runPanProbe()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;
    if (!navigator->openNotebook(qEnvironmentVariable("PDFIO_PROBE"), &why)) {
        say(QStringLiteral("pan: cannot open the notebook: %1").arg(why));
        return;
    }

    QTimer::singleShot(900, this, [navigator]() {
        KisView *view = navigator->currentView();
        KisDocument *document = navigator->currentDocument();
        if (!view || !view->canvasBase() || !document || !document->image()) {
            say(QStringLiteral("pan: no canvas to measure"));
            return;
        }

        KisCanvas2 *canvas = view->canvasBase();
        const KisCoordinatesConverter *converter = canvas->coordinatesConverter();
        QWidget *widget = canvas->canvasWidget();
        if (!converter || !widget) {
            say(QStringLiteral("pan: no converter"));
            return;
        }

        const QRectF pageRect(0, 0, document->image()->width(), document->image()->height());

        QScrollBar *vertical = nullptr;
        QScrollBar *horizontal = nullptr;
        const QList<QScrollBar *> bars = widget->findChildren<QScrollBar *>();
        for (QScrollBar *bar : bars) {
            say(QStringLiteral("pan: %1 scrollbar range %2..%3 value %4 pageStep %5 widget %6x%7")
                    .arg(bar->orientation() == Qt::Vertical ? QStringLiteral("vertical")
                                                            : QStringLiteral("horizontal"))
                    .arg(bar->minimum()).arg(bar->maximum()).arg(bar->value())
                    .arg(bar->pageStep()).arg(widget->width()).arg(widget->height()));
            if (bar->orientation() == Qt::Vertical) {
                vertical = bar;
            } else {
                horizontal = bar;
            }
        }

        const QRectF pageOnScreen = converter->documentToWidget(pageRect);
        say(QStringLiteral("pan: page on screen %1,%2 %3x%4, viewport %5x%6")
                .arg(pageOnScreen.x()).arg(pageOnScreen.y())
                .arg(pageOnScreen.width()).arg(pageOnScreen.height())
                .arg(widget->width()).arg(widget->height()));

        if (vertical) {
            vertical->setValue(vertical->maximum());
            const QRectF atBottom = converter->documentToWidget(pageRect);
            say(QStringLiteral("pan: at the very bottom the page bottom sits at %1 of %2")
                    .arg(atBottom.bottom()).arg(widget->height()));
        }
        if (horizontal) {
            horizontal->setValue(horizontal->maximum());
            const QRectF atRight = converter->documentToWidget(pageRect);
            say(QStringLiteral("pan: at the far right the page right sits at %1 of %2")
                    .arg(atRight.right()).arg(widget->width()));
        }
    });
}

void PdfIoPlugin::runStripProbe()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    /// The scope the application ships with, not a number of the probe's own: the strip probe had
    /// its own three, so it tested a window the user never sees.
    navigator->setScope(5);

    QString why;
    if (!navigator->openNotebook(qEnvironmentVariable("PDFIO_PROBE"), &why)) {
        say(QStringLiteral("strip: cannot open the notebook: %1").arg(why));
        return;
    }

    const int first = navigator->currentIndex();
    say(QStringLiteral("strip: opened page %1 with scope %2")
            .arg(first + 1).arg(navigator->scope()));

    /// Not a ternary: document->image() hands back a weak pointer, and a conditional cannot mix
    /// that with a strong one. Third time this has been written the wrong way.
    KisDocument *document = navigator->currentDocument();
    if (!document) {
        say(QStringLiteral("strip: no document is open"));
        return;
    }
    KisImageSP image = document->image();
    if (!image) {
        say(QStringLiteral("strip: no image is open"));
        return;
    }

    /// The page's own rectangle inside the strip, worked out the same way the strip was: the mark
    /// goes a hundred pixels in from the page's corner, wherever that corner is.
    /// The navigator's OWN scope and resolution, not a number of the probe's own: the page-size
    /// bound can have changed the dpi the strip was built at, and a rectangle worked out at any
    /// other one is the wrong rectangle.
    const PdfStripLayout layout = PdfStripLayout::forWindow(
        navigator->manifest(), first, navigator->scope(), navigator->currentRenderDpi());
    const int slot = layout.slotForPage(first);
    if (!layout.isValid() || slot < 0) {
        say(QStringLiteral("strip: the layout does not hold that page"));
        return;
    }
    const QRect area = layout.slots().at(slot).rect;

    KisPaintLayer *stroke = nullptr;
    for (quint32 i = 0; i < image->root()->childCount() && !stroke; ++i) {
        KisNodeSP child = image->root()->at(i);
        if (child->name() != QStringLiteral("Ink")) {
            continue;
        }
        /// Both modes keep the stroke inside an "Ink" group now; a bare paint layer of that name
        /// is the older shape and is still accepted. The page is the part of the stroke inside its
        /// own rectangle, so either way this is the layer the mark is drawn into.
        if (KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(child.data())) {
            stroke = layer;
            break;
        }
        for (quint32 c = 0; c < child->childCount(); ++c) {
            if (KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(child->at(c).data())) {
                stroke = layer;
                break;
            }
        }
    }
    if (!stroke) {
        say(QStringLiteral("strip: no Ink layer for page %1").arg(first + 1));
        return;
    }

    stroke->paintDevice()->fill(QRect(area.x() + 100, area.y() + 100, 200, 40),
                                KoColor(Qt::black, image->colorSpace()));
    /// The window only writes a page it believes is dirty, and a direct write into a paint device
    /// does not set that: without this the turn writes no artifact, and the roll this probe exists
    /// to exercise has nothing to read back.
    if (KisDocument *page = navigator->currentDocument()) {
        page->setModified(true);
    }

    say(QStringLiteral("strip: drew at %1,%2 in the strip, which is the page's 100,100")
            .arg(area.x() + 100).arg(area.y() + 100));

    /// Where a stroke would go. Reported before and after every turn, because "the active page did
    /// not change" is exactly what it looks like when this does not move.
    auto activeNodeName = [navigator]() {
        KisView *view = navigator->currentView();
        if (!view || !view->viewManager() || !view->viewManager()->nodeManager()) {
            return QStringLiteral("(no node manager)");
        }
        KisNodeSP node = view->viewManager()->nodeManager()->activeNode();
        return node ? node->name() : QStringLiteral("(none)");
    };

    /// Where the canvas is looking. The page that is active is meant to be in the middle of the
    /// viewport, so turning a page has to move this -- and if it does not, the page did not move on
    /// screen however correct everything else is.
    auto centreName = [navigator]() {
        KisView *view = navigator->currentView();
        if (!view || !view->canvasController()) {
            return QStringLiteral("(no controller)");
        }
        const QPointF c = view->canvasController()->preferredCenter();
        return QStringLiteral("%1,%2").arg(int(c.x())).arg(int(c.y()));
    };

    say(QStringLiteral("strip: active node before any turn is \"%1\"").arg(activeNodeName()));
    say(QStringLiteral("strip: the canvas is looking at %1 before any turn").arg(centreName()));

    QTimer::singleShot(700, this, [this, navigator, first, activeNodeName, centreName]() {
        QString why;
        say(QStringLiteral("strip: turning forward"));
        if (!navigator->next(&why)) {
            say(QStringLiteral("strip: cannot turn forward: %1").arg(why));
            return;
        }

        say(QStringLiteral("strip: active node after turning forward is \"%1\"")
                .arg(activeNodeName()));
        say(QStringLiteral("strip: the canvas is looking at %1 after turning forward")
                .arg(centreName()));

        QTimer::singleShot(700, this, [this, navigator, first, activeNodeName, centreName]() {
            QString why;
            say(QStringLiteral("strip: turning back"));
            if (!navigator->previous(&why)) {
                say(QStringLiteral("strip: cannot turn back: %1").arg(why));
                return;
            }

            QTimer::singleShot(1200, this, [navigator, first]() {
                const PdfPageRecord &page = navigator->manifest().pages.at(first);
                const QImage ink = PdfInkLoader::loadInk(
                    QDir(navigator->projectDir()).filePath(page.kraFile), nullptr);

                const int expectedWidth = qRound(page.sizePt.width() * 200.0 / 72.0);
                const int expectedHeight = qRound(page.sizePt.height() * 200.0 / 72.0);

                QRect darkBounds;
                for (int y = 0; y < ink.height(); ++y) {
                    for (int x = 0; x < ink.width(); ++x) {
                        if (qAlpha(ink.pixel(x, y)) > 0) {
                            darkBounds = darkBounds.isNull()
                                ? QRect(x, y, 1, 1)
                                : darkBounds.united(QRect(x, y, 1, 1));
                        }
                    }
                }

                /// Turn far enough to MOVE the window. One turn forward and back never reaches
                /// the roll -- the active page has to hit the edge of the strip first -- and the
                /// roll is the path this probe exists to reach: a window move saves every page,
                /// reads them back and repaints.
                for (int i = 0; i < 6; ++i) {
                    QString rollWhy;
                    if (!navigator->next(&rollWhy)) {
                        say(QStringLiteral("strip: cannot turn forward any further: %1")
                                .arg(rollWhy));
                        break;
                    }
                }
                say(QStringLiteral("strip: after six turns the active page is %1")
                        .arg(navigator->currentIndex() + 1));

                /// The same artifact, read AGAIN after the window moved.
                ///
                /// The measurement below runs before the roll, and before a window move the page has
                /// not been written at all -- which is what made the line below report 0x0 while
                /// every artifact on disk carries a 16 to 24 KB merged image. This is the read that
                /// says whether the content actually came back.
                const QImage afterRoll = PdfInkLoader::loadInk(
                    QDir(navigator->projectDir()).filePath(page.kraFile), nullptr);
                QRect rolledBounds;
                for (int y = 0; y < afterRoll.height(); ++y) {
                    for (int x = 0; x < afterRoll.width(); ++x) {
                        if (qAlpha(afterRoll.pixel(x, y)) > 0) {
                            rolledBounds = rolledBounds.isNull()
                                ? QRect(x, y, 1, 1)
                                : rolledBounds.united(QRect(x, y, 1, 1));
                        }
                    }
                }
                say(QStringLiteral("strip: after the roll, page %1 artifact %2x%3, ink at "
                                   "%4,%5 %6x%7 (expected 100,100 200x40)")
                        .arg(first + 1)
                        .arg(afterRoll.width()).arg(afterRoll.height())
                        .arg(rolledBounds.x()).arg(rolledBounds.y())
                        .arg(rolledBounds.width()).arg(rolledBounds.height()));

                say(QStringLiteral("strip: artifact %1x%2, page is %3x%4, ink at %5,%6 %7x%8 "
                                   "(expected 100,100 200x40)")
                        .arg(ink.width()).arg(ink.height())
                        .arg(expectedWidth).arg(expectedHeight)
                        .arg(darkBounds.x()).arg(darkBounds.y())
                        .arg(darkBounds.width()).arg(darkBounds.height()));
            });
        });
    });
}

void PdfIoPlugin::runThumbnailProbe()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;
    if (!navigator->openNotebook(qEnvironmentVariable("PDFIO_PROBE"), &why)) {
        say(QStringLiteral("thumbs: cannot open the notebook: %1").arg(why));
        return;
    }

    const QDir project(navigator->projectDir());
    for (int i = 0; i < navigator->pageCount(); ++i) {
        navigator->ensureThumbnail(i);
    }

    /// Time for the queue, which works one page at a time on purpose.
    QTimer::singleShot(3000, this, [navigator, project]() {
        for (int i = 0; i < navigator->pageCount(); ++i) {
            const QFileInfo info(project.filePath(navigator->manifest().pages.at(i).thumbFile));
            say(QStringLiteral("thumbs: page %1 %2 (%3 bytes)")
                    .arg(i + 1)
                    .arg(info.exists() ? QStringLiteral("written") : QStringLiteral("MISSING"))
                    .arg(info.size()));
        }
    });
}

void PdfIoPlugin::runScaleProbe(int pages)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;

    say(QStringLiteral("scale: rss before %1 KB").arg(residentKb()));

    if (!navigator->openNotebook(qEnvironmentVariable("PDFIO_PROBE"), &why)) {
        say(QStringLiteral("scale: cannot open the notebook: %1").arg(why));
        return;
    }
    say(QStringLiteral("scale: page %1 rss %2 KB").arg(navigator->currentIndex() + 1).arg(residentKb()));

    /// Driven by a timer rather than a loop, and not for tidiness: closing a page defers the
    /// destruction of its view and document, so a tight loop frees nothing and the measurement
    /// would show growth that does not exist in use. A person also does not turn eight pages in
    /// the same millisecond.
    auto *timer = new QTimer(this);
    auto *turned = new int(1);

    connect(timer, &QTimer::timeout, this, [this, timer, turned, pages, navigator]() {
        QString why;
        if (*turned >= pages) {
            say(QStringLiteral("scale: done after %1 pages").arg(*turned));
            timer->stop();
            timer->deleteLater();
            delete turned;
            return;
        }

        if (!navigator->next(&why)) {
            say(QStringLiteral("scale: stopped after %1 pages: %2").arg(*turned).arg(why));
            timer->stop();
            timer->deleteLater();
            delete turned;
            return;
        }

        ++(*turned);
        say(QStringLiteral("scale: page %1 rss %2 KB").arg(navigator->currentIndex() + 1).arg(residentKb()));
    });

    timer->start(600);
}

#include "PdfIoPlugin.moc"
