 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Gayle (and motherboard resources) memory bank
  *
  * (c) 2006 - 2007 Toni Wilen
  */

#define GAYLE_LOG 0
#define IDE_LOG 0
#define MBRES_LOG 0

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"

#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "gayle.h"
#include "zfile.h"
#include "filesys.h"
#include "savestate.h"
#include "gui.h"

/*
D80000 to D8FFFF		64 KB SPARE chip select
D90000 to D9FFFF		64 KB ARCNET chip select
DA0000 to DA3FFF		16 KB IDE drive
DA4000 to DA4FFF		16 KB IDE reserved
DA8000 to DAFFFF		32 KB Credit Card and IDE configregisters
DB0000 to DBFFFF		64 KB Not used(reserved for external IDE)
* DC0000 to DCFFFF		64 KB Real Time Clock(RTC)
DD0000 to DDFFFF		64 KB A3000 DMA controller
DD0000 to DD1FFF                      A4000 DMAC
DD2000 to DDFFFF                      A4000 IDE
DE0000 to DEFFFF		64 KB Motherboard resources
*/

#define NCR_OFFSET 0x40

/* Gayle definitions from Linux drivers */

/* PCMCIA stuff */

#define GAYLE_RAM               (0x600000+zTwoBase)
#define GAYLE_RAMSIZE           (0x400000)
#define GAYLE_ATTRIBUTE         (0xa00000+zTwoBase)
#define GAYLE_ATTRIBUTESIZE     (0x020000)
#define GAYLE_IO                (0xa20000+zTwoBase)     /* 16bit and even 8bit registers */
#define GAYLE_IOSIZE            (0x010000)
#define GAYLE_IO_8BITODD        (0xa30000+zTwoBase)     /* odd 8bit registers */
/* offset for accessing odd IO registers */
#define GAYLE_ODD               (GAYLE_IO_8BITODD-GAYLE_IO-1)

#define GAYLE_ADDRESS   (0xda8000)      /* gayle main registers base address */
#define GAYLE_RESET     (0xa40000)      /* write 0x00 to start reset,
					   read 1 byte to stop reset */
/* GAYLE_CARDSTATUS bit def */
#define GAYLE_CS_CCDET          0x40    /* credit card detect */
#define GAYLE_CS_BVD1           0x20    /* battery voltage detect 1 */
#define GAYLE_CS_SC             0x20    /* credit card status change */
#define GAYLE_CS_BVD2           0x10    /* battery voltage detect 2 */
#define GAYLE_CS_DA             0x10    /* digital audio */
#define GAYLE_CS_WR             0x08    /* write enable (1 == enabled) */
#define GAYLE_CS_BSY            0x04    /* credit card busy */
#define GAYLE_CS_IRQ            0x04    /* interrupt request */

/* GAYLE_CONFIG bit def
   (bit 0-1 for program voltage, bit 2-3 for access speed */
#define GAYLE_CFG_0V            0x00
#define GAYLE_CFG_5V            0x01
#define GAYLE_CFG_12V           0x02

#define GAYLE_CFG_100NS         0x08
#define GAYLE_CFG_150NS         0x04
#define GAYLE_CFG_250NS         0x00
#define GAYLE_CFG_720NS         0x0c

/* IDE stuff */

/*  Bases of the IDE interfaces */
#define GAYLE_BASE_4000 0xdd2020    /* A4000/A4000T */
#define GAYLE_BASE_1200 0xda0000    /* A1200/A600 and E-Matrix 530 */
/* IDE drive registers */
#define IDE_DATA	0x00
#define IDE_ERROR	0x01	    /* see err-bits */
#define IDE_NSECTOR	0x02	    /* nr of sectors to read/write */
#define IDE_SECTOR	0x03	    /* starting sector */
#define IDE_LCYL	0x04	    /* starting cylinder */
#define IDE_HCYL	0x05	    /* high byte of starting cyl */
#define IDE_SELECT	0x06	    /* 101dhhhh , d=drive, hhhh=head */
#define IDE_STATUS	0x07	    /* see status-bits */
#define IDE_DEVCON	0x0406
#define IDE_DRVADDR	0x0407
/* STATUS bits */
#define IDE_STATUS_ERR 0x01
#define IDE_STATUS_IDX 0x02
#define IDE_STATUS_DRQ 0x08
#define IDE_STATUS_DSC 0x10
#define IDE_STATUS_DRDY 0x40
#define IDE_STATUS_BSY 0x80
/* ERROR bits */
#define IDE_ERR_ABRT 0x04

