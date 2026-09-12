/****************************************************************************
 * AI Audio Module Implementation
 * 智爱陪伴 - AI老人陪伴守护终端
 * 音频模块 - 麦克风录音与音频播放
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ai_audio.h"
#include <sys/ioctl.h>
#include <nuttx/audio/audio.h>

/* AI_AUDIO_REAL_DEVICE_V1
 * Lifecycle API: one controlling thread per context.
 * Callbacks must not start another operation or deinitialize the context.
 * STOP cancellation of a permanently blocked driver is not implemented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 音频设备路径 */
#define AUDIO_RECORD_DEVICE "/dev/audio/audio0"
#define AUDIO_PLAY_DEVICE "/dev/audio/audio0"

/* 缓冲区大小计算 */
#define AUDIO_BUF_SIZE(frames, channels, bps) \
    ((frames) * (channels) * (bps) / 8)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void *audio_record_thread(void *arg);
static void *audio_play_thread(void *arg);
static uint32_t audio_calc_frame_energy(const int16_t *data, size_t frames);
static int audio_open_record_device(audio_context_t *ctx);
static int audio_open_play_device(audio_context_t *ctx);
static void audio_close_record_device(audio_context_t *ctx);
static void audio_close_play_device(audio_context_t *ctx);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 状态名称表 */
static const char *g_state_names[] =
{
  [AUDIO_STATE_UNINIT]     = "UNINIT",
  [AUDIO_STATE_IDLE]       = "IDLE",
  [AUDIO_STATE_RECORDING]  = "RECORDING",
  [AUDIO_STATE_PLAYING]    = "PLAYING",
  [AUDIO_STATE_BOTH]       = "BOTH"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief  计算单帧能量
 */

static uint32_t audio_calc_frame_energy(const int16_t *data, size_t frames)
{
  uint64_t sum = 0;

  for (size_t i = 0; i < frames; i++)
    {
      sum += (int64_t)data[i] * data[i];
    }

  return (uint32_t)(sum / frames);
}

/**
 * @brief  打开录音设备
 */


static int audio_hw_volume(int fd, uint8_t percent)
{
  struct audio_caps_desc_s desc;
  memset(&desc, 0, sizeof(desc));
  desc.caps.ac_len = sizeof(struct audio_caps_s);
  desc.caps.ac_type = AUDIO_TYPE_FEATURE;
  desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
  desc.caps.ac_controls.hw[0] = (uint16_t)percent * 10;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&desc) < 0)
    {
      return -errno;
    }

  return 0;
}

static int audio_hw_open(bool output, uint8_t volume)
{
  const char *path = output ? AUDIO_PLAY_DEVICE : AUDIO_RECORD_DEVICE;
  int fd = open(path, output ? O_WRONLY : O_RDONLY);
  int ret;
  struct audio_caps_desc_s desc;

  if (fd < 0)
    {
      ret = -errno;
      printf("[AUDIO] open %s failed: %d\n", path, ret);
      return ret;
    }

  memset(&desc, 0, sizeof(desc));
  desc.caps.ac_len = sizeof(struct audio_caps_s);
  desc.caps.ac_type = output ? AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
  desc.caps.ac_channels = 1;
  desc.caps.ac_controls.hw[0] = 16000;
  desc.caps.ac_controls.b[2] = 16;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&desc) < 0)
    {
      ret = -errno;
      goto fail;
    }

  /* Only set speaker volume on the playback path. */
  if (output)
    {
      ret = audio_hw_volume(fd, volume);
      if (ret < 0)
        {
          goto fail;
        }
    }

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      ret = -errno;
      goto fail;
    }

  return fd;

fail:
  printf("[AUDIO] configure/start failed: %d\n", ret);
  close(fd);
  return ret;
}

static int audio_open_record_device(audio_context_t *ctx)
{
  int fd = audio_hw_open(false, ctx->config.volume);
  if (fd < 0)
    {
      return fd;
    }

  ctx->record_fd = fd;
  return 0;
}

/**
 * @brief  打开播放设备
 */

static int audio_open_play_device(audio_context_t *ctx)
{
  int fd = audio_hw_open(true, ctx->config.volume);
  if (fd < 0)
    {
      return fd;
    }

  ctx->play_fd = fd;
  return 0;
}

