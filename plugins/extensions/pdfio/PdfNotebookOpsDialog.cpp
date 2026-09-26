/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "PdfNotebookOpsDialog.h"

#include "session/PdfSession.h"

#include <KLocalizedString>

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <functional>

namespace {

/// The columns the table shows, in order.
enum Column {
    ThumbnailColumn,
    PositionColumn,
    SourceColumn,
    SizeColumn,
    TurnColumn,
    InkColumn,
    ColumnCount,
};

/// Where a page comes from, in the words someone reading the list needs: the PDF's name and the page
/// inside it. That is what the manifest records, and the design asks for it to be visible.
QString sourceLabel(const PdfSessionManifest &manifest, const PdfPageRecord &record)
{
    const QString file = record.source > 0 && record.source < manifest.sourceCount()
                             ? QFileInfo(manifest.sourceAt(record.source).file).fileName()
                             : QFileInfo(manifest.sourceFile).fileName();
    return i18n("%1, page %2", file, record.index + 1);
}

QString sizeLabel(const PdfPageRecord &record)
{
    const QSizeF size = record.displaySizePt();
    return QStringLiteral("%1 x %2 pt")
        .arg(QString::number(size.width(), 'f', 0), QString::number(size.height(), 'f', 0));
}

QString turnLabel(int degrees)
{
    switch (((degrees % 360) + 360) % 360) {
    case 90:
        return i18n("turned right");
    case 180:
        return i18n("upside down");
    case 270:
        return i18n("turned left");
    default:
        return i18n("as it is");
    }
}

} // namespace

PdfNotebookOpsDialog::PdfNotebookOpsDialog(const QString &projectDir,
                                           const PdfSessionManifest &manifest, int anchorPage,
                                           QWidget *parent)
    : QDialog(parent)
    , m_projectDir(projectDir)
    , m_original(manifest)
    , m_anchor(anchorPage)
    , m_nextNumber(PdfNotebookOps::nextFreePageNumber(projectDir))
{
    setWindowTitle(i18n("Notebook ops"));

    for (const PdfPageRecord &page : manifest.pages) {
        Row row;
        row.record = page;
        row.fromDir = projectDir;
        m_rows.append(row);
    }

    buildUi();
    refresh();
}

void PdfNotebookOpsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(
        i18n("Notebook \"%1\" has %2 page(s). Change the list here; nothing is written until you "
             "press Apply, and then the whole change is one step you can undo.",
             m_original.displayName(), m_rows.size()),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *middle = new QHBoxLayout;

    m_table = new QTableWidget(m_rows.size(), ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ i18n("Page"), i18n("#"), i18n("From"), i18n("Size"),
                                         i18n("Turn"), i18n("Ink") });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setIconSize(QSize(46, 62));
    m_table->verticalHeader()->setDefaultSectionSize(70);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setMinimumWidth(560);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() { refreshFooter(); });
    middle->addWidget(m_table, 1);

    auto *buttons = new QVBoxLayout;
    const auto addButton = [this, buttons](const QString &text, const std::function<void()> &action) {
        auto *button = new QPushButton(text, this);
        connect(button, &QPushButton::clicked, this, action);
        buttons->addWidget(button);
        return button;
    };
    /// Every control is named, so a test -- and a screenshot probe looking for the screen -- can
    /// reach it without reading pixel positions.
    m_moveUp = addButton(i18n("Move up"), [this]() { moveSelected(-1); });
    m_moveUp->setObjectName(QStringLiteral("pdfio_ops_move_up"));
    m_moveDown = addButton(i18n("Move down"), [this]() { moveSelected(1); });
    m_moveDown->setObjectName(QStringLiteral("pdfio_ops_move_down"));
    buttons->addSpacing(10);
    m_duplicate = addButton(i18n("Duplicate"), [this]() { duplicateSelected(); });
    m_duplicate->setObjectName(QStringLiteral("pdfio_ops_duplicate"));
    m_delete = addButton(i18n("Delete page"), [this]() { deleteSelected(); });
    m_delete->setObjectName(QStringLiteral("pdfio_ops_delete_page"));
    m_keep = addButton(i18n("Keep page"), [this]() { keepSelected(); });
    m_keep->setObjectName(QStringLiteral("pdfio_ops_keep_page"));
    buttons->addSpacing(10);
    m_rotateLeft = addButton(i18n("Turn left"), [this]() { rotateSelected(-90); });
    m_rotateLeft->setObjectName(QStringLiteral("pdfio_ops_turn_left"));
    m_rotateRight = addButton(i18n("Turn right"), [this]() { rotateSelected(90); });
    m_rotateRight->setObjectName(QStringLiteral("pdfio_ops_turn_right"));
    buttons->addSpacing(18);

    /// The whole-notebook actions live here too, so one screen covers what a person can do to a
    /// notebook. They are available while the page list is untouched, and the hint says why when
    /// they are not.
    const auto addAction = [this, buttons](const QString &text, RequestedAction action) {
        auto *button = new QPushButton(text, this);
        connect(button, &QPushButton::clicked, this, [this, action]() {
            m_requested = action;
            accept();
        });
        buttons->addWidget(button);
        return button;
    };
    m_insert = addAction(i18n("Insert pages from a PDF..."), InsertPagesAction);
    m_insert->setObjectName(QStringLiteral("pdfio_ops_insert"));
    m_extract = addAction(i18n("Extract a page range..."), ExtractRangeAction);
    m_extract->setObjectName(QStringLiteral("pdfio_ops_extract"));
    m_merge = addAction(i18n("Merge a notebook in..."), MergeNotebookAction);
    m_merge->setObjectName(QStringLiteral("pdfio_ops_merge_notebook"));

    buttons->addStretch(1);
    middle->addLayout(buttons);
    layout->addLayout(middle, 1);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("pdfio_ops_summary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("pdfio_ops_hint"));
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_apply = box->addButton(i18n("Apply"), QDialogButtonBox::AcceptRole);
    m_apply->setObjectName(QStringLiteral("pdfio_ops_apply"));
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(box);

    resize(920, 640);
}

