#include "PluginEditor.h"

#include "AnalysisView.h"
#include "AssistantWorker.h"
#include "PluginProcessor.h"
#include "Text.h"

#include <smix/MixAssistant.h>
#include <smix/PerceptualProfile.h>

namespace
{
const juce::Colour kBackground (0xff1b1d22);
const juce::Colour kPanel (0xff24272e);
const juce::Colour kAccent (0xff4fc3a1);
const juce::Colour kWarn (0xffe0b050);
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

void dimLabel (juce::Label& l, const juce::String& text = {})
{
    l.setColour (juce::Label::textColourId, kDim);
    if (text.isNotEmpty())
        l.setText (text, juce::dontSendNotification);
}

void setLogText (juce::TextEditor& t, juce::String& last, const juce::String& text)
{
    if (text == last)
        return;
    last = text;
    t.setText (text, false);
    t.moveCaretToEnd();
}

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

/** Base class: every tab refreshes itself 4x per second while visible. */
struct Tab : juce::Component
{
    virtual void refresh() = 0;
};

//==============================================================================
class MixTab : public Tab
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
        protectAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, "protect", protect);
        gain.setSliderStyle (juce::Slider::LinearHorizontal);
        gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
        gainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, "aiGain", gain);
        autoMix.setButtonText (ko ("AI 자동 믹싱"));
        gainLock.setButtonText (ko ("레벨 잠금"));
        protect.setButtonText (ko ("이 채널 보호 (AI 수정 금지)"));

        parentBox.onChange = [this] {
            const int idx = parentBox.getSelectedItemIndex();
            if (idx >= 0 && idx < parentIds.size())
                processor.setParentSetting (parentIds[idx]);
        };

        nameEditor.setTextToShowWhenEmpty (ko ("예: 킥, 리드 보컬, 일렉 기타"), kDim);
        nameEditor.onReturnKey = [this] { saveName(); };
        saveNameButton.setButtonText (ko ("이름 저장"));
        saveNameButton.onClick = [this] { saveName(); };

        dimLabel (kindLabel, ko ("채널 종류"));
        dimLabel (roleLabel, ko ("악기 역할"));
        dimLabel (parentLabel, ko ("출력 대상(버스)"));
        dimLabel (gainLabel, ko ("AI 게인"));
        dimLabel (scopeLabel);
        dimLabel (nameLabel);
        nameLabel.setColour (juce::Label::textColourId, kWarn);

        model.count = [this] { return static_cast<int> (rows.size()); };
        model.paint = [this] (int row, juce::Graphics& g, int w, int h, bool) {
            if (row >= static_cast<int> (rows.size()))
                return;
            const auto* c = processor.getHub().getSession().find (rows[static_cast<size_t> (row)]);
            if (c == nullptr)
                return;
            g.setColour (c->id == processor.getInstanceId() ? kAccent : kText);
            g.setFont (13.0f);
            g.drawText (juce::String (c->name) + (c->protectedChannel ? ko (" (보호)") : juce::String()), 6, 0, 170, h, juce::Justification::centredLeft);
            g.setColour (kDim);
            g.drawText (juce::String (smix::koreanName (c->role)), 180, 0, 90, h, juce::Justification::centredLeft);
            g.drawText (c->features.valid ? juce::String (c->features.shortTermLufs, 1) + " LUFS" : juce::String ("-"), 270, 0, 85, h,
                        juce::Justification::centredLeft);
            g.drawText (juce::String (c->aiGainDb, 1) + " dB", 355, 0, 60, h, juce::Justification::centredLeft);
            g.drawText (juce::String (c->style), 415, 0, 130, h, juce::Justification::centredLeft);
            const auto profile = smix::PerceptualProfile::analyse (c->features, c->role);
            g.drawText (c->features.valid ? juce::String (profile.summary()) : ko ("재생 중 분석 대기"), 550, 0, w - 555, h,
                        juce::Justification::centredLeft);
        };
        channels.setModel (&model);
        styleList (channels);
        styleLog (activity);

        for (auto* c : std::initializer_list<juce::Component*> { &kindLabel, &roleLabel, &parentLabel, &gainLabel, &scopeLabel, &kindBox, &roleBox,
                                                                 &parentBox, &autoMix, &gainLock, &protect, &gain, &channels, &activity,
                                                                 &nameLabel, &nameEditor, &saveNameButton })
            addAndMakeVisible (c);
    }

    void refresh() override
    {
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

        // Requirement 12: ask for a meaningful channel name.
        const bool needsName = smix::MixAssistant::isUninformativeName (processor.getDisplayName().toStdString())
                               && processor.getEffectiveRole() == smix::InstrumentRole::Unknown;
        nameLabel.setText (needsName ? ko ("이 채널에 어떤 소리가 들어 있나요? 이름을 알려 주면 AI가 가장 빨리 이해해요 →")
                                     : ko ("채널 이름: ") + processor.getDisplayName(),
                           juce::dontSendNotification);
        nameLabel.setColour (juce::Label::textColourId, needsName ? kWarn : kDim);

        rows = processor.getHub().getSession().scopeOf (processor.getInstanceId());
        scopeLabel.setText (ko ("믹싱 범위: ") + juce::String (smix::toString (processor.getEffectiveKind())) + " - "
                                + juce::String (static_cast<int> (rows.size())) + ko ("개 채널"),
                            juce::dontSendNotification);
        channels.updateContent();
        channels.repaint();

        juce::String text;
        for (auto& e : processor.getLog())
            if (e.who == "auto" || e.who == "system")
                text << "[" << e.who << "] " << e.text << "\n";
        setLogText (activity, lastActivity, text);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto row = r.removeFromTop (26);
        nameLabel.setBounds (row.removeFromLeft (row.getWidth() - 330));
        saveNameButton.setBounds (row.removeFromRight (90));
        nameEditor.setBounds (row.reduced (4, 0));
        r.removeFromTop (6);
        row = r.removeFromTop (24);
        kindLabel.setBounds (row.removeFromLeft (70));
        kindBox.setBounds (row.removeFromLeft (120));
        row.removeFromLeft (8);
        roleLabel.setBounds (row.removeFromLeft (70));
        roleBox.setBounds (row.removeFromLeft (140));
        row.removeFromLeft (8);
        parentLabel.setBounds (row.removeFromLeft (100));
        parentBox.setBounds (row.removeFromLeft (170));
        r.removeFromTop (6);
        row = r.removeFromTop (24);
        autoMix.setBounds (row.removeFromLeft (120));
        gainLock.setBounds (row.removeFromLeft (95));
        protect.setBounds (row.removeFromLeft (210));
        gainLabel.setBounds (row.removeFromLeft (55));
        gain.setBounds (row.removeFromLeft (260));
        r.removeFromTop (6);
        scopeLabel.setBounds (r.removeFromTop (20));
        activity.setBounds (r.removeFromBottom (110));
        r.removeFromBottom (6);
        channels.setBounds (r);
    }

