/*
 * ehci_hw.c — EHCI controller bring-up, hardware layer.
 *
 * Pure EHCI-spec logic: no OS calls. Operates on a mapped register base and a
 * pre-allocated, physically-contiguous periodic frame list. Endianness handled
 * via the ehci_* helpers (EHCI regs + DMA structures are little-endian).
 */
#include "ehci.h"

#define EHCI_SPIN_LIMIT 500000

/* Spin ~ms milliseconds off the running controller's frame counter: FRINDEX
 * advances one microframe (125us) per tick while RUN=1. Pure MMIO, no OS calls —
 * used for the USB connect-debounce wait after routing the ports to EHCI. */
static void ehci_frame_delay(volatile void *op, UInt32 ms)
{
    UInt32 want = ms * 8;                              /* 1 ms = 8 microframes */
    UInt32 prev = ehci_read32(op, EHCI_FRINDEX) & 0x3FFF;
    UInt32 elapsed = 0; long guard = 0;
    while (elapsed < want && guard++ < 20000000L) {
        UInt32 now = ehci_read32(op, EHCI_FRINDEX) & 0x3FFF;
        elapsed += (now - prev) & 0x3FFF;             /* accumulate deltas (handles wrap) */
        prev = now;
    }
}

/* Stop the controller and issue HCReset. 0 = ok, -1 = timeout. */
int ehci_hc_reset(ehci_softc *sc)
{
    volatile void *op = sc->opBase;
    UInt32 cmd;
    long spin;

    /* clear Run/Stop, wait for HCHalted */
    cmd = ehci_read32(op, EHCI_USBCMD) & ~EHCI_CMD_RUN;
    ehci_write32(op, EHCI_USBCMD, cmd);
    for (spin = 0; spin < EHCI_SPIN_LIMIT; spin++)
        if (ehci_read32(op, EHCI_USBSTS) & EHCI_STS_HALTED)
            break;

    /* assert HCReset, wait for it to self-clear */
    ehci_write32(op, EHCI_USBCMD, EHCI_CMD_HCRESET);
    for (spin = 0; spin < EHCI_SPIN_LIMIT; spin++)
        if (!(ehci_read32(op, EHCI_USBCMD) & EHCI_CMD_HCRESET))
            return 0;
    return -1;
}

/* ---- Power management: quiesce for sleep, restore on wake ----
 * The IOGEAR/NEC card (and USB 2.0 PCI cards generally) are reported to break
 * OS 9 sleep — plausibly because the EHCI function is left running and unmanaged
 * (no UIM ever claimed it). Claiming it and stopping it cleanly on a sleep
 * request is our lever to fix that (paired with a Power Manager handler in the
 * OS layer). */
void ehci_hc_suspend(ehci_softc *sc)
{
    UInt32 cmd = ehci_read32(sc->opBase, EHCI_USBCMD);
    long spin;
    cmd &= ~(EHCI_CMD_RUN | EHCI_CMD_ASE | EHCI_CMD_PSE);   /* stop schedules+run */
    ehci_write32(sc->opBase, EHCI_USBCMD, cmd);
    for (spin = 0; spin < EHCI_SPIN_LIMIT; spin++)
        if (ehci_read32(sc->opBase, EHCI_USBSTS) & EHCI_STS_HALTED)
            break;
}

void ehci_hc_resume(ehci_softc *sc)
{
    /* frame list + async ring are still programmed; just re-enable and run */
    ehci_write32(sc->opBase, EHCI_USBCMD,
        ((UInt32)1 << EHCI_CMD_ITC_SHIFT) |   /* r68: ITC 8->1 microframe (1ms->125us interrupt coalescing) — cut per-command completion latency (r67: ~75% of each command is overhead) */
        EHCI_CMD_ASE | EHCI_CMD_PSE | EHCI_CMD_RUN);
#if EHCI_ROUTE_PORTS_TO_EHCI
    ehci_write32(sc->opBase, EHCI_CONFIGFLAG, EHCI_CONFIGFLAG_CF);
#endif
}

/* ---- Root-hub port operations (the UIM simulates a root hub for the USL) ---- */

/* Read a root port's PORTSC (connect/enable/reset/power/change bits). */
UInt32 ehci_root_port_status(ehci_softc *sc, int port)
{
    return ehci_read32(sc->opBase, EHCI_PORTSC(port));
}

/* Write PORTSC while preserving the write-1-to-clear change bits (so we don't
 * inadvertently acknowledge connect/enable changes). */
