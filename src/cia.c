 /*
  * UAE - The Un*x Amiga Emulator
  *
  * CIA chip support
  *
  * Copyright 1995 Bernd Schmidt, Alessandro Bissacco
  * Copyright 1996, 1997 Stefan Reinauer, Christian Schmitt
  */

#include "sysconfig.h"
#include "sysdeps.h"
#include <assert.h>

#include "options.h"
#include "threaddep/thread.h"
#include "events.h"
#include "memory.h"
#include "custom.h"
#include "cia.h"
#include "serial.h"
#include "disk.h"
#include "xwin.h"
#include "inputdevice.h"
#include "keybuf.h"
#include "gui.h"
#include "savestate.h"
#include "audio.h"

#define DIV10 (5*CYCLE_UNIT) /* Yes, a bad identifier. */

/* battclock stuff */
#define RTC_D_ADJ      8
#define RTC_D_IRQ      4
#define RTC_D_BUSY     2
#define RTC_D_HOLD     1
#define RTC_E_t1       8
#define RTC_E_t0       4
#define RTC_E_INTR     2
#define RTC_E_MASK     1
#define RTC_F_TEST     8
#define RTC_F_24_12    4
#define RTC_F_STOP     2
#define RTC_F_RSET     1

static unsigned int clock_control_d = RTC_D_ADJ + RTC_D_HOLD;
static unsigned int clock_control_e = 0;
static unsigned int clock_control_f = RTC_F_24_12;

static unsigned int ciaaicr, ciaaimask, ciabicr, ciabimask;
static unsigned int ciaacra, ciaacrb, ciabcra, ciabcrb;

/* Values of the CIA timers.  */
static unsigned long ciaata, ciaatb, ciabta, ciabtb;
/* Computed by compute_passed_time.  */
static unsigned long ciaata_passed, ciaatb_passed, ciabta_passed, ciabtb_passed;

static unsigned long ciaatod, ciabtod, ciaatol, ciabtol, ciaaalarm, ciabalarm;
static int ciaatlatch, ciabtlatch;
static int oldled, oldovl;

static unsigned int ciabpra;

unsigned int gui_ledstate;

static unsigned long ciaala, ciaalb, ciabla, ciablb;
static int ciaatodon, ciabtodon;
static unsigned int ciaapra, ciaaprb, ciaadra, ciaadrb, ciaasdr;
static unsigned int ciabprb, ciabdra, ciabdrb, ciabsdr;
static int div10;
static int kbstate, kback, ciaasdr_unread = 0;

static int prtopen;
static FILE *prttmp;
static int warned = 10;

static void setclr (unsigned int *p, unsigned int val)
{
    if (val & 0x80) {
	*p |= val & 0x7F;
    } else {
	*p &= ~val;
    }
}

static void RethinkICRA (void)
{
    if (ciaaimask & ciaaicr) {
	ciaaicr |= 0x80;
	INTREQ_0 (0x8000 | 0x0008);
    } else {
	ciaaicr &= 0x7F;
/*	custom_bank.wput(0xDFF09C,0x0008);*/
    }
}

static void RethinkICRB (void)
{
    if (ciabimask & ciabicr) {
	ciabicr |= 0x80;
	INTREQ_0 (0x8000 | 0x2000);
    } else {
	ciabicr &= 0x7F;
    }
}

void rethink_cias (void)
{
    RethinkICRA ();
    RethinkICRB ();
}

/* Figure out how many CIA timer cycles have passed for each timer since the
   last call of CIA_calctimers.  */

static void compute_passed_time (void)
{
    unsigned long int ccount = (get_cycles () - eventtab[ev_cia].oldcycles + div10);
    unsigned long int ciaclocks = ccount / DIV10;

    ciaata_passed = ciaatb_passed = ciabta_passed = ciabtb_passed = 0;

    /* CIA A timers */
    if ((ciaacra & 0x21) == 0x01) {
	assert ((ciaata + 1) >= ciaclocks);
	ciaata_passed = ciaclocks;
    }
    if ((ciaacrb & 0x61) == 0x01) {
	assert ((ciaatb + 1) >= ciaclocks);
	ciaatb_passed = ciaclocks;
    }

    /* CIA B timers */
    if ((ciabcra & 0x21) == 0x01) {
	assert ((ciabta + 1) >= ciaclocks);
	ciabta_passed = ciaclocks;
    }
    if ((ciabcrb & 0x61) == 0x01) {
	assert ((ciabtb + 1) >= ciaclocks);
	ciabtb_passed = ciaclocks;
    }
}

/* Called to advance all CIA timers to the current time.  This expects that
   one of the timer values will be modified, and CIA_calctimers will be called
   in the same cycle.  */