/**
 * @brief  关闭录音设备
 */

static void audio_close_record_device(audio_context_t *ctx)
{
  if (ctx->record_fd >= 0)
    {
      if (ioctl(ctx->record_fd, AUDIOIOC_STOP, 0) < 0)
        {
          printf("[AUDIO] record STOP failed: %d\n", errno);
        }

      close(ctx->record_fd);
      ctx->record_fd = -1;
    }
}

/**
 * @brief  关闭播放设备
 */

static void audio_close_play_device(audio_context_t *ctx)
{
  if (ctx->play_fd >= 0)
    {
      if (ioctl(ctx->play_fd, AUDIOIOC_STOP, 0) < 0)
        {
          printf("[AUDIO] play STOP failed: %d\n", errno);
        }

      close(ctx->play_fd);
      ctx->play_fd = -1;
    }
}

/**
 * @brief  录音线程
 */

static void *audio_record_thread(void *arg)
{
  audio_context_t *ctx = (audio_context_t *)arg;
  size_t frames_per_read;
  ssize_t nbytes;

  AUDIO_DEBUG("录音线程启动");

  /* 每次读取的帧数 */

  frames_per_read = ctx->config.sample_rate * ctx->config.frame_ms / 1000;

  while (!__atomic_load_n(&ctx->record_stop, __ATOMIC_ACQUIRE))
    {

      size_t wanted = frames_per_read * sizeof(int16_t);
      size_t received = 0;
      int read_error = 0;

      while (received < wanted &&
             !__atomic_load_n(&ctx->record_stop, __ATOMIC_ACQUIRE))
        {
          ssize_t n = read(ctx->record_fd,
                           (uint8_t *)ctx->record_buf + received,
                           wanted - received);

          if (n < 0)
            {
              if (errno == EINTR)
                {
                  continue;
                }

              read_error = errno;
              break;
            }

          if (n == 0)
            {
              read_error = EIO;
              break;
            }

          received += (size_t)n;
        }

      if (read_error != 0)
        {
          printf("[AUDIO] record read failed: %d\n", read_error);
          break;
        }

      if (__atomic_load_n(&ctx->record_stop, __ATOMIC_ACQUIRE))
        {
          break;
        }

      nbytes = (ssize_t)received;

      if (nbytes > 0)
        {
          size_t frames_read = nbytes / (2 * ctx->config.channels);
          size_t samples_read = frames_read * ctx->config.channels;

          /* VAD检测 */

          if (ctx->vad_enabled)
            {
              uint32_t energy = audio_calc_frame_energy(
                ctx->record_buf, samples_read);

              if (energy > ctx->vad_energy_threshold)
                {
                  /* 检测到语音 */

                  ctx->vad_silence_frames = 0;
                  ctx->vad_speech_frames++;

                  uint32_t min_speech_frames =
                    (ctx->record_cfg.min_speech_ms +
                     ctx->config.frame_ms - 1) / ctx->config.frame_ms;

                  if (!ctx->vad_speech_active &&
                      ctx->vad_speech_frames >= min_speech_frames)
                    {
                      AUDIO_DEBUG("VAD: 检测到语音开始 (能量=%lu)",
                                  (unsigned long)energy);
                      ctx->vad_speech_active = true;

                      if (ctx->vad_callback)
                        {
                          ctx->vad_callback(true, ctx->vad_user_data);
                        }
                    }
                }
              else
                {
                  /* 静音 */

                  ctx->vad_speech_frames = 0;
                  ctx->vad_silence_frames++;

                  uint32_t silence_frames =
                    (ctx->record_cfg.silence_timeout_ms +
                     ctx->config.frame_ms - 1) / ctx->config.frame_ms;

                  if (ctx->vad_speech_active &&
                      ctx->vad_silence_frames >= silence_frames)
                    {
                      AUDIO_DEBUG("VAD: 语音结束 (静音超时)");
                      ctx->vad_speech_active = false;

                      if (ctx->vad_callback)
                        {
                          ctx->vad_callback(false, ctx->vad_user_data);
                        }
                    }
                }
            }


          /* 调用数据回调 */

          if (ctx->record_cfg.data_callback)
            {
              ctx->record_cfg.data_callback(ctx->record_buf,
                                            frames_read,
                                            ctx->record_cfg.user_data);
            }
        }
      else
        {
          AUDIO_DEBUG("录音读取失败: %zd", nbytes);
          break;
        }
    }

  audio_close_record_device(ctx);
  AUDIO_DEBUG("录音线程退出");
  ctx->recording = false;
  ctx->state = ctx->playing ? AUDIO_STATE_PLAYING : AUDIO_STATE_IDLE;
  return NULL;
}

