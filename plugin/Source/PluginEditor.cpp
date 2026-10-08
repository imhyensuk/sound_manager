#include "PluginEditor.h"

#include "PluginProcessor.h"
#include "Text.h"

#include <smix/PerceptualProfile.h>

namespace
{
const juce::Colour kBackground (0xff1b1d22);
const juce::Colour kPanel (0xff24272e);
const juce::Colour kAccent (0xff4fc3a1);
const juce::Colour kText (0xffe6e6e6);
const juce::Colour kDim (0xff9aa0a6);

void styleList (juce::ListBox& l)
{
    l.setColour (juce::ListBox::backgroundColourId, kPanel);
    l.setRowHeight (24);
}

void styleLog (juce::TextEditor& t)
{
    t.setMultiLine (true);
    t.setReadOnly (true);
    t.setScrollbarsShown (true);
    t.setCaretVisible (false);
    t.setColour (juce::TextEditor::backgroundColourId, kPanel);
    t.setColour (juce::TextEditor::textColourId, kText);
    t.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
}

/** Simple ListBoxModel driven by a lambda that paints a row. */
struct LambdaListModel : juce::ListBoxModel
{
    std::function<int()> count;
    std::function<void (int, juce::Graphics&, int, int, bool)> paint;
    std::function<void (int, const juce::MouseEvent&)> clicked;

    int getNumRows() override { return count ? count() : 0; }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (selected)
            g.fillAll (kAccent.withAlpha (0.25f));
        if (paint)
            paint (row, g, w, h, selected);
    }
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (clicked)
            clicked (row, e);
    }
};

