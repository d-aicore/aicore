#include "I18n.h"

#include "AppSettings.h"

#include <unordered_map>

namespace aicore::ui
{
namespace
{
struct Table
{
    bool loaded = false, pt = false;
    std::unordered_map<juce::String, juce::String> exact, upper; // English -> Portuguese; ENGLISH -> PORTUGUÊS
    std::vector<std::pair<juce::String, juce::String>> prefixes;  // keys that end a sentence start ("Opened: ")
};

Table& table()
{
    static Table t;
    return t;
}

juce::File tableFile()
{
    auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    for (auto f : { exe.getChildFile ("i18n-pt.json"), exe.getParentDirectory().getParentDirectory().getParentDirectory().getParentDirectory().getChildFile ("app").getChildFile ("resources").getChildFile ("i18n-pt.json") })
        if (f.existsAsFile())
            return f;
    return {};
}
} // namespace

void I18n::init()
{
    auto& t = table();
    if (t.loaded)
        return;
    t.loaded = true;
    // Diogo 2026-11-16: the whole interface in English. The Portuguese table is kept out of the app; tr() is the
    // identity unless AICORE_LANGUAGE=pt and an i18n-pt.json is put next to the exe by hand.
    t.pt = juce::SystemStats::getEnvironmentVariable ("AICORE_LANGUAGE", {}) == "pt";
    if (! t.pt)
        return;
    auto v = juce::JSON::parse (tableFile());
    if (auto* o = v.getDynamicObject())
        for (auto& p : o->getProperties())
        {
            const auto en = p.name.toString(), pt = p.value.toString();
            t.exact[en] = pt;
            t.upper[en.toUpperCase()] = upper (pt);
            if (en.length() >= 4 && (en.endsWith (": ") || en.endsWith (" ") || en.endsWith (":")))
                t.prefixes.push_back ({ en, pt });
        }
    std::sort (t.prefixes.begin(), t.prefixes.end(), [] (auto& a, auto& b) { return a.first.length() > b.first.length(); });
}

bool I18n::portuguese()
{
    init();
    return table().pt;
}

juce::String I18n::tr (const juce::String& english)
{
    init();
    auto& t = table();
    if (! t.pt || english.isEmpty())
        return english;
    if (auto it = t.exact.find (english); it != t.exact.end())
        return it->second;
    if (auto it = t.upper.find (english); it != t.upper.end())
        return it->second;
    // a message made at run time: its known start, then the rest as it is
    for (auto& [en, pt] : t.prefixes)
        if (english.startsWith (en))
            return pt + english.substring (en.length());
    // "Fade in ms": the words, then the unit as it is
    if (const int sp = english.lastIndexOfChar (' '); sp > 0)
    {
        const auto head = english.substring (0, sp), tail = english.substring (sp);
        if (tail.length() <= 5)
            if (auto it = t.exact.find (head); it != t.exact.end())
                return it->second + tail;
    }
    // "Name · rest" (section buttons, takes): each part
    if (english.contains (juce::String::fromUTF8 (" \xc2\xb7 ")))
    {
        auto parts = juce::StringArray::fromTokens (english, juce::String::fromUTF8 ("\xc2\xb7"), "");
        bool any = false;
        for (auto& part : parts)
        {
            const auto core = part.trim();
            const auto done = tr (core);
            if (done != core)
            {
                part = part.replace (core, done);
                any = true;
            }
        }
        if (any)
            return parts.joinIntoString (juce::String::fromUTF8 ("\xc2\xb7"));
    }
    return english;
}

juce::String I18n::upper (const juce::String& s)
{
    static const juce::String lower = juce::String::fromUTF8 ("\xc3\xa1\xc3\xa0\xc3\xa2\xc3\xa3\xc3\xa9\xc3\xaa\xc3\xad\xc3\xb3\xc3\xb4\xc3\xb5\xc3\xba\xc3\xa7");
    static const juce::String upperCase = juce::String::fromUTF8 ("\xc3\x81\xc3\x80\xc3\x82\xc3\x83\xc3\x89\xc3\x8a\xc3\x8d\xc3\x93\xc3\x94\xc3\x95\xc3\x9a\xc3\x87");
    juce::String out;
    for (auto c : s)
    {
        const int i = lower.indexOfChar (c);
        out << (i >= 0 ? juce::String::charToString (upperCase[i]) : juce::String::charToString (juce::CharacterFunctions::toUpperCase (c)));
    }
    return out;
}

void I18n::translateTree (juce::Component& root)
{
    if (! portuguese())
        return;
    for (auto* c : root.getChildren())
    {
        if (auto* te = dynamic_cast<juce::TextEditor*> (c))
        {
            const auto hint = te->getTextToShowWhenEmpty();
            const auto done = tr (hint);
            if (done != hint)
                te->setTextToShowWhenEmpty (done, te->findColour (juce::TextEditor::textColourId).withAlpha (0.55f));
        }
        if (auto* tc = dynamic_cast<juce::SettableTooltipClient*> (c))
        {
            const auto tip = tc->getTooltip();
            const auto done = tr (tip);
            if (done != tip)
                tc->setTooltip (done);
        }
        if (auto* header = dynamic_cast<juce::TableHeaderComponent*> (c))
            for (int i = 0; i < header->getNumColumns (false); ++i)
            {
                const int id = header->getColumnIdOfIndex (i, false);
                const auto name = header->getColumnName (id);
                if (tr (name) != name)
                    header->setColumnName (id, tr (name));
            }
        translateTree (*c);
    }
}
} // namespace aicore::ui
