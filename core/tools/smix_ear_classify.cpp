// Classifies WAV files with an ear model (.smxear), as the plugin does for the naming question.
//   smix_ear_classify <model.smxear> <file.wav> [...]

#include <iostream>

#include <smix/WavFile.h>
#include <smix/ear/EarModel.h>

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: smix_ear_classify model.smxear file.wav ...\n";
        return 2;
    }
    smix::mem::Runtime runtime (64u << 20);
    smix::ear::EarModel model (&runtime);
    std::string error;
    if (! model.load (argv[1], error))
    {
        std::cerr << error << "\n";
        return 1;
    }
    for (int i = 2; i < argc; ++i)
    {
        smix::WavData wav;
        if (! smix::readWav (argv[i], wav, error))
        {
            std::cerr << argv[i] << ": " << error << "\n";
            continue;
        }
        std::vector<float> mono (wav.samples[0].size());
        for (size_t s = 0; s < mono.size(); ++s)
        {
            float v = 0;
            for (auto& ch : wav.samples) v += ch[s];
            mono[s] = v / static_cast<float> (wav.channels);
        }
        const size_t n = std::min (mono.size(), static_cast<size_t> (wav.sampleRate * 3.0));
        std::cout << argv[i] << ":";
        for (auto& g : model.classify (smix::ear::EarFeatures::extract (mono.data(), n, wav.sampleRate)))
            std::cout << "  " << g.label << " " << static_cast<int> (g.probability * 100) << "%";
        std::cout << "\n";
    }
    return 0;
}