//==============================================================================
class MixTab : public juce::Component
{
public:
    explicit MixTab (SoundManagerProcessor& p) : processor (p)
    {
        auto& apvts = p.getParameters();

        kindBox.addItemList (SoundManagerProcessor::kindChoices(), 1);
        roleBox.addItemList (SoundManagerProcessor::roleChoices(), 1);
        kindAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, "kind", kindBox);
        roleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, "role", roleBox);
        autoAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, "autoMix", autoMix);
        lockAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, "gainLock", gainLock);
        gain.setSliderStyle (juce::Slider::LinearHorizontal);
        gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
        gainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, "aiGain", gain);

        autoMix.setButtonText (ko ("AI 자동 믹싱"));
        gainLock.setButtonText (ko ("레벨 잠금"));
        parentBox.onChange = [this] {
            const int idx = parentBox.getSelectedItemIndex();
            if (idx >= 0 && idx < parentIds.size())
                processor.setParentSetting (parentIds[idx]);
        };

        for (auto* l : { &kindLabel, &roleLabel, &parentLabel, &gainLabel, &scopeLabel })
        {
            l->setColour (juce::Label::textColourId, kDim);
            addAndMakeVisible (*l);
        }
        kindLabel.setText (ko ("채널 종류"), juce::dontSendNotification);
        roleLabel.setText (ko ("악기 역할"), juce::dontSendNotification);
        parentLabel.setText (ko ("출력 대상(버스)"), juce::dontSendNotification);
        gainLabel.setText (ko ("AI 게인"), juce::dontSendNotification);

        model.count = [this] { return static_cast<int> (rows.size()); };
        model.paint = [this] (int row, juce::Graphics& g, int w, int h, bool) {
            if (row >= static_cast<int> (rows.size()))
                return;
            const auto* c = processor.getHub().getSession().find (rows[static_cast<size_t> (row)]);
            if (c == nullptr)
                return;
            g.setColour (c->id == processor.getInstanceId() ? kAccent : kText);
            g.setFont (13.0f);
            const auto lufs = c->features.valid ? juce::String (c->features.shortTermLufs, 1) + " LUFS" : juce::String ("-");
            g.drawText (juce::String (c->name), 6, 0, 160, h, juce::Justification::centredLeft);
            g.setColour (kDim);
            g.drawText (juce::String (smix::toString (c->role)), 170, 0, 110, h, juce::Justification::centredLeft);
            g.drawText (lufs, 280, 0, 90, h, juce::Justification::centredLeft);
            g.drawText (juce::String (c->aiGainDb, 1) + " dB", 370, 0, 70, h, juce::Justification::centredLeft);
            const auto profile = smix::PerceptualProfile::analyse (c->features, c->role);
            g.drawText (c->features.valid ? juce::String (profile.summary()) : ko ("재생 중 분석 대기"), 445, 0, w - 450, h,
                        juce::Justification::centredLeft);
        };
        channels.setModel (&model);
        styleList (channels);
        styleLog (activity);

        for (auto* c : std::initializer_list<juce::Component*> { &kindBox, &roleBox, &parentBox, &autoMix, &gainLock, &gain, &channels, &activity })
            addAndMakeVisible (c);
    }

    void refresh()
    {
        // Routing menu
        const auto parents = processor.getHub().possibleParents (processor);
        juce::StringArray names { ko ("자동 (이름으로 추정)"), ko ("마스터로 직접") };
        juce::StringArray ids { "auto", "master" };
        for (auto* p : parents)
        {
            names.add (p->getDisplayName());
            ids.add (juce::String (p->getInstanceId()));
        }
        if (names != parentNames)
        {
            parentNames = names;
            parentIds = ids;
            parentBox.clear (juce::dontSendNotification);
            parentBox.addItemList (names, 1);
        }
        const int current = parentIds.indexOf (processor.getParentSetting());
        parentBox.setSelectedItemIndex (current >= 0 ? current : 0, juce::dontSendNotification);

        // Channels in scope
        rows = processor.getHub().getSession().scopeOf (processor.getInstanceId());
        const auto kind = processor.getEffectiveKind();
        scopeLabel.setText (ko ("믹싱 범위: ") + juce::String (smix::toString (kind)) + " - " + juce::String (static_cast<int> (rows.size()))
                                + ko ("개 채널  (각 트랙에도 이 플러그인을 넣으면 버스/마스터 인스턴스가 함께 믹싱합니다)"),
                            juce::dontSendNotification);
        channels.updateContent();
        channels.repaint();

        // Activity
        juce::String text;
        for (auto& e : processor.getLog())
            if (e.who == "auto" || e.who == "system")
                text << "[" << e.who << "] " << e.text << "\n";
        if (text != lastActivity)
        {
            lastActivity = text;
            activity.setText (text, false);
            activity.moveCaretToEnd();
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto row = r.removeFromTop (24);
        kindLabel.setBounds (row.removeFromLeft (70));
        kindBox.setBounds (row.removeFromLeft (130));
        row.removeFromLeft (10);
        roleLabel.setBounds (row.removeFromLeft (70));
        roleBox.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (10);
        parentLabel.setBounds (row.removeFromLeft (100));
        parentBox.setBounds (row.removeFromLeft (170));

        r.removeFromTop (8);
        row = r.removeFromTop (24);
        autoMix.setBounds (row.removeFromLeft (130));
        gainLock.setBounds (row.removeFromLeft (100));
        gainLabel.setBounds (row.removeFromLeft (60));
        gain.setBounds (row.removeFromLeft (300));

        r.removeFromTop (8);
        scopeLabel.setBounds (r.removeFromTop (20));
        activity.setBounds (r.removeFromBottom (110));
        r.removeFromBottom (6);
        channels.setBounds (r);
    }

private:
    SoundManagerProcessor& processor;
    juce::ComboBox kindBox, roleBox, parentBox;
    juce::ToggleButton autoMix, gainLock;
    juce::Slider gain;
    juce::Label kindLabel, roleLabel, parentLabel, gainLabel, scopeLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> kindAttachment, roleAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoAttachment, lockAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment;
    juce::StringArray parentNames, parentIds;
    LambdaListModel model;
    juce::ListBox channels;
    std::vector<std::string> rows;
    juce::TextEditor activity;
    juce::String lastActivity;
};

