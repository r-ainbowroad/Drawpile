// SPDX-License-Identifier: GPL-3.0-or-later

#include "SessionController.h"
#include "LayerRenderer.h"
#include "Palettize.h"

#include <cmake-config/config.h>
#include <libshared/net/servercmd.h>
#include <libshared/util/qtcompat.h>

#include "QCoroOnceFuture.h"
#include <QCoroFuture>
#include <QCoroProcess>
#include <QCoroTimer>
#include <QtConcurrent/QtConcurrent>
#include <QtMqtt/QtMqtt>

#include <format>
#include <optional>
#include <type_traits>
#include <utility>

extern "C" {
#include <dpcommon/output.h>
#include <dpengine/image.h>
}

SessionController::SessionController(SessionSettings settings, QObject *parent)
	: QObject(parent)
	, m_settings(std::move(settings))
	, m_userList(new canvas::UserListModel(this))
{
	// Initialize m_palette with the 2023 r/place ending palette.
	struct RGBA8 {
		uint8_t r;
		uint8_t g;
		uint8_t b;
		uint8_t a;
	};
	std::array<RGBA8, 32> defaultPalette{{
		{109, 0, 26, 255},	  {190, 0, 57, 255},	{255, 69, 0, 255},
		{255, 168, 0, 255},	  {255, 214, 53, 255},	{255, 248, 184, 255},
		{0, 163, 104, 255},	  {0, 204, 120, 255},	{126, 237, 86, 255},
		{0, 117, 111, 255},	  {0, 158, 170, 255},	{0, 204, 192, 255},
		{36, 80, 164, 255},	  {54, 144, 234, 255},	{81, 233, 244, 255},
		{73, 58, 193, 255},	  {106, 92, 255, 255},	{148, 179, 255, 255},
		{129, 30, 159, 255},  {180, 74, 192, 255},	{228, 171, 255, 255},
		{222, 16, 127, 255},  {255, 56, 129, 255},	{255, 153, 170, 255},
		{109, 72, 47, 255},	  {156, 105, 38, 255},	{255, 180, 112, 255},
		{0, 0, 0, 255},		  {81, 82, 82, 255},	{137, 141, 144, 255},
		{212, 215, 217, 255}, {255, 255, 255, 255},
	}};
	for(auto c : defaultPalette)
		m_palette.push_back(
			{.bytes = {.b = c.b, .g = c.g, .r = c.r, .a = c.a}});
}

void SessionController::sendChatMessage(QString msg)
{
	assert(
		m_myUserId != -1 && "sendChatMessage called before session connected!");
	emit sendMessage(net::makeChatMessage(m_myUserId, 0, 0, msg));
}

template <typename QEnum> std::string QtEnumToString(const QEnum value)
{
	return std::string(QMetaEnum::fromType<QEnum>().valueToKey(value));
}

