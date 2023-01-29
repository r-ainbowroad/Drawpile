/*
 * Copyright (C) 2022 - 2023 askmeaboutloom
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --------------------------------------------------------------------
 *
 * This code is based on Drawpile, using it under the GNU General Public
 * License, version 3. See 3rdparty/licenses/drawpile/COPYING for details.
 */
#include "paint_engine.h"
#include "canvas_diff.h"
#include "canvas_history.h"
#include "canvas_state.h"
#include "draw_context.h"
#include "layer_content.h"
#include "layer_group.h"
#include "layer_list.h"
#include "layer_props.h"
#include "layer_props_list.h"
#include "layer_routes.h"
#include "paint.h"
#include "player.h"
#include "recorder.h"
#include "tile.h"
#include "timeline.h"
#include "view_mode.h"
#include <dpcommon/atomic.h>
#include <dpcommon/common.h>
#include <dpcommon/conversions.h>
#include <dpcommon/cpu.h>
#include <dpcommon/file.h>
#include <dpcommon/output.h>
#include <dpcommon/perf.h>
#include <dpcommon/queue.h>
#include <dpcommon/threading.h>
#include <dpcommon/vector.h>
#include <dpcommon/worker.h>
#include <dpmsg/acl.h>
#include <dpmsg/blend_mode.h>
#include <dpmsg/message.h>
#include <dpmsg/message_queue.h>
#include <dpmsg/msg_internal.h>
#include <ctype.h>
#include <limits.h>

#define DP_PERF_CONTEXT "paint_engine"


#define INITIAL_QUEUE_CAPACITY 64

#define PREVIEW_SUBLAYER_ID -100
#define INSPECT_SUBLAYER_ID -200

#define RECORDER_UNCHANGED 0
#define RECORDER_STARTED   1
#define RECORDER_STOPPED   2

typedef struct DP_PaintEnginePreview DP_PaintEnginePreview;
typedef DP_CanvasState *(*DP_PaintEnginePreviewRenderFn)(
    DP_PaintEnginePreview *preview, DP_CanvasState *cs, DP_DrawContext *dc,
    int offset_x, int offset_y);
typedef void (*DP_PaintEnginePreviewDisposeFn)(DP_PaintEnginePreview *preview);

struct DP_PaintEnginePreview {
    int initial_offset_x, initial_offset_y;
    DP_PaintEnginePreviewRenderFn render;
    DP_PaintEnginePreviewDisposeFn dispose;
};

static DP_PaintEnginePreview null_preview;

typedef struct DP_PaintEngineCutPreview {
    DP_PaintEnginePreview parent;
    DP_LayerContent *lc;
    DP_LayerProps *lp;
    int layer_id;
    int x, y;
    int width, height;
    bool have_mask;
    uint8_t mask[];
} DP_PaintEngineCutPreview;

typedef struct DP_PaintEngineDabsPreview {
    DP_PaintEnginePreview parent;
    int layer_id;
    int count;
    DP_Message *messages[];
} DP_PaintEngineDabsPreview;

typedef struct DP_PaintEngineLaserBuffer {
    uint8_t persistence;
    uint8_t b, g, r, a;
} DP_PaintEngineLaserBuffer;

typedef struct DP_PaintEngineLaserChanges {
    uint8_t count;
    uint8_t users[256];
    bool active[256];
    DP_PaintEngineLaserBuffer buffers[256];
} DP_PaintEngineLaserChanges;

typedef struct DP_PaintEngineCursorPosition {
    int32_t x, y;
} DP_PaintEngineCursorPosition;

typedef struct DP_PaintEngineCursorChanges {
    uint8_t count;
    uint8_t users[256];
    bool active[256];
    DP_PaintEngineCursorPosition positions[256];
} DP_PaintEngineCursorChanges;

typedef struct DP_PaintEngineMetaBuffer {
    uint8_t acl_change_flags;
    DP_PaintEngineLaserChanges laser_changes;
    DP_PaintEngineCursorChanges cursor_changes;
} DP_PaintEngineMetaBuffer;

struct DP_PaintEngine {
    unsigned char *buffers;
    DP_AclState *acls;
    DP_CanvasHistory *ch;
    DP_CanvasDiff *diff;
    DP_TransientLayerContent *tlc;
    DP_Tile *checker;
    DP_CanvasState *history_cs;
    DP_CanvasState *view_cs;
    struct {
        int active_layer_id;
        int active_frame_index;
        DP_ViewMode view_mode;
        const DP_OnionSkins *onion_skins;
        bool reveal_censored;
        unsigned int inspect_context_id;
        DP_Vector hidden_layers;
        DP_LayerPropsList *prev_lpl;
        DP_Timeline *prev_tl;
        DP_LayerPropsList *lpl;
        DP_Tile *background_tile;
        bool background_opaque;
        bool layers_can_decrease_opacity;
    } local_view;
    DP_DrawContext *paint_dc;
    DP_PaintEnginePreview *preview;
    DP_DrawContext *preview_dc;
    DP_Queue local_queue;
    DP_Queue remote_queue;
    DP_Semaphore *queue_sem;
    DP_Mutex *queue_mutex;
    DP_Atomic running;
    DP_Atomic catchup;
    DP_Atomic default_layer_id;
    DP_AtomicPtr next_preview;
    DP_Thread *paint_thread;
    struct {
        char *path;
        DP_Recorder *recorder;
        DP_Semaphore *start_sem;
        int state_change;
        DP_RecorderGetTimeMsFn get_time_ms_fn;
        void *get_time_ms_user;
    } record;
    struct {
        DP_Player *player;
        DP_PaintEnginePlaybackFn fn;
        DP_PaintEngineDumpPlaybackFn dump_fn;
        void *user;
    } playback;
    struct {
        DP_Worker *worker;
        DP_Semaphore *tiles_done_sem;
        int tiles_waiting;
    } render;
};

struct DP_PaintEngineRenderParams {
    DP_PaintEngine *pe;
    int xtiles;
    bool needs_checkers;
    DP_PaintEngineRenderTileFn render_tile;
    void *user;
};

struct DP_PaintEngineRenderJobParams {
    struct DP_PaintEngineRenderParams *render_params;
    int x, y;
};


static void push_cleanup_message(void *user, DP_Message *msg)
{
    DP_PaintEngine *pe = user;
    DP_message_queue_push_noinc(&pe->remote_queue, msg);
    DP_SEMAPHORE_MUST_POST(pe->queue_sem);
}

static void free_preview(DP_PaintEnginePreview *preview)
{
    if (preview && preview != &null_preview) {
        preview->dispose(preview);
        DP_free(preview);
    }
}

static void decref_messages(int count, DP_Message **msgs)
{
    for (int i = 0; i < count; ++i) {
        DP_message_decref(msgs[i]);
    }
}

static void drop_message(DP_UNUSED void *user, DP_Message *msg)
{
    DP_message_decref(msg);
}

static void handle_dump_command(DP_PaintEngine *pe, DP_MsgInternal *mi)
{
    DP_CanvasHistory *ch = pe->ch;
    DP_DrawContext *dc = pe->paint_dc;
    int count;
    DP_Message **msgs = DP_msg_internal_dump_command_messages(mi, &count);
    int type = DP_msg_internal_dump_command_type(mi);
    switch (type) {
    case DP_DUMP_REMOTE_MESSAGE:
        DP_canvas_history_local_drawing_in_progress_set(ch, false);
        if (!DP_canvas_history_handle(ch, dc, msgs[0])) {
            DP_warn("Error handling remote dump message: %s", DP_error());
        }
        decref_messages(count, msgs);
        break;
    case DP_DUMP_REMOTE_MESSAGE_LOCAL_DRAWING_IN_PROGRESS:
        DP_canvas_history_local_drawing_in_progress_set(ch, true);
        if (!DP_canvas_history_handle(ch, dc, msgs[0])) {
            DP_warn("Error handling remote dump message with local drawing in "
                    "progress: %s",
                    DP_error());
        }
        decref_messages(count, msgs);
        break;
    case DP_DUMP_LOCAL_MESSAGE:
        if (!DP_canvas_history_handle_local(ch, dc, msgs[0])) {
            DP_warn("Error handling local dump message: %s", DP_error());
        }
        decref_messages(count, msgs);
        break;
    case DP_DUMP_REMOTE_MULTIDAB:
        DP_canvas_history_local_drawing_in_progress_set(ch, false);
        DP_canvas_history_handle_multidab_dec(ch, dc, count, msgs);
        break;
    case DP_DUMP_REMOTE_MULTIDAB_LOCAL_DRAWING_IN_PROGRESS:
        DP_canvas_history_local_drawing_in_progress_set(ch, true);
        DP_canvas_history_handle_multidab_dec(ch, dc, count, msgs);
        break;
    case DP_DUMP_LOCAL_MULTIDAB:
        DP_canvas_history_handle_local_multidab_dec(ch, dc, count, msgs);
        break;
    case DP_DUMP_RESET:
        decref_messages(count, msgs);
        DP_canvas_history_reset(ch);
        break;
    case DP_DUMP_SOFT_RESET:
        decref_messages(count, msgs);
        DP_canvas_history_soft_reset(ch);
        break;
    case DP_DUMP_CLEANUP:
        decref_messages(count, msgs);
        DP_canvas_history_cleanup(ch, dc, drop_message, NULL);
        break;
    default:
        decref_messages(count, msgs);
        DP_warn("Unknown dump entry type %d", type);
        break;
    }
}

static void handle_internal(DP_PaintEngine *pe, DP_DrawContext *dc,
                            DP_MsgInternal *mi)
{
    DP_MsgInternalType type = DP_msg_internal_type(mi);
    switch (type) {
    case DP_MSG_INTERNAL_TYPE_RESET:
        DP_canvas_history_reset(pe->ch);
        break;
    case DP_MSG_INTERNAL_TYPE_SOFT_RESET:
        DP_canvas_history_soft_reset(pe->ch);
        break;
    case DP_MSG_INTERNAL_TYPE_RESET_TO_STATE:
        DP_canvas_history_reset_to_state_noinc(
            pe->ch, DP_msg_internal_reset_to_state_data(mi));
        break;
    case DP_MSG_INTERNAL_TYPE_SNAPSHOT:
        if (!DP_canvas_history_snapshot(pe->ch)) {
            DP_warn("Error requesting snapshot: %s", DP_error());
        }
        break;
    case DP_MSG_INTERNAL_TYPE_CATCHUP:
        DP_atomic_set(&pe->catchup, DP_msg_internal_catchup_progress(mi));
        break;
    case DP_MSG_INTERNAL_TYPE_CLEANUP:
        DP_MUTEX_MUST_LOCK(pe->queue_mutex);
        DP_canvas_history_cleanup(pe->ch, dc, push_cleanup_message, pe);
        while (pe->local_queue.used != 0) {
            DP_message_queue_push_noinc(
                &pe->remote_queue, DP_message_queue_shift(&pe->local_queue));
        }
        DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);
        break;
    case DP_MSG_INTERNAL_TYPE_PREVIEW:
        free_preview(DP_atomic_ptr_xch(&pe->next_preview,
                                       DP_msg_internal_preview_data(mi)));
        break;
    case DP_MSG_INTERNAL_TYPE_RECORDER_START:
        DP_SEMAPHORE_MUST_POST(pe->record.start_sem);
        break;
    case DP_MSG_INTERNAL_TYPE_PLAYBACK: {
        DP_PaintEnginePlaybackFn playback_fn = pe->playback.fn;
        if (playback_fn) {
            playback_fn(pe->playback.user,
                        DP_msg_internal_playback_position(mi),
                        DP_msg_internal_playback_interval(mi));
        }
        break;
    }
    case DP_MSG_INTERNAL_TYPE_DUMP_PLAYBACK: {
        DP_PaintEngineDumpPlaybackFn dump_playback_fn = pe->playback.dump_fn;
        if (dump_playback_fn) {
            DP_CanvasHistorySnapshot *chs =
                DP_canvas_history_snapshot_new(pe->ch);
            dump_playback_fn(pe->playback.user,
                             DP_msg_internal_dump_playback_position(mi), chs);
            DP_canvas_history_snapshot_decref(chs);
        }
        break;
    }
    case DP_MSG_INTERNAL_TYPE_DUMP_COMMAND:
        handle_dump_command(pe, mi);
        break;
    default:
        DP_warn("Unhandled internal message type %d", (int)type);
        break;
    }
}


