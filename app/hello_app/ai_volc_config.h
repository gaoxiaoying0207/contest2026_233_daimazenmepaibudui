/****************************************************************************
 * Volcengine Voice Credentials
 * 火山引擎语音凭据 - 在此填写后编译，开机自动写入配置
 *
 * 获取方式：
 *   API Key    → https://console.volcengine.com/speech/new/setting/apikeys
 *   AppID      → https://console.volcengine.com/speech/ 创建应用
 *   Token      → 同上
 *   Cluster    → 默认 volcengine_streaming_common
 *   Speaker    → https://www.volcengine.com/docs/6561/97465
 ****************************************************************************/

#ifndef __AI_VOLC_CONFIG_H
#define __AI_VOLC_CONFIG_H

/* ---- 在下方填入你的凭据 ---- */

#define VOLC_API_KEY        ""   /* 火山引擎 API Key */
#define VOLC_APPKEY         ""   /* 语音应用 AppID */
#define VOLC_TOKEN          ""   /* 语音应用 Token */
#define VOLC_ASR_CLUSTER    "volcengine_streaming_common"
#define VOLC_TTS_CLUSTER    "volcengine_streaming_common"
#define VOLC_SPEAKER        "zh_female_cancan"

/* ---- 以下无需修改 ---- */

#define VOLC_CFG_KEY_API_KEY    "volc_api_key"
#define VOLC_CFG_KEY_APPKEY     "volc_appkey"
#define VOLC_CFG_KEY_TOKEN      "volc_token"
#define VOLC_CFG_KEY_ASR_CLUSTER "volc_asr_cluster"
#define VOLC_CFG_KEY_TTS_CLUSTER "volc_cluster"
#define VOLC_CFG_KEY_SPEAKER    "volc_speaker"

#endif /* __AI_VOLC_CONFIG_H */
