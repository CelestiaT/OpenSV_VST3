# C2: OpenSV headless VST3 instrument (M1: plumbing shell, no engine yet).
# See docs/PLAN.md §4 M1/M2 and §6 for the parameter table.

juce_add_plugin(OpenSVVst3
    PRODUCT_NAME "OpenSV"
    COMPANY_NAME "OpenSV"
    BUNDLE_ID "org.opensv.vst3"
    PLUGIN_MANUFACTURER_CODE Open
    PLUGIN_CODE OsV3
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORY "Synth|Instrument"
)

target_sources(OpenSVVst3 PRIVATE
    src/OpenSVProcessor.cpp
)
target_include_directories(OpenSVVst3 PRIVATE src)

target_compile_definitions(OpenSVVst3 PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_IGNORE_VST3_MISMATCHED_PARAMETER_ID_WARNING=1
)

# OpenSVEngine is linked already although M1 only uses JUCE plumbing: the
# static-lib + plugin wiring is validated in one go (M2 drops the engine into
# processBlock without touching build machinery again).
target_link_libraries(OpenSVVst3 PRIVATE
    OpenSVEngine
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

if(MSVC)
    target_compile_options(OpenSVVst3 PRIVATE /utf-8 /permissive- /EHsc)
endif()