void SessionController::onSessionStart(int myUserId)
{
	m_myUserId = myUserId;
	m_paintEngine = std::make_unique<PaintEngine>();
	// TODO: Hook up acl changes from the `DP_PaintEngine` to `m_userList`.
	//       Right now user state changes are ignored, and we can't use trusted
	//       for controlling who can commit changes.
	if(m_mqttClient)
		m_mqttClient->deleteLater();
	m_mqttClient = new QMqttClient(this);
	m_mqttClient->setProtocolVersion(QMqttClient::MQTT_5_0);
	m_mqttClient->setClientId(
		QString("drawpile-client - ") + cmake_config::version());
	QUrl url = m_settings.mqttUrl;
	m_mqttClient->setHostname(url.host());
	if(url.port() != -1)
		m_mqttClient->setPort(url.port());
	if(QString user = url.userName(); !user.isEmpty())
		m_mqttClient->setUsername(user);
	if(QString pass = url.password(); !pass.isEmpty())
		m_mqttClient->setPassword(pass);

	// TODO: Handle mqtt disconnecting. Probably just reject commits until a
	//       connection can be reestablished.
	connect(m_mqttClient, &QMqttClient::disconnected, this, []() {
		std::print(std::cerr, "lost MQTT connection\n");
		qApp->exit(1);
	});
	connect(
		m_mqttClient, &QMqttClient::stateChanged, this,
		[](QMqttClient::ClientState state) {
			std::print(
				std::cerr, "MQTT state changed to: {}\n",
				QtEnumToString(state));
		});
	connect(
		m_mqttClient, &QMqttClient::errorChanged, this,
		[](QMqttClient::ClientError error) {
			std::print(std::cerr, "MQTT error: {}\n", QtEnumToString(error));
			qApp->exit(1);
		});

	m_mqttTopicUpdates =
		QMqttTopicName(QString("templates/") + m_settings.faction + "/updates");

	QSslConfiguration sslConfig;
	// TODO: Get a valid TLS cert for this
	sslConfig.setPeerVerifyMode(QSslSocket::PeerVerifyMode::VerifyNone);
	m_mqttClient->connectToHostEncrypted(sslConfig);
	// TODO: Wait for mqtt to fully connect before processing commits

	m_rcloneEnv = QStringList()
				  << "RCLONE_CONFIG_MINIO_TYPE=s3"
				  << "RCLONE_CONFIG_MINIO_PROVIDER=Minio"
				  << QString("RCLONE_CONFIG_MINIO_ACCESS_KEY_ID=%1")
						 .arg(m_settings.s3Url.userName())
				  << QString("RCLONE_CONFIG_MINIO_SECRET_ACCESS_KEY=%1")
						 .arg(m_settings.s3Url.password())
				  << QString("RCLONE_CONFIG_MINIO_ENDPOINT=%1")
						 .arg(m_settings.s3Url.toString(
							 QUrl::RemoveUserInfo | QUrl::RemovePassword))
				  << "RCLONE_CONFIG_MINIO_REGION=us-east-1";
}

void SessionController::onSessionEnd()
{
	// TODO: Gracefully handle losing the session. Try to reconnect, and if
	//       that fails exit and let the service handler relaunch it.
}

void SessionController::onMessagesReceived(int count, const net::Message *msgs)
{
	m_pendingMessages.isProcessingMessages = true;
	m_pendingMessages.count = count;
	m_pendingMessages.next = 0;
	m_pendingMessages.msgs = msgs;

	while(m_pendingMessages.next < m_pendingMessages.count)
		processMessage();
	enqueuePendingMessages();
	assert(
		m_pendingMessages.count == 0 &&
		"not all (or too many) messages were processed!");

	m_pendingMessages.isProcessingMessages = false;
}

void SessionController::enqueuePendingMessages()
{
	assert(
		m_pendingMessages.isProcessingMessages &&
		"processMessage outside of onMessagesReceived!");
	if(m_pendingMessages.next == 0 || m_pendingMessages.count == 0)
		return;
	// Next always holds the index of the next message, so send [0, next).
	int countToSend = m_pendingMessages.next;
	m_paintEngine->onMessagesReceived(countToSend, m_pendingMessages.msgs);
	m_pendingMessages.msgs += countToSend;
	m_pendingMessages.count -= countToSend;
	m_pendingMessages.next = 0;
}

void SessionController::enqueueCanvasSync(
	returnsVoidOrTask<drawdance::CanvasState> auto func)
{
	// Send all pending messages to the paint engine so the canvas sync happens
	// after they have all been processed.
	enqueuePendingMessages();
	SessionController *context = this;

	// The purpose of this code is to first get a CanvasState object
	// representing the canvas at this point in the message stream, then run
	// each `func` passed to this function in isolation on SessionController's
	// thread, even if func is itself a coroutine that yields back to the event
	// loop.
	//
	// To do this we need three lambdas:
	//
	// The outer lambda is called on the paint engine thread when the canvas
	// state is ready. All this does is queue a lambda on SessionController's
	// event loop.
	//
	// The middle lambda runs on SessionController's thread and starts a
	// task coroutine on the canvas history modification queue.
	//
	// The inner lambda is just there to handle passing in the CanvasState.
	m_paintEngine->enqueueCanvasSync([context, func = std::move(func)](
										 drawdance::CanvasState cs) mutable {
		// On paint engine thread
		QMetaObject::invokeMethod(
			context,
			[context, func = std::move(func), cs = std::move(cs)]() mutable {
				// Back on SessionController's thread
				context->enqueueCanvasHistory(
					[func = std::move(func), cs = std::move(cs)]() mutable {
						return func(std::move(cs));
					});
			},
			Qt::QueuedConnection);
	});
}

