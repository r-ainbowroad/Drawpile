#ifndef HEADLESS_CLIENT_PAINT_ENGINE_H
#define HEADLESS_CLIENT_PAINT_ENGINE_H

#include "LayerRenderer.h"
#include "libclient/drawdance/aclstate.h"
#include "libclient/drawdance/paintengine.h"
#include "libclient/drawdance/snapshotqueue.h"
#include <deque>
#include <functional>
#include <mutex>

class PaintEngine {
public:
	using SyncCallback = std::function<void(drawdance::CanvasState)>;

	PaintEngine();
	~PaintEngine();

	int onMessagesReceived(int count, const net::Message *msgs);
	void enqueueCanvasSync(SyncCallback callback);
	static void onSyncCanvasState(void *user, DP_CanvasState *cs);

	// These are not actually used, but need to exist to pass to the underlying
	// drawdance paint engine.
	static void
	onRenderTile(void *user, int tileX, int tileY, DP_Pixel8 *pixels);
	static void onRenderUnlock(void *user);
	static void onRenderResize(
		void *user, int width, int height, int prevWidth, int prevHeight,
		int offsetX, int offsetY);
	static void onPlayback(void *user, long long position);
	static void onDumpPlayback(
		void *user, long long position, DP_CanvasHistorySnapshot *chs);

private:
	drawdance::AclState m_aclState;
	drawdance::SnapshotQueue m_snapshotQueue;
	drawdance::PaintEngine m_paintEngine;
	std::mutex m_syncRequestsMutex;
	std::deque<SyncCallback> m_syncRequests;
};

#endif // HEADLESS_CLIENT_PAINT_ENGINE_H
