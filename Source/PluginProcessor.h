#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

//==============================================================================
// Small helper: second-order filter (RBJ cookbook), state kept per channel.
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1[2] = { 0.0f, 0.0f };
    float z2[2] = { 0.0f, 0.0f };

    void reset()
    {
        z1[0] = z1[1] = 0.0f;
        z2[0] = z2[1] = 0.0f;
    }

    float process (int ch, float x)
    {
        const float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }

    void setLowPass (double fs, double freq, double q)
    {
        const double w0 = 2.0 * juce::MathConstants<double>::pi * freq / fs;
        const double cw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = (float) (((1.0 - cw) * 0.5) / a0);
        b1 = (float) ((1.0 - cw) / a0);
        b2 = b0;
        a1 = (float) ((-2.0 * cw) / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    void setPeak (double fs, double freq, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * freq / fs;
        const double cw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha / A;
        b0 = (float) ((1.0 + alpha * A) / a0);
        b1 = (float) ((-2.0 * cw) / a0);
        b2 = (float) ((1.0 - alpha * A) / a0);
        a1 = b1;
        a2 = (float) ((1.0 - alpha / A) / a0);
    }
};

//==============================================================================
// Small helper: delay line with linear interpolation.
struct DelayLine
{
    std::vector<float> buf;
    int mask = 0;
    int w = 0;

    void prepare (int maxSamples)
    {
        int size = 1;
        while (size < maxSamples + 4)
            size <<= 1;
        buf.assign ((size_t) size, 0.0f);
        mask = size - 1;
        w = 0;
    }

    void reset()
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        w = 0;
    }

    void push (float x)
    {
        buf[(size_t) w] = x;
        w = (w + 1) & mask;
    }

    // Reads the signal 'delaySamples' behind the most recently pushed sample.
    float read (float delaySamples) const
    {
        const float pos = (float) (w - 1) - delaySamples;
        const int i0 = (int) std::floor (pos);
        const float frac = pos - (float) i0;
        const float a = buf[(size_t) (i0 & mask)];
        const float b = buf[(size_t) ((i0 + 1) & mask)];
        return a + frac * (b - a);
    }
};

//==============================================================================
class AnalogMultiFXAudioProcessor : public juce::AudioProcessor
{
public:
    AnalogMultiFXAudioProcessor();
    ~AnalogMultiFXAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void processChunk (juce::AudioBuffer<float>& buffer);

    // Parameter handles
    std::atomic<float>* pDrive = nullptr;
    std::atomic<float>* pBump = nullptr;
    std::atomic<float>* pTone = nullptr;
    std::atomic<float>* pWow = nullptr;
    std::atomic<float>* pFlutter = nullptr;
    std::atomic<float>* pMix = nullptr;
    std::atomic<float>* pOutput = nullptr;

    // DSP state
    double currentSampleRate = 44100.0;
    int maxBlock = 8192;
    int baseDelayInt = 0;
    int totalLatency = 0;
    float wowMaxSamples = 0.0f;
    float flutterMaxSamples = 0.0f;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;

    DelayLine wetDelay[2];
    DelayLine dryDelay[2];
    Biquad bumpFilter;
    Biquad toneFilter;

    float dcX[2] = { 0.0f, 0.0f };
    float dcY[2] = { 0.0f, 0.0f };
    float dcR = 0.999f;

    double wowPhase1 = 0.0, wowPhase2 = 0.0, flPhase1 = 0.0, flPhase2 = 0.0;

    juce::AudioBuffer<float> dryBuffer;
    std::vector<float> gains;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> driveGain;
    juce::SmoothedValue<float> wowSmooth, flutterSmooth, mixSmooth, outSmooth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalogMultiFXAudioProcessor)
};