private:
    void saveName()
    {
        const auto name = nameEditor.getText().trim();
        if (name.isEmpty())
            return;
        processor.setUserLabel (name);
        nameEditor.clear();
        processor.addLog ("system", ko ("채널 이름을 '") + name + ko ("'(으)로 저장했어요."));
    }

    SoundManagerProcessor& processor;
    juce::ComboBox kindBox, roleBox, parentBox;
    juce::ToggleButton autoMix, gainLock, protect;
    juce::Slider gain;
    juce::Label kindLabel, roleLabel, parentLabel, gainLabel, scopeLabel, nameLabel;
    juce::TextEditor nameEditor;
    juce::TextButton saveNameButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> kindAttachment, roleAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoAttachment, lockAttachment, protectAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment;
    juce::StringArray parentNames, parentIds;
    LambdaListModel model;
    juce::ListBox channels;
    std::vector<std::string> rows;
    juce::TextEditor activity;
    juce::String lastActivity;
};

//==============================================================================
class ChainTab : public Tab
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
            g.setColour (slot->bypassed || slot->hibernated ? kDim : kText);
            g.setFont (14.0f);
            juce::String text = juce::String (row + 1) + ".  " + juce::String::fromUTF8 (slot->name.c_str());
            if (info != nullptr) text << "   [" << juce::String (smix::toString (info->category)) << "]";
            if (slot->bypassed) text << ko ("   바이패스");
            if (slot->hibernated) text << ko ("   절전 중(메모리 해제됨)");
            if (slot->protectedSlot) text << ko ("   보호됨");
            g.drawText (text, 8, 0, 700, h, juce::Justification::centredLeft);
        };
        model.clicked = [this] (int row, const juce::MouseEvent& e) {
            if (e.getNumberOfClicks() > 1)
                processor.showHostedEditor (row);
        };
        list.setModel (&model);
        styleList (list);

        plan.setButtonText (ko ("AI 체인 계획 적용"));
        plan.onClick = [this] {
            const auto result = processor.getHub().planChainFor (processor.getInstanceId());
            for (auto& n : result.notes)
                processor.addLog ("system", juce::String (n));
            if (result.slots.empty())
                return;
            smix::MixAction a;
            a.type = smix::ActionType::SetChain;
            a.channelId = processor.getInstanceId();
            a.plugins = result.pluginUids();
            for (auto& o : processor.getHub().apply ({ a }, processor.getInstanceId(), "AI 체인 계획", "plan"))
                processor.addLog ("system", juce::String (o.ok ? o.message : (o.needsUser ? o.manualRequest : o.message)));
            juce::String why;
            for (auto& s : result.slots)
                why << juce::String (s.pluginName) << ": " << juce::String (s.purpose) << "\n";
            processor.addLog ("system", why.trimEnd());
        };

        up.setButtonText (ko ("위로"));
        down.setButtonText (ko ("아래로"));
        bypass.setButtonText (ko ("바이패스"));
        protect.setButtonText (ko ("보호"));
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
        protect.onClick = [this] {
            const int row = list.getSelectedRow();
            if (auto* slot = processor.getChain().slot (row))
                processor.setSlotProtected (row, ! slot->protectedSlot);
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
        dimLabel (hint, ko ("체크한 플러그인만 사용돼요. '보호'한 플러그인은 AI가 바꾸지 않고, 바꿔야 할 때는 직접 바꿔 달라고 요청해요. "
                            "30초 이상 바이패스된 플러그인은 절전(메모리 해제)됐다가 다시 켜면 그대로 돌아와요."));
        for (auto* c : std::initializer_list<juce::Component*> { &list, &plan, &up, &down, &bypass, &protect, &remove, &open, &addBox, &add, &hint })
            addAndMakeVisible (c);
    }

    void refresh() override
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
        plan.setBounds (top.removeFromLeft (150));
        top.removeFromLeft (8);
        for (auto* b : { &up, &down, &bypass, &protect, &remove, &open })
        {
            b->setBounds (top.removeFromLeft (b == &open ? 95 : 64));
            top.removeFromLeft (4);
        }
        auto bottom = r.removeFromBottom (28);
        add.setBounds (bottom.removeFromRight (70));
        bottom.removeFromRight (6);
        addBox.setBounds (bottom);
        r.removeFromBottom (6);
        hint.setBounds (r.removeFromBottom (40));
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
    juce::TextButton plan, up, down, bypass, protect, remove, open, add;
    juce::ComboBox addBox;
    juce::Label hint;
    std::vector<std::string> addUids;
};