// Since draw dabs messages are so common and come in bunches, we have special
// handling to deal with them in batches. That makes the code more complicated,
// but it gives significantly better performance, so it's worth it in the end.

// These limits are static and arbitrary. They could surely be improved
// by making them dynamic or at least measuring what good limits are.

// Maximum number of multidab messages in a single go.
#define MAX_MULTIDAB_MESSAGES 1024
// Largest area that all dabs together are allowed to cover.
#define MAX_MULTIDAB_AREA (256 * 256 * 16)
// Threshold that the first message must not exceed to keep shifting more
// messages. Presumably if the first message reaches half of our limit, the next
// message is likely to push us over it, so we don't even need to try.
#define MAX_MULTIDAB_AREA_THRESHOLD (MAX_MULTIDAB_AREA / 2)

static bool shift_first_message(DP_PaintEngine *pe, DP_Message **msgs)
{
    // Local queue takes priority, we want our own strokes to be responsive.
    DP_Message *msg = DP_message_queue_shift(&pe->local_queue);
    if (msg) {
        msgs[0] = msg;
        return true;
    }
    else {
        msgs[0] = DP_message_queue_shift(&pe->remote_queue);
        return false;
    }
}

static int get_classic_dabs_area(DP_MsgDrawDabsClassic *mddc, int dabs_area)
{
    int count;
    const DP_ClassicDab *cds = DP_msg_draw_dabs_classic_dabs(mddc, &count);
    for (int i = 0; i < count && dabs_area < MAX_MULTIDAB_AREA; ++i) {
        int radius = DP_classic_dab_size(DP_classic_dab_at(cds, i)) / 256;
        int diameter = radius * 2;
        int area = DP_max_int(1, diameter * diameter);
        dabs_area += area;
    }
    return dabs_area;
}

static int get_pixel_dabs_area(DP_MsgDrawDabsPixel *mddp, int dabs_area)
{
    int count;
    const DP_PixelDab *pds = DP_msg_draw_dabs_pixel_dabs(mddp, &count);
    for (int i = 0; i < count && dabs_area < MAX_MULTIDAB_AREA; ++i) {
        int radius = DP_pixel_dab_size(DP_pixel_dab_at(pds, i));
        int diameter = radius * 2;
        int area = DP_max_int(1, diameter * diameter);
        dabs_area += area;
    }
    return dabs_area;
}

static int get_mypaint_dabs_area(DP_MsgDrawDabsMyPaint *mddmp, int dabs_area)
{
    int count;
    const DP_MyPaintDab *mpds = DP_msg_draw_dabs_mypaint_dabs(mddmp, &count);
    for (int i = 0; i < count && dabs_area < MAX_MULTIDAB_AREA; ++i) {
        // FIXME: size is supposed to be the radius, not the diameter. I think.
        // But currently, the painting code makes this mistake as well, so this
        // is the correct way of counting the dab size.
        int diameter = DP_mypaint_dab_size(DP_mypaint_dab_at(mpds, i)) / 256;
        int area = DP_max_int(1, diameter * diameter);
        dabs_area += area;
    }
    return dabs_area;
}

static int get_dabs_area(DP_Message *msg, DP_MessageType type, int dabs_area)
{
    switch (type) {
    case DP_MSG_DRAW_DABS_CLASSIC:
        return get_classic_dabs_area(DP_message_internal(msg), dabs_area);
    case DP_MSG_DRAW_DABS_PIXEL:
    case DP_MSG_DRAW_DABS_PIXEL_SQUARE:
        return get_pixel_dabs_area(DP_message_internal(msg), dabs_area);
    case DP_MSG_DRAW_DABS_MYPAINT:
        return get_mypaint_dabs_area(DP_message_internal(msg), dabs_area);
    default:
        return MAX_MULTIDAB_AREA + 1;
    }
}

static int shift_more_draw_dabs_messages(DP_PaintEngine *pe, bool local,
                                         DP_Message **msgs,
                                         int initial_dabs_area)
{
    int count = 1;
    int total_dabs_area = initial_dabs_area;
    DP_Queue *queue = local ? &pe->local_queue : &pe->remote_queue;

    DP_Message *msg;
    while (count < MAX_MULTIDAB_MESSAGES
           && (msg = DP_message_queue_peek(queue))) {
        total_dabs_area =
            get_dabs_area(msg, DP_message_type(msg), total_dabs_area);
        if (total_dabs_area <= MAX_MULTIDAB_AREA) {
            DP_queue_shift(queue);
            msgs[count++] = msg;
        }
        else {
            break;
        }
    }

    if (count > 1) {
        int n = count - 1;
        DP_ASSERT(DP_semaphore_value(pe->queue_sem) >= n);
        DP_SEMAPHORE_MUST_WAIT_N(pe->queue_sem, n);
    }

    return count;
}

static int maybe_shift_more_messages(DP_PaintEngine *pe, bool local,
                                     DP_MessageType type, DP_Message **msgs)
{
    int dabs_area = get_dabs_area(msgs[0], type, 0);
    if (dabs_area <= MAX_MULTIDAB_AREA_THRESHOLD) {
        return shift_more_draw_dabs_messages(pe, local, msgs, dabs_area);
    }
    else {
        return 1;
    }
}

static void handle_single_message(DP_PaintEngine *pe, DP_DrawContext *dc,
                                  bool local, DP_MessageType type,
                                  DP_Message *msg)
{
    if (type == DP_MSG_INTERNAL) {
        handle_internal(pe, dc, DP_msg_internal_cast(msg));
    }
    else if (type == DP_MSG_DEFAULT_LAYER) {
        DP_MsgDefaultLayer *mdl = DP_message_internal(msg);
        int default_layer_id = DP_msg_default_layer_id(mdl);
        DP_atomic_xch(&pe->default_layer_id, default_layer_id);
    }
    else if (local) {
        if (!DP_canvas_history_handle_local(pe->ch, dc, msg)) {
            DP_warn("Handle local command: %s", DP_error());
        }
    }
    else {
        if (!DP_canvas_history_handle(pe->ch, dc, msg)) {
            DP_warn("Handle remote command: %s", DP_error());
        }
    }
    DP_message_decref(msg);
}

static void handle_multidab(DP_PaintEngine *pe, DP_DrawContext *dc, bool local,
                            int count, DP_Message **msgs)
{
    if (local) {
        DP_canvas_history_handle_local_multidab_dec(pe->ch, dc, count, msgs);
    }
    else {
        DP_canvas_history_handle_multidab_dec(pe->ch, dc, count, msgs);
    }
}

static void handle_message(DP_PaintEngine *pe, DP_DrawContext *dc,
                           DP_Message **msgs)
{
    DP_MUTEX_MUST_LOCK(pe->queue_mutex);
    bool local = shift_first_message(pe, msgs);
    DP_Message *first = msgs[0];
    DP_MessageType type = DP_message_type(first);
    int count = maybe_shift_more_messages(pe, local, type, msgs);
    DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);

    DP_ASSERT(count > 0);
    DP_ASSERT(count <= MAX_MULTIDAB_MESSAGES);
    if (count == 1) {
        handle_single_message(pe, dc, local, type, first);
    }
    else {
        handle_multidab(pe, dc, local, count, msgs);
    }
}

static void run_paint_engine(void *user)
{
    DP_PaintEngine *pe = user;
    DP_DrawContext *dc = pe->paint_dc;
    DP_Semaphore *sem = pe->queue_sem;
    DP_Message **msgs = DP_malloc(sizeof(*msgs) * MAX_MULTIDAB_MESSAGES);
    while (true) {
        DP_SEMAPHORE_MUST_WAIT(sem);
        if (DP_atomic_get(&pe->running)) {
            handle_message(pe, dc, msgs);
        }
        else {
            break;
        }
    }
    DP_free(msgs);
}


static void flatten_onion_skin(DP_TransientTile *tt, DP_CanvasState *cs,
                               int tile_index, int frame_index,
                               const DP_OnionSkin *os)
{
    DP_ViewModeFilter vmf = DP_view_mode_filter_make_frame(cs, frame_index);
    uint16_t opacity = os->opacity;
    if (!DP_view_mode_filter_excludes_everything(&vmf) && opacity != 0) {
        DP_TransientTile *skin_tt = DP_canvas_state_flatten_tile(
            cs, tile_index, DP_FLAT_IMAGE_RENDER_ONION_SKIN_FLAGS, &vmf);

        DP_UPixel15 tint = os->tint;
        if (tint.a != 0) {
            DP_transient_tile_brush_apply(skin_tt, tint, DP_BLEND_MODE_RECOLOR,
                                          DP_tile_opaque_mask(), tint.a, 0, 0,
                                          DP_TILE_SIZE, DP_TILE_SIZE, 0);
        }

        DP_transient_tile_merge(tt, (DP_Tile *)skin_tt, opacity,
                                DP_BLEND_MODE_NORMAL);
        DP_transient_tile_decref(skin_tt);
    }
}

static DP_TransientTile *flatten_tile(DP_PaintEngine *pe, bool needs_checkers,
                                      int tile_index)
{
    DP_CanvasState *cs = pe->view_cs;
    DP_TransientTile *tt = DP_transient_tile_new_nullable(
        DP_canvas_state_background_tile_noinc(cs), 0);

    DP_ViewMode vm = pe->local_view.view_mode;
    const DP_OnionSkins *oss = pe->local_view.onion_skins;
    int active_frame_index = pe->local_view.active_frame_index;
    if (vm == DP_VIEW_MODE_FRAME && oss) {
        int count_below = DP_onion_skins_count_below(oss);
        for (int i = 0; i < count_below; ++i) {
            int skin_frame_index = active_frame_index - count_below + i;
            flatten_onion_skin(tt, cs, tile_index, skin_frame_index,
                               DP_onion_skins_skin_below_at(oss, i));
        }

        int count_above = DP_onion_skins_count_above(oss);
        for (int i = 0; i < count_above; ++i) {
            int skin_frame_index = active_frame_index + i + 1;
            flatten_onion_skin(tt, cs, tile_index, skin_frame_index,
                               DP_onion_skins_skin_above_at(oss, i));
        }
    }

    DP_ViewModeFilter vmf = DP_view_mode_filter_make(
        vm, cs, pe->local_view.active_layer_id, active_frame_index);
    DP_layer_list_flatten_tile_to(DP_canvas_state_layers_noinc(cs),
                                  DP_canvas_state_layer_props_noinc(cs),
                                  tile_index, tt, DP_BIT15, true, &vmf);

    if (needs_checkers) {
        DP_transient_tile_merge(tt, pe->checker, DP_BIT15,
                                DP_BLEND_MODE_BEHIND);
    }
    DP_transient_layer_content_transient_tile_set_noinc(pe->tlc, tt,
                                                        tile_index);
    return tt;
}

