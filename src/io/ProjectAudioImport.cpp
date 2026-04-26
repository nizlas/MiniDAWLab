#include "io/ProjectAudioImport.h"

juce::File mini_daw::getProjectAudioDir(const juce::File& projectFolder)
{
    return projectFolder.getChildFile("Audio");
}

juce::Result
    mini_daw::importAudioIntoProjectAudioDir(const juce::File& source, const juce::File& audioDir, juce::File& outPathToUse)
{
    if (!source.existsAsFile())
    {
        return juce::Result::fail("The selected file is missing or is not a file: " + source.getFullPathName());
    }
    if (!audioDir.isDirectory() && !audioDir.createDirectory())
    {
        return juce::Result::fail("Could not create the project Audio folder: " + audioDir.getFullPathName());
    }
    if (source.getParentDirectory() == audioDir)
    {
        outPathToUse = source;
        return juce::Result::ok();
    }
    juce::String stem = juce::File::createLegalFileName(source.getFileNameWithoutExtension());
    if (stem.isEmpty())
    {
        stem = "clip";
    }
    const juce::String ext = source.getFileExtension();
    juce::File dest;
    bool found = false;
    for (int i = 0; i < 10000; ++i)
    {
        const juce::String name
            = (i == 0) ? (stem + ext) : (stem + "_" + juce::String(i) + ext);
        juce::File candidate = audioDir.getChildFile(name);
        if (!candidate.exists())
        {
            dest = candidate;
            found = true;
            break;
        }
    }
    if (!found)
    {
        return juce::Result::fail("Could not find a free filename in the project Audio folder.");
    }
    if (!source.copyFileTo(dest))
    {
        return juce::Result::fail("Could not write " + dest.getFullPathName());
    }
    if (!dest.existsAsFile())
    {
        return juce::Result::fail("Copy reported success but the file is missing: " + dest.getFullPathName());
    }
    outPathToUse = dest;
    return juce::Result::ok();
}
