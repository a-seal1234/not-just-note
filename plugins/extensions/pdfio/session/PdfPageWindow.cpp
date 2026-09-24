/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageWindow.h"

#include <QtGlobal>

#include <algorithm>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

PdfPageWindow::PdfPageWindow(int capacity)
    : m_capacity(qMax(1, capacity))
{
}

int PdfPageWindow::capacity() const
{
    return m_capacity;
}

void PdfPageWindow::setCapacity(int capacity)
{
    m_capacity = qMax(1, capacity);
}

void PdfPageWindow::setSaver(SaveFn saver)
{
    m_saver = std::move(saver);
}

bool PdfPageWindow::open(int index, QString *why)
{
    if (index < 0) {
        fail(why, QStringLiteral("page %1 is not a page").arg(index));
        return false;
    }

    /// Already open: the only thing opening it again changes is when it was last used.
    if (m_open.contains(index)) {
        m_open.removeAll(index);
        m_open.append(index);
        return true;
    }

    /// A while loop, not an if: a window that was shrunk below what it holds has to give back
    /// several slots, and the policy should not be able to end an open() call still over its bound.
    while (m_open.size() >= m_capacity) {
        /// A clean page costs nothing to drop: it is on disk, and reopening it re-renders it from
        /// the source the same way it was rendered the first time.
        int victim = -1;
        for (int page : m_open) {
            if (!m_dirty.contains(page)) {
                victim = page;
                break;
            }
        }

        if (victim < 0) {
            /// Every slot holds ink that is on disk nowhere. Save the least recently used one and
            /// let that save be what makes room. Only that page is tried: a save that failed has
            /// failed for a reason (the disk, the destination) that the next page shares, and
            /// trying the rest would turn one refused eviction into a run of refused saves.
            const int candidate = m_open.first();
            QString saveError;
            if (m_saver && m_saver(candidate, &saveError)) {
                m_dirty.remove(candidate);
                ++m_savedBeforeEviction;
                victim = candidate;
            } else {
                ++m_blockedEvictions;
                fail(why,
                     QStringLiteral("page %1 cannot be closed to open page %2: saving it failed (%3). "
                                    "The page stays open and nothing was discarded.")
                         .arg(candidate + 1)
                         .arg(index + 1)
                         .arg(saveError.isEmpty() ? QStringLiteral("no saver is installed") : saveError));
                return false;
            }
        }

        m_open.removeAll(victim);
        m_dirty.remove(victim);
        ++m_evictions;
    }

    m_open.append(index);
    return true;
}

void PdfPageWindow::setDirty(int index, bool dirty)
{
    if (dirty) {
        m_dirty.insert(index);
    } else {
        m_dirty.remove(index);
    }
}

bool PdfPageWindow::isDirty(int index) const
{
    return m_dirty.contains(index);
}

QList<int> PdfPageWindow::dirtyPages() const
{
    /// Sorted: the mark set is a QSet, and an order that changes with every insertion would make
    /// the sequence of idle writes depend on nothing anyone chose.
    QList<int> pages = m_dirty.values();
    std::sort(pages.begin(), pages.end());
    return pages;
}

bool PdfPageWindow::isOpen(int index) const
{
    return m_open.contains(index);
}

int PdfPageWindow::openCount() const
{
    return m_open.size();
}

QList<int> PdfPageWindow::openPages() const
{
    return m_open;
}

void PdfPageWindow::clear()
{
    m_open.clear();
    m_dirty.clear();
    m_evictions = 0;
    m_savedBeforeEviction = 0;
    m_blockedEvictions = 0;
}

int PdfPageWindow::evictionCount() const
{
    return m_evictions;
}

int PdfPageWindow::savedBeforeEvictionCount() const
{
    return m_savedBeforeEviction;
}

int PdfPageWindow::blockedEvictionCount() const
{
    return m_blockedEvictions;
}