/*
 *  These are at different offsets from the base
 */
#define GAYLE_IRQ_4000  0x3020    /* MSB = 1, Harddisk is source of interrupt */
#define GAYLE_IRQ_1200  0x9000
#define GAYLE_INT_1200  0xA000

/* GAYLE_IRQ bit def */
#define GAYLE_IRQ_IDE	0x80
#define GAYLE_IRQ_CCDET	0x40
#define GAYLE_IRQ_BVD1	0x20
#define GAYLE_IRQ_SC	0x20
#define GAYLE_IRQ_BVD2	0x10
#define GAYLE_IRQ_DA	0x10
#define GAYLE_IRQ_WR	0x08
#define GAYLE_IRQ_BSY	0x04
#define GAYLE_IRQ_IRQ	0x04
#define GAYLE_IRQ_IDEACK1 0x02
#define GAYLE_IRQ_IDEACK0 0x01

struct ide_hdf
{
    uae_u8 secbuf[512 * 256];
    uae_u8 multiple_mode;
    uae_u8 status;
    int irq_delay;
    int num;
};

static struct ide_hdf idedrive[4];

static int gayle_id_cnt;
static uae_u8 gayle_irq, gayle_intena;
static uae_u8 ide_select, ide_nsector, ide_sector, ide_lcyl, ide_hcyl, ide_devcon, ide_error, ide_feat;
static uae_u8 ide_nsector2, ide_sector2, ide_lcyl2, ide_hcyl2, ide_feat2;
static int ide_drv, ide2, ide_splitter;

static struct ide_hdf *ide;

STATIC_INLINE void pw (int offset, uae_u16 w)
{
    ide->secbuf[offset * 2 + 0] = (uae_u8)w;
    ide->secbuf[offset * 2 + 1] = w >> 8;
}

static void ps (int offset, char *s, int max)
{
    int i, len;

    offset *= 2;
    len = strlen(s);
    for (i = 0; i < max; i++) {
	char c = ' ';
	if (i < len)
	    c = s[i];
	ide->secbuf[offset ^ 1] = c;
	offset++;
    }
}

static void ide_interrupt(void)
{
    ide->status |= IDE_STATUS_BSY;
    ide->irq_delay = 2;
}

static void ide_interrupt_do(struct ide_hdf *ide)
{
    ide->status &= ~IDE_STATUS_BSY;
    if (gayle_intena & GAYLE_IRQ_IDE) {
	gayle_irq |= GAYLE_IRQ_IDE;
	INTREQ (0x8000 | 0x0008);
    }
}

static void ide_fail(void)
{
    ide_error |= IDE_ERR_ABRT;
#if 0
    if (ide_drv == 1 && idedrive[ide2 + 1].hdhfd.size == 0)
	idedrive[ide2].status |= IDE_STATUS_ERR;
#endif
    ide->status |= IDE_STATUS_ERR;
    ide_interrupt();
}

static void ide_recalibrate(void)
{
    write_log ("IDE%d recalibrate\n", ide->num);
    ide_sector = 0;
    ide_lcyl = ide_hcyl = 0;
    ide_interrupt();
}

static void ide_identify_drive(void)
{
    int totalsecs;
    int v;

    ide_fail();
    return;
}

static void ide_initialize_drive_parameters(void)
{
    ide_error |= IDE_ERR_ABRT;
    ide->status |= IDE_STATUS_ERR;
    ide_interrupt();
}

static void ide_set_multiple_mode(void)
{
    write_log ("IDE%d drive multiple mode = %d\n", ide->num, ide_nsector);
    ide->multiple_mode = ide_nsector;
    ide_interrupt();
}

static void ide_set_features(void)
{
    write_log ("IDE%d set features %02.2X (%02.2X)\n", ide->num, ide_feat, ide_nsector);
    ide_fail();
}

static void ide_read_sectors(int multi)
{
    ide_fail();
}

static void ide_write_sectors(int multi)
{
    ide_fail();
}

