/* Copyright (c) 2010 - 2020, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "model_common.h"

#include <stdint.h>

#include "app_timer.h"
#include "app_error.h"
#include "nrf_mesh_assert.h"
#include "nrf_mesh_config_core.h"
#include "nrf_mesh_events.h"
#include "utils.h"
#include "mesh_opt.h"
#include "mesh_config_entry.h"
#include "mesh_stack.h"

typedef struct
{
    uint16_t    light_lightness_instance_count;
    uint16_t    light_lc_instance_count;
    uint16_t    light_ctl_instance_count;
} model_common_metadata_t;

typedef struct
{
    uint8_t is_metadata_stored : 1;
    uint8_t is_load_failed : 1;
} model_common_status_t;

#define TRANSITION_TIME_STEP_RESOLUTION_100MS   (0x00)
#define TRANSITION_TIME_STEP_RESOLUTION_1S      (0x40)
#define TRANSITION_TIME_STEP_RESOLUTION_10S     (0x80)
#define TRANSITION_TIME_STEP_RESOLUTION_10M     (0xC0)

#define TRANSITION_TIME_STEP_MASK               (0xC0)
#define TRANSITION_TIME_STEP_100MS_FACTOR       (100)
#define TRANSITION_TIME_STEP_1S_FACTOR          (1000)
#define TRANSITION_TIME_STEP_10S_FACTOR         (10*1000)
#define TRANSITION_TIME_STEP_10M_FACTOR         (10*60*1000)

/* As defined in @tagMeshMdlSp */
#define TID_VALIDATION_INTERVAL_US              (SEC_TO_US(6))

/* This macro ensures that the remaining timeout is always greater than APP_TIMER_MIN_TIMEOUT_TICKS.*/
#define APP_TIMER_MAX_TIMEOUT                   (MODEL_TIMER_MAX_TIMEOUT_TICKS - (APP_TIMER_MIN_TIMEOUT_TICKS * 2))

#define MODEL_COMMON_METADATA_EID   MESH_CONFIG_ENTRY_ID(MESH_OPT_MODEL_FILE_ID, MESH_APP_MODEL_COMMON_ID)

NRF_MESH_STATIC_ASSERT(MODEL_TIMER_MAX_TIMEOUT_TICKS > APP_TIMER_MAX_TIMEOUT);
NRF_MESH_STATIC_ASSERT(MODEL_TIMER_PERIOD_MS_GET(MODEL_TIMER_TIMEOUT_MIN_TICKS) > 0);

MESH_CONFIG_FILE(m_model_storage, MESH_OPT_MODEL_FILE_ID, MESH_CONFIG_STRATEGY_CONTINUOUS);

static uint32_t model_common_metadata_setter(mesh_config_entry_id_t id, const void * p_entry);
static void     model_common_metadata_getter(mesh_config_entry_id_t id, void * p_entry);

MESH_CONFIG_ENTRY(m_model_common_metadata_entry,
                  MODEL_COMMON_METADATA_EID,
                  1,
                  sizeof(model_common_metadata_t),
                  model_common_metadata_setter,
                  model_common_metadata_getter,
                  NULL,
                  true);

static model_common_status_t m_status;
/** Mesh event handler */
static nrf_mesh_evt_handler_t m_mesh_evt_handler;

static void metadata_store(void)
{
    mesh_config_entry_id_t entry_id = MODEL_COMMON_METADATA_EID;
    model_common_metadata_t metadata =
    {
        .light_lightness_instance_count = LIGHT_LIGHTNESS_SETUP_SERVER_INSTANCES_MAX,
        .light_lc_instance_count = LIGHT_LC_SETUP_SERVER_INSTANCES_MAX,
        .light_ctl_instance_count = LIGHT_CTL_SETUP_SERVER_INSTANCES_MAX,
    };

    NRF_MESH_ERROR_CHECK(mesh_config_entry_set(entry_id, &metadata));
}

