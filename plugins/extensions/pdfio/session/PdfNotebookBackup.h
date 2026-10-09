/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFNOTEBOOKBACKUP_H
#define PDFNOTEBOOKBACKUP_H

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * Daily, verified snapshots of a notebook and a conservative stale-artifact collector.
 *
 * Snapshots use the existing .pnb format and live outside the notebook directory. The collector
 * only considers generated page, thumbnail and source files; its caller presents the complete
 * candidate list before asking this class to remove anything.
 */
class PdfNotebookBackup
{
public:
    struct Snapshot {
        QString path;
        QString displayName;
        QDateTime modified;
        qint64 bytes = 0;
    };

    /// Creates today's one scheduled snapshot, or returns the already verified one for today.
    static bool createDailyBackup(const QString &projectDir,
                                  QString *backupPath = nullptr,
                                  bool *created = nullptr,
                                  QString *why = nullptr);

    /// Creates an additional verified safety snapshot, used before manual artifact collection.
    static bool createSafetyBackup(const QString &projectDir,
                                   QString *backupPath = nullptr,
                                   QString *why = nullptr);

    /// Reuses the latest verified snapshot if it still matches the saved notebook state.
    static bool ensureFreshSafetyBackup(const QString &projectDir,
                                        QString *backupPath = nullptr,
                                        QString *why = nullptr);

    /// The valid retained snapshots for one notebook, newest first.
    static QList<Snapshot> availableBackups(const QString &projectDir);

    /// Lists orphan candidates without changing the notebook. Fails closed on uncertain undo data.
    static bool findOrphanedArtifacts(const QString &projectDir,
                                      QStringList *candidates,
                                      QString *why = nullptr);

    /// Rechecks reachability and the confirmed candidate list, then removes those files only.
    static bool removeOrphanedArtifacts(const QString &projectDir,
                                        const QStringList &confirmedCandidates,
                                        QStringList *removed,
                                        QString *why = nullptr);

    /// Extracts a retained snapshot to a unique new notebook directory, never replacing one.
    static bool restoreAsNewNotebook(const QString &backupPath,
                                     QString *newProjectDir,
                                     QString *why = nullptr);

private:
    PdfNotebookBackup() = delete;
};

#endif // PDFNOTEBOOKBACKUP_H
