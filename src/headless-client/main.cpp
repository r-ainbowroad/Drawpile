#include "HeadlessClientApp.h"
#include "libclient/drawdance/global.h"
#include "libclient/net/loginsessions.h"
#include <iostream>
#include <qcommandlineparser.h>

HeadlessClientApp::HeadlessClientApp(int &argc, char **argv)
	: QCoreApplication(argc, argv)
{
}

HeadlessClientApp::~HeadlessClientApp() {}

HeadlessClientApp *HeadlessClientApp::instance()
{
	return reinterpret_cast<HeadlessClientApp *>(QCoreApplication::instance());
}

#define LOG_SOCKET(base, name)                                                 \
	[]() {                                                                     \
		std::cout << #base << "::" << #name << std::endl;                      \
	}
#define CONNECT_LOG_SOCKET(object, base, name)                                 \
	connect(object, &base::name, this, LOG_SOCKET(object, name))
#define CONNECT_LOGIN_LOG_SOCKET(name)                                         \
	CONNECT_LOG_SOCKET(m_loginHandler, net::LoginHandler, name)
#define CONNECT_CLIENT_LOG_SOCKET(name)                                        \
	CONNECT_LOG_SOCKET(m_client, net::Client, name)

void HeadlessClientApp::init()
{
	// TODO: Setup proper logging.
	initDriver();

	drawdance::initLogging();
	drawdance::initCpuSupport();
	drawdance::DrawContextPool::init();

	qInfo() << "Connecting to: " << m_url;

	QUrl addr(m_url);
	m_settings = new libclient::settings::Settings(this);
	m_loginHandler =
		new net::LoginHandler(net::LoginHandler::Mode::Join, addr, this);
	m_client = new net::Client(this);

	initCanvas();

	connect(
		m_loginHandler, &net::LoginHandler::sessionPasswordNeeded, this,
		&HeadlessClientApp::onSessionPasswordNeeded);
	connect(
		m_loginHandler, &net::LoginHandler::loginNeeded, this,
		&HeadlessClientApp::onLoginNeeded);
	connect(
		m_loginHandler, &net::LoginHandler::loginMethodChoiceNeeded, this,
		&HeadlessClientApp::onLoginMethodChoiceNeeded);
	connect(
		m_loginHandler, &net::LoginHandler::sessionChoiceNeeded, this,
		&HeadlessClientApp::onSessionChoiceNeeded);
	connect(
		m_loginHandler, &net::LoginHandler::sessionConfirmationNeeded, this,
		&HeadlessClientApp::onSessionConfirmationNeeded);
	CONNECT_LOGIN_LOG_SOCKET(ruleAcceptanceNeeded);
	CONNECT_LOGIN_LOG_SOCKET(loginMethodMismatch);
	CONNECT_LOGIN_LOG_SOCKET(usernameNeeded);
	CONNECT_LOGIN_LOG_SOCKET(extAuthNeeded);
	CONNECT_LOGIN_LOG_SOCKET(extAuthComplete);
	CONNECT_LOGIN_LOG_SOCKET(loginOk);
	CONNECT_LOGIN_LOG_SOCKET(badLoginPassword);
	CONNECT_LOGIN_LOG_SOCKET(badSessionPassword);
	CONNECT_LOGIN_LOG_SOCKET(certificateCheckNeeded);
	CONNECT_LOGIN_LOG_SOCKET(serverTitleChanged);

	connect(
		m_client, &net::Client::serverConnected, this,
		&HeadlessClientApp::onServerConnected);
	connect(
		m_client, &net::Client::serverDisconnected, this,
		&HeadlessClientApp::onServerDisconnected);
	connect(
		m_client, &net::Client::serverLoggedIn, this,
		&HeadlessClientApp::onServerLogin);
	connect(
		m_client, &net::Client::serverLog, this,
		&HeadlessClientApp::onServerLog);
	CONNECT_CLIENT_LOG_SOCKET(messagesReceived);
	CONNECT_CLIENT_LOG_SOCKET(drawingCommandsLocal);
	CONNECT_CLIENT_LOG_SOCKET(catchupProgress);
	CONNECT_CLIENT_LOG_SOCKET(bansExported);
	CONNECT_CLIENT_LOG_SOCKET(bansImported);
	CONNECT_CLIENT_LOG_SOCKET(bansImpExError);
	CONNECT_CLIENT_LOG_SOCKET(needSnapshot);
	CONNECT_CLIENT_LOG_SOCKET(sessionResetted);
	// CONNECT_CLIENT_LOG_SOCKET(sessionConfChange);
	CONNECT_CLIENT_LOG_SOCKET(sessionOutOfSpace);
	CONNECT_CLIENT_LOG_SOCKET(serverLoggedIn);
	CONNECT_CLIENT_LOG_SOCKET(serverDisconnecting);
	CONNECT_CLIENT_LOG_SOCKET(serverMessage);
	// CONNECT_CLIENT_LOG_SOCKET(bytesReceived);
	// CONNECT_CLIENT_LOG_SOCKET(bytesSent);
	// CONNECT_CLIENT_LOG_SOCKET(lagMeasured);
	CONNECT_CLIENT_LOG_SOCKET(autoresetRequested);
	CONNECT_CLIENT_LOG_SOCKET(serverStatusUpdate);
	CONNECT_CLIENT_LOG_SOCKET(userInfoRequested);
	CONNECT_CLIENT_LOG_SOCKET(userInfoReceived);
	CONNECT_CLIENT_LOG_SOCKET(currentBrushRequested);
	CONNECT_CLIENT_LOG_SOCKET(currentBrushReceived);

	m_client->connectToServer(20, m_loginHandler, false);
}