void PdfNotebookOpsDialog::refresh()
{
    const int keepSelection = m_table->currentRow();

    m_table->setRowCount(m_rows.size());
    int position = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        if (!row.removed) {
            ++position;
        }

        auto *thumbnail = new QTableWidgetItem;
        const QString path = QDir(row.fromDir).filePath(row.record.thumbFile);
        if (!row.record.thumbFile.isEmpty() && QFileInfo::exists(path)) {
            thumbnail->setIcon(QIcon(QPixmap(path)));
        }
        m_table->setItem(i, ThumbnailColumn, thumbnail);

        const auto cell = [this, i, &row](Column column, const QString &text) {
            auto *item = new QTableWidgetItem(text);
            if (row.removed) {
                /// Struck through rather than taken away: a page marked for deletion stays visible
                /// until Apply, so a mistake is one click from being undone.
                QFont font = item->font();
                font.setStrikeOut(true);
                item->setFont(font);
            }
            m_table->setItem(i, column, item);
        };
        cell(PositionColumn, row.removed ? i18n("(gone)") : QString::number(position));
        cell(SourceColumn, sourceLabel(m_original, row.record));
        cell(SizeColumn, sizeLabel(row.record));
        cell(TurnColumn, turnLabel(row.record.extraRotation));

        /// The ink column answers "has this page ever been drawn on": the artifact is the ink, and a
        /// page that was never drawn on has none.
        const bool inked = QFileInfo::exists(QDir(row.fromDir).filePath(row.record.kraFile));
        cell(InkColumn, row.isNew ? i18n("new copy") : (inked ? i18n("drawn on") : i18n("blank")));
    }

    if (keepSelection >= 0 && keepSelection < m_rows.size()) {
        m_table->selectRow(keepSelection);
    } else if (!m_rows.isEmpty()) {
        m_table->selectRow(0);
    }

    refreshFooter();
}

void PdfNotebookOpsDialog::refreshFooter()
{
    const QStringList pending = pendingDescriptions();
    const bool dirty = hasPendingEdits();

    /// Apply is off while nothing has changed: a no-op would still close and reopen the notebook,
    /// and a change here is meant to be a deliberate single act.
    m_apply->setEnabled(dirty);
    m_summary->setText(dirty
                           ? i18n("%1 change(s) pending: %2", pending.size(),
                                  pending.join(i18n(", ")))
                           : i18n("No changes yet: the notebook is as it is."));

    const int row = m_table->currentRow();
    const bool selected = row >= 0 && row < m_rows.size();
    const bool live = selected && !m_rows.at(row).removed;

    m_moveUp->setEnabled(live && keptAbove(row));
    m_moveDown->setEnabled(live && keptBelow(row));
    m_duplicate->setEnabled(live);
    m_delete->setEnabled(live && keptCount() > 1);
    m_keep->setEnabled(selected && m_rows.at(row).removed);
    m_rotateLeft->setEnabled(live);
    m_rotateRight->setEnabled(live);

    /// A notebook cannot be half-edited and merged into at the same time: these wait for Apply or
    /// Cancel, and the hint says so.
    const bool wholeNotebook = !dirty;
    m_insert->setEnabled(wholeNotebook);
    m_extract->setEnabled(wholeNotebook);
    m_merge->setEnabled(wholeNotebook);

    m_hint->setText(availabilityHint());
}