static void render_job(void *user, int thread_index)
{
    struct DP_PaintEngineRenderJobParams *job_params = user;
    struct DP_PaintEngineRenderParams *render_params =
        job_params->render_params;
    int x = job_params->x;
    int y = job_params->y;

    DP_PaintEngine *pe = render_params->pe;
    int tile_index = y * render_params->xtiles + x;
    DP_TransientTile *tt =
        flatten_tile(pe, render_params->needs_checkers, tile_index);

    DP_Pixel8 *pixel_buffer =
        ((DP_Pixel8 *)pe->buffers) + DP_TILE_LENGTH * thread_index;
    DP_pixels15_to_8_tile(pixel_buffer, DP_transient_tile_pixels(tt));

    render_params->render_tile(render_params->user, x, y, pixel_buffer,
                               thread_index);

    DP_SEMAPHORE_MUST_POST(pe->render.tiles_done_sem);
}

static void apply_hidden_layers_recursive(DP_Vector *hidden_layers,
                                          DP_LayerPropsList *lpl)
{
    int count = DP_layer_props_list_count(lpl);
    for (int i = 0; i < count; ++i) {
        DP_LayerProps *lp = DP_layer_props_list_at_noinc(lpl, i);
        if (DP_layer_props_hidden(lp)) {
            int layer_id = DP_layer_props_id(lp);
            DP_VECTOR_PUSH_TYPE(hidden_layers, int, layer_id);
        }
        DP_LayerPropsList *child_lpl = DP_layer_props_children_noinc(lp);
        if (child_lpl) {
            apply_hidden_layers_recursive(hidden_layers, child_lpl);
        }
    }
}

DP_PaintEngine *DP_paint_engine_new_inc(
    DP_DrawContext *paint_dc, DP_DrawContext *preview_dc, DP_AclState *acls,
    DP_CanvasState *cs_or_null, DP_CanvasHistorySavePointFn save_point_fn,
    void *save_point_user, bool want_canvas_history_dump,
    const char *canvas_history_dump_dir, DP_RecorderGetTimeMsFn get_time_ms_fn,
    void *get_time_ms_user, DP_Player *player_or_null,
    DP_PaintEnginePlaybackFn playback_fn,
    DP_PaintEngineDumpPlaybackFn dump_playback_fn, void *playback_user)
{
    DP_PaintEngine *pe = DP_malloc(sizeof(*pe));

    int render_thread_count = DP_thread_cpu_count();
    size_t buffers_size =
        DP_max_size(sizeof(DP_PaintEngineLaserBuffer),
                    sizeof(DP_Pixel8[DP_TILE_LENGTH])
                        * DP_int_to_size(render_thread_count));
    pe->buffers = DP_malloc_simd(buffers_size);
    pe->acls = acls;
    pe->ch = DP_canvas_history_new_inc(
        cs_or_null, save_point_fn, save_point_user, want_canvas_history_dump,
        canvas_history_dump_dir);
    pe->diff = DP_canvas_diff_new();
    pe->tlc = DP_transient_layer_content_new_init(0, 0, NULL);
    pe->checker = DP_tile_new_checker(
        0, (DP_Pixel15){DP_BIT15 / 2, DP_BIT15 / 2, DP_BIT15 / 2, DP_BIT15},
        (DP_Pixel15){DP_BIT15, DP_BIT15, DP_BIT15, DP_BIT15});
    pe->history_cs = DP_canvas_state_new();
    pe->view_cs = DP_canvas_state_incref(pe->history_cs);
    pe->local_view.active_layer_id = 0;
    pe->local_view.active_frame_index = 0;
    pe->local_view.view_mode = DP_VIEW_MODE_NORMAL;
    pe->local_view.onion_skins = NULL;
    pe->local_view.reveal_censored = false;
    pe->local_view.inspect_context_id = 0;
    DP_VECTOR_INIT_TYPE(&pe->local_view.hidden_layers, int, 8);
    pe->local_view.prev_lpl = NULL;
    pe->local_view.prev_tl = NULL;
    pe->local_view.lpl = NULL;
    pe->local_view.background_tile = NULL;
    pe->local_view.background_opaque = false;
    pe->local_view.layers_can_decrease_opacity = true;
    pe->paint_dc = paint_dc;
    pe->preview = NULL;
    pe->preview_dc = preview_dc;
    DP_message_queue_init(&pe->local_queue, INITIAL_QUEUE_CAPACITY);
    DP_message_queue_init(&pe->remote_queue, INITIAL_QUEUE_CAPACITY);
    pe->queue_sem = DP_semaphore_new(0);
    pe->queue_mutex = DP_mutex_new();
    DP_atomic_set(&pe->running, true);
    DP_atomic_set(&pe->catchup, -1);
    DP_atomic_set(&pe->default_layer_id, -1);
    DP_atomic_ptr_set(&pe->next_preview, NULL);
    pe->paint_thread = DP_thread_new(run_paint_engine, pe);
    pe->record.path = NULL;
    pe->record.recorder = NULL;
    pe->record.start_sem = DP_semaphore_new(0);
    pe->record.state_change = RECORDER_STOPPED;
    pe->record.get_time_ms_fn = get_time_ms_fn;
    pe->record.get_time_ms_user = get_time_ms_user;
    pe->playback.player = player_or_null;
    pe->playback.fn = playback_fn;
    pe->playback.dump_fn = dump_playback_fn;
    pe->playback.user = playback_user;
    pe->render.worker =
        DP_worker_new(1024, sizeof(struct DP_PaintEngineRenderJobParams),
                      render_thread_count, render_job);
    pe->render.tiles_done_sem = DP_semaphore_new(0);
    pe->render.tiles_waiting = 0;
    // If there's hidden layers in the canvas state, add them to our
    // local view so that they actually appear hidden to start with.
    if (cs_or_null) {
        DP_Vector *hidden_layers = &pe->local_view.hidden_layers;
        DP_LayerPropsList *lpl = DP_canvas_state_layer_props_noinc(cs_or_null);
        apply_hidden_layers_recursive(hidden_layers, lpl);
    }
    return pe;
}

void DP_paint_engine_free_join(DP_PaintEngine *pe)
{
    if (pe) {
        DP_paint_engine_recorder_stop(pe);
        DP_atomic_set(&pe->running, false);
        DP_semaphore_free(pe->render.tiles_done_sem);
        DP_worker_free_join(pe->render.worker);
        DP_SEMAPHORE_MUST_POST(pe->queue_sem);
        DP_thread_free_join(pe->paint_thread);
        DP_player_free(pe->playback.player);
        DP_semaphore_free(pe->record.start_sem);
        DP_mutex_free(pe->queue_mutex);
        DP_semaphore_free(pe->queue_sem);
        free_preview(DP_atomic_ptr_xch(&pe->next_preview, NULL));
        DP_message_queue_dispose(&pe->remote_queue);
        DP_Message *msg;
        while ((msg = DP_message_queue_shift(&pe->local_queue))) {
            if (DP_message_type(msg) == DP_MSG_INTERNAL) {
                DP_MsgInternal *mi = DP_msg_internal_cast(msg);
                switch (DP_msg_internal_type(mi)) {
                case DP_MSG_INTERNAL_TYPE_RESET_TO_STATE:
                    DP_canvas_state_decref(
                        DP_msg_internal_reset_to_state_data(mi));
                    break;
                case DP_MSG_INTERNAL_TYPE_PREVIEW:
                    free_preview(DP_msg_internal_preview_data(mi));
                    break;
                case DP_MSG_INTERNAL_TYPE_DUMP_COMMAND: {
                    int count;
                    DP_Message **msgs =
                        DP_msg_internal_dump_command_messages(mi, &count);
                    decref_messages(count, msgs);
                }
                default:
                    break;
                }
            }
            DP_message_decref(msg);
        }
        DP_message_queue_dispose(&pe->local_queue);
        free_preview(pe->preview);
        DP_tile_decref_nullable(pe->local_view.background_tile);
        DP_layer_props_list_decref_nullable(pe->local_view.lpl);
        DP_timeline_decref_nullable(pe->local_view.prev_tl);
        DP_layer_props_list_decref_nullable(pe->local_view.prev_lpl);
        DP_vector_dispose(&pe->local_view.hidden_layers);
        DP_canvas_state_decref_nullable(pe->history_cs);
        DP_canvas_state_decref_nullable(pe->view_cs);
        DP_tile_decref(pe->checker);
        DP_transient_layer_content_decref(pe->tlc);
        DP_canvas_diff_free(pe->diff);
        DP_canvas_history_free(pe->ch);
        DP_free_simd(pe->buffers);
        DP_free(pe);
    }
}

int DP_paint_engine_render_thread_count(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return DP_worker_thread_count(pe->render.worker);
}

DP_TransientLayerContent *
DP_paint_engine_render_content_noinc(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return pe->tlc;
}

void DP_paint_engine_local_drawing_in_progress_set(
    DP_PaintEngine *pe, bool local_drawing_in_progress)
{
    DP_ASSERT(pe);
    DP_canvas_history_local_drawing_in_progress_set(pe->ch,
                                                    local_drawing_in_progress);
}

bool DP_paint_engine_want_canvas_history_dump(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return DP_canvas_history_want_dump(pe->ch);
}

void DP_paint_engine_want_canvas_history_dump_set(DP_PaintEngine *pe,
                                                  bool want_canvas_history_dump)
{
    DP_ASSERT(pe);
    DP_canvas_history_want_dump_set(pe->ch, want_canvas_history_dump);
}


static void invalidate_local_view(DP_PaintEngine *pe, bool check_all)
{
    DP_layer_props_list_decref_nullable(pe->local_view.prev_lpl);
    pe->local_view.prev_lpl = NULL;
    if (check_all) {
        DP_canvas_diff_check_all(pe->diff);
    }
}

void DP_paint_engine_active_layer_id_set(DP_PaintEngine *pe, int layer_id)
{
    if (pe->local_view.active_layer_id != layer_id) {
        pe->local_view.active_layer_id = layer_id;
        if (pe->local_view.view_mode != DP_VIEW_MODE_NORMAL) {
            invalidate_local_view(pe, true);
        }
    }
}

void DP_paint_engine_active_frame_index_set(DP_PaintEngine *pe, int frame_index)
{
    if (pe->local_view.active_frame_index != frame_index) {
        pe->local_view.active_frame_index = frame_index;
        if (pe->local_view.view_mode == DP_VIEW_MODE_FRAME) {
            invalidate_local_view(pe, true);
        }
    }
}

void DP_paint_engine_view_mode_set(DP_PaintEngine *pe, DP_ViewMode vm)
{
    DP_ASSERT(pe);
    if (pe->local_view.view_mode != vm) {
        pe->local_view.view_mode = vm;
        invalidate_local_view(pe, true);
    }
}

void DP_paint_engine_onion_skins_set(DP_PaintEngine *pe,
                                     const DP_OnionSkins *oss_or_null)
{
    DP_ASSERT(pe);
    if (pe->local_view.onion_skins != oss_or_null) {
        pe->local_view.onion_skins = oss_or_null;
        invalidate_local_view(pe, true);
    }
}

bool DP_paint_engine_reveal_censored(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return pe->local_view.reveal_censored;
}

void DP_paint_engine_reveal_censored_set(DP_PaintEngine *pe,
                                         bool reveal_censored)
{
    DP_ASSERT(pe);
    if (pe->local_view.reveal_censored != reveal_censored) {
        pe->local_view.reveal_censored = reveal_censored;
        invalidate_local_view(pe, false);
    }
}


void DP_paint_engine_inspect_context_id_set(DP_PaintEngine *pe,
                                            unsigned int context_id)
{
    DP_ASSERT(pe);
    if (pe->local_view.inspect_context_id != context_id) {
        pe->local_view.inspect_context_id = context_id;
        invalidate_local_view(pe, false);
    }
}


