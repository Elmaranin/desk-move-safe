#ifndef PARAMS_H
#define PARAMS_H
//
// params — the stored record as named parameters, for the 'eeprom' command.
//
// Every field of settings_t has one entry here, named <section>_<field> after
// the console group it belongs to: panel_*, desk_*, lim_*. Reading shows the
// live value; setting goes through the owning module's setter, so the same
// validation applies as anywhere else and the record is marked dirty and
// written by settings.c once things are quiet.
//
// Some are read-only here: the travel limits' ends are measured, not typed in
// ('lim min', 'lim max', 'lim span', 'reset'), and the layout version is
// the firmware's. Setting one of those names the command that changes it.
//
// docs/commands.md has a row per parameter; tools/check_commands.sh checks the
// names against the table in params.c on every build.
//
#include <stdbool.h>

void params_print_all(void);
bool params_print(const char *name);                // false: no such name
void params_set(const char *name, const char *value);

#endif // PARAMS_H