/**
 * @brief  播放线程
 */

static void *audio_play_thread(void *arg)
{
  audio_context_t *ctx = arg;
  size_t total = ctx->play_frames * sizeof(int16_t);
  size_t offset = 0;
  int error = 0;

  while (offset < total &&
         !__atomic_load_n(&ctx->play_stop, __ATOMIC_ACQUIRE))
    {
      size_t chunk = total - offset;
      if (chunk > ctx->record_buf_size)
        {
          chunk = ctx->record_buf_size;
        }

      ssize_t n = write(ctx->play_fd,
                        (const uint8_t *)ctx->play_buf + offset,
                        chunk);

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          error = errno;
          break;
        }

      if (n == 0)
        {
          error = EIO;
          break;
        }

      offset += (size_t)n;
    }

  audio_close_play_device(ctx);

  if (error != 0)
    {
      printf("[AUDIO] playback failed: %d, wrote %lu/%lu bytes\n",
             error, (unsigned long)offset, (unsigned long)total);
    }

  /* Callback runs before the worker is marked inactive.
   * It must only notify the controlling thread.
   */
  if (error == 0 && offset == total &&
      !__atomic_load_n(&ctx->play_stop, __ATOMIC_ACQUIRE) &&
      ctx->play_cb != NULL)
    {
      ctx->play_cb(ctx->play_user_data);
    }

  ctx->playing = false;
  ctx->state = AUDIO_STATE_IDLE;
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief  初始化音频模块
 */

int audio_init(audio_context_t *ctx, const audio_config_t *config)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  AUDIO_DEBUG("初始化音频模块");

  /* 清零上下文 */

  memset(ctx, 0, sizeof(audio_context_t));

  /* 设置默认配置 */

  if (config != NULL)
    {
      memcpy(&ctx->config, config, sizeof(audio_config_t));
    }
  else
    {
      ctx->config.sample_rate = AUDIO_DEFAULT_SAMPLE_RATE;
      ctx->config.channels = AUDIO_DEFAULT_CHANNELS;
      ctx->config.format = AUDIO_FORMAT_S16_LE;
      ctx->config.frame_ms = AUDIO_DEFAULT_FRAME_MS;
      ctx->config.volume = AI_AUDIO_VOLUME_DEFAULT;
    }


  if (ctx->config.sample_rate != AUDIO_RATE_16K ||
      ctx->config.channels != AUDIO_CH_MONO ||
      ctx->config.format != AUDIO_FORMAT_S16_LE ||
      ctx->config.frame_ms != AUDIO_DEFAULT_FRAME_MS ||
      ctx->config.volume > AI_AUDIO_VOLUME_MAX)
    {
      return -EINVAL;
    }

  /* 初始化设备文件描述符 */

  ctx->record_fd = -1;
  ctx->play_fd = -1;

  /* 初始化VAD参数 */

  ctx->vad_energy_threshold = AUDIO_VAD_ENERGY_THRESHOLD;

  /* 分配缓冲区 */

  size_t frame_bytes = ctx->config.channels * sizeof(int16_t);
  size_t frames_per_period =
    (size_t)ctx->config.sample_rate * ctx->config.frame_ms / 1000;

  ctx->record_buf_size = frames_per_period * frame_bytes;
  ctx->record_buf = (int16_t *)calloc(1, ctx->record_buf_size);
  if (ctx->record_buf == NULL)
    {
      AUDIO_DEBUG("分配录音缓冲区失败");
      return -ENOMEM;
    }

  ctx->play_buf_size =
    (size_t)ctx->config.sample_rate * AUDIO_PLAY_BUFFER_MS / 1000 *
    frame_bytes;
  ctx->play_buf = (int16_t *)calloc(1, ctx->play_buf_size);
  if (ctx->play_buf == NULL)
    {
      AUDIO_DEBUG("分配播放缓冲区失败");
      free(ctx->record_buf);
      return -ENOMEM;
    }

  ctx->state = AUDIO_STATE_IDLE;
  ctx->initialized = true;

  AUDIO_DEBUG("音频模块初始化完成");
  AUDIO_DEBUG("  采样率: %d Hz", ctx->config.sample_rate);
  AUDIO_DEBUG("  通道数: %d", ctx->config.channels);
  AUDIO_DEBUG("  帧长: %d ms", ctx->config.frame_ms);

  return OK;
}