static bool is_hidden_layer_id(void *element, void *user)
{
    return *(int *)element == *(int *)user;
}

static void check_layer_id(DP_PaintEngine *pe, int layer_id)
{
    DP_CanvasState *cs = pe->view_cs;
    DP_LayerRoutes *lr = DP_canvas_state_layer_routes_noinc(cs);
    DP_LayerRoutesEntry *lre = DP_layer_routes_search(lr, layer_id);
    if (lre) {
        if (DP_layer_routes_entry_is_group(lre)) {
            DP_LayerGroup *lg = DP_layer_routes_entry_group(lre, cs);
            DP_layer_group_diff_mark(lg, pe->diff);
        }
        else {
            DP_LayerContent *lc = DP_layer_routes_entry_content(lre, cs);
            DP_layer_content_diff_mark(lc, pe->diff);
        }
    }
}

void DP_paint_engine_layer_visibility_set(DP_PaintEngine *pe, int layer_id,
                                          bool hidden)
{
    DP_ASSERT(pe);
    DP_Vector *hidden_layers = &pe->local_view.hidden_layers;
    int index = DP_vector_search_index(hidden_layers, sizeof(int),
                                       is_hidden_layer_id, &layer_id);
    if (hidden && index == -1) {
        DP_VECTOR_PUSH_TYPE(hidden_layers, int, layer_id);
        invalidate_local_view(pe, false);
        check_layer_id(pe, layer_id);
    }
    else if (!hidden && index != -1) {
        DP_VECTOR_REMOVE_TYPE(hidden_layers, int, index);
        invalidate_local_view(pe, false);
        check_layer_id(pe, layer_id);
    }
}


DP_Tile *DP_paint_engine_local_background_tile_noinc(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return pe->local_view.background_tile;
}

void DP_paint_engine_local_background_tile_set_noinc(DP_PaintEngine *pe,
                                                     DP_Tile *tile_or_null)
{
    DP_ASSERT(pe);
    DP_Tile *prev = pe->local_view.background_tile;
    if (prev != tile_or_null) {
        pe->local_view.background_tile = tile_or_null;
        pe->local_view.background_opaque = DP_tile_opaque(tile_or_null);
        DP_tile_decref_nullable(prev);
        invalidate_local_view(pe, false);
    }
}


static bool start_recording(DP_PaintEngine *pe, DP_RecorderType type,
                            char *path, bool with_history)
{
    DP_Output *output = DP_file_output_new_from_path(path);
    if (!output) {
        DP_free(path);
        return false;
    }

    DP_Recorder *r;
    if (with_history) {
        // To get a clean initial recording, we have to first spin down all
        // queued messages. We send ourselves a RECORDER_START internal message
        // and then block this thread (which must be the only thread interacting
        // with the paint engine, maybe we should verify that somehow) until the
        // paint thread gets to it.
        DP_MUTEX_MUST_LOCK(pe->queue_mutex);
        DP_message_queue_push_noinc(&pe->remote_queue,
                                    DP_msg_internal_recorder_start_new(0));
        DP_SEMAPHORE_MUST_POST(pe->queue_sem);
        DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);
        // The paint thread will post to this semaphore when it reaches our
        // recorder start message.
        DP_SEMAPHORE_MUST_WAIT(pe->record.start_sem);
        DP_ASSERT(pe->remote_queue.used == 0);
        // Now all queued messages have been handled. We can't just take the
        // current canvas state from the canvas history though, since that would
        // lose undo history and might contain state from local messages. So
        // instead, we grab the oldest reachable state and then record the
        // entire history since then.
        r = DP_canvas_history_recorder_new(pe->ch, type,
                                           pe->record.get_time_ms_fn,
                                           pe->record.get_time_ms_user, output);
    }
    else {
        // This is a continuation recording after a reset. That means there's no
        // history to worry about, just create a fresh recorder.
        r = DP_recorder_new_inc(type, NULL, pe->record.get_time_ms_fn,
                                pe->record.get_time_ms_user, output);
    }

    if (r) {
        if (pe->record.recorder) {
            DP_warn("Stopping already running recording");
            DP_paint_engine_recorder_stop(pe);
        }
        // When dealing with cleanup after a disconnect, the recorder might
        // currently be in the process of being fed messages from the local
        // fork, so we have to take a lock around manipulating it. We re-use
        // the queue mutex for that, since it's what the cleanup uses too.
        DP_MUTEX_MUST_LOCK(pe->queue_mutex);
        pe->record.path = path;
        pe->record.recorder = r;
        DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);
        pe->record.state_change = RECORDER_STARTED;
        return true;
    }
    else {
        DP_free(path);
        return false;
    }
}

bool DP_paint_engine_recorder_start(DP_PaintEngine *pe, DP_RecorderType type,
                                    const char *path)
{
    return start_recording(pe, type, DP_strdup(path), true);
}

bool DP_paint_engine_recorder_stop(DP_PaintEngine *pe)
{
    if (pe->record.recorder) {
        // Need to take a lock due to cleanup handling, see explanation above.
        DP_MUTEX_MUST_LOCK(pe->queue_mutex);
        DP_recorder_free_join(pe->record.recorder);
        DP_free(pe->record.path);
        pe->record.path = NULL;
        pe->record.recorder = NULL;
        DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);
        pe->record.state_change = RECORDER_STOPPED;
        return true;
    }
    else {
        return false;
    }
}

bool DP_paint_engine_recorder_is_recording(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return pe->record.recorder != NULL;
}


static void push_playback(DP_PaintEnginePushMessageFn push_message, void *user,
                          long long position, int interval)
{
    push_message(user, DP_msg_internal_playback_new(0, position, interval));
}

static DP_PlayerResult
skip_playback_forward(DP_PaintEngine *pe, long long steps, bool single_step,
                      DP_PaintEnginePushMessageFn push_message, void *user)
{
    DP_ASSERT(steps >= 0);
    DP_ASSERT(push_message);
    DP_debug("Skip playback forward by %lld %s", steps,
             single_step ? "step(s)" : "undo point(s)");

    DP_Player *player = pe->playback.player;
    if (!player) {
        DP_error_set("No player set");
        push_playback(push_message, user, -1, 0);
        return DP_PLAYER_ERROR_OPERATION;
    }

    long long done = 0;
    int interval = 0;
    while (done < steps) {
        DP_Message *msg;
        DP_PlayerResult result = DP_player_step(player, &msg);
        if (result == DP_PLAYER_SUCCESS) {
            DP_MessageType type = DP_message_type(msg);
            if (type == DP_MSG_INTERVAL) {
                DP_MsgInterval *mi = DP_message_internal(msg);
                int ms = DP_msg_interval_msecs(mi);
                // Don't overflow the interval, cap it at INT_MAX instead.
                interval = ms <= INT_MAX - interval ? interval + ms : INT_MAX;
                DP_message_decref(msg);
                if (single_step) {
                    ++done;
                }
            }
            else {
                if (single_step || type == DP_MSG_UNDO_POINT) {
                    ++done;
                }
                push_message(user, msg);
            }
        }
        else if (result == DP_PLAYER_ERROR_PARSE) {
            if (single_step) {
                ++done;
            }
            DP_warn("Can't play back message: %s", DP_error());
        }
        else {
            // We're either at the end of the recording or encountered an input
            // error. In either case, we're done playing this recording, report
            // a negative value as the position to indicate that.
            push_playback(push_message, user, -1, 0);
            return result;
        }
    }

    // If we don't have an index, report the position as a percentage
    // completion based on the amount of bytes read from the recording.
    long long position =
        DP_player_index_loaded(player)
            ? DP_player_position(player)
            : DP_double_to_llong(DP_player_progress(player) * 100.0 + 0.5);
    push_playback(push_message, user, position, interval);
    return DP_PLAYER_SUCCESS;
}

DP_PlayerResult
DP_paint_engine_playback_step(DP_PaintEngine *pe, long long steps,
                              DP_PaintEnginePushMessageFn push_message,
                              void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(steps >= 0);
    DP_ASSERT(push_message);
    return skip_playback_forward(pe, steps, true, push_message, user);
}

static DP_PlayerResult
jump_playback_to(DP_PaintEngine *pe, DP_DrawContext *dc, long long to,
                 bool relative, bool exact,
                 DP_PaintEnginePushMessageFn push_message, void *user)
{
    DP_Player *player = pe->playback.player;
    if (!player) {
        DP_error_set("No player set");
        push_playback(push_message, user, -1, 0);
        return DP_PLAYER_ERROR_OPERATION;
    }

    if (!DP_player_index_loaded(player)) {
        DP_error_set("No index loaded");
        push_playback(push_message, user, -1, 0);
        return DP_PLAYER_ERROR_OPERATION;
    }

    long long player_position = DP_player_position(player);
    long long target_position = relative ? player_position + to : to;
    DP_debug("Jump playback from %lld to %s %lld", player_position,
             exact ? "exactly" : "snapshot nearest", target_position);

    long long message_count =
        DP_uint_to_llong(DP_player_index_message_count(player));
    if (message_count == 0) {
        DP_error_set("Recording contains no messages");
        push_playback(push_message, user, player_position, 0);
        return DP_PLAYER_ERROR_INPUT;
    }
    else if (target_position < 0) {
        target_position = 0;
    }
    else if (target_position >= message_count) {
        target_position = message_count - 1;
    }
    DP_debug("Clamped position to %lld (message count %lld)", target_position,
             message_count);

    DP_PlayerIndexEntry entry =
        DP_player_index_entry_search(player, target_position);
    DP_debug("Loaded entry with message index %lld, message offset %zu, "
             "snapshot offset %zu, thumbnail offset %zu",
             entry.message_index, entry.message_offset, entry.snapshot_offset,
             entry.thumbnail_offset);

    bool inside_snapshot = player_position >= entry.message_index
                        && player_position < target_position;
    if (inside_snapshot) {
        long long steps = target_position - player_position;
        DP_debug("Already inside snapshot, stepping forward by %lld", steps);
        DP_PlayerResult result =
            skip_playback_forward(pe, steps, true, push_message, user);
        // If skipping playback forward doesn't work, e.g. because we're
        // in an input error state, punt to reloading the snapshot.
        if (result == DP_PLAYER_SUCCESS) {
            return result;
        }
        else {
            DP_warn("Skipping inside snapshot failed with result %d: %s",
                    (int)result, DP_error());
        }
    }

    DP_CanvasState *cs = DP_player_index_entry_load(player, dc, entry);
    if (!cs) {
        DP_debug("Reading snapshot failed: %s", DP_error());
        push_playback(push_message, user, player_position, 0);
        return DP_PLAYER_ERROR_INPUT;
    }

    if (!DP_player_seek(player, entry.message_index, entry.message_offset)) {
        DP_canvas_state_decref(cs);
        push_playback(push_message, user, player_position, 0);
        return DP_PLAYER_ERROR_INPUT;
    }

    push_message(user, DP_msg_internal_reset_to_state_new(0, cs));
    return skip_playback_forward(
        pe, exact ? target_position - entry.message_index : 0, true,
        push_message, user);
}

DP_PlayerResult DP_paint_engine_playback_skip_by(
    DP_PaintEngine *pe, DP_DrawContext *dc, long long steps,
    DP_PaintEnginePushMessageFn push_message, void *user)
{
    DP_ASSERT(pe);
    if (steps < 0) {
        return jump_playback_to(pe, dc, steps, true, false, push_message, user);
    }
    else {
        return skip_playback_forward(pe, steps, false, push_message, user);
    }
}

DP_PlayerResult DP_paint_engine_playback_jump_to(
    DP_PaintEngine *pe, DP_DrawContext *dc, long long position,
    DP_PaintEnginePushMessageFn push_message, void *user)
{
    DP_ASSERT(pe);
    return jump_playback_to(pe, dc, position, false, true, push_message, user);
}

