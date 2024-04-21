#include "PaintEngine.h"
#include "LayerRenderer.h"
#include "libclient/drawdance/image.h"
#include <span>
extern "C" {
#include <dpcommon/output.h>
#include <dpengine/image.h>
#include <dpengine/layer_content.h>
#include <dpengine/layer_group.h>
#include <dpengine/layer_list.h>
#include <dpmsg/blend_mode.h>
}

PaintEngine::PaintEngine()
	: m_snapshotQueue(20, 2000)
	, m_paintEngine(
		  m_aclState, m_snapshotQueue, false, false, QColor(), QColor(),
		  PaintEngine::onRenderTile, PaintEngine::onRenderUnlock,
		  PaintEngine::onRenderResize, this, nullptr, this,
		  PaintEngine::onPlayback, PaintEngine::onDumpPlayback, this,
		  drawdance::CanvasState::null(), PaintEngine::onSyncCanvasState, this)
{
	net::Message msg = net::makeInternalResetMessage(0);
	onMessagesReceived(1, &msg);
}

PaintEngine::~PaintEngine() = default;

int PaintEngine::onMessagesReceived(int count, const net::Message *msgs)
{
	int ret = DP_paint_engine_handle_inc(
		m_paintEngine.get(), false, true, count,
		net::Message::asRawMessages(msgs),
		+[](void *user [[maybe_unused]], int aclChangeFlags [[maybe_unused]]) {
		},
		+[](void *user [[maybe_unused]],
			unsigned int contextId [[maybe_unused]],
			int persistence [[maybe_unused]], uint32_t color [[maybe_unused]]) {
		},
		+[](void *user [[maybe_unused]],
			unsigned int contextId [[maybe_unused]], int x [[maybe_unused]],
			int y [[maybe_unused]]) {
		},
		this);
	return ret;
}

void PaintEngine::enqueueCanvasSync(SyncCallback callback)
{
	std::unique_lock _(m_syncRequestsMutex);
	m_syncRequests.push_back(std::move(callback));
	_.unlock();
	net::Message msg = net::makeInternalSyncCanvasStateMessage();
	onMessagesReceived(1, &msg);
}

// NOTE: This is called from the paint engine thread.
void PaintEngine::onSyncCanvasState(void *user, DP_CanvasState *cs)
{
	auto *self = static_cast<PaintEngine *>(user);
	std::unique_lock _(self->m_syncRequestsMutex);
	assert(
		!self->m_syncRequests.empty() &&
		"got sync event with no registered sync request!");
	auto fn = std::move(self->m_syncRequests.front());
	self->m_syncRequests.pop_front();
	_.unlock();
	fn(drawdance::CanvasState::noinc(cs));
}

void PaintEngine::onRenderTile(void *, int, int, DP_Pixel8 *) {}

void PaintEngine::onRenderUnlock(void *) {}

void PaintEngine::onRenderResize(void *, int, int, int, int, int, int) {}

void PaintEngine::onPlayback(void *, long long int) {}

void PaintEngine::onDumpPlayback(
	void *, long long int, DP_CanvasHistorySnapshot *)
{
}
