/*
    ChannelFX.cpp - Implementation of per-mic channel strip DSP and mixer presets.
*/

#include "ChannelFX.h"
#include "XmlLite.h"

namespace glorp
{

namespace
{
constexpr float kPi = 3.14159265358979323846f;

inline float dbToGain(float db) noexcept
{
    return std::pow(10.0f, db * 0.05f);
}

inline float softClip(float x) noexcept
{
    // Transparent soft knee starting at |x| > 0.85, asymptotes to 1.0
    const float ax = std::abs(x);
    if (ax <= 0.85f)
        return x;

    const float sign = x < 0.0f ? -1.0f : 1.0f;
    const float excess = ax - 0.85f;
    return sign * (0.85f + 0.15f * std::tanh(excess / 0.15f));
}

struct MicRoleFlags
{
    bool trigger = false;
    bool kick = false;
    bool snare = false;
    bool hat = false;
    bool cymbal = false;
    bool overhead = false;
    bool room = false;
    bool tom = false;
    bool left = false;
    bool right = false;
};

inline MicRoleFlags classifyMic(const juce::String &name)
{
    const auto n = name.toLowerCase();
    MicRoleFlags r;

    r.trigger = n.contains("trigger");
    r.kick = r.trigger || n.contains("kick") || n.contains("kdrum") || n.contains("bdrum");
    r.snare = n.contains("snare") || n.contains("snr");
    r.hat = n.contains("hihat") || n.contains("hh") || n.contains("hat");
    r.cymbal = (n.contains("ride") || n.contains("crash") || n.contains("china") || n.contains("splash")
                || n.contains("cymbal") || n.contains("zilbel") || n.contains("bell"))
               && !r.hat && !n.contains("cowbell");
    r.overhead = n.contains("overhead") || n.contains("oh");
    r.room = n.contains("room") || n.contains("pzm") || n.contains("ambience") || n.contains("ambient")
             || n.startsWith("ambl") || n.startsWith("ambr") || n.startsWith("amb ") || n.startsWith("amb_")
             || n == "amb";
    r.tom = (n.contains("tom") || n.contains("floor")) && !r.snare && !r.kick && !r.hat && !r.cymbal
            && !r.overhead && !r.room && !n.contains("stomp") && !n.contains("custom") && !n.contains("bottom");

    const bool centered = n.contains("center") || n.contains("centre") || n.contains("mono");
    r.left = !centered
             && (n.endsWith("l") || n.contains("left") || n.contains("_l") || n.contains("ohl") || n.contains("ambl"));
    r.right = !centered
              && (n.endsWith("r") || n.contains("right") || n.contains("_r") || n.contains("ohr") || n.contains("ambr"));

    return r;
}

inline void applyFactoryMicPolarity(MicStripParams &s, const juce::String &name)
{
    const auto n = name.toLowerCase();
    if (n.contains("kdrum_back") || n.contains("kick back") || n.contains("kickback"))
        s.phaseInvert = true;
}

inline void applyFactoryMicFilters(MicStripParams &s, const MicRoleFlags &r)
{
    if (r.trigger) { s.hpfHz = 20.0f; s.lpfHz = 20000.0f; }
    else if (r.kick) { s.hpfHz = 25.0f; s.lpfHz = 20000.0f; }
    else if (r.snare) { s.hpfHz = 50.0f; s.lpfHz = 20000.0f; }
    else if (r.tom) { s.hpfHz = 40.0f; s.lpfHz = 20000.0f; }
    else if (r.hat) { s.hpfHz = 300.0f; s.lpfHz = 18000.0f; }
    else if (r.cymbal) { s.hpfHz = 400.0f; s.lpfHz = 18000.0f; }
    else if (r.overhead) { s.hpfHz = 200.0f; s.lpfHz = 16000.0f; }
    else if (r.room) { s.hpfHz = 150.0f; s.lpfHz = 12000.0f; }
}

inline void setFactoryBus(MasterStripParams &m, float lowDb, float midDb, float midHz, float highDb,
                          float thresholdDb, float ratio, float attackMs, float releaseMs,
                          float makeupDb, float parallelMix, float satDrive)
{
    m.busEqLowDb = lowDb;
    m.busEqMidDb = midDb;
    m.busEqMidFreq = midHz;
    m.busEqHighDb = highDb;
    m.busCompEnabled = true;
    m.busCompThresholdDb = thresholdDb;
    m.busCompRatio = ratio;
    m.busCompAttackMs = attackMs;
    m.busCompReleaseMs = releaseMs;
    m.busCompMakeupDb = makeupDb;
    m.busCompMix = parallelMix;
    m.busSaturationDrive = satDrive;
}
} // namespace

//==============================================================================
// ChannelStripFX
//==============================================================================

ChannelStripFX::ChannelStripFX() = default;
ChannelStripFX::~ChannelStripFX() = default;

void ChannelStripFX::prepare(double sampleRate, int maxBlockSize)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    currentBlockSize = std::max(16, maxBlockSize);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(currentBlockSize);
    spec.numChannels = 2;

    compressor.prepare(spec);
    compressor.setThreshold(lastCompThresh);
    compressor.setRatio(lastCompRatio);
    compressor.setAttack(lastCompAttack);
    compressor.setRelease(lastCompRelease);

    // Envelope follower coefficients for transient shaper
    // Fast: attack ~2ms, release ~25ms
    fastAttackCoeff = std::exp(-1.0f / (static_cast<float>(currentSampleRate) * 0.002f));
    fastReleaseCoeff = std::exp(-1.0f / (static_cast<float>(currentSampleRate) * 0.025f));

    // Slow: attack ~30ms, release ~150ms
    slowAttackCoeff = std::exp(-1.0f / (static_cast<float>(currentSampleRate) * 0.030f));
    slowReleaseCoeff = std::exp(-1.0f / (static_cast<float>(currentSampleRate) * 0.150f));

    reset();
}

void ChannelStripFX::reset()
{
    lowShelfL.reset();
    lowShelfR.reset();
    midPeakL.reset();
    midPeakR.reset();
    highShelfL.reset();
    highShelfR.reset();

    compressor.reset();

    fastEnvL = 0.0f;
    fastEnvR = 0.0f;
    slowEnvL = 0.0f;
    slowEnvR = 0.0f;

    // Force coefficient recalculation on next block
    lastLowGain = -999.0f;
}

void ChannelStripFX::updateFilters(const MicStripParams &params)
{
    const auto hpf = std::clamp(params.hpfHz, 20.0f, 500.0f);
    if (std::abs(hpf - lastHpf) > 0.5f)
    {
        lastHpf = hpf;
        auto c = juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, lastHpf, 0.707f);
        hpfL.coefficients = c;
        hpfR.coefficients = c;
    }

    const auto lpf = std::clamp(params.lpfHz, 2000.0f, 20000.0f);
    if (std::abs(lpf - lastLpf) > 0.5f)
    {
        lastLpf = lpf;
        auto c = juce::dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate, lastLpf, 0.707f);
        lpfL.coefficients = c;
        lpfR.coefficients = c;
    }

    if (std::abs(params.eqLowGainDb - lastLowGain) > 0.05f)
    {
        lastLowGain = params.eqLowGainDb;
        const auto gainLinear = dbToGain(lastLowGain);
        auto c = juce::dsp::IIR::Coefficients<float>::makeLowShelf(currentSampleRate, 80.0f, 0.707f, gainLinear);
        lowShelfL.coefficients = c;
        lowShelfR.coefficients = c;
    }

    if (std::abs(params.eqMidGainDb - lastMidGain) > 0.05f || std::abs(params.eqMidFreq - lastMidFreq) > 1.0f
        || std::abs(params.eqMidQ - lastMidQ) > 0.02f)
    {
        lastMidGain = params.eqMidGainDb;
        lastMidFreq = std::clamp(params.eqMidFreq, 100.0f, 10000.0f);
        lastMidQ = std::clamp(params.eqMidQ, 0.2f, 5.0f);
        const auto gainLinear = dbToGain(lastMidGain);
        auto c =
            juce::dsp::IIR::Coefficients<float>::makePeakFilter(currentSampleRate, lastMidFreq, lastMidQ, gainLinear);
        midPeakL.coefficients = c;
        midPeakR.coefficients = c;
    }

    if (std::abs(params.eqHighGainDb - lastHighGain) > 0.05f)
    {
        lastHighGain = params.eqHighGainDb;
        const auto gainLinear = dbToGain(lastHighGain);
        auto c = juce::dsp::IIR::Coefficients<float>::makeHighShelf(currentSampleRate, 10000.0f, 0.707f, gainLinear);
        highShelfL.coefficients = c;
        highShelfR.coefficients = c;
    }

    if (params.compEnabled)
    {
        if (std::abs(params.compThresholdDb - lastCompThresh) > 0.1f)
        {
            lastCompThresh = params.compThresholdDb;
            compressor.setThreshold(lastCompThresh);
        }
        if (std::abs(params.compRatio - lastCompRatio) > 0.1f)
        {
            lastCompRatio = params.compRatio;
            compressor.setRatio(lastCompRatio);
        }
        if (std::abs(params.compAttackMs - lastCompAttack) > 0.5f)
        {
            lastCompAttack = params.compAttackMs;
            compressor.setAttack(lastCompAttack);
        }
        if (std::abs(params.compReleaseMs - lastCompRelease) > 1.0f)
        {
            lastCompRelease = params.compReleaseMs;
            compressor.setRelease(lastCompRelease);
        }
    }
}

