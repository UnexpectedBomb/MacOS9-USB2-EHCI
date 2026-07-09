/*
 * ehci_regs.h — EHCI (USB 2.0 Enhanced Host Controller Interface) register map.
 *
 * Per the Intel EHCI Specification for USB, rev 1.0. These definitions are
 * hardware/OS-independent.
 *
 * ENDIANNESS: EHCI is a PCI device — its memory-mapped registers AND all the
 * in-memory DMA structures it reads (frame list, queue heads, qTDs) are
 * LITTLE-ENDIAN. We run on big-endian PowerPC, so every 32-bit register and
 * DMA field must be byte-swapped. Use the LE helpers below (they compile to the
 * PPC lwbrx/stwbrx byte-reversed load/store instructions). This is precisely
 * why Apple's OHCI UIM imports EndianSwap32Bit throughout.
 */
#ifndef EHCI_REGS_H
#define EHCI_REGS_H

typedef unsigned char  UInt8;
typedef unsigned short UInt16;
typedef unsigned long  UInt32;

/* --- byte-reversed (little-endian) MMIO access on big-endian PPC --- */
static inline UInt8 ehci_read8(volatile void *base, UInt32 off)
{
    return *((volatile UInt8 *)base + off);   /* byte access needs no swap */
}
static inline UInt32 ehci_read32(volatile void *base, UInt32 off)
{
    volatile UInt32 *p = (volatile UInt32 *)((volatile UInt8 *)base + off);
    UInt32 v;
    __asm__ __volatile__("lwbrx %0,0,%1" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline void ehci_write32(volatile void *base, UInt32 off, UInt32 v)
{
    volatile UInt32 *p = (volatile UInt32 *)((volatile UInt8 *)base + off);
    __asm__ __volatile__("stwbrx %0,0,%1" : : "r"(v), "r"(p) : "memory");
    __asm__ __volatile__("eieio");   /* order the MMIO store */
}
/* Convert a host (big-endian) value to/from a little-endian DMA field. */
static inline UInt32 ehci_cpu_to_le32(UInt32 v)
{
    return ((v >> 24) & 0x000000FFUL) | ((v >> 8) & 0x0000FF00UL) |
           ((v << 8) & 0x00FF0000UL) | ((v << 24) & 0xFF000000UL);
}
#define ehci_le32_to_cpu(v) ehci_cpu_to_le32(v)

/* ================= Capability registers (at register base) ================= */
#define EHCI_CAPLENGTH      0x00   /* UInt8:  offset to operational regs        */
#define EHCI_HCIVERSION     0x02   /* UInt16: BCD interface version             */
#define EHCI_HCSPARAMS      0x04   /* structural params (see bits below)        */
#define EHCI_HCCPARAMS      0x08   /* capability params                         */
#define EHCI_HCSP_PORTROUTE 0x0C   /* companion port-route (optional)           */

/* HCSPARAMS fields */
#define EHCI_HCS_N_PORTS(p)      ((p) & 0x0F)          /* # of root ports       */
#define EHCI_HCS_PPC(p)          (((p) >> 4) & 1)      /* port power control    */
#define EHCI_HCS_N_CC(p)         (((p) >> 12) & 0x0F)  /* # companion controllers*/
#define EHCI_HCS_N_PCC(p)        (((p) >> 8) & 0x0F)   /* ports per companion   */

/* HCCPARAMS fields */
#define EHCI_HCC_64BIT(p)        ((p) & 1)             /* 64-bit addressing     */
#define EHCI_HCC_PROG_FLF(p)     (((p) >> 1) & 1)      /* programmable framelist*/
#define EHCI_HCC_EECP(p)         (((p) >> 8) & 0xFF)   /* EHCI extended cap ptr */

/* ============ Operational registers (at register base + CAPLENGTH) ========== */
#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_USBINTR        0x08
#define EHCI_FRINDEX        0x0C
#define EHCI_CTRLDSSEGMENT  0x10   /* high 32 bits of 64-bit addresses          */
#define EHCI_PERIODICLISTBASE 0x14
#define EHCI_ASYNCLISTADDR  0x18
#define EHCI_CONFIGFLAG     0x40
#define EHCI_PORTSC(n)      (0x44 + (n) * 4)   /* per-root-port status/control  */

/* USBCMD bits */
#define EHCI_CMD_RUN        0x00000001UL   /* Run(1)/Stop(0)                     */
#define EHCI_CMD_HCRESET    0x00000002UL   /* host controller reset             */
#define EHCI_CMD_PSE        0x00000010UL   /* periodic schedule enable          */
#define EHCI_CMD_ASE        0x00000020UL   /* async schedule enable             */
#define EHCI_CMD_IAAD       0x00000040UL   /* interrupt-on-async-advance doorbell*/
#define EHCI_CMD_ITC_SHIFT  16             /* interrupt threshold control        */
#define EHCI_CMD_FLS_SHIFT  2              /* frame list size (00=1024)          */

/* USBSTS / USBINTR bits (shared layout for the interrupt sources) */
#define EHCI_STS_USBINT     0x00000001UL   /* USB transfer completion            */
#define EHCI_STS_USBERRINT  0x00000002UL   /* USB error                          */
#define EHCI_STS_PCD        0x00000004UL   /* port change detect                 */
#define EHCI_STS_FLR        0x00000008UL   /* frame list rollover                */
#define EHCI_STS_HSE        0x00000010UL   /* host system error                  */
#define EHCI_STS_IAA        0x00000020UL   /* interrupt on async advance         */
#define EHCI_STS_HALTED     0x00001000UL   /* HCHalted (RO)                      */
#define EHCI_STS_ASS        0x00008000UL   /* async schedule status (RO)         */
#define EHCI_STS_PSS        0x00004000UL   /* periodic schedule status (RO)      */

/* CONFIGFLAG */
#define EHCI_CONFIGFLAG_CF  0x00000001UL   /* 1 => route all ports to EHCI       */

/* PORTSC bits */
#define EHCI_PORT_CONNECT   0x00000001UL
#define EHCI_PORT_CONNECT_CH 0x00000002UL
#define EHCI_PORT_ENABLE    0x00000004UL
#define EHCI_PORT_ENABLE_CH 0x00000008UL
#define EHCI_PORT_RESET     0x00000100UL
#define EHCI_PORT_POWER     0x00001000UL
#define EHCI_PORT_OWNER     0x00002000UL   /* 1 => port owned by companion (1.1) */
/* PORTSC write-1-to-clear change bits: CSC(bit1)|PEC(bit3)|OCC(bit5). Mask these OFF on
 * every read-modify-write so a control write doesn't inadvertently clear a pending change. */
#define EHCI_PORTSC_RW1C    0x0000002aUL

/* Periodic frame list: 1024 entries when FLS=00; 4 KB, 4 KB-aligned. */
#define EHCI_FRAMELIST_ENTRIES 1024
#define EHCI_FRAMELIST_BYTES   (EHCI_FRAMELIST_ENTRIES * 4)
#define EHCI_LINK_TERMINATE    0x00000001UL   /* T-bit: link pointer invalid     */

#endif /* EHCI_REGS_H */