static void ide_do_command(uae_u8 cmd)
{
    if (IDE_LOG > 1)
	write_log ("**** IDE%d command %02.2X\n", ide->num, cmd);
    ide->status &= ~(IDE_STATUS_DRDY | IDE_STATUS_DRQ | IDE_STATUS_ERR);
    ide_error = 0;

    if (cmd == 0x10) { /* recalibrate */
	ide_recalibrate();
    } else if (cmd == 0xec) { /* identify drive */
	ide_identify_drive();
    } else if (cmd == 0x91) { /* initialize drive parameters */
	ide_initialize_drive_parameters();
    } else if (cmd == 0xc6) { /* set multiple mode */
	ide_set_multiple_mode();
    } else if (cmd == 0x20 || cmd == 0x21) { /* read sectors */
	ide_read_sectors(0);
    } else if (cmd == 0x30 || cmd == 0x31) { /* write sectors */
	ide_write_sectors(0);
    } else if (cmd == 0xc4) { /* read multiple */
	ide_read_sectors(1);
    } else if (cmd == 0xc5) { /* write multiple */
	ide_write_sectors(1);
    } else if (cmd == 0x50) { /* format track (nop) */
	ide_interrupt();
    } else if (cmd == 0xa1) { /* ATAPI identify (IDE HD is not ATAPI) */
	ide_fail();
    } else if (cmd == 0xef) { /* set features  */
	ide_set_features();
    } else {
	ide_fail();
	write_log ("IDE: unknown command %x\n", cmd);
    }
}

static uae_u16 ide_get_data (void)
{
    if (IDE_LOG > 0)
	write_log ("IDE%d DATA read without DRQ!?\n", ide->num);
    return 0xffff;
}

static void ide_put_data (uae_u16 v)
{
    if (IDE_LOG > 0)
	write_log ("IDE%d DATA write without DRQ!?\n", ide->num);
}

static int get_ide_reg (uaecptr addr)
{
    uaecptr a = addr;
    addr &= 0xffff;
    if (addr >= 0x3020 && addr <= 0x3021 && currprefs.cs_ide == 2)
	return -1;
    addr &= ~0x2020;
    addr >>= 2;
    ide2 = 0;
    if (addr & 0x400) {
	if (ide_splitter) {
	    ide2 = 2;
	    addr &= ~0x400;
	}
    }
    ide = &idedrive[ide_drv + ide2];
    return addr;
}

static uae_u32 ide_read (uaecptr addr)
{
    int ide_reg;
    uae_u8 v = 0;

    addr &= 0xffff;
    if (IDE_LOG > 1 && addr != 0x2000 && addr != 0x2001 && addr != 0x2020 && addr != 0x2021 && addr != GAYLE_IRQ_1200)
	write_log ("IDE_READ %08.8X PC=%X\n", addr, m68k_getpc ());
    if (currprefs.cs_ide <= 0) {
	if (addr == 0x201c) // AR1200 IDE detection hack
	    return 0x7f;
	return 0xff;
    }
    if (addr >= GAYLE_IRQ_4000 && addr <= GAYLE_IRQ_4000 + 1 && currprefs.cs_ide == 2) {
	uae_u8 v = gayle_irq;
	gayle_irq = 0;
	return v;
    }
    if (addr >= 0x4000) {
	if (addr == GAYLE_IRQ_1200) {
	    if (currprefs.cs_ide == 1)
		return gayle_irq;
	    return 0;
	} else if (addr == GAYLE_INT_1200) {
	    if (currprefs.cs_ide == 1)
		return gayle_intena;
	    return 0;
	}
	return 0;
    }
    ide_reg = get_ide_reg(addr);
    /* Emulated "ide killer". Prevents long KS boot delay if no drives installed */
    if (ide_reg == IDE_STATUS)
	return 0x7f;
    return 0xff;

}

