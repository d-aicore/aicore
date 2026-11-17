#include "AicoreTheme.h"
#include "I18n.h"

namespace aicore::ui
{
juce::Font uiFont (float px, bool bold)
{
    // Qt font-size is the em size in px; JUCE heights include ascent + descent, so set the point height
    return juce::Font (juce::FontOptions ("Segoe UI", px, bold ? juce::Font::bold : juce::Font::plain).withPointHeight (px));
}

namespace
{
const float kPieces[3][6][2] = {
    { { 0, 135 }, { 210, 0 }, { 283, 52 }, { 86, 180 }, { 86, 315 }, { 0, 360 } },
    { { 238, 125 }, { 313, 75 }, { 393, 130 }, { 318, 178 }, { -1, -1 }, { -1, -1 } },
    { { 43, 380 }, { 108, 330 }, { 206, 388 }, { 328, 315 }, { 393, 350 }, { 206, 478 } },
};
const float kCore[4][2] = { { 208, 200 }, { 256, 248 }, { 208, 295 }, { 160, 248 } };
} // namespace

void drawLogo (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, juce::Colour coreColour)
{
    const float scale = juce::jmin (area.getWidth() / 393.0f, area.getHeight() / 478.0f);
    const float dx = area.getX() + (area.getWidth() - 393.0f * scale) / 2.0f;
    const float dy = area.getY() + (area.getHeight() - 478.0f * scale) / 2.0f;
    auto poly = [&] (const float (*pts)[2], int n) {
        juce::Path p;
        p.startNewSubPath (dx + pts[0][0] * scale, dy + pts[0][1] * scale);
        for (int i = 1; i < n && pts[i][0] >= 0; ++i)
            p.lineTo (dx + pts[i][0] * scale, dy + pts[i][1] * scale);
        p.closeSubPath();
        return p;
    };
    g.setColour (colour);
    for (auto& piece : kPieces)
        g.fillPath (poly (piece, 6));
    g.setColour (coreColour);
    g.fillPath (poly (kCore, 4));
}

juce::String clockText (double seconds)
{
    seconds = juce::jmax (0.0, seconds);
    const int m = (int) (seconds / 60.0);
    return juce::String (m) + ":" + juce::String (std::fmod (seconds, 60.0), 3).paddedLeft ('0', 6);
}

AicoreLookAndFeel::AicoreLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, col::background);
    setColour (juce::TextButton::textColourOffId, col::selectedText);
    setColour (juce::TextButton::textColourOnId, col::onText);
    setColour (juce::Label::textColourId, col::text);
    setColour (juce::TextEditor::textColourId, col::text);
    setColour (juce::TextEditor::backgroundColourId, col::field);
    setColour (juce::TextEditor::outlineColourId, col::outline);
    setColour (juce::TextEditor::focusedOutlineColourId, col::muted);
    setColour (juce::TextEditor::highlightColourId, col::hover);
    setColour (juce::CaretComponent::caretColourId, col::text);
    setColour (juce::ComboBox::backgroundColourId, col::field);
    setColour (juce::ComboBox::textColourId, col::text);
    setColour (juce::ComboBox::outlineColourId, col::outline);
    setColour (juce::ComboBox::arrowColourId, col::muted);
    setColour (juce::PopupMenu::backgroundColourId, col::field);
    setColour (juce::PopupMenu::textColourId, col::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, col::hover);
    setColour (juce::PopupMenu::highlightedTextColourId, col::onText);
    setColour (juce::ScrollBar::thumbColourId, col::outline);
    setColour (juce::ListBox::backgroundColourId, col::field);
    setColour (juce::ToggleButton::textColourId, col::text);
    setColour (juce::ToggleButton::tickColourId, col::muted);
    setColour (juce::AlertWindow::backgroundColourId, col::panel);
    setColour (juce::AlertWindow::textColourId, col::text);
    setDefaultSansSerifTypefaceName ("Segoe UI");
}

void AicoreLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    juce::Colour edge = col::edge;
    if (! b.isEnabled())
    {
        g.setColour (col::disabledFill);
        g.fillRoundedRectangle (r, 2.0f);
        edge = col::disabledEdge;
    }
    else if (down)
    {
        g.setColour (col::pressed);
        g.fillRoundedRectangle (r, 2.0f);
        edge = col::dim;
    }
    else if (over || b.getToggleState())
    {
        g.setGradientFill (juce::ColourGradient (col::onTop, r.getX(), r.getY(), col::onBottom, r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 2.0f);
        edge = col::onEdge;
    }
    else
    {
        g.setGradientFill (juce::ColourGradient (col::buttonTop, r.getX(), r.getY(), col::buttonBottom, r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 2.0f);
    }
    g.setColour (edge);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);
}

void AicoreLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool down)
{
    juce::Colour c = ! b.isEnabled() ? col::disabledText
                     : (over || down || b.getToggleState()) ? col::onText : col::selectedText;
    g.setColour (c);
    g.setFont (uiFont (15.0f));
    g.drawFittedText (tr (b.getButtonText()), b.getLocalBounds().reduced (6, 0), juce::Justification::centred, 1);
}

int AButton::idealWidth (int padding) const
{
    return juce::roundToInt (juce::GlyphArrangement::getStringWidth (uiFont (15.0f), tr (getButtonText()))) + padding;
}

void AicoreLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    g.fillAll (label.findColour (juce::Label::backgroundColourId));
    if (! label.isBeingEdited())
    {
        const float alpha = label.isEnabled() ? 1.0f : 0.5f;
        const auto font = getLabelFont (label);
        g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));
        g.setFont (font);
        auto area = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());
        g.drawFittedText (tr (label.getText()), area, label.getJustificationType(),
                          juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight())), label.getMinimumHorizontalScale());
        g.setColour (label.findColour (juce::Label::outlineColourId).withMultipliedAlpha (alpha));
    }
    else if (label.isEnabled())
        g.setColour (label.findColour (juce::Label::outlineColourId));
    g.drawRect (label.getLocalBounds());
}

void AicoreLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                           bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                           const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour)
{
    LookAndFeel_V4::drawPopupMenuItem (g, area, isSeparator, isActive, isHighlighted, isTicked, hasSubMenu, tr (text), shortcutKeyText, icon, textColour);
}

void AicoreLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth, int& idealHeight)
{
    LookAndFeel_V4::getIdealPopupMenuItemSize (tr (text), isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
}

void AicoreLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& name)
{
    LookAndFeel_V4::drawPopupMenuSectionHeader (g, area, tr (name));
}

void AicoreLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.isEnabled() ? col::field : col::card);
    g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, 2.0f);
}

void AicoreLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.hasKeyboardFocus (true) ? col::muted : col::outline);
    g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, 2.0f, 1.0f);
    if (! e.hasKeyboardFocus (true))
    {
        g.setColour (col::cardEdge); // the QSS border-top-color
        g.drawHorizontalLine (0, 1.0f, (float) w - 1.0f);
    }
}

void AicoreLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    g.setColour (col::field);
    g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, 2.0f);
    g.setColour (box.isMouseOver (true) ? col::accent : col::outline);
    g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, 2.0f, 1.0f);
    juce::Path arrow;
    const float cx = (float) w - 14.0f, cy = (float) h * 0.5f;
    arrow.addTriangle (cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
    g.setColour (col::muted);
    g.fillPath (arrow);
}

void AicoreLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                       int thumbStart, int thumbSize, bool over, bool)
{
    g.setColour (col::grid);
    g.fillRect (x, y, w, h);
    auto thumb = vertical ? juce::Rectangle<int> (x + 2, thumbStart, w - 4, thumbSize)
                          : juce::Rectangle<int> (thumbStart, y + 2, thumbSize, h - 4);
    g.setColour (over ? col::dim : col::outline);
    g.fillRoundedRectangle (thumb.toFloat(), 3.0f);
}
} // namespace aicore::ui
