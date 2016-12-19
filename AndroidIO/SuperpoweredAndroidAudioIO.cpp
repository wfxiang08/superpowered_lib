#include "SuperpoweredAndroidAudioIO.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#include <pthread.h>
#include <unistd.h>

//
// Superpowered如何和底层的Audio交互呢?
//
//
// 记录AudioEngine的内部状态
typedef struct SuperpoweredAndroidAudioIOInternals {
    void *clientdata;
    audioProcessingCallback callback;

    // 参考: https://developer.android.com/ndk/guides/audio/opensl-prog-notes.html
    //      Interface对象，和iOS AudioKit类似
    SLObjectItf openSLEngine, outputMix;

    // 这两个Interface分别和Player和Recorder对应
    // 和OpenGL比较类似, output表示信号源，input表示获取信号的，类似sink
    SLObjectItf outputBufferQueue;
    SLObjectItf inputBufferQueue;

    SLAndroidSimpleBufferQueueItf outputBufferQueueInterface, inputBufferQueueInterface;

    short int *fifobuffer, *silence;

    int samplerate, buffersize, silenceSamples, latencySamples, numBuffers, bufferStep;

    int readBufferIndex, writeBufferIndex;

    bool hasOutput, hasInput, foreground;

    // 运行状态
    bool started;

} SuperpoweredAndroidAudioIOInternals;



// The entire operation is based on two Android Simple Buffer Queues, one for the audio input and one for the audio output.
static void startQueues(SuperpoweredAndroidAudioIOInternals *internals) {
    // 防止多次调用
    if (internals->started) {
        return;
    } else {
        internals->started = true;
    }

    if (internals->inputBufferQueue) {
        SLRecordItf recordInterface;
        (*internals->inputBufferQueue)->GetInterface(internals->inputBufferQueue,
                                                     SL_IID_RECORD, &recordInterface);
        (*recordInterface)->SetRecordState(recordInterface, SL_RECORDSTATE_RECORDING);
    }

    if (internals->outputBufferQueue) {
        SLPlayItf outputPlayInterface;
        (*internals->outputBufferQueue)->GetInterface(internals->outputBufferQueue,
                                                      SL_IID_PLAY, &outputPlayInterface);
        (*outputPlayInterface)->SetPlayState(outputPlayInterface, SL_PLAYSTATE_PLAYING);
    };
}

// Stopping the Simple Buffer Queues.
static void stopQueues(SuperpoweredAndroidAudioIOInternals *internals) {
    if (!internals->started) {
        return;
    } else {
        internals->started = false;
    }

    if (internals->outputBufferQueue) {
        SLPlayItf outputPlayInterface;
        (*internals->outputBufferQueue)->GetInterface(internals->outputBufferQueue,
                                                      SL_IID_PLAY,
                                                      &outputPlayInterface);
        (*outputPlayInterface)->SetPlayState(outputPlayInterface,
                                             SL_PLAYSTATE_STOPPED);
    }

    if (internals->inputBufferQueue) {
        SLRecordItf recordInterface;
        (*internals->inputBufferQueue)->GetInterface(internals->inputBufferQueue,
                                                     SL_IID_RECORD,
                                                     &recordInterface);
        (*recordInterface)->SetRecordState(recordInterface,
                                           SL_RECORDSTATE_STOPPED);
    };
}