static void ide_write (uaecptr addr, uae_u32 val)
{
    int ide_reg;

    addr &= 0xffff;
    ide_devcon &= ~0x80;
    if (IDE_LOG > 1 && addr != 0x2000 && addr != 0x2001 && addr != 0x2020 && addr != 0x2021 && addr != GAYLE_IRQ_1200)
	write_log ("IDE_WRITE %08.8X=%02.2X PC=%X\n", addr, (uae_u32)val & 0xff, m68k_getpc ());
    if (currprefs.cs_ide <= 0)
	return;
    if (currprefs.cs_ide == 1) {
	if (addr == GAYLE_IRQ_1200) {
	    gayle_irq = 0;
	    return;
	}
	if (addr == GAYLE_INT_1200) {
	    gayle_intena = val;
	    return;
	}
    }
    if (addr >= 0x4000)
	return;
    ide_reg = get_ide_reg(addr);
    if (IDE_LOG > 2 && ide_reg > 0)
	write_log ("IDE%d register %d=%02.2X\n", ide->num, ide_reg, (uae_u32)val & 0xff);
    switch (ide_reg)
    {
	case IDE_DRVADDR:
	break;
	case IDE_DEVCON:
	    ide_devcon = val;
	break;
	case IDE_DATA:
	break;
	case IDE_ERROR:
	    ide_feat2 = ide_feat;
	    ide_feat = val;
	break;
	case IDE_NSECTOR:
	    ide_nsector2 = ide_nsector;
	    ide_nsector = val;
	break;
	case IDE_SECTOR:
	    ide_sector2 = ide_sector;
	    ide_sector = val;
	break;
	case IDE_LCYL:
	    ide_lcyl2 = ide_lcyl;
	    ide_lcyl = val;
	break;
	case IDE_HCYL:
	    ide_hcyl2 = ide_hcyl;
	    ide_hcyl = val;
	break;
	case IDE_SELECT:
	    ide_select = val;
	    ide_drv = (val & 0x10) ? 1 : 0;
	break;
	case IDE_STATUS:
	    ide_do_command (val);
	break;
    }
}

static int gayle_read (uaecptr addr)
{
    uaecptr oaddr = addr;
    uae_u32 v = 0;
#ifdef JIT
    special_mem |= S_READ;
#endif
    v = ide_read(addr);
    if (GAYLE_LOG)
	write_log ("GAYLE_READ %08.8X=%02.2X PC=%08.8X\n", oaddr, (uae_u32)v & 0xff, m68k_getpc ());
    return v;
}
static void gayle_write (uaecptr addr, int val)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    if (GAYLE_LOG)
	write_log ("GAYLE_WRITE %08.8X=%02.2X PC=%08.8X\n", addr, (uae_u32)val & 0xff, m68k_getpc ());
    ide_write(addr, val);
}

static uae_u32 gayle_lget (uaecptr) REGPARAM;
static uae_u32 gayle_wget (uaecptr) REGPARAM;
static uae_u32 gayle_bget (uaecptr) REGPARAM;
static void gayle_lput (uaecptr, uae_u32) REGPARAM;
static void gayle_wput (uaecptr, uae_u32) REGPARAM;
static void gayle_bput (uaecptr, uae_u32) REGPARAM;

addrbank gayle_bank = {
    gayle_lget, gayle_wget, gayle_bget,
    gayle_lput, gayle_wput, gayle_bput,
    default_xlate, default_check, NULL, "Gayle (low)"
};

#if 0
static int isa4000t (uaecptr addr)
{
    if (currprefs.cs_mbdmac != 2)
	return 0;
    if ((addr & 0xffff) >= (GAYLE_BASE_4000 & 0xffff))
	return 0;
    return 1;
}
#endif

static uae_u32 REGPARAM2 gayle_lget (uaecptr addr)
{
    uae_u32 v;
#ifdef JIT
    special_mem |= S_READ;
#endif
    v = gayle_wget (addr) << 16;
    v |= gayle_wget (addr + 2);
    return v;
}
static uae_u32 REGPARAM2 gayle_wget (uaecptr addr)
{
    int ide_reg;
    uae_u16 v;
#ifdef JIT
    special_mem |= S_READ;
#endif
#if 0
    if (isa4000t(addr)) {
	addr -= NCR_OFFSET;
	return (ncr_bget2 (addr) << 8) | ncr_bget2 (addr + 1);
    }
#endif
    ide_reg = get_ide_reg (addr);
    if (ide_reg == IDE_DATA)
	return ide_get_data ();
    v = gayle_bget (addr) << 8;
    v |= gayle_bget (addr + 1);
    return v;
}
static uae_u32 REGPARAM2 gayle_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= S_READ;
#endif
#if 0
    if (isa4000t(addr)) {
	addr -= NCR_OFFSET;
	return ncr_bget2(addr);
    }
