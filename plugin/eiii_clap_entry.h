// The plugin's CLAP entry functions (eiii_clap.cpp), exported as clap_entry by eiii_clap_entry.cpp.
#pragma once

bool eiii_rack_init(char const *plugin_path);
void eiii_rack_deinit();
void const *eiii_rack_get_factory(char const *factory_id);
