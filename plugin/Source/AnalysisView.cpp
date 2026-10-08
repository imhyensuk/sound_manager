#include "AnalysisView.h"

#include "PluginProcessor.h"
#include "Text.h"

namespace
{
const juce::Colour kBg (0xff15171b), kGrid (0xff2c3038), kLine (0xff4fc3a1), kPeak (0xffe0b050), kText (0xff9aa0a6);

juce::Colour heat (float t)
{
    t = juce::jlimit (0.0f, 1.0f, t);
    // black -> blue -> teal -> yellow -> white
    if (t < 0.25f) return juce::Colour::fromFloatRGBA (0.0f, 0.0f, t * 2.4f, 1.0f);
    if (t < 0.5f)  return juce::Colour::fromFloatRGBA (0.0f, (t - 0.25f) * 3.2f, 0.6f, 1.0f);
    if (t < 0.75f) return juce::Colour::fromFloatRGBA ((t - 0.5f) * 4.0f, 0.8f, 0.6f - (t - 0.5f) * 2.0f, 1.0f);
    return juce::Colour::fromFloatRGBA (1.0f, 0.8f + (t - 0.75f) * 0.8f, (t - 0.75f) * 4.0f, 1.0f);
}

float xForBin (int bin, juce::Rectangle<float> r)
{
    return r.getX() + r.getWidth() * (static_cast<float> (bin) + 0.5f) / smix::AudioAnalyzer::kRtaBins;
}

float yForDb (float db, juce::Rectangle<float> r, float lo = -96.0f, float hi = 0.0f)
{
    return juce::jmap (juce::jlimit (lo, hi, db), lo, hi, r.getBottom(), r.getY());
}
} // namespace

juce::String AnalysisView::modeLabel (const juce::String& m)
{
    if (m == "rta")         return ko ("RTA 스펙트럼");
    if (m == "waterfall")   return ko ("Hz 워터폴");
    if (m == "meters")      return ko ("볼륨 미터");
    if (m == "loudness")    return ko ("라우드니스(LUFS)");
    if (m == "correlation") return ko ("위상 상관");
    return m;
}

AnalysisView::AnalysisView (SoundManagerProcessor& p) : processor (p)
{
    for (auto* f : { &current, &peakHold, &smoothed })
        f->db.fill (-120.0f);
}

AnalysisView::~AnalysisView()
{
    processor.getAnalyzer().setVisualsEnabled (false);
}

void AnalysisView::setMode (const juce::String& m)
{
    if (modes().contains (m))
        mode = m;
    repaint();
}

void AnalysisView::visibilityChanged()
{
    // The visualiser module is on only while a graph is on screen.
    const bool on = isShowing() && processor.getEngine().modules().isEnabled (smix::modules::ModuleId::Visualizer);
    processor.getAnalyzer().setVisualsEnabled (on);
    if (on)
        startTimerHz (30);
    else
        stopTimer();
}

void AnalysisView::resized()
{
    waterfall = juce::Image (juce::Image::RGB, smix::AudioAnalyzer::kRtaBins, juce::jmax (64, getHeight()), true);
    waterfall.clear (waterfall.getBounds(), juce::Colours::black);
}

void AnalysisView::pushWaterfallRow (const smix::AudioAnalyzer::RtaFrame& f)
{
    if (! waterfall.isValid())
        return;
    waterfall.moveImageSection (0, 1, 0, 0, waterfall.getWidth(), waterfall.getHeight() - 1);  // scroll down
    for (int b = 0; b < smix::AudioAnalyzer::kRtaBins; ++b)
        waterfall.setPixelAt (b, 0, heat ((f.db[static_cast<size_t> (b)] + 96.0f) / 90.0f));
}

