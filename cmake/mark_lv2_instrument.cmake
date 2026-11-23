# Mark the generated LV2 metadata as an instrument.
#
# JUCE's LV2 helper writes lv2:Plugin for every plugin and only emits
# lv2:InstrumentPlugin when IS_SYNTH is true. Suite Drums Glorp needs the
# instrument class so hosts like REAPER expose it as an LV2i and route MIDI to
# it, but we keep IS_SYNTH false so the VST3 wrapper still advertises the
# additional outputs as auxiliary buses.

if(NOT DEFINED PLUGIN_TTL_DIR)
    message(FATAL_ERROR "PLUGIN_TTL_DIR is required")
endif()

foreach(ttl_name manifest.ttl dsp.ttl)
    set(ttl_path "${PLUGIN_TTL_DIR}/${ttl_name}")

    if(NOT EXISTS "${ttl_path}")
        message(FATAL_ERROR "Missing LV2 metadata: ${ttl_path}")
    endif()

    file(READ "${ttl_path}" contents)
    string(REPLACE "a lv2:Plugin ;" "a lv2:Plugin, lv2:InstrumentPlugin ;" contents "${contents}")
    file(WRITE "${ttl_path}" "${contents}")
endforeach()
