/*
 * ehci_activate.r — SIZE resource for EHCIActivate.
 *
 * ★ THE n4d BUG THIS FIXES. n4d calls SetFrontProcess(Finder) so the Finder, not us, receives the
 * driver's PostEvent(diskEvt). But Retro68's default SIZE flags are 0x0080 (is32BitCompatible only) —
 * canBackground is NOT set, and classic Mac OS does not schedule a process in the background without it.
 * So the moment n4d handed the front away it stopped being scheduled: its WaitNextEvent loop stalled,
 * USLPolledProcessDoneQueue stopped being called, and dispatch slot 23 died after a few dozen calls.
 * The n5d2 probe measured exactly that — fewer than 64 uim23 calls in a whole session, with every driver
 * state variable clean and gServiceStop = 0.
 *
 * It also explains why the app still felt responsive: selecting it in the Application menu brings it
 * forward, which RESUMES it, so Cmd-Q works fine and nothing looks wrong from the desktop.
 *
 * Flags 0x5080 = 0x0080 is32BitCompatible (Retro68's default, kept)
 *              | 0x1000 canBackground          <- the fix: schedule us while in the background
 *              | 0x4000 acceptSuspendResumeEvents  <- so we are resumed cleanly rather than frozen
 * Partition sizes are copied EXACTLY from the build Retro68 was already producing (1024K pref and min);
 * changing them is not part of this fix, and the app is known to run in 1MB.
 *
 * Raw `data` rather than the SIZE template so this does not depend on a Rez include being present.
 */
data 'SIZE' (-1, purgeable) {
    $"5080"          /* flags: is32BitCompatible | canBackground | acceptSuspendResumeEvents */
    $"00100000"      /* preferred partition size = 1024K */
    $"00100000"      /* minimum   partition size = 1024K */
};
