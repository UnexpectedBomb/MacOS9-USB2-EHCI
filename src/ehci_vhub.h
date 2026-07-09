/*
 * ehci_vhub.h — Virtual root hub + downstream-transfer engine for the EHCI UIM (USL-4.2).
 *
 * This is the USB-stack-facing logic that was proven on real hardware in the app leg
 * (probe/ehci_hub.c), re-homed for the RESIDENT ndrv, where the USL drives it through
 * ThePluginDispatchTable rather than a foreground app. It operates on the single
 * controller soft state (gSoftc). A driver-context fragment can't printf, so any
 * diagnostics go through a log hook that the ndrv wires to USBExpertStatus.
 *
 * Proven slot map (from the app leg): 2/6 = Create Control/Bulk endpoint,
 * 3/7 = Control/Bulk transfer, 10/11 = Create Interrupt endpoint/transfer,
 * 24 = frame-time (must return a 64-bit value whose LOW word is a monotonic 1 ms
 * count — the fix that beat the USL completion-firing wall).
 *
 * Ported incrementally; see project memory (USL-4.2 build plan).
 */
#ifndef EHCI_VHUB_H
#define EHCI_VHUB_H
#include "ehci.h"

/* Driver-context logging hook (no printf in a UIM fragment). Messages must be short,
 * static strings; the ndrv shims this to USBExpertStatus, the dev build may drop it. */
typedef void (*ehci_vhub_logfn)(const char *msg);
void ehci_vhub_set_log(ehci_vhub_logfn fn);

/* USL completion UPPs — foreign transition vectors we call by cast (proven safe in the
 * app leg). Control: (pipe, status). Interrupt: (refcon, status, actualLength). */
typedef void (*ehci_usl_complete)(void *pipe, unsigned long status);
typedef void (*ehci_usl_intcomplete)(void *refcon, long status, unsigned long len);

/* ---- implemented in this increment ---- */
unsigned long long ehci_vhub_frame_time(void);             /* dispatch slot 24 clock   */
void ehci_vhub_port_status(int port, volatile UInt8 *out); /* 4-byte hub port status   */

/* ---- ported in the next increments (declared here as the design contract) ----
 *  xfer_init    : one-time DMA page + downstream control QH (task level).
 *  control_xfer : dispatch slot 3 — root-hub simulation (devAddr == root hub) OR a real
 *                 downstream control phase (devAddr != root hub), per-phase (pid 2/1/0).
 *  int_xfer     : dispatch slot 11 — hold the root hub's status-change interrupt pipe.
 *  service      : reap finished transfers + port changes + deliver completions; called
 *                 from the real EHCI interrupt handler and a periodic timer. */
int  ehci_vhub_xfer_init(void);
long ehci_vhub_control_xfer(void *pipe, void *complUPP, volatile UInt8 *buf,
                            UInt32 devAddr, UInt32 len, UInt32 pid);
long ehci_vhub_int_xfer(UInt32 devAddr, UInt32 endpt, void *refcon,
                        void *callback, volatile UInt8 *buf, UInt32 len);
/* Bulk (slots 6/7): register an endpoint, then transfer by (addr, endpt). dirIn: 1=IN, 0=OUT. */
long ehci_vhub_create_bulk(UInt32 addr, UInt32 endpt, UInt32 dirIn, UInt32 maxpkt);
long ehci_vhub_bulk_xfer(void *pipe, void *complUPP, volatile UInt8 *buf,
                         UInt32 addr, UInt32 endpt, UInt32 len, UInt32 dirIn);
UInt32 ehci_vhub_bulk_stats(long *lastStat, UInt32 *doneN, UInt32 *errN, UInt8 *d16);
void   ehci_vhub_irq_stats(unsigned long *isrHits, int *a2live);   /* r35: real-IRQ install/fire state */
/* r36 reliability diagnostics (all read at TASK level from uim23; interrupt-safe producers). */
int    ehci_vhub_portevt_pop(UInt32 *ms, UInt8 *port, UInt8 *ev, UInt32 *portsc);   /* 1=popped, 0=empty */
void   ehci_vhub_down_stats(UInt32 *done, UInt32 *err, UInt32 *timeouts, UInt32 *qdrop,
                            UInt32 *lastAddr, UInt32 *lastPid, long *lastStat);
UInt32 ehci_vhub_roothub_addr(void);   /* current root-hub USB address (ctrl_trace budgeting) */
/* r39: controller + shared-QH state captured at a downstream transfer TIMEOUT; returns a seq that
 * bumps on each new capture (task-level edge-trigger from uim23). */
UInt32 ehci_vhub_timeout_state(UInt32 *cmd, UInt32 *sts, UInt32 *async, UInt32 *qhP,
                               UInt32 *epChar, UInt32 *curQtd, UInt32 *ovlTok, UInt32 *qtdTok);
/* r41: last BOT WRITE/READ CSW failure (+ context counts); seq bumps on each new failure. */
UInt32 ehci_vhub_biofail(UInt8 *isWrite, UInt32 *lba, UInt16 *chunk, UInt8 *cswStat,
                         UInt32 *cswSig, UInt32 *cswResid, UInt32 *wrOk, UInt32 *rdOk, UInt32 *reject);
/* r21: task-level self-driven SCSI probe (INQUIRY/READ CAPACITY/READ blk0) over the mounter's idle bulk
 * endpoints, to bypass the parked Apple mounter. Call once per uim23 (task context). */
void ehci_vhub_selfprobe_tick(void);
unsigned long ehci_vhub_ms(void);   /* read-only clock snapshot (safe from any context) */
void ehci_vhub_service(void);

/* Install the real EHCI interrupt handler on the node's driver-ist member + arm a periodic
 * timer, both driving ehci_vhub_service. Call from slot 0 (Initialize) after the controller
 * is up. This is how a resident UIM services its own controller (as Apple's OHCI UIM does). */
void ehci_vhub_start_service(EHCIRegEntryIDPtr node);

#endif /* EHCI_VHUB_H */