void ChannelStripFX::process(
    float *left, float *right, int numSamples, const MicStripParams &params, float *reverbAccumL, float *reverbAccumR)
{
    if (numSamples <= 0)
        return;

    // 1. Polarity / Phase Invert
    if (params.phaseInvert)
    {
        juce::FloatVectorOperations::multiply(left, -1.0f, numSamples);
        juce::FloatVectorOperations::multiply(right, -1.0f, numSamples);
    }

    if (!params.fxEnabled)
        return;

    updateFilters(params);

    // 2. High-pass / low-pass filters
    if (lastHpf > 21.0f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            left[i] = hpfL.processSample(left[i]);
            right[i] = hpfR.processSample(right[i]);
        }
    }
    if (lastLpf < 19900.0f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            left[i] = lpfL.processSample(left[i]);
            right[i] = lpfR.processSample(right[i]);
        }
    }

    // 3. 3-Band Parametric EQ
    if (params.eqEnabled)
    {
        if (std::abs(lastLowGain) > 0.05f)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                left[i] = lowShelfL.processSample(left[i]);
                right[i] = lowShelfR.processSample(right[i]);
            }
        }

        if (std::abs(lastMidGain) > 0.05f)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                left[i] = midPeakL.processSample(left[i]);
                right[i] = midPeakR.processSample(right[i]);
            }
        }

        if (std::abs(lastHighGain) > 0.05f)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                left[i] = highShelfL.processSample(left[i]);
                right[i] = highShelfR.processSample(right[i]);
            }
        }
    }

    // 3. VCA Compressor
    if (params.compEnabled)
    {
        float *channels[2] = {left, right};
        juce::dsp::AudioBlock<float> block(channels, 2, static_cast<size_t>(numSamples));
        juce::dsp::ProcessContextReplacing<float> context(block);
        compressor.process(context);

        if (params.compMakeupDb > 0.05f)
        {
            const float makeup = dbToGain(params.compMakeupDb);
            juce::FloatVectorOperations::multiply(left, makeup, numSamples);
            juce::FloatVectorOperations::multiply(right, makeup, numSamples);
        }
    }

    // 4. Tape / Tube Saturation (harmonic waveshaper)
    if (params.saturationDrive > 0.01f)
    {
        const float driveAmount = 1.0f + 2.5f * params.saturationDrive;
        const float normFactor = 1.0f / (1.0f + 1.2f * params.saturationDrive);

        for (int i = 0; i < numSamples; ++i)
        {
            left[i] = std::tanh(left[i] * driveAmount) * normFactor;
            right[i] = std::tanh(right[i] * driveAmount) * normFactor;
        }
    }

    // 5. Transient Shaper (attack punch & sustain body)
    if (std::abs(params.transientAttack) > 0.01f || std::abs(params.transientSustain) > 0.01f)
    {
        const float attCoeff = params.transientAttack;
        const float susCoeff = params.transientSustain;

        for (int i = 0; i < numSamples; ++i)
        {
            const float inAbsL = std::abs(left[i]);
            const float inAbsR = std::abs(right[i]);

            // Left envelope followers
            if (inAbsL > fastEnvL)
                fastEnvL = inAbsL + fastAttackCoeff * (fastEnvL - inAbsL);
            else
                fastEnvL = inAbsL + fastReleaseCoeff * (fastEnvL - inAbsL);

            if (inAbsL > slowEnvL)
                slowEnvL = inAbsL + slowAttackCoeff * (slowEnvL - inAbsL);
            else
                slowEnvL = inAbsL + slowReleaseCoeff * (slowEnvL - inAbsL);

            // Right envelope followers
            if (inAbsR > fastEnvR)
                fastEnvR = inAbsR + fastAttackCoeff * (fastEnvR - inAbsR);
            else
                fastEnvR = inAbsR + fastReleaseCoeff * (fastEnvR - inAbsR);

            if (inAbsR > slowEnvR)
                slowEnvR = inAbsR + slowAttackCoeff * (slowEnvR - inAbsR);
            else
                slowEnvR = inAbsR + slowReleaseCoeff * (slowEnvR - inAbsR);

            const float diffL = std::max(0.0f, fastEnvL - slowEnvL);
            const float diffR = std::max(0.0f, fastEnvR - slowEnvR);

            const float gainL = std::clamp(1.0f + attCoeff * 2.0f * (diffL / (fastEnvL + 0.01f))
                                               + susCoeff * 1.2f * (slowEnvL / (fastEnvL + 0.01f)),
                                           0.1f,
                                           3.0f);
            const float gainR = std::clamp(1.0f + attCoeff * 2.0f * (diffR / (fastEnvR + 0.01f))
                                               + susCoeff * 1.2f * (slowEnvR / (fastEnvR + 0.01f)),
                                           0.1f,
                                           3.0f);

            left[i] *= gainL;
            right[i] *= gainR;
        }
    }

    // 6. Reverb Send Accumulation
    if (params.reverbSend > 0.005f && reverbAccumL != nullptr && reverbAccumR != nullptr)
    {
        const float sendGain = params.reverbSend * 0.707f;
        for (int i = 0; i < numSamples; ++i)
        {
            reverbAccumL[i] += left[i] * sendGain;
            reverbAccumR[i] += right[i] * sendGain;
        }
    }
}

//==============================================================================
// MasterFX
//==============================================================================

MasterFX::MasterFX() = default;
MasterFX::~MasterFX() = default;

void MasterFX::prepare(double sampleRate, int maxBlockSize)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    currentBlockSize = std::max(16, maxBlockSize);

    reverbWetL.resize(static_cast<size_t>(currentBlockSize));
    reverbWetR.resize(static_cast<size_t>(currentBlockSize));
    parallelL.resize(static_cast<size_t>(currentBlockSize));
    parallelR.resize(static_cast<size_t>(currentBlockSize));

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(currentBlockSize);
    spec.numChannels = 2;

    reverb.prepare(spec);
    busCompressor.prepare(spec);
    reset();
}

void MasterFX::reset()
{
    reverb.reset();
    busCompressor.reset();
    busLowL.reset();
    busLowR.reset();
    busMidL.reset();
    busMidR.reset();
    busHighL.reset();
    busHighR.reset();
}