//==============================================================================
/** Questions the assistant is waiting on, with one button per option, skip and a countdown. */
class QuestionPanel : public juce::Component
{
public:
    explicit QuestionPanel (SoundManagerProcessor& p) : processor (p) {}

    void update()
    {
        const auto questions = processor.getAssistant().pendingQuestions();
        juce::String signature;
        for (auto& q : questions)
            signature << q.id << ",";
        if (signature != lastSignature)
        {
            lastSignature = signature;
            rebuild (questions);
        }
        for (size_t i = 0; i < questions.size() && i < timers.size(); ++i)
            timers[i]->setText (questions[i].remainingSeconds >= 0
                                    ? juce::String (static_cast<int> (questions[i].remainingSeconds)) + ko ("초 후 기본값")
                                    : juce::String(),
                                juce::dontSendNotification);
    }

    int preferredHeight() const { return juce::jmin (260, static_cast<int> (rows.size()) * 64); }

    void resized() override
    {
        auto r = getLocalBounds();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto row = r.removeFromTop (64).reduced (2);
            auto& q = *rows[i];
            q.text->setBounds (row.removeFromTop (30));
            auto buttons = row;
            timers[i]->setBounds (buttons.removeFromRight (90));
            q.skip->setBounds (buttons.removeFromRight (70));
            for (auto& b : q.options)
                b->setBounds (buttons.removeFromLeft (juce::jmin (160, juce::jmax (60, b->getBestWidthForHeight (26) + 10))).reduced (2, 0));
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (! rows.empty())
            g.fillAll (kWarn.withAlpha (0.08f));
    }

private:
    struct Row
    {
        std::unique_ptr<juce::Label> text;
        std::vector<std::unique_ptr<juce::TextButton>> options;
        std::unique_ptr<juce::TextButton> skip;
    };

    void rebuild (const std::vector<AssistantWorker::PendingQuestion>& questions)
    {
        rows.clear();
        timers.clear();
        removeAllChildren();
        for (auto& q : questions)
        {
            auto row = std::make_unique<Row>();
            row->text = std::make_unique<juce::Label> ();
            row->text->setText (q.text, juce::dontSendNotification);
            row->text->setColour (juce::Label::textColourId, kWarn);
            row->text->setMinimumHorizontalScale (0.6f);
            addAndMakeVisible (*row->text);
            for (int i = 0; i < q.options.size(); ++i)
            {
                auto b = std::make_unique<juce::TextButton> (q.options[i]);
                const int id = q.id;
                b->onClick = [this, id, i] { processor.getAssistant().answer (id, i, false); };
                addAndMakeVisible (*b);
                row->options.push_back (std::move (b));
            }
            row->skip = std::make_unique<juce::TextButton> (ko ("건너뛰기"));
            const int id = q.id;
            row->skip->onClick = [this, id] { processor.getAssistant().answer (id, -1, true); };
            addAndMakeVisible (*row->skip);
            auto timer = std::make_unique<juce::Label>();
            dimLabel (*timer);
            addAndMakeVisible (*timer);
            timers.push_back (std::move (timer));
            rows.push_back (std::move (row));
        }
        resized();
        if (auto* parent = getParentComponent())
            parent->resized();
    }

    SoundManagerProcessor& processor;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<std::unique_ptr<juce::Label>> timers;
    juce::String lastSignature;
};

class ChatTab : public Tab
{
public:
    explicit ChatTab (SoundManagerProcessor& p) : processor (p), questions (p)
    {
        styleLog (history);
        input.setTextToShowWhenEmpty (ko ("예: 드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어 / 전체 믹스 시작해줘 / waterfall 보여줘"), kDim);
        input.onReturnKey = [this] { send(); };
        sendButton.setButtonText (ko ("보내기"));
        sendButton.onClick = [this] { send(); };
        reset.setButtonText (ko ("새 대화"));
        reset.onClick = [this] { processor.getAssistant().resetConversation(); };
        status.setColour (juce::Label::textColourId, kAccent);
        dimLabel (engineLabel);
        for (auto* c : std::initializer_list<juce::Component*> { &history, &questions, &input, &sendButton, &reset, &status, &engineLabel })
            addAndMakeVisible (c);
    }

