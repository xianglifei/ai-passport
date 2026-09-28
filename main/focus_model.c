#include "focus_model.h"

#include <stddef.h>

static uint64_t deadline_for(uint64_t now_ms, uint32_t duration_sec) {
    return now_ms + (uint64_t)duration_sec * 1000ULL;
}

void focus_model_init(focus_model_t *model, uint64_t now_ms) {
    (void)now_ms;
    if (!model) return;
    model->state = FOCUS_STATE_HOME;
    model->deadline_ms = 0;
    model->celebrate_end_ms = 0;
    model->duration_sec = 0;
}

bool focus_model_start_work(focus_model_t *model, uint64_t now_ms) {
    if (!model || model->state != FOCUS_STATE_HOME) return false;
    model->duration_sec = FOCUS_WORK_SEC;
    model->deadline_ms = deadline_for(now_ms, FOCUS_WORK_SEC);
    model->state = FOCUS_STATE_WORK_RUNNING;
    return true;
}

bool focus_model_start_rest(focus_model_t *model, uint64_t now_ms) {
    if (!model || model->state != FOCUS_STATE_HOME) return false;
    model->duration_sec = FOCUS_REST_SEC;
    model->deadline_ms = deadline_for(now_ms, FOCUS_REST_SEC);
    model->state = FOCUS_STATE_REST_RUNNING;
    return true;
}

bool focus_model_abort(focus_model_t *model) {
    if (!model) return false;
    if (model->state != FOCUS_STATE_WORK_RUNNING &&
        model->state != FOCUS_STATE_REST_RUNNING) return false;
    model->state = FOCUS_STATE_HOME;
    model->deadline_ms = 0;
    model->duration_sec = 0;
    return true;
}

bool focus_model_skip_celebrate(focus_model_t *model) {
    if (!model || model->state != FOCUS_STATE_CELEBRATE) return false;
    model->state = FOCUS_STATE_HOME;
    model->celebrate_end_ms = 0;
    return true;
}

focus_event_t focus_model_tick(focus_model_t *model, uint64_t now_ms) {
    if (!model) return FOCUS_EVENT_NONE;
    switch (model->state) {
    case FOCUS_STATE_WORK_RUNNING:
        if (now_ms < model->deadline_ms) return FOCUS_EVENT_NONE;
        model->state = FOCUS_STATE_CELEBRATE;
        model->celebrate_end_ms = now_ms + FOCUS_CELEBRATE_MS;
        return FOCUS_EVENT_WORK_DONE;
    case FOCUS_STATE_REST_RUNNING:
        if (now_ms < model->deadline_ms) return FOCUS_EVENT_NONE;
        model->state = FOCUS_STATE_HOME;
        model->deadline_ms = 0;
        model->duration_sec = 0;
        return FOCUS_EVENT_REST_DONE;
    case FOCUS_STATE_CELEBRATE:
        if (now_ms < model->celebrate_end_ms) return FOCUS_EVENT_NONE;
        model->state = FOCUS_STATE_HOME;
        model->celebrate_end_ms = 0;
        return FOCUS_EVENT_CELEBRATE_DONE;
    default:
        return FOCUS_EVENT_NONE;
    }
}

uint32_t focus_model_remaining_sec(const focus_model_t *model, uint64_t now_ms) {
    if (!model || now_ms >= model->deadline_ms) return 0;
    return (uint32_t)((model->deadline_ms - now_ms + 999) / 1000);
}
