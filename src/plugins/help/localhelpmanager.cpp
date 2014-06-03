/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "localhelpmanager.h"

#include "bookmarkmanager.h"
#include "helpconstants.h"

#include <app/app_version.h>
#include <coreplugin/helpmanager.h>

#include <QMutexLocker>

#include <QHelpEngine>

using namespace Help::Internal;

static LocalHelpManager *m_instance = 0;

QMutex LocalHelpManager::m_guiMutex;
QHelpEngine* LocalHelpManager::m_guiEngine = 0;

QMutex LocalHelpManager::m_bkmarkMutex;
BookmarkManager* LocalHelpManager::m_bookmarkManager = 0;

LocalHelpManager::LocalHelpManager(QObject *parent)
    : QObject(parent)
    , m_guiNeedsSetup(true)
    , m_needsCollectionFile(true)
{
    m_instance = this;
}

LocalHelpManager::~LocalHelpManager()
{
    if (m_bookmarkManager) {
        m_bookmarkManager->saveBookmarks();
        delete m_bookmarkManager;
        m_bookmarkManager = 0;
    }

    delete m_guiEngine;
    m_guiEngine = 0;
}

LocalHelpManager *LocalHelpManager::instance()
{
    return m_instance;
}

void LocalHelpManager::setupGuiHelpEngine()
{
    if (m_needsCollectionFile) {
        m_needsCollectionFile = false;
        helpEngine().setCollectionFile(Core::HelpManager::collectionFilePath());
    }

    if (m_guiNeedsSetup) {
        m_guiNeedsSetup = false;
        helpEngine().setupData();
    }
}

void LocalHelpManager::setEngineNeedsUpdate()
{
    m_guiNeedsSetup = true;
}

QHelpEngine &LocalHelpManager::helpEngine()
{
    if (!m_guiEngine) {
        QMutexLocker _(&m_guiMutex);
        if (!m_guiEngine) {
            m_guiEngine = new QHelpEngine(QString());
            m_guiEngine->setAutoSaveFilter(false);
        }
    }
    return *m_guiEngine;
}

BookmarkManager& LocalHelpManager::bookmarkManager()
{
    if (!m_bookmarkManager) {
        QMutexLocker _(&m_bkmarkMutex);
        if (!m_bookmarkManager) {
            m_bookmarkManager = new BookmarkManager;
            m_bookmarkManager->setupBookmarkModels();
            const QString &url = QString::fromLatin1("qthelp://org.qt-project.qtcreator."
                "%1%2%3/doc/index.html").arg(IDE_VERSION_MAJOR).arg(IDE_VERSION_MINOR)
                .arg(IDE_VERSION_RELEASE);
            helpEngine().setCustomValue(QLatin1String("DefaultHomePage"), url);
        }
    }
    return *m_bookmarkManager;
}

QVariant LocalHelpManager::engineFontSettings()
{
    return helpEngine().customValue(Constants::FontKey, QVariant());
}

/*!
 * Checks if the string does contain a scheme, and if that scheme is a "sensible" scheme for
 * opening in a internal or external browser (qthelp, about, file, http, https).
 * This is necessary to avoid trying to open e.g. "Foo::bar" in a external browser.
 */
bool LocalHelpManager::isValidUrl(const QString &link)
{
    QUrl url(link);
    if (!url.isValid())
        return false;
    const QString scheme = url.scheme();
    return (scheme == QLatin1String("qthelp")
            || scheme == QLatin1String("about")
            || scheme == QLatin1String("file")
            || scheme == QLatin1String("http")
            || scheme == QLatin1String("https"));
}

QByteArray LocalHelpManager::helpData(const QUrl &url)
{
    const QHelpEngineCore &engine = helpEngine();

    return engine.findFile(url).isValid() ? engine.fileData(url)
            : tr("Could not load \"%1\".").arg(url.toString()).toUtf8();
}
