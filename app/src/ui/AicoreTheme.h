// AICore-C — the old AICORE's light theme (D:\AICore\app ... aicore_studio/theme.py), copied value by value:
// Core's Cool Slate with light surfaces, Core's dark buttons, Segoe UI 15 px. No new look (owner 2026-11-16:
// "tem de ficar como a versao anterior").
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace aicore::ui
{
namespace col
{
    inline const juce::Colour background { 0xffcdd4dc }, panel { 0xffe2e7ec }, card { 0xffeef1f4 }, field { 0xfff9fafb },
        grid { 0xffd3d9e0 }, outline { 0xffb4bfcb }, control { 0xff3d4555 }, hover { 0xffa8b8c8 }, edge { 0xff6e7e98 },
        onText { 0xff262b35 }, text { 0xff2e3440 }, muted { 0xff4c5568 }, dim { 0xff7a8aa2 }, primary { 0xff6e7e98 },
        title { 0xff4c5568 }, accent { 0xff6e7e98 }, ok { 0xff4f9a84 }, warn { 0xffb07a3c }, error { 0xffb45a52 },
        selectedText { 0xffd8e4ec }, level { 0xff8fa3bd }, muteOn { 0xffd9a250 }, cardEdge { 0xff9eaab7 };
    // gradients of the QSS
    inline const juce::Colour buttonTop { 0xff48515f }, buttonBottom { 0xff363d4b }, onTop { 0xffb9c7d4 },
        onBottom { 0xffa2b2c3 }, onEdge { 0xff8a9bb0 }, pressed { 0xff94a5b6 }, disabledFill { 0xff8f98a8 },
        disabledText { 0xffc9d1da }, disabledEdge { 0xffa3acba }, headerTop { 0xffeef1f4 }, headerBottom { 0xffdfe4ea },
        headerLine { 0xffa3aebb }, sectionTop { 0xffd9dfe5 }, sectionBottom { 0xffccd3db }, centreTop { 0xfff2f4f7 },
        centreBottom { 0xffe8ecf0 };
} // namespace col

/** Segoe UI at a pixel size, like the QSS font-size. */
juce::Font uiFont (float px, bool bold = false);

/** Core's segmented C (theme.py draw_logo), scaled into area; the diamond in coreColour. */
void drawLogo (juce::Graphics&, juce::Rectangle<float> area, juce::Colour colour, juce::Colour coreColour);

/** "1:02.345" as the old clock (multitrack.clock). */
juce::String clockText (double seconds);

class AicoreLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AicoreLookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return uiFont (15.0f); }
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return uiFont (15.0f); }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
    juce::Font getPopupMenuFont() override { return uiFont (15.0f); }
    // the texts in the chosen language (I18n): labels, combo boxes and menus are translated where they are drawn
    void drawLabel (juce::Graphics&, juce::Label&) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>&, const juce::String&) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart,
                        int thumbSize, bool over, bool down) override;
};

/** A button as the old QPushButton: dark block, light when hovered/checked. */
class AButton : public juce::TextButton
{
public:
    explicit AButton (const juce::String& text = {}, std::function<void()> onClickFn = {})
        : juce::TextButton (text)
    {
        onClick = std::move (onClickFn);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus (false);
    }
    int idealWidth (int padding = 28) const;
};
} // namespace aicore::ui