static uint32_t timeout_update_and_schedule(model_timer_t * p_timer)
{
    uint32_t status;

    if ((p_timer->remaining_ticks) > MODEL_TIMER_MAX_TIMEOUT_TICKS)
    {
        p_timer->remaining_ticks -= APP_TIMER_MAX_TIMEOUT;
        status = app_timer_start(*p_timer->p_timer_id, APP_TIMER_MAX_TIMEOUT, p_timer);
    }
    else
    {
        status = app_timer_start(*p_timer->p_timer_id, p_timer->remaining_ticks, p_timer);
        p_timer->remaining_ticks = 0;
    }

    /* This should never assert */
    NRF_MESH_ASSERT_DEBUG(status == NRF_SUCCESS);
    return status;
}

static void model_timer_cb(void * p_context)
{
    model_timer_t * p_timer = (model_timer_t *) p_context;

    NRF_MESH_ASSERT(p_timer->cb != NULL);

    p_timer->total_rtc_ticks += app_timer_cnt_diff_compute(app_timer_cnt_get(), p_timer->last_rtc_stamp);
    p_timer->last_rtc_stamp = app_timer_cnt_get();

    if (p_timer->remaining_ticks == 0)
    {
        /* Trigger callback and repeat if required */
        p_timer->cb_active = true;
        p_timer->cb(p_timer->p_context);
        p_timer->cb_active = false;

        if (p_timer->mode == MODEL_TIMER_MODE_REPEATED)
        {
            p_timer->remaining_ticks = p_timer->timeout_rtc_ticks;
        }
    }

    /* Continue if time is left */
    if (p_timer->remaining_ticks > 0)
    {
        (void) timeout_update_and_schedule(p_timer);
    }
}

static void model_tid_timer_cb(timestamp_t timestamp, void * p_context)
{
    tid_tracker_t * p_item = (tid_tracker_t *) p_context;
    p_item->tid_expiry_timer.cb = NULL;
}

static void mesh_evt_handler(const nrf_mesh_evt_t * p_evt)
{
    switch (p_evt->type)
    {
        case NRF_MESH_EVT_CONFIG_LOAD_FAILURE:
            if (p_evt->params.config_load_failure.id.file == MESH_OPT_MODEL_FILE_ID)
            {
                m_status.is_load_failed = 1;
            }
            break;

        default:
            break;
    }
}

/* Setter and getter definitions.
 */
static uint32_t model_common_metadata_setter(mesh_config_entry_id_t id, const void * p_entry)
{
    NRF_MESH_ASSERT_DEBUG(MESH_APP_MODEL_COMMON_ID == id.record);

    const model_common_metadata_t * p_metadata = (const model_common_metadata_t *) p_entry;

    if ((p_metadata->light_lightness_instance_count == LIGHT_LIGHTNESS_SETUP_SERVER_INSTANCES_MAX) &&
        (p_metadata->light_lc_instance_count == LIGHT_LC_SETUP_SERVER_INSTANCES_MAX) &&
        (p_metadata->light_ctl_instance_count == LIGHT_CTL_SETUP_SERVER_INSTANCES_MAX))
    {
        m_status.is_metadata_stored = 1;
    }
    else
    {
        return NRF_ERROR_INVALID_DATA;
    }

    return NRF_SUCCESS;
}

static void model_common_metadata_getter(mesh_config_entry_id_t id, void * p_entry)
{
    NRF_MESH_ASSERT_DEBUG(MESH_APP_MODEL_COMMON_ID == id.record);

    model_common_metadata_t * p_metadata = (model_common_metadata_t *) p_entry;
    p_metadata->light_lightness_instance_count = LIGHT_LIGHTNESS_SETUP_SERVER_INSTANCES_MAX;
    p_metadata->light_lc_instance_count = LIGHT_LC_SETUP_SERVER_INSTANCES_MAX;
    p_metadata->light_ctl_instance_count = LIGHT_CTL_SETUP_SERVER_INSTANCES_MAX;
}


