#include "PluginProcessor.h"

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout AnalogMultiFXAudioProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto percent = [] (const char* id, const char* name, float def)
    {
        return std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name,
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), def,
            juce::AudioParameterFloatAttributes().withLabel ("%"));
    };

    layout.add (percent ("drive",   "Drive",       30.0f));
    layout.add (percent ("bump",    "Head Bump",   30.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "tone", 1 }, "Tone",
        juce::NormalisableRange<float> (2000.0f, 20000.0f, 1.0f, 0.4f), 12000.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    layout.add (percent ("wow",     "Wow",         20.0f));
    layout.add (percent ("flutter", "Flutter",     20.0f));
    layout.add (percent ("mix",     "Mix",        100.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    return layout;
}

//==============================================================================
AnalogMultiFXAudioProcessor::AnalogMultiFXAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pDrive   = apvts.getRawParameterValue ("drive");
    pBump    = apvts.getRawParameterValue ("bump");
    pTone    = apvts.getRawParameterValue ("tone");
    pWow     = apvts.getRawParameterValue ("wow");
    pFlutter = apvts.getRawParameterValue ("flutter");
    pMix     = apvts.getRawParameterValue ("mix");
    pOutput  = apvts.getRawParameterValue ("output");
}

//==============================================================================
bool AnalogMultiFXAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return out == layouts.getMainInputChannelSet();
}

//==============================================================================
void AnalogMultiFXAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    maxBlock = juce::jmax (samplesPerBlock, 8192);

    // 2x oversampling around the saturator to keep aliasing low.
    oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
        2, 1,
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
        true, true);
    oversampling->initProcessing ((size_t) maxBlock);
    oversampling->reset();

    const int osLatency = (int) std::lround (oversampling->getLatencyInSamples());
    baseDelayInt = (int) std::lround (0.003 * sampleRate);   // 3 ms base delay for wow/flutter
    totalLatency = osLatency + baseDelayInt;
    setLatencySamples (totalLatency);

    wowMaxSamples     = (float) (0.001   * sampleRate);      // +/- 1 ms at full wow
    flutterMaxSamples = (float) (0.00004 * sampleRate);      // +/- 0.04 ms at full flutter

    for (int ch = 0; ch < 2; ++ch)
    {
        wetDelay[ch].prepare (baseDelayInt + (int) (0.002 * sampleRate) + 8);
        wetDelay[ch].reset();
        dryDelay[ch].prepare (totalLatency + 8);
        dryDelay[ch].reset();
        dcX[ch] = 0.0f;
        dcY[ch] = 0.0f;
    }

    bumpFilter.reset();
    toneFilter.reset();
    dcR = (float) (1.0 - (2.0 * juce::MathConstants<double>::pi * 8.0 / sampleRate));

    wowPhase1 = wowPhase2 = flPhase1 = flPhase2 = 0.0;

    dryBuffer.setSize (2, maxBlock);
    gains.assign ((size_t) maxBlock, 1.0f);

    const float drive01 = pDrive->load() * 0.01f;

    driveGain.reset (sampleRate, 0.05);
    driveGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (drive01 * 30.0f));

    wowSmooth.reset (sampleRate, 0.05);
    wowSmooth.setCurrentAndTargetValue (pWow->load() * 0.01f);

    flutterSmooth.reset (sampleRate, 0.05);
    flutterSmooth.setCurrentAndTargetValue (pFlutter->load() * 0.01f);

    mixSmooth.reset (sampleRate, 0.05);
    mixSmooth.setCurrentAndTargetValue (pMix->load() * 0.01f);

    outSmooth.reset (sampleRate, 0.05);
    outSmooth.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pOutput->load()));
}

//==============================================================================
void AnalogMultiFXAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn  = getTotalNumInputChannels();
    const int totalOut = getTotalNumOutputChannels();

    for (int ch = totalIn; ch < totalOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (oversampling == nullptr)
        return;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int total = buffer.getNumSamples();
    int pos = 0;

    // Process in chunks no larger than the size we prepared for.
    while (pos < total)
    {
        const int len = juce::jmin (maxBlock, total - pos);
        juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), numCh, pos, len);
        processChunk (chunk);
        pos += len;
    }
}

