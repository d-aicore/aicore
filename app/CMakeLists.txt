# AICoreC — JUCE app (GUI window + headless CLI). Console subsystem so that
# --version / --self-test write to stdout; the same exe opens the JUCE window.
juce_add_console_app(AICoreC
    PRODUCT_NAME "AICore-C"
    COMPANY_NAME "AICORE"
    VERSION_STRING "0.1.0")

target_sources(AICoreC PRIVATE
    src/main.cpp
    src/ServiceJob.cpp
    src/ConsoleDetach.cpp
    src/ui/AicoreTheme.cpp
    src/ui/Workspace.cpp
    src/tracks/Arrange.cpp
    src/tracks/MasterChain.cpp
    src/tracks/ChainPanel.cpp
    src/tracks/ArrangeView.cpp
    src/tracks/TracksPage.cpp
    src/ui/AudioOut.cpp
    src/ui/AppSettings.cpp
    src/sample/SampleEdit.cpp
    src/sample/WaveView.cpp
    src/sample/SamplePlayer.cpp
    src/sample/SamplePage.cpp
    src/ui/Worker.cpp
    src/stems/StemsPage.cpp
    src/midi/MidiPage.cpp
    src/ai/AiPage.cpp
    src/lora/LoraPage.cpp
    src/renders/TemplateRendersPage.cpp
    src/database/DatabasePage.cpp
    src/settings/SettingsPages.cpp
    src/ui/I18n.cpp)

target_compile_definitions(AICoreC PRIVATE
    JUCE_WEB_BROWSER=0        # zero browser / zero webview (AGENTS.md)
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(AICoreC PRIVATE
    aicore_core
    juce::juce_core
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_cryptography)

# aicore-render (S5) — offline render of a PPQ 960 .mid through one VST3 instrument, contract
# aicore.service/1 profile "render". Headless host (juce_audio_processors_headless): no GUI, no
# Python, no GPL in our code. Console subsystem: stdout carries protocol lines only.
juce_add_console_app(aicore-render
    PRODUCT_NAME "aicore-render"
    COMPANY_NAME "AICORE"
    VERSION_STRING "0.1.0")

target_sources(aicore-render PRIVATE
    render/render_main.cpp)

target_compile_definitions(aicore-render PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1   # offline warm-up pumps the message thread (runDispatchLoopUntil)
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_ARA=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(aicore-render PRIVATE
    aicore_core
    juce::juce_core
    juce::juce_events
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors_headless
    juce::juce_cryptography
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# The fixed list of plugins AICore supports travels next to the exe (scan looks only for these).
# A target of its own that runs on every build: a POST_BUILD step only ran when aicore-render relinked,
# so editing the list alone left a stale copy next to the exe (audit 2026-11-16).
add_custom_target(aicore-render-plugin-list ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/render/supported-plugins.json"
            "$<TARGET_FILE_DIR:aicore-render>/supported-plugins.json"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/render/supported-plugins.json"
    COMMENT "supported-plugins.json -> next to aicore-render")
add_dependencies(aicore-render-plugin-list aicore-render)

