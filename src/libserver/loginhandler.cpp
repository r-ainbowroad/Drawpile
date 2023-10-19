// SPDX-License-Identifier: GPL-3.0-or-later
#include "libserver/loginhandler.h"
#include "cmake-config/config.h"
#include "libserver/client.h"
#include "libserver/serverconfig.h"
#include "libserver/serverlog.h"
#include "libserver/session.h"
#include "libserver/sessions.h"
#include "libshared/net/servercmd.h"
#include "libshared/util/authtoken.h"
#include "libshared/util/networkaccess.h"
#include "libshared/util/validators.h"
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>

namespace server {

Sessions::~Sessions() {}

LoginHandler::LoginHandler(
	Client *client, Sessions *sessions, ServerConfig *config)
	: QObject(client)
	, m_client(client)
	, m_sessions(sessions)
	, m_config(config)
{
	connect(
		client, &Client::loginMessage, this, &LoginHandler::handleLoginMessage);
}

void LoginHandler::startLoginProcess()
{
	m_state = State::WaitForIdent;

	QJsonArray flags;

	if(m_config->getConfigInt(config::SessionCountLimit) > 1)
		flags << "MULTI";
	if(m_config->getConfigBool(config::EnablePersistence))
		flags << "PERSIST";
	if(m_client->hasSslSupport()) {
		flags << "TLS"
			  << "SECURE";
		m_state = State::WaitForSecure;
	}
	if(!m_config->getConfigBool(config::AllowGuests))
		flags << "NOGUEST";
	if(m_config->internalConfig().reportUrl.isValid() &&
	   m_config->getConfigBool(config::AbuseReport))
		flags << "REPORT";
	if(m_config->getConfigBool(config::AllowCustomAvatars))
		flags << "AVATAR";
#ifdef HAVE_LIBSODIUM
	if(!m_config->internalConfig().cryptKey.isEmpty()) {
		flags << "CBANIMPEX";
	}
#endif
	// Moderators can always export bans.
	flags << "MBANIMPEX";

	// Start by telling who we are
	send(net::ServerReply::makeLoginGreeting(
		QStringLiteral("Drawpile server %1").arg(cmake_config::version()),
		cmake_config::proto::server(), flags));

	// Client should disconnect upon receiving the above if the version number
	// does not match
}

void LoginHandler::announceServerInfo()
{
	const QJsonArray sessions = m_sessions->sessionDescriptions();

	QString message = QStringLiteral("Welcome");
	QString title = m_config->getConfigString(config::ServerTitle);
	bool wholeMessageSent = send(
		title.isEmpty()
			? net::ServerReply::makeLoginSessions(message, sessions)
			: net::ServerReply::makeLoginWelcome(message, title, sessions));
	if(!wholeMessageSent) {
		// Reply was too long to fit in the message envelope!
		// Split the reply into separate announcements and send it in pieces
		if(!title.isEmpty()) {
			send(net::ServerReply::makeLoginTitle(message, title));
		}
		for(const QJsonValue &session : sessions) {
			send(net::ServerReply::makeLoginSessions(message, {session}));
		}
	}
}

void LoginHandler::announceSession(const QJsonObject &session)
{
	Q_ASSERT(session.contains("id"));
	if(m_state == State::WaitForLogin) {
		send(net::ServerReply::makeLoginSessions(
			QStringLiteral("New session"), {session}));
	}
}

void LoginHandler::announceSessionEnd(const QString &id)
{
	if(m_state == State::WaitForLogin) {
		send(net::ServerReply::makeLoginRemoveSessions(
			QStringLiteral("Session ended"), {id}));
	}
}

void LoginHandler::handleLoginMessage(const net::Message &msg)
{
	if(msg.type() != DP_MSG_SERVER_COMMAND) {
		m_client->log(
			Log()
				.about(Log::Level::Error, Log::Topic::RuleBreak)
				.message("Login handler was passed a non-login message"));
		return;
	}

	net::ServerCommand cmd = net::ServerCommand::fromMessage(msg);

	if(m_state == State::Banned) {
		// Intentionally leave the client hanging.
	} else if(m_state == State::WaitForSecure) {
		// Secure mode: wait for STARTTLS before doing anything
		if(cmd.cmd == "startTls") {
			handleStarttls();
		} else {
			m_client->log(Log()
							  .about(Log::Level::Error, Log::Topic::RuleBreak)
							  .message("Client did not upgrade to TLS mode!"));
			sendError("tlsRequired", "TLS required");
		}

	} else if(m_state == State::WaitForIdent) {
		// Wait for user identification before moving on to session listing
		if(cmd.cmd == "ident") {
			handleIdentMessage(cmd);
		} else {
			m_client->log(
				Log()
					.about(Log::Level::Error, Log::Topic::RuleBreak)
					.message(
						"Invalid login command (while waiting for ident): " +
						cmd.cmd));
			m_client->disconnectClient(
				Client::DisconnectionReason::Error, "invalid message");
		}
	} else {
		if(cmd.cmd == "host") {
			handleHostMessage(cmd);
		} else if(cmd.cmd == "join") {
			handleJoinMessage(cmd);
		} else if(cmd.cmd == "report") {
			handleAbuseReport(cmd);
		} else {
			m_client->log(Log()
							  .about(Log::Level::Error, Log::Topic::RuleBreak)
							  .message(
								  "Invalid login command (while waiting for "
								  "join/host): " +
								  cmd.cmd));
			m_client->disconnectClient(
				Client::DisconnectionReason::Error, "invalid message");
		}
	}
}

#if HAVE_LIBSODIUM
static QStringList jsonArrayToStringList(const QJsonArray &a)
{
	QStringList sl;
	for(const auto &v : a) {
		const auto s = v.toString();
		if(!s.isEmpty())
			sl << s;
	}
	return sl;
}
#endif

void LoginHandler::handleIdentMessage(const net::ServerCommand &cmd)
{
	if(cmd.args.size() != 1 && cmd.args.size() != 2) {
		sendError("syntax", "Expected username and (optional) password");
		return;
	}

	const QString username = cmd.args[0].toString();
	const QString password =
		cmd.args.size() > 1 ? cmd.args[1].toString() : QString();

	if(!validateUsername(username)) {
		sendError("badUsername", "Invalid username");
		return;
	}

	const RegisteredUser userAccount =
		m_config->getUserAccount(username, password);

	if(userAccount.status != RegisteredUser::NotFound &&
	   cmd.kwargs.contains("extauth")) {
		// This should never happen. If it does, it means there's a bug in the
		// client or someone is probing for bugs in the server.
		sendError(
			"extAuthError",
			"Cannot use extauth with an internal user account!");
		return;
	}

	if(cmd.kwargs.contains("avatar") &&
	   m_config->getConfigBool(config::AllowCustomAvatars)) {
		// TODO validate
		m_client->setAvatar(
			QByteArray::fromBase64(cmd.kwargs["avatar"].toString().toUtf8()));
	}

	switch(userAccount.status) {
	case RegisteredUser::NotFound: {
		// Account not found in internal user list. Allow guest login (if
		// enabled) or require external authentication
		const bool allowGuests = m_config->getConfigBool(config::AllowGuests);
		const bool useExtAuth =
			m_config->internalConfig().extAuthUrl.isValid() &&
			m_config->getConfigBool(config::UseExtAuth);

		if(useExtAuth) {
#ifdef HAVE_LIBSODIUM
			if(cmd.kwargs.contains("extauth")) {
				// An external authentication token was provided
				if(m_extauth_nonce == 0) {
					sendError("extAuthError", "Ext auth not requested!");
					return;
				}
				const AuthToken extAuthToken(
					cmd.kwargs["extauth"].toString().toUtf8());
				const QByteArray key = QByteArray::fromBase64(
					m_config->getConfigString(config::ExtAuthKey).toUtf8());
				if(!extAuthToken.checkSignature(key)) {
					sendError(
						"extAuthError", "Ext auth token signature mismatch!");
					return;
				}
				if(!extAuthToken.validatePayload(
					   m_config->getConfigString(config::ExtAuthGroup),
					   m_extauth_nonce)) {
					sendError("extAuthError", "Ext auth token is invalid!");
					return;
				}

				// Token is valid: log in as an authenticated user
				const QJsonObject ea = extAuthToken.payload();
				const QJsonValue uid = ea["uid"];

				bool extAuthMod = m_config->getConfigBool(config::ExtAuthMod);
				bool extAuthBanExempt =
					m_config->getConfigBool(config::ExtAuthBanExempt);
				QStringList flags =
					jsonArrayToStringList(ea["flags"].toArray());
				m_client->applyBanExemption(
					(extAuthMod && flags.contains("MOD")) ||
					(extAuthBanExempt && flags.contains("BANEXEMPT")));

				if(uid.isDouble() && !verifyUserId(uid.toDouble())) {
					return;
				}

				// We need some unique identifier. If the server didn't provide
				// one, the username is better than nothing.
				QString extAuthId =
					uid.isDouble() ? QString::number(qlonglong(uid.toDouble()))
								   : uid.toString();
				if(extAuthId.isEmpty())
					extAuthId = ea["username"].toString();

				// Prefix to identify this auth ID as an ext-auth ID
				extAuthId = m_config->internalConfig().extAuthUrl.host() + ":" +
							extAuthId;

				QByteArray avatar;
				if(m_config->getConfigBool(config::ExtAuthAvatars))
					avatar = extAuthToken.avatar();

				authLoginOk(
					ea["username"].toString(), extAuthId, flags, avatar,
					m_config->getConfigBool(config::ExtAuthMod),
					m_config->getConfigBool(config::ExtAuthHost));

			} else {
				// No ext-auth token provided: request it now

				// If both guest logins and ExtAuth is enabled, we must query
				// the auth server first to determine if guest login is possible
				// for this user. If guest logins are not enabled, we always
				// just request ext-auth
				if(allowGuests)
					extAuthGuestLogin(username);
				else
					requestExtAuth();
			}

#else
			// This should never be reached
			sendError(
				"extAuthError",
				"Server misconfiguration: ext-auth support not compiled in.");
#endif
			return;

		} else {
			// ExtAuth not enabled: permit guest login if enabled
			if(allowGuests) {
				guestLogin(username);
				return;
			}
		}
		Q_FALLTHROUGH(); // fall through to badpass if guest logins are disabled
	}
	case RegisteredUser::BadPass:
		if(password.isEmpty()) {
			// No password: tell client that guest login is not possible (for
			// this username)
			m_state = State::WaitForIdent;
			send(net::ServerReply::makeResultPasswordNeeded(
				QStringLiteral("Password needed"),
				QStringLiteral("needPassword")));

		} else {
			sendError("badPassword", "Incorrect password");
		}
		return;

	case RegisteredUser::Banned:
		sendError("bannedName", "This username is banned");
		return;

	case RegisteredUser::Ok:
		// Yay, username and password were valid!
		m_client->applyBanExemption(
			userAccount.flags.contains("MOD") ||
			userAccount.flags.contains("BANEXEMPT"));
		authLoginOk(
			username, QStringLiteral("internal:%1").arg(userAccount.userId),
			userAccount.flags, QByteArray(), true, true);
		break;
	}
}

void LoginHandler::authLoginOk(
	const QString &username, const QString &authId, const QStringList &flags,
	const QByteArray &avatar, bool allowMod, bool allowHost)
{
	Q_ASSERT(!authId.isEmpty());

	m_client->setUsername(username);
	m_client->setAuthId(authId);
	m_client->setAuthFlags(flags);

	m_client->setModerator(flags.contains("MOD") && allowMod);
	if(!avatar.isEmpty())
		m_client->setAvatar(avatar);
	m_hostPrivilege = flags.contains("HOST") && allowHost;

	if(m_client->triggerBan(true)) {
		m_state = State::Banned;
		return;
	} else {
		m_state = State::WaitForLogin;
	}

	send(net::ServerReply::makeResultLoginOk(
		QStringLiteral("Authenticated login OK!"), QStringLiteral("identOk"),
		QJsonArray::fromStringList(flags), m_client->username(), false));
	announceServerInfo();
}

/**
 * @brief Make a request to the ext-auth server and check if guest login is
 * possible for the given username
 *
 * If guest login is possible, do that.
 * Otherwise request external authentication.
 *
 * If the authserver cannot be reached, guest login is permitted (fallback mode)
 * or not.
 * @param username
 */
void LoginHandler::extAuthGuestLogin(const QString &username)
{
	QNetworkRequest req(m_config->internalConfig().extAuthUrl);
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

	QJsonObject o;
	o["username"] = username;
	const QString authGroup = m_config->getConfigString(config::ExtAuthGroup);
	if(!authGroup.isEmpty())
		o["group"] = authGroup;

	m_client->log(Log()
					  .about(Log::Level::Info, Log::Topic::Status)
					  .message(QStringLiteral("Querying auth server for %1...")
								   .arg(username)));
	QNetworkReply *reply =
		networkaccess::getInstance()->post(req, QJsonDocument(o).toJson());
	connect(reply, &QNetworkReply::finished, this, [reply, username, this]() {
		reply->deleteLater();

		if(m_state != State::WaitForIdent) {
			sendError(
				"extauth", "Received auth serveer reply in unexpected state");
			return;
		}

		bool fail = false;
		if(reply->error() != QNetworkReply::NoError) {
			fail = true;
			m_client->log(
				Log()
					.about(Log::Level::Warn, Log::Topic::Status)
					.message("Auth server error: " + reply->errorString()));
		}

		QJsonDocument doc;
		if(!fail) {
			QJsonParseError error;
			doc = QJsonDocument::fromJson(reply->readAll(), &error);
			if(error.error != QJsonParseError::NoError) {
				fail = true;
				m_client->log(Log()
								  .about(Log::Level::Warn, Log::Topic::Status)
								  .message(
									  "Auth server JSON parse error: " +
									  error.errorString()));
			}
		}

		if(fail) {
			if(m_config->getConfigBool(config::ExtAuthFallback)) {
				// Fall back to guest logins
				guestLogin(username);
			} else {
				// If fallback mode is disabled, deny all non-internal logins
				sendError("noExtAuth", "Authentication server is unavailable!");
			}
			return;
		}

		const QJsonObject obj = doc.object();
		const QString status = obj["status"].toString();
		if(status == "auth") {
			requestExtAuth();

		} else if(status == "guest") {
			guestLogin(username);

		} else if(status == "outgroup") {
			sendError(
				"extauthOutgroup",
				"This username cannot log in to this server");

		} else {
			sendError("extauth", "Unexpected ext-auth response: " + status);
		}
	});
}

void LoginHandler::requestExtAuth()
{
#ifdef HAVE_LIBSODIUM
	Q_ASSERT(m_extauth_nonce == 0);
	m_extauth_nonce = AuthToken::generateNonce();
	send(net::ServerReply::makeResultExtAuthNeeded(
		QStringLiteral("External authentication needed"),
		QStringLiteral("needExtAuth"),
		m_config->internalConfig().extAuthUrl.toString(),
		QString::number(m_extauth_nonce, 16),
		m_config->getConfigString(config::ExtAuthGroup),
		m_client->avatar().isEmpty() &&
			m_config->getConfigBool(config::ExtAuthAvatars)));
#else
	qFatal("Bug: requestExtAuth() called, even though libsodium is not "
		   "compiled in!");
#endif
}

void LoginHandler::guestLogin(const QString &username)
{
	if(!m_config->getConfigBool(config::AllowGuests)) {
		sendError("noGuest", "Guest logins not allowed");
		return;
	}

	m_client->setUsername(username);

	if(m_client->triggerBan(true)) {
		m_state = State::Banned;
		return;
	} else {
		m_state = State::WaitForLogin;
	}

	send(net::ServerReply::makeResultLoginOk(
		QStringLiteral("Guest login OK!"), QStringLiteral("identOk"), {},
		m_client->username(), true));
	announceServerInfo();
}

static QJsonArray sessionFlags(const Session *session)
{
	QJsonArray flags;
	// Note: this is "NOAUTORESET" for backward compatibility. In 3.0, we should
	// change it to "AUTORESET"
	if(!session->supportsAutoReset())
		flags << "NOAUTORESET";
	// TODO for version 3.0: PERSIST should be a session specific flag

	return flags;
}

void LoginHandler::handleHostMessage(const net::ServerCommand &cmd)
{
	Q_ASSERT(!m_client->username().isEmpty());
	// Basic validation
	if(!m_config->getConfigBool(config::AllowGuestHosts) && !m_hostPrivilege) {
		sendError("unauthorizedHost", "Hosting not authorized");
		return;
	}

	protocol::ProtocolVersion protocolVersion =
		protocol::ProtocolVersion::fromString(
			cmd.kwargs.value("protocol").toString());

	if(!protocolVersion.isValid()) {
		sendError("syntax", "Unparseable protocol version");
		return;
	}

	if(!verifySystemId(cmd, protocolVersion)) {
		return;
	}

	int userId = cmd.kwargs.value("user_id").toInt();

	if(userId < 1 || userId > 254) {
		sendError("syntax", "Invalid user ID (must be in range 1-254)");
		return;
	}

	m_client->setId(userId);

	QString sessionAlias = cmd.kwargs.value("alias").toString();
	if(!sessionAlias.isEmpty()) {
		if(!validateSessionIdAlias(sessionAlias)) {
			sendError("badAlias", "Invalid session alias");
			return;
		}
	}

	if(m_client->triggerBan(false)) {
		m_state = State::Banned;
		return;
	}

	// Create a new session
	Session *session;
	QString sessionErrorCode;
	std::tie(session, sessionErrorCode) = m_sessions->createSession(
		Ulid::make().toString(), sessionAlias, protocolVersion,
		m_client->username());

	if(!session) {
		QString msg;
		if(sessionErrorCode == "idInUse")
			msg = "An internal server error occurred.";
		else if(sessionErrorCode == "badProtocol")
			msg = "This server does not support this protocol version.";
		else if(sessionErrorCode == "closed")
			msg = "This server is full.";
		else
			msg = sessionErrorCode;

		sendError(sessionErrorCode, msg);
		return;
	}

	if(cmd.kwargs["password"].isString())
		session->history()->setPassword(cmd.kwargs["password"].toString());

	// Mark login phase as complete.
	// No more login messages will be sent to this user.
	send(net::ServerReply::makeResultJoinHost(
		QStringLiteral("Starting new session!"), QStringLiteral("host"),
		{{QStringLiteral("id"),
		  sessionAlias.isEmpty() ? session->id() : sessionAlias},
		 {QStringLiteral("user"), userId},
		 {QStringLiteral("flags"), sessionFlags(session)},
		 {QStringLiteral("authId"), m_client->authId()}}));

	m_complete = true;
	session->joinUser(m_client, true);
	logClientInfo(cmd);

	deleteLater();
}

void LoginHandler::handleJoinMessage(const net::ServerCommand &cmd)
{
	Q_ASSERT(!m_client->username().isEmpty());
	if(cmd.args.size() != 1) {
		sendError("syntax", "Expected session ID");
		return;
	}

	QString sessionId = cmd.args.at(0).toString();
	Session *session = m_sessions->getSessionById(sessionId, true);
	if(!session) {
		sendError("notFound", "Session not found!");
		return;
	}

	if(!verifySystemId(cmd, session->history()->protocolVersion())) {
		return;
	}

	if(!m_client->isModerator()) {
		// Non-moderators have to obey access restrictions
		int banId = session->history()->banlist().isBanned(
			m_client->username(), m_client->peerAddress(), m_client->authId(),
			m_client->sid());
		if(banId != 0) {
			session->log(
				Log()
					.about(Log::Level::Info, Log::Topic::Ban)
					.user(
						m_client->id(), m_client->peerAddress(),
						m_client->username())
					.message(
						QStringLiteral("Join prevented by ban %1").arg(banId)));
			sendError(
				"banned", QStringLiteral(
							  "You have been banned from this session (ban %1)")
							  .arg(banId));
			return;
		}
		if(session->isClosed()) {
			sendError("closed", "This session is closed");
			return;
		}
		if(session->history()->hasFlag(SessionHistory::AuthOnly) &&
		   !m_client->isAuthenticated()) {
			sendError("authOnly", "This session does not allow guest logins");
			return;
		}

		if(!session->history()->checkPassword(
			   cmd.kwargs.value("password").toString())) {
			sendError("badPassword", "Incorrect password");
			return;
		}
	}

	if(session->getClientByUsername(m_client->username())) {
#ifdef NDEBUG
		sendError("nameInuse", "This username is already in use");
		return;
#else
		// Allow identical usernames in debug builds, so I don't have to keep
		// changing the username when testing. There is no technical requirement
		// for unique usernames; the limitation is solely for the benefit of the
		// human users.
		m_client->log(
			Log()
				.about(Log::Level::Warn, Log::Topic::RuleBreak)
				.message(
					"Username clash ignored because this is a debug build."));
#endif
	}

	if(m_client->triggerBan(false)) {
		m_state = State::Banned;
		return;
	}

	// Ok, join the session
	session->assignId(m_client);
	send(net::ServerReply::makeResultJoinHost(
		QStringLiteral("Joining a session!"), QStringLiteral("join"),
		{{QStringLiteral("id"), session->aliasOrId()},
		 {QStringLiteral("user"), m_client->id()},
		 {QStringLiteral("flags"), sessionFlags(session)},
		 {QStringLiteral("authId"), m_client->authId()}}));

	m_complete = true;
	session->joinUser(m_client, false);
	logClientInfo(cmd);

	deleteLater();
}

void LoginHandler::logClientInfo(const net::ServerCommand &cmd)
{
	QJsonObject info;
	QString keys[] = {
		QStringLiteral("app_version"), QStringLiteral("protocol_version"),
		QStringLiteral("qt_version"),  QStringLiteral("os"),
		QStringLiteral("s"),
	};
	for(const QString &key : keys) {
		if(cmd.kwargs.contains(key)) {
			QJsonValue value = cmd.kwargs[key];
			if(value.isString()) {
				QString s = value.toString().trimmed();
				if(!s.isEmpty()) {
					s.truncate(64);
					info[key] = s;
				}
			}
		}
	}


	info[QStringLiteral("address")] = m_client->peerAddress().toString();
	if(m_client->isAuthenticated()) {
		info[QStringLiteral("auth_id")] = m_client->authId();
	}
	m_client->log(
		Log()
			.about(Log::Level::Info, Log::Topic::ClientInfo)
			.message(QJsonDocument(info).toJson(QJsonDocument::Compact)));
}

void LoginHandler::handleAbuseReport(const net::ServerCommand &cmd)
{
	// We don't tell users that they're banned until they actually attempt to
	// join or host a session, so they don't get to make reports before that.
	if(!m_client->isBanInProgress()) {
		Session *s =
			m_sessions->getSessionById(cmd.kwargs["session"].toString(), false);
		if(s) {
			s->sendAbuseReport(m_client, 0, cmd.kwargs["reason"].toString());
		}
	}
}

void LoginHandler::handleStarttls()
{
	if(!m_client->hasSslSupport()) {
		// Note. Well behaved clients shouldn't send STARTTLS if TLS was not
		// listed in server features.
		sendError("noTls", "TLS not supported");
		return;
	}

	if(m_client->isSecure()) {
		sendError(
			"alreadySecure",
			"Connection already secured"); // shouldn't happen normally
		return;
	}

	send(net::ServerReply::makeResultStartTls(
		QStringLiteral("Start TLS now!"), true));

	m_client->startTls();
	m_state = State::WaitForIdent;
}

bool LoginHandler::send(const net::Message &msg)
{
	if(!m_complete) {
		if(msg.isNull()) {
			qWarning("Login message is null (input oversized?)");
			return false;
		}
		m_client->sendDirectMessage(msg);
	}
	return true;
}

void LoginHandler::sendError(const QString &code, const QString &message)
{
	send(net::ServerReply::makeError(message, code));
	m_client->disconnectClient(
		Client::DisconnectionReason::Error, "Login error");
}

bool LoginHandler::verifySystemId(
	const net::ServerCommand &cmd, const protocol::ProtocolVersion &protover)
{
	QString sid = cmd.kwargs[QStringLiteral("s")].toString();
	m_client->setSid(sid);
	if(sid.isEmpty()) {
		if(protover.shouldHaveSystemId()) {
			m_client->log(Log()
							  .about(Log::Level::Error, Log::Topic::RuleBreak)
							  .message(QStringLiteral("Missing required sid")));
			m_client->disconnectClient(
				Client::DisconnectionReason::Error, "invalid message");
			return false;
		}
	} else if(!isValidSid(sid)) {
		m_client->log(Log()
						  .about(Log::Level::Error, Log::Topic::RuleBreak)
						  .message(QStringLiteral("Invalid sid %1").arg(sid)));
		m_client->disconnectClient(
			Client::DisconnectionReason::Error, "invalid message");
		return false;
	} else if(!m_client->isBanInProgress()) {
		BanResult ban = m_config->isSystemBanned(sid);
		m_client->applyBan(ban);
	}
	return true;
}

bool LoginHandler::isValidSid(const QString &sid)
{
	static QRegularExpression sidRe(QStringLiteral("\\A[0-9a-fA-F]{32}\\z"));
	return sidRe.match(sid).hasMatch();
}

bool LoginHandler::verifyUserId(long long userId)
{
	if(!m_client->isBanInProgress()) {
		BanResult ban = m_config->isUserBanned(userId);
		m_client->applyBan(ban);
	}
	return true;
}

}