#endif
    return gayle_read (addr);
}

static void REGPARAM2 gayle_lput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    gayle_wput (addr, value >> 16);
    gayle_wput (addr + 2, value & 0xffff);
}
static void REGPARAM2 gayle_wput (uaecptr addr, uae_u32 value)
{
    int ide_reg;
#ifdef JIT
    special_mem |= S_WRITE;
#endif
#if 0
    if (isa4000t (addr)) {
	addr -= NCR_OFFSET;
	ncr_bput2 (addr, value >> 8);
	ncr_bput2 (addr + 1, value);
	return;
    }
#endif
    ide_reg = get_ide_reg (addr);
    if (ide_reg == IDE_DATA) {
	ide_put_data(value);
	return;
    }
    gayle_bput (addr, value >> 8);
    gayle_bput (addr + 1, value & 0xff);
}

static void REGPARAM2 gayle_bput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
#if 0
    if (isa4000t(addr)) {
	addr -= NCR_OFFSET;
	ncr_bput2(addr, value);
	return;
    }
#endif
    gayle_write (addr, value);
}

static void gayle2_write(uaecptr addr, uae_u32 v)
{
    gayle_id_cnt = 0;
}

static uae_u32 gayle2_read(uaecptr addr)
{
    uae_u8 v = 0;
    addr &= 0xffff;
    if (addr == 0x1000) {
	/* Gayle ID */
	if ((gayle_id_cnt & 3) == 2)
	    v = 0x7f;
	else
	    v = 0x80;
	gayle_id_cnt++;
    }
    return v;
}

static uae_u32 gayle2_lget (uaecptr) REGPARAM;
static uae_u32 gayle2_wget (uaecptr) REGPARAM;
static uae_u32 gayle2_bget (uaecptr) REGPARAM;
static void gayle2_lput (uaecptr, uae_u32) REGPARAM;
static void gayle2_wput (uaecptr, uae_u32) REGPARAM;
static void gayle2_bput (uaecptr, uae_u32) REGPARAM;

addrbank gayle2_bank = {
    gayle2_lget, gayle2_wget, gayle2_bget,
    gayle2_lput, gayle2_wput, gayle2_bput,
    default_xlate, default_check, NULL, "Gayle (high)"
};

static uae_u32 REGPARAM2 gayle2_lget (uaecptr addr)
{
    uae_u32 v;
#ifdef JIT
    special_mem |= S_READ;
#endif
    v = gayle2_wget (addr) << 16;
    v |= gayle2_wget (addr + 2);
    return v;
}
static uae_u32 REGPARAM2 gayle2_wget (uaecptr addr)
{
    uae_u16 v;
#ifdef JIT
    special_mem |= S_READ;
#endif
    v = gayle2_bget (addr) << 8;
    v |= gayle2_bget (addr + 1);
    return v;
}
static uae_u32 REGPARAM2 gayle2_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= S_READ;
#endif
    return gayle2_read (addr);
}

static void REGPARAM2 gayle2_lput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    gayle2_wput (addr, value >> 16);
    gayle2_wput (addr + 2, value & 0xffff);
}

static void REGPARAM2 gayle2_wput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    gayle2_bput (addr, value >> 8);
    gayle2_bput (addr + 1, value & 0xff);
}

static void REGPARAM2 gayle2_bput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    gayle2_write (addr, value);
}

static uae_u8 ramsey_config;
static int gary_coldboot, gary_toenb, gary_timeout;
static int garyidoffset;

static void mbres_write (uaecptr addr, uae_u32 val, int size)
{
    addr &= 0xffff;

    if (MBRES_LOG > 0)
	write_log ("MBRES_WRITE %08X=%08X (%d) PC=%08X S=%d\n", addr, val, size, m68k_getpc (), regs.s);
    if (regs.s) { /* CPU FC = supervisor only */
	if (addr == 0x1002)
	    garyidoffset = -1;
	if (addr == 0x03)
	    ramsey_config = val;
	if (addr == 0x02)
	    gary_coldboot = (val & 0x80) ? 1 : 0;
	if (addr == 0x01)
	    gary_toenb = (val & 0x80) ? 1 : 0;
	if (addr == 0x00)
	    gary_timeout = (val & 0x80) ? 1 : 0;
    } else {
	custom_bank.wput (addr, val);
    }
}