// TODO: This should be broken out into a separate class.
QCoro::Task<>
SessionController::enqueueCanvasHistory(returnsVoidOrTask<> auto func)
{
	m_canvasHistoryModificationQueue.push_back(
		[func = std::move(func)]() mutable -> std::optional<QCoro::Task<>> {
			// Handle both QCoro::Task<> and void returning
			// funcs.
			if constexpr(std::is_same_v<
							 std::invoke_result_t<decltype(func)>, void>) {
				func();
				return std::nullopt;
			} else
				return func();
		});

	// This can be called while another task is suspended, so just let the
	// already running task handle the task that was just added to the queue.
	if(m_working)
		co_return;

	m_working = true;
	while(!m_canvasHistoryModificationQueue.empty()) {
		// This runs the function up to the first suspend point or
		// return.
		std::optional<QCoro::Task<>> task =
			m_canvasHistoryModificationQueue.front()();
		if(task)
			co_await *task;
		// Don't destroy the lambda object until after the task is complete.
		m_canvasHistoryModificationQueue.pop_front();
	}
	m_working = false;
}

void SessionController::processMessage()
{
	assert(
		m_pendingMessages.isProcessingMessages &&
		"processMessage outside of onMessagesReceived!");
	assert(
		m_pendingMessages.count > 0 &&
		"processMessage called on an empty queue!");
	const net::Message &msg = m_pendingMessages.msgs[m_pendingMessages.next];
	++m_pendingMessages.next;
	DP_MessageType type = msg.type();
	switch(type) {
	case DP_MSG_SERVER_COMMAND: {
		if(m_isCaughtUp)
			break;
		auto reply = net::ServerReply::fromMessage(msg);
		switch(reply.type) {
		case net::ServerReply::ReplyType::Catchup:
			m_catchUpKey = reply.reply.value("key").toInt(-1);
			break;
		case net::ServerReply::ReplyType::CaughtUp:
			if(reply.reply.value("key").toInt(-1) == m_catchUpKey) {
				m_isCaughtUp = true;
				handleFullyCaughtUp();
			}
			break;
		default:
			break;
		}
		break;
	}
	case DP_MSG_JOIN:
		handleJoin(msg);
		break;
	case DP_MSG_LEAVE:
		m_userList->userLogout(msg.contextId());
		break;
	case DP_MSG_CHAT:
		handleChatMessage(msg);
		break;
	case DP_MSG_CANVAS_RESIZE: {
		enqueueCanvasSync([this, msg](const drawdance::CanvasState &) {
			// We don't want to actually resize the canvas until the next
			// commit, but we do need to record what direction the canvas
			// changed in to be able to compare correctly and inform clients.
			DP_MsgCanvasResize *resize = DP_msg_canvas_resize_cast(msg.get());
			m_previousResize.top = DP_msg_canvas_resize_top(resize);
			m_previousResize.bottom = DP_msg_canvas_resize_bottom(resize);
			m_previousResize.left = DP_msg_canvas_resize_left(resize);
			m_previousResize.right = DP_msg_canvas_resize_right(resize);
		});
		break;
	}
	default:
		break;
	}
}

struct ParsedCommand {
	canvas::User sender;
	char type{};
	std::string_view command;
	std::string_view rest;
	std::string_view fullMessage;
};

std::string_view popFront(std::string_view &sv, std::size_t count)
{
	std::string_view ret = sv.substr(0, count);
	sv.remove_prefix(count);
	return ret;
}

