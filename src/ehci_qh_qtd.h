/*
 * ehci_qh_qtd.h — EHCI Queue Head (QH) and Queue Element Transfer Descriptor
 * (qTD) structures. Per the Intel EHCI Specification rev 1.0, sections 3.5-3.6.
 *
 * These live in DMA memory read by the controller, so ALL 32-bit fields are
 * little-endian: build them with ehci_cpu_to_le32() and read with
 * ehci_le32_to_cpu() (see ehci_regs.h). Both structures must be 32-byte aligned.
 */
#ifndef EHCI_QH_QTD_H
#define EHCI_QH_QTD_H
#include "ehci_regs.h"

/* Link pointer type field (bits 2:1) and terminate bit (bit 0). */
#define EHCI_LINK_TYP_ITD   (0u << 1)
#define EHCI_LINK_TYP_QH    (1u << 1)
#define EHCI_LINK_TYP_SITD  (2u << 1)
#define EHCI_LINK_TYP_FSTN  (3u << 1)
/* EHCI_LINK_TERMINATE (bit 0) is defined in ehci_regs.h */

/* ---- Queue Element Transfer Descriptor (qTD): 32 bytes, 32-byte aligned ---- */
typedef struct ehci_qtd {
    UInt32 next;            /* 0x00 next qTD pointer (| T)                       */
    UInt32 altNext;         /* 0x04 alternate next qTD pointer (| T)             */
    UInt32 token;           /* 0x08 status/PID/bytes/toggle (see EHCI_QTD_* )    */
    UInt32 buffer[5];       /* 0x0C..0x1C buffer pointers (page-based)           */
} ehci_qtd;                 /* = 32 bytes                                        */

/* qTD token fields */
#define EHCI_QTD_STATUS_ACTIVE   0x00000080u
#define EHCI_QTD_STATUS_HALTED   0x00000040u
#define EHCI_QTD_STATUS_DBERR    0x00000020u   /* data buffer error             */
#define EHCI_QTD_STATUS_BABBLE   0x00000010u
#define EHCI_QTD_STATUS_XACTERR  0x00000008u   /* transaction error             */
#define EHCI_QTD_STATUS_MISSED   0x00000004u   /* missed micro-frame            */
#define EHCI_QTD_STATUS_SPLITX   0x00000002u
#define EHCI_QTD_STATUS_PING     0x00000001u
#define EHCI_QTD_STATUS_MASK     0x000000FFu

#define EHCI_QTD_PID_OUT         (0u << 8)
#define EHCI_QTD_PID_IN          (1u << 8)
#define EHCI_QTD_PID_SETUP       (2u << 8)
#define EHCI_QTD_CERR(n)         (((n) & 3u) << 10)   /* error-retry counter     */
#define EHCI_QTD_CPAGE(n)        (((n) & 7u) << 12)
#define EHCI_QTD_IOC             0x00008000u          /* interrupt on complete   */
#define EHCI_QTD_BYTES(n)        (((n) & 0x7FFFu) << 16)  /* total bytes (<=20KB) */
#define EHCI_QTD_BYTES_GET(tok)  (((tok) >> 16) & 0x7FFFu)
#define EHCI_QTD_TOGGLE          0x80000000u          /* data toggle             */

/* ---- Queue Head (QH): 48 bytes, 32-byte aligned ---- */
typedef struct ehci_qh {
    UInt32 hlink;           /* 0x00 horizontal link ptr (| TYP | T)              */
    UInt32 epChar;          /* 0x04 endpoint characteristics (dword1)            */
    UInt32 epCaps;          /* 0x08 endpoint capabilities (dword2)               */
    UInt32 curQtd;          /* 0x0C current qTD pointer                          */
    /* transfer overlay (a working copy of the active qTD) */
    UInt32 ovlNext;         /* 0x10 next qTD pointer                             */
    UInt32 ovlAltNext;      /* 0x14 alternate next qTD pointer                   */
    UInt32 ovlToken;        /* 0x18 token                                        */
    UInt32 ovlBuffer[5];    /* 0x1C..0x2C buffer pointers                        */
} ehci_qh;                  /* = 48 bytes                                        */

/* epChar (dword1) fields */
#define EHCI_QH_DEVADDR(a)       ((a) & 0x7Fu)
#define EHCI_QH_INACTIVATE       0x00000080u
#define EHCI_QH_ENDPT(e)         (((e) & 0xFu) << 8)
#define EHCI_QH_EPS_FULL         (0u << 12)
#define EHCI_QH_EPS_LOW          (1u << 12)
#define EHCI_QH_EPS_HIGH         (2u << 12)
#define EHCI_QH_DTC              0x00004000u   /* data toggle from qTD           */
#define EHCI_QH_HEAD             0x00008000u   /* head of reclamation list (H)   */
#define EHCI_QH_MAXLEN(n)        (((n) & 0x7FFu) << 16)
#define EHCI_QH_CTRL_EP          0x08000000u   /* control endpoint (non-HS)      */
#define EHCI_QH_RL(n)            (((n) & 0xFu) << 28)   /* NAK count reload       */

/* epCaps (dword2) fields */
#define EHCI_QH_SMASK(m)         ((m) & 0xFFu)          /* interrupt S-mask       */
#define EHCI_QH_CMASK(m)         (((m) & 0xFFu) << 8)   /* split completion C-mask*/
#define EHCI_QH_HUBADDR(a)       (((a) & 0x7Fu) << 16)
#define EHCI_QH_PORTNUM(p)       (((p) & 0x7Fu) << 23)
#define EHCI_QH_MULT(m)          (((m) & 3u) << 30)     /* transactions/uframe    */

#endif /* EHCI_QH_QTD_H */
