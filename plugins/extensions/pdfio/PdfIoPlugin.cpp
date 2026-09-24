/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "AndroidDocumentPicker.h"
#include "PdfIoDocker.h"
#include "PdfIoPlugin.h"
#include "PdfIoProbe.h"
#include "PdfPageNavigator.h"
#include "PdfRendererSpike.h"

#include <cstdio>
#include <unistd.h>

#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>

/// Not behind PDFIO_HAVE_POPPLER: the session, the saver, the ink loader and the exporter are all
/// plain C++ and are built on every platform. Only the renderer differs, and that is chosen by
/// PdfRenderBackend::create.
#include "session/PdfExporter.h"
#include "session/PdfInkLoader.h"
#include "session/PdfNotebookBundle.h"
#include "session/PdfPageSaver.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfStripBuilder.h"
#include "session/PdfStripLayout.h"
#include "session/PdfSession.h"

#include <QHash>
#include <QImage>

#include <KoDocumentInfo.h>

#include <kis_canvas2.h>
#include <kis_coordinates_converter.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <KisView.h>

#include <KoColor.h>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisViewManager.h>
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

} // namespace

PdfIoPlugin::PdfIoPlugin(QObject *parent, const QVariantList &)
    : KisActionPlugin(parent)
{
    registerActions();

    /// Once per process: a view plugin is created for every view.
    registerPdfIoDocker();

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

    /// Deferred on purpose. Opening a document touches the main window, and from the plugin
    /// constructor during startup that window is still being built: the welcome screen and the
    /// toolbar handler are not ready. The real action is triggered long after startup, so queueing
    /// the probe the same way is both the fix and a faithful stand-in.
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

        /// A plain action: the KisAction that carries the icon and the shortcut is the action
        /// manager's to make, and there is none yet. The registration that does have a view
        /// manager takes it off the menu again, so the entry is never doubled.
        if (!alone->findChild<QAction *>(QStringLiteral("pdfio_open_notebook_alone"))) {
            QAction *open = alone->addAction(i18n("Import PDF as notebook..."));
            open->setObjectName(QStringLiteral("pdfio_open_notebook_alone"));
            connect(open, &QAction::triggered, this, &PdfIoPlugin::slotOpenNotebook);
        }
        return;
    }

    struct Entry {
        const char *name;
        void (PdfIoPlugin::*slot)();
    };

    const Entry entries[] = {
        { "pdfio_open_notebook", &PdfIoPlugin::slotOpenNotebook },
        { "pdfio_open_bundle", &PdfIoPlugin::slotOpenNotebookBundle },
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

        /// And the first-screen stand-in goes before the real action is added: two entries that do
        /// the same thing is how a menu starts looking broken.
        if (QAction *standin = menu->findChild<QAction *>(QStringLiteral("pdfio_open_notebook_alone"))) {
            menu->removeAction(standin);
            standin->deleteLater();
        }
    }

    for (const Entry &entry : entries) {
        KisAction *action = viewManager()->actionManager()->createAction(QString::fromLatin1(entry.name));
        if (!action) {
            continue;
        }
        connect(action, &KisAction::triggered, this, entry.slot);

        /// Creating an action does not put it anywhere. Without this the plugin is invisible.
        if (menu) {
            menu->addAction(action);
        }
    }

    /// The strip switch. The strip existed behind PDFIO_PROBE_STRIP only, which nobody can set on
    /// a tablet; a checkable action is both the way in and the indicator of which mode is in force.
    KisAction *stripAction =
        viewManager()->actionManager()->createAction(QStringLiteral("pdfio_strip_mode"));
    if (stripAction) {
        stripAction->setCheckable(true);
        m_stripAction = stripAction;
        connect(stripAction, &KisAction::toggled, this, &PdfIoPlugin::slotToggleStripMode);
        if (menu) {
            menu->addSeparator();
            menu->addAction(stripAction);
        }
    }
    updateStripAction();

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
            menu->addSeparator();
            menu->addAction(followAction);
        }
    }
}

void PdfIoPlugin::updateStripAction()
{
    if (!m_stripAction) {
        return;
    }

    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    const bool strip = navigator->scope() > 1;

    /// The check mark is the mode indicator, and the text says what is on without a menu open.
    m_stripAction->setChecked(strip);
    m_stripAction->setText(strip ? i18n("Five-page strip (active ±2) is on")
                                 : i18n("Five-page strip (active ±2)"));
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
    /// Five, so the active page has two pages on each side of it. The window rolls -- repainting
    /// only the slots that leave -- when the active page reaches that edge.
    const int wanted = (m_stripAction && m_stripAction->isChecked()) ? 5 : 1;

    navigator->setScope(wanted);
    updateStripAction();

    if (!navigator->hasNotebook()) {
        say(QStringLiteral("strip: mode set to %1 page(s); it applies when a notebook is opened")
                .arg(wanted));
        return;
    }

    /// What is on screen is written before the document that holds it is torn down.
    navigator->saveStripPages();

    const int page = navigator->currentIndex();
    say(QStringLiteral("strip: switching to %1, keeping page %2 open")
            .arg(wanted > 1 ? QStringLiteral("a five-page strip") : QStringLiteral("one page"))
            .arg(page + 1));

    QTimer::singleShot(0, this, [this, page]() { rebuildForScope(page, 6); });
}