    void refresh() override
    {
        juce::String text;
        for (auto& e : processor.getLog())
        {
            if (e.who == "you")     text << ko ("나: ") << e.text << "\n\n";
            else if (e.who == "AI") text << "AI: " << e.text << "\n\n";
        }
        setLogText (history, lastText, text);
        status.setText (processor.getAssistant().status(), juce::dontSendNotification);
        engineLabel.setText (processor.getAssistant().reasoningEngine() == "local-llm" ? ko ("추론: 로컬 LLM") : ko ("추론: 규칙 기반"),
                             juce::dontSendNotification);
        questions.update();
        if (questions.preferredHeight() != lastQuestionHeight)
        {
            lastQuestionHeight = questions.preferredHeight();
            resized();
        }
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
        auto info = r.removeFromBottom (20);
        engineLabel.setBounds (info.removeFromRight (140));
        status.setBounds (info);
        questions.setBounds (r.removeFromBottom (questions.preferredHeight()));
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
        processor.getAssistant().submit (text);
    }

    SoundManagerProcessor& processor;
    QuestionPanel questions;
    juce::TextEditor history, input;
    juce::TextButton sendButton, reset;
    juce::Label status, engineLabel;
    juce::String lastText;
    int lastQuestionHeight = -1;
};

//==============================================================================
class AnalysisTab : public Tab
{
public:
    explicit AnalysisTab (SoundManagerProcessor& p) : view (p)
    {
        for (auto& m : AnalysisView::modes())
        {
            auto b = std::make_unique<juce::TextButton> (AnalysisView::modeLabel (m));
            b->setClickingTogglesState (true);
            b->setRadioGroupId (77);
            b->onClick = [this, m] { view.setMode (m); };
            addAndMakeVisible (*b);
            buttons.push_back (std::move (b));
        }
        buttons.front()->setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (view);
    }

    void show (const juce::String& mode)
    {
        view.setMode (mode);
        const int i = AnalysisView::modes().indexOf (mode);
        for (size_t b = 0; b < buttons.size(); ++b)
            buttons[b]->setToggleState (static_cast<int> (b) == i, juce::dontSendNotification);
    }

    AnalysisView& getView() noexcept { return view; }
    void refresh() override {}

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (28);
        for (auto& b : buttons)
        {
            b->setBounds (top.removeFromLeft (130));
            top.removeFromLeft (4);
        }
        r.removeFromTop (6);
        view.setBounds (r);
    }

private:
    AnalysisView view;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
};

//==============================================================================
class HistoryTab : public Tab
{
public:
    explicit HistoryTab (SoundManagerProcessor& p) : processor (p)
    {
        model.count = [this] { return static_cast<int> (ids.size()); };
        model.paint = [this] (int row, juce::Graphics& g, int w, int h, bool) {
            const auto* s = processor.getHub().history().find (ids[static_cast<size_t> (row)]);
            if (s == nullptr)
                return;
            const auto time = juce::Time (static_cast<juce::int64> (s->time * 1000.0));
            g.setColour (kDim);
            g.setFont (13.0f);
            g.drawText (time.toString (false, true, true, true), 6, 0, 80, h, juce::Justification::centredLeft);
            g.drawText (juce::String (s->source), 90, 0, 60, h, juce::Justification::centredLeft);
            g.setColour (kText);
            juce::String channels;
            for (auto& c : s->channels)
                channels << juce::String (c.channelName) << " ";
            g.drawText (juce::String (s->label), 155, 0, 300, h, juce::Justification::centredLeft);
            g.setColour (kDim);
            g.drawText (channels, 460, 0, w - 465, h, juce::Justification::centredLeft);
        };
        list.setModel (&model);
        styleList (list);
        restore.setButtonText (ko ("이 시점으로 되돌리기"));
        restore.onClick = [this] {
            const int row = list.getSelectedRow();
            if (! juce::isPositiveAndBelow (row, static_cast<int> (ids.size())))
                return;
            juce::String report;
            processor.getHub().restore (ids[static_cast<size_t> (row)], processor.getInstanceId(), &report);
            processor.addLog ("system", report);
        };
        dimLabel (hint, ko ("AI가 바꿀 때마다(채팅, 자동 믹스, 스타일, 체인) 믹스 상태가 기록돼요. 플러그인 상태 전체는 memopro 메모리 런타임에 압축 보관되고 프로젝트에도 저장돼요."));
        for (auto* c : std::initializer_list<juce::Component*> { &list, &restore, &hint })
            addAndMakeVisible (c);
    }

