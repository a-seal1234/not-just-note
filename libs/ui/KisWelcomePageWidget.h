/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2018 Scott Petrovic <scottpetrovic@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef KISWELCOMEPAGEWIDGET_H
#define KISWELCOMEPAGEWIDGET_H

#include "kritaui_export.h"
#include "KisViewManager.h"
#include <KisUpdaterBase.h>
#include <KisKineticScroller.h>

#include <QAction>
#include <QWidget>
#include "ui_KisWelcomePage.h"
#include <QStandardItemModel>
#include <QScopedPointer>
#include <QFont>

#include <functional>

#include "config-updaters.h"
class RecentItemDelegate;
class KisMainWindow;
class ExtraRecentEntriesProxy;

// Custom QAction to bridge a QLabel::linkActivated signal to a QAction::setChecked signal
class ShowNewsAction : public QAction
{
  Q_OBJECT
public:
    using QAction::QAction;
private Q_SLOTS:
    void enableFromLink(QString unused_url);
};

/// A widget for displaying if no documents are open. This will display in the MDI area
class KRITAUI_EXPORT KisWelcomePageWidget : public QWidget, public Ui::KisWelcomePage
{
    Q_OBJECT

    public:
    explicit KisWelcomePageWidget(QWidget *parent);
    ~KisWelcomePageWidget() override;

    void setMainWindow(KisMainWindow* m_mainWindow);

    /**
     * An extra entry for the Recent Images list: a notebook, in practice, shown beside Krita's own
     * recent documents.
     *
     * Core UI knows nothing about what an entry is. \a token is opaque and is handed back to the
     * activation callback when the entry is clicked; \a thumbnailPath may be empty, in which case a
     * plain icon stands in.
     */
    struct ExtraRecentEntry {
        QString name;
        QString thumbnailPath;
        QString token;
    };

    /**
     * Replaces the extra entries every welcome page shows, and the callback an entry's click calls.
     *
     * Called by the pdfio plugin; with an empty list the extra rows go away again. Krita's own
     * recent documents are never touched: the entries are appended beside them by a proxy this
     * widget owns, and an extra row has no source index at all.
     */
    static void setExtraRecentEntries(const QList<ExtraRecentEntry> &entries,
                                      std::function<void(const QString &token)> activate);

    /**
     * Registers the two notebook buttons beside New Image and Open Image.
     *
     * Both stay hidden until handlers are set, so a core UI without the pdfio plugin looks exactly
     * as it did. Passing empty functions hides them again.
     */
    static void setNotebookActions(std::function<void()> importNotebook,
                                   std::function<void()> openNotebookFile);

public Q_SLOTS:
    /// if a document is placed over this area, a dotted line will appear as an indicator
    /// that it is a droppable area. KisMainwindow is what triggers this
    void showDropAreaIndicator(bool show);

    void slotUpdateThemeColors();

#ifdef ENABLE_UPDATERS
    void slotSetUpdateStatus(KisUpdaterStatus updateStatus);
    void slotShowUpdaterErrorDetails();
#endif

private Q_SLOTS:
    void slotNewFileClicked();
    void slotOpenFileClicked();
    void slotPaste();

    void recentDocumentClicked(QModelIndex index);
    void slotRecentDocContextMenuRequest(const QPoint &pos);
    void slotImportNotebookClicked();
    void slotOpenNotebookClicked();

    /**
     * Once all files in the recent documents model are checked, cleanup the UI if the model is empty
     */
    void slotRecentFilesModelIsUpToDate();

    void slotScrollerStateChanged(QScroller::State state){ KisKineticScroller::updateCursor(this, state); }

#ifdef ENABLE_UPDATERS
    void slotRunVersionUpdate();
    void slotToggleUpdateChecks(bool state);
#endif

    bool isDevelopmentBuild();

    QFont largerFont();

protected:

    // QWidget overrides
    void dragEnterEvent(QDragEnterEvent * event) override;
    void dropEvent(QDropEvent * event) override;
    void dragMoveEvent(QDragMoveEvent * event) override;
    void dragLeaveEvent(QDragLeaveEvent * event) override;
    void changeEvent(QEvent *event) override;

    bool eventFilter(QObject *watched, QEvent *event) override;


private:
    /// Rebuilds the extra rows from the static registry above.
    void refreshExtraRecentEntries();

    /// Shows and enables the notebook buttons from the static registry above.
    void updateNotebookButtons();

    void setupNewsLangSelection(QMenu *newsOptionMenu);
    void showDevVersionHighlight();

    static void updateShortcutLink(QToolButton *button, QLabel *label, QAction *action);

#ifdef ENABLE_UPDATERS
    void updateVersionUpdaterFrame();
#endif


    KisMainWindow *m_mainWindow {nullptr};

    /// help us see how many people are clicking startup screen links
    /// you can see the results in Matomo (stats.kde.org)
    /// this will be listed in the "Acquisition" section of Matomo
    /// just append some text to this to associate it with an event/page
    const QString analyticsString = "pk_campaign=startup-sceen&pk_kwd=";


    // keeping track of link colors with theme change
    QColor textColor;
    QColor backgroundColor;
    QColor blendedColor;
    QString blendedStyle;

#ifdef ENABLE_UPDATERS
    QScopedPointer<KisUpdaterBase> m_versionUpdater;
    KisUpdaterStatus m_updaterStatus;
#endif
    bool m_networkIsAllowed {false};

    QScopedPointer<RecentItemDelegate> recentItemDelegate;

    /// Krita's recent-documents model with the extra entries appended; owned by this widget.
    ExtraRecentEntriesProxy *m_extraProxy {nullptr};

    /// Every live welcome page, so a static registration can reach them all.
    static QList<KisWelcomePageWidget *> s_instances;
    static QList<ExtraRecentEntry> s_extraEntries;
    static std::function<void(const QString &token)> s_activateExtra;
    static std::function<void()> s_importNotebook;
    static std::function<void()> s_openNotebook;
};

#endif // KISWELCOMEPAGEWIDGET_H
