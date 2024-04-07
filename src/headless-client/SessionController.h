// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_CLIENT_SESSION_CONTROLLER_H
#define HEADLESS_CLIENT_SESSION_CONTROLLER_H

#include "PaintEngine.h"
#include "libclient/canvas/userlist.h"
#include "libclient/net/message.h"
#include <QCoroTask>
#include <QFuture>
#include <QMqttClient>
#include <QUrl>
#include <memory>

struct SessionSettings {
	bool verbose = false;
	QString outputPath;
	QString faction;
	QUrl mqttUrl;
	QUrl s3Url;
};

// This class is responsible for connecting and driving all the high level bits
// of managing template updates.
class SessionController : public QObject {
	Q_OBJECT
public:
	explicit SessionController(
		SessionSettings settings, QObject *parent = nullptr);
	void sendChatMessage(QString msg);

public slots:
	void onSessionStart(int myUserId);
	void onSessionEnd();
	void onMessagesReceived(int count, const net::Message *msgs);

signals:
	// This is emitted either when a chat message requests that the client
	// reconnects or when the session controller is in an unrecoverable state.
	void endSession();

	void sendMessage(const net::Message &msg);

private:
	// These functions are all called as part of `onMessagesReceived`. We need
	// to be able to inject messages into the messages that get sent to the
	// paint engine, so the message queue is kept track of as class state, and
	// `enqueuePendingMessage` is used to send all messages, including the
	// current message, to the paint engine.
	void processMessage();
	void enqueuePendingMessages();
	void enqueueCanvasSync(std::function<void(drawdance::CanvasState)> func);
	void handleChatMessage(const net::Message &chatMsg);
	void handleJoin(const net::Message &msg);
	void handleFullyCaughtUp();
	// end `onMessagesReceived` helpers.

	QCoro::Task<>
	pushLayerToCDN(const BGRA8OffsetImage &img, QString name, QString cdnPath);
	QCoro::Task<> handleCommit(
		drawdance::CanvasState cs, std::string commitMessage, bool caughtUp);
	QFuture<LayerRenderer> asyncGetLayers(drawdance::CanvasState cs);

	// TODO: Make it clearer which of these variables only makes sense in canvas
	//       sync order, and which are only used during `onMessagesReceived`.
	SessionSettings m_settings;
	canvas::UserListModel *m_userList;
	QMqttClient *m_mqttClient = nullptr;
	QMqttTopicName m_mqttTopicUpdates;
	QStringList m_rcloneEnv;
	std::unique_ptr<PaintEngine> m_paintEngine;
	int m_myUserId = -1;
	int m_catchUpKey = -1;
	bool m_isCaughtUp = false;
	struct {
		bool isProcessingMessages = false;
		int next = 0;
		int count = 0;
		const net::Message *msgs = nullptr;
	} m_pendingMessages;
	std::vector<DP_UPixel8> m_palette;

	// Change tracking
	bool m_isJoinedMessagePresent = false;
	bool m_isPreviousCommitConfirmed = true;
	drawdance::CanvasState m_previousCommit;
	std::vector<DP_UPixel8> m_previousPalette;
	BGRA8OffsetImage m_previousImage{nullptr, 0, 0, 0, 0};
	int64_t m_previousCommitId = 0;
};

#endif // HEADLESS_CLIENT_SESSION_CONTROLLER_H