bool DP_paint_engine_playback_index_build(
    DP_PaintEngine *pe, DP_DrawContext *dc,
    DP_PlayerIndexShouldSnapshotFn should_snapshot_fn,
    DP_PlayerIndexProgressFn progress_fn, void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(dc);
    DP_Player *player = pe->playback.player;
    if (player) {
        return DP_player_index_build(player, dc, should_snapshot_fn,
                                     progress_fn, user);
    }
    else {
        DP_error_set("No player set");
        return false;
    }
}

bool DP_paint_engine_playback_index_load(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    DP_Player *player = pe->playback.player;
    if (player) {
        return DP_player_index_load(player);
    }
    else {
        DP_error_set("No player set");
        return false;
    }
}

unsigned int DP_paint_engine_playback_index_message_count(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    DP_Player *player = pe->playback.player;
    if (player) {
        return DP_player_index_message_count(player);
    }
    else {
        DP_error_set("No player set");
        return 0;
    }
}

size_t DP_paint_engine_playback_index_entry_count(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    DP_Player *player = pe->playback.player;
    if (player) {
        return DP_player_index_entry_count(player);
    }
    else {
        DP_error_set("No player set");
        return 0;
    }
}

DP_Image *DP_paint_engine_playback_index_thumbnail_at(DP_PaintEngine *pe,
                                                      size_t index,
                                                      bool *out_error)
{
    DP_ASSERT(pe);
    DP_Player *player = pe->playback.player;
    if (player) {
        return DP_player_index_thumbnail_at(player, index, out_error);
    }
    else {
        DP_error_set("No player set");
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
}

static DP_PlayerResult step_dump(DP_Player *player,
                                 DP_PaintEnginePushMessageFn push_message,
                                 void *user)
{
    DP_DumpType type;
    int count;
    DP_Message **msgs;
    DP_PlayerResult result = DP_player_step_dump(player, &type, &count, &msgs);
    if (result == DP_PLAYER_SUCCESS) {
        push_message(user, DP_msg_internal_dump_command_new_inc(0, (int)type,
                                                                count, msgs));
    }
    return result;
}

static void push_dump_playback(DP_PaintEnginePushMessageFn push_message,
                               void *user, long long position)
{
    push_message(user, DP_msg_internal_dump_playback_new(0, position));
}

DP_PlayerResult DP_paint_engine_playback_dump_step(
    DP_PaintEngine *pe, DP_PaintEnginePushMessageFn push_message, void *user)
{
    DP_ASSERT(pe);
    DP_Player *player = pe->playback.player;
    if (!player) {
        DP_error_set("No player set");
        push_dump_playback(push_message, user, -1);
        return DP_PLAYER_ERROR_OPERATION;
    }

    DP_PlayerResult result = step_dump(player, push_message, user);
    long long position_to_report =
        result == DP_PLAYER_SUCCESS ? DP_player_position(player) : -1;
    push_dump_playback(push_message, user, position_to_report);
    return result;
}

DP_PlayerResult DP_paint_engine_playback_dump_jump_previous_reset(
    DP_PaintEngine *pe, DP_PaintEnginePushMessageFn push_message, void *user)
{
    DP_Player *player = pe->playback.player;
    if (!player) {
        DP_error_set("No player set");
        push_dump_playback(push_message, user, -1);
        return DP_PLAYER_ERROR_OPERATION;
    }

    if (!DP_player_seek_dump(player, DP_player_position(player) - 1)) {
        push_dump_playback(push_message, user, -1);
        return DP_PLAYER_ERROR_INPUT;
    }

    push_message(user, DP_msg_internal_reset_new(0));
    push_dump_playback(push_message, user, DP_player_position(player));
    return DP_PLAYER_SUCCESS;
}

static int step_toward_next_reset(DP_Player *player, bool stop_at_reset,
                                  DP_PaintEnginePushMessageFn push_message,
                                  void *user)
{
    long long position = DP_player_position(player);
    size_t offset = DP_player_tell(player);
    DP_DumpType type;
    int count;
    DP_Message **msgs;
    DP_PlayerResult result = DP_player_step_dump(player, &type, &count, &msgs);
    if (result == DP_PLAYER_SUCCESS) {
        if (type == DP_DUMP_RESET && stop_at_reset) {
            if (DP_player_seek(player, position, offset)) {
                return -1;
            }
            else {
                return DP_PLAYER_ERROR_INPUT;
            }
        }
        else {
            push_message(user, DP_msg_internal_dump_command_new_inc(
                                   0, (int)type, count, msgs));
        }
    }
    return (int)result;
}

DP_PlayerResult DP_paint_engine_playback_dump_jump_next_reset(
    DP_PaintEngine *pe, DP_PaintEnginePushMessageFn push_message, void *user)
{
    DP_ASSERT(pe);
    DP_Player *player = pe->playback.player;
    if (!player) {
        DP_error_set("No player set");
        push_dump_playback(push_message, user, -1);
        return DP_PLAYER_ERROR_OPERATION;
    }

    // We want to jump up to, but excluding the next reset. So we keep walking
    // through the dump until we hit a reset and then seek to before it. If we
    // hit a reset as our very first message, we push through it, since
    // otherwise we'd be doing nothing at all until manually stepping forward.
    int result = step_toward_next_reset(player, false, push_message, user);
    while (result == DP_PLAYER_SUCCESS) {
        result = step_toward_next_reset(player, true, push_message, user);
    }

    push_dump_playback(push_message, user, DP_player_position(player));
    return result == -1 ? DP_PLAYER_SUCCESS : (DP_PlayerResult)result;
}

DP_PlayerResult
DP_paint_engine_playback_dump_jump(DP_PaintEngine *pe, long long position,
                                   DP_PaintEnginePushMessageFn push_message,
                                   void *user)
{
    DP_ASSERT(pe);
    DP_Player *player = pe->playback.player;
    if (!player) {
        DP_error_set("No player set");
        push_dump_playback(push_message, user, -1);
        return DP_PLAYER_ERROR_OPERATION;
    }

    if (!DP_player_seek_dump(player, position)) {
        push_dump_playback(push_message, user, -1);
        return DP_PLAYER_ERROR_INPUT;
    }

    if (DP_player_position(player) == 0) {
        push_message(user, DP_msg_internal_reset_new(0));
    }

    DP_PlayerResult result = DP_PLAYER_SUCCESS;
    while (DP_player_position(player) < position) {
        result = step_dump(player, push_message, user);
        if (result != DP_PLAYER_SUCCESS) {
            break;
        }
    }
    push_dump_playback(push_message, user, DP_player_position(player));
    return result;
}

bool DP_paint_engine_playback_close(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    DP_Player *player = pe->playback.player;
    if (player) {
        DP_player_free(player);
        pe->playback.player = NULL;
        return true;
    }
    else {
        return false;
    }
}


static bool is_pushable_type(DP_MessageType type)
{
    return type >= 128 || type == DP_MSG_INTERNAL
        || type == DP_MSG_DEFAULT_LAYER;
}

static DP_PaintEngineMetaBuffer *get_meta_buffer(DP_PaintEngine *pe)
{
    return (DP_PaintEngineMetaBuffer *)pe->buffers;
}

static void restart_recording(DP_PaintEngine *pe)
{
    char *path = DP_file_path_unique_nonexistent(pe->record.path, 99);
    if (!path) {
        DP_warn("Can't restart recording: no available file name after %s",
                pe->record.path);
    }

    DP_RecorderType type = DP_recorder_type(pe->record.recorder);
    DP_paint_engine_recorder_stop(pe);
    if (path && !start_recording(pe, type, path, false)) {
        DP_warn("Can't restart recording: %s", DP_error());
    }
}

static void record_message(DP_PaintEngine *pe, DP_Message *msg,
                           DP_MessageType type)
{
    DP_Recorder *r = pe->record.recorder;
    if (r) {
        if (type == DP_MSG_INTERNAL) {
            DP_MsgInternal *mi = DP_message_internal(msg);
            if (DP_msg_internal_type(mi) == DP_MSG_INTERNAL_TYPE_RESET) {
                restart_recording(pe);
            }
        }
        else if (!DP_recorder_message_push_inc(r, msg)) {
            DP_warn("Failed to push message to recorder: %s", DP_error());
            DP_paint_engine_recorder_stop(pe);
        }
    }
}

static void handle_laser_trail(DP_PaintEngine *pe, DP_Message *msg)
{
    uint8_t context_id = DP_uint_to_uint8(DP_message_context_id(msg));
    DP_PaintEngineLaserChanges *laser = &get_meta_buffer(pe)->laser_changes;

    int laser_count = laser->count;
    if (laser_count == 0) {
        memset(laser->active, 0, sizeof(laser->active));
        laser->users[0] = context_id;
        laser->count = 1;
    }
    else if (!laser->active[context_id]) {
        laser->active[context_id] = true;
        laser->users[laser_count] = context_id;
        ++laser->count;
    }

    DP_MsgLaserTrail *mlt = DP_msg_laser_trail_cast(msg);
    DP_UPixel8 pixel = {DP_msg_laser_trail_color(mlt)};
    laser->buffers[context_id] =
        (DP_PaintEngineLaserBuffer){DP_msg_laser_trail_persistence(mlt),
                                    pixel.b, pixel.g, pixel.r, pixel.a};
}

static void handle_move_pointer(DP_PaintEngine *pe, DP_Message *msg)
{
    uint8_t context_id = DP_uint_to_uint8(DP_message_context_id(msg));
    DP_PaintEngineCursorChanges *cursor = &get_meta_buffer(pe)->cursor_changes;

    int cursor_count = cursor->count;
    if (cursor_count == 0) {
        memset(cursor->active, 0, sizeof(cursor->active));
        cursor->users[0] = context_id;
        cursor->count = 1;
    }
    else if (!cursor->active[context_id]) {
        cursor->active[context_id] = true;
        cursor->users[cursor_count] = context_id;
        ++cursor->count;
    }

    DP_MsgMovePointer *mmp = DP_msg_move_pointer_cast(msg);
    cursor->positions[context_id] = (DP_PaintEngineCursorPosition){
        DP_msg_move_pointer_x(mmp), DP_msg_move_pointer_y(mmp)};
}

static bool should_push_message_remote(DP_PaintEngine *pe, DP_Message *msg,
                                       bool override_acls)
{
    uint8_t result = DP_acl_state_handle(pe->acls, msg, override_acls);
    get_meta_buffer(pe)->acl_change_flags |= result;
    if (result & DP_ACL_STATE_FILTERED_BIT) {
        DP_debug("ACL filtered %s message from user %u",
                 DP_message_type_enum_name_unprefixed(DP_message_type(msg)),
                 DP_message_context_id(msg));
    }
    else {
        DP_MessageType type = DP_message_type(msg);
        record_message(pe, msg, type);
        if (is_pushable_type(type)) {
            return true;
        }
        else if (type == DP_MSG_LASER_TRAIL) {
            handle_laser_trail(pe, msg);
        }
        else if (type == DP_MSG_MOVE_POINTER) {
            handle_move_pointer(pe, msg);
        }
    }
    return false;
}

static bool should_push_message_local(DP_UNUSED DP_PaintEngine *pe,
                                      DP_Message *msg,
                                      DP_UNUSED bool ignore_acls)
{
    DP_MessageType type = DP_message_type(msg);
    return is_pushable_type(type);
}

static int push_messages(DP_PaintEngine *pe, DP_Queue *queue,
                         bool override_acls, int count, DP_Message **msgs,
                         bool (*should_push)(DP_PaintEngine *, DP_Message *,
                                             bool))
{
    DP_MUTEX_MUST_LOCK(pe->queue_mutex);
    // First message is the one that triggered the call to this function,
    // push it unconditionally. Then keep checking the rest again.
    int pushed = 1;
    DP_message_queue_push_inc(queue, msgs[0]);
    for (int i = 1; i < count; ++i) {
        DP_Message *msg = msgs[i];
        if (should_push(pe, msg, override_acls)) {
            DP_message_queue_push_inc(queue, msg);
            ++pushed;
        }
    }
    DP_SEMAPHORE_MUST_POST_N(pe->queue_sem, pushed);
    DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);
    return pushed;
}

