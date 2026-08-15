/* Instrumented R2b-1 INIT packaging: the 68K INIT code resource (ehci_init_dbg.flt) + the SAME
 * embedded PowerPC BootMain fragment (BootMain.pef) that r2b1 shipped, as a 'PPC ' (128) resource
 * loaded via Get1Resource('PPC ',128) -> GetMemFragment. Both pulled in with Rez's $$read(). */
#include "Retro68.r"

type 'INIT' {
	RETRO68_CODE_TYPE
};

resource 'INIT' (128, locked) {
	dontBreakAtEntry, $$read("ehci_init_dbg.flt");
};

/* The PowerPC CFM fragment, embedded raw so the 68K INIT can GetMemFragment it. */
data 'PPC ' (128) {
	$$read("BootMain.pef")
};