void MasterFX::process(float *masterL,
                       float *masterR,
                       int numSamples,
                       const float *reverbSendL,
                       const float *reverbSendR,
                       const MasterStripParams &params)
{
    if (numSamples <= 0)
        return;

    // 1. Drum-bus EQ
    if (std::abs(params.busEqLowDb - lastBusLow) > 0.05f)
    {
        lastBusLow = params.busEqLowDb;
        auto c = juce::dsp::IIR::Coefficients<float>::makeLowShelf(
            currentSampleRate, 80.0f, 0.707f, dbToGain(lastBusLow));
        busLowL.coefficients = c;
        busLowR.coefficients = c;
    }
    if (std::abs(params.busEqMidDb - lastBusMid) > 0.05f
        || std::abs(params.busEqMidFreq - lastBusMidFreq) > 1.0f)
    {
        lastBusMid = params.busEqMidDb;
        lastBusMidFreq = std::clamp(params.busEqMidFreq, 100.0f, 10000.0f);
        auto c = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
            currentSampleRate, lastBusMidFreq, 0.9f, dbToGain(lastBusMid));
        busMidL.coefficients = c;
        busMidR.coefficients = c;
    }
    if (std::abs(params.busEqHighDb - lastBusHigh) > 0.05f)
    {
        lastBusHigh = params.busEqHighDb;
        auto c = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            currentSampleRate, 10000.0f, 0.707f, dbToGain(lastBusHigh));
        busHighL.coefficients = c;
        busHighR.coefficients = c;
    }

    if (std::abs(lastBusLow) > 0.05f || std::abs(lastBusMid) > 0.05f || std::abs(lastBusHigh) > 0.05f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float l = masterL[i];
            float r = masterR[i];

            if (std::abs(lastBusLow) > 0.05f)
            {
                l = busLowL.processSample(l);
                r = busLowR.processSample(r);
            }
            if (std::abs(lastBusMid) > 0.05f)
            {
                l = busMidL.processSample(l);
                r = busMidR.processSample(r);
            }
            if (std::abs(lastBusHigh) > 0.05f)
            {
                l = busHighL.processSample(l);
                r = busHighR.processSample(r);
            }

            masterL[i] = l;
            masterR[i] = r;
        }
    }

    // 2. Drum-bus glue compressor, with a dry/compressed mix for parallel use.
    if (params.busCompEnabled)
    {
        if (std::abs(params.busCompThresholdDb - lastBusCompThreshold) > 0.1f)
        {
            lastBusCompThreshold = params.busCompThresholdDb;
            busCompressor.setThreshold(lastBusCompThreshold);
        }
        if (std::abs(params.busCompRatio - lastBusCompRatio) > 0.1f)
        {
            lastBusCompRatio = params.busCompRatio;
            busCompressor.setRatio(lastBusCompRatio);
        }
        if (std::abs(params.busCompAttackMs - lastBusCompAttack) > 0.5f)
        {
            lastBusCompAttack = params.busCompAttackMs;
            busCompressor.setAttack(lastBusCompAttack);
        }
        if (std::abs(params.busCompReleaseMs - lastBusCompRelease) > 1.0f)
        {
            lastBusCompRelease = params.busCompReleaseMs;
            busCompressor.setRelease(lastBusCompRelease);
        }

        const float mix = std::clamp(params.busCompMix, 0.0f, 1.0f);
        const bool parallel = mix < 0.999f;

        if (parallel)
        {
            std::copy(masterL, masterL + numSamples, parallelL.data());
            std::copy(masterR, masterR + numSamples, parallelR.data());
        }

        float *channels[2] = {masterL, masterR};
        juce::dsp::AudioBlock<float> block(channels, 2, static_cast<size_t>(numSamples));
        juce::dsp::ProcessContextReplacing<float> context(block);
        busCompressor.process(context);

        if (params.busCompMakeupDb > 0.05f)
        {
            const float makeup = dbToGain(params.busCompMakeupDb);
            juce::FloatVectorOperations::multiply(masterL, makeup, numSamples);
            juce::FloatVectorOperations::multiply(masterR, makeup, numSamples);
        }

        if (parallel)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                masterL[i] = parallelL[static_cast<size_t>(i)] * (1.0f - mix) + masterL[i] * mix;
                masterR[i] = parallelR[static_cast<size_t>(i)] * (1.0f - mix) + masterR[i] * mix;
            }
        }
    }

    // 3. Drum-bus saturation
    if (params.busSaturationDrive > 0.01f)
    {
        const float driveAmount = 1.0f + 2.5f * params.busSaturationDrive;
        const float normFactor = 1.0f / (1.0f + 1.2f * params.busSaturationDrive);

        for (int i = 0; i < numSamples; ++i)
        {
            masterL[i] = std::tanh(masterL[i] * driveAmount) * normFactor;
            masterR[i] = std::tanh(masterR[i] * driveAmount) * normFactor;
        }
    }

    // 4. Process algorithmic reverb return
    if (params.reverbReturn > 0.01f && reverbSendL != nullptr && reverbSendR != nullptr)
    {
        std::copy(reverbSendL, reverbSendL + numSamples, reverbWetL.data());
        std::copy(reverbSendR, reverbSendR + numSamples, reverbWetR.data());

        // A small dry feed makes the master reverb useful as a master room even
        // when a preset has all channel reverb sends at zero.
        constexpr float masterDrySend = 0.15f;
        for (int i = 0; i < numSamples; ++i)
        {
            reverbWetL[static_cast<size_t>(i)] += masterL[i] * masterDrySend;
            reverbWetR[static_cast<size_t>(i)] += masterR[i] * masterDrySend;
        }

        juce::dsp::Reverb::Parameters rParams;
        rParams.roomSize = std::clamp(params.reverbSize, 0.0f, 1.0f);
        rParams.damping = std::clamp(params.reverbDamp, 0.0f, 1.0f);
        rParams.wetLevel = 1.0f;
        rParams.dryLevel = 0.0f;
        rParams.width = std::clamp(params.reverbWidth, 0.0f, 1.0f);
        rParams.freezeMode = 0.0f;

        if (rParams.roomSize != lastReverbParams.roomSize || rParams.damping != lastReverbParams.damping
            || rParams.width != lastReverbParams.width)
        {
            lastReverbParams = rParams;
            reverb.setParameters(rParams);
        }

        float *revChannels[2] = {reverbWetL.data(), reverbWetR.data()};
        juce::dsp::AudioBlock<float> revBlock(revChannels, 2, static_cast<size_t>(numSamples));
        juce::dsp::ProcessContextReplacing<float> revContext(revBlock);
        reverb.process(revContext);

        const float wetGain = params.reverbReturn;
        for (int i = 0; i < numSamples; ++i)
        {
            masterL[i] += reverbWetL[static_cast<size_t>(i)] * wetGain;
            masterR[i] += reverbWetR[static_cast<size_t>(i)] * wetGain;
        }
    }

    // 2. Master pan and level (balance panning for stereo bus)
    float panL = 1.0f, panR = 1.0f;
    if (params.pan < 0.0f)
    {
        panL = 1.0f;
        panR = 1.0f + params.pan;
    }
    else if (params.pan > 0.0f)
    {
        panL = 1.0f - params.pan;
        panR = 1.0f;
    }

    const float finalGainL = params.level * panL;
    const float finalGainR = params.level * panR;

    for (int i = 0; i < numSamples; ++i)
    {
        float outL = masterL[i] * finalGainL;
        float outR = masterR[i] * finalGainR;

        if (params.limiterEnabled)
        {
            outL = softClip(outL);
            outR = softClip(outR);
        }

        masterL[i] = outL;
        masterR[i] = outR;
    }
}

//==============================================================================
// Factory Mixer Presets
//==============================================================================

MixerPreset MixerPreset::createDefaultClean(int numMics, const std::vector<juce::String> &names)
{
    MixerPreset p;
    p.name = "Default Clean";
    p.master.level = 0.88f;
    p.master.limiterEnabled = true;
    p.master.reverbReturn = 0.15f;
    p.master.reverbSize = 0.40f;
    setFactoryBus(p.master, 0.8f, -0.3f, 650.0f, 0.5f, -18.0f, 2.0f, 30.0f, 180.0f, 1.0f, 0.35f, 0.03f);
    p.micNames = names;

    p.micStrips.resize(static_cast<size_t>(numMics));

    for (size_t i = 0; i < static_cast<size_t>(numMics); ++i)
    {
        const auto role = classifyMic(i < names.size() ? names[i] : juce::String());
        auto &s = p.micStrips[i];
        s.level = 1.0f;
        s.pan = 0.0f;
        s.eqEnabled = true;
        s.compEnabled = false;
        s.saturationDrive = 0.0f;
        s.transientAttack = 0.0f;
        s.transientSustain = 0.0f;
        s.reverbSend = 0.08f;

        if (role.kick)
        {
            s.level = role.trigger ? 1.35f : 1.15f;
            s.mono = true;
            s.eqLowGainDb = 2.5f;
            s.eqMidGainDb = -1.5f;
            s.eqMidFreq = 400.0f;
            s.eqHighGainDb = role.trigger ? 3.5f : 1.5f;
            s.reverbSend = 0.02f;
        }
        else if (role.snare)
        {
            s.level = 1.10f;
            s.eqLowGainDb = 0.5f;
            s.eqMidGainDb = 1.5f;
            s.eqMidFreq = 1800.0f;
            s.eqHighGainDb = 1.5f;
            s.reverbSend = 0.10f;
        }
        else if (role.tom)
        {
            s.level = 1.0f;
            s.eqLowGainDb = 1.0f;
            s.eqHighGainDb = 0.5f;
            s.reverbSend = 0.06f;
        }
        else if (role.hat)
        {
            s.level = 0.62f;
            s.eqLowGainDb = -6.0f;
            s.eqHighGainDb = -1.5f;
            s.reverbSend = 0.04f;
            s.pan = role.left ? -0.25f : (role.right ? 0.25f : -0.15f);
        }
        else if (role.cymbal)
        {
            s.level = 0.58f;
            s.eqLowGainDb = -7.0f;
            s.eqHighGainDb = -2.0f;
            s.reverbSend = 0.05f;
            s.pan = role.left ? -0.35f : (role.right ? 0.35f : 0.0f);
        }
        else if (role.overhead)
        {
            s.level = 0.78f;
            s.eqLowGainDb = -5.0f;
            s.reverbSend = 0.06f;
            s.pan = role.left ? -0.55f : (role.right ? 0.55f : 0.0f);
        }
        else if (role.room)
        {
            s.level = 0.45f;
            s.eqLowGainDb = -7.0f;
            s.eqHighGainDb = -1.0f;
            s.reverbSend = 0.04f;
            s.pan = role.left ? -0.65f : (role.right ? 0.65f : 0.0f);
        }
        applyFactoryMicFilters(s, role);
        applyFactoryMicPolarity(s, i < names.size() ? names[i] : juce::String());
    }
    return p;
}