int DP_paint_engine_handle_inc(DP_PaintEngine *pe, bool local,
                               bool override_acls, int count, DP_Message **msgs,
                               DP_PaintEngineAclsChangedFn acls_changed,
                               DP_PaintEngineLaserTrailFn laser_trail,
                               DP_PaintEngineMovePointerFn move_pointer,
                               void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(msgs);
    DP_PERF_BEGIN_DETAIL(fn, "handle", "count=%d,local=%d", count, local);

    bool (*should_push)(DP_PaintEngine *, DP_Message *, bool) =
        local ? should_push_message_local : should_push_message_remote;

    DP_PaintEngineMetaBuffer *meta_buffer = get_meta_buffer(pe);
    meta_buffer->acl_change_flags = 0;
    meta_buffer->laser_changes.count = 0;
    meta_buffer->cursor_changes.count = 0;

    // Don't lock anything until we actually find a message to push.
    int pushed = 0;
    for (int i = 0; i < count; ++i) {
        if (should_push(pe, msgs[i], override_acls)) {
            DP_PERF_BEGIN(push, "handle:push");
            pushed =
                push_messages(pe, local ? &pe->local_queue : &pe->remote_queue,
                              override_acls, count - i, msgs + i, should_push);
            DP_PERF_END(push);
            break;
        }
    }

    int acl_change_flags =
        meta_buffer->acl_change_flags & DP_ACL_STATE_CHANGE_MASK;
    if (acl_change_flags != 0) {
        acls_changed(user, acl_change_flags);
    }

    DP_PaintEngineLaserChanges *laser = &meta_buffer->laser_changes;
    int laser_count = laser->count;
    for (int i = 0; i < laser_count; ++i) {
        uint8_t context_id = laser->users[i];
        DP_PaintEngineLaserBuffer *lb = &laser->buffers[context_id];
        DP_UPixel8 pixel = {.b = lb->b, .g = lb->g, .r = lb->r, .a = lb->a};
        laser_trail(user, context_id, lb->persistence, pixel.color);
    }

    DP_PaintEngineCursorChanges *cursor = &meta_buffer->cursor_changes;
    int cursor_count = cursor->count;
    for (int i = 0; i < cursor_count; ++i) {
        uint8_t context_id = cursor->users[i];
        DP_PaintEngineCursorPosition *cp = &cursor->positions[context_id];
        move_pointer(user, context_id, DP_int32_to_int(cp->x),
                     DP_int32_to_int(cp->y));
    }

    DP_PERF_END(fn);
    return pushed;
}


static DP_CanvasState *apply_preview(DP_PaintEngine *pe, DP_CanvasState *cs)
{
    DP_PaintEnginePreview *preview = pe->preview;
    if (preview) {
        DP_PERF_BEGIN(preview, "tick:changes:preview");
        DP_CanvasState *next_cs = preview->render(
            preview, cs, pe->preview_dc,
            preview->initial_offset_x - DP_canvas_state_offset_x(cs),
            preview->initial_offset_y - DP_canvas_state_offset_y(cs));
        DP_PERF_END(preview);
        return next_cs;
    }
    else {
        return DP_canvas_state_incref(cs);
    }
}

static DP_TransientCanvasState *
get_or_make_transient_canvas_state(DP_CanvasState *cs)
{
    if (DP_canvas_state_transient(cs)) {
        return (DP_TransientCanvasState *)cs;
    }
    else {
        DP_TransientCanvasState *tcs = DP_transient_canvas_state_new(cs);
        DP_canvas_state_decref(cs);
        return tcs;
    }
}


static DP_TransientLayerContent *
make_inspect_sublayer(DP_TransientCanvasState *tcs, DP_DrawContext *dc)
{
    int index_count;
    int *indexes = DP_draw_context_layer_indexes(dc, &index_count);
    DP_TransientLayerContent *tlc =
        DP_layer_routes_entry_indexes_transient_content(index_count, indexes,
                                                        tcs);
    DP_TransientLayerContent *sub_tlc;
    DP_TransientLayerProps *sub_tlp;
    DP_transient_layer_content_transient_sublayer(tlc, INSPECT_SUBLAYER_ID,
                                                  &sub_tlc, &sub_tlp);
    DP_transient_layer_props_opacity_set(sub_tlp, DP_BIT15 - DP_BIT15 / 4);
    DP_transient_layer_props_blend_mode_set(sub_tlp, DP_BLEND_MODE_RECOLOR);
    return sub_tlc;
}

static void maybe_add_inspect_sublayer(DP_TransientCanvasState *tcs,
                                       DP_DrawContext *dc,
                                       unsigned int context_id, int xtiles,
                                       int ytiles, DP_LayerContent *lc)
{
    DP_TransientLayerContent *sub_tlc = NULL;
    for (int y = 0; y < ytiles; ++y) {
        for (int x = 0; x < xtiles; ++x) {
            DP_Tile *t = DP_layer_content_tile_at_noinc(lc, x, y);
            if (t && DP_tile_context_id(t) == context_id) {
                if (!sub_tlc) {
                    sub_tlc = make_inspect_sublayer(tcs, dc);
                }
                DP_transient_layer_content_tile_set_noinc(
                    sub_tlc, DP_tile_censored_inc(), y * xtiles + x);
            }
        }
    }
}

static void apply_inspect_recursive(DP_TransientCanvasState *tcs,
                                    DP_DrawContext *dc, unsigned int context_id,
                                    int xtiles, int ytiles, DP_LayerList *ll)
{
    int count = DP_layer_list_count(ll);
    DP_draw_context_layer_indexes_push(dc);
    for (int i = 0; i < count; ++i) {
        DP_draw_context_layer_indexes_set(dc, i);
        DP_LayerListEntry *lle = DP_layer_list_at_noinc(ll, i);
        if (DP_layer_list_entry_is_group(lle)) {
            DP_LayerGroup *lg = DP_layer_list_entry_group_noinc(lle);
            DP_LayerList *child_lle = DP_layer_group_children_noinc(lg);
            apply_inspect_recursive(tcs, dc, context_id, xtiles, ytiles,
                                    child_lle);
        }
        else {
            DP_LayerContent *lc = DP_layer_list_entry_content_noinc(lle);
            maybe_add_inspect_sublayer(tcs, dc, context_id, xtiles, ytiles, lc);
        }
    }
    DP_draw_context_layer_indexes_pop(dc);
}

static DP_CanvasState *apply_inspect(DP_PaintEngine *pe, DP_CanvasState *cs)
{
    unsigned int context_id = pe->local_view.inspect_context_id;
    if (context_id == 0) {
        return cs;
    }
    else {
        DP_PERF_BEGIN(inspect, "tick:changes:inspect");
        DP_TransientCanvasState *tcs = get_or_make_transient_canvas_state(cs);
        DP_DrawContext *dc = pe->preview_dc;
        DP_draw_context_layer_indexes_clear(dc);
        DP_TileCounts tile_counts = DP_tile_counts_round(
            DP_canvas_state_width(cs), DP_canvas_state_height(cs));
        apply_inspect_recursive(tcs, dc, context_id, tile_counts.x,
                                tile_counts.y,
                                DP_canvas_state_layers_noinc(cs));
        DP_PERF_END(inspect);
        return (DP_CanvasState *)tcs;
    }
}


// Only grab the timeline if we're in a view mode where that's relevant and
// we're not dealing with an automatic timeline, where the layers act as frames.
static DP_Timeline *maybe_get_timeline(DP_CanvasState *cs, DP_ViewMode vm)
{
    bool want_timeline =
        vm == DP_VIEW_MODE_FRAME && DP_canvas_state_use_timeline(cs);
    return want_timeline ? DP_canvas_state_timeline_noinc(cs) : NULL;
}

static void set_local_layer_props_recursive(DP_TransientCanvasState *tcs,
                                            DP_DrawContext *dc,
                                            bool reveal_censored,
                                            DP_LayerPropsList *lpl)
{
    int count = DP_layer_props_list_count(lpl);
    DP_draw_context_layer_indexes_push(dc);
    for (int i = 0; i < count; ++i) {
        DP_draw_context_layer_indexes_set(dc, i);

        DP_LayerProps *lp = DP_layer_props_list_at_noinc(lpl, i);
        // Hidden layers are supposed to be purely local state. If the remote
        // state gives us hidden layers, we unhide them first. That may get
        // undone by the hidden layers processing that comes after this step.
        bool needs_show = DP_layer_props_hidden(lp);
        bool needs_reveal = reveal_censored && DP_layer_props_censored(lp);
        if (needs_show || needs_reveal) {
            int index_count;
            int *indexes = DP_draw_context_layer_indexes(dc, &index_count);
            DP_TransientLayerProps *tlp =
                DP_layer_routes_entry_indexes_transient_props(index_count,
                                                              indexes, tcs);
            if (needs_show) {
                DP_transient_layer_props_hidden_set(tlp, false);
            }
            if (needs_reveal) {
                DP_transient_layer_props_censored_set(tlp, false);
            }
        }

        DP_LayerPropsList *child_lpl = DP_layer_props_children_noinc(lp);
        if (child_lpl) {
            set_local_layer_props_recursive(tcs, dc, reveal_censored,
                                            child_lpl);
        }
    }
    DP_draw_context_layer_indexes_pop(dc);
}

static void set_hidden_layer_props(DP_PaintEngine *pe,
                                   DP_TransientCanvasState *tcs)
{
    DP_Vector *hidden_layers = &pe->local_view.hidden_layers;
    DP_LayerRoutes *lr = DP_transient_canvas_state_layer_routes_noinc(tcs);
    // Using a while loop since we might purge elements along the way.
    size_t i = 0;
    size_t used = hidden_layers->used;
    while (i < used) {
        int layer_id = DP_VECTOR_AT_TYPE(hidden_layers, int, i);
        DP_LayerRoutesEntry *lre = DP_layer_routes_search(lr, layer_id);
        if (lre) {
            DP_TransientLayerProps *tlp =
                DP_layer_routes_entry_transient_props(lre, tcs);
            DP_transient_layer_props_hidden_set(tlp, true);
            ++i;
        }
        else {
            DP_VECTOR_REMOVE_TYPE(hidden_layers, int, i);
            --used;
        }
    }
}

static DP_CanvasState *set_local_layer_props(DP_PaintEngine *pe,
                                             DP_CanvasState *cs)
{
    DP_TransientCanvasState *tcs = get_or_make_transient_canvas_state(cs);

    bool reveal_censored = pe->local_view.reveal_censored;
    DP_DrawContext *dc = pe->preview_dc;
    DP_draw_context_layer_indexes_clear(dc);
    set_local_layer_props_recursive(
        tcs, dc, reveal_censored,
        DP_transient_canvas_state_layer_props_noinc(tcs));

    set_hidden_layer_props(pe, tcs);

    // We remember the root layer props list for later and then jam it into
    // subsequent canvas states. That'll make them diff correctly instead of
    // reporting a layer props change on every tick.
    DP_layer_props_list_decref_nullable(pe->local_view.lpl);
    pe->local_view.lpl =
        DP_layer_props_list_incref(DP_transient_layer_props_list_persist(
            DP_transient_canvas_state_transient_layer_props(tcs, 0)));
    return (DP_CanvasState *)tcs;
}