static void CIA_update (void)
{
    unsigned long int ccount = (get_cycles () - eventtab[ev_cia].oldcycles + div10);
    unsigned long int ciaclocks = ccount / DIV10;

    int aovfla = 0, aovflb = 0, bovfla = 0, bovflb = 0;

    div10 = ccount % DIV10;

    /* CIA A timers */
    if ((ciaacra & 0x21) == 0x01) {
	assert ((ciaata + 1) >= ciaclocks);
	if ((ciaata + 1) == ciaclocks) {
	    aovfla = 1;
	    if ((ciaacrb & 0x61) == 0x41) {
		if (ciaatb-- == 0)
		    aovflb = 1;
	    }
	}
	ciaata -= ciaclocks;
    }
    if ((ciaacrb & 0x61) == 0x01) {
	assert ((ciaatb + 1) >= ciaclocks);
	if ((ciaatb + 1) == ciaclocks)
	    aovflb = 1;
	ciaatb -= ciaclocks;
    }

    /* CIA B timers */
    if ((ciabcra & 0x21) == 0x01) {
	assert ((ciabta + 1) >= ciaclocks);
	if ((ciabta + 1) == ciaclocks) {
	    bovfla = 1;
	    if ((ciabcrb & 0x61) == 0x41) {
		if (ciabtb-- == 0)
		    bovflb = 1;
	    }
	}
	ciabta -= ciaclocks;
    }
    if ((ciabcrb & 0x61) == 0x01) {
	assert ((ciabtb + 1) >= ciaclocks);
	if ((ciabtb + 1) == ciaclocks)
	    bovflb = 1;
	ciabtb -= ciaclocks;
    }
    if (aovfla) {
	ciaaicr |= 1; RethinkICRA ();
	ciaata = ciaala;
	if (ciaacra & 0x8) ciaacra &= ~1;
    }
    if (aovflb) {
	ciaaicr |= 2; RethinkICRA ();
	ciaatb = ciaalb;
	if (ciaacrb & 0x8) ciaacrb &= ~1;
    }
    if (bovfla) {
	ciabicr |= 1; RethinkICRB ();
	ciabta = ciabla;
	if (ciabcra & 0x8) ciabcra &= ~1;
    }
    if (bovflb) {
	ciabicr |= 2; RethinkICRB ();
	ciabtb = ciablb;
	if (ciabcrb & 0x8) ciabcrb &= ~1;
    }
}

/* Call this only after CIA_update has been called in the same cycle.  */

static void CIA_calctimers (void)
{
    unsigned long ciaatimea = ~0UL, ciaatimeb = ~0UL, ciabtimea = ~0UL, ciabtimeb = ~0UL;

    eventtab[ev_cia].oldcycles = get_cycles ();
    if ((ciaacra & 0x21) == 0x01) {
	ciaatimea = (DIV10 - div10) + DIV10 * ciaata;
    }
    if ((ciaacrb & 0x61) == 0x41) {
	/* Timer B will not get any pulses if Timer A is off. */
	if (ciaatimea != ~0UL) {
	    /* If Timer A is in one-shot mode, and Timer B needs more than
	     * one pulse, it will not underflow. */
	    if (ciaatb == 0 || (ciaacra & 0x8) == 0) {
		/* Otherwise, we can determine the time of the underflow. */
		/* This may overflow, however.  So just ignore this timer and
		   use the fact that we'll call CIA_handler for the A timer.  */
#if 0
		ciaatimeb = ciaatimea + ciaala * DIV10 * ciaatb;
#endif
	    }
	}
    }
    if ((ciaacrb & 0x61) == 0x01) {
	ciaatimeb = (DIV10 - div10) + DIV10 * ciaatb;
    }

    if ((ciabcra & 0x21) == 0x01) {
	ciabtimea = (DIV10 - div10) + DIV10 * ciabta;
    }
    if ((ciabcrb & 0x61) == 0x41) {
	/* Timer B will not get any pulses if Timer A is off. */
	if (ciabtimea != ~0UL) {
	    /* If Timer A is in one-shot mode, and Timer B needs more than
	     * one pulse, it will not underflow. */
	    if (ciabtb == 0 || (ciabcra & 0x8) == 0) {
		/* Otherwise, we can determine the time of the underflow. */
#if 0
		ciabtimeb = ciabtimea + ciabla * DIV10 * ciabtb;
#endif
	    }
	}
    }
    if ((ciabcrb & 0x61) == 0x01) {
	ciabtimeb = (DIV10 - div10) + DIV10 * ciabtb;
    }
    eventtab[ev_cia].active = (ciaatimea != ~0UL || ciaatimeb != ~0UL
			       || ciabtimea != ~0UL || ciabtimeb != ~0UL);
    if (eventtab[ev_cia].active) {
	unsigned long int ciatime = ~0UL;
	if (ciaatimea != ~0UL) ciatime = ciaatimea;
	if (ciaatimeb != ~0UL && ciaatimeb < ciatime) ciatime = ciaatimeb;
	if (ciabtimea != ~0UL && ciabtimea < ciatime) ciatime = ciabtimea;
	if (ciabtimeb != ~0UL && ciabtimeb < ciatime) ciatime = ciabtimeb;
	eventtab[ev_cia].evtime = ciatime + get_cycles ();
    }
    events_schedule();
}