void PdfNotebookOpsDialog::moveSelected(int delta)
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }

    /// The nearest page that is still kept, in the direction asked for: a page marked for deletion
    /// is not somewhere a page can be moved to.
    int target = -1;
    for (int i = row + delta; i >= 0 && i < m_rows.size(); i += delta) {
        if (!m_rows.at(i).removed) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        return;
    }

    m_rows.swapItemsAt(row, target);
    refresh();
    m_table->selectRow(target);
}

void PdfNotebookOpsDialog::duplicateSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }

    const Row source = m_rows.at(row);
    Row copy;
    copy.record = source.record;
    copy.record.generation = 0;
    copy.fromDir = m_projectDir;
    copy.isNew = true;
    /// The allocator is asked once, at the start, and each new page takes the next number: the
    /// engine refuses a destination the notebook already names, so a number is never guessed.
    const int number = m_nextNumber++;
    copy.record.kraFile = PdfSession::pageFileNameForNumber(number);
    copy.record.thumbFile = PdfSession::thumbFileNameForNumber(number);

    /// The files come from where the page is now, which may itself be a copy made a moment ago: the
    /// engine copies the list in order, so a chain of duplicates resolves.
    m_copies.append(qMakePair(QDir(source.fromDir).filePath(source.record.kraFile),
                              copy.record.kraFile));
    if (!source.record.thumbFile.isEmpty()) {
        m_copies.append(qMakePair(QDir(source.fromDir).filePath(source.record.thumbFile),
                                  copy.record.thumbFile));
    }
    m_copyDirs.append(qMakePair(QDir(source.fromDir).filePath(source.record.kraFile
                                                             + QStringLiteral(".layers")),
                                copy.record.kraFile + QStringLiteral(".layers")));

    m_rows.insert(row + 1, copy);
    refresh();
    m_table->selectRow(row + 1);
}

void PdfNotebookOpsDialog::deleteSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }
    if (keptCount() <= 1) {
        return; /// the button is off, and the hint says why
    }

    if (m_rows.at(row).isNew) {
        /// It never existed: dropping it drops the files it would have brought, and there is
        /// nothing for the journal to take over.
        const Row dropped = m_rows.takeAt(row);
        const auto forget = [&dropped](QList<QPair<QString, QString>> *list) {
            for (int i = list->size() - 1; i >= 0; --i) {
                const QString destination = list->at(i).second;
                if (destination == dropped.record.kraFile
                    || destination == dropped.record.kraFile + QStringLiteral(".layers")
                    || (!dropped.record.thumbFile.isEmpty()
                        && destination == dropped.record.thumbFile)) {
                    list->removeAt(i);
                }
            }
        };
        forget(&m_copies);
        forget(&m_copyDirs);
    } else {
        Row &gone = m_rows[row];
        gone.removed = true;
        m_removals << gone.record.kraFile;
        if (!gone.record.thumbFile.isEmpty()) {
            m_removals << gone.record.thumbFile;
        }
    }

    refresh();
    m_table->selectRow(qBound(0, row, m_rows.size() - 1));
}

void PdfNotebookOpsDialog::keepSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || !m_rows.at(row).removed) {
        return;
    }

    Row &back = m_rows[row];
    back.removed = false;
    m_removals.removeAll(back.record.kraFile);
    if (!back.record.thumbFile.isEmpty()) {
        m_removals.removeAll(back.record.thumbFile);
    }

    refresh();
    m_table->selectRow(row);
}

void PdfNotebookOpsDialog::rotateSelected(int degrees)
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }

    Row &turning = m_rows[row];
    turning.record.extraRotation = ((turning.record.extraRotation + degrees) % 360 + 360) % 360;
    /// The preview was made from the page before this turn, so it goes with the change: a stale
    /// preview would show the page the wrong way up, and the next save makes a new one.
    dropThumbnail(turning);

    refresh();
    m_table->selectRow(row);
}

void PdfNotebookOpsDialog::dropThumbnail(Row &row)
{
    if (row.record.thumbFile.isEmpty()) {
        return;
    }

    const QString thumb = row.record.thumbFile;
    for (int i = m_copies.size() - 1; i >= 0; --i) {
        if (m_copies.at(i).second == thumb) {
            m_copies.removeAt(i);
        }
    }
    if (!row.isNew && !m_removals.contains(thumb)) {
        m_removals << thumb;
    }
    row.record.thumbFile.clear();
}

bool PdfNotebookOpsDialog::hasPendingEdits() const
{
    if (!m_copies.isEmpty() || !m_copyDirs.isEmpty() || !m_removals.isEmpty()) {
        return true;
    }

    QList<PdfPageRecord> now;
    for (const Row &row : m_rows) {
        if (!row.removed) {
            now.append(row.record);
        }
    }
    if (now.size() != m_original.pages.size()) {
        return true;
    }
    for (int i = 0; i < now.size(); ++i) {
        const PdfPageRecord &a = now.at(i);
        const PdfPageRecord &b = m_original.pages.at(i);
        if (a.kraFile != b.kraFile || a.source != b.source || a.index != b.index
            || a.extraRotation != b.extraRotation || a.sizePt != b.sizePt
            || a.thumbFile != b.thumbFile) {
            return true;
        }
    }
    return false;
}

