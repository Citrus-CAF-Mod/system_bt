/******************************************************************************
 * Copyright (C) 2017, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 ******************************************************************************/
/******************************************************************************
 *
 *  Copyright (C) 2016 The Android Open Source Project
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_a2dp_control"

#include <base/logging.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio_a2dp_hw/include/audio_a2dp_hw.h"
#include "bt_common.h"
#include "btif_a2dp.h"
#include "btif_a2dp_control.h"
#include "btif_a2dp_sink.h"
#include "btif_a2dp_source.h"
#include "btif_av.h"
#include "btif_av_co.h"
#include "btif_hf.h"
#include "osi/include/osi.h"
#include "uipc.h"

#define A2DP_DATA_READ_POLL_MS 10

extern int btif_av_get_latest_device_idx_to_start();
extern int btif_get_is_remote_started_idx();
extern tBTA_AV_HNDL btif_av_get_av_hdl_from_idx(int idx);
extern bool btif_av_is_playing_on_other_idx(int current_index);
extern bool btif_av_is_handoff_set();
extern int btif_max_av_clients;

static void btif_a2dp_data_cb(tUIPC_CH_ID ch_id, tUIPC_EVENT event);
static void btif_a2dp_ctrl_cb(tUIPC_CH_ID ch_id, tUIPC_EVENT event);

/* We can have max one command pending */
static tA2DP_CTRL_CMD a2dp_cmd_pending = A2DP_CTRL_CMD_NONE;
bool is_block_hal_start = false;

void btif_a2dp_control_init(void) {
  UIPC_Init(NULL);
  UIPC_Open(UIPC_CH_ID_AV_CTRL, btif_a2dp_ctrl_cb);
}

void btif_a2dp_control_cleanup(void) {
  /* This calls blocks until UIPC is fully closed */
  UIPC_Close(UIPC_CH_ID_ALL);
}