void AnalysisView::timerCallback()
{
    auto& an = processor.getAnalyzer();
    processor.getEngine().modules().touch (smix::modules::ModuleId::Visualizer, Engine::now());
    if (! an.visualsAreEnabled())
        an.setVisualsEnabled (true);

    smix::AudioAnalyzer::RtaFrame f;
    while (an.popRta (f))
    {
        current = f;
        haveFrame = true;
        for (size_t b = 0; b < f.db.size(); ++b)
        {
            smoothed.db[b] = smoothed.db[b] < -110.0f ? f.db[b] : 0.7f * smoothed.db[b] + 0.3f * f.db[b];
            peakHold.db[b] = std::max (peakHold.db[b] - 0.15f, f.db[b]);
        }
        pushWaterfallRow (f);
    }
    smix::AudioAnalyzer::MeterFrame m;
    while (an.popMeter (m))
    {
        meter = m;
        peakHoldL = std::max (peakHoldL - 0.3f, m.peakL);
        peakHoldR = std::max (peakHoldR - 0.3f, m.peakR);
        lufsHistory.push_back (m.shortTermLufs);
        correlationHistory.push_back (m.correlation);
        while (lufsHistory.size() > 600) lufsHistory.pop_front();          // 60 s
        while (correlationHistory.size() > 600) correlationHistory.pop_front();
    }
    repaint();
}

void AnalysisView::drawFrequencyGrid (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setFont (11.0f);
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = r.getX() + r.getWidth() * std::log (hz / 20.0f) / std::log (1000.0f);
        g.setColour (kGrid);
        g.drawVerticalLine (static_cast<int> (x), r.getY(), r.getBottom());
        g.setColour (kText);
        g.drawText (hz >= 1000 ? juce::String (hz / 1000, 0) + "k" : juce::String (hz, 0), static_cast<int> (x) + 2,
                    static_cast<int> (r.getBottom()) - 14, 40, 12, juce::Justification::left);
    }
}

void AnalysisView::paintRta (juce::Graphics& g, juce::Rectangle<float> r)
{
    drawFrequencyGrid (g, r);
    for (float db : { -84.0f, -60.0f, -36.0f, -12.0f })
    {
        const float y = yForDb (db, r);
        g.setColour (kGrid);
        g.drawHorizontalLine (static_cast<int> (y), r.getX(), r.getRight());
        g.setColour (kText);
        g.drawText (juce::String (db, 0) + " dB", static_cast<int> (r.getX()) + 2, static_cast<int> (y) - 12, 60, 12, juce::Justification::left);
    }
    juce::Path line, peak;
    for (int b = 0; b < smix::AudioAnalyzer::kRtaBins; ++b)
    {
        const float x = xForBin (b, r);
        const float y = yForDb (smoothed.db[static_cast<size_t> (b)], r);
        const float py = yForDb (peakHold.db[static_cast<size_t> (b)], r);
        if (b == 0) { line.startNewSubPath (x, y); peak.startNewSubPath (x, py); }
        else        { line.lineTo (x, y); peak.lineTo (x, py); }
    }
    juce::Path fill (line);
    fill.lineTo (r.getRight(), r.getBottom());
    fill.lineTo (r.getX(), r.getBottom());
    fill.closeSubPath();
    g.setColour (kLine.withAlpha (0.18f));
    g.fillPath (fill);
    g.setColour (kLine);
    g.strokePath (line, juce::PathStrokeType (1.6f));
    g.setColour (kPeak.withAlpha (0.7f));
    g.strokePath (peak, juce::PathStrokeType (1.0f));
}

void AnalysisView::paintWaterfall (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (waterfall.isValid())
        g.drawImage (waterfall, r, juce::RectanglePlacement::stretchToFit);
    drawFrequencyGrid (g, r);
    g.setColour (kText);
    g.drawText (ko ("위: 지금 · 아래: 과거"), r.removeFromTop (16).toNearestInt(), juce::Justification::right);
}

