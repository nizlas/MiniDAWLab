#include <juce_audio_processors/juce_audio_processors.h>

// Deterministic VST3 effect for the insert-preset tests. The processor state is a small
// blob with the gain parameter AND a side string that is not itself a parameter, so a
// preset round trip can show that the whole component state came back.

namespace
{
constexpr const char* kMagic = "DALPRB1";

class PresetProbeProcessor final : public juce::AudioProcessor
{
public:
    PresetProbeProcessor()
        : juce::AudioProcessor(BusesProperties()
                                   .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        addParameter(gain_ = new juce::AudioParameterFloat({"gain", 1}, "Gain", 0.0f, 1.0f, 1.0f));
    }

    const juce::String getName() const override { return "PresetProbe"; }
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
               && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const float g = gain_ != nullptr ? gain_->get() : 1.0f;
        side_ = "side-state:" + juce::String(g, 3);
        buffer.applyGain(g);
    }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& dest) override
    {
        juce::MemoryOutputStream stream;
        stream.write(kMagic, 7);
        stream.writeFloat(gain_ != nullptr ? gain_->get() : 1.0f);
        stream.writeString(side_);
        dest.replaceAll(stream.getData(), stream.getDataSize());
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        if (data == nullptr || sizeInBytes < 7)
        {
            return;
        }
        juce::MemoryInputStream stream(data, (size_t)sizeInBytes, false);
        char magic[7] {};
        if (stream.read(magic, 7) != 7 || std::memcmp(magic, kMagic, 7) != 0)
        {
            return;
        }
        const float g = stream.readFloat();
        side_ = stream.readString();
        if (gain_ != nullptr)
        {
            gain_->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, g));
        }
    }

private:
    juce::AudioParameterFloat* gain_ = nullptr;
    juce::String side_ { "unset" };
};
} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PresetProbeProcessor();
}