static DP_CanvasState *apply_local_layer_props(DP_PaintEngine *pe,
                                               DP_CanvasState *cs)
{
    DP_ViewMode vm = pe->local_view.view_mode;
    DP_LayerPropsList *lpl = DP_canvas_state_layer_props_noinc(cs);
    DP_Timeline *tl = maybe_get_timeline(cs, vm);
    // This function here may replace the layer props entirely, so there can't
    // have been any meddling with it beforehand. Any layer props changes must
    // be part of this function so that they're cached properly.
    DP_ASSERT(!DP_layer_props_list_transient(lpl));
    DP_LayerPropsList *prev_lpl = pe->local_view.prev_lpl;
    DP_Timeline *prev_tl = pe->local_view.prev_tl;
    if (lpl == prev_lpl && tl == prev_tl) {
        // If our local view doesn't have anything to change about the canvas
        // state, we don't need to replace anything in the target state either.
        bool keep_layer_props = vm == DP_VIEW_MODE_NORMAL
                             && !pe->local_view.reveal_censored
                             && pe->local_view.hidden_layers.used == 0;
        if (keep_layer_props) {
            return cs;
        }
        else {
            DP_TransientCanvasState *tcs =
                get_or_make_transient_canvas_state(cs);
            DP_transient_canvas_state_layer_props_set_inc(tcs,
                                                          pe->local_view.lpl);
            return (DP_CanvasState *)tcs;
        }
    }
    else {
        DP_PERF_BEGIN(local, "tick:changes:local");
        if (vm == DP_VIEW_MODE_FRAME && tl != prev_tl) {
            // We're currently viewing the canvas in single-frame mode and the
            // timeline has been manipulated just now. The diff won't pick up
            // that it needs to potentially re-render some tiles, since it
            // doesn't have all that context. We'll just mark all tiles as
            // needing re-rendering instead of complicating the diff logic.
            DP_canvas_diff_check_all(pe->diff);
        }
        DP_layer_props_list_decref_nullable(prev_lpl);
        DP_timeline_decref_nullable(prev_tl);
        pe->local_view.prev_lpl = DP_layer_props_list_incref(lpl);
        pe->local_view.prev_tl = DP_timeline_incref_nullable(tl);
        DP_CanvasState *next_cs = set_local_layer_props(pe, cs);
        DP_PERF_END(local);
        return next_cs;
    }
}

static DP_CanvasState *apply_local_background_tile(DP_PaintEngine *pe,
                                                   DP_CanvasState *cs)
{
    DP_Tile *t = pe->local_view.background_tile;
    if (t) {
        DP_TransientCanvasState *tcs = get_or_make_transient_canvas_state(cs);
        DP_transient_canvas_state_background_tile_set_noinc(
            tcs, DP_tile_incref(t), pe->local_view.background_opaque);
        return (DP_CanvasState *)tcs;
    }
    else {
        return cs;
    }
}

static void
emit_changes(DP_PaintEngine *pe, DP_CanvasState *prev, DP_CanvasState *cs,
             DP_UserCursorBuffer *ucb, DP_PaintEngineResizedFn resized,
             DP_CanvasDiffEachPosFn tile_changed,
             DP_PaintEngineLayerPropsChangedFn layer_props_changed,
             DP_PaintEngineAnnotationsChangedFn annotations_changed,
             DP_PaintEngineDocumentMetadataChangedFn document_metadata_changed,
             DP_PaintEngineTimelineChangedFn timeline_changed,
             DP_PaintEngineCursorMovedFn cursor_moved, void *user)
{
    int prev_width = DP_canvas_state_width(prev);
    int prev_height = DP_canvas_state_height(prev);
    int width = DP_canvas_state_width(cs);
    int height = DP_canvas_state_height(cs);
    if (prev_width != width || prev_height != height) {
        resized(user,
                DP_canvas_state_offset_x(prev) - DP_canvas_state_offset_x(cs),
                DP_canvas_state_offset_y(prev) - DP_canvas_state_offset_y(cs),
                prev_width, prev_height);
    }

    DP_CanvasDiff *diff = pe->diff;
    DP_canvas_state_diff(cs, prev, diff);
    DP_canvas_diff_each_pos(diff, tile_changed, user);

    if (DP_canvas_diff_layer_props_changed_reset(diff)) {
        layer_props_changed(user, DP_canvas_state_layer_props_noinc(cs));
    }

    DP_AnnotationList *al = DP_canvas_state_annotations_noinc(cs);
    if (al != DP_canvas_state_annotations_noinc(prev)) {
        annotations_changed(user, al);
    }

    DP_DocumentMetadata *dm = DP_canvas_state_metadata_noinc(cs);
    if (dm != DP_canvas_state_metadata_noinc(prev)) {
        document_metadata_changed(user, dm);
    }

    DP_Timeline *tl = DP_canvas_state_timeline_noinc(cs);
    if (tl != DP_canvas_state_timeline_noinc(prev)) {
        timeline_changed(user, tl);
    }

    int cursors_count = ucb->count;
    for (int i = 0; i < cursors_count; ++i) {
        DP_UserCursor *uc = &ucb->cursors[i];
        cursor_moved(user, uc->context_id, uc->layer_id, uc->x, uc->y);
    }
}

void DP_paint_engine_tick(
    DP_PaintEngine *pe, DP_PaintEngineCatchupFn catchup,
    DP_PaintEngineRecorderStateChanged recorder_state_changed,
    DP_PaintEngineResizedFn resized, DP_CanvasDiffEachPosFn tile_changed,
    DP_PaintEngineLayerPropsChangedFn layer_props_changed,
    DP_PaintEngineAnnotationsChangedFn annotations_changed,
    DP_PaintEngineDocumentMetadataChangedFn document_metadata_changed,
    DP_PaintEngineTimelineChangedFn timeline_changed,
    DP_PaintEngineCursorMovedFn cursor_moved,
    DP_PaintEngineDefaultLayerSetFn default_layer_set, void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(catchup);
    DP_ASSERT(resized);
    DP_ASSERT(tile_changed);
    DP_ASSERT(layer_props_changed);
    DP_PERF_BEGIN(fn, "tick");

    int progress = DP_atomic_xch(&pe->catchup, -1);
    if (progress != -1) {
        catchup(user, progress);
    }

    switch (pe->record.state_change) {
    case RECORDER_STARTED:
        pe->record.state_change = RECORDER_UNCHANGED;
        recorder_state_changed(user, true);
        break;
    case RECORDER_STOPPED:
        pe->record.state_change = RECORDER_UNCHANGED;
        recorder_state_changed(user, false);
        break;
    default:
        break;
    }

    DP_CanvasState *prev_history_cs = pe->history_cs;
    DP_UserCursorBuffer *ucb = (DP_UserCursorBuffer *)pe->buffers;
    DP_CanvasState *next_history_cs =
        DP_canvas_history_compare_and_get(pe->ch, prev_history_cs, ucb);
    if (next_history_cs) {
        DP_canvas_state_decref(prev_history_cs);
        pe->history_cs = next_history_cs;
    }

    DP_PaintEnginePreview *next_preview =
        DP_atomic_ptr_xch(&pe->next_preview, NULL);
    if (next_preview) {
        free_preview(pe->preview);
        pe->preview = next_preview == &null_preview ? NULL : next_preview;
    }

    bool local_view_changed = !pe->local_view.prev_lpl;

    if (next_history_cs || next_preview || local_view_changed) {
        DP_PERF_BEGIN(changes, "tick:changes");
        // Previews, hidden layers etc. are local changes, so we have to apply
        // them on top of the canvas state we got out of the history.
        DP_CanvasState *prev_view_cs = pe->view_cs;
        DP_CanvasState *next_view_cs = apply_local_background_tile(
            pe, apply_local_layer_props(
                    pe, apply_inspect(pe, apply_preview(pe, pe->history_cs))));
        pe->view_cs = next_view_cs;

        DP_LayerPropsList *lpl =
            DP_canvas_state_layer_props_noinc(next_view_cs);
        if (lpl != DP_canvas_state_layer_props_noinc(prev_view_cs)) {
            pe->local_view.layers_can_decrease_opacity =
                DP_layer_props_list_can_decrease_opacity(lpl);
        }

        emit_changes(pe, prev_view_cs, next_view_cs, ucb, resized, tile_changed,
                     layer_props_changed, annotations_changed,
                     document_metadata_changed, timeline_changed, cursor_moved,
                     user);
        DP_canvas_state_decref(prev_view_cs);
        DP_PERF_END(changes);
    }

    int default_layer_id = DP_atomic_xch(&pe->default_layer_id, -1);
    if (default_layer_id != -1) {
        default_layer_set(user, default_layer_id);
    }

    DP_PERF_END(fn);
}

void DP_paint_engine_prepare_render(DP_PaintEngine *pe,
                                    DP_PaintEngineRenderSizeFn render_size,
                                    void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(render_size);
    DP_CanvasState *cs = pe->view_cs;
    int width = DP_canvas_state_width(cs);
    int height = DP_canvas_state_height(cs);
    render_size(user, width, height);

    DP_TransientLayerContent *tlc = pe->tlc;
    bool render_size_changed = width != DP_transient_layer_content_width(tlc)
                            || height != DP_transient_layer_content_height(tlc);
    if (render_size_changed) {
        DP_transient_layer_content_decref(tlc);
        pe->tlc = DP_transient_layer_content_new_init(width, height, NULL);
    }
}

static void render_pos(void *user, int x, int y)
{
    struct DP_PaintEngineRenderParams *params = user;
    DP_PaintEngine *pe = params->pe;
    ++pe->render.tiles_waiting;
    struct DP_PaintEngineRenderJobParams job_params = {params, x, y};
    DP_worker_push(pe->render.worker, &job_params);
}

static void wait_for_render(DP_PaintEngine *pe)
{
    int n = pe->render.tiles_waiting;
    if (n != 0) {
        pe->render.tiles_waiting = 0;
        DP_SEMAPHORE_MUST_WAIT_N(pe->render.tiles_done_sem, n);
    }
}

static struct DP_PaintEngineRenderParams
make_render_params(DP_PaintEngine *pe, DP_PaintEngineRenderTileFn render_tile,
                   void *user)
{
    // It's very rare in practice for the checkerboard background to actually be
    // visible behind the canvas. Only if the canvas background is set to a
    // non-opaque value or there's weird blend modes like Erase at top-level.
    bool needs_checkers = !DP_canvas_state_background_opaque(pe->view_cs)
                       || pe->local_view.layers_can_decrease_opacity;
    return (struct DP_PaintEngineRenderParams){
        pe, DP_tile_count_round(DP_canvas_state_width(pe->view_cs)),
        needs_checkers, render_tile, user};
}

void DP_paint_engine_render_everything(DP_PaintEngine *pe,
                                       DP_PaintEngineRenderTileFn render_tile,
                                       void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(render_tile);
    DP_PERF_BEGIN(fn, "render:everything");
    struct DP_PaintEngineRenderParams params =
        make_render_params(pe, render_tile, user);
    DP_canvas_diff_each_pos_reset(pe->diff, render_pos, &params);
    wait_for_render(pe);
    DP_PERF_END(fn);
}

