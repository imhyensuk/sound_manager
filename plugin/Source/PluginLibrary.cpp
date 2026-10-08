#include "PluginLibrary.h"

#include <fstream>

namespace
{
const char* const kOwnName = "Sound Manager AI";
}

//==============================================================================
class PluginLibrary::ScanThread : public juce::Thread
{
public:
    explicit ScanThread (PluginLibrary& o) : juce::Thread ("Sound Manager plugin scan"), owner (o) {}

    void run() override
    {
        const auto deadMansPedal = PluginLibrary::dataDirectory().getChildFile ("scan-in-progress.txt");

        for (auto* format : owner.formatManager.getFormats())
        {
            juce::PluginDirectoryScanner scanner (owner.knownPlugins, *format, format->getDefaultLocationsToSearch(),
                                                  true, deadMansPedal, false);
            juce::String name;
            while (! threadShouldExit())
            {
                owner.setStatus ("Scanning " + format->getName() + ": " + scanner.getNextPluginFileThatWillBeScanned());
                if (! scanner.scanNextFile (true, name))
                    break;
            }
            for (auto& failed : scanner.getFailedFiles())
                owner.knownPlugins.addToBlacklist (failed);
        }

        owner.setStatus ("Scan finished");
        juce::MessageManager::callAsync ([safe = juce::WeakReference<PluginLibrary::ScanThread> (this), &o = owner] {
            if (safe != nullptr)
                o.syncCatalogFromKnownList();
        });
    }

private:
    PluginLibrary& owner;
    JUCE_DECLARE_WEAK_REFERENCEABLE (ScanThread)
};

//==============================================================================
juce::File PluginLibrary::dataDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SoundManagerAI");
    dir.createDirectory();
    return dir;
}

PluginLibrary::PluginLibrary()
{
    formatManager.addDefaultFormats();

    juce::PropertiesFile::Options options;
    options.applicationName = "SoundManagerAI";
    options.filenameSuffix = ".settings";
    options.folderName = "SoundManagerAI";
    options.osxLibrarySubFolder = "Application Support";
    settings = std::make_unique<juce::PropertiesFile> (options);

    if (auto xml = juce::parseXML (dataDirectory().getChildFile ("known-plugins.xml")))
        knownPlugins.recreateFromXml (*xml);

    std::ifstream in (dataDirectory().getChildFile ("catalog.json").getFullPathName().toStdString());
    if (in)
    {
        const auto j = nlohmann::json::parse (in, nullptr, false);
        if (! j.is_discarded())
            pluginCatalog = smix::PluginCatalog::fromJson (j);
    }
    syncCatalogFromKnownList();
}

PluginLibrary::~PluginLibrary()
{
    if (scanThread != nullptr)
        scanThread->stopThread (5000);
    saveCatalog();
}

void PluginLibrary::setStatus (const juce::String& s)
{
    const juce::ScopedLock sl (statusLock);
    scanStatus = s;
}

juce::String PluginLibrary::getScanStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return scanStatus;
}

void PluginLibrary::startScan()
{
    if (isScanning())
        return;
    scanThread = std::make_unique<ScanThread> (*this);
    scanThread->startThread();
}

bool PluginLibrary::isScanning() const
{
    return scanThread != nullptr && scanThread->isThreadRunning();
}

void PluginLibrary::syncCatalogFromKnownList()
{
    for (auto& d : knownPlugins.getTypes())
    {
        if (d.isInstrument || d.name.containsIgnoreCase (kOwnName))
            continue;  // instruments are not mix processors; never host ourselves

        smix::PluginInfo info;
        info.uid = d.createIdentifierString().toStdString();
        info.name = d.name.toStdString();
        info.manufacturer = d.manufacturerName.toStdString();
        info.format = d.pluginFormatName.toStdString();
        info.hostCategory = d.category.toStdString();
        pluginCatalog.addOrUpdate (info);
    }

    if (auto xml = knownPlugins.createXml())
        xml->writeTo (dataDirectory().getChildFile ("known-plugins.xml"));
    saveCatalog();
    sendChangeMessage();
}

void PluginLibrary::saveCatalog()
{
    std::ofstream out (dataDirectory().getChildFile ("catalog.json").getFullPathName().toStdString());
    out << pluginCatalog.toJson().dump (2);
}

std::vector<std::string> PluginLibrary::addPluginFile (const juce::String& path)
{
    std::vector<std::string> uids;
    for (auto* format : formatManager.getFormats())
    {
        if (! format->fileMightContainThisPluginType (path))
            continue;
        juce::OwnedArray<juce::PluginDescription> found;
        knownPlugins.scanAndAddFile (path, false, found, *format);
        for (auto* d : found)
            uids.push_back (d->createIdentifierString().toStdString());
    }
    syncCatalogFromKnownList();
    return uids;
}

std::optional<juce::PluginDescription> PluginLibrary::descriptionFor (const std::string& uid) const
{
    for (auto& d : knownPlugins.getTypes())
        if (d.createIdentifierString().toStdString() == uid)
            return d;
    return std::nullopt;
}

juce::String PluginLibrary::getApiKey() const
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("ANTHROPIC_API_KEY", {});
    return env.isNotEmpty() ? env : settings->getValue ("apiKey");
}

void PluginLibrary::setApiKey (const juce::String& key)   { settings->setValue ("apiKey", key); settings->saveIfNeeded(); }
juce::String PluginLibrary::getModel() const              { return settings->getValue ("model", "claude-opus-5-5"); }
void PluginLibrary::setModel (const juce::String& m)      { settings->setValue ("model", m); settings->saveIfNeeded(); }
juce::String PluginLibrary::getEffort() const             { return settings->getValue ("effort", "medium"); }
void PluginLibrary::setEffort (const juce::String& e)     { settings->setValue ("effort", e); settings->saveIfNeeded(); }