    void refresh() override
    {
        std::vector<std::int64_t> now;
        const auto& session = processor.getHub().getSession();
        for (auto it = processor.getHub().history().snapshots().rbegin(); it != processor.getHub().history().snapshots().rend(); ++it)
            for (auto& c : it->channels)
                if (session.inScope (processor.getInstanceId(), c.channelId))
                {
                    now.push_back (it->id);
                    break;
                }
        if (now != ids)
        {
            ids = now;
            list.updateContent();
        }
        list.repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (28);
        restore.setBounds (top.removeFromLeft (180));
        hint.setBounds (r.removeFromBottom (36));
        r.removeFromTop (6);
        list.setBounds (r);
    }

private:
    SoundManagerProcessor& processor;
    LambdaListModel model;
    juce::ListBox list;
    juce::TextButton restore;
    juce::Label hint;
    std::vector<std::int64_t> ids;
};

//==============================================================================
class ReferenceTab : public Tab
{
public:
    explicit ReferenceTab (SoundManagerProcessor& p) : processor (p)
    {
        load.setButtonText (ko ("음원/영상 올리기"));
        load.onClick = [this] {
            chooser = std::make_unique<juce::FileChooser> (ko ("원하는 스타일의 음원이나 영상"), juce::File(),
                                                           "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.mp4;*.mov;*.mkv;*.webm;*.aac");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
                if (fc.getResult().existsAsFile())
                    processor.getEngine().referenceJob().analyse (fc.getResult());
            });
        };
        use.setButtonText (ko ("이 프로젝트의 레퍼런스로 선택"));
        use.onClick = [this] {
            const int row = list.getSelectedRow();
            const auto& refs = processor.getEngine().references();
            if (juce::isPositiveAndBelow (row, static_cast<int> (refs.size())))
            {
                processor.setReferenceName (juce::String (refs[static_cast<size_t> (row)].name));
                processor.addLog ("system", ko ("레퍼런스 '") + juce::String (refs[static_cast<size_t> (row)].name) + ko ("'를 선택했어요."));
            }
        };
        match.setButtonText (ko ("레퍼런스에 맞추기"));
        match.onClick = [this] {
            processor.addLog ("you", ko ("레퍼런스 스타일에 맞춰줘"));
            processor.getAssistant().submit (ko ("레퍼런스 스타일에 맞춰줘"));
        };
        model.count = [this] { return static_cast<int> (processor.getEngine().references().size()); };
        model.paint = [this] (int row, juce::Graphics& g, int w, int h, bool) {
            const auto& r = processor.getEngine().references()[static_cast<size_t> (row)];
            const bool chosen = juce::String (r.name) == processor.getReferenceName();
            g.setColour (chosen ? kAccent : kText);
            g.drawText (juce::String (r.name) + (chosen ? ko ("  (선택됨)") : juce::String()), 6, 0, 260, h, juce::Justification::centredLeft);
            g.setColour (kDim);
            juce::String d;
            for (auto& x : r.descriptors()) d << juce::String (x) << "  ";
            g.drawText (juce::String (r.integratedLufs, 1) + " LUFS   " + d, 270, 0, w - 275, h, juce::Justification::centredLeft);
        };
        list.setModel (&model);
        styleList (list);
        status.setColour (juce::Label::textColourId, kAccent);
        dimLabel (hint, ko ("레퍼런스의 톤 밸런스(1/3옥타브), 라우드니스, 다이내믹, 스테레오 폭을 분석해 마스터/버스를 맞추고, 악기별 기본 스타일을 정해요. "
                            "영상은 로컬 ffmpeg로 소리만 읽어요(인터넷 사용 없음)."));
        for (auto* c : std::initializer_list<juce::Component*> { &load, &use, &match, &list, &status, &hint })
            addAndMakeVisible (c);
    }

    void refresh() override
    {
        status.setText (processor.getEngine().referenceJob().describe(), juce::dontSendNotification);
        list.updateContent();
        list.repaint();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // 1/3-octave curve of the selected reference vs this channel's current tone (10 bands).
        const auto* ref = processor.getEngine().findReference (processor.getReferenceName().toStdString());
        auto r = plotArea.toFloat();
        g.setColour (kPanel);
        g.fillRect (r);
        if (ref == nullptr)
        {
            g.setColour (kDim);
            g.drawText (ko ("레퍼런스를 선택하면 톤 곡선을 비교해 보여줘요"), plotArea, juce::Justification::centred);
            return;
        }
        auto curve = [&] (auto values, int n, auto freqOf, juce::Colour c) {
            juce::Path p;
            for (int i = 0; i < n; ++i)
            {
                const float x = r.getX() + r.getWidth() * std::log (freqOf (i) / 20.0f) / std::log (1000.0f);
                const float y = juce::jmap (juce::jlimit (-40.0f, 0.0f, values[static_cast<size_t> (i)]), -40.0f, 0.0f, r.getBottom(), r.getY());
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (2.0f));
        };
        curve (ref->thirdOctaveDb, smix::style::kThirdOctaveBands, [] (int i) { return smix::style::thirdOctaveCentreHz (i); }, kAccent);
        if (const auto* c = processor.getHub().getSession().find (processor.getInstanceId()); c != nullptr && c->features.valid)
            curve (c->features.bandLevelDb, smix::SpectrumBands::kNumBands, [] (int i) { return smix::SpectrumBands::centreHz (i); }, kWarn);
        g.setColour (kDim);
        g.drawText (ko ("초록: 레퍼런스 · 노랑: 지금 이 채널"), plotArea.withHeight (16), juce::Justification::right);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (28);
        load.setBounds (top.removeFromLeft (150));
        top.removeFromLeft (6);
        use.setBounds (top.removeFromLeft (200));
        top.removeFromLeft (6);
        match.setBounds (top.removeFromLeft (150));
        r.removeFromTop (6);
        status.setBounds (r.removeFromTop (22));
        hint.setBounds (r.removeFromBottom (36));
        plotArea = r.removeFromBottom (r.getHeight() / 2).reduced (0, 6);
        list.setBounds (r);
    }