static std::optional<ParsedCommand>
parseCommand(const net::Message &chatMsg, canvas::UserListModel *users)
{
	if(DP_msg_chat_oflags(chatMsg.toChat()) != 0 ||
	   DP_msg_chat_tflags(chatMsg.toChat()) != 0)
		return std::nullopt;
	ParsedCommand ret;
	size_t len;
	const char *text = DP_msg_chat_message(chatMsg.toChat(), &len);
	std::string_view msg(text, len);
	ret.fullMessage = msg;
	if(!msg.starts_with('!') && !msg.starts_with('%'))
		return std::nullopt;
	ret.sender = users->getUserById(chatMsg.contextId());
	ret.type = msg[0];
	msg.remove_prefix(1);
	auto loc = msg.find(' ');
	if(loc == std::string_view::npos)
		ret.command = msg;
	else {
		ret.command = popFront(msg, loc);
		msg.remove_prefix(1);
		ret.rest = msg;
	}
	return ret;
}

void SessionController::handleChatMessage(const net::Message &chatMsg)
{
	auto maybeCmd = parseCommand(chatMsg, m_userList);
	if(!maybeCmd)
		return;
	ParsedCommand cmd = *maybeCmd;
	if(cmd.type == '!') {
		if(cmd.command == "commit") {
			if(!m_isJoinedMessagePresent)
				return;
			if(!m_isPreviousCommitConfirmed && m_isCaughtUp) {
				sendChatMessage(
					"Cannot commit while previous commit attempt is "
					"outstanding, commit was ignored.");
				return;
			}
			m_isPreviousCommitConfirmed = false;
			enqueueCanvasSync(
				[this, commitMsg = std::string(cmd.rest),
				 caughtUp = m_isCaughtUp](drawdance::CanvasState cs) mutable {
					return this->handleCommit(
						std::move(cs), std::move(commitMsg), caughtUp);
				});
		} else if(cmd.command == "reset" && m_isCaughtUp) {
			// Don't send a %reset while a commit is being processed.
			enqueueCanvasHistory([this] {
				sendChatMessage("%reset requested");
			});
		}
	} else if(cmd.type == '%') {
		if(!cmd.sender.isLocal) {
			std::println(
				std::cout, "Non-bot user sent % message: {}: {}",
				cmd.sender.name.toStdString(), cmd.fullMessage);
			std::cout.flush();
			return;
		} else if(cmd.command == "joined")
			m_isJoinedMessagePresent = true;
		else if(cmd.command == "committed") {
			m_isPreviousCommitConfirmed = true;
			if(!m_isCaughtUp)
				enqueueCanvasSync([this, rest = std::string(cmd.rest)](
									  const drawdance::CanvasState &) {
					std::ignore = std::from_chars(
						rest.data(), rest.data() + rest.size(),
						m_previousCommitId, 10);
				});
		} else if(cmd.command == "commit-ignored")
			m_isPreviousCommitConfirmed = true;
		else if(cmd.command == "reset") {
			// Resetting the previous commit state needs to happen in sync with
			// other modification to these variables which happens in the commit
			// handler driven by canvas state syncing.
			enqueueCanvasSync([this](const drawdance::CanvasState &) {
				m_previousImage = {};
				m_previousCommit = drawdance::CanvasState();
				m_previousCommitId = 0;
			});
			m_isPreviousCommitConfirmed = true;
		}
	}
}

void SessionController::handleJoin(const net::Message &msg)
{
	DP_MsgJoin *mj = DP_msg_join_cast(msg.get());
	uint8_t user_id = msg.contextId();

	size_t nameLength;
	const char *nameBytes = DP_msg_join_name(mj, &nameLength);
	QString name = QString::fromUtf8(nameBytes, compat::castSize(nameLength));

	uint8_t flags = DP_msg_join_flags(mj);
	const canvas::User u{
		user_id,
		name,
		{},
		user_id == m_myUserId,
		false,
		false,
		bool(flags & DP_MSG_JOIN_FLAGS_MOD),
		bool(flags & DP_MSG_JOIN_FLAGS_BOT),
		bool(flags & DP_MSG_JOIN_FLAGS_AUTH),
		false,
		false,
		true};

	m_userList->userLogin(u);
}

void SessionController::handleFullyCaughtUp()
{
	if(!m_isJoinedMessagePresent)
		sendChatMessage(
			"%joined Hello, I'm a bot that tracks template updates see <lol "
			"what is documentation> for how I work.");
	if(!m_isPreviousCommitConfirmed) {
		sendChatMessage(
			"%reset I crashed before confirming the previous commit. Clients "
			"are in an unknown state, restarting updates.");
	}
}