/* Weak functions declearation */
__WEAK void light_lightness_mc_init(void)
{}

__WEAK void light_lc_mc_init(void)
{}

__WEAK void light_ctl_mc_init(void)
{}

__WEAK void light_lightness_mc_clear(void)
{}

__WEAK void light_lc_mc_clear(void)
{}

__WEAK void light_ctl_mc_clear(void)
{}

/* Public APIs for models */
bool model_tid_validate(tid_tracker_t * p_tid_tracker, const access_message_rx_meta_t * p_meta,
                        uint32_t message_id, uint8_t tid)
{
    if (p_tid_tracker->src != p_meta->src.value ||
        p_tid_tracker->dst != p_meta->dst.value ||
        p_tid_tracker->old_tid != tid ||
        p_tid_tracker->message_id != message_id ||
        p_tid_tracker->tid_expiry_timer.cb == NULL)
    {
        p_tid_tracker->src = p_meta->src.value;
        p_tid_tracker->dst = p_meta->dst.value;
        p_tid_tracker->message_id = message_id;
        p_tid_tracker->old_tid = tid;

        p_tid_tracker->tid_expiry_timer.interval = 0;
        p_tid_tracker->tid_expiry_timer.cb = model_tid_timer_cb;
        p_tid_tracker->tid_expiry_timer.p_context = p_tid_tracker;
        timer_sch_reschedule(&p_tid_tracker->tid_expiry_timer, timer_now() + TID_VALIDATION_INTERVAL_US);

        p_tid_tracker->new_transaction = true;
    }
    else
    {
        p_tid_tracker->new_transaction = false;
    }

    return p_tid_tracker->new_transaction;
}

bool model_transaction_is_new(tid_tracker_t * p_tid_tracker)
{
    return p_tid_tracker->new_transaction;
}

uint32_t model_transition_time_decode(uint8_t enc_transition_time)
{
    uint32_t time = 0;

    if ((enc_transition_time & ~TRANSITION_TIME_STEP_MASK) == TRANSITION_TIME_UNKNOWN)
    {
        return MODEL_TRANSITION_TIME_UNKNOWN;
    }

    switch(enc_transition_time & TRANSITION_TIME_STEP_MASK)
    {
        case TRANSITION_TIME_STEP_RESOLUTION_100MS:
            time = (enc_transition_time & ~TRANSITION_TIME_STEP_MASK) * TRANSITION_TIME_STEP_100MS_FACTOR;
        break;

        case TRANSITION_TIME_STEP_RESOLUTION_1S:
            time = (enc_transition_time & ~TRANSITION_TIME_STEP_MASK) * TRANSITION_TIME_STEP_1S_FACTOR;
        break;

        case TRANSITION_TIME_STEP_RESOLUTION_10S:
            time = (enc_transition_time & ~TRANSITION_TIME_STEP_MASK) * TRANSITION_TIME_STEP_10S_FACTOR;
        break;

        case TRANSITION_TIME_STEP_RESOLUTION_10M:
            time = (enc_transition_time & ~TRANSITION_TIME_STEP_MASK) * TRANSITION_TIME_STEP_10M_FACTOR;
        break;

        default:
            break;
    }

    return time;
}