static void printVersion()
{
	printf("headless-client %s\n", cmake_config::version());
	printf(
		"Protocol version: %d.%d\n", cmake_config::proto::major(),
		cmake_config::proto::minor());
	printf(
		"Qt version: %s (compiled against %s)\n", qVersion(), QT_VERSION_STR);
}

void HeadlessClientApp::initDriver()
{
	setOrganizationName("drawpile");
	setOrganizationDomain("drawpile.net");
	setApplicationName("headless-client");
	setApplicationVersion(cmake_config::version());

	QCommandLineParser parser;
	parser.setApplicationDescription("Headless Drawpile client");
	parser.addHelpOption();

	// --version, -v
	QCommandLineOption versionOption(
		QStringList() << "v"
					  << "version",
		"Displays version information.");
	parser.addOption(versionOption);

	// --verbose, -V
	QCommandLineOption verboseOption(
		QStringList() << "V"
					  << "verbose",
		"Enable verbose output.");
	parser.addOption(verboseOption);

	// --url, -u
	QCommandLineOption urlOption(
		QStringList() << "u"
					  << "url",
		"drawpile:// session URL.", "url");
	parser.addOption(urlOption);

	// --out, -o
	QCommandLineOption outOption(
		QStringList() << "o"
					  << "out",
		"Path to write output to.", "path");
	parser.addOption(outOption);

	QCommandLineOption factionOption(
		QStringList() << "t"
					  << "template",
		"Template name used for the topic name in mqtt and folder on s3.",
		"name");
	parser.addOption(factionOption);

	QCommandLineOption mqttOption(
		QStringList() << "m"
					  << "mqtt",
		"URL for mqtt connection.", "url");
	parser.addOption(mqttOption);

	QCommandLineOption s3Option(
		QStringList() << "s"
					  << "s3",
		"URL for s3 upload. Includes keys as username and password.", "url");
	parser.addOption(s3Option);

	parser.process(*this);

	if(parser.isSet(versionOption)) {
		printVersion();
		qApp->exit(0);
	}

	if(!parser.isSet(urlOption) || !parser.isSet(outOption))
		parser.showHelp(1);

	m_url = parser.value(urlOption);
	m_sessionSettings.verbose = parser.isSet(verboseOption);
	m_sessionSettings.outputPath = parser.value(outOption);
	m_sessionSettings.faction = parser.value(factionOption);
	m_sessionSettings.mqttUrl = parser.value(mqttOption);
	m_sessionSettings.s3Url = parser.value(s3Option);
}