private:
    SoundManagerProcessor& processor;
    juce::TextButton load, use, match;
    LambdaListModel model;
    juce::ListBox list;
    juce::Label status, hint;
    juce::Rectangle<int> plotArea;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
class PluginsTab : public Tab
{
public:
    explicit PluginsTab (SoundManagerProcessor& p) : processor (p)
    {
        scan.setButtonText (ko ("플러그인 스캔"));
        scan.onClick = [this] { processor.getLibrary().startScan(); };
        learn.setButtonText (ko ("모든 플러그인 분석·학습"));
        learn.onClick = [this] {
            auto& job = processor.getEngine().profiler();
            if (job.isRunning())
                job.cancel();
            else
                job.start (processor.getEngine().unprofiledPlugins());
        };
        filter.setTextToShowWhenEmpty (ko ("검색 (이름/제조사/종류)"), kDim);
        filter.onTextChange = [this] { rebuild(); };
        ask.setTextToShowWhenEmpty (ko ("지식 검색: 예) 보컬 치찰음 줄이는 플러그인"), kDim);
        ask.onReturnKey = [this] {
            auto& e = processor.getEngine();
            auto lease = e.modules().acquire (smix::modules::ModuleId::Knowledge, Engine::now());
            juce::String out;
            for (auto& h : e.knowledge().search (ask.getText().toStdString(), 5))
                out << juce::String (h.name) << " [" << juce::String (smix::toString (h.category)) << "]  " << juce::String (h.score, 2) << "\n";
            answer.setText (out.isEmpty() ? ko ("결과 없음 (먼저 플러그인을 분석해 주세요)") : out, false);
        };
        styleLog (answer);

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
            g.drawText (juce::String (info->name), 32, 0, 240, h, juce::Justification::centredLeft);
            g.setColour (kDim);
            g.drawText (juce::String (info->manufacturer) + " (" + juce::String (info->format) + ")", 275, 0, 210, h, juce::Justification::centredLeft);
            g.drawText (juce::String (smix::toString (info->category)) + (info->categoryOverridden ? " *" : ""), 490, 0, 110, h,
                        juce::Justification::centredLeft);
            const auto state = profiled.count (info->uid) ? profiled[info->uid] : 0;
            g.setColour (state == 1 ? kAccent : state == 2 ? juce::Colours::indianred : kDim);
            g.drawText (state == 1 ? ko ("학습됨") : state == 2 ? ko ("분석 실패") : ko ("미분석"), 605, 0, w - 610, h, juce::Justification::centredLeft);
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
                    menu.addItem (c + 1, juce::String (smix::toString (static_cast<smix::PluginCategory> (c))), true, static_cast<int> (info->category) == c);
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
        };
        list.setModel (&model);
        styleList (list);
        dimLabel (hint, ko ("AI가 쓸 플러그인을 체크하세요(클릭: 허용/해제, 우클릭: 종류 수정). '분석·학습'은 각 플러그인에 테스트 신호를 보내 파라미터가 실제로 "
                            "무엇을 하는지 측정해 기억해요. 별도 프로세스에서 돌아서 플러그인이 죽어도 DAW는 안전해요."));
        status.setColour (juce::Label::textColourId, kAccent);
        dimLabel (count);
        for (auto* c : std::initializer_list<juce::Component*> { &scan, &learn, &filter, &list, &hint, &status, &count, &ask, &answer })
            addAndMakeVisible (c);
        rebuild();
    }

    void refresh() override
    {
        auto& lib = processor.getLibrary();
        auto& job = processor.getEngine().profiler();
        learn.setButtonText (job.isRunning() ? ko ("분석 중지") : ko ("모든 플러그인 분석·학습"));
        status.setText (lib.isScanning() ? lib.getScanStatus() : job.describe(), juce::dontSendNotification);
        count.setText (ko ("허용: ") + juce::String (static_cast<int> (lib.catalog().allowedPlugins().size())) + " / "
                           + juce::String (static_cast<int> (lib.catalog().all().size())),
                       juce::dontSendNotification);
        if (lib.catalog().all().size() != lastTotal || ++ticks % 8 == 0)
            rebuild();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (28);
        scan.setBounds (top.removeFromLeft (110));
        top.removeFromLeft (6);
        learn.setBounds (top.removeFromLeft (170));
        top.removeFromLeft (6);
        filter.setBounds (top.removeFromLeft (200));
        top.removeFromLeft (6);
        count.setBounds (top);
        r.removeFromTop (4);
        status.setBounds (r.removeFromTop (20));
        hint.setBounds (r.removeFromTop (36));
        auto bottom = r.removeFromBottom (90);
        ask.setBounds (bottom.removeFromTop (26));
        answer.setBounds (bottom.withTrimmedTop (4));
        r.removeFromBottom (6);
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
        profiled.clear();
        auto& e = processor.getEngine();
        if (e.modules().isLoaded (smix::modules::ModuleId::Knowledge) || e.knowledgeFile().existsAsFile())
        {
            auto lease = e.modules().acquire (smix::modules::ModuleId::Knowledge, Engine::now());
            for (auto& uid : visible)
                if (e.knowledge().contains (uid))
                    profiled[uid] = e.knowledge().get (uid)->failed ? 2 : 1;
        }
        list.updateContent();
        list.repaint();
    }

    SoundManagerProcessor& processor;
    juce::TextButton scan, learn;
    juce::TextEditor filter, ask, answer;
    juce::Label hint, status, count;
    LambdaListModel model;
    juce::ListBox list;
    std::vector<std::string> visible;
    std::map<std::string, int> profiled;
    size_t lastTotal = 0;
    int ticks = 0;
};