//
// This is called periodically by the input audio queue.
// Audio input is received from the media server at this point.
// 注意这里有一个概念: MediaServer(万恶的设计, Android的各种Delay从之类开始）
//
static void SuperpoweredAndroidAudioIO_InputCallback(SLAndroidSimpleBufferQueueItf caller,
                                                     void *pContext) {

    // Callback在什么时候调用呢?
    // 处理完毕它内部的buffer之后，调用
    // 因此在初始化时，会事先给他一个buffer; 然后再InputCallback中再次给一个buffer

    SuperpoweredAndroidAudioIOInternals *internals = (SuperpoweredAndroidAudioIOInternals *)pContext;

    // 获取一个buffer, 并且 writeBufferIndex切换到下一个位置，表示当前的buffer已经被占用
    short int *buffer = internals->fifobuffer + internals->writeBufferIndex * internals->bufferStep;
    if (internals->writeBufferIndex < internals->numBuffers - 1) {
        internals->writeBufferIndex++;
    } else {
        internals->writeBufferIndex = 0;
    }

    // 如果没有信号源，那么: buffer是否需要silence呢?
    // 如果不输出到耳机，那么整个信号由Mic端来驱动；
    //       如果: latencySamples == 0, 则readBufferIndex和writeBufferIndex应该总是同步的
    // 如果输出到耳机，则由Phone端来驱动
    //
    if (!internals->hasOutput) {
        // When there is no audio output configured.

        // writeBufferIndex 下一个待输入数据的位置
        // readBufferIndex  下一个待读取数据的位置
        // Invariant: readBufferIndex <= writeBufferIndex
        // 例如：
        // readBufferIndex  writeBufferIndex
        //      0                  0           初始状态
        //      0                  1           表示从mic读取一个buffer， 然后 readBufferIndex 就跟随 writeBufferIndex 变化
        // 如果没有output, 那么就由输入来驱动，Audio模型为push
        //   buffersAvailable 作用就是简单地配合 latencySamples 来完成数据的delay处理
        // 如果有output, 那Audio模型为pull; 这里的callback就只负责: writeBufferIndex 的管理
        //
        int buffersAvailable = internals->writeBufferIndex - internals->readBufferIndex;
        if (buffersAvailable < 0) {
            buffersAvailable = internals->numBuffers + buffersAvailable;
        }

        // if we have enough audio input available
        // latencySamples 可以假设为0, 则
        if (buffersAvailable * internals->buffersize >= internals->latencySamples) {
            short int *readBuffer = internals->fifobuffer + internals->readBufferIndex * internals->bufferStep;

            // readBuffer的数据
            internals->callback(internals->clientdata,
                                readBuffer,
                                internals->buffersize,
                                internals->samplerate);

            if (internals->readBufferIndex < internals->numBuffers - 1) {
                internals->readBufferIndex++;
            } else {
                internals->readBufferIndex = 0;
            }
        };
    }

    // 不同的caller, 代表不同数据的意义
    // 采集到的信号交给:  SLAndroidSimpleBufferQueueItf
    // buffer待输入数据，writeBufferIndex 表示下一个待输入数据的buffer index
    //
    (*caller)->Enqueue(caller, buffer, (SLuint32)internals->buffersize * 4);
}