static void port_write_preserve(ehci_softc *sc, int port, UInt32 v)
{
    v &= ~(EHCI_PORT_CONNECT_CH | EHCI_PORT_ENABLE_CH);
    ehci_write32(sc->opBase, EHCI_PORTSC(port), v);
}

void ehci_root_port_power(ehci_softc *sc, int port, int on)
{
    UInt32 v = ehci_read32(sc->opBase, EHCI_PORTSC(port));
    if (on) v |= EHCI_PORT_POWER; else v &= ~EHCI_PORT_POWER;
    port_write_preserve(sc, port, v);
}

/* Begin a port reset (assert Reset, clear Enable). The caller must hold reset
 * for >=50 ms then call ehci_root_port_reset_done() to release it. */
void ehci_root_port_reset(ehci_softc *sc, int port)
{
    UInt32 v = ehci_read32(sc->opBase, EHCI_PORTSC(port));
    v &= ~EHCI_PORT_ENABLE;
    v |= EHCI_PORT_RESET;
    port_write_preserve(sc, port, v);
}

void ehci_root_port_reset_done(ehci_softc *sc, int port)
{
    UInt32 v = ehci_read32(sc->opBase, EHCI_PORTSC(port));
    v &= ~EHCI_PORT_RESET;
    port_write_preserve(sc, port, v);   /* HC enables the port if high-speed */
}

/* Acknowledge (write-1-to-clear) and return the pending controller interrupt
 * events. Called from the UIM's interrupt-service slot. */
UInt32 ehci_int_ack(ehci_softc *sc)
{
    UInt32 sts = ehci_read32(sc->opBase, EHCI_USBSTS);
    UInt32 evt = sts & (EHCI_STS_USBINT | EHCI_STS_USBERRINT | EHCI_STS_PCD |
                        EHCI_STS_FLR | EHCI_STS_HSE | EHCI_STS_IAA);
    if (evt)
        ehci_write32(sc->opBase, EHCI_USBSTS, evt);   /* RW1C acknowledge */
    return evt;
}

/* Build the async-schedule anchor: a dummy queue head that heads the circular
 * reclamation list. H-bit set, high-speed, links to itself, overlay inactive.
 * Endpoint QHs are later spliced into this ring after the anchor. */
long ehci_build_async_anchor(ehci_softc *sc)
{
    UInt32 phys;
    ehci_qh *qh = (ehci_qh *)ehci_dma_alloc(&sc->pool, sizeof(ehci_qh), 32, &phys);
    if (qh == 0)
        return -1;
    sc->asyncAnchor = qh;
    sc->asyncQHPhys = phys;

    qh->hlink      = ehci_cpu_to_le32(phys | EHCI_LINK_TYP_QH);   /* -> itself   */
    qh->epChar     = ehci_cpu_to_le32(EHCI_QH_HEAD | EHCI_QH_EPS_HIGH);
    qh->epCaps     = 0;
    qh->curQtd     = 0;
    qh->ovlNext    = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    qh->ovlAltNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    qh->ovlToken   = ehci_cpu_to_le32(EHCI_QTD_STATUS_HALTED);    /* not active  */
    return 0;
}

/* Initialize a queue head for a high-speed control or bulk endpoint.
 * (Full/low-speed via a companion split-transaction is a later milestone.) */
void ehci_qh_init(ehci_qh *qh, UInt8 devAddr, UInt8 endpt,
                  UInt16 maxPacket, int isControl)
{
    UInt32 ch = EHCI_QH_DEVADDR(devAddr) | EHCI_QH_ENDPT(endpt) |
                EHCI_QH_EPS_HIGH | EHCI_QH_MAXLEN(maxPacket) |
                EHCI_QH_RL(isControl ? 0u : 4u);
    if (isControl)
        ch |= EHCI_QH_DTC;              /* control: data toggle carried in qTDs */

    qh->hlink      = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);  /* set on link      */
    qh->epChar     = ehci_cpu_to_le32(ch);
    qh->epCaps     = ehci_cpu_to_le32(EHCI_QH_MULT(1));
    qh->curQtd     = 0;
    qh->ovlNext    = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    qh->ovlAltNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    qh->ovlToken   = 0;   /* idle: not active, not halted (controller will advance) */
}

/* Splice a QH into the async reclamation ring, just after the anchor:
 *   anchor -> newQH -> (old anchor->next) -> ... -> anchor
 * Link the new QH first, then publish it into the ring (ordered by eieio). */