uint8_t model_transition_time_encode(uint32_t transition_time)
{
    uint8_t enc_time = TRANSITION_TIME_UNKNOWN;

    if(transition_time <= TRANSITION_TIME_STEP_100MS_MAX)
    {
        enc_time = (transition_time / TRANSITION_TIME_STEP_100MS_FACTOR) | TRANSITION_TIME_STEP_RESOLUTION_100MS;
    }
    else if (transition_time <= TRANSITION_TIME_STEP_1S_MAX)
    {
        enc_time = (transition_time / TRANSITION_TIME_STEP_1S_FACTOR) | TRANSITION_TIME_STEP_RESOLUTION_1S;
    }
    else if (transition_time <= TRANSITION_TIME_STEP_10S_MAX)
    {
        enc_time = (transition_time / TRANSITION_TIME_STEP_10S_FACTOR) | TRANSITION_TIME_STEP_RESOLUTION_10S;
    }
    else if (transition_time <= TRANSITION_TIME_STEP_10M_MAX)
    {
        enc_time = (transition_time / TRANSITION_TIME_STEP_10M_FACTOR) | TRANSITION_TIME_STEP_RESOLUTION_10M;
    }

    return enc_time;
}

bool model_transition_time_is_valid(uint8_t enc_transition_time)
{
    return ((enc_transition_time & ~TRANSITION_TIME_STEP_MASK) != TRANSITION_TIME_UNKNOWN);
}

uint32_t model_delay_decode(uint8_t enc_delay)
{
    return (enc_delay * DELAY_TIME_STEP_FACTOR_MS);
}

uint8_t model_delay_encode(uint32_t delay)
{
    if (delay > DELAY_TIME_STEP_MAX)
    {
        return DELAY_TIME_STEP_MAX;
    }
    else
    {
        return (delay/DELAY_TIME_STEP_FACTOR_MS);
    }
}

uint64_t model_timer_elapsed_ticks_get(model_timer_t * p_timer)
{
    return (p_timer->total_rtc_ticks);
}

uint32_t model_timer_schedule(model_timer_t * p_timer)
{

    if (p_timer == NULL || p_timer->cb == NULL)
    {
        return NRF_ERROR_NULL;
    }

    if (p_timer->timeout_rtc_ticks < APP_TIMER_MIN_TIMEOUT_TICKS)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    (void) app_timer_stop(*p_timer->p_timer_id);

    p_timer->remaining_ticks = p_timer->timeout_rtc_ticks;
    p_timer->last_rtc_stamp = app_timer_cnt_get();
    p_timer->total_rtc_ticks = 0;

    if (!p_timer->cb_active)
    {
        return timeout_update_and_schedule(p_timer);
    }

    return NRF_SUCCESS;
}

void model_timer_abort(model_timer_t * p_timer)
{
    NRF_MESH_ASSERT(p_timer != NULL);

    (void) app_timer_stop(*p_timer->p_timer_id);
    p_timer->remaining_ticks = 0;
    p_timer->timeout_rtc_ticks = 0;
    p_timer->total_rtc_ticks = 0;
}

uint32_t model_timer_create(model_timer_t * p_timer)
{
    if (p_timer == NULL || p_timer->cb == NULL)
    {
        return NRF_ERROR_NULL;
    }

    p_timer->cb_active = false;

    /* For simplicity, always operate app_timer in single shot mode */
    return (app_timer_create(p_timer->p_timer_id, APP_TIMER_MODE_SINGLE_SHOT, model_timer_cb));
}

void model_common_init(void)
{
    m_mesh_evt_handler.evt_cb = mesh_evt_handler;
    nrf_mesh_evt_handler_add(&m_mesh_evt_handler);

    m_status.is_load_failed = 0;

    light_lightness_mc_init();
    light_lc_mc_init();
    light_ctl_mc_init();
}

uint32_t model_common_config_apply(void)
{
    if (m_status.is_load_failed)
    {
        /* Loading of the model failed in some way, so the stack config also need to be cleared. */
        mesh_stack_config_clear();

        light_lightness_mc_clear();
        light_lc_mc_clear();
        light_ctl_mc_clear();

        m_status.is_metadata_stored = 0;
        (void) mesh_config_entry_delete(MODEL_COMMON_METADATA_EID);

        metadata_store();
        return NRF_ERROR_INVALID_DATA;
    }

    if (!m_status.is_metadata_stored)
    {
        /* Store default values */
        metadata_store();
    }

    return NRF_SUCCESS;
}