void DP_paint_engine_render_tile_bounds(DP_PaintEngine *pe, int tile_left,
                                        int tile_top, int tile_right,
                                        int tile_bottom,
                                        DP_PaintEngineRenderTileFn render_tile,
                                        void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(render_tile);
    DP_PERF_BEGIN(fn, "render:tile_bounds");
    struct DP_PaintEngineRenderParams params =
        make_render_params(pe, render_tile, user);
    DP_canvas_diff_each_pos_tile_bounds_reset(pe->diff, tile_left, tile_top,
                                              tile_right, tile_bottom,
                                              render_pos, &params);
    wait_for_render(pe);
    DP_PERF_END(fn);
}


static void sync_preview(DP_PaintEngine *pe, DP_PaintEnginePreview *preview)
{
    // Make the preview go through the paint engine so that there's less jerking
    // as previews are created and cleared. There may still be flickering, but
    // it won't look like transforms undo themselves for a moment.
    DP_Message *msg = DP_msg_internal_preview_new(0, preview);
    DP_MUTEX_MUST_LOCK(pe->queue_mutex);
    DP_message_queue_push_noinc(&pe->local_queue, msg);
    DP_SEMAPHORE_MUST_POST(pe->queue_sem);
    DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);
}

static void set_preview(DP_PaintEngine *pe, DP_PaintEnginePreview *preview,
                        DP_PaintEnginePreviewRenderFn render,
                        DP_PaintEnginePreviewDisposeFn dispose)
{
    DP_CanvasState *cs = pe->view_cs;
    preview->initial_offset_x = DP_canvas_state_offset_x(cs);
    preview->initial_offset_y = DP_canvas_state_offset_y(cs);
    preview->render = render;
    preview->dispose = dispose;
    sync_preview(pe, preview);
}


static DP_LayerContent *get_cut_preview_content(DP_PaintEngineCutPreview *pecp,
                                                DP_CanvasState *cs,
                                                int offset_x, int offset_y)
{
    DP_LayerContent *lc = pecp->lc;
    int canvas_width = DP_canvas_state_width(cs);
    int canvas_height = DP_canvas_state_height(cs);
    bool needs_render = !lc || DP_layer_content_width(lc) != canvas_width
                     || DP_layer_content_height(lc) != canvas_height;
    if (needs_render) {
        DP_layer_content_decref_nullable(lc);
        DP_TransientLayerContent *tlc = DP_transient_layer_content_new_init(
            canvas_width, canvas_height, NULL);

        int left = pecp->x + offset_x;
        int top = pecp->y + offset_y;
        int width = pecp->width;
        int height = pecp->height;
        int right =
            DP_min_int(DP_transient_layer_content_width(tlc), left + width);
        int bottom =
            DP_min_int(DP_transient_layer_content_height(tlc), top + height);

        if (pecp->have_mask) {
            for (int y = top; y < bottom; ++y) {
                for (int x = left; x < right; ++x) {
                    uint8_t a = pecp->mask[(y - top) * width + (x - left)];
                    if (a != 0) {
                        DP_transient_layer_content_pixel_at_set(
                            tlc, 0, x, y,
                            (DP_Pixel15){0, 0, 0, DP_channel8_to_15(a)});
                    }
                }
            }
        }
        else {
            DP_transient_layer_content_fill_rect(
                tlc, 0, DP_BLEND_MODE_REPLACE, left, top, right, bottom,
                (DP_UPixel15){0, 0, 0, DP_BIT15});
        }

        return pecp->lc = DP_transient_layer_content_persist(tlc);
    }
    else {
        return lc;
    }
}

static DP_CanvasState *cut_preview_render(DP_PaintEnginePreview *preview,
                                          DP_CanvasState *cs,
                                          DP_UNUSED DP_DrawContext *dc,
                                          int offset_x, int offset_y)
{
    DP_PaintEngineCutPreview *pecp = (DP_PaintEngineCutPreview *)preview;
    int layer_id = pecp->layer_id;
    DP_LayerRoutes *lr = DP_canvas_state_layer_routes_noinc(cs);
    DP_LayerRoutesEntry *lre = DP_layer_routes_search(lr, layer_id);
    if (!lre || DP_layer_routes_entry_is_group(lre)) {
        return DP_canvas_state_incref(cs);
    }

    DP_TransientCanvasState *tcs = DP_transient_canvas_state_new(cs);
    if (!pecp->lp) {
        DP_TransientLayerProps *tlp =
            DP_transient_layer_props_new_init(PREVIEW_SUBLAYER_ID, false);
        DP_transient_layer_props_blend_mode_set(tlp, DP_BLEND_MODE_ERASE);
        pecp->lp = DP_transient_layer_props_persist(tlp);
    }

    DP_TransientLayerContent *tlc =
        DP_layer_routes_entry_transient_content(lre, tcs);
    DP_transient_layer_content_sublayer_insert_inc(
        tlc, get_cut_preview_content(pecp, cs, offset_x, offset_y), pecp->lp);
    return (DP_CanvasState *)tcs;
}

static void cut_preview_dispose(DP_PaintEnginePreview *preview)
{
    DP_PaintEngineCutPreview *pecp = (DP_PaintEngineCutPreview *)preview;
    DP_layer_props_decref_nullable(pecp->lp);
    DP_layer_content_decref_nullable(pecp->lc);
}

void DP_paint_engine_preview_cut(DP_PaintEngine *pe, int layer_id, int x, int y,
                                 int width, int height,
                                 const DP_Pixel8 *mask_or_null)
{
    DP_ASSERT(pe);
    int mask_count = mask_or_null ? width * height : 0;
    DP_PaintEngineCutPreview *pecp = DP_malloc(DP_FLEX_SIZEOF(
        DP_PaintEngineCutPreview, mask, DP_int_to_size(mask_count)));
    pecp->lc = NULL;
    pecp->lp = NULL;
    pecp->layer_id = layer_id;
    pecp->x = x;
    pecp->y = y;
    pecp->width = width;
    pecp->height = height;
    pecp->have_mask = mask_or_null;
    for (int i = 0; i < mask_count; ++i) {
        pecp->mask[i] = mask_or_null[i].a;
    }
    set_preview(pe, &pecp->parent, cut_preview_render, cut_preview_dispose);
}


static DP_CanvasState *dabs_preview_render(DP_PaintEnginePreview *preview,
                                           DP_CanvasState *cs,
                                           DP_DrawContext *dc, int offset_x,
                                           int offset_y)
{
    DP_PaintEngineDabsPreview *pedp = (DP_PaintEngineDabsPreview *)preview;
    int layer_id = pedp->layer_id;
    DP_LayerRoutes *lr = DP_canvas_state_layer_routes_noinc(cs);
    DP_LayerRoutesEntry *lre = DP_layer_routes_search(lr, layer_id);
    if (!lre || DP_layer_routes_entry_is_group(lre)) {
        return DP_canvas_state_incref(cs);
    }

    DP_TransientCanvasState *tcs = DP_transient_canvas_state_new(cs);
    DP_TransientLayerContent *tlc =
        DP_layer_routes_entry_transient_content(lre, tcs);
    DP_TransientLayerContent *sub_tlc = NULL;

    int count = pedp->count;
    DP_PaintDrawDabsParams params = {0};
    for (int i = 0; i < count; ++i) {
        DP_Message *msg = pedp->messages[i];
        DP_MessageType type = DP_message_type(msg);
        switch (type) {
        case DP_MSG_DRAW_DABS_CLASSIC: {
            DP_MsgDrawDabsClassic *mddc = DP_message_internal(msg);
            params.origin_x = DP_msg_draw_dabs_classic_x(mddc);
            params.origin_y = DP_msg_draw_dabs_classic_y(mddc);
            params.color = DP_msg_draw_dabs_classic_color(mddc);
            params.blend_mode = DP_msg_draw_dabs_classic_mode(mddc);
            params.indirect = DP_msg_draw_dabs_classic_indirect(mddc);
            params.classic.dabs =
                DP_msg_draw_dabs_classic_dabs(mddc, &params.dab_count);
            break;
        }
        case DP_MSG_DRAW_DABS_PIXEL:
        case DP_MSG_DRAW_DABS_PIXEL_SQUARE: {
            DP_MsgDrawDabsPixel *mddp = DP_message_internal(msg);
            params.origin_x = DP_msg_draw_dabs_pixel_x(mddp);
            params.origin_y = DP_msg_draw_dabs_pixel_y(mddp);
            params.color = DP_msg_draw_dabs_pixel_color(mddp);
            params.blend_mode = DP_msg_draw_dabs_pixel_mode(mddp);
            params.indirect = DP_msg_draw_dabs_pixel_indirect(mddp);
            params.pixel.dabs =
                DP_msg_draw_dabs_pixel_dabs(mddp, &params.dab_count);
            break;
        }
        case DP_MSG_DRAW_DABS_MYPAINT: {
            DP_MsgDrawDabsMyPaint *mddmp = DP_message_internal(msg);
            params.origin_x = DP_msg_draw_dabs_mypaint_x(mddmp);
            params.origin_y = DP_msg_draw_dabs_mypaint_y(mddmp);
            params.color = DP_msg_draw_dabs_mypaint_color(mddmp);
            params.blend_mode = DP_BLEND_MODE_NORMAL_AND_ERASER;
            params.indirect = false;
            params.mypaint.dabs =
                DP_msg_draw_dabs_mypaint_dabs(mddmp, &params.dab_count);
            params.mypaint.lock_alpha =
                DP_msg_draw_dabs_mypaint_lock_alpha(mddmp);
            // TODO colorize and posterize when implemented into the protocol
            break;
        }
        default:
            continue;
        }

        if (params.indirect) {
            if (!sub_tlc) {
                DP_TransientLayerProps *tlp;
                DP_transient_layer_content_transient_sublayer(
                    tlc, PREVIEW_SUBLAYER_ID, &sub_tlc, &tlp);
                DP_transient_layer_props_blend_mode_set(tlp, params.blend_mode);
                DP_transient_layer_props_opacity_set(
                    tlp, DP_channel8_to_15(DP_uint32_to_uint8(
                             (params.color & 0xff000000) >> 24)));
            }
            params.blend_mode = DP_BLEND_MODE_NORMAL;
        }

        params.type = (int)type;
        params.origin_x += offset_x;
        params.origin_y += offset_y;
        DP_paint_draw_dabs(dc, &params, params.indirect ? sub_tlc : tlc);
    }

    return (DP_CanvasState *)tcs;
}

static void dabs_preview_dispose(DP_PaintEnginePreview *preview)
{
    DP_PaintEngineDabsPreview *pedp = (DP_PaintEngineDabsPreview *)preview;
    int count = pedp->count;
    for (int i = 0; i < count; ++i) {
        DP_message_decref(pedp->messages[i]);
    }
}

void DP_paint_engine_preview_dabs_inc(DP_PaintEngine *pe, int layer_id,
                                      int count, DP_Message **messages)
{
    DP_ASSERT(pe);
    if (count > 0) {
        DP_PaintEngineDabsPreview *pedp = DP_malloc(DP_FLEX_SIZEOF(
            DP_PaintEngineDabsPreview, messages, DP_int_to_size(count)));
        pedp->layer_id = layer_id;
        pedp->count = count;
        for (int i = 0; i < count; ++i) {
            pedp->messages[i] = DP_message_incref(messages[i]);
        }
        set_preview(pe, &pedp->parent, dabs_preview_render,
                    dabs_preview_dispose);
    }
}


void DP_paint_engine_preview_clear(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    sync_preview(pe, &null_preview);
}


DP_CanvasState *DP_paint_engine_view_canvas_state_inc(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return DP_canvas_state_incref(pe->view_cs);
}

DP_CanvasState *DP_paint_engine_history_canvas_state_inc(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return DP_canvas_state_incref(pe->history_cs);
}

DP_CanvasState *DP_paint_engine_sample_canvas_state_inc(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return DP_canvas_history_get(pe->ch);
}