void ehci_qh_link_async(ehci_softc *sc, ehci_qh *qh, UInt32 qhPhys)
{
    qh->hlink = sc->asyncAnchor->hlink;                    /* already LE          */
    __asm__ __volatile__("eieio");                         /* order before publish */
    sc->asyncAnchor->hlink = ehci_cpu_to_le32(qhPhys | EHCI_LINK_TYP_QH);
}

/* Program the schedules, route all ports to EHCI, and start the controller. */
int ehci_hc_start(ehci_softc *sc)
{
    volatile void *op = sc->opBase;
    UInt32 i;

    /* start with an empty periodic list: every frame pointer terminated */
    for (i = 0; i < EHCI_FRAMELIST_ENTRIES; i++)
        sc->frameList[i] = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);

    ehci_write32(op, EHCI_CTRLDSSEGMENT,   0);                 /* 32-bit only    */
    ehci_write32(op, EHCI_PERIODICLISTBASE, sc->frameListPhys);
    ehci_write32(op, EHCI_ASYNCLISTADDR,    sc->asyncQHPhys);

    /* interrupt threshold 8 microframes; enable async + periodic; Run */
    ehci_write32(op, EHCI_USBCMD,
        ((UInt32)1 << EHCI_CMD_ITC_SHIFT) |   /* r68: ITC 8->1 microframe (1ms->125us interrupt coalescing) — cut per-command completion latency (r67: ~75% of each command is overhead) */
        EHCI_CMD_ASE | EHCI_CMD_PSE | EHCI_CMD_RUN);

#if EHCI_ROUTE_PORTS_TO_EHCI
    /* CONFIGFLAG = 1 routes all root ports to EHCI (the hardware forces every
     * Port Owner to 0). On a machine where the EHCI shares its physical ports
     * with the companion 1.1 controllers that drive the keyboard/mouse (e.g. the
     * Mac Mini's on-board controller), blindly claiming ALL ports would seize the
     * keyboard. So: hand every currently-OCCUPIED port back to the companion
     * (Owner = 1) and claim only the EMPTY ports for EHCI. The drive is inserted
     * afterward into an empty (now EHCI-owned) port and comes up at high speed,
     * while the keyboard/mouse stay on the companion. On a machine with dedicated
     * EHCI ports (MDD + PCI card) nothing is attached at claim time, so every
     * port is empty and this claims them all — identical to the old behavior.
     * Verified on a Mac Mini G4 by the MiniClaim test (kbd on port 0 released,
     * drive on port 1 links at high speed; kbd/mouse survive once the hosting app
     * pumps the event loop so the companion can re-enumerate them). */
    /* This chip has Port Power Control (HCSPARAMS PPC=1): after the HCReset above,
     * every port is powered OFF, and Current-Connect-Status reads 0 while a port is
     * unpowered. So POWER the ports FIRST, let the connect debounce, and only THEN
     * read CONNECT to decide. (Earlier builds read CCS with power still off, so an
     * attached device — the keyboard — looked empty and got CLAIMED instead of
     * RELEASED, stranding it on an EHCI port that can't drive it.) */
    ehci_write32(op, EHCI_CONFIGFLAG, EHCI_CONFIGFLAG_CF);
    for (i = 0; i < sc->nPorts; i++) {                     /* power every port so CCS becomes valid */
        UInt32 pv = (ehci_read32(op, EHCI_PORTSC(i)) & ~EHCI_PORTSC_RW1C) | EHCI_PORT_POWER;
        ehci_write32(op, EHCI_PORTSC(i), pv);
    }
    ehci_frame_delay(op, 500);                            /* power-good + connect debounce, power applied */
    sc->sharedCompanion = 0;
    for (i = 0; i < sc->nPorts; i++) {                    /* release occupied ports; keep empty ones for EHCI */
        UInt32 pv = ehci_read32(op, EHCI_PORTSC(i)) & ~EHCI_PORTSC_RW1C;
        if (pv & EHCI_PORT_CONNECT) {
            pv |= EHCI_PORT_OWNER;          /* occupied -> hand to the companion (kbd/mouse) ...        */
            sc->sharedCompanion = 1;        /* ...and a live device shares this controller's IRQ line   */
        } else {
            pv &= ~EHCI_PORT_OWNER;         /* empty -> EHCI owns it (the drive lands here)             */
        }
        pv |= EHCI_PORT_POWER;                               /* keep power on either way */
        ehci_write32(op, EHCI_PORTSC(i), pv);
    }
#else
    /* Safe first test: leave CONFIGFLAG = 0 — ports remain on the companion
     * controllers, so the working 1.1 ports are undisturbed while we validate
     * that the EHCI controller itself initializes. */
#endif
    sc->started = 1;
    return 0;
}
