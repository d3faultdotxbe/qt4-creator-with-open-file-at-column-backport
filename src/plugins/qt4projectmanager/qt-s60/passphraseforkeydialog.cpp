/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "passphraseforkeydialog.h"

#include <QtGui/QCheckBox>
#include <QtGui/QDialogButtonBox>
#include <QtGui/QLabel>
#include <QtGui/QLineEdit>
#include <QtGui/QPushButton>
#include <QtGui/QVBoxLayout>

using namespace Qt4ProjectManager;

PassphraseForKeyDialog::PassphraseForKeyDialog(const QString &keyName, QWidget *parent) :
    QDialog(parent),
    m_buttonBox(0),
    m_passphraseEdit(0),
    m_saveCheckBox(0)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    QHBoxLayout *hPasswordLayout = new QHBoxLayout;

    QLabel *passphraseLabel = new QLabel(this);
    passphraseLabel->setText(tr("Passphrase:"));
    hPasswordLayout->addWidget(passphraseLabel);

    m_passphraseEdit = new QLineEdit(this);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    connect(m_passphraseEdit, SIGNAL(textChanged(QString)), this, SLOT(passphraseChanged()));
    hPasswordLayout->addWidget(m_passphraseEdit);

    m_saveCheckBox = new QCheckBox(this);
    m_saveCheckBox->setText(tr("Save passphrase"));
    m_saveCheckBox->setToolTip(tr("This is an insecure option. The password will be saved as plain text."));

    m_buttonBox = new QDialogButtonBox(this);
    m_buttonBox->setStandardButtons(QDialogButtonBox::Cancel|QDialogButtonBox::Ok);

    connect(m_buttonBox, SIGNAL(accepted()), this, SLOT(accept()));
    connect(m_buttonBox, SIGNAL(rejected()), this, SLOT(reject()));

    layout->addLayout(hPasswordLayout);
    layout->addWidget(m_saveCheckBox);
    layout->addItem(new QSpacerItem(0, 10));
    layout->addWidget(m_buttonBox);

    setWindowTitle(tr("Passphrase for %1").arg(keyName));
    setFixedSize(sizeHint());

    passphraseChanged();
}

void PassphraseForKeyDialog::passphraseChanged()
{
    // We tried the empty passphrase when we get here, so disallow it
    Q_ASSERT(m_buttonBox->button(QDialogButtonBox::Ok));
    m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(!m_passphraseEdit->text().isEmpty());
}

QString PassphraseForKeyDialog::passphrase() const
{
    return m_passphraseEdit->text();
}

bool PassphraseForKeyDialog::savePassphrase() const
{
    return m_saveCheckBox->isChecked();
}