//==============================================================================
class SettingsTab : public Tab
{
public:
    explicit SettingsTab (SoundManagerProcessor& p) : processor (p)
    {
        auto& lib = processor.getLibrary();
        auto addRow = [this] (juce::Label& label, const juce::String& text, juce::TextEditor& editor, const juce::String& value) {
            dimLabel (label, text);
            editor.setText (value, false);
            addAndMakeVisible (label);
            addAndMakeVisible (editor);
        };
        addRow (llmLabel, ko ("로컬 언어 모델 (GGUF)"), llmPath, lib.getSetting ("llmModelPath", processor.getEngine().llmModelFile().getFullPathName()));
        addRow (earLabel, ko ("악기 인식 모델"), earPath, lib.getSetting ("earModelPath", processor.getEngine().earModelFile().getFullPathName()));
        addRow (ffmpegLabel, ko ("ffmpeg (영상 레퍼런스)"), ffmpegPath, lib.getSetting ("ffmpegPath", "ffmpeg"));
        addRow (budgetLabel, ko ("memopro 메모리 상한 (MiB)"), budget, lib.getSetting ("memoryBudgetMiB", "512"));
        addRow (moduleBudgetLabel, ko ("모듈 메모리 상한 (MiB)"), moduleBudget, lib.getSetting ("moduleBudgetMiB", "4096"));
        aggressive.setButtonText (ko ("무음 채널의 플러그인도 절전 (소리가 다시 나면 자동 복귀, 처음 0.5초는 원음)"));
        aggressive.setToggleState (lib.getSetting ("hibernateSilent", "0") == "1", juce::dontSendNotification);
        addAndMakeVisible (aggressive);

        save.setButtonText (ko ("저장"));
        save.onClick = [this] {
            auto& l = processor.getLibrary();
            l.setSetting ("llmModelPath", llmPath.getText().trim());
            l.setSetting ("earModelPath", earPath.getText().trim());
            l.setSetting ("ffmpegPath", ffmpegPath.getText().trim());
            l.setSetting ("memoryBudgetMiB", juce::String (budget.getText().getIntValue()));
            l.setSetting ("moduleBudgetMiB", juce::String (moduleBudget.getText().getIntValue()));
            l.setSetting ("hibernateSilent", aggressive.getToggleState() ? "1" : "0");
            processor.getEngine().modules().setMemoryBudget (static_cast<std::uint64_t> (moduleBudget.getText().getIntValue()) << 20);
            processor.addLog ("system", ko ("설정을 저장했어요. 메모리 상한 변경은 DAW를 다시 시작하면 적용돼요."));
        };
        addAndMakeVisible (save);

        model.count = [this] { return static_cast<int> (moduleIds.size()); };
        model.paint = [this] (int row, juce::Graphics& g, int w, int h, bool) {
            const auto id = moduleIds[static_cast<size_t> (row)];
            auto& mm = processor.getEngine().modules();
            const bool on = mm.isEnabled (id);
            g.setColour (on ? kAccent : kDim);
            g.drawRect (8, h / 2 - 7, 14, 14);
            if (on)
                g.fillRect (11, h / 2 - 4, 8, 8);
            g.setColour (kText);
            g.drawText (juce::String (smix::modules::koreanName (id)), 32, 0, 230, h, juce::Justification::centredLeft);
            g.setColour (mm.isLoaded (id) ? kAccent : kDim);
            g.drawText (juce::String (smix::modules::toString (mm.state (id))), 265, 0, 90, h, juce::Justification::centredLeft);
            g.setColour (kDim);
            g.drawText (statusFor (id), 360, 0, w - 365, h, juce::Justification::centredLeft);
        };
        model.clicked = [this] (int row, const juce::MouseEvent&) {
            const auto id = moduleIds[static_cast<size_t> (row)];
            if (id == smix::modules::ModuleId::Ear)
                return;  // the ear is the core of every instance
            auto& mm = processor.getEngine().modules();
            const bool on = ! mm.isEnabled (id);
            mm.setEnabled (id, on);
            processor.getLibrary().setSetting ("module." + juce::String (smix::modules::toString (id)), on ? "1" : "0");
            modules.repaint();
        };
        modules.setModel (&model);
        styleList (modules);
        styleLog (status);
        for (auto* c : std::initializer_list<juce::Component*> { &modules, &status })
            addAndMakeVisible (c);
    }