QStringList PdfNotebookOpsDialog::pendingDescriptions() const
{
    QStringList parts;

    int deleted = 0;
    int duplicated = 0;
    for (const Row &row : m_rows) {
        if (row.removed) {
            ++deleted;
        } else if (row.isNew) {
            ++duplicated;
        }
    }
    if (deleted > 0) {
        parts << i18np("%1 page deleted", "%1 pages deleted", deleted);
    }
    if (duplicated > 0) {
        parts << i18np("%1 page duplicated", "%1 pages duplicated", duplicated);
    }

    QHash<QString, int> turnBefore;
    for (const PdfPageRecord &page : m_original.pages) {
        turnBefore.insert(page.kraFile, page.extraRotation);
    }
    int turned = 0;
    for (const Row &row : m_rows) {
        if (row.removed || row.isNew) {
            continue;
        }
        if (turnBefore.value(row.record.kraFile, row.record.extraRotation)
            != row.record.extraRotation) {
            ++turned;
        }
    }
    if (turned > 0) {
        parts << i18np("%1 page turned", "%1 pages turned", turned);
    }

    /// The order: the pages that were here, in the order they are in now, against the order they
    /// had. A new page's place is not a move of anything, so it is left out of the comparison.
    QStringList were, are;
    for (const PdfPageRecord &page : m_original.pages) {
        were << page.kraFile;
    }
    for (const Row &row : m_rows) {
        if (!row.removed && !row.isNew) {
            are << row.record.kraFile;
        }
    }
    QStringList stillHere;
    for (const QString &name : were) {
        if (are.contains(name)) {
            stillHere << name;
        }
    }
    if (!are.isEmpty() && are != stillHere) {
        parts << i18n("the order changed");
    }

    if (parts.isEmpty() && hasPendingEdits()) {
        parts << i18n("the page list changed");
    }
    return parts;
}

QString PdfNotebookOpsDialog::availabilityHint() const
{
    if (m_rows.isEmpty()) {
        return i18n("There are no pages to list.");
    }
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size()) {
        return i18n("Select a page to move, duplicate, turn or delete it.");
    }
    if (m_rows.at(row).removed) {
        return i18n("This page is marked for deletion. \"Keep page\" brings it back; Cancel "
                    "leaves the notebook alone.");
    }

    QStringList notes;
    if (!hasPendingEdits()) {
        notes << i18n("Apply is off because nothing has changed yet.");
    } else {
        notes << i18n("Insert, extract and merge are off until this change is applied or "
                      "cancelled: a notebook is not two changes at once.");
    }
    if (keptCount() <= 1) {
        notes << i18n("A notebook keeps at least one page, so this page cannot be deleted.");
    }
    if (!keptAbove(row)) {
        notes << i18n("This is the first page, so it cannot move up.");
    }
    if (!keptBelow(row)) {
        notes << i18n("This is the last page, so it cannot move down.");
    }
    if (notes.isEmpty()) {
        return i18n("The whole change is applied in one step, and one undo takes it back.");
    }
    return notes.join(QStringLiteral(" "));
}

QString PdfNotebookOpsDialog::summaryLine() const
{
    const QStringList parts = pendingDescriptions();
    if (parts.isEmpty()) {
        return i18n("the notebook's pages were changed");
    }
    return i18n("the notebook's pages were changed: %1", parts.join(i18n(", ")));
}

PdfNotebookOps::PageEdits PdfNotebookOpsDialog::edits() const
{
    PdfNotebookOps::PageEdits edits;
    edits.sources = m_original.sources;
    for (const Row &row : m_rows) {
        if (!row.removed) {
            edits.pages.append(row.record);
        }
    }
    edits.copyExternal = m_copies;
    edits.copyExternalDirs = m_copyDirs;
    edits.removeAfter = m_removals;
    edits.summary = summaryLine();
    return edits;
}

int PdfNotebookOpsDialog::keptCount() const
{
    int kept = 0;
    for (const Row &row : m_rows) {
        if (!row.removed) {
            ++kept;
        }
    }
    return kept;
}

bool PdfNotebookOpsDialog::keptAbove(int row) const
{
    for (int i = row - 1; i >= 0; --i) {
        if (!m_rows.at(i).removed) {
            return true;
        }
    }
    return false;
}

bool PdfNotebookOpsDialog::keptBelow(int row) const
{
    for (int i = row + 1; i < m_rows.size(); ++i) {
        if (!m_rows.at(i).removed) {
            return true;
        }
    }
    return false;
}