MixerPreset MixerPreset::createMassiveRock(int numMics, const std::vector<juce::String> &names)
{
    MixerPreset p;
    p.name = "Massive Rock";
    p.master.level = 0.88f;
    p.master.limiterEnabled = true;
    p.master.reverbReturn = 0.35f;
    p.master.reverbSize = 0.65f;
    p.master.reverbDamp = 0.30f;
    setFactoryBus(p.master, 1.5f, -0.8f, 500.0f, 0.8f, -16.0f, 4.0f, 20.0f, 120.0f, 2.0f, 0.30f, 0.12f);
    p.micNames = names;

    p.micStrips.resize(static_cast<size_t>(numMics));
    for (size_t i = 0; i < static_cast<size_t>(numMics); ++i)
    {
        const auto n = (i < names.size()) ? names[i].toLowerCase() : juce::String();
        const auto role = classifyMic(i < names.size() ? names[i] : juce::String());
        auto &s = p.micStrips[i];
        s.level = 1.0f;
        s.eqEnabled = true;

        if (role.kick)
        {
            s.level = role.trigger ? 1.35f : 1.18f;
            s.mono = true;
            s.eqLowGainDb = 4.5f;  // Sub punch
            s.eqMidGainDb = -3.5f; // Scoop boxy 400Hz
            s.eqMidFreq = 400.0f;
            s.eqHighGainDb = role.trigger ? 4.5f : 3.0f; // Beater click
            s.compEnabled = true;
            s.compThresholdDb = role.trigger ? -14.0f : -16.0f;
            s.compRatio = 4.0f;
            s.compAttackMs = 25.0f; // Let punch through
            s.compReleaseMs = 80.0f;
            s.compMakeupDb = 2.5f;
            s.saturationDrive = 0.15f;
            s.transientAttack = 0.25f;
            s.reverbSend = 0.05f;
        }
        else if (role.snare)
        {
            s.level = 1.15f;
            s.eqLowGainDb = 2.0f; // Low body
            s.eqMidGainDb = 2.5f; // 1.2k crack
            s.eqMidFreq = 1200.0f;
            s.eqHighGainDb = 4.0f; // Air sizzle
            s.compEnabled = true;
            s.compThresholdDb = -14.0f;
            s.compRatio = 4.0f;
            s.compAttackMs = 18.0f;
            s.compReleaseMs = 120.0f;
            s.compMakeupDb = 2.0f;
            s.saturationDrive = 0.25f; // Warm tube drive
            s.transientAttack = 0.35f; // Extra snare crack
            s.reverbSend = 0.30f;
            if (n.contains("bottom") || n.contains("snr_bot"))
                s.phaseInvert = true; // Invert bottom snare mic phase
        }
        else if (role.hat)
        {
            s.level = 0.55f;
            s.eqLowGainDb = -7.0f;
            s.eqMidGainDb = -1.0f;
            s.eqMidFreq = 3000.0f;
            s.eqHighGainDb = -2.0f;
            s.compEnabled = true;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.compAttackMs = 5.0f;
            s.compReleaseMs = 80.0f;
            s.transientAttack = -0.10f;
            s.transientSustain = -0.10f;
            s.saturationDrive = 0.05f;
            s.reverbSend = 0.06f;
            s.pan = role.left ? -0.25f : (role.right ? 0.25f : -0.15f);
        }
        else if (role.cymbal)
        {
            s.level = 0.58f;
            s.eqLowGainDb = -8.0f;
            s.eqMidGainDb = -1.5f;
            s.eqMidFreq = 3500.0f;
            s.eqHighGainDb = -2.0f;
            s.compEnabled = true;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.compAttackMs = 5.0f;
            s.compReleaseMs = 100.0f;
            s.transientAttack = -0.15f;
            s.transientSustain = -0.10f;
            s.saturationDrive = 0.05f;
            s.reverbSend = 0.08f;
            s.pan = role.left ? -0.35f : (role.right ? 0.35f : 0.0f);
        }
        else if (role.overhead)
        {
            s.level = 0.78f;
            s.eqLowGainDb = -5.0f; // High-pass rumble
            s.eqHighGainDb = 1.0f; // Open but not splashy
            s.compEnabled = true;
            s.compThresholdDb = -18.0f;
            s.compRatio = 2.5f;
            s.saturationDrive = 0.10f;
            s.reverbSend = 0.20f;
            s.pan = role.left ? -0.55f : (role.right ? 0.55f : 0.0f);
        }
        else if (role.room)
        {
            s.level = 0.85f; // Still present, but not overpowering
            s.eqLowGainDb = 3.0f;
            s.eqHighGainDb = -2.0f;
            s.compEnabled = true; // Heavy room smashing
            s.compThresholdDb = -22.0f;
            s.compRatio = 6.0f;
            s.compAttackMs = 5.0f;
            s.compReleaseMs = 150.0f;
            s.compMakeupDb = 4.5f;
            s.saturationDrive = 0.30f;
            s.transientSustain = 0.35f;
            s.reverbSend = 0.40f;
            s.pan = role.left ? -0.65f : (role.right ? 0.65f : 0.0f);
        }
        else // Toms, Percussion
        {
            s.level = 1.0f;
            s.compEnabled = true;
            s.compThresholdDb = -15.0f;
            s.compRatio = 3.5f;
            s.transientAttack = 0.20f;
            s.reverbSend = 0.25f;
        }
        applyFactoryMicFilters(s, role);
        applyFactoryMicPolarity(s, i < names.size() ? names[i] : juce::String());
    }
    return p;
}

