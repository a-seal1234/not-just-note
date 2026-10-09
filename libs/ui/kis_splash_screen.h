/*
 *  SPDX-FileCopyrightText: 2014 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2021 Alvin Wong <alvin@alvinhc.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_SPLASH_SCREEN_H
#define KIS_SPLASH_SCREEN_H

#include <QWidget>
#include <QTimer>

#include "ui_wdgsplash.h"

class QPixmap;
#ifndef Q_OS_ANDROID
class QSvgWidget;
#endif

#include "kritaui_export.h"

class KRITAUI_EXPORT KisSplashScreen : public QWidget, public Ui::WdgSplash
{
    Q_OBJECT
public:
    struct Source {
        QString resourcePath;
        QString artistCredit;
    };

    explicit KisSplashScreen(bool themed = false, QWidget *parent = 0, Qt::WindowFlags f = Qt::WindowFlags());

    void repaint();

    void show();
    void displayLinks(bool show);
    void displayRecentFiles(bool show);

    void setLoadingText(QString text);

    static Source getImageSource();

private Q_SLOTS:

    void toggleShowAtStartup(bool toggle);
    void linkClicked(const QString &link);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateText();
    QString colorString() const;
    void updateSplashImage();

private:

    QTimer m_timer;
    bool m_themed;
    bool m_displayLinks { false };
#ifndef Q_OS_ANDROID
    QSvgWidget *m_brandingSvg;
    QSvgWidget *m_bannerSvg;
#endif
    QLabel *m_loadingTextLabel;
#ifndef Q_OS_ANDROID
    QLabel *m_artCreditsLabel;
#endif
    QString m_versionHtml;
};

#endif // KIS_SPLASH_SCREEN_H
