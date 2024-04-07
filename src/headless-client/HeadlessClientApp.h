#ifndef HEADLESS_CLIENT_APP_H
#define HEADLESS_CLIENT_APP_H

#include "SessionController.h"
#include "libclient/net/client.h"
#include "libclient/net/login.h"
#include "libclient/settings.h"
#include "libshared/util/qtcompat.h"
#include <QCoreApplication>

class HeadlessClientApp final : public QCoreApplication {
	Q_OBJECT
public:
	HeadlessClientApp(int &argc, char **argv);
	~HeadlessClientApp() override;

	static HeadlessClientApp *instance();

	void init();
	void initDriver();
	void initCanvas();

public slots:
	void onLoginMethodChoiceNeeded(
		const QVector<net::LoginHandler::LoginMethod> &methods, const QUrl &url,
		const QUrl &extAuthUrl, const QString &loginInfo);

	void onSessionPasswordNeeded();
	void onServerDisconnected(
		const QString &message, const QString &errorcode, bool localDisconnect);
	void onServerConnected(const QString &address, int port);
	void onLoginNeeded(
		const QString &currentUsername, const QString &prompt,
		const QString &host, net::LoginHandler::LoginMethod intent);
	void onServerLogin(
		bool join, bool compatibilityMode, const QString &joinPassword,
		const QString &authId);
	void onServerLog(const QString &message);
	void onSessionChoiceNeeded(net::LoginSessionModel *sessions);
	void
	onSessionConfirmationNeeded(const QString &title, bool nsfm, bool autoJoin);
	void onMessagesReceived(int count, const net::Message *msgs);

private:
	libclient::settings::Settings *m_settings;
	net::LoginHandler *m_loginHandler;
	net::Client *m_client;
	SessionController *m_sessionController;
	QString m_url;
	SessionSettings m_sessionSettings;
};

#endif // HEADLESS_CLIENT_APP_H