MixerPreset MixerPreset::createVintageWarmth(int numMics, const std::vector<juce::String> &names)
{
    MixerPreset p;
    p.name = "Vintage Warmth";
    p.master.level = 0.85f;
    p.master.limiterEnabled = true;
    p.master.reverbReturn = 0.30f;
    p.master.reverbSize = 0.55f;
    p.master.reverbDamp = 0.60f; // Dark warm reverb
    setFactoryBus(p.master, 2.0f, 0.5f, 800.0f, -0.8f, -20.0f, 2.0f, 40.0f, 200.0f, 1.5f, 0.50f, 0.15f);
    p.micNames = names;

    p.micStrips.resize(static_cast<size_t>(numMics));
    for (size_t i = 0; i < static_cast<size_t>(numMics); ++i)
    {
        const auto role = classifyMic(i < names.size() ? names[i] : juce::String());
        auto &s = p.micStrips[i];
        s.level = 1.0f;
        s.eqEnabled = true;
        s.eqLowGainDb = 2.0f;      // Warm low end
        s.eqHighGainDb = -2.5f;    // Tamed tape top end
        s.saturationDrive = 0.35f; // Generous tape drive
        s.compEnabled = true;
        s.compThresholdDb = -12.0f;
        s.compRatio = 2.0f; // Gentle vintage glue
        s.compAttackMs = 30.0f;
        s.compReleaseMs = 150.0f;
        s.transientAttack = -0.10f; // Rounded vintage transient
        s.transientSustain = 0.20f; // Warm woody resonance
        s.reverbSend = 0.15f;

        if (role.kick)
        {
            s.level = role.trigger ? 1.35f : 1.18f;
            s.mono = true;
            s.eqLowGainDb = 3.0f;
            s.eqMidGainDb = -1.0f;
            s.eqMidFreq = 450.0f;
            s.eqHighGainDb = role.trigger ? 2.5f : 0.5f;
            s.reverbSend = 0.04f;
        }
        else if (role.snare)
        {
            s.level = 1.12f;
            s.eqMidGainDb = 1.5f;
            s.eqMidFreq = 1500.0f;
            s.reverbSend = 0.12f;
        }
        else if (role.tom)
        {
            s.level = 1.05f;
            s.eqLowGainDb = 2.5f;
            s.reverbSend = 0.10f;
        }
        else if (role.hat)
        {
            s.level = 0.60f;
            s.eqLowGainDb = -6.0f;
            s.eqHighGainDb = -3.0f;
            s.saturationDrive = 0.20f;
            s.reverbSend = 0.06f;
            s.pan = role.left ? -0.25f : (role.right ? 0.25f : -0.15f);
        }
        else if (role.cymbal)
        {
            s.level = 0.58f;
            s.eqLowGainDb = -7.0f;
            s.eqHighGainDb = -3.0f;
            s.saturationDrive = 0.20f;
            s.reverbSend = 0.08f;
            s.pan = role.left ? -0.35f : (role.right ? 0.35f : 0.0f);
        }
        else if (role.overhead)
        {
            s.level = 0.78f;
            s.eqLowGainDb = -5.0f;
            s.reverbSend = 0.10f;
            s.pan = role.left ? -0.55f : (role.right ? 0.55f : 0.0f);
        }
        else if (role.room)
        {
            s.level = 0.50f;
            s.eqLowGainDb = -6.0f;
            s.reverbSend = 0.12f;
            s.pan = role.left ? -0.65f : (role.right ? 0.65f : 0.0f);
        }
        applyFactoryMicFilters(s, role);
        applyFactoryMicPolarity(s, i < names.size() ? names[i] : juce::String());
        if (role.overhead) { s.hpfHz = 150.0f; s.lpfHz = 13000.0f; }
        else if (role.room) { s.hpfHz = 120.0f; s.lpfHz = 9000.0f; }
    }
    return p;
}

MixerPreset MixerPreset::createPunchyStudio(int numMics, const std::vector<juce::String> &names)
{
    MixerPreset p;
    p.name = "Punchy Studio";
    p.master.level = 0.9f;
    p.master.limiterEnabled = true;
    p.master.reverbReturn = 0.12f;
    p.master.reverbSize = 0.35f;
    p.master.reverbDamp = 0.45f;
    p.master.reverbWidth = 0.9f;
    setFactoryBus(p.master, 1.0f, -0.5f, 600.0f, 1.0f, -18.0f, 3.5f, 12.0f, 90.0f, 1.5f, 0.20f, 0.06f);
    p.micNames = names;

    p.micStrips.resize(static_cast<size_t>(numMics));

    for (size_t i = 0; i < static_cast<size_t>(numMics); ++i)
    {
        const auto n = (i < names.size()) ? names[i].toLowerCase() : juce::String();
        const auto role = classifyMic(i < names.size() ? names[i] : juce::String());
        auto &s = p.micStrips[i];
        s.level = 1.0f;
        s.pan = 0.0f;
        s.fxEnabled = true;
        s.eqEnabled = true;
        s.compEnabled = false;
        s.reverbSend = 0.06f;

        const bool isTrigger = n.contains("trigger");
        const bool isKick = isTrigger || n.contains("kick") || n.contains("kdrum") || n.contains("bdrum");
        const bool isSnare = n.contains("snare") || n.contains("snr");
        const bool isHat = n.contains("hihat") || n.contains("hh") || n.contains("hat");
        const bool isCymbal = n.contains("ride") || n.contains("crash") || n.contains("china") || n.contains("splash")
                               || n.contains("cymbal") || n.contains("zilbel") || n.contains("bell");
        const bool isOverhead = n.contains("overhead") || n.contains("oh");
        const bool isRoom = n.contains("amb") || n.contains("room") || n.contains("pzm");
        const bool isTom = n.contains("tom") || n.contains("floor");

        const bool centered = n.contains("center") || n.contains("centre") || n.contains("mono");
        const bool left = !centered
                          && (n.endsWith("l") || n.contains("left") || n.contains("_l") || n.contains("ohl")
                              || n.contains("ambl"));
        const bool right = !centered
                           && (n.endsWith("r") || n.contains("right") || n.contains("_r") || n.contains("ohr")
                               || n.contains("ambr"));

        if (isKick)
        {
            s.level = isTrigger ? 1.45f : 1.30f;
            s.mono = true;
            s.eqLowGainDb = 4.0f;
            s.eqMidGainDb = isTrigger ? -6.0f : -3.0f;
            s.eqMidFreq = isTrigger ? 700.0f : 400.0f;
            s.eqHighGainDb = isTrigger ? 5.0f : 2.5f;
            s.compEnabled = true;
            s.compThresholdDb = isTrigger ? -12.0f : -18.0f;
            s.compRatio = isTrigger ? 6.0f : 4.0f;
            s.compAttackMs = isTrigger ? 2.0f : 20.0f;
            s.compReleaseMs = isTrigger ? 50.0f : 90.0f;
            s.compMakeupDb = isTrigger ? 3.0f : 2.0f;
            s.saturationDrive = isTrigger ? 0.25f : 0.15f;
            s.transientAttack = isTrigger ? 0.45f : 0.30f;
            s.transientSustain = -0.15f;
            s.reverbSend = 0.0f;
        }
        else if (isSnare)
        {
            s.level = 1.35f;
            s.eqLowGainDb = 1.5f;
            s.eqMidGainDb = 2.5f;
            s.eqMidFreq = 1800.0f;
            s.eqHighGainDb = 3.5f;
            s.compEnabled = true;
            s.compThresholdDb = -16.0f;
            s.compRatio = 3.5f;
            s.compAttackMs = 15.0f;
            s.compReleaseMs = 120.0f;
            s.compMakeupDb = 2.0f;
            s.saturationDrive = 0.20f;
            s.transientAttack = 0.35f;
            s.transientSustain = -0.05f;
            s.reverbSend = 0.12f;

            if (n.contains("bottom") || n.contains("bot") || n.contains("under"))
                s.phaseInvert = true;
        }
        else if (isTom)
        {
            s.level = 1.10f;
            s.eqLowGainDb = 2.0f;
            s.eqMidGainDb = -1.5f;
            s.eqMidFreq = 500.0f;
            s.eqHighGainDb = 1.0f;
            s.compEnabled = true;
            s.compThresholdDb = -18.0f;
            s.compRatio = 3.0f;
            s.compAttackMs = 20.0f;
            s.compReleaseMs = 120.0f;
            s.compMakeupDb = 1.0f;
            s.saturationDrive = 0.10f;
            s.transientAttack = 0.20f;
            s.reverbSend = 0.06f;
        }
        else if (isHat)
        {
            s.level = 0.50f;
            s.eqLowGainDb = -8.0f;
            s.eqMidGainDb = -1.5f;
            s.eqMidFreq = 3000.0f;
            s.eqHighGainDb = -1.5f;
            s.compEnabled = true;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.compAttackMs = 5.0f;
            s.compReleaseMs = 80.0f;
            s.transientAttack = -0.10f;
            s.reverbSend = 0.06f;
            s.pan = left ? -0.25f : (right ? 0.25f : -0.15f);
        }
        else if (isCymbal)
        {
            s.level = 0.48f;
            s.eqLowGainDb = -9.0f;
            s.eqMidGainDb = -2.0f;
            s.eqMidFreq = 3500.0f;
            s.eqHighGainDb = -2.0f;
            s.compEnabled = true;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.compAttackMs = 5.0f;
            s.compReleaseMs = 120.0f;
            s.transientAttack = -0.15f;
            s.reverbSend = 0.08f;
            s.pan = left ? -0.35f : (right ? 0.35f : 0.0f);
        }
        else if (isOverhead)
        {
            s.level = 0.55f;
            s.eqLowGainDb = -8.0f;
            s.eqMidGainDb = -1.0f;
            s.eqMidFreq = 2500.0f;
            s.eqHighGainDb = 0.5f;
            s.compEnabled = true;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.compAttackMs = 10.0f;
            s.compReleaseMs = 120.0f;
            s.saturationDrive = 0.05f;
            s.reverbSend = 0.08f;
            s.pan = left ? -0.55f : (right ? 0.55f : 0.0f);
        }
        else if (isRoom)
        {
            s.level = 0.28f;
            s.eqLowGainDb = -10.0f;
            s.eqMidGainDb = -2.0f;
            s.eqMidFreq = 800.0f;
            s.eqHighGainDb = -2.0f;
            s.compEnabled = true;
            s.compThresholdDb = -24.0f;
            s.compRatio = 2.5f;
            s.compAttackMs = 10.0f;
            s.compReleaseMs = 180.0f;
            s.saturationDrive = 0.08f;
            s.reverbSend = 0.05f;
            s.pan = left ? -0.65f : (right ? 0.65f : 0.0f);
        }
        applyFactoryMicFilters(s, role);
        applyFactoryMicPolarity(s, i < names.size() ? names[i] : juce::String());
    }

    return p;
}

