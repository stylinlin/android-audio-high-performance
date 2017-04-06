/*
 * Copyright 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <thread>
#include <cassert>
#include <jni.h>

#include "audio_common.h"
#include "SineGenerator.h"
#include "stream_builder.h"

/*
 * This Sample's Engine Structure
 */
struct AAudioEngine {
    uint32_t     sampleRate_;
    uint16_t     sampleChannels_;
    uint16_t     bitsPerSample_;
    aaudio_audio_format_t sampleFormat_;

    AAudioStream *playStream_;
    bool   requestStop_;
    bool   playAudio_;

};
static AAudioEngine engine;

/*
 * Functions exposed to Java code...
 */
extern "C" {
  JNIEXPORT jboolean JNICALL
  Java_com_google_sample_aaudio_play_MainActivity_createEngine(
            JNIEnv *env, jclass);
  JNIEXPORT void JNICALL
  Java_com_google_sample_aaudio_play_MainActivity_deleteEngine(
            JNIEnv *env, jclass type);
  JNIEXPORT jboolean JNICALL
  Java_com_google_sample_aaudio_play_MainActivity_start(
            JNIEnv *env, jclass type);
  JNIEXPORT jboolean JNICALL
  Java_com_google_sample_aaudio_play_MainActivity_stop(
            JNIEnv *env, jclass type);
}

bool TunePlayerForLowLatency(AAudioStream* stream);

/*
 * PlayAudioThreadProc()
 *   Rendering audio frames continuously; if user asks to play audio, render
 *   sine wave; if user asks to stop, renders silent audio (all 0s)
 *
 */
void PlayAudioThreadProc(void* ctx) {
  AAudioEngine* eng = reinterpret_cast<AAudioEngine*>(ctx);

  bool status = TunePlayerForLowLatency(engine.playStream_);
  if (!status) {
    // if tune up is failed, audio could still play
    LOGW("Failed to tune up the audio buffer size,"
             "low latency audio may not be guaranteed");
  }
  // double check the tuning result: not necessary
  PrintAudioStreamInfo(engine.playStream_);

  LOGV("=====: currentState=%d", AAudioStream_getState(eng->playStream_));

  // prepare for data generator
  SineGenerator sineOscLeft, sineOscRight;
  sineOscLeft.setup(440.0, eng->sampleRate_, 0.25);
  sineOscRight.setup(660.0, eng->sampleRate_, 0.25);
  int32_t framesPerBurst = AAudioStream_getFramesPerBurst(eng->playStream_);
  int32_t samplesPerFrame = AAudioStream_getSamplesPerFrame(eng->playStream_);

  // Writing it out as frames per burst, total 5 seconds
  int16_t *buf = new int16_t[framesPerBurst * samplesPerFrame];
  assert(buf);

  aaudio_result_t result;
  while (!eng->requestStop_) {
    if (eng->playAudio_) {
      sineOscRight.render(&buf[0], samplesPerFrame, framesPerBurst);
      if (samplesPerFrame == 2) {
        sineOscLeft.render(&buf[0] + 1, samplesPerFrame, framesPerBurst);
      }
    } else {
      memset(buf, 0, sizeof(int16_t) * framesPerBurst * samplesPerFrame);
    }
    result = AAudioStream_write(eng->playStream_,
                                buf,
                                framesPerBurst,
                                100000000);
    assert(result > 0);
  }

  delete [] buf;
  eng->requestStop_ = false;

  AAudioStream_requestStop(eng->playStream_);

  AAudioStream_close(eng->playStream_);
  eng->playStream_ = nullptr;

  LOGV("====Player is done");
}

/*
 * Create sample engine and put application into started state:
 * audio is already rendering -- rendering silent audio.
 */
JNIEXPORT jboolean JNICALL
Java_com_google_sample_aaudio_play_MainActivity_createEngine(
        JNIEnv *env, jclass type) {

    memset(&engine, 0, sizeof(engine));

    // Initialize AAudio wrapper
    if(!InitAAudio()) {
      LOGE("AAudio is not supported on your platform, cannot proceed");
      return JNI_FALSE;
    }

    engine.sampleChannels_   = AUDIO_SAMPLE_CHANNELS;
    engine.sampleFormat_ = AAUDIO_FORMAT_PCM_I16;
    engine.bitsPerSample_ = SampleFormatToBpp(engine.sampleFormat_);

    // Create an Output Stream
    StreamBuilder builder;
    engine.playStream_ = builder.CreateStream(engine.sampleFormat_,
                                            engine.sampleChannels_,
                                            AAUDIO_SHARING_MODE_SHARED);
    assert(engine.playStream_);
    PrintAudioStreamInfo(engine.playStream_);

    engine.sampleRate_ = AAudioStream_getSampleRate(engine.playStream_);
    aaudio_result_t result = AAudioStream_requestStart(engine.playStream_);
    if (result != AAUDIO_OK) {
      assert(result == AAUDIO_OK);
      return JNI_FALSE;
    }

    std::thread t(PlayAudioThreadProc, &engine);
    t.detach();
   return JNI_TRUE;
}

