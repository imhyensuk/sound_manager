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
   #if JUCE_MAC
    // ~/Library/Application Support/SoundManagerAI (next to the settings file)
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Application Support").getChildFile ("SoundManagerAI");
   #else
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SoundManagerAI");
   #endif
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

    if (auto xml = juce::parseXML (knownPluginsFile()))
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
        xml->writeTo (knownPluginsFile());
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

juce::String PluginLibrary::getSetting (const juce::String& key, const juce::String& fallback) const
{
    return settings->getValue (key, fallback);
}

void PluginLibrary::setSetting (const juce::String& key, const juce::String& value)
{
    settings->setValue (key, value);
    settings->saveIfNeeded();
}

juce::File PluginLibrary::modelsDirectory()
{
    auto dir = dataDirectory().getChildFile ("models");
    dir.createDirectory();
    return dir;
}