// `QCoro::Task` is `std::suspend_never` and so immediate starts running when
// called. `co_await` in this function is equivalent to splitting out to
// separate functions and calling `connect` on the previous events
QCoro::Task<> SessionController::handleCommit(
	drawdance::CanvasState cs, std::string commitMessage, bool caughtUp)
{
	// TODO: This function does way too much, the work it's doing should be
	//       split up.
	if(!caughtUp) {
		m_previousCommit = std::move(cs);
		m_previousPalette = m_palette;
		co_return;
	}

	using namespace std::chrono;
	auto commitId =
		duration_cast<seconds>(steady_clock::now().time_since_epoch()).count();

	std::println(
		std::cout, "{} Commit {} - w: {} h: {} {}", commitId, caughtUp,
		cs.width(), cs.height(), commitMessage);
	std::cout.flush();

	// Render layers on another thread to let the event loop continue.
	LayerRenderer renderer =
		co_await QCoro::detail::QCoroOnceFuture(asyncGetLayers(cs));
	std::vector<QCoro::Task<>> pendingTasks;
	auto awaitPendingTasks = [&]() -> QCoro::Task<> {
		for(auto &task : pendingTasks)
			co_await task;
	};

	for(auto &layer : renderer.m_rootLayers) {
		if(!layer.parsedTitle().is_exported || !layer.m_renderedLayer)
			continue;
		std::string name(layer.parsedTitle().name);
		name.erase(
			std::ranges::remove_if(
				name,
				[](char c) {
					return !std::isalnum(uint8_t(c));
				})
				.begin(),
			name.end());
		pendingTasks.push_back(pushLayerToCDN(
			renderer.toPixels(layer.m_renderedLayer, true, m_palette),
			QString::fromStdString(name),
			QString("endu/%1.png").arg(QString::fromStdString(name))));
	}
	// TODO: This palettizes each top level layer twice, maybe we should
	//       palettize once at each top level layer in transient layer space?
	BGRA8OffsetImage fullImg = renderer.toPixels(
		renderer.m_fullImage.m_renderedLayer, false, m_palette);

	if(m_previousImage.pixels == nullptr && !m_previousCommit.isNull()) {
		// We just rejoined a session with existing history. Render the previous
		// image now.
		LayerRenderer prevRenderer = co_await QCoro::detail::QCoroOnceFuture(
			asyncGetLayers(m_previousCommit));
		m_previousImage = prevRenderer.toPixels(
			prevRenderer.m_fullImage.m_renderedLayer, false, m_previousPalette);
	} else if(m_previousCommit.isNull()) {
		// This is the first commit after a reset, send a special message.
		// TODO: send mqtt reset message.
		m_previousCommit = std::move(cs);
		m_previousCommitId = commitId;
		m_previousImage = {};
		pendingTasks.push_back(pushLayerToCDN(
			fullImg, "full", QString("full/%1.png").arg(commitId)));
		co_await awaitPendingTasks();
		sendChatMessage(QString::fromStdString(std::format(
			"%committed {} from full image: {}", commitId, commitMessage)));
		co_return;
	}

	// TODO: Investigate using canvas diffing for this. Drawdance supports
	//       efficient diffing of different canvas states.
	if(!m_previousCommit.isNull()) {
		bool resized = false;
		if(fullImg.width != m_previousImage.width ||
		   fullImg.height != m_previousImage.height) {
			resized = true;
			BGRA8OffsetImage resizedImage(fullImg.width, fullImg.height);
			resizedImage.copyFrom(
				m_previousImage, m_previousResize.left, m_previousResize.top);
			m_previousImage = std::move(resizedImage);
		}
		DP_UPixel8 *oldImg = m_previousImage.pixels;
		DP_UPixel8 *newImg = fullImg.pixels;
		auto *diffImg = (DP_UPixel8 *)DP_malloc(fullImg.sizeInBytes());
		for(int i = 0; i < fullImg.sizeInPixels(); ++i) {
			if(oldImg[i] == newImg[i])
				diffImg[i] = {.bytes = {.b = 255, .g = 255, .r = 0, .a = 254}};
			else
				diffImg[i] = newImg[i];
		}
		BGRA8OffsetImage diff{diffImg, 0, 0, fullImg.width, fullImg.height};
		diff.crop({.bytes = {.b = 255, .g = 255, .r = 0, .a = 254}});
		if(diff.sizeInPixels() == 0 && !resized) {
			// No diff in this commit, but we still need to send a committed
			// message. The canvas state is close enough to what it was before
			// that saving a new canvas state is ok on reload.
			sendChatMessage(
				"%commit-ignored no change from previous template.");
			// Still update the previous commit so that we match the behavior on
			// reload.
			m_previousCommit = std::move(cs);
			co_return;
		}

		pendingTasks.push_back(pushLayerToCDN(
			fullImg, "full", QString("full/%1.png").arg(commitId)));

		QJsonObject obj(
			{{"type", "diff"},
			 {"previous_id", m_previousCommitId},
			 {"id", commitId},
			 {"x", diff.x},
			 {"y", diff.y},
			 {"message", QString::fromStdString(commitMessage)}});
		if(diff.sizeInBytes() != 0) {
			std::vector<char> pngBytes = diff.toPng();
			QByteArray imageBytes(
				pngBytes.data(), static_cast<int>(pngBytes.size()));
			QByteArray diffString("data:image/png;base64,");
			diffString += imageBytes.toBase64();
			obj["diff"] = QString(diffString);
		}
		if(resized) {
			QJsonObject resizeObj;
			if(m_previousResize.top != 0)
				resizeObj["top"] = m_previousResize.top;
			if(m_previousResize.bottom != 0)
				resizeObj["bottom"] = m_previousResize.bottom;
			if(m_previousResize.left != 0)
				resizeObj["left"] = m_previousResize.left;
			if(m_previousResize.right != 0)
				resizeObj["right"] = m_previousResize.right;
			obj["resize"] = std::move(resizeObj);
		}
		QByteArray mqttUpdate = QJsonDocument(obj).toJson();
		pushLayerToCDN(diff, "diff", QString());
		m_mqttClient->publish(m_mqttTopicUpdates, mqttUpdate, 1, true);
		// TODO: Don't fully confirm commits until we get a
		//       QMqttClient::messageSent signal that says the broker got it.
		co_await awaitPendingTasks();
		m_previousCommitId = commitId;
		m_previousImage = std::move(fullImg);
		m_previousCommit = std::move(cs);
		std::string msg = std::format(
			"%committed {} from diff offsetX: {} offsetY: {} width: {} height: "
			"{} message: {}",
			commitId, diff.x, diff.y, diff.width, diff.height, commitMessage);
		sendChatMessage(QString::fromStdString(msg));
	}
}