static uae_u32 mbres_read (uaecptr addr, int size)
{
    uae_u32 v = 0;
    addr &= 0xffff;

    if (regs.s) { /* CPU FC = supervisor only */
	/* Gary ID (I don't think this exists in real chips..) */
	if (addr == 0x1002 && currprefs.cs_fatgaryrev >= 0) {
	    garyidoffset++;
	    garyidoffset &= 7;
	    v = (currprefs.cs_fatgaryrev << garyidoffset) & 0x80;
	}
	if (addr == 0x43) { /* RAMSEY revision */
	    if (currprefs.cs_ramseyrev >= 0)
		v = currprefs.cs_ramseyrev;
	}
	if (addr == 0x03) { /* RAMSEY config */
	    if (currprefs.cs_ramseyrev >= 0)
		v = ramsey_config;
	}
	if (addr == 0x02) { /* coldreboot flag */
	    if (currprefs.cs_fatgaryrev >= 0)
		v = gary_coldboot ? 0x80 : 0x00;
	}
	if (addr == 0x01) { /* toenb flag */
	    if (currprefs.cs_fatgaryrev >= 0)
		v = gary_toenb ? 0x80 : 0x00;
	}
	if (addr == 0x00) { /* timeout flag */
	    if (currprefs.cs_fatgaryrev >= 0)
		 v = gary_timeout ? 0x80 : 0x00;
	}
    } else {
	v = custom_bank.wget (addr);
    }
    if (MBRES_LOG > 0)
	write_log ("MBRES_READ %08X=%08X (%d) PC=%08X S=%d\n", addr, v, size, m68k_getpc (), regs.s);
    return v;
}

static uae_u32 mbres_lget (uaecptr) REGPARAM;
static uae_u32 mbres_wget (uaecptr) REGPARAM;
static uae_u32 mbres_bget (uaecptr) REGPARAM;
static void mbres_lput (uaecptr, uae_u32) REGPARAM;
static void mbres_wput (uaecptr, uae_u32) REGPARAM;
static void mbres_bput (uaecptr, uae_u32) REGPARAM;

static uae_u32 REGPARAM2 mbres_lget (uaecptr addr)
{
    uae_u32 v;
#ifdef JIT
    special_mem |= S_READ;
#endif
    v = mbres_wget (addr) << 16;
    v |= mbres_wget (addr + 2);
    return v;
}
static uae_u32 REGPARAM2 mbres_wget (uaecptr addr)
{
#ifdef JIT
    special_mem |= S_READ;
#endif
    return mbres_read (addr, 2);
}
static uae_u32 REGPARAM2 mbres_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= S_READ;
#endif
    return mbres_read (addr, 1);
}

static void REGPARAM2 mbres_lput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    mbres_wput (addr, value >> 16);
    mbres_wput (addr + 2, value & 0xffff);
}

static void REGPARAM2 mbres_wput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    mbres_write (addr, value, 2);
}

static void REGPARAM2 mbres_bput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    mbres_write (addr, value, 1);
}

addrbank mbres_bank = {
    mbres_lget, mbres_wget, mbres_bget,
    mbres_lput, mbres_wput, mbres_bput,
    default_xlate, default_check, NULL, "Motherboard Resources"
};

void gayle_hsync (void)
{
    int i;

    for (i = 0; i < 4; i++) {
	struct ide_hdf *ide = &idedrive[i];
	if (ide->irq_delay > 0) {
	    ide->irq_delay--;
	    if (ide->irq_delay == 0)
		ide_interrupt_do (ide);
	}
    }
}