// This is called periodically by the output audio queue.
// Audio for the user should be provided here.
// 直接输出到耳机
// 希望准备
static void SuperpoweredAndroidAudioIO_OutputCallback(SLAndroidSimpleBufferQueueItf caller, void *pContext) {

    SuperpoweredAndroidAudioIOInternals *internals = (SuperpoweredAndroidAudioIOInternals *)pContext;

    //                     readBufferIndex    writerBufferIndex
    // writerBufferIndex   readBufferIndex                           --> (writerBufferIndex + numBuffers)
    //
    // 有多少帧可用, 原则: internals->writeBufferIndex >= internals->readBufferIndex
    //
    int buffersAvailable = internals->writeBufferIndex - internals->readBufferIndex;
    if (buffersAvailable < 0) {
        buffersAvailable = internals->numBuffers + buffersAvailable;
    }

    // 驱动力
    // Queque, Thread
    // 如果有输出，则Audio模型为pull, 输入端的Queue负责读取数据到queue中，然后改写: writeBufferIndex

    short int *output = internals->fifobuffer + internals->readBufferIndex * internals->bufferStep;

    if (internals->hasInput) {
        // If audio input is enabled.
        // if we have enough audio input available
        // 立马需要处理的数据在: readBufferIndex 处
        if (buffersAvailable * internals->buffersize >= internals->latencySamples) {

            // 如果没有改写output，则需要主动silence这个信号
            if (!internals->callback(internals->clientdata, output,
                                     internals->buffersize, internals->samplerate)) {
                // 将输出设置为silence
                memset(output, 0, (size_t)internals->buffersize * 4);
                internals->silenceSamples += internals->buffersize;
            } else {
                // 正常情况下，不应该出现silence, 除非文件读取完毕等
                internals->silenceSamples = 0;
            }
        } else {
            // dropout, not enough audio input
            output = NULL;
        }
    } else {
        // 压根就没有另外一个callback, 所有的数据流都由当前分支来驱动
        // If audio input is not enabled.
        // 如果没有Mic输入，则整个事件靠输出来驱动
        // writeBufferIndex和readBufferIndex之前的关系还是需要清晰地维护
        //
        short int *audioToGenerate = internals->fifobuffer + internals->writeBufferIndex * internals->bufferStep;

        // 先生成数据再说
        if (!internals->callback(internals->clientdata, audioToGenerate, internals->buffersize, internals->samplerate)) {
            memset(audioToGenerate, 0, (size_t)internals->buffersize * 4);
            internals->silenceSamples += internals->buffersize;
        } else {
            internals->silenceSamples = 0;
        }

        if (internals->writeBufferIndex < internals->numBuffers - 1) {
            internals->writeBufferIndex++;
        } else {
            internals->writeBufferIndex = 0;
        }
        // dropout, not enough audio generated
        if ((buffersAvailable + 1) * internals->buffersize < internals->latencySamples) {
            output = NULL;
        }
    };

    //
    if (output) {
        if (internals->readBufferIndex < internals->numBuffers - 1) {
            internals->readBufferIndex++;
        } else {
            internals->readBufferIndex = 0;
        }
    }

    // 修改: readBufferIndex, 表示output buffer消费完毕，跳转到下一个buffer
    (*caller)->Enqueue(caller, output ? output : internals->silence, (SLuint32)internals->buffersize * 4);

    // 如果不在前台，并且持续了一段时间，那么直接暂停
    if (!internals->foreground && (internals->silenceSamples > internals->samplerate)) {
        internals->silenceSamples = 0;
        stopQueues(internals);
    }
}