MixerPreset MixerPreset::createDryTight(int numMics, const std::vector<juce::String> &names)
{
    MixerPreset p;
    p.name = "Dry & Tight";
    p.master.level = 0.85f;
    p.master.limiterEnabled = true;
    p.master.reverbReturn = 0.0f; // Zero reverb!
    setFactoryBus(p.master, 0.5f, -0.5f, 700.0f, 0.5f, -20.0f, 3.0f, 10.0f, 80.0f, 1.0f, 0.15f, 0.02f);
    p.micNames = names;

    p.micStrips.resize(static_cast<size_t>(numMics));
    for (size_t i = 0; i < static_cast<size_t>(numMics); ++i)
    {
        const auto n = (i < names.size()) ? names[i].toLowerCase() : juce::String();
        const auto role = classifyMic(i < names.size() ? names[i] : juce::String());
        auto &s = p.micStrips[i];
        s.level = 1.0f;
        s.eqEnabled = true;
        s.compEnabled = true;
        s.compThresholdDb = -16.0f;
        s.compRatio = 4.5f;
        s.compAttackMs = 10.0f;
        s.compReleaseMs = 60.0f; // Fast recovery
        s.transientAttack = 0.30f;
        s.transientSustain = -0.40f; // Deadened tight tails
        s.saturationDrive = 0.05f;
        s.reverbSend = 0.0f;

        if (role.kick)
        {
            s.level = role.trigger ? 1.35f : 1.20f;
            s.mono = true;
            s.eqLowGainDb = 2.5f;
            s.eqMidGainDb = role.trigger ? -4.0f : -2.0f;
            s.eqMidFreq = role.trigger ? 700.0f : 500.0f;
            s.eqHighGainDb = role.trigger ? 3.5f : 2.0f;
        }
        else if (role.snare)
        {
            s.level = 1.15f;
            s.eqLowGainDb = 1.0f;
            s.eqMidGainDb = 2.0f;
            s.eqMidFreq = 2000.0f;
            s.eqHighGainDb = 2.0f;
        }
        else if (role.tom)
        {
            s.level = 1.05f;
        }
        else if (role.hat)
        {
            s.level = 0.58f;
            s.eqLowGainDb = -8.0f;
            s.eqHighGainDb = -2.0f;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.transientAttack = -0.15f;
            s.transientSustain = -0.20f;
            s.pan = role.left ? -0.25f : (role.right ? 0.25f : -0.15f);
        }
        else if (role.cymbal)
        {
            s.level = 0.56f;
            s.eqLowGainDb = -8.0f;
            s.eqHighGainDb = -2.5f;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.transientAttack = -0.20f;
            s.transientSustain = -0.20f;
            s.pan = role.left ? -0.35f : (role.right ? 0.35f : 0.0f);
        }
        else if (role.overhead)
        {
            s.level = 0.58f;
            s.eqLowGainDb = -7.0f;
            s.pan = role.left ? -0.55f : (role.right ? 0.55f : 0.0f);
        }
        else if (role.room)
        {
            s.level = 0.20f; // Heavily duck room/ambience
            s.eqLowGainDb = -9.0f;
            s.mute = false;
            s.pan = role.left ? -0.65f : (role.right ? 0.65f : 0.0f);
        }
        applyFactoryMicFilters(s, role);
        applyFactoryMicPolarity(s, i < names.size() ? names[i] : juce::String());
        if (role.kick) { s.hpfHz = 30.0f; s.lpfHz = 18000.0f; }
        else if (role.snare) { s.hpfHz = 80.0f; s.lpfHz = 18000.0f; }
        else if (role.tom) { s.hpfHz = 40.0f; s.lpfHz = 18000.0f; }
        else if (role.hat) { s.hpfHz = 500.0f; s.lpfHz = 14000.0f; }
        else if (role.cymbal) { s.hpfHz = 600.0f; s.lpfHz = 14000.0f; }
        else if (role.overhead) { s.hpfHz = 300.0f; s.lpfHz = 12000.0f; }
        else if (role.room) { s.hpfHz = 300.0f; s.lpfHz = 9000.0f; }
    }
    return p;
}