/**
 * @brief  反初始化音频模块
 */

void audio_deinit(audio_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return;
    }


  if ((ctx->record_thread_valid &&
       pthread_equal(pthread_self(), ctx->record_thread)) ||
      (ctx->play_thread_valid &&
       pthread_equal(pthread_self(), ctx->play_thread)))
    {
      printf("[AUDIO] deinit must run on the controlling thread\n");
      return;
    }

  AUDIO_DEBUG("反初始化音频模块");


  /* 停止录音和播放 */

  audio_record_stop(ctx);
  audio_play_stop(ctx);

  /* 关闭设备 */

  audio_close_record_device(ctx);
  audio_close_play_device(ctx);

  /* 释放缓冲区 */

  if (ctx->record_buf != NULL)
    {
      free(ctx->record_buf);
      ctx->record_buf = NULL;
    }

  if (ctx->play_buf != NULL)
    {
      free(ctx->play_buf);
      ctx->play_buf = NULL;
    }

  ctx->initialized = false;
  ctx->state = AUDIO_STATE_UNINIT;

  AUDIO_DEBUG("音频模块已反初始化");
}

/**
 * @brief  开始录音
 */

int audio_record_start(audio_context_t *ctx,
                       const audio_record_config_t *config)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return -EINVAL;
    }

  if (ctx->recording || ctx->playing)
    {
      AUDIO_DEBUG("已在录音中");
      return -EBUSY;
    }

  if (ctx->record_thread_valid)
    {
      pthread_join(ctx->record_thread, NULL);
      ctx->record_thread_valid = false;
    }

  AUDIO_DEBUG("开始录音");

  /* 保存录音配置 */

  if (config != NULL)
    {
      memcpy(&ctx->record_cfg, config, sizeof(audio_record_config_t));
    }
  else
    {
      memset(&ctx->record_cfg, 0, sizeof(audio_record_config_t));
    }

  if (ctx->record_cfg.silence_timeout_ms == 0)
    {
      ctx->record_cfg.silence_timeout_ms = AUDIO_VAD_SILENCE_TIMEOUT_MS;
    }

  if (ctx->record_cfg.min_speech_ms == 0)
    {
      ctx->record_cfg.min_speech_ms = AUDIO_VAD_MIN_SPEECH_MS;
    }

  /* 打开录音设备 */

  int ret = audio_open_record_device(ctx);
  if (ret < 0)
    {
      AUDIO_DEBUG("打开录音设备失败: %d", ret);
      return ret;
    }

  /* 启用VAD */

  if (ctx->record_cfg.enable_vad)
    {
      ctx->vad_enabled = true;
      ctx->vad_speech_active = false;
      ctx->vad_silence_frames = 0;
      ctx->vad_speech_frames = 0;
    }

  /* 启动录音线程 */


  __atomic_store_n(&ctx->record_stop, false, __ATOMIC_RELEASE);
  ctx->recording = true;

  ctx->state = AUDIO_STATE_RECORDING;

  ret = pthread_create(&ctx->record_thread, NULL,
                       audio_record_thread, ctx);
  if (ret != 0)
    {
      AUDIO_DEBUG("创建录音线程失败: %d", ret);
      ctx->recording = false;
      audio_close_record_device(ctx);
      ctx->state = AUDIO_STATE_IDLE;
      return -ret;
    }

  ctx->record_thread_valid = true;

  /* 更新状态 */



  return OK;
}