QFuture<LayerRenderer>
SessionController::asyncGetLayers(drawdance::CanvasState cs)
{
	return QtConcurrent::run([this, cs = std::move(cs)]() mutable {
		LayerRenderer renderer(std::move(cs));
		renderer.render();
		return renderer;
	});
}

QCoro::Task<> SessionController::pushLayerToCDN(
	const BGRA8OffsetImage &image, QString name, QString cdnPath)
{
	DP_UPixel8 *pixels = image.pixels;
	if(!pixels)
		co_return;

	std::string where = std::format(
		"{}/{}.png", m_settings.outputPath.toStdString(), name.toStdString());
	DP_Image *img = DP_image_new(image.width, image.height);
	DP_Output *out = DP_file_output_new_from_path(where.c_str());
	if(!out) {
		std::cerr << "Bad output path: " << where.c_str() << std::endl;
		co_return;
	}
	memcpy(DP_image_pixels(img), pixels, image.sizeInBytes());
	DP_image_write_png(img, out);
	DP_output_free(out);
	DP_image_free(img);

	if(cdnPath.isEmpty())
		co_return;

	QProcess rclone;
	rclone.setEnvironment(m_rcloneEnv);
	rclone.start(
		"rclone", QStringList()
					  << "copyto" << "-c" << QString::fromStdString(where)
					  << QString("minio:templates/%1/%2")
							 .arg(m_settings.faction, cdnPath));
	co_await qCoro(rclone).waitForFinished();
	// TODO: Handle upload failure.
}
