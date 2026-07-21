/*
 * ehci.h — EHCI UIM soft state + controller bring-up prototypes.
 */
#ifndef EHCI_H
#define EHCI_H
#include "ehci_regs.h"
#include "ehci_qh_qtd.h"

/* Milestone-6 bring-up switch.
 *   0 = SAFE FIRST TEST: leave CONFIGFLAG at 0, so all root ports stay routed to
 *       the companion (USB 1.1) controllers. Validates the whole init path
 *       (bind, PCI enable, EECP handoff, register map, HCReset, schedules, run)
 *       WITHOUT seizing the working 1.1 ports — a partial failure is harmless.
 *   1 = FULL TAKEOVER: route all ports to EHCI (real high-speed operation).
 * Flip to 1 once init is confirmed clean on hardware. */
#ifndef EHCI_ROUTE_PORTS_TO_EHCI
#define EHCI_ROUTE_PORTS_TO_EHCI 1   /* init confirmed clean on HW (r6); full takeover for the MOUNT */
#endif

/* Minimal Name Registry entry id (matches <NameRegistry.h> RegEntryID layout).
 * The USB Expert passes UIMInitialize a pointer to the controller's node. */
typedef struct { UInt32 contents[4]; } EHCIRegEntryID;
typedef EHCIRegEntryID *EHCIRegEntryIDPtr;

/* A wired DMA page that sub-allocates aligned, physically-addressable blocks. */
typedef struct ehci_dma_pool {
    UInt8 *base;       /* logical base of the wired page   */
    UInt32 basePhys;   /* host-order physical base          */
    UInt32 size;       /* page size in bytes                */
    UInt32 used;       /* bytes handed out so far           */
} ehci_dma_pool;

/* Per-controller soft state (one instance per matched EHCI PCI node). */
typedef struct ehci_softc {
    EHCIRegEntryID node;         /* copy of the controller's Name Registry node   */
    volatile void *capBase;      /* EHCI capability register base (from the node) */
    volatile void *opBase;       /* operational regs = capBase + CAPLENGTH        */
    UInt32         regPhys;      /* physical addr of the register BAR (diagnostics)*/
    UInt32        *frameList;    /* logical addr, 1024-entry periodic list        */
    UInt32         frameListPhys;/* host-order physical addr of frameList          */
    ehci_qh       *asyncAnchor;  /* dummy QH heading the async reclamation ring    */
    UInt32         asyncQHPhys;  /* host-order physical addr of the async anchor QH*/
    ehci_dma_pool  pool;         /* DMA memory for QHs/qTDs                         */
    UInt8          nPorts;       /* root ports (from HCSPARAMS)                     */
    UInt8          started;      /* controller running                             */
    UInt8          sharedCompanion; /* 1 = the claim released an occupied port to a companion (a live
                                     * kbd/mouse shares this controller's IRQ) -> the ISR must chain to
                                     * the displaced handler. 0 = dedicated line (PCI card) -> never chain
                                     * on our own interrupts (that stalls our completion path).          */
} ehci_softc;

/* Pure EHCI bring-up (no OS calls): operate on an already-mapped sc->capBase and
 * an already-allocated, physically-contiguous sc->frameList/asyncQH. */
int ehci_hc_reset(ehci_softc *sc);   /* stop + HCReset; 0 on success, -1 timeout */
int ehci_hc_start(ehci_softc *sc);   /* program schedules, route ports, run      */

/* Shared controller soft state (defined in ehci_uim.c). */
extern ehci_softc gSoftc;

/* OS glue (ehci_os.c): enable PCI, map registers, alloc frame list, bring up. */
long ehci_os_init(ehci_softc *sc, EHCIRegEntryIDPtr node);

/* Init-phase tracing to a flushed disk file (ehci_os.c); safe from the app-context Initialize. */
void ehci_os_log(const char *s);
void ehci_os_logx(const char *label, unsigned long v);

/* DMA pool (ehci_os.c): wired-page allocator for QHs/qTDs.
 * ehci_dma_alloc returns a zeroed, `align`-aligned logical block and writes its
 * host-order physical address to *physOut; returns 0 (NULL) if the pool is full. */
long  ehci_dma_pool_init(ehci_dma_pool *p, UInt32 size);
void *ehci_dma_alloc(ehci_dma_pool *p, UInt32 bytes, UInt32 align, UInt32 *physOut);

/* Async schedule anchor (ehci_hw.c): build the dummy head-of-reclamation QH. */
long ehci_build_async_anchor(ehci_softc *sc);

/* Endpoint QH setup + async ring insertion (ehci_hw.c, pure EHCI logic). */
void ehci_qh_init(ehci_qh *qh, UInt8 devAddr, UInt8 endpt,
                  UInt16 maxPacket, int isControl);
void ehci_qh_link_async(ehci_softc *sc, ehci_qh *qh, UInt32 qhPhys);

/* Transfer construction (ehci_xfer.c, pure EHCI logic). */
long ehci_control_transfer(ehci_softc *sc, ehci_qh *qh, UInt32 setupPhys,
                           UInt32 dataPhys, UInt32 dataLen, int dirIn,
                           UInt32 *statusQtdPhys);
long ehci_bulk_transfer(ehci_softc *sc, ehci_qh *qh, UInt32 bufPhys,
                        UInt32 len, int dirIn, UInt32 *qtdPhysOut);
long ehci_qtd_status(ehci_qtd *qtd);   /* 1=active, 0=ok, <0=error status bits */

/* Root-hub port ops + interrupt ack (ehci_hw.c, pure register logic). */
UInt32 ehci_root_port_status(ehci_softc *sc, int port);
void   ehci_root_port_power(ehci_softc *sc, int port, int on);
void   ehci_root_port_reset(ehci_softc *sc, int port);
void   ehci_root_port_reset_done(ehci_softc *sc, int port);
UInt32 ehci_int_ack(ehci_softc *sc);   /* ack + return pending interrupt events */
void   ehci_hc_suspend(ehci_softc *sc);/* quiesce controller for system sleep    */
void   ehci_hc_resume(ehci_softc *sc); /* re-run controller on wake              */

#endif /* EHCI_H */