//==============================================================================
class ChainTab : public juce::Component
{
public:
    explicit ChainTab (SoundManagerProcessor& p) : processor (p)
    {
        model.count = [this] { return processor.getChain().size(); };
        model.paint = [this] (int row, juce::Graphics& g, int, int h, bool) {
            auto* slot = processor.getChain().slot (row);
            if (slot == nullptr)
                return;
            const auto* info = processor.getLibrary().catalog().find (slot->uid);
            g.setColour (slot->bypassed ? kDim : kText);
            g.setFont (14.0f);
            g.drawText (juce::String (row + 1) + ".  " + slot->instance->getName()
                            + (info != nullptr ? "   [" + juce::String (smix::toString (info->category)) + "]" : juce::String())
                            + (slot->bypassed ? ko ("   (바이패스)") : juce::String()),
                        8, 0, 600, h, juce::Justification::centredLeft);
        };
        model.clicked = [this] (int row, const juce::MouseEvent& e) {
            if (e.getNumberOfClicks() > 1)
                processor.showHostedEditor (row);
        };
        list.setModel (&model);
        styleList (list);

        plan.setButtonText (ko ("AI 체인 계획 적용"));
        plan.onClick = [this] {
            const auto planResult = processor.getHub().planChainFor (processor.getInstanceId());
            for (auto& n : planResult.notes)
                processor.addLog ("system", juce::String (n));
            if (planResult.slots.empty())
                return;
            smix::MixAction a;
            a.type = smix::ActionType::SetChain;
            a.channelId = processor.getInstanceId();
            a.plugins = planResult.pluginUids();
            for (auto& o : processor.getHub().apply ({ a }, processor.getInstanceId()))
                processor.addLog ("system", juce::String (o.message));
            juce::String why;
            for (auto& s : planResult.slots)
                why << juce::String (s.pluginName) << ": " << juce::String (s.purpose) << "\n";
            processor.addLog ("system", why.trimEnd());
        };

        up.setButtonText (ko ("위로"));
        down.setButtonText (ko ("아래로"));
        bypass.setButtonText (ko ("바이패스"));
        remove.setButtonText (ko ("제거"));
        open.setButtonText (ko ("에디터 열기"));
        add.setButtonText (ko ("추가"));

        up.onClick = [this] { move (-1); };
        down.onClick = [this] { move (1); };
        bypass.onClick = [this] {
            const int row = list.getSelectedRow();
            if (auto* slot = processor.getChain().slot (row))
                processor.setSlotBypass (row, ! slot->bypassed.load());
            list.repaint();
        };
        remove.onClick = [this] {
            const int row = list.getSelectedRow();
            auto uids = currentUids();
            if (juce::isPositiveAndBelow (row, static_cast<int> (uids.size())))
            {
                uids.erase (uids.begin() + row);
                processor.loadChain (uids);
            }
        };
        open.onClick = [this] { processor.showHostedEditor (list.getSelectedRow()); };
        add.onClick = [this] {
            const int idx = addBox.getSelectedItemIndex();
            if (! juce::isPositiveAndBelow (idx, static_cast<int> (addUids.size())))
                return;
            auto uids = currentUids();
            uids.push_back (addUids[static_cast<size_t> (idx)]);
            processor.loadChain (uids);
        };

        hint.setText (ko ("체크한 플러그인만 사용됩니다. 더블클릭하면 플러그인 화면이 열립니다. AI는 분석 결과에 따라 순서를 정하고 값을 조절합니다."),
                      juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, kDim);

        for (auto* c : std::initializer_list<juce::Component*> { &list, &plan, &up, &down, &bypass, &remove, &open, &addBox, &add, &hint })
            addAndMakeVisible (c);
    }

