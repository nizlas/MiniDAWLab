#include "domain/Session.h"

#include "io/AudioFileLoader.h"

Session::Session()
    : clip_(std::shared_ptr<const AudioClip>{})
{
}

Session::~Session() = default;

juce::Result Session::replaceClipFromFile(const juce::File& file, double deviceSampleRate)
{
    std::unique_ptr<AudioClip> loaded;
    const juce::Result loadResult = AudioFileLoader::loadFromFile(file, deviceSampleRate, loaded);

    if (!loadResult.wasOk())
        return loadResult;

    std::shared_ptr<const AudioClip> shared(std::move(loaded));
    std::atomic_store_explicit(&clip_, shared, std::memory_order_release);
    return juce::Result::ok();
}

void Session::clearClip() noexcept
{
    std::atomic_store_explicit(&clip_, {}, std::memory_order_release);
}

const AudioClip* Session::getCurrentClip() const noexcept
{
    return loadClipForAudioThread().get();
}

std::shared_ptr<const AudioClip> Session::loadClipForAudioThread() const noexcept
{
    return std::atomic_load_explicit(&clip_, std::memory_order_acquire);
}