void AnalysisView::paintMeters (juce::Graphics& g, juce::Rectangle<float> r)
{
    auto bars = r.removeFromLeft (r.getWidth() * 0.45f).reduced (20, 10);
    const float w = bars.getWidth() / 5.0f;
    auto drawBar = [&] (int i, float rms, float pk, float hold, const juce::String& label) {
        auto b = juce::Rectangle<float> (bars.getX() + i * w * 1.5f, bars.getY(), w, bars.getHeight() - 16);
        g.setColour (kGrid);
        g.fillRect (b);
        const float yr = juce::jmap (juce::jlimit (-60.0f, 0.0f, rms), -60.0f, 0.0f, b.getBottom(), b.getY());
        const float yp = juce::jmap (juce::jlimit (-60.0f, 0.0f, pk), -60.0f, 0.0f, b.getBottom(), b.getY());
        const float yh = juce::jmap (juce::jlimit (-60.0f, 0.0f, hold), -60.0f, 0.0f, b.getBottom(), b.getY());
        g.setColour (kLine.withAlpha (0.45f));
        g.fillRect (b.withTop (yp));
        g.setColour (pk > -1.0f ? juce::Colours::red : kLine);
        g.fillRect (b.withTop (yr));
        g.setColour (kPeak);
        g.drawHorizontalLine (static_cast<int> (yh), b.getX(), b.getRight());
        g.setColour (kText);
        g.drawText (label, b.withY (b.getBottom() + 2).withHeight (14).toNearestInt(), juce::Justification::centred);
    };
    drawBar (0, meter.rmsL, meter.peakL, peakHoldL, "L");
    drawBar (1, meter.rmsR, meter.peakR, peakHoldR, "R");

    auto text = r.reduced (10);
    g.setFont (22.0f);
    g.setColour (kText);
    auto line = [&] (const juce::String& label, float value, const juce::String& unit) {
        auto row = text.removeFromTop (40);
        g.setFont (14.0f);
        g.drawText (label, row.removeFromLeft (150).toNearestInt(), juce::Justification::centredLeft);
        g.setFont (24.0f);
        g.setColour (juce::Colours::white);
        g.drawText (value <= -119.0f ? juce::String ("-inf") : juce::String (value, 1) + " " + unit, row.toNearestInt(), juce::Justification::centredLeft);
        g.setColour (kText);
    };
    line (ko ("피크 L / R"), juce::jmax (meter.peakL, meter.peakR), "dBFS");
    line (ko ("RMS"), 0.5f * (meter.rmsL + meter.rmsR), "dBFS");
    line (ko ("모멘터리"), meter.momentaryLufs, "LUFS");
    line (ko ("숏텀 (3초)"), meter.shortTermLufs, "LUFS");
    line (ko ("위상 상관"), meter.correlation, "");
}

void AnalysisView::paintHistory (juce::Graphics& g, juce::Rectangle<float> r, const std::deque<float>& values, float lo, float hi,
                                 const juce::String& unit)
{
    for (int i = 0; i <= 4; ++i)
    {
        const float v = lo + (hi - lo) * i / 4.0f;
        const float y = juce::jmap (v, lo, hi, r.getBottom(), r.getY());
        g.setColour (kGrid);
        g.drawHorizontalLine (static_cast<int> (y), r.getX(), r.getRight());
        g.setColour (kText);
        g.drawText (juce::String (v, 1) + " " + unit, static_cast<int> (r.getX()) + 2, static_cast<int> (y) - 12, 80, 12, juce::Justification::left);
    }
    if (values.size() < 2)
        return;
    juce::Path p;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const float x = r.getRight() - r.getWidth() * static_cast<float> (values.size() - 1 - i) / 600.0f;
        const float y = juce::jmap (juce::jlimit (lo, hi, values[i]), lo, hi, r.getBottom(), r.getY());
        if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
    }
    g.setColour (kLine);
    g.strokePath (p, juce::PathStrokeType (1.6f));
    g.setColour (kText);
    g.drawText (ko ("최근 60초"), r.removeFromBottom (14).toNearestInt(), juce::Justification::right);
}

void AnalysisView::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    auto r = getLocalBounds().toFloat().reduced (8);
    if (! processor.getEngine().modules().isEnabled (smix::modules::ModuleId::Visualizer))
    {
        g.setColour (kText);
        g.drawText (ko ("분석 그래프 모듈이 꺼져 있어요 (설정 탭)"), r.toNearestInt(), juce::Justification::centred);
        return;
    }
    if (! haveFrame && mode != "meters")
    {
        g.setColour (kText);
        g.drawText (ko ("재생하면 그래프가 나타나요"), r.toNearestInt(), juce::Justification::centred);
    }
    if (mode == "rta")              paintRta (g, r);
    else if (mode == "waterfall")   paintWaterfall (g, r);
    else if (mode == "meters")      paintMeters (g, r);
    else if (mode == "loudness")    paintHistory (g, r, lufsHistory, -48.0f, 0.0f, "LUFS");
    else if (mode == "correlation") paintHistory (g, r, correlationHistory, -1.0f, 1.0f, "");
}