    void refresh()
    {
        list.updateContent();
        list.repaint();

        std::vector<std::string> uids;
        juce::StringArray names;
        for (auto& p : processor.getLibrary().catalog().allowedPlugins())
        {
            uids.push_back (p.uid);
            names.add (juce::String (p.name) + "  [" + juce::String (smix::toString (p.category)) + "]");
        }
        if (uids != addUids)
        {
            addUids = uids;
            addBox.clear (juce::dontSendNotification);
            addBox.addItemList (names, 1);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (28);
        plan.setBounds (top.removeFromLeft (160));
        top.removeFromLeft (10);
        for (auto* b : { &up, &down, &bypass, &remove, &open })
        {
            b->setBounds (top.removeFromLeft (b == &open ? 100 : 70));
            top.removeFromLeft (4);
        }
        auto bottom = r.removeFromBottom (28);
        add.setBounds (bottom.removeFromRight (70));
        bottom.removeFromRight (6);
        addBox.setBounds (bottom);
        r.removeFromBottom (6);
        hint.setBounds (r.removeFromBottom (36));
        r.removeFromTop (8);
        list.setBounds (r);
    }

private:
    std::vector<std::string> currentUids() const
    {
        std::vector<std::string> uids;
        for (int i = 0; i < processor.getChain().size(); ++i)
            uids.push_back (processor.getChain().slot (i)->uid);
        return uids;
    }

    void move (int delta)
    {
        const int row = list.getSelectedRow();
        if (processor.moveSlot (row, row + delta))
            list.selectRow (row + delta);
        list.repaint();
    }

    SoundManagerProcessor& processor;
    LambdaListModel model;
    juce::ListBox list;
    juce::TextButton plan, up, down, bypass, remove, open, add;
    juce::ComboBox addBox;
    juce::Label hint;
    std::vector<std::string> addUids;
};

//==============================================================================
class PluginsTab : public juce::Component
{
public:
    explicit PluginsTab (SoundManagerProcessor& p) : processor (p)
    {
        scan.setButtonText (ko ("플러그인 스캔"));
        scan.onClick = [this] { processor.getLibrary().startScan(); };
        filter.setTextToShowWhenEmpty (ko ("검색 (이름/제조사/종류)"), kDim);
        filter.onTextChange = [this] { rebuild(); };

        model.count = [this] { return static_cast<int> (visible.size()); };
        model.paint = [this] (int row, juce::Graphics& g, int w, int h, bool) {
            const auto* info = processor.getLibrary().catalog().find (visible[static_cast<size_t> (row)]);
            if (info == nullptr)
                return;
            g.setColour (info->allowed ? kAccent : kDim);
            g.drawRect (8, h / 2 - 7, 14, 14);
            if (info->allowed)
                g.fillRect (11, h / 2 - 4, 8, 8);
            g.setColour (kText);
            g.setFont (14.0f);
            g.drawText (juce::String (info->name), 32, 0, 260, h, juce::Justification::centredLeft);
            g.setColour (kDim);
            g.drawText (juce::String (info->manufacturer) + "  (" + juce::String (info->format) + ")", 300, 0, 240, h,
                        juce::Justification::centredLeft);
            g.drawText (juce::String (smix::toString (info->category)) + (info->categoryOverridden ? " *" : ""), 545, 0, w - 550, h,
                        juce::Justification::centredLeft);
        };
        model.clicked = [this] (int row, const juce::MouseEvent& e) {
            const auto uid = visible[static_cast<size_t> (row)];
            auto& catalog = processor.getLibrary().catalog();
            const auto* info = catalog.find (uid);
            if (info == nullptr)
                return;

            if (e.mods.isPopupMenu())
            {
                juce::PopupMenu menu;
                for (int c = 0; c <= static_cast<int> (smix::PluginCategory::PitchCorrection); ++c)
                    menu.addItem (c + 1, juce::String (smix::toString (static_cast<smix::PluginCategory> (c))), true,
                                  static_cast<int> (info->category) == c);
                menu.showMenuAsync (juce::PopupMenu::Options(), [this, uid] (int result) {
                    if (result <= 0)
                        return;
                    processor.getLibrary().catalog().setCategory (uid, static_cast<smix::PluginCategory> (result - 1));
                    processor.getLibrary().saveCatalog();
                    list.repaint();
                });
                return;
            }
            catalog.setAllowed (uid, ! info->allowed);
            processor.getLibrary().saveCatalog();
            list.repaint();
            updateCount();
        };
        list.setModel (&model);
        styleList (list);

        hint.setText (ko ("AI가 사용할 플러그인을 체크하세요 (클릭: 허용/해제, 우클릭: 종류 수정). 체크하지 않은 플러그인은 절대 사용하지 않습니다."),
                      juce::dontSendNotification);
        for (auto* l : { &hint, &status, &count })
        {
            l->setColour (juce::Label::textColourId, kDim);
            addAndMakeVisible (*l);
        }
        for (auto* c : std::initializer_list<juce::Component*> { &scan, &filter, &list })
            addAndMakeVisible (c);
        rebuild();
    }