void PdfIoPlugin::slotOpenNotebook()
{
#if defined(Q_OS_ANDROID)
    /// QFileDialog is not usable here: the application has no broad filesystem access, so the
    /// file arrives as a content URI and has to be copied to a path the renderer can open.
    auto *picker = new AndroidDocumentPicker(this);
    say(QStringLiteral("the document picker is opening"));
    picker->pickPdf([this, picker](const QString &localPath, const QString &why) {
        picker->deleteLater();
        say(QStringLiteral("picker finished: path \"%1\" reason \"%2\"").arg(localPath, why));

        if (localPath.isEmpty()) {
            say(QStringLiteral("nothing was opened: %1").arg(why));
            return;
        }

        /// Deferred out of the activity result callback. Opening a document builds a view and
        /// walks the resource system, and doing that while the activity transition is still
        /// unwinding crashed inside Qt's own hash tables.
        QTimer::singleShot(0, this, [this, localPath]() {
            QString error;
            if (!PdfPageNavigator::instance()->openNotebook(localPath, &error)) {
                say(QStringLiteral("could not open the chosen file: %1").arg(error));
            }
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

    if (!PdfPageNavigator::instance()->openNotebook(path, nullptr)) {
        qWarning() << "pdfio could not open" << path;
    }
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
    const QString base = navigator->hasNotebook()
        ? QFileInfo(navigator->manifest().sourceFile).completeBaseName()
        : QStringLiteral("notebook");
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
        QString why;
        if (!PdfPageNavigator::instance()->openNotebook(source, &why)) {
            say(QStringLiteral("the unpacked notebook could not be opened: %1").arg(why));
            return;
        }

        /// Opening goes through the navigator, which keys a project by the source's own hash. It
        /// finds the directory just written only while its root is the one assumed here, so a drift
        /// between the two is said out loud instead of leaving an empty notebook and no reason.
        if (QFileInfo(PdfPageNavigator::instance()->projectDir()).absoluteFilePath()
            != QFileInfo(destination).absoluteFilePath()) {
            say(QStringLiteral("WARNING: the notebook opened from %1, not from %2: the project root "
                               "assumed by PdfNotebookBundle::defaultProjectRoot() no longer matches "
                               "the navigator's, and the ink that came in the file was not used")
                    .arg(PdfPageNavigator::instance()->projectDir(), destination));
        }
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
                                                        QStringLiteral("notebook.pdf"),
                                                        i18n("PDF documents (*.pdf)"));
    if (target.isEmpty()) {
        return;
    }
#endif

    /// One image per page that was ever drawn on. The ink is read straight out of the saved
    /// artifacts, so exporting does not have to open a document per page.
    QHash<int, QImage> ink;
    const QDir project(navigator->projectDir());
    for (const PdfPageRecord &page : navigator->manifest().pages) {
        const QImage pageInk = PdfInkLoader::loadInk(project.filePath(page.kraFile), nullptr);
        if (!pageInk.isNull()) {
            ink.insert(page.index, pageInk);
        }
    }

    QString why;
    if (!PdfExporter::exportWithInk(navigator->sourcePath(), navigator->manifest(),
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
    const QString suggested = QStringLiteral("%1-notes.pdf")
                                  .arg(QFileInfo(navigator->manifest().sourceFile).completeBaseName());
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

    QString why;
    KisDocument *pageDocument = PdfPageSaver::createPageLayersDocument(document->image(), &why);
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
    }
    return opened;
}

void PdfIoPlugin::runRestoreProbe()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;

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
    const PdfStripLayout layout =
        PdfStripLayout::forWindow(navigator->manifest(), first, 3, 200.0);
    const int slot = layout.slotForPage(first);
    if (!layout.isValid() || slot < 0) {
        say(QStringLiteral("strip: the layout does not hold that page"));
        return;
    }
    const QRect area = layout.slots().at(slot).rect;

    KisPaintLayer *stroke = nullptr;
    const QString groupName = PdfStripBuilder::inkGroupName(first);
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        KisNodeSP child = image->root()->at(i);
        /// The strip holds ONE content layer, "Ink" (PdfStripBuilder::build), and a page is the
        /// part of it inside that page's rectangle. This used to look for a group per page, a
        /// design the strip no longer has: the probe answered "no Ink layer for page 1" and never
        /// reached the turn it exists to exercise.
        if (child->name() == QStringLiteral("Ink")) {
            stroke = qobject_cast<KisPaintLayer *>(child.data());
            break;
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