void HeadlessClientApp::initCanvas()
{
	connect(
		m_client, &net::Client::messagesReceived, this,
		&HeadlessClientApp::onMessagesReceived);
	m_sessionController =
		new SessionController(std::move(m_sessionSettings), this);
	connect(
		m_sessionController, &SessionController::sendMessage, m_client,
		&net::Client::sendMessage);
}

void HeadlessClientApp::onSessionPasswordNeeded()
{
	std::cout << "HeadlessClientApp::onSessionPasswordNeeded" << std::endl;
	// This should already be included in the URL.
	qCritical()
		<< "drawpile:// URL did not include the correct session password";
	exit(1);
}

void HeadlessClientApp::onServerConnected(const QString &address, int port)
{
	std::cout << "server connected: addr: " << address.toStdString()
			  << " port: " << port << std::endl;
}

void HeadlessClientApp::onServerDisconnected(
	const QString &message, const QString &errorcode [[maybe_unused]],
	bool localDisconnect [[maybe_unused]])
{
	qCritical() << message;
	exit(1);
}

void HeadlessClientApp::onLoginNeeded(
	const QString &currentUsername [[maybe_unused]],
	const QString &prompt [[maybe_unused]],
	const QString &host [[maybe_unused]],
	net::LoginHandler::LoginMethod intent [[maybe_unused]])
{
	std::cout << "login needed" << std::endl;
}

const char *methodToStr(net::LoginHandler::LoginMethod method)
{
	switch(method) {
	case net::LoginHandler::LoginMethod::Auth:
		return "Auth";
	case net::LoginHandler::LoginMethod::ExtAuth:
		return "ExtAuth";
	case net::LoginHandler::LoginMethod::Guest:
		return "Guest";
	case net::LoginHandler::LoginMethod::Unknown:
		return "Unknown";
	}
	return "<bad value>";
}

void HeadlessClientApp::onLoginMethodChoiceNeeded(
	const QVector<net::LoginHandler::LoginMethod> &methods, const QUrl &url,
	const QUrl &extAuthUrl [[maybe_unused]], const QString &loginInfo)
{
	std::cout << "onLoginMethodChoiceNeeded - "
			  << "url: " << url.toString().toStdString()
			  << " loginInfo: " << loginInfo.toStdString() << " methods:\n";
	for(auto method : methods) {
		std::cout << "\t" << methodToStr(method) << "\n";
	}
	std::cout.flush();
	QUrl dpUrl(m_url);
	m_loginHandler->selectIdentity(
		dpUrl.userName(), dpUrl.password(),
		net::LoginHandler::LoginMethod::Auth);
}

void HeadlessClientApp::onServerLogin(
	bool join, bool compatibilityMode [[maybe_unused]],
	const QString &joinPassword, const QString &authId)
{
	std::cout << "HeadlessClientApp::onServerLogin - "
			  << " join: " << join
			  << " password: " << joinPassword.toStdString()
			  << " authId: " << authId.toStdString() << std::endl;
	m_sessionController->onSessionStart(m_client->myId());
}

void HeadlessClientApp::onServerLog(const QString &message)
{
	if(m_sessionSettings.verbose)
		std::cout << "server message: " << message.toStdString() << std::endl;
}

void HeadlessClientApp::onSessionChoiceNeeded(net::LoginSessionModel *sessions)
{
	std::cout << "HeadlessClientApp::onSessionChoiceNeeded - "
			  << " sessions: " << sessions->rowCount() << std::endl;
}

void HeadlessClientApp::onSessionConfirmationNeeded(
	const QString &title, bool nsfm, bool autoJoin)
{
	std::cout << "HeadlessClientApp::onSessionConfirmationNeeded - "
			  << " title: " << title.toStdString() << " nsfm?: " << nsfm
			  << " autoJoin?: " << autoJoin << std::endl;
	m_loginHandler->confirmJoinSelectedSession();
}

void HeadlessClientApp::onMessagesReceived(int count, const net::Message *msgs)
{
	if(m_sessionController)
		m_sessionController->onMessagesReceived(count, msgs);
}

int main(int argc, char *argv[])
{
	HeadlessClientApp app(argc, argv);
	app.init();
	return HeadlessClientApp::exec();
}