SuperpoweredAndroidAudioIO::SuperpoweredAndroidAudioIO(int samplerate,
                                                       int buffersize,
                                                       bool enableInput,
                                                       bool enableOutput,
                                                       audioProcessingCallback callback,
                                                       void *clientdata,
                                                       int inputStreamType,
                                                       int outputStreamType,
                                                       int latencySamples) {

    static const SLboolean requireds[2] = {
            SL_BOOLEAN_TRUE,
            SL_BOOLEAN_FALSE
    };

    // 初始化
    // 设置各种参数
    internals = new SuperpoweredAndroidAudioIOInternals;
    memset(internals, 0, sizeof(SuperpoweredAndroidAudioIOInternals));
    internals->samplerate = samplerate;
    internals->buffersize = buffersize;

    // clientdata & callback
    internals->callback = callback;
    internals->clientdata = clientdata;

    internals->hasInput = enableInput;
    internals->hasOutput = enableOutput;
    internals->foreground = true;
    internals->started = false;

    // silence的作用
    // buffersize：一个loop最多处理的采样点数
    // buffersize * 2(sizeof short) * 2(stereo)
    //
    internals->silence = (short int *)malloc((size_t)buffersize * 4);
    memset(internals->silence, 0, (size_t)buffersize * 4);

    internals->latencySamples = latencySamples < buffersize ? buffersize : latencySamples;

    internals->numBuffers = (internals->latencySamples / buffersize) * 2;

    // 最少使用16个Buffer
    if (internals->numBuffers < 16) {
        internals->numBuffers = 16;
    }
    internals->bufferStep = (buffersize + 64) * 2;

    // 分配: fifobuffer
    size_t fifoBufferSizeBytes = internals->numBuffers * internals->bufferStep * sizeof(short int);
    internals->fifobuffer = (short int *)malloc(fifoBufferSizeBytes);
    memset(internals->fifobuffer, 0, fifoBufferSizeBytes);

    // 创建openSLEngine & openSLEngineInterface
    slCreateEngine(&internals->openSLEngine, 0, NULL, 0, NULL, NULL);
    (*internals->openSLEngine)->Realize(internals->openSLEngine, SL_BOOLEAN_FALSE);
    SLEngineItf openSLEngineInterface = NULL;
    (*internals->openSLEngine)->GetInterface(internals->openSLEngine, SL_IID_ENGINE, &openSLEngineInterface);

    // 在通过: openSLEngineInterface 来创建更多的多项
    // Create the output mix.
    (*openSLEngineInterface)->CreateOutputMix(openSLEngineInterface, &internals->outputMix, 0, NULL, NULL);
    (*internals->outputMix)->Realize(internals->outputMix, SL_BOOLEAN_FALSE);
    SLDataLocator_OutputMix outputMixLocator = { SL_DATALOCATOR_OUTPUTMIX, internals->outputMix };

    // The value of the samplesPerSec field is in units of milliHz, despite the misleading name.
    SLuint32 samplerateInMillHz = samplerate * 1000;

    // 允许输入数据
    if (enableInput) { // Create the audio input buffer queue.
        SLDataLocator_IODevice deviceInputLocator = {
                SL_DATALOCATOR_IODEVICE,
                SL_IODEVICE_AUDIOINPUT,        // DeviceType
                SL_DEFAULTDEVICEID_AUDIOINPUT, // DeviceID
                NULL
        };
        SLDataSource inputSource = { &deviceInputLocator, NULL };
        SLDataLocator_AndroidSimpleBufferQueue inputLocator = {
                SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1
        };


        SLDataFormat_PCM inputFormat = { SL_DATAFORMAT_PCM, 2,
                                         samplerateInMillHz,
                                         SL_PCMSAMPLEFORMAT_FIXED_16,
                                         SL_PCMSAMPLEFORMAT_FIXED_16,
                                         SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN };
        SLDataSink inputSink = {
                &inputLocator,
                &inputFormat
        };
        const SLInterfaceID inputInterfaces[2] = {
                SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                SL_IID_ANDROIDCONFIGURATION
        };

        // 创建一个Recorder,
        (*openSLEngineInterface)->CreateAudioRecorder(openSLEngineInterface,
                                                      &internals->inputBufferQueue,
                                                      &inputSource, &inputSink, 2,
                                                      inputInterfaces,
                                                      requireds // 两个Interface, 第一个必须，第二个非必须
        );

        // Configure the voice recognition preset which has no signal processing for lower latency.
        if (inputStreamType == -1) {
            // 为什么默认为 语音识别呢？
            inputStreamType = (int)SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
        }
        if (inputStreamType > -1) {
            SLAndroidConfigurationItf inputConfiguration;
            // 设置: inputStreamtype
            if ((*internals->inputBufferQueue)->GetInterface(internals->inputBufferQueue,
                                                             SL_IID_ANDROIDCONFIGURATION,
                                                             &inputConfiguration) == SL_RESULT_SUCCESS) {
                SLuint32 st = (SLuint32)inputStreamType;
                (*inputConfiguration)->SetConfiguration(inputConfiguration,
                                                        SL_ANDROID_KEY_RECORDING_PRESET,
                                                        &st, sizeof(SLuint32));
            };
        };

        // 初始化
        (*internals->inputBufferQueue)->Realize(internals->inputBufferQueue, SL_BOOLEAN_FALSE);
    };

    if (enableOutput) {
        // Create the audio output buffer queue.
        SLDataLocator_AndroidSimpleBufferQueue outputLocator = {
                SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1
        };

        SLDataFormat_PCM outputFormat = {
                SL_DATAFORMAT_PCM, // formatType
                2, // numChannels
                samplerateInMillHz, // samplesPerSec
                SL_PCMSAMPLEFORMAT_FIXED_16, // bitsPerSample
                SL_PCMSAMPLEFORMAT_FIXED_16, // containerSize
                SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, // channelMask, 双声道
                SL_BYTEORDER_LITTLEENDIAN
        };
        SLDataSource outputSource = { &outputLocator, &outputFormat };
        const SLInterfaceID outputInterfaces[2] = {
                SL_IID_BUFFERQUEUE,
                SL_IID_ANDROIDCONFIGURATION
        };
        SLDataSink outputSink = {
                &outputMixLocator, NULL
        };

        // 创建AudioPlayer
        (*openSLEngineInterface)->CreateAudioPlayer(openSLEngineInterface,
                                                    &internals->outputBufferQueue,
                                                    &outputSource, &outputSink, 2,
                                                    outputInterfaces, requireds);

        // Configure the stream type.
        if (outputStreamType > -1) {
            SLAndroidConfigurationItf outputConfiguration;
            if ((*internals->outputBufferQueue)->GetInterface(internals->outputBufferQueue,
                                                              SL_IID_ANDROIDCONFIGURATION,
                                                              &outputConfiguration) == SL_RESULT_SUCCESS) {
                SLint32 st = (SLint32)outputStreamType;
                (*outputConfiguration)->SetConfiguration(outputConfiguration,
                                                         SL_ANDROID_KEY_STREAM_TYPE,
                                                         &st, sizeof(SLint32));
            };
        };

        (*internals->outputBufferQueue)->Realize(internals->outputBufferQueue,
                                                 SL_BOOLEAN_FALSE);
    };

    if (enableInput) {
        // Initialize the audio input buffer queue.
        // SL_IID_ANDROIDSIMPLEBUFFERQUEUE 是必须的Interface ===> inputBufferQueueInterface
        (*internals->inputBufferQueue)->GetInterface(internals->inputBufferQueue,
                                                     SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                     &internals->inputBufferQueueInterface);

        // 注册Callback, 然后就以SampleRate的速率来读取信号
        (*internals->inputBufferQueueInterface)->RegisterCallback(internals->inputBufferQueueInterface,
                                                                  SuperpoweredAndroidAudioIO_InputCallback,
                                                                  internals);

        // 注意: 这两个Queue对fifobuffer的使用, 如何管理呢？
        (*internals->inputBufferQueueInterface)->Enqueue(internals->inputBufferQueueInterface,
                                                         internals->fifobuffer, (SLuint32)buffersize * 4);
    };

    //
    // internals->fifobuffer 如何使用呢?
    //
    if (enableOutput) { // Initialize the audio output buffer queue.
        (*internals->outputBufferQueue)->GetInterface(internals->outputBufferQueue,
                                                      SL_IID_BUFFERQUEUE, &internals->outputBufferQueueInterface);
        (*internals->outputBufferQueueInterface)->RegisterCallback(internals->outputBufferQueueInterface,
                                                                   SuperpoweredAndroidAudioIO_OutputCallback, internals);

        (*internals->outputBufferQueueInterface)->Enqueue(internals->outputBufferQueueInterface,
                                                          internals->fifobuffer, (SLuint32)buffersize * 4);
    };

    startQueues(internals);
}

void SuperpoweredAndroidAudioIO::onForeground() {
    internals->foreground = true;
    startQueues(internals);
}

void SuperpoweredAndroidAudioIO::onBackground() {
    internals->foreground = false;
}

void SuperpoweredAndroidAudioIO::start() {
    startQueues(internals);
}

void SuperpoweredAndroidAudioIO::stop() {
    stopQueues(internals);
}

SuperpoweredAndroidAudioIO::~SuperpoweredAndroidAudioIO() {

    stopQueues(internals);
    usleep(200000); // sleep 200ms

    // SLObjectItf 如何释放自己呢?
    if (internals->outputBufferQueue) {
        (*internals->outputBufferQueue)->Destroy(internals->outputBufferQueue);
    }
    if (internals->inputBufferQueue) {
        (*internals->inputBufferQueue)->Destroy(internals->inputBufferQueue);
    }

    (*internals->outputMix)->Destroy(internals->outputMix);
    (*internals->openSLEngine)->Destroy(internals->openSLEngine);

    free(internals->fifobuffer);
    free(internals->silence);

    delete internals;
}