void CIA_handler (void)
{
    CIA_update ();
    CIA_calctimers ();
}

void cia_diskindex (void)
{
    ciabicr |= 0x10;
    RethinkICRB ();
}

static int checkalarm (unsigned long tod, unsigned long alarm, int inc)
{
    if (tod == alarm)
	return 1;
    if (!inc)
	return 0;
    /* emulate buggy TODMED counter.
     * it counts: .. 29 2A 2B 2C 2D 2E 2F 20 30 31 32 ..
     * (2F->20->30 only takes couple of cycles but it will trigger alarm..
     */
    if (tod & 0x000fff)
	return 0;
    if (((tod - 1) & 0xfff000) == alarm)
	return 1;
    return 0;
}

STATIC_INLINE void ciab_checkalarm (int inc)
{
    if (checkalarm (ciabtod, ciabalarm, inc)) {
	ciabicr |= 4;
	RethinkICRB ();
    }
}

STATIC_INLINE void ciaa_checkalarm (int inc)
{
    if (checkalarm (ciaatod, ciaaalarm, inc)) {
	ciaaicr |= 4;
	RethinkICRA ();
    }
}

void CIA_hsync_handler (void)
{
    static unsigned int keytime = 0, sleepyhead = 0;

    if (ciabtodon) {
	ciabtod++;
	ciabtod &= 0xFFFFFF;
	ciab_checkalarm (1);
    }

    if (ciabtod == ciabalarm) {
	ciabicr |= 4; RethinkICRB ();
    }

    /* check wether the serial port gets some data */
    if (doreadser)
	doreadser = SERDATS();

    if (keys_available() && kback && (ciaacra & 0x40) == 0 && (++keytime & 15) == 0) {
	/*
	 * This hack lets one possible ciaaicr cycle go by without any key
	 * being read, for every cycle in which a key is pulled out of the
	 * queue.  If no hack is used, a lot of key events just get lost
	 * when you type fast.  With a simple hack that waits for ciaasdr
	 * to be read before feeding it another, it will keep up until the
	 * queue gets about 14 characters ahead and then lose events, and
	 * the mouse pointer will freeze while typing is being taken in.
	 * With this hack, you can type 30 or 40 characters ahead with little
	 * or no lossage, and the mouse doesn't get stuck.  The tradeoff is
	 * that the total slowness of typing appearing on screen is worse.
	 */
	if (ciaasdr_unread == 2) {
	    ciaasdr_unread = 0;
	} else if (ciaasdr_unread == 0) {
	    switch (kbstate) {
	     case 0:
		ciaasdr = (uae_s8)~0xFB; /* aaarghh... stupid compiler */
		kbstate++;
		break;
	     case 1:
		kbstate++;
		ciaasdr = (uae_s8)~0xFD;
		break;
	     case 2:
		ciaasdr = ~get_next_key();
		ciaasdr_unread = 1;      /* interlock to prevent lost keystrokes */
		break;
	    }
	    ciaaicr |= 8;
	    RethinkICRA ();
	    sleepyhead = 0;
	} else if (!(++sleepyhead & 15)) {
	    ciaasdr_unread = 0;          /* give up on this key event after unread for a long time */
	}
    }
}

void CIA_vsync_handler ()
{
    if (ciaatodon) {
	ciaatod++;
	ciaatod &= 0xFFFFFF;
	ciaa_checkalarm (1);
    }

    doreadser = 1;
    serstat = -1;
    serial_flush_buffer();
}

static void bfe001_change (void)
{
    uae_u8 v = ciaapra;

    v |= ~ciaadra; /* output is high when pin's direction is input */
    if ((v & 2) != oldled) {
	int led = (v & 2) ? 0 : 1;
	oldled = v & 2;
	gui_ledstate &= ~1;
	gui_ledstate |= led;
	gui_data.powerled = led;
	gui_led (0, led);
	led_filter_audio ();
    }
    if ((v & 1) != oldovl) {
	int i = (allocated_chipmem>>16) > 32 ? allocated_chipmem >> 16 : 32;
	oldovl = v & 1;

	if (!oldovl || ersatzkickfile) {
	    map_overlay (1);
	} else if (!(currprefs.chipset_mask & CSMASK_AGA)) {
	    /* pin disconnected in AGA chipset, CD audio mute on/off on CD32 */
	    map_overlay (0);
	}
    }
}

