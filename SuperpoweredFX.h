#ifndef Header_SuperpoweredFX
#define Header_SuperpoweredFX

// 音效模块的接口定义
class SuperpoweredFX {
public:
    bool enabled;

    // 是否enabled
    virtual void enable(bool flag) = 0; // Use this to turn it on/off.
    // 设置采样率: samplerate 44100, 48000, etc.
    // 采样率的影响:
    // 1. 不要试图去resample, 效率是一个大问题；更加重要的是我们没有搞清楚这个对输入和输出的影响
    // 2. samplerate对很多算法来说影响不大
    // 3. samplerate和文件io的关系呢?
    //    44100 vs. 48000
    virtual void setSamplerate(unsigned int samplerate) = 0;

    // Reset all internals, sets the instance as good as new and turns it off(什么意思? disable it)
    // 很多FX单元内部有自己的缓存等，例如: Autotune, Delay等， 在stop, restart, 拖放之后，数据会出现不一致；因此需要reset
    // 否则可能出现噪声
    virtual void reset() = 0;


    // Processes the audio.
    // It's not locked when you call other methods from other threads, and they not interfere with process() at all.
    // Check the process() documentation of each fx for the minimum number of samples and an optional vector size limitation.
    // For maximum compatibility with all Superpowered effects, numberOfSamples should be minimum 32 and a multiply of 8.
    // 返回结果: Put something into output or not.
    // input 32-bit interleaved stereo input buffer. (数据格式)
    // output 32-bit interleaved stereo output buffer.
    // numberOfSamples Number of samples to process.
    virtual bool process(float *input, float *output, unsigned int numberOfSamples) = 0;
    
    virtual ~SuperpoweredFX() {};
};
#endif