MixerPreset MixerPreset::createMixReady(int numMics, const std::vector<juce::String> &names)
{
    MixerPreset p;
    p.name = "Mix Ready";
    p.master.level = 0.9f;
    p.master.limiterEnabled = true;
    p.master.reverbReturn = 0.12f;
    p.master.reverbSize = 0.35f;
    p.master.reverbDamp = 0.45f;
    p.master.reverbWidth = 0.9f;
    setFactoryBus(p.master, 1.0f, -0.5f, 700.0f, 0.7f, -18.0f, 2.5f, 25.0f, 150.0f, 1.2f, 0.35f, 0.06f);
    p.micNames = names;

    p.micStrips.resize(static_cast<size_t>(numMics));

    // Toms are spread across the image in channel order, with floor toms on the
    // right. The first floor tom carries the extra body/attack from the saved
    // preset; the second floor tom stays neutral.
    std::vector<int> tomIndices;
    std::vector<bool> floorTomFlags(static_cast<size_t>(numMics), false);
    for (int i = 0; i < numMics; ++i)
    {
        const auto n = (i < static_cast<int>(names.size())) ? names[static_cast<size_t>(i)].toLowerCase() : juce::String();
        const auto role = classifyMic(i < static_cast<int>(names.size()) ? names[static_cast<size_t>(i)] : juce::String());
        if (!role.tom)
            continue;

        const bool explicitFloor = n.contains("floor") || n.contains("ftom")
                                   || n.contains("tom4") || n.contains("tom5") || n.contains("tom6")
                                   || n.contains("tom7") || n.contains("tom8");
        floorTomFlags[static_cast<size_t>(i)] = explicitFloor;
        tomIndices.push_back(i);
    }

    bool hasFloorTom = false;
    for (bool f : floorTomFlags)
        hasFloorTom = hasFloorTom || f;

    if (!hasFloorTom && !tomIndices.empty())
        floorTomFlags[static_cast<size_t>(tomIndices.back())] = true;

    std::vector<int> rackToms, floorToms;
    for (int idx : tomIndices)
        (floorTomFlags[static_cast<size_t>(idx)] ? floorToms : rackToms).push_back(idx);

    const auto ordinalIn = [] (const std::vector<int> &list, int index)
    {
        const auto it = std::find(list.begin(), list.end(), index);
        return it == list.end() ? -1 : static_cast<int>(it - list.begin());
    };

    const auto rackPan = [] (int ordinal, int count)
    {
        if (count <= 1)
            return -0.20f;
        if (count == 2)
            return ordinal == 0 ? -0.342857f : -0.057143f;
        if (count == 3)
            return ordinal == 0 ? -0.342857f : (ordinal == 1 ? -0.20f : -0.057143f);

        const auto t = static_cast<float>(ordinal) / static_cast<float>(count - 1);
        return -0.342857f + t * 0.285714f;
    };

    const auto floorPan = [] (int ordinal, int count)
    {
        if (count <= 1)
            return 0.40f;
        if (count == 2)
            return ordinal == 0 ? 0.371429f : 0.428571f;
        if (count == 3)
            return ordinal == 0 ? 0.371429f : (ordinal == 1 ? 0.40f : 0.428571f);

        const auto t = static_cast<float>(ordinal) / static_cast<float>(count - 1);
        return 0.371429f + t * 0.057142f;
    };

    for (int i = 0; i < numMics; ++i)
    {
        const auto name = i < static_cast<int>(names.size()) ? names[static_cast<size_t>(i)] : juce::String();
        const auto n = name.toLowerCase();
        const auto role = classifyMic(name);
        auto &s = p.micStrips[static_cast<size_t>(i)];

        s.level = 1.0f;
        s.pan = 0.0f;
        s.fxEnabled = true;
        s.eqEnabled = true;
        s.compEnabled = false;
        s.reverbSend = 0.06f;

        if (role.kick)
        {
            s.level = role.trigger ? 1.45f : 1.30f;
            s.mono = true;
            s.eqLowGainDb = 4.0f;
            s.eqMidGainDb = role.trigger ? -6.0f : -3.0f;
            s.eqMidFreq = role.trigger ? 700.0f : 400.0f;
            s.eqHighGainDb = role.trigger ? 5.0f : 2.5f;
            s.compEnabled = true;
            s.compThresholdDb = role.trigger ? -12.0f : -18.0f;
            s.compRatio = role.trigger ? 6.0f : 4.0f;
            s.compAttackMs = role.trigger ? 2.0f : 20.0f;
            s.compReleaseMs = role.trigger ? 50.0f : 90.0f;
            s.compMakeupDb = role.trigger ? 3.0f : 2.0f;
            s.saturationDrive = role.trigger ? 0.25f : 0.15f;
            s.transientAttack = role.trigger ? 0.45f : 0.30f;
            s.transientSustain = -0.15f;
            s.reverbSend = 0.0f;
        }
        else if (role.snare)
        {
            const bool bottom = n.contains("bottom") || n.contains("bot") || n.contains("under");

            s.level = 1.35f;
            s.eqLowGainDb = 1.5f;
            s.eqMidGainDb = 2.5f;
            s.eqMidFreq = 1800.0f;
            s.eqHighGainDb = 3.5f;
            s.compEnabled = true;
            s.compRatio = 3.5f;
            s.saturationDrive = 0.20f;
            s.transientAttack = 0.35f;
            s.reverbSend = 0.12f;

            if (bottom)
            {
                s.compThresholdDb = -16.0f;
                s.compAttackMs = 15.0f;
                s.compReleaseMs = 120.0f;
                s.compMakeupDb = 2.0f;
                s.transientSustain = -0.05f;
                s.phaseInvert = true;
            }
            else
            {
                // The top snare is deliberately squashed and held in the saved
                // preset: more sustain keeps it sitting forward in the mix.
                s.compThresholdDb = -8.6f;
                s.compAttackMs = 9.7733f;
                s.compReleaseMs = 50.9333f;
                s.compMakeupDb = 5.04f;
                s.transientSustain = 1.0f;
            }
        }
        else if (role.tom)
        {
            s.level = 1.10f;
            s.eqLowGainDb = 2.0f;
            s.eqMidGainDb = -1.5f;
            s.eqMidFreq = 500.0f;
            s.eqHighGainDb = 1.0f;
            s.compEnabled = true;
            s.compThresholdDb = -18.0f;
            s.compRatio = 3.0f;
            s.compAttackMs = 20.0f;
            s.compReleaseMs = 120.0f;
            s.compMakeupDb = 1.0f;
            s.saturationDrive = 0.10f;
            s.transientAttack = 0.20f;
            s.reverbSend = 0.06f;

            const auto rackOrdinal = ordinalIn(rackToms, i);
            const auto floorOrdinal = ordinalIn(floorToms, i);

            if (floorOrdinal >= 0)
                s.pan = floorPan(floorOrdinal, static_cast<int>(floorToms.size()));
            else if (rackOrdinal >= 0)
                s.pan = rackPan(rackOrdinal, static_cast<int>(rackToms.size()));

            if (rackOrdinal == 0)
                s.transientSustain = 0.64f;

            if (floorOrdinal == 0)
            {
                s.eqLowGainDb = 5.36f;
                s.eqHighGainDb = 5.32f;
                s.compThresholdDb = -17.2f;
                s.transientAttack = 0.9067f;
                s.transientSustain = 1.0f;
            }
        }
        else if (role.hat)
        {
            s.level = 0.50f;
            s.eqLowGainDb = -8.0f;
            s.eqMidGainDb = -1.5f;
            s.eqMidFreq = 3000.0f;
            s.eqHighGainDb = -1.5f;
            s.compEnabled = true;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.compAttackMs = 5.0f;
            s.compReleaseMs = 80.0f;
            s.transientAttack = -0.10f;
            s.reverbSend = 0.06f;
            s.pan = role.left ? -0.25f : (role.right ? 0.25f : -0.15f);
        }
        else if (role.cymbal)
        {
            s.level = 0.48f;
            s.eqLowGainDb = -9.0f;
            s.eqMidGainDb = -2.0f;
            s.eqMidFreq = 3500.0f;
            s.eqHighGainDb = -2.0f;
            s.compEnabled = true;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.compAttackMs = 5.0f;
            s.compReleaseMs = 120.0f;
            s.transientAttack = -0.15f;
            s.reverbSend = 0.08f;
            s.pan = role.left ? -0.35f : (role.right ? 0.35f : 0.0f);
        }
        else if (role.overhead)
        {
            s.level = role.left ? 0.8867f : (role.right ? 0.8763f : 0.55f);
            s.eqLowGainDb = -8.0f;
            s.eqMidGainDb = -1.0f;
            s.eqMidFreq = 2500.0f;
            s.eqHighGainDb = 0.5f;
            s.compEnabled = true;
            s.compThresholdDb = -22.0f;
            s.compRatio = 2.0f;
            s.compAttackMs = 10.0f;
            s.compReleaseMs = 120.0f;
            s.saturationDrive = 0.05f;
            s.reverbSend = 0.08f;
            s.pan = role.left ? -0.55f : (role.right ? 0.55f : 0.0f);
        }
        else if (role.room)
        {
            s.level = 0.6489f;
            s.eqLowGainDb = -10.0f;
            s.eqMidGainDb = -2.0f;
            s.eqMidFreq = 800.0f;
            s.eqHighGainDb = -2.0f;
            s.compEnabled = true;
            s.compRatio = 8.0f;
            s.saturationDrive = 0.08f;
            s.reverbSend = 0.05f;
            s.pan = role.left ? -0.65f : (role.right ? 0.65f : 0.0f);

            // Squash both room channels equally. The saved preset had the left
            // room compressed hard and the right room effectively open; keep the
            // aggressive squash on both sides for a consistent stereo image.
            s.compThresholdDb = -30.0f;
            s.compAttackMs = 6.2267f;
            s.compReleaseMs = 104.0f;
            s.compMakeupDb = 8.24f;
        }
        applyFactoryMicFilters(s, role);
        applyFactoryMicPolarity(s, i < names.size() ? names[i] : juce::String());
    }

    return p;
}

//==============================================================================
// XML Serialization
//==============================================================================