static uae_u8 ReadCIAA (unsigned int addr)
{
    unsigned int tmp;

    compute_passed_time ();

    switch (addr & 0xf) {
    case 0:
	if (currprefs.use_serial && (serstat < 0)) /* Only read status when needed */
	    serstat=serial_readstatus();		/* and only once per frame */

	tmp = (DISK_status() & 0x3C);
	tmp |= handle_joystick_buttons (ciaadra);
	tmp |= (ciaapra | (ciaadra ^ 3)) & 0x03;
	if (ciaadra & 0x40)
	    tmp = (tmp & ~0x40) | (ciaapra & 0x40);
	if (ciaadra & 0x80)
	    tmp = (tmp & ~0x80) | (ciaapra & 0x80);
	return tmp;
    case 1:
	/* Returning 0xFF is necessary for Tie Break - otherwise its joystick
	   code won't work.  */
	return prtopen ? ciaaprb : 0xFF;
    case 2:
	return ciaadra;
    case 3:
	return ciaadrb;
    case 4:
	return (ciaata - ciaata_passed) & 0xff;
    case 5:
	return (ciaata - ciaata_passed) >> 8;
    case 6:
	return (ciaatb - ciaatb_passed) & 0xff;
    case 7:
	return (ciaatb - ciaatb_passed) >> 8;
    case 8:
	if (ciaatlatch) {
	    ciaatlatch = 0;
	    return ciaatol & 0xff;
	} else
	    return ciaatod & 0xff;
    case 9:
	if (ciaatlatch)
	    return (ciaatol >> 8) & 0xff;
	else
	    return (ciaatod >> 8) & 0xff;
    case 10:
	ciaatlatch = 1;
	ciaatol = ciaatod; /* ??? only if not already latched? */
	return (ciaatol >> 16) & 0xff;
    case 12:
	if (ciaasdr_unread == 1)
	    ciaasdr_unread = 2;
	return ciaasdr;
    case 13:
	tmp = ciaaicr; ciaaicr = 0; RethinkICRA ();
	return tmp;
    case 14:
	return ciaacra;
    case 15:
	return ciaacrb;
    }
    return 0;
}

static uae_u8 ReadCIAB (unsigned int addr)
{
    unsigned int tmp;

    compute_passed_time ();

    switch (addr & 0xf) {
    case 0:
	if (currprefs.use_serial && serstat < 0) /* Only read status when needed */
	    serstat=serial_readstatus();	 /* and only once per frame      */
	/* Returning some 1 bits is necessary for Tie Break - otherwise its joystick
	   code won't work.  */
	return ciabpra | (prtopen ? 0 : 3);
    case 1:
	return ciabprb;
    case 2:
	return ciabdra;
    case 3:
	return ciabdrb;
    case 4:
	return (ciabta - ciabta_passed) & 0xff;
    case 5:
	return (ciabta - ciabta_passed) >> 8;
    case 6:
	return (ciabtb - ciabtb_passed) & 0xff;
    case 7:
	return (ciabtb - ciabtb_passed) >> 8;
    case 8:
	if (ciabtlatch) {
	    ciabtlatch = 0;
	    return ciabtol & 0xff;
	} else
	    return ciabtod & 0xff;
    case 9:
	if (ciabtlatch)
	    return (ciabtol >> 8) & 0xff;
	else
	    return (ciabtod >> 8) & 0xff;
    case 10:
	ciabtlatch = 1;
	ciabtol = ciabtod;
	return (ciabtol >> 16) & 0xff;
    case 12:
	return ciabsdr;
    case 13:
	tmp = ciabicr; ciabicr = 0; RethinkICRB ();
	return tmp;
    case 14:
	return ciabcra;
    case 15:
	return ciabcrb;
    }
    return 0;
}