    void refresh()
    {
        status.setText (processor.getLibrary().isScanning() ? processor.getLibrary().getScanStatus()
                                                            : juce::String (static_cast<int> (processor.getLibrary().catalog().all().size()))
                                                                  + ko ("개 이펙트 플러그인"),
                        juce::dontSendNotification);
        if (processor.getLibrary().catalog().all().size() != lastTotal)
            rebuild();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (28);
        scan.setBounds (top.removeFromLeft (120));
        top.removeFromLeft (10);
        filter.setBounds (top.removeFromLeft (260));
        top.removeFromLeft (10);
        count.setBounds (top.removeFromRight (140));
        status.setBounds (top);
        r.removeFromTop (6);
        hint.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        list.setBounds (r);
    }

private:
    void rebuild()
    {
        const auto& all = processor.getLibrary().catalog().all();
        lastTotal = all.size();
        const auto f = filter.getText().toLowerCase().toStdString();
        visible.clear();
        for (auto& p : all)
        {
            const auto hay = smix::toLowerAscii (p.name + " " + p.manufacturer + " " + smix::toString (p.category));
            if (f.empty() || hay.find (f) != std::string::npos)
                visible.push_back (p.uid);
        }
        list.updateContent();
        list.repaint();
        updateCount();
    }

    void updateCount()
    {
        count.setText (ko ("허용됨: ") + juce::String (static_cast<int> (processor.getLibrary().catalog().allowedPlugins().size())),
                       juce::dontSendNotification);
    }

    SoundManagerProcessor& processor;
    juce::TextButton scan;
    juce::TextEditor filter;
    juce::Label hint, status, count;
    LambdaListModel model;
    juce::ListBox list;
    std::vector<std::string> visible;
    size_t lastTotal = 0;
};

//==============================================================================
class ChatTab : public juce::Component
{
public:
    explicit ChatTab (SoundManagerProcessor& p) : processor (p)
    {
        styleLog (history);
        input.setTextToShowWhenEmpty (ko ("예: 드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어"), kDim);
        input.onReturnKey = [this] { send(); };
        sendButton.setButtonText (ko ("보내기"));
        sendButton.onClick = [this] { send(); };
        reset.setButtonText (ko ("새 대화"));
        reset.onClick = [this] { processor.getAgent().resetConversation(); processor.addLog ("system", ko ("새 대화를 시작합니다.")); };
        busy.setColour (juce::Label::textColourId, kAccent);

        for (auto* c : std::initializer_list<juce::Component*> { &history, &input, &sendButton, &reset, &busy })
            addAndMakeVisible (c);
    }

    void refresh()
    {
        juce::String text;
        for (auto& e : processor.getLog())
        {
            if (e.who == "you")
                text << ko ("나: ") << e.text << "\n\n";
            else if (e.who == "AI")
                text << "AI: " << e.text << "\n\n";
        }
        if (text != lastText)
        {
            lastText = text;
            history.setText (text, false);
            history.moveCaretToEnd();
        }
        busy.setText (processor.getAgent().isBusy() ? ko ("AI가 듣고 조정하는 중...") : juce::String(), juce::dontSendNotification);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto bottom = r.removeFromBottom (30);
        reset.setBounds (bottom.removeFromRight (80));
        bottom.removeFromRight (6);
        sendButton.setBounds (bottom.removeFromRight (80));
        bottom.removeFromRight (6);
        input.setBounds (bottom);
        r.removeFromBottom (4);
        busy.setBounds (r.removeFromBottom (20));
        history.setBounds (r);
    }

private:
    void send()
    {
        const auto text = input.getText().trim();
        if (text.isEmpty())
            return;
        input.clear();
        processor.addLog ("you", text);
        processor.getAgent().submit (text);
    }