static void btif_a2dp_recv_ctrl_data(void) {
  tA2DP_CTRL_CMD cmd = A2DP_CTRL_CMD_NONE;
  int n;
  int rs_idx, cur_idx;

  uint8_t read_cmd = 0; /* The read command size is one octet */
  n = UIPC_Read(UIPC_CH_ID_AV_CTRL, NULL, &read_cmd, 1);
  cmd = static_cast<tA2DP_CTRL_CMD>(read_cmd);

  /* detach on ctrl channel means audioflinger process was terminated */
  if (n == 0) {
    APPL_TRACE_WARNING("%s: CTRL CH DETACHED", __func__);
    UIPC_Close(UIPC_CH_ID_AV_CTRL);
    return;
  }

  APPL_TRACE_WARNING("%s: a2dp-ctrl-cmd : %s", __func__,
                     audio_a2dp_hw_dump_ctrl_event(cmd));
  a2dp_cmd_pending = cmd;

  switch (cmd) {
    case A2DP_CTRL_CMD_CHECK_READY:
      if (btif_a2dp_source_media_task_is_shutting_down()) {
        APPL_TRACE_WARNING("%s: A2DP command %s while media task shutting down",
                           __func__, audio_a2dp_hw_dump_ctrl_event(cmd));
        btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
        return;
      }
      /* check for valid AV connection */
      if (btif_av_is_connected()) {
        BTIF_TRACE_DEBUG("%s:Got valid connection",__func__);
        btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      } else {
        BTIF_TRACE_DEBUG("%s:A2dp command %s while valid AV connection",
                         __func__, audio_a2dp_hw_dump_ctrl_event(cmd));
        btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
      }
      break;

    case A2DP_CTRL_CMD_CHECK_STREAM_STARTED:
      if ((btif_av_stream_started_ready() == TRUE))
        btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      else
        btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
      break;

    case A2DP_CTRL_CMD_START:
      /*
       * Don't send START request to stack while we are in a call.
       * Some headsets such as "Sony MW600", don't allow AVDTP START
       * while in a call, and respond with BAD_STATE.
       */
      if (!btif_hf_is_call_vr_idle()) {
        btif_a2dp_command_ack(A2DP_CTRL_ACK_INCALL_FAILURE);
        break;
      }

      if (btif_av_is_handoff_set() && is_block_hal_start) {
        APPL_TRACE_WARNING("%s: A2DP command %s under handoff and HAL Start block",
                           __func__, audio_a2dp_hw_dump_ctrl_event(cmd));
        btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
        break;
      }

      if (btif_a2dp_source_is_streaming()) {
        APPL_TRACE_WARNING("%s: A2DP command %s while source is streaming, return",
                           __func__, audio_a2dp_hw_dump_ctrl_event(cmd));
        btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
        break;
      }

      rs_idx = btif_get_is_remote_started_idx();
      cur_idx = btif_av_get_latest_device_idx_to_start();

      APPL_TRACE_IMP("%s: RS Idx %d, Cur Idx %d, A2DP command %s",
        __func__, rs_idx, cur_idx, audio_a2dp_hw_dump_ctrl_event(cmd));

      if (btif_a2dp_source_is_remote_start()) {
          if (((rs_idx != btif_max_av_clients) && (rs_idx == cur_idx))
                                    || (rs_idx == btif_max_av_clients)){
            btif_a2dp_source_cancel_remote_start();
            if (rs_idx != btif_max_av_clients)
                btif_dispatch_sm_event(
                BTIF_AV_RESET_REMOTE_STARTED_FLAG_UPDATE_AUDIO_STATE_EVT, NULL, 0);
             APPL_TRACE_WARNING("%s: Cancel RS timer for the current index",
                                __func__);
          } else {
            APPL_TRACE_WARNING("%s: RS timer running on other index, ignore",
                               __func__);
          }
      }

      /* In Dual A2dp, first check for started state of stream
       * as we dont want to START again as while doing Handoff
       * the stack state will be started, so it is not needed
       * to send START again, just open the media socket
       * and ACK the audio HAL.
       */
      if (btif_av_stream_started_ready()) {
        if ((rs_idx != btif_max_av_clients) && (rs_idx == cur_idx)) {
             uint8_t hdl = btif_av_get_av_hdl_from_idx(rs_idx);
             APPL_TRACE_DEBUG("%s: setup codec idx %d hdl = %d",
                                                __func__, rs_idx, hdl);
             if (hdl >= 0)
                btif_a2dp_source_setup_codec(hdl);
        }
        UIPC_Open(UIPC_CH_ID_AV_AUDIO, btif_a2dp_data_cb);
        btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
        APPL_TRACE_WARNING("%s: A2DP command %s while AV stream is alreday started",
                         __func__, audio_a2dp_hw_dump_ctrl_event(cmd));
        break;
      } else if (btif_av_stream_ready()) {
        /* Setup audio data channel listener */
        UIPC_Open(UIPC_CH_ID_AV_AUDIO, btif_a2dp_data_cb);
        /*
         * Post start event and wait for audio path to open.
         * If we are the source, the ACK will be sent after the start
         * procedure is completed, othewise send it now.
         */
        btif_dispatch_sm_event(BTIF_AV_START_STREAM_REQ_EVT, NULL, 0);
        int idx = btif_av_get_latest_device_idx_to_start();
        if (btif_av_get_peer_sep(idx) == AVDT_TSEP_SRC)
          btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
        break;
      } else if (btif_av_is_handoff_set() && !(is_block_hal_start)) {
          APPL_TRACE_DEBUG("%s: Entertain Audio Start after stream open", __func__);
          UIPC_Open(UIPC_CH_ID_AV_AUDIO, btif_a2dp_data_cb);
          btif_dispatch_sm_event(BTIF_AV_START_STREAM_REQ_EVT, NULL, 0);
          int idx = btif_av_get_latest_device_idx_to_start();
          if (btif_av_get_peer_sep(idx) == AVDT_TSEP_SRC)
            btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
          break;
      }

      APPL_TRACE_WARNING("%s: A2DP command %s while AV stream is not ready",
                         __func__, audio_a2dp_hw_dump_ctrl_event(cmd));
      btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
      break;

    case A2DP_CTRL_CMD_STOP: {
      int idx = btif_av_get_latest_playing_device_idx();
      if (btif_av_get_peer_sep(idx) == AVDT_TSEP_SNK &&
          !btif_a2dp_source_is_streaming()) {
        /* We are already stopped, just ack back */
        btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
        break;
      }

      btif_dispatch_sm_event(BTIF_AV_STOP_STREAM_REQ_EVT, NULL, 0);
      btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      break;
    }

    case A2DP_CTRL_CMD_SUSPEND:
      /* Local suspend */
      rs_idx = btif_get_is_remote_started_idx();
      cur_idx = btif_av_get_latest_device_idx_to_start();

      APPL_TRACE_IMP("%s: RS Idx %d, Cur Idx %d, A2DP command %s",
                __func__, rs_idx, cur_idx, audio_a2dp_hw_dump_ctrl_event(cmd));

      if (btif_av_stream_started_ready()) {
          if (rs_idx != btif_max_av_clients) {
              bool is_playing_othr_idx = btif_av_is_playing_on_other_idx(rs_idx);
              if (is_playing_othr_idx)
              {
                  APPL_TRACE_DEBUG("%s: Other Idx than RS Idx %d is started",
                                                            __func__, rs_idx);
                  btif_dispatch_sm_event(BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL, 0);
                  break;
              } else {
                  APPL_TRACE_DEBUG("%s: Idx %d is remote started, ACK back",
                                                                __func__, rs_idx);
              }
          } else {
              APPL_TRACE_DEBUG("%s: No idx in remote started, trigger suspend",
                                                                        __func__);
              btif_dispatch_sm_event(BTIF_AV_SUSPEND_STREAM_REQ_EVT, NULL, 0);
              break;
          }
      }
      /* If we are not in started state, just ack back ok and let
       * audioflinger close the channel. This can happen if we are
       * remotely suspended, clear REMOTE SUSPEND flag.
       */
      btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      break;

    case A2DP_CTRL_GET_INPUT_AUDIO_CONFIG: {
      tA2DP_SAMPLE_RATE sample_rate = btif_a2dp_sink_get_sample_rate();
      tA2DP_CHANNEL_COUNT channel_count = btif_a2dp_sink_get_channel_count();

      btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      UIPC_Send(UIPC_CH_ID_AV_CTRL, 0, reinterpret_cast<uint8_t*>(&sample_rate),
                sizeof(tA2DP_SAMPLE_RATE));
      UIPC_Send(UIPC_CH_ID_AV_CTRL, 0, &channel_count,
                sizeof(tA2DP_CHANNEL_COUNT));
      break;
    }

    case A2DP_CTRL_GET_OUTPUT_AUDIO_CONFIG: {
      btav_a2dp_codec_config_t codec_config;
      btav_a2dp_codec_config_t codec_capability;
      codec_config.sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_NONE;
      codec_config.bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_NONE;
      codec_config.channel_mode = BTAV_A2DP_CODEC_CHANNEL_MODE_NONE;
      codec_capability.sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_NONE;
      codec_capability.bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_NONE;
      codec_capability.channel_mode = BTAV_A2DP_CODEC_CHANNEL_MODE_NONE;

      A2dpCodecConfig* current_codec = bta_av_get_a2dp_current_codec();
      if (current_codec != nullptr) {
        codec_config = current_codec->getCodecConfig();
        codec_capability = current_codec->getCodecCapability();
      }

      btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      // Send the current codec config
      UIPC_Send(UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_config.sample_rate),
                sizeof(btav_a2dp_codec_sample_rate_t));
      UIPC_Send(UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_config.bits_per_sample),
                sizeof(btav_a2dp_codec_bits_per_sample_t));
      UIPC_Send(UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_config.channel_mode),
                sizeof(btav_a2dp_codec_channel_mode_t));
      // Send the current codec capability
      UIPC_Send(UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_capability.sample_rate),
                sizeof(btav_a2dp_codec_sample_rate_t));
      UIPC_Send(UIPC_CH_ID_AV_CTRL, 0, reinterpret_cast<const uint8_t*>(
                                           &codec_capability.bits_per_sample),
                sizeof(btav_a2dp_codec_bits_per_sample_t));
      UIPC_Send(UIPC_CH_ID_AV_CTRL, 0, reinterpret_cast<const uint8_t*>(
                                           &codec_capability.channel_mode),
                sizeof(btav_a2dp_codec_channel_mode_t));
      break;
    }

    case A2DP_CTRL_SET_OUTPUT_AUDIO_CONFIG: {
      btav_a2dp_codec_config_t codec_config;
      codec_config.sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_NONE;
      codec_config.bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_NONE;
      codec_config.channel_mode = BTAV_A2DP_CODEC_CHANNEL_MODE_NONE;

      btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      // Send the current codec config
      if (UIPC_Read(UIPC_CH_ID_AV_CTRL, 0,
                    reinterpret_cast<uint8_t*>(&codec_config.sample_rate),
                    sizeof(btav_a2dp_codec_sample_rate_t)) !=
          sizeof(btav_a2dp_codec_sample_rate_t)) {
        APPL_TRACE_ERROR("%s: Error reading sample rate from audio HAL",
                         __func__);
        break;
      }
      if (UIPC_Read(UIPC_CH_ID_AV_CTRL, 0,
                    reinterpret_cast<uint8_t*>(&codec_config.bits_per_sample),
                    sizeof(btav_a2dp_codec_bits_per_sample_t)) !=
          sizeof(btav_a2dp_codec_bits_per_sample_t)) {
        APPL_TRACE_ERROR("%s: Error reading bits per sample from audio HAL",
                         __func__);
        break;
      }
      if (UIPC_Read(UIPC_CH_ID_AV_CTRL, 0,
                    reinterpret_cast<uint8_t*>(&codec_config.channel_mode),
                    sizeof(btav_a2dp_codec_channel_mode_t)) !=
          sizeof(btav_a2dp_codec_channel_mode_t)) {
        APPL_TRACE_ERROR("%s: Error reading channel mode from audio HAL",
                         __func__);
        break;
      }
      APPL_TRACE_DEBUG(
          "%s: A2DP_CTRL_SET_OUTPUT_AUDIO_CONFIG: "
          "sample_rate=0x%x bits_per_sample=0x%x "
          "channel_mode=0x%x",
          __func__, codec_config.sample_rate, codec_config.bits_per_sample,
          codec_config.channel_mode);
      btif_a2dp_source_feeding_update_req(codec_config);
      break;
    }

    case A2DP_CTRL_CMD_OFFLOAD_START: {
      uint8_t hdl = 0;
      int idx = btif_get_is_remote_started_idx();
      if (idx < btif_max_av_clients) {
        hdl = btif_av_get_av_hdl_from_idx(idx);
        APPL_TRACE_DEBUG("%s: hdl = %d",__func__, hdl);
      } else {
        APPL_TRACE_ERROR("%s: Invalid index",__func__);
        break;
      }
      btif_dispatch_sm_event(BTIF_AV_OFFLOAD_START_REQ_EVT, (char *)&hdl, 1);
      break;
    }
    case A2DP_CTRL_GET_SINK_LATENCY: {
      tA2DP_LATENCY sink_latency;

      sink_latency = btif_av_get_sink_latency();
      btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      /* Send sink latency */
      UIPC_Send(UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&sink_latency),
                sizeof(tA2DP_LATENCY));
      break;
    }

    case A2DP_CTRL_CMD_STREAM_OPEN:
      APPL_TRACE_DEBUG("Accept Audio Start after Stream open");
      is_block_hal_start = false;
      btif_a2dp_source_cancel_unblock_audio_start();
      btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      break;

    default:
      APPL_TRACE_ERROR("%s: UNSUPPORTED CMD (%d)", __func__, cmd);
      btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
      break;
  }
  APPL_TRACE_WARNING("%s: a2dp-ctrl-cmd : %s DONE", __func__,
                     audio_a2dp_hw_dump_ctrl_event(cmd));
}