/**
 * @brief  停止录音
 */

void audio_record_stop(audio_context_t *ctx)
{
  if (ctx == NULL || !ctx->record_thread_valid)
    {
      return;
    }

  AUDIO_DEBUG("停止录音");

  /* 设置停止标志 */

  __atomic_store_n(&ctx->record_stop, true, __ATOMIC_RELEASE);

  if (pthread_equal(pthread_self(), ctx->record_thread))
    {
      return;
    }

  /* 等待线程退出 */

  pthread_join(ctx->record_thread, NULL);
  ctx->record_thread_valid = false;

  /* 关闭设备 */

  audio_close_record_device(ctx);

  ctx->recording = false;

  /* 更新状态 */

  ctx->state = ctx->playing ? AUDIO_STATE_PLAYING : AUDIO_STATE_IDLE;
}

/**
 * @brief  是否正在录音
 */

bool audio_is_recording(audio_context_t *ctx)
{
  return (ctx != NULL) ? ctx->recording : false;
}

/**
 * @brief  开始播放音频数据
 */

int audio_play_start(audio_context_t *ctx,
                     const int16_t *data, size_t frames,
                     audio_play_complete_cb_t callback,
                     void *user_data)
{
  if (ctx == NULL || !ctx->initialized || data == NULL || frames == 0)
    {
      return -EINVAL;
    }

  if (ctx->playing || ctx->recording)
    {
      AUDIO_DEBUG("已在播放中");
      return -EBUSY;
    }

  if (ctx->play_thread_valid)
    {
      pthread_join(ctx->play_thread, NULL);
      ctx->play_thread_valid = false;
    }

  AUDIO_DEBUG("开始播放 (%zu 帧)", frames);

  /* 保存回调 */

  ctx->play_cb = callback;
  ctx->play_user_data = user_data;

  /* 复制音频数据到缓冲区 */

  size_t frame_bytes = ctx->config.channels * sizeof(int16_t);
  size_t capacity_frames = ctx->play_buf_size / frame_bytes;
  if (frames > capacity_frames)
    {
      return -ENOSPC;
    }

  size_t copy_bytes = frames * frame_bytes;

  if (data != ctx->play_buf && ctx->play_buf != NULL)
    {
      memcpy(ctx->play_buf, data, copy_bytes);
    }

  ctx->play_frames = frames;

  /* 打开播放设备 */

  int ret = audio_open_play_device(ctx);
  if (ret < 0)
    {
      AUDIO_DEBUG("打开播放设备失败: %d", ret);
      return ret;
    }

  /* 启动播放线程 */


  __atomic_store_n(&ctx->play_stop, false, __ATOMIC_RELEASE);
  ctx->playing = true;

  ctx->state = AUDIO_STATE_PLAYING;

  ret = pthread_create(&ctx->play_thread, NULL,
                       audio_play_thread, ctx);
  if (ret != 0)
    {
      AUDIO_DEBUG("创建播放线程失败: %d", ret);
      ctx->playing = false;
      audio_close_play_device(ctx);
      ctx->state = AUDIO_STATE_IDLE;
      return -ret;
    }

  ctx->play_thread_valid = true;

  /* 更新状态 */



  return OK;
}

/**
 * @brief  从文件播放音频
 */

int audio_play_file(audio_context_t *ctx, const char *filepath,
                    audio_play_complete_cb_t callback,
                    void *user_data)
{
  if (ctx == NULL || !ctx->initialized || filepath == NULL)
    {
      return -EINVAL;
    }

  if (ctx->recording || ctx->playing)
    {
      return -EBUSY;
    }

  if (ctx->play_thread_valid)
    {
      pthread_join(ctx->play_thread, NULL);
      ctx->play_thread_valid = false;
    }

  FILE *file = fopen(filepath, "rb");
  if (file == NULL)
    {
      return -errno;
    }

  size_t used = fread(ctx->play_buf, 1, ctx->play_buf_size, file);
  int extra = fgetc(file);
  int failed = ferror(file);
  fclose(file);

  if (failed)
    {
      return -EIO;
    }

  if (extra != EOF)
    {
      return -EFBIG;
    }

  if (used == 0 || used % sizeof(int16_t) != 0)
    {
      return -EINVAL;
    }

  /* Reject common containers rather than playing their headers as PCM. */
  if ((used >= 4 && memcmp(ctx->play_buf, "RIFF", 4) == 0) ||
      (used >= 3 && memcmp(ctx->play_buf, "ID3", 3) == 0))
    {
      return -ENOTSUP;
    }

  return audio_play_start(ctx, ctx->play_buf,
                          used / sizeof(int16_t), callback, user_data);
}

