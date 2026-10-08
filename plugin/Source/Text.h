#pragma once

#include <juce_core/juce_core.h>

/** UI text is Korean; juce::String (const char*) assumes ASCII, so UTF-8 literals go through here. */
inline juce::String ko (const char* utf8)
{
    return juce::String::fromUTF8 (utf8);
}