static void btif_a2dp_ctrl_cb(UNUSED_ATTR tUIPC_CH_ID ch_id,
                              tUIPC_EVENT event) {
  APPL_TRACE_WARNING("%s: A2DP-CTRL-CHANNEL EVENT %s", __func__,
                     dump_uipc_event(event));

  switch (event) {
    case UIPC_OPEN_EVT:
      break;

    case UIPC_CLOSE_EVT:
      /* restart ctrl server unless we are shutting down */
      if (btif_a2dp_source_media_task_is_running())
        UIPC_Open(UIPC_CH_ID_AV_CTRL, btif_a2dp_ctrl_cb);
      break;

    case UIPC_RX_DATA_READY_EVT:
      btif_a2dp_recv_ctrl_data();
      break;

    default:
      APPL_TRACE_ERROR("%s: ### A2DP-CTRL-CHANNEL EVENT %d NOT HANDLED ###",
                       __func__, event);
      break;
  }
}

static void btif_a2dp_data_cb(UNUSED_ATTR tUIPC_CH_ID ch_id,
                              tUIPC_EVENT event) {
  APPL_TRACE_WARNING("%s: BTIF MEDIA (A2DP-DATA) EVENT %s", __func__,
                     dump_uipc_event(event));

  switch (event) {
    case UIPC_OPEN_EVT: {
      /*
       * Read directly from media task from here on (keep callback for
       * connection events.
       */
      UIPC_Ioctl(UIPC_CH_ID_AV_AUDIO, UIPC_REG_REMOVE_ACTIVE_READSET, NULL);
      UIPC_Ioctl(UIPC_CH_ID_AV_AUDIO, UIPC_SET_READ_POLL_TMO,
                 reinterpret_cast<void*>(A2DP_DATA_READ_POLL_MS));

      int idx = btif_av_get_latest_playing_device_idx();
      if (btif_av_get_peer_sep(idx) == AVDT_TSEP_SNK) {
        /* Start the media task to encode the audio */
        btif_a2dp_source_start_audio_req();
      }

      /* ACK back when media task is fully started */
      break;
    }

    case UIPC_CLOSE_EVT:
      APPL_TRACE_EVENT("%s: ## AUDIO PATH DETACHED ##", __func__);
      btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      /*
       * Send stop request only if we are actively streaming and haven't
       * received a stop request. Potentially, the audioflinger detached
       * abnormally.
       */
      if (btif_a2dp_source_is_streaming()) {
        /* Post stop event and wait for audio path to stop */
        btif_dispatch_sm_event(BTIF_AV_STOP_STREAM_REQ_EVT, NULL, 0);
      }
      break;

    default:
      APPL_TRACE_ERROR("%s: ### A2DP-DATA EVENT %d NOT HANDLED ###", __func__,
                       event);
      break;
  }
}

void btif_a2dp_command_ack(tA2DP_CTRL_ACK status) {
  uint8_t ack = status;

  APPL_TRACE_WARNING("%s: ## a2dp ack : %s, status %d ##", __func__,
                     audio_a2dp_hw_dump_ctrl_event(a2dp_cmd_pending), status);

  /* Sanity check */
  if (a2dp_cmd_pending == A2DP_CTRL_CMD_NONE) {
    APPL_TRACE_ERROR("%s: warning : no command pending, ignore ack", __func__);
    return;
  }

  /* Clear pending */
  a2dp_cmd_pending = A2DP_CTRL_CMD_NONE;

  /* Acknowledge start request */
  UIPC_Send(UIPC_CH_ID_AV_CTRL, 0, &ack, sizeof(ack));
}
tA2DP_CTRL_CMD btif_a2dp_get_pending_command() {
  return a2dp_cmd_pending;
}