static void WriteCIAA (uae_u16 addr, uae_u8 val)
{
    int oldled, oldovl;
    switch (addr & 0xf) {
    case 0:
 	ciaapra = (ciaapra & ~0x3) | (val & 0x3);
	bfe001_change ();
	break;
    case 1:
	ciaaprb = val;
	if (prtopen==1) {
#ifndef __DOS__
	    fprintf (prttmp, "%c",val);
#else
	    fputc (val, prttmp);
	    fflush (prttmp);
#endif
	    if (val==0x04) {
#if defined(__unix) && !defined(__BEOS__) && !defined(__DOS__)
		pclose (prttmp);
#else
		fclose (prttmp);
#endif
		prtopen = 0;
	    }
	} else {
#if defined(__unix) && !defined(__BEOS__) && !defined(__DOS__)
	    prttmp = (FILE *)popen ((const char *)currprefs.prtname, "w");
#else
	    prttmp = (FILE *)fopen ((const char *)currprefs.prtname, "wb");
#endif
	    if (prttmp != NULL) {
		prtopen = 1;
#ifndef __DOS__
		fprintf (prttmp,"%c",val);
#else
		fputc (val, prttmp);
		fflush (prttmp);
#endif
	    }
	}
	ciaaicr |= 0x10;
	break;
    case 2:
	ciaadra = val;
	bfe001_change ();
	break;
    case 3:
	ciaadrb = val;
	break;
    case 4:
	CIA_update ();
	ciaala = (ciaala & 0xff00) | val;
	CIA_calctimers ();
	break;
    case 5:
	CIA_update ();
	ciaala = (ciaala & 0xff) | (val << 8);
	if ((ciaacra & 1) == 0)
	    ciaata = ciaala;
	if (ciaacra & 8) {
	    ciaata = ciaala;
	    ciaacra |= 1;
	}
	CIA_calctimers ();
	break;
    case 6:
	CIA_update ();
	ciaalb = (ciaalb & 0xff00) | val;
	CIA_calctimers ();
	break;
    case 7:
	CIA_update ();
	ciaalb = (ciaalb & 0xff) | (val << 8);
	if ((ciaacrb & 1) == 0)
	    ciaatb = ciaalb;
	if (ciaacrb & 8) {
	    ciaatb = ciaalb;
	    ciaacrb |= 1;
	}
	CIA_calctimers ();
	break;
    case 8:
	if (ciaacrb & 0x80) {
	    ciaaalarm = (ciaaalarm & ~0xff) | val;
	} else {
	    ciaatod = (ciaatod & ~0xff) | val;
	    ciaatodon = 1;
	}
	break;
    case 9:
	if (ciaacrb & 0x80) {
	    ciaaalarm = (ciaaalarm & ~0xff00) | (val << 8);
	} else {
	    ciaatod = (ciaatod & ~0xff00) | (val << 8);
	    ciaatodon = 0;
	}
	break;
    case 10:
	if (ciaacrb & 0x80) {
	    ciaaalarm = (ciaaalarm & ~0xff0000) | (val << 16);
	} else {
	    ciaatod = (ciaatod & ~0xff0000) | (val << 16);
	    ciaatodon = 0;
	}
	break;
    case 12:
	ciaasdr = val;
	break;
    case 13:
	setclr(&ciaaimask,val);
	break;
    case 14:
	CIA_update ();
	ciaacra = val;
	if (ciaacra & 0x10) {
	    ciaacra &= ~0x10;
	    ciaata = ciaala;
	}
	if (ciaacra & 0x40)
	    kback = 1;
	CIA_calctimers ();
	break;
    case 15:
	CIA_update ();
	ciaacrb = val;
	if (ciaacrb & 0x10) {
	    ciaacrb &= ~0x10;
	    ciaatb = ciaalb;
	}
	CIA_calctimers ();
	break;
    }
}

static void WriteCIAB (uae_u16 addr, uae_u8 val)
{
    int oldval;
    switch (addr & 0xf) {
    case 0:
	if (currprefs.use_serial) {
	    oldval = ciabpra;
	    ciabpra = serial_writestatus (oldval, val);
	} else
	    ciabpra = val;
	break;
    case 1:
	ciabprb = val; DISK_select(val); break;
    case 2:
	ciabdra = val; break;
    case 3:
	ciabdrb = val; break;
    case 4:
	CIA_update ();
	ciabla = (ciabla & 0xff00) | val;
	CIA_calctimers ();
	break;
    case 5:
	CIA_update ();
	ciabla = (ciabla & 0xff) | (val << 8);
	if ((ciabcra & 1) == 0)
	    ciabta = ciabla;
	if (ciabcra & 8) {
	    ciabta = ciabla;
	    ciabcra |= 1;
	}
	CIA_calctimers ();
	break;
    case 6:
	CIA_update ();
	ciablb = (ciablb & 0xff00) | val;
	CIA_calctimers ();
	break;
    case 7:
	CIA_update ();
	ciablb = (ciablb & 0xff) | (val << 8);
	if ((ciabcrb & 1) == 0)
	    ciabtb = ciablb;
	if (ciabcrb & 8) {
	    ciabtb = ciablb;
	    ciabcrb |= 1;
	}
	CIA_calctimers ();
	break;
    case 8:
	if (ciabcrb & 0x80) {
	    ciabalarm = (ciabalarm & ~0xff) | val;
	} else {
	    ciabtod = (ciabtod & ~0xff) | val;
	    ciabtodon = 1;
	}
	break;
    case 9:
	if (ciabcrb & 0x80) {
	    ciabalarm = (ciabalarm & ~0xff00) | (val << 8);
	} else {
	    ciabtod = (ciabtod & ~0xff00) | (val << 8);
	    ciabtodon = 0;
	}
	break;
    case 10:
	if (ciabcrb & 0x80) {
	    ciabalarm = (ciabalarm & ~0xff0000) | (val << 16);
	} else {
	    ciabtod = (ciabtod & ~0xff0000) | (val << 16);
	    ciabtodon = 0;
	}
	break;
    case 12:
	ciabsdr = val;
	break;
    case 13:
	setclr(&ciabimask,val);
	break;
    case 14:
	CIA_update ();
	ciabcra = val;
	if (ciabcra & 0x10) {
	    ciabcra &= ~0x10;
	    ciabta = ciabla;
	}
	CIA_calctimers ();
	break;
    case 15:
	CIA_update ();
	ciabcrb = val;
	if (ciabcrb & 0x10) {
	    ciabcrb &= ~0x10;
	    ciabtb = ciablb;
	}
	CIA_calctimers ();
	break;
    }
}

