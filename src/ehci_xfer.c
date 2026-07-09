/*
 * ehci_xfer.c — EHCI transfer construction (control + bulk), pure EHCI logic.
 *
 * Builds qTD chains in DMA memory and attaches them to an endpoint's queue head
 * so the controller executes them. All fields little-endian (ehci_cpu_to_le32).
 * No OS calls except ehci_dma_alloc() (implemented in ehci_os.c).
 *
 * NOTE (M3): qTDs are bump-allocated from the pool and not yet recycled — a
 * completion path that frees/reuses qTDs is a follow-up. Split transactions for
 * full/low-speed devices behind a hub are a later milestone (high-speed only here).
 */
#include "ehci.h"

/* EHCI buffer pointers are page-based: buffer[0] carries the start offset,
 * buffer[1..4] are the successive 4 KB pages. One qTD spans up to ~20 KB. */
static void qtd_set_buffers(ehci_qtd *qtd, UInt32 bufPhys)
{
    UInt32 page = bufPhys & ~0xFFFUL;
    int i;
    qtd->buffer[0] = ehci_cpu_to_le32(bufPhys);
    for (i = 1; i < 5; i++) {
        page += 0x1000;
        qtd->buffer[i] = ehci_cpu_to_le32(page);
    }
}

/* Fill one qTD. pidToggle carries the PID code + (for DTC endpoints) the toggle
 * and IOC bits. `nextPhys` is 0 for the last qTD (terminates the chain). */
static void qtd_fill(ehci_qtd *qtd, UInt32 pidToggle, UInt32 bufPhys,
                     UInt32 len, UInt32 nextPhys)
{
    UInt32 tok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) |
                 EHCI_QTD_BYTES(len) | pidToggle;
    qtd->next    = ehci_cpu_to_le32(nextPhys ? nextPhys : EHCI_LINK_TERMINATE);
    qtd->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    qtd->token   = ehci_cpu_to_le32(tok);
    if (len)
        qtd_set_buffers(qtd, bufPhys);
}

/* Attach a built qTD chain (first qTD at firstPhys) to the QH overlay so the
 * controller begins executing it on its next pass over this QH. */
static void qh_activate(ehci_qh *qh, UInt32 firstPhys)
{
    qh->ovlNext    = ehci_cpu_to_le32(firstPhys);   /* next qTD pointer, T=0    */
    qh->ovlAltNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    __asm__ __volatile__("eieio");
    qh->ovlToken   = 0;   /* clear Active/Halted so the HC advances to ovlNext  */
}

/*
 * Build and queue a control transfer:
 *   SETUP (8 bytes, toggle 0) -> [DATA (toggle 1, dir)] -> STATUS (opposite dir,
 *   toggle 1, zero-length, IOC). Returns the physical addr of the status qTD in
 *  *statusQtdPhys so the caller can poll it, or <0 on allocation failure.
 */
long ehci_control_transfer(ehci_softc *sc, ehci_qh *qh, UInt32 setupPhys,
                           UInt32 dataPhys, UInt32 dataLen, int dirIn,
                           UInt32 *statusQtdPhys)
{
    UInt32 pSetup, pData = 0, pStatus;
    ehci_qtd *qSetup, *qData = 0, *qStatus;

    qSetup  = (ehci_qtd *)ehci_dma_alloc(&sc->pool, sizeof(ehci_qtd), 32, &pSetup);
    qStatus = (ehci_qtd *)ehci_dma_alloc(&sc->pool, sizeof(ehci_qtd), 32, &pStatus);
    if (!qSetup || !qStatus) return -1;
    if (dataLen) {
        qData = (ehci_qtd *)ehci_dma_alloc(&sc->pool, sizeof(ehci_qtd), 32, &pData);
        if (!qData) return -1;
    }

    /* STATUS: opposite direction to data (IN if data was OUT/none), toggle 1, IOC */
    qtd_fill(qStatus, (dirIn ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN) |
                      EHCI_QTD_TOGGLE | EHCI_QTD_IOC, 0, 0, 0);
    if (dataLen) {
        qtd_fill(qData, (dirIn ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) |
                        EHCI_QTD_TOGGLE, dataPhys, dataLen, pStatus);
        qtd_fill(qSetup, EHCI_QTD_PID_SETUP, setupPhys, 8, pData);
    } else {
        qtd_fill(qSetup, EHCI_QTD_PID_SETUP, setupPhys, 8, pStatus);
    }

    qh_activate(qh, pSetup);
    if (statusQtdPhys) *statusQtdPhys = pStatus;
    return 0;
}

/* Build and queue a single bulk transfer (one qTD; callers split >20 KB). */
long ehci_bulk_transfer(ehci_softc *sc, ehci_qh *qh, UInt32 bufPhys,
                        UInt32 len, int dirIn, UInt32 *qtdPhysOut)
{
    UInt32 pData;
    ehci_qtd *qData = (ehci_qtd *)ehci_dma_alloc(&sc->pool, sizeof(ehci_qtd), 32, &pData);
    if (!qData) return -1;
    /* bulk QH has DTC=0, so the QH tracks the toggle; leave qTD toggle clear */
    qtd_fill(qData, (dirIn ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) | EHCI_QTD_IOC,
             bufPhys, len, 0);
    qh_activate(qh, pData);
    if (qtdPhysOut) *qtdPhysOut = pData;
    return 0;
}

/* Poll a qTD's status. Returns: 1 = still active, 0 = done OK, <0 = error bits. */
long ehci_qtd_status(ehci_qtd *qtd)
{
    UInt32 tok = ehci_le32_to_cpu(qtd->token);
    if (tok & EHCI_QTD_STATUS_ACTIVE) return 1;
    if (tok & (EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_XACTERR |
               EHCI_QTD_STATUS_BABBLE | EHCI_QTD_STATUS_DBERR))
        return -(long)(tok & EHCI_QTD_STATUS_MASK);
    return 0;
}
