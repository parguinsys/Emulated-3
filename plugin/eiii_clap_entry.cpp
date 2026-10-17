// clap_entry, the one symbol the plugin exports.  Kept out of eiii_clap.cpp so the CMake build can link the same plugin
// code into the .clap and, through clap-wrapper, into the .vst3 (make_clapfirst_plugins: ENTRY_SOURCE).
#include "clap/clap.h"
#include "plugin/eiii_clap_entry.h"

extern "C" CLAP_EXPORT clap_plugin_entry_t const clap_entry = { CLAP_VERSION_INIT, eiii_rack_init, eiii_rack_deinit, eiii_rack_get_factory };
