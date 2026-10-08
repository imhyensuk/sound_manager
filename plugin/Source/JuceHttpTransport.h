#pragma once

#include <juce_core/juce_core.h>
#include <smix/ClaudeMixAgent.h>

/** HTTPS POST through JUCE's WebInputStream (WinINet / NSURLSession / libcurl). */
class JuceHttpTransport : public smix::HttpTransport
{
public:
    smix::HttpResponse post (const std::string& url,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             const std::string& body) override;
};