    void refresh() override
    {
        const auto statusJson = processor.getEngine().modules().status (Engine::now());
        moduleIds.clear();
        for (int i = 0; i <= static_cast<int> (smix::modules::ModuleId::Hibernation); ++i)
            moduleIds.push_back (static_cast<smix::modules::ModuleId> (i));
        moduleStatus = statusJson;
        modules.updateContent();
        modules.repaint();
        juce::String text = processor.getEngine().statusText();
        text << ko ("\n분석 도우미: ")
             << (Engine::ProfilerJob::helperExecutable().existsAsFile() ? Engine::ProfilerJob::helperExecutable().getFullPathName()
                                                                         : ko ("찾을 수 없음"));
        text << ko ("\n이 플러그인은 인터넷을 사용하지 않아요. 모든 모델과 데이터는 이 컴퓨터에만 있어요.");
        status.setText (text, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto row = [&r] { auto x = r.removeFromTop (24); r.removeFromTop (4); return x; };
        for (auto [label, editor] : { std::pair<juce::Label*, juce::TextEditor*> { &llmLabel, &llmPath }, { &earLabel, &earPath },
                                      { &ffmpegLabel, &ffmpegPath }, { &budgetLabel, &budget }, { &moduleBudgetLabel, &moduleBudget } })
        {
            auto x = row();
            label->setBounds (x.removeFromLeft (190));
            editor->setBounds (x);
        }
        aggressive.setBounds (row());
        save.setBounds (row().removeFromLeft (100));
        r.removeFromTop (6);
        status.setBounds (r.removeFromBottom (110));
        modules.setBounds (r);
    }

private:
    juce::String statusFor (smix::modules::ModuleId id) const
    {
        for (auto& m : moduleStatus.value ("modules", nlohmann::json::array()))
            if (m.value ("id", std::string {}) == smix::modules::toString (id))
            {
                juce::String s = juce::String (m.value ("memory", std::string {}));
                if (m.value ("in_use", 0) > 0) s << ko (" · 사용 중");
                const auto err = m.value ("error", std::string {});
                if (! err.empty()) s << " · " << juce::String::fromUTF8 (err.c_str());
                return s;
            }
        return {};
    }

    SoundManagerProcessor& processor;
    juce::Label llmLabel, earLabel, ffmpegLabel, budgetLabel, moduleBudgetLabel;
    juce::TextEditor llmPath, earPath, ffmpegPath, budget, moduleBudget, status;
    juce::ToggleButton aggressive;
    juce::TextButton save;
    LambdaListModel model;
    juce::ListBox modules;
    std::vector<smix::modules::ModuleId> moduleIds;
    nlohmann::json moduleStatus;
};
} // namespace

//==============================================================================
SoundManagerEditor::SoundManagerEditor (SoundManagerProcessor& p) : AudioProcessorEditor (p), processor (p)
{
    getLookAndFeel().setColour (juce::ResizableWindow::backgroundColourId, kBackground);
    tabs.setTabBarDepth (30);
    tabs.addTab (ko ("믹스"), kBackground, new MixTab (p), true);
    tabs.addTab (ko ("체인"), kBackground, new ChainTab (p), true);
    tabs.addTab (ko ("채팅"), kBackground, new ChatTab (p), true);
    auto* analysisTab = new AnalysisTab (p);
    analysis = &analysisTab->getView();
    analysisTabIndex = tabs.getNumTabs();
    tabs.addTab (ko ("분석"), kBackground, analysisTab, true);
    tabs.addTab (ko ("기록"), kBackground, new HistoryTab (p), true);
    tabs.addTab (ko ("레퍼런스"), kBackground, new ReferenceTab (p), true);
    tabs.addTab (ko ("플러그인"), kBackground, new PluginsTab (p), true);
    tabs.addTab (ko ("모듈·설정"), kBackground, new SettingsTab (p), true);
    addAndMakeVisible (tabs);
    setResizable (true, true);
    setResizeLimits (820, 520, 1800, 1300);
    setSize (980, 640);
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
    // A graph requested in chat ("waterfall 보여줘") opens the analysis tab.
    const auto view = processor.consumeRequestedView();
    if (view.isNotEmpty() && analysisTabIndex >= 0)
    {
        tabs.setCurrentTabIndex (analysisTabIndex);
        if (auto* t = dynamic_cast<AnalysisTab*> (tabs.getTabContentComponent (analysisTabIndex)))
            t->show (view);
    }
    if (auto* t = dynamic_cast<Tab*> (tabs.getCurrentContentComponent()))
        t->refresh();
}
