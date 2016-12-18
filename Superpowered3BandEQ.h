#ifndef Header_Superpowered3BandEQ
#define Header_Superpowered3BandEQ

#include "SuperpoweredFX.h"
struct eqInternals;


// 经典的三段EQ算法
// Classic three-band equalizer with unique characteristics and total kills.
// It doesn't allocate any internal buffers and needs just a few bytes of memory.

// bands Low/mid/high gain. Read-write. 1.0f is "flat", 2.0f is +6db. Kill is enabled under -40 db (0.01f).
// enabled True if the effect is enabled (processing audio). Read only. Use the enable() method to set.
// 低，中，高的定义？
// 其他参数的设置？
//
class Superpowered3BandEQ: public SuperpoweredFX {
public:
    // 这三个参数的意义？
    float bands[3]; // READ-WRITE parameter.

    // Turns the effect on/off.
    void enable(bool flag);

    // Create an eq instance with the current sample rate value.
    // Enabled is false by default, use enable(true) to enable.
    // Example: Superpowered3BandEQ eq = new Superpowered3BandEQ(44100);
    // 注意采样率的问题: 不要刻意去追求控制采样率到: 44100Hz, 48000Hz似乎也是一个可以接受的选择
    Superpowered3BandEQ(unsigned int samplerate);
    ~Superpowered3BandEQ();
    
    // samplerate 44100, 48000, etc.
    void setSamplerate(unsigned int samplerate);

    // Reset all internals, sets the instance as good as new and turns it off.
    void reset();

    // Processes the audio.
    // It's not locked when you call other methods from other threads, and they not interfere with process() at all.
    // Check the process() documentation of each fx for the minimum number of samples and an optional vector size limitation.
    // For maximum compatibility with all Superpowered effects,
    // numberOfSamples的格式要求:
    //     numberOfSamples should be minimum 32 and a multiply of 8.
    // 返回结果: Put something into output or not.
    // input 32-bit interleaved stereo input buffer. (数据格式)
    // output 32-bit interleaved stereo output buffer.
    // numberOfSamples Number of samples to process.
    bool process(float *input, float *output, unsigned int numberOfSamples);
    
private:
    eqInternals *internals;
    Superpowered3BandEQ(const Superpowered3BandEQ&);
    Superpowered3BandEQ& operator=(const Superpowered3BandEQ&);
};

#endif