static uae_u32 gayle_attr_read (uaecptr addr)
{
    uae_u8 v = 0;
    write_log ("R: %x %x\n", addr, m68k_getpc ());
    addr &= 262144 - 1;
    switch (addr)
    {
    case 0:
	v = 0x91;
	break;
    case 2:
	v = 0x05;
	break;
    case 4:
	v = 0x23;
	break;
    }
    return v;
}
static void gayle_attr_write (uaecptr addr, uae_u32 v)
{
    write_log ("W: %x=%x %x\n", addr, v, m68k_getpc ());
    addr &= 262144 - 1;
    if (addr == 0x40000) {
	if (v)
	    write_log ("GAYLE: Reset active\n");
	else
	    write_log ("GAYLE: Reset non-active\n");
    }
}

static uae_u32 gayle_attr_lget (uaecptr) REGPARAM;
static uae_u32 gayle_attr_wget (uaecptr) REGPARAM;
static uae_u32 gayle_attr_bget (uaecptr) REGPARAM;
static void gayle_attr_lput (uaecptr, uae_u32) REGPARAM;
static void gayle_attr_wput (uaecptr, uae_u32) REGPARAM;
static void gayle_attr_bput (uaecptr, uae_u32) REGPARAM;

addrbank gayle_attr_bank = {
    gayle_attr_lget, gayle_attr_wget, gayle_attr_bget,
    gayle_attr_lput, gayle_attr_wput, gayle_attr_bput,
    default_xlate, default_check, NULL, "Gayle PCMCIA attribute"
};

static uae_u32 REGPARAM2 gayle_attr_lget (uaecptr addr)
{
    uae_u32 v;
#ifdef JIT
    special_mem |= S_READ;
#endif
    v = gayle_attr_wget (addr) << 16;
    v |= gayle_attr_wget (addr + 2);
    return v;
}
static uae_u32 REGPARAM2 gayle_attr_wget (uaecptr addr)
{
    uae_u16 v;
#ifdef JIT
    special_mem |= S_READ;
#endif
    v = gayle_attr_bget (addr) << 8;
    v |= gayle_attr_bget (addr + 1);
    return v;
}
static uae_u32 REGPARAM2 gayle_attr_bget (uaecptr addr)
{
#ifdef JIT
    special_mem |= S_READ;
#endif
    return gayle_attr_read (addr);
}

static void REGPARAM2 gayle_attr_lput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    gayle_attr_wput (addr, value >> 16);
    gayle_attr_wput (addr + 2, value & 0xffff);
}

static void REGPARAM2 gayle_attr_wput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    gayle_attr_bput (addr, value >> 8);
    gayle_attr_bput (addr + 1, value & 0xff);
}

static void REGPARAM2 gayle_attr_bput (uaecptr addr, uae_u32 value)
{
#ifdef JIT
    special_mem |= S_WRITE;
#endif
    gayle_attr_write (addr, value);
}

static void initide (void)
{
    int i;

    if (savestate_state == STATE_RESTORE)
	return;
    ide_error = 1;
    ide_sector = ide_nsector = 1;
    ide_select = 0;
    ide_lcyl = ide_hcyl = ide_devcon = ide_feat = 0;
    ide_drv = 0;
    ide_splitter = 0;
    gayle_irq = gayle_intena = 0;
    if (currprefs.cs_ide == 2)
	gayle_intena = 0xff;

}

void gayle_reset (int hardreset)
{
    static char bankname[100];
    initide ();
    if (hardreset) {
	ramsey_config = 0;
	gary_coldboot = 1;
	gary_timeout = 0;
	gary_toenb = 0;
    }
    strcpy (bankname, "Gayle (low)");
    if (currprefs.cs_ide == 2)
	strcpy (bankname, "A4000 IDE");
#if 0
    if (currprefs.cs_mbdmac == 2) {
	strcat (bankname," + NCR53C710 SCSI");
	ncr_reset ();
    }
#endif
    gayle_bank.name = bankname;
}

const uae_u8 *restore_gayle (const uae_u8 *src)
{
    changed_prefs.cs_ide = restore_u8();
    gayle_intena = restore_u8();
    gayle_irq = restore_u8();
    ide_error = 0;
    return src;
}

uae_u8 *save_gayle (int *len)
{
    uae_u8 *dstbak, *dst;

    if (currprefs.cs_ide <= 0)
	return NULL;
    dstbak = dst = (uae_u8*)malloc (1000);
    save_u8 (currprefs.cs_ide);
    save_u8 (gayle_intena);
    save_u8 (gayle_irq);
    *len = dst - dstbak;
    return dstbak;
}