/**
 * @brief  停止播放
 */

void audio_play_stop(audio_context_t *ctx)
{
  if (ctx == NULL || !ctx->play_thread_valid)
    {
      return;
    }

  AUDIO_DEBUG("停止播放");

  /* 设置停止标志 */

  __atomic_store_n(&ctx->play_stop, true, __ATOMIC_RELEASE);

  if (pthread_equal(pthread_self(), ctx->play_thread))
    {
      return;
    }

  /* 等待线程退出 */

  pthread_join(ctx->play_thread, NULL);
  ctx->play_thread_valid = false;

  /* 关闭设备 */

  audio_close_play_device(ctx);

  ctx->playing = false;

  /* 更新状态 */

  ctx->state = ctx->recording ? AUDIO_STATE_RECORDING : AUDIO_STATE_IDLE;
}

/**
 * @brief  是否正在播放
 */

bool audio_is_playing(audio_context_t *ctx)
{
  return (ctx != NULL) ? ctx->playing : false;
}

/**
 * @brief  设置音量
 */

int audio_set_volume(audio_context_t *ctx, uint8_t volume)
{
  if (ctx == NULL || !ctx->initialized ||
      volume > AI_AUDIO_VOLUME_MAX)
    {
      return -EINVAL;
    }

  if (ctx->recording || ctx->playing)
    {
      return -EBUSY;
    }

  ctx->config.volume = volume;
  return 0;
}

/**
 * @brief  获取音量
 */

uint8_t audio_get_volume(audio_context_t *ctx)
{
  if (ctx == NULL || !ctx->initialized)
    {
      return 0;
    }

  return ctx->config.volume;
}

/**
 * @brief  启用VAD检测
 */

void audio_vad_enable(audio_context_t *ctx,
                      audio_vad_cb_t callback,
                      void *user_data)
{
  if (ctx == NULL)
    {
      return;
    }

  AUDIO_DEBUG("启用VAD检测");

  ctx->vad_enabled = true;
  ctx->vad_callback = callback;
  ctx->vad_user_data = user_data;
  ctx->vad_speech_active = false;
  ctx->vad_silence_frames = 0;
  ctx->vad_speech_frames = 0;
}

/**
 * @brief  禁用VAD检测
 */

void audio_vad_disable(audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  AUDIO_DEBUG("禁用VAD检测");

  ctx->vad_enabled = false;
  ctx->vad_callback = NULL;
  ctx->vad_user_data = NULL;
}

/**
 * @brief  设置VAD能量阈值
 */

void audio_vad_set_threshold(audio_context_t *ctx, uint32_t threshold)
{
  if (ctx == NULL)
    {
      return;
    }

  AUDIO_DEBUG("设置VAD阈值: %lu", (unsigned long)threshold);

  ctx->vad_energy_threshold = threshold;
}

/**
 * @brief  计算音频帧能量
 */

uint32_t audio_calc_energy(const int16_t *data, size_t frames)
{
  if (data == NULL || frames == 0)
    {
      return 0;
    }

  return audio_calc_frame_energy(data, frames);
}

/**
 * @brief  获取音频模块状态
 */

audio_state_t audio_get_state(audio_context_t *ctx)
{
  if (ctx == NULL)
    {
      return AUDIO_STATE_UNINIT;
    }

  return ctx->state;
}

/**
 * @brief  获取状态名称字符串
 */

const char *audio_get_state_name(audio_state_t state)
{
  if ((int)state >= 0 && state <= AUDIO_STATE_BOTH)
    {
      return g_state_names[state];
    }

  return "UNKNOWN";
}