void CIA_reset (void)
{
    kback = 1;
    kbstate = 0;
    oldovl = -1;
    oldled = -1;

    if (!savestate_state) {
	ciaatlatch = ciabtlatch = 0;
	ciaapra = 3;
	ciaatod = ciabtod = 0; ciaatodon = ciabtodon = 0;
	ciaaicr = ciabicr = ciaaimask = ciabimask = 0;
	ciaacra = ciaacrb = ciabcra = ciabcrb = 0x4; /* outmode = toggle; */
	ciaala = ciaalb = ciabla = ciablb = ciaata = ciaatb = ciabta = ciabtb = 0xFFFF;
	ciabpra = 0x8C;
	div10 = 0;
    }
    CIA_calctimers ();
    if (! ersatzkickfile) {
	int i = allocated_chipmem > 0x200000 ? allocated_chipmem >> 16 : 32;
	map_banks (&kickmem_bank, 0, i, 0x80000);
    }

    if (currprefs.use_serial && !savestate_state)
	serial_dtr_off (); /* Drop DTR at reset */

    if (savestate_state) {
	bfe001_change ();
	/* select drives */
	DISK_select (ciabprb);
    }
}

void dumpcia (void)
{
    printf("A: CRA: %02x, CRB: %02x, IMASK: %02x, TOD: %08lx %7s TA: %04lx (%04lx), TB: %04lx (%04lx)\n",
	   (int)ciaacra, (int)ciaacrb, (int)ciaaimask, ciaatod,
	   ciaatlatch ? "L" : "", ciaata, ciaala, ciaatb, ciaalb);
    printf("B: CRA: %02x, CRB: %02x, IMASK: %02x, TOD: %08lx %7s TA: %04lx (%04lx), TB: %04lx (%04lx)\n",
	   (int)ciabcra, (int)ciabcrb, (int)ciabimask, ciabtod,
	   ciabtlatch ? "L" : "", ciabta, ciabla, ciabtb, ciablb);
}

/* CIA memory access */

static uae_u32 cia_lget (uaecptr) REGPARAM;
static uae_u32 cia_wget (uaecptr) REGPARAM;
static uae_u32 cia_bget (uaecptr) REGPARAM;
static void cia_lput (uaecptr, uae_u32) REGPARAM;
static void cia_wput (uaecptr, uae_u32) REGPARAM;
static void cia_bput (uaecptr, uae_u32) REGPARAM;

addrbank cia_bank = {
    cia_lget, cia_wget, cia_bget,
    cia_lput, cia_wput, cia_bput,
    default_xlate, default_check, NULL, "CIA"
};

static void cia_wait (void)
{
    if (!div10)
	return;
    do_cycles (DIV10 - div10 + CYCLE_UNIT);
    CIA_handler ();
}

uae_u32 REGPARAM2 cia_bget (uaecptr addr)
{
    int r = (addr & 0xf00) >> 8;
    uae_u8 v;

    cia_wait ();
    v = 0xff;
    switch ((addr >> 12) & 3)
    {
    case 0:
	v = (addr & 1) ? ReadCIAA (r) : ReadCIAB (r);
	break;
    case 1:
	v = (addr & 1) ? 0xff : ReadCIAB (r);
	break;
    case 2:
	v = (addr & 1) ? ReadCIAA (r) : 0xff;
	break;
#if 0
    case 3:
	if (currprefs.cpu_level == 0 && currprefs.cpu_compatible)
	    v = (addr & 1) ? regs.irc : regs.irc >> 8;
	if (warned > 0) {
	    write_log ("cia_bget: unknown CIA address %x PC=%x\n", addr, m68k_getpc ());
	    warned--;
	}
	break;
#endif
    }
    return v;
}

uae_u32 REGPARAM2 cia_wget (uaecptr addr)
{
    int r = (addr & 0xf00) >> 8;
    uae_u16 v;
    cia_wait ();
    v = 0xffff;
    switch ((addr >> 12) & 3)
    {
    case 0:
	v = (ReadCIAB (r) << 8) | ReadCIAA (r);
	break;
    case 1:
	v = (ReadCIAB (r) << 8) | 0xff;
	break;
    case 2:
	v = (0xff << 8) | ReadCIAA (r);
	break;
    }
    return v;
}

uae_u32 REGPARAM2 cia_lget (uaecptr addr)
{
    uae_u32 v;
    v = cia_wget (addr) << 16;
    v |= cia_wget (addr + 2);
    return v;
}