//==============================================================================
void AnalogMultiFXAudioProcessor::processChunk (juce::AudioBuffer<float>& buffer)
{
    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n = buffer.getNumSamples();
    const double fs = currentSampleRate;
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;

    // ---- read parameters -------------------------------------------------
    const float drive01 = pDrive->load() * 0.01f;
    const float bump01  = pBump->load()  * 0.01f;
    const float toneHz  = pTone->load();

    driveGain.setTargetValue (juce::Decibels::decibelsToGain (drive01 * 30.0f));
    wowSmooth.setTargetValue (pWow->load() * 0.01f);
    flutterSmooth.setTargetValue (pFlutter->load() * 0.01f);
    mixSmooth.setTargetValue (pMix->load() * 0.01f);
    outSmooth.setTargetValue (juce::Decibels::decibelsToGain (pOutput->load()));

    bumpFilter.setPeak (fs, 60.0, 0.8, (double) bump01 * 4.0);
    toneFilter.setLowPass (fs, juce::jmin ((double) toneHz, fs * 0.45), 0.7071);

    const float bias = 0.15f * drive01;        // slight asymmetry gives even harmonics
    const float biasOffset = std::tanh (bias);

    float* chan[2] = { nullptr, nullptr };
    float* dry[2]  = { nullptr, nullptr };

    for (int ch = 0; ch < numCh; ++ch)
    {
        chan[ch] = buffer.getWritePointer (ch);
        dry[ch]  = dryBuffer.getWritePointer (ch);
    }

    // ---- 1. keep a delayed copy of the dry signal (matches wet latency) ---
    for (int ch = 0; ch < numCh; ++ch)
    {
        for (int i = 0; i < n; ++i)
        {
            dryDelay[ch].push (chan[ch][i]);
            dry[ch][i] = dryDelay[ch].read ((float) totalLatency);
        }
    }

    // ---- 2. drive (input gain) -------------------------------------------
    for (int i = 0; i < n; ++i)
        gains[(size_t) i] = driveGain.getNextValue();

    for (int ch = 0; ch < numCh; ++ch)
        for (int i = 0; i < n; ++i)
            chan[ch][i] *= gains[(size_t) i];

    // ---- 3. tape saturation, oversampled ----------------------------------
    juce::dsp::AudioBlock<float> block (buffer);
    juce::dsp::AudioBlock<float> sub = block.getSubsetChannelBlock (0, (size_t) numCh);

    auto osBlock = oversampling->processSamplesUp (sub);

    for (size_t ch = 0; ch < osBlock.getNumChannels(); ++ch)
    {
        float* d = osBlock.getChannelPointer (ch);
        for (size_t j = 0; j < osBlock.getNumSamples(); ++j)
            d[j] = std::tanh (d[j] + bias) - biasOffset;
    }

    oversampling->processSamplesDown (sub);

    // ---- 4. head bump, tone, wow/flutter, mix, output --------------------
    for (int i = 0; i < n; ++i)
    {
        const float comp = 1.0f / std::sqrt (gains[(size_t) i]);

        const float wowAmp = wowSmooth.getNextValue() * wowMaxSamples;
        const float flAmp  = flutterSmooth.getNextValue() * flutterMaxSamples;

        const float mod = wowAmp * (0.7f * std::sin ((float) wowPhase1) + 0.3f * std::sin ((float) wowPhase2))
                        + flAmp  * (0.6f * std::sin ((float) flPhase1)  + 0.4f * std::sin ((float) flPhase2));

        wowPhase1 += twoPi * 0.55 / fs;
        wowPhase2 += twoPi * 1.37 / fs;
        flPhase1  += twoPi * 6.3  / fs;
        flPhase2  += twoPi * 11.7 / fs;
        if (wowPhase1 > twoPi) wowPhase1 -= twoPi;
        if (wowPhase2 > twoPi) wowPhase2 -= twoPi;
        if (flPhase1  > twoPi) flPhase1  -= twoPi;
        if (flPhase2  > twoPi) flPhase2  -= twoPi;

        const float delaySamples = (float) baseDelayInt + mod;
        const float mixV = mixSmooth.getNextValue();
        const float outV = outSmooth.getNextValue();

        for (int ch = 0; ch < numCh; ++ch)
        {
            float x = chan[ch][i] * comp;

            // remove any DC the asymmetric saturation created
            const float y = x - dcX[ch] + dcR * dcY[ch];
            dcX[ch] = x;
            dcY[ch] = y;
            x = y;

            x = bumpFilter.process (ch, x);
            x = toneFilter.process (ch, x);

            wetDelay[ch].push (x);
            const float wet = wetDelay[ch].read (delaySamples);

            chan[ch][i] = (dry[ch][i] * (1.0f - mixV) + wet * mixV) * outV;
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* AnalogMultiFXAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void AnalogMultiFXAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void AnalogMultiFXAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AnalogMultiFXAudioProcessor();
}
