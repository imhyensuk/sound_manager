#include "JuceHttpTransport.h"

smix::HttpResponse JuceHttpTransport::post (const std::string& url,
                                            const std::vector<std::pair<std::string, std::string>>& headers,
                                            const std::string& body)
{
    juce::String headerText;
    for (auto& [k, v] : headers)
        headerText << juce::String (k) << ": " << juce::String (v) << "\r\n";

    const auto target = juce::URL (juce::String (url)).withPOSTData (juce::MemoryBlock (body.data(), body.size()));
    juce::WebInputStream stream (target, true);
    stream.withExtraHeaders (headerText).withConnectionTimeout (120000).withNumRedirectsToFollow (2);

    smix::HttpResponse r;
    stream.connect (nullptr);
    r.status = stream.getStatusCode();

    // A 4xx/5xx still carries a JSON error body worth reading; only a missing status is a transport failure.
    if (r.status == 0)
    {
        r.status = 0;
        r.error = "could not connect to " + url;
        return r;
    }

    r.body = stream.readEntireStreamAsString().toStdString();
    return r;
}