void REGPARAM2 cia_bput (uaecptr addr, uae_u32 value)
{
    int r = (addr & 0xf00) >> 8;
    cia_wait ();
    if ((addr & 0x2000) == 0)
	WriteCIAB (r, value);
    if ((addr & 0x1000) == 0)
	WriteCIAA (r, value);
}

void REGPARAM2 cia_wput (uaecptr addr, uae_u32 value)
{
    int r = (addr & 0xf00) >> 8;
    cia_wait ();
    if ((addr & 0x2000) == 0)
	WriteCIAB (r, value >> 8);
    if ((addr & 0x1000) == 0)
	WriteCIAA (r, value & 0xff);
}

void REGPARAM2 cia_lput (uaecptr addr, uae_u32 value)
{
    cia_wput (addr, value >> 16);
    cia_wput (addr + 2, value & 0xffff);
}

/* battclock memory access */

static uae_u32 clock_lget (uaecptr) REGPARAM;
static uae_u32 clock_wget (uaecptr) REGPARAM;
static uae_u32 clock_bget (uaecptr) REGPARAM;
static void clock_lput (uaecptr, uae_u32) REGPARAM;
static void clock_wput (uaecptr, uae_u32) REGPARAM;
static void clock_bput (uaecptr, uae_u32) REGPARAM;

addrbank clock_bank = {
    clock_lget, clock_wget, clock_bget,
    clock_lput, clock_wput, clock_bput,
    default_xlate, default_check, NULL, "Battery backed up clock"
};

uae_u32 REGPARAM2 clock_lget (uaecptr addr)
{
    return clock_bget (addr + 3);
}

uae_u32 REGPARAM2 clock_wget (uaecptr addr)
{
    return clock_bget (addr + 1);
}

uae_u32 REGPARAM2 clock_bget (uaecptr addr)
{
    time_t t = time(0);
    struct tm *ct;

    ct = localtime (&t);

    switch (addr & 0x3f) {
    case 0x03: return ct->tm_sec % 10;
    case 0x07: return ct->tm_sec / 10;
    case 0x0b: return ct->tm_min % 10;
    case 0x0f: return ct->tm_min / 10;
    case 0x13: return ct->tm_hour % 10;
    case 0x17: return ct->tm_hour / 10;
    case 0x1b: return ct->tm_mday % 10;
    case 0x1f: return ct->tm_mday / 10;
    case 0x23: return (ct->tm_mon+1) % 10;
    case 0x27: return (ct->tm_mon+1) / 10;
    case 0x2b: return ct->tm_year % 10;
    case 0x2f: return ct->tm_year / 10;

    case 0x33: return ct->tm_wday;  /*Hack by -=SR=- */
    case 0x37: return clock_control_d;
    case 0x3b: return clock_control_e;
    case 0x3f: return clock_control_f;
    }
    return 0;
}

void REGPARAM2 clock_lput (uaecptr addr, uae_u32 value)
{
    /* No way */
}

void REGPARAM2 clock_wput (uaecptr addr, uae_u32 value)
{
    /* No way */
}

void REGPARAM2 clock_bput (uaecptr addr, uae_u32 value)
{
    switch (addr & 0x3f) {
    case 0x37: clock_control_d = value; break;
    case 0x3b: clock_control_e = value; break;
    case 0x3f: clock_control_f = value; break;
    }
}

/* CIA-A and CIA-B save/restore code */

const uae_u8 *restore_cia (int num, const uae_u8 *src)
{
    uae_u8 b;
    uae_u16 w;
    uae_u32 l;

    /* CIA registers */
    b = restore_u8 ();					/* 0 PRA */
    if (num) ciabpra = b; else ciaapra = b;
    b = restore_u8 ();					/* 1 PRB */
    if (num) ciabprb = b; else ciaaprb = b;
    b = restore_u8 ();					/* 2 DDRA */
    if (num) ciabdra = b; else ciaadra = b;
    b = restore_u8 ();					/* 3 DDRB */
    if (num) ciabdrb = b; else ciaadrb = b;
    w = restore_u16 ();					/* 4 TA */
    if (num) ciabta = w; else ciaata = w;
    w = restore_u16 ();					/* 6 TB */
    if (num) ciabtb = w; else ciaatb = w;
    l = restore_u8 ();					/* 8/9/A TOD */
    l |= restore_u8 () << 8;
    l |= restore_u8 () << 16;
    if (num) ciabtod = l; else ciaatod = l;
    restore_u8 ();						/* B unused */
    b = restore_u8 ();					/* C SDR */
    if (num) ciabsdr = b; else ciaasdr = b;
    b = restore_u8 ();					/* D ICR INFORMATION (not mask!) */
    if (num) ciabicr = b; else ciaaicr = b;
    b = restore_u8 ();					/* E CRA */
    if (num) ciabcra = b; else ciaacra = b;
    b = restore_u8 ();					/* F CRB */
    if (num) ciabcrb = b; else ciaacrb = b;

/* CIA internal data */

    b = restore_u8 ();					/* ICR MASK */
    if (num) ciabimask = b; else ciaaimask = b;
    w = restore_u8 ();					/* timer A latch */
    w |= restore_u8 () << 8;
    if (num) ciabla = w; else ciaala = w;
    w = restore_u8 ();					/* timer B latch */
    w |= restore_u8 () << 8;
    if (num) ciablb = w; else ciaalb = w;
    w = restore_u8 ();					/* TOD latched value */
    w |= restore_u8 () << 8;
    w |= restore_u8 () << 16;
    if (num) ciabtol = w; else ciaatol = w;
    l = restore_u8 ();					/* alarm */
    l |= restore_u8 () << 8;
    l |= restore_u8 () << 16;
    if (num) ciabalarm = l; else ciaaalarm = l;
    b = restore_u8 ();
    if (num) ciabtlatch = b & 1; else ciaatlatch = b & 1;	/* is TOD latched? */
    if (num) ciabtodon = b & 2; else ciaatodon = b & 2;		/* is TOD stopped? */
    if (num) {
	div10 = CYCLE_UNIT * restore_u8 ();
    }
    return src;
}