/*
 * start():
 *   start to render sine wave audio.
 */
JNIEXPORT jboolean JNICALL
Java_com_google_sample_aaudio_play_MainActivity_start(
    JNIEnv *env, jclass type) {
  if (!engine.playStream_)
    return JNI_FALSE;

  engine.playAudio_ = true;
  return JNI_TRUE;
}

/*
 * stop():
 *   stop rendering sine wave audio ( resume rendering silent audio )
 */
JNIEXPORT jboolean JNICALL
Java_com_google_sample_aaudio_play_MainActivity_stop(
    JNIEnv *env, jclass type) {
  if (!engine.playStream_)
    return JNI_TRUE;

  engine.playAudio_ = false;
  return JNI_TRUE;
}

/*
 * delete()
 *   clean-up sample: application is going away. Simply setup stop request
 *   flag and rendering thread will see it and perform clean-up
 */
JNIEXPORT void JNICALL
Java_com_google_sample_aaudio_play_MainActivity_deleteEngine(
    JNIEnv *env, jclass type) {
  if (!engine.playStream_) {
    return;
  }
  engine.requestStop_ = true;
}

/*
 * TunePlayerForLowLatency()
 *   start from the framesPerBurst, find out the smallest size that has no
 *   underRan for buffer between Application and AAudio
 *  If tune-up failed, we still let it continue by restoring the value
 *  upon entering the function; the failure of the tuning is notified to
 *  caller with false return value.
 * Return:
 *   true:  tune-up is completed, AAudio is at its best
 *   false: tune-up is not complete, AAudio is at its default condition
 */
bool TunePlayerForLowLatency(AAudioStream* stream) {
  aaudio_stream_state_t state = AAudioStream_getState(stream);
  if (state != AAUDIO_STREAM_STATE_STARTED) {
    LOGE("stream(%p) is not in started state when tuning", stream);
    return false;
  }

  int32_t framesPerBurst = AAudioStream_getFramesPerBurst(stream);
  int32_t orgSize = AAudioStream_getBufferSizeInFrames(stream);

  int32_t bufSize = framesPerBurst;
  int32_t bufCap  = AAudioStream_getBufferCapacityInFrames(stream);

  uint8_t *buf = new uint8_t [bufCap * engine.bitsPerSample_ / 8];
  assert(buf);
  memset(buf, 0, bufCap * engine.bitsPerSample_ / 8);

  int32_t prevXRun = AAudioStream_getXRunCount(stream);
  int32_t prevBufSize = 0;
  bool trainingError = false;
  while (bufSize <= bufCap) {
    aaudio_result_t  result = AAudioStream_setBufferSizeInFrames(stream, bufSize);
    if(result <= AAUDIO_OK) {
      trainingError = true;
      break;
    }

    // check whether we are really setting to our value
    // AAudio might already reached its optimized state
    // so we set-get-compare, then act accordingly
    bufSize = AAudioStream_getBufferSizeInFrames(stream);
    if (bufSize == prevBufSize) {
      // AAudio refuses to go up, tuning is complete
      break;
    }
    // remember the current buf size so we could continue for next round tuning up
    prevBufSize = bufSize;
    result = AAudioStream_write(stream, buf, bufCap, 1000000000);

    if (result < 0 ) {
      assert(result >= 0);
      trainingError = true;
      break;
    }
    int32_t curXRun = AAudioStream_getXRunCount(stream);
    if (curXRun <= prevXRun) {
      // no more errors, we are done
      break;
    }
    prevXRun = curXRun;
    bufSize += framesPerBurst;
  }

  delete [] buf;
  if (trainingError) {
    // we are playing conservative here: if anything wrong, we restore to default
    // size WHEN engine was created
    AAudioStream_setBufferSizeInFrames(stream, orgSize);
    return false;
  }
  return true;
}
