#ifndef DRAWDANCE_SNAPSHOT_QUEUE_H
#define DRAWDANCE_SNAPSHOT_QUEUE_H

extern "C" {
#include <dpengine/snapshots.h>
}

#include <QtGlobal>
#include <functional>

namespace drawdance {

class SnapshotQueue {
public:
    using SnapshotAtFn = std::function<DP_Snapshot * (int index)>;
    using GetSnapshotsFn = std::function<void (size_t count, SnapshotAtFn at)>;

    SnapshotQueue(size_t maxCount, long long minDelayMs);

    ~SnapshotQueue();

    SnapshotQueue(const SnapshotQueue &) = delete;
    SnapshotQueue(SnapshotQueue &&) = delete;
    SnapshotQueue &operator=(const SnapshotQueue &) = delete;
    SnapshotQueue &operator=(SnapshotQueue &&) = delete;

    DP_SnapshotQueue *get();

    void getSnapshotsWith(GetSnapshotsFn get) const;

private:
    static long long getTimestampMs(void *user);

    static void getSnapshots(
        void *user, DP_SnapshotQueue *sq, size_t count, DP_SnapshotAtFn at);

    DP_SnapshotQueue *m_data;
};

}

#endif