juce::String MixerPreset::toXmlString() const
{
    juce::XmlElement xml("GlorpMixPreset");
    xml.setAttribute("name", name);
    xml.setAttribute("version", "1.0");

    auto *mXml = xml.createNewChildElement("Master");
    mXml->setAttribute("level", static_cast<double>(master.level));
    mXml->setAttribute("pan", static_cast<double>(master.pan));
    mXml->setAttribute("limiter", master.limiterEnabled);
    mXml->setAttribute("reverbReturn", static_cast<double>(master.reverbReturn));
    mXml->setAttribute("reverbSize", static_cast<double>(master.reverbSize));
    mXml->setAttribute("reverbDamp", static_cast<double>(master.reverbDamp));
    mXml->setAttribute("reverbWidth", static_cast<double>(master.reverbWidth));
    mXml->setAttribute("busEqLowDb", static_cast<double>(master.busEqLowDb));
    mXml->setAttribute("busEqMidDb", static_cast<double>(master.busEqMidDb));
    mXml->setAttribute("busEqMidFreq", static_cast<double>(master.busEqMidFreq));
    mXml->setAttribute("busEqHighDb", static_cast<double>(master.busEqHighDb));
    mXml->setAttribute("busCompEnabled", master.busCompEnabled);
    mXml->setAttribute("busCompThresholdDb", static_cast<double>(master.busCompThresholdDb));
    mXml->setAttribute("busCompRatio", static_cast<double>(master.busCompRatio));
    mXml->setAttribute("busCompAttackMs", static_cast<double>(master.busCompAttackMs));
    mXml->setAttribute("busCompReleaseMs", static_cast<double>(master.busCompReleaseMs));
    mXml->setAttribute("busCompMakeupDb", static_cast<double>(master.busCompMakeupDb));
    mXml->setAttribute("busCompMix", static_cast<double>(master.busCompMix));
    mXml->setAttribute("busSaturationDrive", static_cast<double>(master.busSaturationDrive));

    for (size_t i = 0; i < micStrips.size(); ++i)
    {
        const auto &s = micStrips[i];
        auto *chXml = xml.createNewChildElement("Channel");
        chXml->setAttribute("index", static_cast<int>(i));
        if (i < micNames.size())
            chXml->setAttribute("name", micNames[i]);

        chXml->setAttribute("level", static_cast<double>(s.level));
        chXml->setAttribute("pan", static_cast<double>(s.pan));
        chXml->setAttribute("mono", s.mono);
        chXml->setAttribute("mute", s.mute);
        chXml->setAttribute("solo", s.solo);
        chXml->setAttribute("phaseInvert", s.phaseInvert);

        chXml->setAttribute("hpfHz", static_cast<double>(s.hpfHz));
        chXml->setAttribute("lpfHz", static_cast<double>(s.lpfHz));

        chXml->setAttribute("eqEnabled", s.eqEnabled);
        chXml->setAttribute("eqLowGainDb", static_cast<double>(s.eqLowGainDb));
        chXml->setAttribute("eqMidGainDb", static_cast<double>(s.eqMidGainDb));
        chXml->setAttribute("eqMidFreq", static_cast<double>(s.eqMidFreq));
        chXml->setAttribute("eqMidQ", static_cast<double>(s.eqMidQ));
        chXml->setAttribute("eqHighGainDb", static_cast<double>(s.eqHighGainDb));

        chXml->setAttribute("compEnabled", s.compEnabled);
        chXml->setAttribute("compThresholdDb", static_cast<double>(s.compThresholdDb));
        chXml->setAttribute("compRatio", static_cast<double>(s.compRatio));
        chXml->setAttribute("compAttackMs", static_cast<double>(s.compAttackMs));
        chXml->setAttribute("compReleaseMs", static_cast<double>(s.compReleaseMs));
        chXml->setAttribute("compMakeupDb", static_cast<double>(s.compMakeupDb));

        chXml->setAttribute("saturationDrive", static_cast<double>(s.saturationDrive));
        chXml->setAttribute("transientAttack", static_cast<double>(s.transientAttack));
        chXml->setAttribute("transientSustain", static_cast<double>(s.transientSustain));
        chXml->setAttribute("reverbSend", static_cast<double>(s.reverbSend));
    }

    return xml.toString();
}

bool MixerPreset::fromXmlString(const juce::String &xmlText, MixerPreset &outPreset)
{
    const auto xml = juce::parseXML(xmlText);
    if (xml == nullptr
        || (!xml->hasTagName("GlorpMixPreset") && !xml->hasTagName("MixerState")))
        return false;

    outPreset.name = xml->getStringAttribute("name", "User Preset");

    if (auto *mXml = xml->getChildByName("Master"))
    {
        outPreset.master.level = static_cast<float>(mXml->getDoubleAttribute("level", 0.85));
        outPreset.master.pan = static_cast<float>(mXml->getDoubleAttribute("pan", 0.0));
        outPreset.master.limiterEnabled = mXml->getBoolAttribute("limiter", true);
        outPreset.master.reverbReturn = static_cast<float>(mXml->getDoubleAttribute("reverbReturn", 0.25));
        outPreset.master.reverbSize = static_cast<float>(mXml->getDoubleAttribute("reverbSize", 0.45));
        outPreset.master.reverbDamp = static_cast<float>(mXml->getDoubleAttribute("reverbDamp", 0.40));
        outPreset.master.reverbWidth = static_cast<float>(mXml->getDoubleAttribute("reverbWidth", 0.85));
        outPreset.master.busEqLowDb = static_cast<float>(mXml->getDoubleAttribute("busEqLowDb", 0.0));
        outPreset.master.busEqMidDb = static_cast<float>(mXml->getDoubleAttribute("busEqMidDb", 0.0));
        outPreset.master.busEqMidFreq = static_cast<float>(mXml->getDoubleAttribute("busEqMidFreq", 800.0));
        outPreset.master.busEqHighDb = static_cast<float>(mXml->getDoubleAttribute("busEqHighDb", 0.0));
        outPreset.master.busCompEnabled = mXml->getBoolAttribute("busCompEnabled", false);
        outPreset.master.busCompThresholdDb =
            static_cast<float>(mXml->getDoubleAttribute("busCompThresholdDb", -12.0));
        outPreset.master.busCompRatio = static_cast<float>(mXml->getDoubleAttribute("busCompRatio", 2.0));
        outPreset.master.busCompAttackMs = static_cast<float>(mXml->getDoubleAttribute("busCompAttackMs", 30.0));
        outPreset.master.busCompReleaseMs = static_cast<float>(mXml->getDoubleAttribute("busCompReleaseMs", 150.0));
        outPreset.master.busCompMakeupDb = static_cast<float>(mXml->getDoubleAttribute("busCompMakeupDb", 0.0));
        outPreset.master.busCompMix = static_cast<float>(mXml->getDoubleAttribute("busCompMix", 1.0));
        outPreset.master.busSaturationDrive =
            static_cast<float>(mXml->getDoubleAttribute("busSaturationDrive", 0.0));
    }

    outPreset.micStrips.clear();
    outPreset.micNames.clear();

    for (auto *chXml : xml->getChildWithTagNameIterator("Channel"))
    {
        MicStripParams s;
        s.level = static_cast<float>(chXml->getDoubleAttribute("level", 1.0));
        s.pan = static_cast<float>(chXml->getDoubleAttribute("pan", 0.0));
        s.mono = chXml->getBoolAttribute("mono", false);
        s.mute = chXml->getBoolAttribute("mute", false);
        s.solo = chXml->getBoolAttribute("solo", false);
        s.phaseInvert = chXml->getBoolAttribute("phaseInvert", false);

        s.hpfHz = static_cast<float>(chXml->getDoubleAttribute("hpfHz", 20.0));
        s.lpfHz = static_cast<float>(chXml->getDoubleAttribute("lpfHz", 20000.0));

        s.eqEnabled = chXml->getBoolAttribute("eqEnabled", true);
        s.eqLowGainDb = static_cast<float>(chXml->getDoubleAttribute("eqLowGainDb", 0.0));
        s.eqMidGainDb = static_cast<float>(chXml->getDoubleAttribute("eqMidGainDb", 0.0));
        s.eqMidFreq = static_cast<float>(chXml->getDoubleAttribute("eqMidFreq", 1000.0));
        s.eqMidQ = static_cast<float>(chXml->getDoubleAttribute("eqMidQ", 1.0));
        s.eqHighGainDb = static_cast<float>(chXml->getDoubleAttribute("eqHighGainDb", 0.0));

        s.compEnabled = chXml->getBoolAttribute("compEnabled", false);
        s.compThresholdDb = static_cast<float>(chXml->getDoubleAttribute("compThresholdDb", -14.0));
        s.compRatio = static_cast<float>(chXml->getDoubleAttribute("compRatio", 3.0));
        s.compAttackMs = static_cast<float>(chXml->getDoubleAttribute("compAttackMs", 15.0));
        s.compReleaseMs = static_cast<float>(chXml->getDoubleAttribute("compReleaseMs", 100.0));
        s.compMakeupDb = static_cast<float>(chXml->getDoubleAttribute("compMakeupDb", 0.0));

        s.saturationDrive = static_cast<float>(chXml->getDoubleAttribute("saturationDrive", 0.0));
        s.transientAttack = static_cast<float>(chXml->getDoubleAttribute("transientAttack", 0.0));
        s.transientSustain = static_cast<float>(chXml->getDoubleAttribute("transientSustain", 0.0));
        s.reverbSend = static_cast<float>(chXml->getDoubleAttribute("reverbSend", 0.0));

        outPreset.micStrips.push_back(s);
        outPreset.micNames.push_back(chXml->getStringAttribute("name"));
    }

    return true;
}

} // namespace glorp
