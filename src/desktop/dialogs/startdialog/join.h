// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DESKTOP_DIALOGS_STARTDIALOG_JOIN_H
#define DESKTOP_DIALOGS_STARTDIALOG_JOIN_H

#include "desktop/dialogs/startdialog/page.h"
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QUrl;

namespace widgets {
class RecentScroll;
}

namespace dialogs {
namespace startdialog {

class Join final : public Page {
	Q_OBJECT
public:
	Join(QWidget *parent = nullptr);
	void activate() final override;
	void accept() final override;

public slots:
	void setAddress(const QString &address);

signals:
	void showButtons();
	void enableJoin(bool enabled);
	void join(const QUrl &url);

private slots:
	void acceptAddress(const QString &address);
	void addressChanged(const QString &address);

private:
	void resetAddressPlaceholderText();
	void updateJoinButton();

	static QString fixUpInviteOrWebAddress(const QString &address);

	QUrl getUrl() const;

	QLineEdit *m_addressEdit;
	QLabel *m_addressMessageLabel;
	widgets::RecentScroll *m_recentScroll;
};

}
}

#endif