    SoundManagerProcessor& processor;
    juce::TextEditor history, input;
    juce::TextButton sendButton, reset;
    juce::Label busy;
    juce::String lastText;
};

//==============================================================================
class SettingsTab : public juce::Component
{
public:
    explicit SettingsTab (SoundManagerProcessor& p) : processor (p)
    {
        auto& lib = processor.getLibrary();
        apiKey.setPasswordCharacter (static_cast<juce::juce_wchar> (0x2022));
        apiKey.setText (lib.getApiKey(), false);
        model.addItemList ({ "claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5" }, 1);
        model.setText (lib.getModel(), juce::dontSendNotification);
        model.setEditableText (true);
        effort.addItemList ({ "low", "medium", "high" }, 1);
        effort.setText (lib.getEffort(), juce::dontSendNotification);

        save.setButtonText (ko ("저장"));
        save.onClick = [this] {
            auto& l = processor.getLibrary();
            l.setApiKey (apiKey.getText().trim());
            l.setModel (model.getText().trim());
            l.setEffort (effort.getText().trim());
            processor.addLog ("system", ko ("설정을 저장했습니다."));
        };

        keyLabel.setText ("Anthropic API key", juce::dontSendNotification);
        modelLabel.setText ("Model", juce::dontSendNotification);
        effortLabel.setText ("Effort", juce::dontSendNotification);
        note.setText (ko ("키는 이 컴퓨터의 사용자 설정 폴더(SoundManagerAI)에 평문으로 저장됩니다. 환경 변수 ANTHROPIC_API_KEY가 있으면 그것을 우선 사용합니다.\n"
                          "키가 없으면 오프라인 규칙 기반 모드로 동작합니다. 채팅 시 채널 분석값과 플러그인 파라미터(오디오 자체는 아님)가 Anthropic API로 전송됩니다."),
                      juce::dontSendNotification);
        for (auto* l : { &keyLabel, &modelLabel, &effortLabel, &note })
        {
            l->setColour (juce::Label::textColourId, kDim);
            addAndMakeVisible (*l);
        }
        for (auto* c : std::initializer_list<juce::Component*> { &apiKey, &model, &effort, &save })
            addAndMakeVisible (c);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto row = [&r] { auto x = r.removeFromTop (26); r.removeFromTop (8); return x; };
        auto a = row(); keyLabel.setBounds (a.removeFromLeft (150)); apiKey.setBounds (a.removeFromLeft (420));
        auto b = row(); modelLabel.setBounds (b.removeFromLeft (150)); model.setBounds (b.removeFromLeft (220));
        auto c = row(); effortLabel.setBounds (c.removeFromLeft (150)); effort.setBounds (c.removeFromLeft (120));
        auto d = row(); save.setBounds (d.removeFromLeft (100));
        note.setBounds (r.removeFromTop (80));
    }

private:
    SoundManagerProcessor& processor;
    juce::TextEditor apiKey;
    juce::ComboBox model, effort;
    juce::TextButton save;
    juce::Label keyLabel, modelLabel, effortLabel, note;
};
} // namespace

//==============================================================================
SoundManagerEditor::SoundManagerEditor (SoundManagerProcessor& p) : AudioProcessorEditor (p), processor (p)
{
    getLookAndFeel().setColour (juce::ResizableWindow::backgroundColourId, kBackground);
    tabs.setTabBarDepth (32);
    tabs.addTab (ko ("믹스"), kBackground, new MixTab (p), true);
    tabs.addTab (ko ("체인"), kBackground, new ChainTab (p), true);
    tabs.addTab (ko ("플러그인"), kBackground, new PluginsTab (p), true);
    tabs.addTab (ko ("채팅"), kBackground, new ChatTab (p), true);
    tabs.addTab (ko ("설정"), kBackground, new SettingsTab (p), true);
    addAndMakeVisible (tabs);

    setResizable (true, true);
    setResizeLimits (760, 480, 1600, 1200);
    setSize (900, 600);
    startTimerHz (4);
}

SoundManagerEditor::~SoundManagerEditor()
{
    stopTimer();
}

void SoundManagerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
}

void SoundManagerEditor::resized()
{
    tabs.setBounds (getLocalBounds());
}

void SoundManagerEditor::timerCallback()
{
    auto* current = tabs.getCurrentContentComponent();
    if (auto* t = dynamic_cast<MixTab*> (current))          t->refresh();
    else if (auto* c = dynamic_cast<ChainTab*> (current))   c->refresh();
    else if (auto* pl = dynamic_cast<PluginsTab*> (current)) pl->refresh();
    else if (auto* ch = dynamic_cast<ChatTab*> (current))   ch->refresh();
}