uae_u8 *save_cia (int num, int *len, uae_u8 *dstptr)
{
    uae_u8 *dstbak,*dst, b;
    uae_u16 t;

    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc (16 + 12 + 1);

    compute_passed_time ();

    /* CIA registers */

    b = num ? ciabpra : ciaapra;				/* 0 PRA */
    save_u8 (b);
    b = num ? ciabprb : ciaaprb;				/* 1 PRB */
    save_u8 (b);
    b = num ? ciabdra : ciaadra;				/* 2 DDRA */
    save_u8 (b);
    b = num ? ciabdrb : ciaadrb;				/* 3 DDRB */
    save_u8 (b);
    t = (num ? ciabta - ciabta_passed : ciaata - ciaata_passed);/* 4 TA */
    save_u16 (t);
    t = (num ? ciabtb - ciabtb_passed : ciaatb - ciaatb_passed);/* 8 TB */
    save_u16 (t);
    b = (num ? ciabtod : ciaatod);			/* 8 TODL */
    save_u8 (b);
    b = (num ? ciabtod >> 8 : ciaatod >> 8);		/* 9 TODM */
    save_u8 (b);
    b = (num ? ciabtod >> 16 : ciaatod >> 16);		/* A TODH */
    save_u8 (b);
    save_u8 (0);						/* B unused */
    b = num ? ciabsdr : ciaasdr;				/* C SDR */
    save_u8 (b);
    b = num ? ciabicr : ciaaicr;				/* D ICR INFORMATION (not mask!) */
    save_u8 (b);
    b = num ? ciabcra : ciaacra;				/* E CRA */
    save_u8 (b);
    b = num ? ciabcrb : ciaacrb;				/* F CRB */
    save_u8 (b);

    /* CIA internal data */

    save_u8 (num ? ciabimask : ciaaimask);			/* ICR */
    b = (num ? ciabla : ciaala);			/* timer A latch LO */
    save_u8 (b);
    b = (num ? ciabla >> 8 : ciaala >> 8);		/* timer A latch HI */
    save_u8 (b);
    b = (num ? ciablb : ciaalb);			/* timer B latch LO */
    save_u8 (b);
    b = (num ? ciablb >> 8 : ciaalb >> 8);		/* timer B latch HI */
    save_u8 (b);
    b = (num ? ciabtol : ciaatol);			/* latched TOD LO */
    save_u8 (b);
    b = (num ? ciabtol >> 8 : ciaatol >> 8);		/* latched TOD MED */
    save_u8 (b);
    b = (num ? ciabtol >> 16 : ciaatol >> 16);		/* latched TOD HI */
    save_u8 (b);
    b = (num ? ciabalarm : ciaaalarm);			/* alarm LO */
    save_u8 (b);
    b = (num ? ciabalarm >> 8 : ciaaalarm >> 8);	/* alarm MED */
    save_u8 (b);
    b = (num ? ciabalarm >> 16 : ciaaalarm >> 16);	/* alarm HI */
    save_u8 (b);
    b = 0;
    if (num)
	b |= ciabtlatch ? 1 : 0;
    else
	b |= ciaatlatch ? 1 : 0; /* is TOD latched? */
    if (num)
	b |= ciabtodon ? 2 : 0;
    else
	b |= ciaatodon ? 2 : 0;   /* TOD stopped? */
    save_u8 (b);
    if (num) {
	/* Save extra state with CIAB.  */
	save_u8 (div10 / CYCLE_UNIT);
    }
    *len = dst - dstbak;
    return dstbak;
}
