 /*
  * UAE - The Un*x Amiga Emulator
  *
  * MC68000 emulation
  *
  * (c) 1995 Bernd Schmidt
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "events.h"
#include "uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "traps.h"
#include "autoconf.h"
#include "ersatz.h"
#include "debug.h"
#include "gui.h"
#include "savestate.h"
#include "blitter.h"

/* Opcode of faulting instruction */
static uae_u16 last_op_for_exception_3;
/* PC at fault time */
static uaecptr last_addr_for_exception_3;
/* Address that generated the exception */
static uaecptr last_fault_for_exception_3;
/* read (0) or write (1) access */
static int last_writeaccess_for_exception_3;
/* instruction (1) or data (0) access */
static int last_instructionaccess_for_exception_3;

const int areg_byteinc[] = { 1,1,1,1,1,1,1,2 };
const int imm8_table[] = { 8,1,2,3,4,5,6,7 };

int movem_index1[256];
int movem_index2[256];
int movem_next[256];

int fpp_movem_index1[256];
int fpp_movem_index2[256];
int fpp_movem_next[256];

cpuop_func *cpufunctbl[65536];

#define COUNT_INSTRS 0

#if COUNT_INSTRS
static unsigned long int instrcount[65536];
static uae_u16 opcodenums[65536];

static int compfn (const void *el1, const void *el2)
{
    return instrcount[*(const uae_u16 *)el1] < instrcount[*(const uae_u16 *)el2];
}

static char *icountfilename (void)
{
    char *name = getenv ("INSNCOUNT");
    if (name)
	return name;
    return COUNT_INSTRS == 2 ? "frequent.68k" : "insncount";
}

void dump_counts (void)
{
    FILE *f = fopen (icountfilename (), "w");
    unsigned long int total;
    int i;

    write_log ("Writing instruction count file...\n");
    for (i = 0; i < 65536; i++) {
	opcodenums[i] = i;
	total += instrcount[i];
    }
    qsort (opcodenums, 65536, sizeof(uae_u16), compfn);

    fprintf (f, "Total: %lu\n", total);
    for (i=0; i < 65536; i++) {
	unsigned long int cnt = instrcount[opcodenums[i]];
	struct instr *dp;
	struct mnemolookup *lookup;
	if (!cnt)
	    break;
	dp = table68k + opcodenums[i];
	for (lookup = lookuptab;lookup->mnemo != dp->mnemo; lookup++)
	    ;
	fprintf (f, "%04x: %lu %s\n", opcodenums[i], cnt, lookup->name);
    }
    fclose (f);
}
#else
void dump_counts (void)
{
}
#endif

int broken_in;

static unsigned long op_illg_1 (uae_u32 opcode) REGPARAM;

static unsigned long REGPARAM2 op_illg_1 (uae_u32 opcode)
{
    op_illg (opcode);
    return 4;
}

static void build_cpufunctbl (void)
{
    int i, opcnt;
    unsigned long opcode;
    const struct cputbl *tbl = 0;
    int lvl;

    switch (currprefs.cpu_model) {
    case 68060:
	lvl = 5;
	tbl = op_smalltbl_0_ff;
	break;
    case 68040:
	lvl = 4;
	tbl = op_smalltbl_1_ff;
	break;
    case 68030:
	lvl = 3;
	tbl = op_smalltbl_2_ff;
	break;
    case 68020:
	tbl = op_smalltbl_3_ff;
	lvl = 2;
	break;
    case 68010:
	tbl = op_smalltbl_4_ff;
	lvl = 1;
	break;
    case 68000:
	lvl = 0;
	tbl = op_smalltbl_6_ff;
	break;
    }

    if (tbl == 0) {
	write_log ("no CPU emulation cores available!");
	abort ();
    }

    for (opcode = 0; opcode < 65536; opcode++)
	cpufunctbl[opcode] = op_illg_1;
    for (i = 0; tbl[i].handler != NULL; i++) {
	if (! tbl[i].specific)
	    cpufunctbl[tbl[i].opcode] = tbl[i].handler;
    }

    opcnt = 0;
    for (opcode = 0; opcode < 65536; opcode++) {
	cpuop_func *f;

	if (table68k[opcode].mnemo == i_ILLG || table68k[opcode].clev > lvl)
	    continue;

	if (table68k[opcode].handler != -1) {
	    f = cpufunctbl[table68k[opcode].handler];
	    if (f == op_illg_1)
		abort();
	    cpufunctbl[opcode] = f;
	    opcnt++;
	}
    }
    for (i = 0; tbl[i].handler != NULL; i++) {
	if (tbl[i].specific)
	    cpufunctbl[tbl[i].opcode] = tbl[i].handler;
    }
    write_log ("Building CPU, %d opcodes (%d). CPU=%d, FPU=%d\n",
	       opcnt,
	       currprefs.address_space_24, currprefs.cpu_model, currprefs.fpu_model);
}

void fill_prefetch_slow (void)
{
#ifdef CPUEMU_6
    if (currprefs.cpu_cycle_exact) {
	regs.ir = get_word_ce (m68k_getpc ());
	regs.irc = get_word_ce (m68k_getpc () + 2);
    } else {
#endif
	regs.ir = get_word (m68k_getpc ());
	regs.irc = get_word (m68k_getpc () + 2);
#ifdef CPUEMU_6
    }
#endif
}

unsigned long cycles_mask, cycles_val;

static void update_68k_cycles (void)
{
    cycles_mask = 0;
    cycles_val = currprefs.m68k_speed;
    if (currprefs.m68k_speed < 1) {
	cycles_mask = 0xFFFFFFFF;
	cycles_val = 0;
    }
}

void check_prefs_changed_cpu (void)
{
    if (currprefs.m68k_speed != changed_prefs.m68k_speed) {
	currprefs.m68k_speed = changed_prefs.m68k_speed;
	reset_frame_rate_hack ();
	update_68k_cycles ();
    }
}

void init_m68k (void)
{
    int i;

    update_68k_cycles ();

    for (i = 0 ; i < 256 ; i++) {
	int j;
	for (j = 0 ; j < 8 ; j++) {
		if (i & (1 << j)) break;
	}
	movem_index1[i] = j;
	movem_index2[i] = 7-j;
	movem_next[i] = i & (~(1 << j));
    }
    for (i = 0 ; i < 256 ; i++) {
	int j;
	for (j = 7 ; j >= 0 ; j--) {
		if (i & (1 << j)) break;
	}
	fpp_movem_index1[i] = 7-j;
	fpp_movem_index2[i] = j;
	fpp_movem_next[i] = i & (~(1 << j));
    }
#if COUNT_INSTRS
    {
	FILE *f = fopen (icountfilename (), "r");
	memset (instrcount, 0, sizeof instrcount);
	if (f) {
	    uae_u32 opcode, count, total;
	    char name[20];
	    write_log ("Reading instruction count file...\n");
	    fscanf (f, "Total: %lu\n", &total);
	    while (fscanf (f, "%lx: %lu %s\n", &opcode, &count, name) == 3) {
		instrcount[opcode] = count;
	    }
	    fclose(f);
	}
    }
#endif
    write_log ("Building CPU table for configuration: 68");
    regs.address_space_mask = 0xffffffff;
    if (currprefs.address_space_24 && currprefs.cpu_model > 68010)
	write_log ("EC");
    write_log ("%03d", currprefs.cpu_model % 1000);
    if (currprefs.fpu_model == 68881 || currprefs.fpu_model == 68882)
	write_log ("/%d", currprefs.fpu_model % 1000);
    if (currprefs.address_space_24) {
	regs.address_space_mask = 0x00ffffff;
	write_log (" 24-bit addressing");
    }
    write_log ("\n");

    read_table68k ();
    do_merges ();

    write_log ("%d CPU functions\n", nr_cpuop_funcs);

    build_cpufunctbl ();
}

struct regstruct regs, lastint_regs;
static struct regstruct regs_backup[16];
static int backup_pointer = 0;
static long int m68kpc_offset;
int lastint_no;

#define get_ibyte_1(o) get_byte (regs.pc + (regs.pc_p - regs.pc_oldp) + (o) + 1)
#define get_iword_1(o) get_word (regs.pc + (regs.pc_p - regs.pc_oldp) + (o))
#define get_ilong_1(o) get_long (regs.pc + (regs.pc_p - regs.pc_oldp) + (o))

uae_s32 ShowEA (FILE *f, int reg, amodes mode, wordsizes size, char *buf)
{
    uae_u16 dp;
    uae_s8 disp8;
    uae_s16 disp16;
    int r;
    uae_u32 dispreg;
    uaecptr addr;
    uae_s32 offset = 0;
    char buffer[80];

    switch (mode){
     case Dreg:
	sprintf (buffer, "D%d", reg);
	break;
     case Areg:
	sprintf (buffer, "A%d", reg);
	break;
     case Aind:
	sprintf (buffer, "(A%d)", reg);
	break;
     case Aipi:
	sprintf (buffer, "(A%d)+", reg);
	break;
     case Apdi:
	sprintf (buffer, "-(A%d)", reg);
	break;
     case Ad16:
	disp16 = get_iword_1 (m68kpc_offset); m68kpc_offset += 2;
	addr = m68k_areg (regs,reg) + (uae_s16)disp16;
	sprintf (buffer, "(A%d,$%04x) == $%08lx", reg, disp16 & 0xffff,
					(unsigned long)addr);
	break;
     case Ad8r:
	dp = get_iword_1 (m68kpc_offset); m68kpc_offset += 2;
	disp8 = dp & 0xFF;
	r = (dp & 0x7000) >> 12;
	dispreg = dp & 0x8000 ? m68k_areg (regs,r) : m68k_dreg (regs,r);
	if (!(dp & 0x800)) dispreg = (uae_s32)(uae_s16)(dispreg);
	dispreg <<= (dp >> 9) & 3;

	if (dp & 0x100) {
	    uae_s32 outer = 0, disp = 0;
	    uae_s32 base = m68k_areg (regs,reg);
	    char name[10];
	    sprintf (name,"A%d, ",reg);
	    if (dp & 0x80) { base = 0; name[0] = 0; }
	    if (dp & 0x40) dispreg = 0;
	    if ((dp & 0x30) == 0x20) { disp = (uae_s32)(uae_s16)get_iword_1 (m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x30) == 0x30) { disp = get_ilong_1 (m68kpc_offset); m68kpc_offset += 4; }
	    base += disp;

	    if ((dp & 0x3) == 0x2) { outer = (uae_s32)(uae_s16)get_iword_1 (m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x3) == 0x3) { outer = get_ilong_1 (m68kpc_offset); m68kpc_offset += 4; }

	    if (!(dp & 4)) base += dispreg;
	    if (dp & 3) base = get_long (base);
	    if (dp & 4) base += dispreg;

	    addr = base + outer;
	    sprintf (buffer, "(%s%c%d.%c*%d+%ld)+%ld == $%08lx", name,
		    dp & 0x8000 ? 'A' : 'D', (int)r, dp & 0x800 ? 'L' : 'W',
		    1 << ((dp >> 9) & 3),
		    disp,outer,
		    (unsigned long)addr);
	} else {
	  addr = m68k_areg (regs,reg) + (uae_s32)((uae_s8)disp8) + dispreg;
	  sprintf (buffer, "(A%d, %c%d.%c*%d, $%02x) == $%08lx", reg,
	       dp & 0x8000 ? 'A' : 'D', (int)r, dp & 0x800 ? 'L' : 'W',
	       1 << ((dp >> 9) & 3), disp8,
	       (unsigned long)addr);
	}
	break;
     case PC16:
	addr = m68k_getpc () + m68kpc_offset;
	disp16 = get_iword_1 (m68kpc_offset); m68kpc_offset += 2;
	addr += (uae_s16)disp16;
	sprintf (buffer, "(PC,$%04x) == $%08lx", disp16 & 0xffff,(unsigned long)addr);
	break;
     case PC8r:
	addr = m68k_getpc () + m68kpc_offset;
	dp = get_iword_1 (m68kpc_offset); m68kpc_offset += 2;
	disp8 = dp & 0xFF;
	r = (dp & 0x7000) >> 12;
	dispreg = dp & 0x8000 ? m68k_areg (regs,r) : m68k_dreg (regs,r);
	if (!(dp & 0x800)) dispreg = (uae_s32)(uae_s16)(dispreg);
	dispreg <<= (dp >> 9) & 3;

	if (dp & 0x100) {
	    uae_s32 outer = 0,disp = 0;
	    uae_s32 base = addr;
	    char name[10];
	    sprintf (name,"PC, ");
	    if (dp & 0x80) { base = 0; name[0] = 0; }
	    if (dp & 0x40) dispreg = 0;
	    if ((dp & 0x30) == 0x20) { disp = (uae_s32)(uae_s16)get_iword_1 (m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x30) == 0x30) { disp = get_ilong_1 (m68kpc_offset); m68kpc_offset += 4; }
	    base += disp;

	    if ((dp & 0x3) == 0x2) { outer = (uae_s32)(uae_s16)get_iword_1 (m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x3) == 0x3) { outer = get_ilong_1 (m68kpc_offset); m68kpc_offset += 4; }

	    if (!(dp & 4)) base += dispreg;
	    if (dp & 3) base = get_long (base);
	    if (dp & 4) base += dispreg;

	    addr = base + outer;
	    sprintf (buffer, "(%s%c%d.%c*%d+%ld)+%ld == $%08lx", name,
		    dp & 0x8000 ? 'A' : 'D', (int)r, dp & 0x800 ? 'L' : 'W',
		    1 << ((dp >> 9) & 3),
		    disp,outer,
		    (unsigned long)addr);
	} else {
	  addr += (uae_s32)((uae_s8)disp8) + dispreg;
	  sprintf (buffer, "(PC, %c%d.%c*%d, $%02x) == $%08lx", dp & 0x8000 ? 'A' : 'D',
		(int)r, dp & 0x800 ? 'L' : 'W',  1 << ((dp >> 9) & 3),
		disp8, (unsigned long)addr);
	}
	break;
     case absw:
	sprintf (buffer, "$%08lx", (unsigned long)(uae_s32)(uae_s16)get_iword_1 (m68kpc_offset));
	m68kpc_offset += 2;
	break;
     case absl:
	sprintf (buffer, "$%08lx", (unsigned long)get_ilong_1 (m68kpc_offset));
	m68kpc_offset += 4;
	break;
     case imm:
	switch (size){
	 case sz_byte:
	    sprintf (buffer, "#$%02x", (unsigned int)(get_iword_1 (m68kpc_offset) & 0xff));
	    m68kpc_offset += 2;
	    break;
	 case sz_word:
	    sprintf (buffer, "#$%04x", (unsigned int)(get_iword_1 (m68kpc_offset) & 0xffff));
	    m68kpc_offset += 2;
	    break;
	 case sz_long:
	    sprintf (buffer, "#$%08lx", (unsigned long)(get_ilong_1 (m68kpc_offset)));
	    m68kpc_offset += 4;
	    break;
	 default:
	    break;
	}
	break;
     case imm0:
	offset = (uae_s32)(uae_s8)get_iword_1 (m68kpc_offset);
	m68kpc_offset += 2;
	sprintf (buffer, "#$%02x", (unsigned int)(offset & 0xff));
	break;
     case imm1:
	offset = (uae_s32)(uae_s16)get_iword_1 (m68kpc_offset);
	m68kpc_offset += 2;
	sprintf (buffer, "#$%04x", (unsigned int)(offset & 0xffff));
	break;
     case imm2:
	offset = (uae_s32)get_ilong_1 (m68kpc_offset);
	m68kpc_offset += 4;
	sprintf (buffer, "#$%08lx", (unsigned long)offset);
	break;
     case immi:
	offset = (uae_s32)(uae_s8)(reg & 0xff);
	sprintf (buffer, "#$%08lx", (unsigned long)offset);
	break;
     default:
	break;
    }
    if (buf == 0)
	fprintf (f, "%s", buffer);
    else
	strcat (buf, buffer);
    return offset;
}

/* The plan is that this will take over the job of exception 3 handling -
 * the CPU emulation functions will just do a longjmp to m68k_go whenever
 * they hit an odd address. */
static int verify_ea (int reg, amodes mode, wordsizes size, uae_u32 *val)
{
    uae_u16 dp;
    uae_s8 disp8;
    uae_s16 disp16;
    int r;
    uae_u32 dispreg;
    uaecptr addr;
    uae_s32 offset = 0;

    switch (mode){
     case Dreg:
	*val = m68k_dreg (regs, reg);
	return 1;
     case Areg:
	*val = m68k_areg (regs, reg);
	return 1;

     case Aind:
     case Aipi:
	addr = m68k_areg (regs, reg);
	break;
     case Apdi:
	addr = m68k_areg (regs, reg);
	break;
     case Ad16:
	disp16 = get_iword_1 (m68kpc_offset); m68kpc_offset += 2;
	addr = m68k_areg (regs,reg) + (uae_s16)disp16;
	break;
     case Ad8r:
	addr = m68k_areg (regs, reg);
     d8r_common:
	dp = get_iword_1 (m68kpc_offset); m68kpc_offset += 2;
	disp8 = dp & 0xFF;
	r = (dp & 0x7000) >> 12;
	dispreg = dp & 0x8000 ? m68k_areg (regs,r) : m68k_dreg (regs,r);
	if (!(dp & 0x800)) dispreg = (uae_s32)(uae_s16)(dispreg);
	dispreg <<= (dp >> 9) & 3;

	if (dp & 0x100) {
	    uae_s32 outer = 0, disp = 0;
	    uae_s32 base = addr;
	    if (dp & 0x80) base = 0;
	    if (dp & 0x40) dispreg = 0;
	    if ((dp & 0x30) == 0x20) { disp = (uae_s32)(uae_s16)get_iword_1 (m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x30) == 0x30) { disp = get_ilong_1 (m68kpc_offset); m68kpc_offset += 4; }
	    base += disp;

	    if ((dp & 0x3) == 0x2) { outer = (uae_s32)(uae_s16)get_iword_1 (m68kpc_offset); m68kpc_offset += 2; }
	    if ((dp & 0x3) == 0x3) { outer = get_ilong_1 (m68kpc_offset); m68kpc_offset += 4; }

	    if (!(dp & 4)) base += dispreg;
	    if (dp & 3) base = get_long (base);
	    if (dp & 4) base += dispreg;

	    addr = base + outer;
	} else {
	  addr += (uae_s32)((uae_s8)disp8) + dispreg;
	}
	break;
     case PC16:
	addr = m68k_getpc () + m68kpc_offset;
	disp16 = get_iword_1 (m68kpc_offset); m68kpc_offset += 2;
	addr += (uae_s16)disp16;
	break;
     case PC8r:
	addr = m68k_getpc () + m68kpc_offset;
	goto d8r_common;
     case absw:
	addr = (uae_s32)(uae_s16)get_iword_1 (m68kpc_offset);
	m68kpc_offset += 2;
	break;
     case absl:
	addr = get_ilong_1 (m68kpc_offset);
	m68kpc_offset += 4;
	break;
     case imm:
	switch (size){
	 case sz_byte:
	    *val = get_iword_1 (m68kpc_offset) & 0xff;
	    m68kpc_offset += 2;
	    break;
	 case sz_word:
	    *val = get_iword_1 (m68kpc_offset) & 0xffff;
	    m68kpc_offset += 2;
	    break;
	 case sz_long:
	    *val = get_ilong_1 (m68kpc_offset);
	    m68kpc_offset += 4;
	    break;
	 default:
	    break;
	}
	return 1;
     case imm0:
	*val = (uae_s32)(uae_s8)get_iword_1 (m68kpc_offset);
	m68kpc_offset += 2;
	return 1;
     case imm1:
	*val = (uae_s32)(uae_s16)get_iword_1 (m68kpc_offset);
	m68kpc_offset += 2;
	return 1;
     case imm2:
	*val = get_ilong_1 (m68kpc_offset);
	m68kpc_offset += 4;
	return 1;
     case immi:
	*val = (uae_s32)(uae_s8)(reg & 0xff);
	return 1;
     default:
	addr = 0;
	break;
    }
    if ((addr & 1) == 0)
	return 1;

    last_addr_for_exception_3 = m68k_getpc () + m68kpc_offset;
    last_fault_for_exception_3 = addr;
    last_writeaccess_for_exception_3 = 0;
    last_instructionaccess_for_exception_3 = 0;
    return 0;
}

uae_u32 get_disp_ea_020 (uae_u32 base, uae_u32 dp)
{
    int reg = (dp >> 12) & 15;
    uae_s32 regd = regs.regs[reg];
    if ((dp & 0x800) == 0)
	regd = (uae_s32)(uae_s16)regd;
    regd <<= (dp >> 9) & 3;
    if (dp & 0x100) {
	uae_s32 outer = 0;
	if (dp & 0x80) base = 0;
	if (dp & 0x40) regd = 0;

	if ((dp & 0x30) == 0x20) base += (uae_s32)(uae_s16)next_iword();
	if ((dp & 0x30) == 0x30) base += next_ilong();

	if ((dp & 0x3) == 0x2) outer = (uae_s32)(uae_s16)next_iword();
	if ((dp & 0x3) == 0x3) outer = next_ilong();

	if ((dp & 0x4) == 0) base += regd;
	if (dp & 0x3) base = get_long (base);
	if (dp & 0x4) base += regd;

	return base + outer;
    } else {
	return base + (uae_s32)((uae_s8)dp) + regd;
    }
}

uae_u32 get_disp_ea_000 (uae_u32 base, uae_u32 dp)
{
    int reg = (dp >> 12) & 15;
    uae_s32 regd = regs.regs[reg];
#if 1
    if ((dp & 0x800) == 0)
	regd = (uae_s32)(uae_s16)regd;
    return base + (uae_s8)dp + regd;
#else
    /* Branch-free code... benchmark this again now that
     * things are no longer inline.  */
    uae_s32 regd16;
    uae_u32 mask;
    mask = ((dp & 0x800) >> 11) - 1;
    regd16 = (uae_s32)(uae_s16)regd;
    regd16 &= mask;
    mask = ~mask;
    base += (uae_s8)dp;
    regd &= mask;
    regd |= regd16;
    return base + regd;
#endif
}

void MakeSR (void)
{
#if 0
    assert((regs.t1 & 1) == regs.t1);
    assert((regs.t0 & 1) == regs.t0);
    assert((regs.s & 1) == regs.s);
    assert((regs.m & 1) == regs.m);
    assert((XFLG & 1) == XFLG);
    assert((NFLG & 1) == NFLG);
    assert((ZFLG & 1) == ZFLG);
    assert((VFLG & 1) == VFLG);
    assert((CFLG & 1) == CFLG);
#endif
    regs.sr = ((regs.t1 << 15) | (regs.t0 << 14)
	       | (regs.s << 13) | (regs.m << 12) | (regs.intmask << 8)
	       | (GET_XFLG << 4) | (GET_NFLG << 3) | (GET_ZFLG << 2) | (GET_VFLG << 1)
	       | GET_CFLG);
}

void MakeFromSR (void)
{
    int oldm = regs.m;
    int olds = regs.s;

    regs.t1 = (regs.sr >> 15) & 1;
    regs.t0 = (regs.sr >> 14) & 1;
    regs.s = (regs.sr >> 13) & 1;
    regs.m = (regs.sr >> 12) & 1;
    regs.intmask = (regs.sr >> 8) & 7;
    SET_XFLG ((regs.sr >> 4) & 1);
    SET_NFLG ((regs.sr >> 3) & 1);
    SET_ZFLG ((regs.sr >> 2) & 1);
    SET_VFLG ((regs.sr >> 1) & 1);
    SET_CFLG (regs.sr & 1);
    if (currprefs.cpu_model >= 68020) {
	if (olds != regs.s) {
	    if (olds) {
		if (oldm)
		    regs.msp = m68k_areg (regs, 7);
		else
		    regs.isp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs.usp;
	    } else {
		regs.usp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs.m ? regs.msp : regs.isp;
	    }
	} else if (olds && oldm != regs.m) {
	    if (oldm) {
		regs.msp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs.isp;
	    } else {
		regs.isp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs.msp;
	    }
	}
    } else {
	if (olds != regs.s) {
	    if (olds) {
		regs.isp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs.usp;
	    } else {
		regs.usp = m68k_areg (regs, 7);
		m68k_areg (regs, 7) = regs.isp;
	    }
	}
    }

    set_special (SPCFLAG_INT);
    if (regs.t1 || regs.t0)
	set_special (SPCFLAG_TRACE);
    else
	/* Keep SPCFLAG_DOTRACE, we still want a trace exception for
	   SR-modifying instructions (including STOP).  */
	unset_special (SPCFLAG_TRACE);
}

static void exception_trace (int nr)
{
    unset_special (SPCFLAG_TRACE | SPCFLAG_DOTRACE);
    if (regs.t1 && !regs.t0) {
	/* trace stays pending if exception is div by zero, chk,
	 * trapv or trap #x
	 */
	if (nr == 5 || nr == 6 || nr ==  7 || (nr >= 32 && nr <= 47))
	    set_special (SPCFLAG_DOTRACE);
    }
    regs.t1 = regs.t0 = regs.m = 0;
}

static void exception_debug (int nr)
{
#ifdef DEBUGGER
    if (!exception_debugging)
	return;
    console_out ("Exception %d, PC=%08.8X\n", nr, m68k_getpc ());
#endif
}

void Exception_normal (int nr, uaecptr oldpc)
{
    uae_u32 currpc = m68k_getpc (), newpc;
    int sv = regs.s;

    exception_debug (nr);
    MakeSR ();

    if (!regs.s) {
	regs.usp = m68k_areg (regs, 7);
	if (currprefs.cpu_model >= 68020)
	    m68k_areg (regs, 7) = regs.m ? regs.msp : regs.isp;
	else
	    m68k_areg (regs, 7) = regs.isp;
	regs.s = 1;
    }
    if (currprefs.cpu_model > 68000) {
	if (nr == 2 || nr == 3) {
	    int i;
	    if (currprefs.cpu_model >= 68040) {
		if (nr == 2) {
		    for (i = 0 ; i < 18 ; i++) {
			m68k_areg (regs, 7) -= 2;
			put_word (m68k_areg (regs, 7), 0);
		    }
		    m68k_areg (regs, 7) -= 4;
		    put_long (m68k_areg (regs, 7), last_fault_for_exception_3);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0x0140 | (sv ? 6 : 2)); /* SSW */
		    m68k_areg (regs, 7) -= 4;
		    put_long (m68k_areg (regs, 7), last_addr_for_exception_3);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0x7000 + nr * 4);
		} else {
		    m68k_areg (regs, 7) -= 4;
		    put_long (m68k_areg (regs, 7), last_fault_for_exception_3);
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0x2000 + nr * 4);
		}
	    } else {
		uae_u16 ssw = (sv ? 4 : 0) | (last_instructionaccess_for_exception_3 ? 2 : 1);
		ssw |= last_writeaccess_for_exception_3 ? 0 : 0x40;
		ssw |= 0x20;
		for (i = 0 ; i < 36; i++) {
		    m68k_areg (regs, 7) -= 2;
		    put_word (m68k_areg (regs, 7), 0);
		}
		m68k_areg (regs, 7) -= 4;
		put_long (m68k_areg (regs, 7), last_fault_for_exception_3);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), 0);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), 0);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), 0);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), ssw);
		m68k_areg (regs, 7) -= 2;
		put_word (m68k_areg (regs, 7), 0xb000 + nr * 4);
	    }
#if 0
	    write_log ("Exception %d (%x) at %x -> %x!\n", nr, oldpc, currpc, get_long (regs.vbr + 4*nr));
#endif
	    
	} else if (nr ==5 || nr == 6 || nr == 7 || nr == 9) {
	    m68k_areg (regs, 7) -= 4;
	    put_long (m68k_areg (regs, 7), oldpc);
	    m68k_areg (regs, 7) -= 2;
	    put_word (m68k_areg (regs, 7), 0x2000 + nr * 4);
	} else if (regs.m && nr >= 24 && nr < 32) { /* M + Interrupt */
	    m68k_areg (regs, 7) -= 2;
	    put_word (m68k_areg (regs, 7), nr * 4);
	    m68k_areg (regs, 7) -= 4;
	    put_long (m68k_areg (regs, 7), currpc);
	    m68k_areg (regs, 7) -= 2;
	    put_word (m68k_areg (regs, 7), regs.sr);
	    regs.sr |= (1 << 13);
	    regs.msp = m68k_areg (regs, 7);
	    m68k_areg (regs, 7) = regs.isp;
	    m68k_areg (regs, 7) -= 2;
	    put_word (m68k_areg (regs, 7), 0x1000 + nr * 4);
	} else {
	    m68k_areg (regs, 7) -= 2;
	    put_word (m68k_areg (regs, 7), nr * 4);
	}
    } else if (nr == 2 || nr == 3) {
	uae_u16 mode = (sv ? 4 : 0) | (last_instructionaccess_for_exception_3 ? 2 : 1);
	mode |= last_writeaccess_for_exception_3 ? 0 : 16;
	m68k_areg (regs, 7) -= 14;
	/* fixme: bit3=I/N */
	put_word (m68k_areg (regs, 7) + 0, mode);
	put_long (m68k_areg (regs, 7) + 2, last_fault_for_exception_3);
	put_word (m68k_areg (regs, 7) + 6, last_op_for_exception_3);
	put_word (m68k_areg (regs, 7) + 8, regs.sr);
	put_long (m68k_areg (regs, 7) + 10, last_addr_for_exception_3);
	write_log ("Exception %d (%x) at %x -> %x!\n", nr, oldpc, currpc, get_long (regs.vbr + 4*nr));
	goto kludge_me_do;
    }
    m68k_areg (regs, 7) -= 4;
    put_long (m68k_areg (regs, 7), currpc);
    m68k_areg (regs, 7) -= 2;
    put_word (m68k_areg (regs, 7), regs.sr);
kludge_me_do:
    newpc = get_long (regs.vbr + 4 * nr);
    if (newpc & 1) {
	if (nr == 2 || nr == 3) {
	    uae_reset (1); /* there is nothing else we can do.. */
	    set_special (SPCFLAG_RESTORE_SANITY);
	} else
	    exception3 (regs.ir, m68k_getpc (), newpc);
	return;
    }
    m68k_setpc (newpc);
    fill_prefetch_slow ();
    exception_trace (nr);
}

void Exception (int nr, uaecptr oldpc)
{
    Exception_normal (nr, oldpc);
}

static void Interrupt (int nr)
{
    regs.stopped = 0;
    unset_special (SPCFLAG_STOP);
    assert(nr < 8 && nr >= 0);
    lastint_regs = regs;
    lastint_no = nr;
    Exception (nr + 24, 0);

    regs.intmask = nr;
    set_special (SPCFLAG_INT);
}

static int movec_illg (int regno)
{
    int regno2 = regno & 0x7ff;

    if (currprefs.cpu_model == 68060) {
	if (regno <= 8)
	    return 0;
	if (regno == 0x800 || regno == 0x801 ||
	    regno == 0x806 || regno == 0x807 || regno == 0x808)
	    return 0;
	return 1;
    } else if (currprefs.cpu_model == 68010) {
	if (regno2 < 2)
	    return 0;
	return 1;
    } else if (currprefs.cpu_model == 68020) {
	if (regno == 3)
	    /* 68040/060 only */
	    return 1;
	 /* 4 is >=68040, but 0x804 is in 68020 */
	 if (regno2 < 4 || regno == 0x804)
	    return 0;
	return 1;
    } else if (currprefs.cpu_model == 68030) {
	if (regno2 <= 2)
	    return 0;
	if (regno == 0x803 || regno == 0x804)
	    return 0;
	return 1;
    } else if (currprefs.cpu_model == 68040) {
	if (regno == 0x802)
	    /* 68020 only */
	    return 1;
	if (regno2 < 8) return 0;
	return 1;
    }
    return 1;
}

int m68k_move2c (int regno, uae_u32 *regp)
{
    if (movec_illg (regno)) {
	op_illg (0x4E7B);
	return 0;
    } else {
	switch (regno) {
	case 0: regs.sfc = *regp & 7; break;
	case 1: regs.dfc = *regp & 7; break;
	case 2:
	{
	    uae_u32 cacr_mask = 0;
	    if (currprefs.cpu_model == 68020)
		cacr_mask = 0x0000000f;
	    else if (currprefs.cpu_model == 68030)
		cacr_mask = 0x00003f1f;
	    else if (currprefs.cpu_model == 68040)
		cacr_mask = 0x80008000;
	    else if (currprefs.cpu_model == 68060)
		cacr_mask = 0xf8e0e000;
	    regs.cacr = *regp & cacr_mask;
	}
	break;
	 /* 68040/060 only */
	case 3:
	    regs.tcr = *regp & (currprefs.cpu_model == 68060 ? 0xfffe : 0xc000);
	    break;

	/* no differences between 68040 and 68060 */
	case 4: regs.itt0 = *regp & 0xffffe364; break;
	case 5: regs.itt1 = *regp & 0xffffe364; break;
	case 6: regs.dtt0 = *regp & 0xffffe364; break;
	case 7: regs.dtt1 = *regp & 0xffffe364; break;
	/* 68060 only */
	case 8: regs.buscr = *regp & 0xf0000000; break;

	case 0x800: regs.usp = *regp; break;
	case 0x801: regs.vbr = *regp; break;
	case 0x802: regs.caar = *regp & 0xfc; break;
	case 0x803: regs.msp = *regp; if (regs.m == 1) m68k_areg (regs, 7) = regs.msp; break;
	case 0x804: regs.isp = *regp; if (regs.m == 0) m68k_areg (regs, 7) = regs.isp; break;
	/* 68040 only */
	case 0x805: regs.mmusr = *regp; break;
	/* 68040/060 */
	case 0x806: regs.urp = *regp; break;
	case 0x807: regs.srp = *regp; break;
	/* 68060 only */
	case 0x808:
	{
	    uae_u32 opcr = regs.pcr;
	    regs.pcr &= ~(0x40 | 2 | 1);
	    regs.pcr |= (*regp) & (0x40 | 2 | 1);
	    if (((opcr ^ regs.pcr) & 2) == 2) {
		write_log("68060 FPU state: %s\n", regs.pcr & 2 ? "disabled" : "enabled");
		/* flush possible already translated FPU instructions */
		flush_icache (2);
	    }
	}
	break;
	default:
	    op_illg (0x4E7B);
	    return 0;
	}
    }
    return 1;
}

int m68k_movec2 (int regno, uae_u32 *regp)
{
    if (movec_illg (regno)) {
	op_illg (0x4E7B);
	return 0;
    } else {
	switch (regno) {
	case 0: *regp = regs.sfc; break;
	case 1: *regp = regs.dfc; break;
	case 2: 
	{
	    uae_u32 v = regs.cacr;
	    uae_u32 cacr_mask = 0;
	    if (currprefs.cpu_model == 68020)
		cacr_mask = 0x00000003;
	    else if (currprefs.cpu_model == 68030)
		cacr_mask = 0x00003313;
	    else if (currprefs.cpu_model == 68040)
		cacr_mask = 0x80008000;
	    else if (currprefs.cpu_model == 68060)
		cacr_mask = 0xf880e000;
	    *regp = v & cacr_mask;
	}
	break;
	case 3: *regp = regs.tcr; break;
	case 4: *regp = regs.itt0; break;
	case 5: *regp = regs.itt1; break;
	case 6: *regp = regs.dtt0; break;
	case 7: *regp = regs.dtt1; break;
	case 8: *regp = regs.buscr; break;

	case 0x800: *regp = regs.usp; break;
	case 0x801: *regp = regs.vbr; break;
	case 0x802: *regp = regs.caar; break;
	case 0x803: *regp = regs.m == 1 ? m68k_areg(regs, 7) : regs.msp; break;
	case 0x804: *regp = regs.m == 0 ? m68k_areg(regs, 7) : regs.isp; break;
	case 0x805: *regp = regs.mmusr; break;
	case 0x806: *regp = regs.urp; break;
	case 0x807: *regp = regs.srp; break;
	case 0x808: *regp = regs.pcr; break;
	default:
	    op_illg (0x4E7A);
	    return 0;
	}
    }
    return 1;
}

STATIC_INLINE int
div_unsigned(uae_u32 src_hi, uae_u32 src_lo, uae_u32 div, uae_u32 *quot, uae_u32 *rem)
{
	uae_u32 q = 0, cbit = 0;
	int i;

	if (div <= src_hi) {
	    return 1;
	}
	for (i = 0 ; i < 32 ; i++) {
		cbit = src_hi & 0x80000000ul;
		src_hi <<= 1;
		if (src_lo & 0x80000000ul) src_hi++;
		src_lo <<= 1;
		q = q << 1;
		if (cbit || div <= src_hi) {
			q |= 1;
			src_hi -= div;
		}
	}
	*quot = q;
	*rem = src_hi;
	return 0;
}

void m68k_divl (uae_u32 opcode, uae_u32 src, uae_u16 extra, uaecptr oldpc)
{
#if defined(uae_s64)
    if (src == 0) {
	Exception (5, oldpc);
	return;
    }
    if (extra & 0x800) {
	/* signed variant */
	uae_s64 a = (uae_s64)(uae_s32)m68k_dreg (regs, (extra >> 12) & 7);
	uae_s64 quot, rem;

	if (extra & 0x400) {
	    a &= 0xffffffffu;
	    a |= (uae_s64)m68k_dreg (regs, extra & 7) << 32;
	}
	rem = a % (uae_s64)(uae_s32)src;
	quot = a / (uae_s64)(uae_s32)src;
	if ((quot & UVAL64(0xffffffff80000000)) != 0
	    && (quot & UVAL64(0xffffffff80000000)) != UVAL64(0xffffffff80000000))
	{
	    SET_VFLG (1);
	    SET_NFLG (1);
	    SET_CFLG (0);
	} else {
	    if (((uae_s32)rem < 0) != ((uae_s64)a < 0)) rem = -rem;
	    SET_VFLG (0);
	    SET_CFLG (0);
	    SET_ZFLG (((uae_s32)quot) == 0);
	    SET_NFLG (((uae_s32)quot) < 0);
	    m68k_dreg (regs, extra & 7) = rem;
	    m68k_dreg (regs, (extra >> 12) & 7) = quot;
	}
    } else {
	/* unsigned */
	uae_u64 a = (uae_u64)(uae_u32)m68k_dreg (regs, (extra >> 12) & 7);
	uae_u64 quot, rem;

	if (extra & 0x400) {
	    a &= 0xffffffffu;
	    a |= (uae_u64)m68k_dreg (regs, extra & 7) << 32;
	}
	rem = a % (uae_u64)src;
	quot = a / (uae_u64)src;
	if (quot > 0xffffffffu) {
	    SET_VFLG (1);
	    SET_NFLG (1);
	    SET_CFLG (0);
	} else {
	    SET_VFLG (0);
	    SET_CFLG (0);
	    SET_ZFLG (((uae_s32)quot) == 0);
	    SET_NFLG (((uae_s32)quot) < 0);
	    m68k_dreg (regs, extra & 7) = rem;
	    m68k_dreg (regs, (extra >> 12) & 7) = quot;
	}
    }
#else
    if (src == 0) {
	Exception (5, oldpc);
	return;
    }
    if (extra & 0x800) {
	/* signed variant */
	uae_s32 lo = (uae_s32)m68k_dreg (regs, (extra >> 12) & 7);
	uae_s32 hi = lo < 0 ? -1 : 0;
	uae_s32 save_high;
	uae_u32 quot, rem;
	uae_u32 sign;

	if (extra & 0x400) {
	    hi = (uae_s32)m68k_dreg (regs, extra & 7);
	}
	save_high = hi;
	sign = (hi ^ src);
	if (hi < 0) {
	    hi = ~hi;
	    lo = -lo;
	    if (lo == 0) hi++;
	}
	if ((uae_s32)src < 0) src = -src;
	if (div_unsigned(hi, lo, src, &quot, &rem) ||
	    (sign & 0x80000000) ? quot > 0x80000000 : quot > 0x7fffffff) {
	    SET_VFLG (1);
	    SET_NFLG (1);
	    SET_CFLG (0);
	} else {
	    if (sign & 0x80000000) quot = -quot;
	    if (((uae_s32)rem < 0) != (save_high < 0)) rem = -rem;
	    SET_VFLG (0);
	    SET_CFLG (0);
	    SET_ZFLG (((uae_s32)quot) == 0);
	    SET_NFLG (((uae_s32)quot) < 0);
	    m68k_dreg (regs, extra & 7) = rem;
	    m68k_dreg (regs, (extra >> 12) & 7) = quot;
	}
    } else {
	/* unsigned */
	uae_u32 lo = (uae_u32)m68k_dreg (regs, (extra >> 12) & 7);
	uae_u32 hi = 0;
	uae_u32 quot, rem;

	if (extra & 0x400) {
	    hi = (uae_u32)m68k_dreg (regs, extra & 7);
	}
	if (div_unsigned(hi, lo, src, &quot, &rem)) {
	    SET_VFLG (1);
	    SET_NFLG (1);
	    SET_CFLG (0);
	} else {
	    SET_VFLG (0);
	    SET_CFLG (0);
	    SET_ZFLG (((uae_s32)quot) == 0);
	    SET_NFLG (((uae_s32)quot) < 0);
	    m68k_dreg (regs, extra & 7) = rem;
	    m68k_dreg (regs, (extra >> 12) & 7) = quot;
	}
    }
#endif
}

STATIC_INLINE void
mul_unsigned(uae_u32 src1, uae_u32 src2, uae_u32 *dst_hi, uae_u32 *dst_lo)
{
	uae_u32 r0 = (src1 & 0xffff) * (src2 & 0xffff);
	uae_u32 r1 = ((src1 >> 16) & 0xffff) * (src2 & 0xffff);
	uae_u32 r2 = (src1 & 0xffff) * ((src2 >> 16) & 0xffff);
	uae_u32 r3 = ((src1 >> 16) & 0xffff) * ((src2 >> 16) & 0xffff);
	uae_u32 lo;

	lo = r0 + ((r1 << 16) & 0xffff0000ul);
	if (lo < r0) r3++;
	r0 = lo;
	lo = r0 + ((r2 << 16) & 0xffff0000ul);
	if (lo < r0) r3++;
	r3 += ((r1 >> 16) & 0xffff) + ((r2 >> 16) & 0xffff);
	*dst_lo = lo;
	*dst_hi = r3;
}

void m68k_mull (uae_u32 opcode, uae_u32 src, uae_u16 extra)
{
#if defined(uae_s64)
    if (extra & 0x800) {
	/* signed variant */
	uae_s64 a = (uae_s64)(uae_s32)m68k_dreg (regs, (extra >> 12) & 7);

	a *= (uae_s64)(uae_s32)src;
	SET_VFLG (0);
	SET_CFLG (0);
	SET_ZFLG (a == 0);
	SET_NFLG (a < 0);
	if (extra & 0x400)
	    m68k_dreg (regs, extra & 7) = a >> 32;
	else if ((a & UVAL64(0xffffffff80000000)) != 0
		 && (a & UVAL64(0xffffffff80000000)) != UVAL64(0xffffffff80000000))
	{
	    SET_VFLG (1);
	}
	m68k_dreg (regs, (extra >> 12) & 7) = (uae_u32)a;
    } else {
	/* unsigned */
	uae_u64 a = (uae_u64)(uae_u32)m68k_dreg (regs, (extra >> 12) & 7);

	a *= (uae_u64)src;
	SET_VFLG (0);
	SET_CFLG (0);
	SET_ZFLG (a == 0);
	SET_NFLG (((uae_s64)a) < 0);
	if (extra & 0x400)
	    m68k_dreg (regs, extra & 7) = a >> 32;
	else if ((a & UVAL64(0xffffffff00000000)) != 0) {
	    SET_VFLG (1);
	}
	m68k_dreg (regs, (extra >> 12) & 7) = (uae_u32)a;
    }
#else
    if (extra & 0x800) {
	/* signed variant */
	uae_s32 src1,src2;
	uae_u32 dst_lo,dst_hi;
	uae_u32 sign;

	src1 = (uae_s32)src;
	src2 = (uae_s32)m68k_dreg (regs, (extra >> 12) & 7);
	sign = (src1 ^ src2);
	if (src1 < 0) src1 = -src1;
	if (src2 < 0) src2 = -src2;
	mul_unsigned((uae_u32)src1,(uae_u32)src2,&dst_hi,&dst_lo);
	if (sign & 0x80000000) {
		dst_hi = ~dst_hi;
		dst_lo = -dst_lo;
		if (dst_lo == 0) dst_hi++;
	}
	SET_VFLG (0);
	SET_CFLG (0);
	SET_ZFLG (dst_hi == 0 && dst_lo == 0);
	SET_NFLG (((uae_s32)dst_hi) < 0);
	if (extra & 0x400)
	    m68k_dreg (regs, extra & 7) = dst_hi;
	else if ((dst_hi != 0 || (dst_lo & 0x80000000) != 0)
		 && ((dst_hi & 0xffffffff) != 0xffffffff
		     || (dst_lo & 0x80000000) != 0x80000000))
	{
	    SET_VFLG (1);
	}
	m68k_dreg (regs, (extra >> 12) & 7) = dst_lo;
    } else {
	/* unsigned */
	uae_u32 dst_lo,dst_hi;

	mul_unsigned(src,(uae_u32)m68k_dreg (regs, (extra >> 12) & 7),&dst_hi,&dst_lo);

	SET_VFLG (0);
	SET_CFLG (0);
	SET_ZFLG (dst_hi == 0 && dst_lo == 0);
	SET_NFLG (((uae_s32)dst_hi) < 0);
	if (extra & 0x400)
	    m68k_dreg (regs, extra & 7) = dst_hi;
	else if (dst_hi != 0) {
	    SET_VFLG (1);
	}
	m68k_dreg (regs, (extra >> 12) & 7) = dst_lo;
    }
#endif
}
static const char *ccnames[] =
{ "T ","F ","HI","LS","CC","CS","NE","EQ",
  "VC","VS","PL","MI","GE","LT","GT","LE" };

#define MC68060_PCR   0x04300000
#define MC68EC060_PCR 0x04310000

void m68k_reset (void)
{
    if (currprefs.cpu_model != changed_prefs.cpu_model
	|| currprefs.fpu_model != changed_prefs.fpu_model
	|| currprefs.address_space_24 != changed_prefs.address_space_24)
    {
	currprefs.address_space_24 = changed_prefs.address_space_24;
	currprefs.cpu_model = changed_prefs.cpu_model;
	currprefs.fpu_model = changed_prefs.fpu_model;
	build_cpufunctbl ();
    }

    regs.kick_mask = 0x00F80000;
    regs.spcflags = 0;
    if (savestate_state == STATE_RESTORE) {
	m68k_setpc (regs.pc);
	/* MakeFromSR() must not swap stack pointer */
	regs.m = (regs.sr >> 12) & 1;
	regs.s = (regs.sr >> 13) & 1;
	MakeFromSR();
	/* set stack pointer */
	if (regs.s)
	    m68k_areg (regs, 7) = regs.m ? regs.msp : regs.isp;
	else
	    m68k_areg (regs, 7) = regs.usp;
	return;
    }

    m68k_areg (regs, 7) = get_long (0x00f80000);
    m68k_setpc (get_long (0x00f80004));
    regs.s = 1;
    regs.m = 0;
    regs.stopped = 0;
    regs.t1 = 0;
    regs.t0 = 0;
    SET_ZFLG (0);
    SET_XFLG (0);
    SET_CFLG (0);
    SET_VFLG (0);
    SET_NFLG (0);
    regs.intmask = 7;
    regs.vbr = regs.sfc = regs.dfc = 0;
    regs.fpcr = regs.fpsr = regs.fpiar = 0;
    regs.irc = 0xffff;

    /* only (E)nable bit is zeroed when CPU is reset, A3000 SuperKickstart expects this */
    regs.tc_030 &= ~0x80000000;
    if (1 /* hardreset */) {
	regs.srp_030 = regs.crp_030 = 0;
	regs.tt0_030 = regs.tt1_030 = regs.tc_030 = 0;
    }
    regs.mmusr_030 = 0;

    /* 68060 FPU is not compatible with 68040,
     * 68060 accelerators' boot ROM disables the FPU
     */
    regs.pcr = 0;
    if (currprefs.cpu_model == 68060) {
	regs.pcr = currprefs.fpu_model != 0 ? MC68060_PCR : MC68EC060_PCR;
#if 0
	regs.pcr |= (currprefs.cpu060_revision & 0xff) << 8;
#endif
	regs.pcr |= 2;
    }
    fill_prefetch_slow ();
}

unsigned long REGPARAM2 op_illg (uae_u32 opcode)
{
    uaecptr pc = m68k_getpc ();

    if (cloanto_rom && (opcode & 0xF100) == 0x7100) {
	m68k_dreg (regs, (opcode >> 9) & 7) = (uae_s8)(opcode & 0xFF);
	m68k_incpc (2);
	fill_prefetch_slow ();
	return 4;
    }

    if (opcode == 0x4E7B && get_long (0x10) == 0 && (pc & 0xF80000) == 0xF80000) {
	write_log ("Your Kickstart requires a 68020 CPU. Giving up.\n");
	broken_in = 1;
	set_special (SPCFLAG_BRK);
	quit_program = 1;
    }
    if (opcode == 0xFF0D) {
	if ((pc & 0xF80000) == 0xF80000) {
	    /* This is from the dummy Kickstart replacement */
	    uae_u16 arg = get_iword (2);
	    m68k_incpc (4);
	    ersatz_perform (arg);
	    fill_prefetch_slow ();
	    return 4;
	} else if ((pc & 0xFFFF0000) == RTAREA_BASE) {
	    /* User-mode STOP replacement */
	    m68k_setstopped (1);
	    return 4;
	}
    }

    if ((opcode & 0xF000) == 0xA000 && (pc & 0xFFFF0000) == RTAREA_BASE) {
	/* Calltrap. */
	m68k_incpc (2);
	m68k_handle_trap (opcode & 0xFFF);
	fill_prefetch_slow ();
	return 4;
    }

    if ((opcode & 0xF000) == 0xF000) {
	Exception(0xB,0);
	return 4;
    }
    if ((opcode & 0xF000) == 0xA000) {
	Exception(0xA,0);
	return 4;
    }
#if 1
    write_log ("Illegal instruction: %04x at %08lx\n", opcode, pc);
#endif
    Exception (4,0);
    return 4;
}

static char *mmu30regs[] = { "TCR", "", "SRP", "CRP", "", "", "", "" };

static void mmu_op30_pmove (uaecptr pc, uae_u32 opcode, uae_u16 next, uaecptr extra)
{
    int preg = (next >> 10) & 31;
    int rw = (next >> 9) & 1;
    int fd = (next >> 8) & 1;
    char *reg = NULL;
    uae_u32 otc = regs.tc_030;
    int siz;

    switch (preg)
    {
    case 0x10: // TC
	reg = "TC";
	siz = 4;
	if (rw)
	    put_long (extra, regs.tc_030);
	else
	    regs.tc_030 = get_long (extra);
	break;
    case 0x12: // SRP
	reg = "SRP";
	siz = 8;
	if (rw) {
	    put_long (extra, regs.srp_030 >> 32);
	    put_long (extra + 4, regs.srp_030);
	} else {
	    regs.srp_030 = (uae_u64)get_long (extra) << 32;
	    regs.srp_030 |= get_long (extra + 4);
	}
	break;
    case 0x13: // CRP
	reg = "CRP";
	siz = 8;
	if (rw) {
	    put_long (extra, regs.crp_030 >> 32);
	    put_long (extra + 4, regs.crp_030);
	} else {
	    regs.crp_030 = (uae_u64)get_long (extra) << 32;
	    regs.crp_030 |= get_long (extra + 4);
	}
	break;
    case 0x18: // MMUSR
	reg = "MMUSR";
	siz = 2;
	if (rw)
	    put_word (extra, regs.mmusr_030);
	else
	    regs.mmusr_030 = get_word (extra);
	break;
    case 0x02: // TT0
	reg = "TT0";
	siz = 4;
	if (rw)
	    put_long (extra, regs.tt0_030);
	else
	    regs.tt0_030 = get_long (extra);
	break;
    case 0x03: // TT1
	reg = "TT1";
	siz = 4;
	if (rw)
	    put_long (extra, regs.tt1_030);
	else
	    regs.tt1_030 = get_long (extra);
	break;
    }

    if (!reg) {
	op_illg (opcode);
	return;
    }
#if MMUOP_DEBUG > 0
    {
	uae_u32 val;
	if (siz == 8) {
	    uae_u32 val2 = get_long (extra);
	    val = get_long (extra + 4);
	    if (rw)
		write_log ("PMOVE %s,%08X%08X", reg, val2, val);
	    else
		write_log ("PMOVE %08X%08X,%s", val2, val, reg);
	} else {
	    if (siz == 4)
		val = get_long (extra);
	    else
		val = get_word (extra);
	    if (rw)
		write_log ("PMOVE %s,%08X", reg, val);
	    else
		write_log ("PMOVE %08X,%s", val, reg);
	}
	write_log (" PC=%08X\n", pc);
    }
#endif
}

static void mmu_op30_ptest (uaecptr pc, uae_u32 opcode, uae_u16 next, uaecptr extra)
{
#if MMUOP_DEBUG > 0
    char tmp[10];

    tmp[0] = 0;
    if ((next >> 8) & 1)
	sprintf(tmp,",A%d", (next >> 4) & 15);
    write_log ("PTEST%c %02X,%08X,#%X%s PC=%08X\n",
	((next >> 9) & 1) ? 'W' : 'R', (next & 15), extra, (next >> 10) & 7, tmp, pc); 
#endif
    //    mmusr_030 = 0;
}

static void mmu_op30_pflush (uaecptr pc, uae_u32 opcode, uae_u16 next, uaecptr extra)
{
#if MMUOP_DEBUG > 0
    write_log ("PFLUSH PC=%08X\n", pc);
#endif
}

void mmu_op30 (uaecptr pc, uae_u32 opcode, int isnext, uaecptr extra)
{
    if (currprefs.cpu_model != 68030) {
	m68k_setpc (pc);
	op_illg (opcode);
	return;
    }
    if (isnext) {
	uae_u16 next = get_word (pc + 2);
	if (next & 0x8000)
	    mmu_op30_ptest (pc, opcode, next, extra);
	else if (next & 0x2000)
	    mmu_op30_pflush (pc, opcode, next, extra);
	else
	    mmu_op30_pmove (pc, opcode, next, extra);
	m68k_setpc (m68k_getpc () + 2);
    } else {
#if MMUOP_DEBUG > 0
	write_log ("MMU030: %04x PC=%08x\n", opcode, m68k_getpc ());
#endif
    }
    return;
}

void mmu_op(uae_u32 opcode, uae_u16 extra)
{
#if MMUOP_DEBUG > 1
    write_log ("mmu_op %04X PC=%08X\n", opcode, m68k_getpc ());
#endif
    if ((opcode & 0xFE0) == 0x0500) {
	/* PFLUSH */
	regs.mmusr = 0;
#if MMUOP_DEBUG > 0
	write_log ("PFLUSH\n");
#endif
	return;
    } else if ((opcode & 0x0FD8) == 0x548) {
	if (currprefs.cpu_model < 68060) { /* PTEST not in 68060 */
	    /* PTEST */
#if MMUOP_DEBUG > 0
	    write_log ("PTEST\n");
#endif
	    return;
	}
    } else if ((opcode & 0x0FB8) == 0x588) {
	/* PLPA */
	if (currprefs.cpu_model == 68060) {
#if MMUOP_DEBUG > 0
	    write_log ("PLPA\n");
#endif
	    return;
	}
    } else if (opcode == 0xff00 && extra == 0x01c0) {
	/* LPSTOP */
	if (currprefs.cpu_model == 68060) {
#if MMUOP_DEBUG > 0
	    write_log ("LPSTOP\n");
#endif
	    return;
	}
    }
#if MMUOP_DEBUG > 0
    write_log ("Unknown MMU OP %04X\n", opcode);
#endif
    m68k_setpc (m68k_getpc () - 2);
    op_illg (opcode);
}

static int n_insns = 0, n_spcinsns = 0;

static uaecptr last_trace_ad = 0;

static void do_trace (void)
{
    if (regs.t0 && currprefs.cpu_model >= 68020) {
	uae_u16 opcode;
	/* should also include TRAP, CHK, SR modification FPcc */
	/* probably never used so why bother */
	/* We can afford this to be inefficient... */
	m68k_setpc (m68k_getpc ());
	fill_prefetch_slow ();
	opcode = get_word (regs.pc);
	if (opcode == 0x4e72 		/* RTE */
	    || opcode == 0x4e74 		/* RTD */
	    || opcode == 0x4e75 		/* RTS */
	    || opcode == 0x4e77 		/* RTR */
	    || opcode == 0x4e76 		/* TRAPV */
	    || (opcode & 0xffc0) == 0x4e80 	/* JSR */
	    || (opcode & 0xffc0) == 0x4ec0 	/* JMP */
	    || (opcode & 0xff00) == 0x6100  /* BSR */
	    || ((opcode & 0xf000) == 0x6000	/* Bcc */
		&& cctrue((opcode >> 8) & 0xf))
	    || ((opcode & 0xf0f0) == 0x5050 /* DBcc */
		&& !cctrue((opcode >> 8) & 0xf)
		&& (uae_s16)m68k_dreg (regs, opcode & 7) != 0))
	{
	    last_trace_ad = m68k_getpc ();
	    unset_special (SPCFLAG_TRACE);
	    set_special (SPCFLAG_DOTRACE);
	}
    } else if (regs.t1) {
	last_trace_ad = m68k_getpc ();
	unset_special (SPCFLAG_TRACE);
	set_special (SPCFLAG_DOTRACE);
    }
}

static int do_specialties (int cycles)
{
    if (regs.spcflags & SPCFLAG_RESTORE_SANITY) {
	m68k_setpc (0xF0FFC0);
	fill_prefetch_slow ();
	unset_special (SPCFLAG_RESTORE_SANITY);
    }
    if (regs.spcflags & SPCFLAG_COPPER)
	do_copper ();

    /*n_spcinsns++;*/
    while ((regs.spcflags & SPCFLAG_BLTNASTY) && cycles > 0) {
	int c = blitnasty();
	if (!c) {
	    cycles -= 2 * CYCLE_UNIT;
	    if (cycles < CYCLE_UNIT)
		cycles = 0;
	    c = 1;
	}
	do_cycles (c * CYCLE_UNIT);
	if (regs.spcflags & SPCFLAG_COPPER)
	    do_copper ();
    }

    if (regs.spcflags & SPCFLAG_DOTRACE)
	Exception (9,last_trace_ad);

    while (regs.spcflags & SPCFLAG_STOP) {
	do_cycles (4 * CYCLE_UNIT);
	if (regs.spcflags & SPCFLAG_COPPER)
	    do_copper ();
	if (regs.spcflags & (SPCFLAG_INT | SPCFLAG_DOINT)) {
	    int intr = intlev ();
	    unset_special (SPCFLAG_INT | SPCFLAG_DOINT);
	    if (intr != -1 && intr > regs.intmask) {
		Interrupt (intr);
		regs.stopped = 0;
		unset_special (SPCFLAG_STOP);
	    }
	}
    }
    if (regs.spcflags & SPCFLAG_TRACE)
	do_trace ();

    if (regs.spcflags & SPCFLAG_DOINT) {
	int intr = intlev ();
	unset_special (SPCFLAG_DOINT);
	if (intr != -1 && intr > regs.intmask) {
	    Interrupt (intr);
	    regs.stopped = 0;
	}
    }
    if (regs.spcflags & SPCFLAG_INT) {
	unset_special (SPCFLAG_INT);
	set_special (SPCFLAG_DOINT);
    }
    if (regs.spcflags & (SPCFLAG_BRK | SPCFLAG_MODE_CHANGE)) {
	unset_special (SPCFLAG_BRK | SPCFLAG_MODE_CHANGE);
	return 1;
    }
    return 0;
}

/* It's really sad to have two almost identical functions for this, but we
   do it all for performance... :( */
static void m68k_run_1 (void)
{
    for (;;) {
	int cycles;
	uae_u32 opcode = regs.ir;

	/* assert (!regs.stopped && !(regs.spcflags & SPCFLAG_STOP)); */
/*	regs_backup[backup_pointer = (backup_pointer + 1) % 16] = regs;*/
#if COUNT_INSTRS == 2
	if (table68k[opcode].handler != -1)
	    instrcount[table68k[opcode].handler]++;
#elif COUNT_INSTRS == 1
	instrcount[opcode]++;
#endif
	cycles = (*cpufunctbl[opcode])(opcode);
	/*n_insns++;*/
	cycles &= cycles_mask;
	cycles |= cycles_val;
	do_cycles (cycles);
	if (regs.spcflags) {
	    if (do_specialties (cycles))
		return;
	}
    }
}

#define DEBUG_PREFETCH

/* Same thing, but don't use prefetch to get opcode.  */
static void m68k_run_2 (void)
{
    for (;;) {
	int cycles;
	uae_u32 opcode = get_iword (0);

	/* assert (!regs.stopped && !(regs.spcflags & SPCFLAG_STOP)); */
/*	regs_backup[backup_pointer = (backup_pointer + 1) % 16] = regs;*/
#if COUNT_INSTRS == 2
	if (table68k[opcode].handler != -1)
	    instrcount[table68k[opcode].handler]++;
#elif COUNT_INSTRS == 1
	instrcount[opcode]++;
#endif
	cycles = (*cpufunctbl[opcode])(opcode);

	/*n_insns++;*/
	cycles &= cycles_mask;
	cycles |= cycles_val;
	do_cycles (cycles);
	if (regs.spcflags) {
	    if (do_specialties (cycles))
		return;
	}
    }
}

#define m68k_run1(F) (F) ()

int in_m68k_go = 0;

static void exception2_handle (uaecptr addr, uaecptr fault)
{
    last_addr_for_exception_3 = addr;
    last_fault_for_exception_3 = fault;
    last_writeaccess_for_exception_3 = 0;
    last_instructionaccess_for_exception_3 = 0;
    Exception (2, addr);
}

void m68k_go (int may_quit)
{
    if (in_m68k_go || !may_quit) {
	write_log ("Bug! m68k_go is not reentrant.\n");
	abort ();
    }

    reset_frame_rate_hack ();
    update_68k_cycles ();

    in_m68k_go++;
    for (;;) {
	if (quit_program > 0) {
	    if (quit_program == 1)
		break;
	    quit_program = 0;
	    if (savestate_state == STATE_RESTORE) {
		restore_state (savestate_filename);
#if 0
		activate_debugger ();
#endif
	    }
	    reset_all_systems ();
	    customreset ();
	    m68k_reset ();
	    /* We may have been restoring state, but we're done now.  */
	    savestate_restore_finish ();
	    fill_prefetch_slow ();
	    handle_active_events ();
	    if (regs.spcflags)
		do_specialties (0);
	    m68k_setpc (regs.pc);
	}

	if (debugging)
	    debug ();
	if (regs.panic) {
	    regs.panic = 0;
	    /* program jumped to non-existing memory and cpu was >= 68020 */
	    get_real_address (regs.isp); /* stack in no one's land? -> reboot */
	    if (regs.isp & 1)
		regs.panic = 1;
	    if (!regs.panic)
		exception2_handle (regs.panic_pc, regs.panic_addr);
	    if (regs.panic) {
		/* system is very badly confused */
		write_log ("double bus error or corrupted stack, forcing reboot..\n");
		regs.panic = 0;
		uae_reset (1);
	    }
	}
	m68k_run1 (currprefs.cpu_model == 68000 ? m68k_run_1 : m68k_run_2);
    }
    in_m68k_go--;
}

static void m68k_verify (uaecptr addr, uaecptr *nextpc)
{
    uae_u32 opcode, val;
    struct instr *dp;

    opcode = get_iword_1(0);
    last_op_for_exception_3 = opcode;
    m68kpc_offset = 2;

    if (cpufunctbl[opcode] == op_illg_1) {
	opcode = 0x4AFC;
    }
    dp = table68k + opcode;

    if (dp->suse) {
	if (!verify_ea (dp->sreg, dp->smode, dp->size, &val)) {
	    Exception (3, 0);
	    return;
	}
    }
    if (dp->duse) {
	if (!verify_ea (dp->dreg, dp->dmode, dp->size, &val)) {
	    Exception (3, 0);
	    return;
	}
    }
}

void m68k_disasm (FILE *f, uaecptr addr, uaecptr *nextpc, int cnt)
{
    uaecptr newpc = 0;
    m68kpc_offset = addr - m68k_getpc ();
    while (cnt-- > 0) {
	char instrname[20],*ccpt;
	int opwords;
	uae_u32 opcode;
	struct mnemolookup *lookup;
	struct instr *dp;
	fprintf (f, "%08lx: ", m68k_getpc () + m68kpc_offset);
	for (opwords = 0; opwords < 5; opwords++){
	    fprintf (f, "%04x ", get_iword_1 (m68kpc_offset + opwords*2));
	}
	opcode = get_iword_1 (m68kpc_offset);
	m68kpc_offset += 2;
	if (cpufunctbl[opcode] == op_illg_1) {
	    opcode = 0x4AFC;
	}
	dp = table68k + opcode;
	for (lookup = lookuptab;lookup->mnemo != dp->mnemo; lookup++)
	    ;

	strcpy (instrname, lookup->name);
	ccpt = strstr (instrname, "cc");
	if (ccpt != 0) {
	    strncpy (ccpt, ccnames[dp->cc], 2);
	}
	fprintf (f, "%s", instrname);
	switch (dp->size){
	 case sz_byte: fprintf (f, ".B "); break;
	 case sz_word: fprintf (f, ".W "); break;
	 case sz_long: fprintf (f, ".L "); break;
	 default: fprintf (f, "   "); break;
	}

	if (dp->suse) {
	    newpc = m68k_getpc () + m68kpc_offset;
	    newpc += ShowEA (f, dp->sreg, dp->smode, dp->size, 0);
	}
	if (dp->suse && dp->duse)
	    fprintf (f, ",");
	if (dp->duse) {
	    newpc = m68k_getpc () + m68kpc_offset;
	    newpc += ShowEA (f, dp->dreg, dp->dmode, dp->size, 0);
	}
	if (ccpt != 0) {
	    if (cctrue(dp->cc))
		fprintf (f, " == %08lx (TRUE)", newpc);
	    else
		fprintf (f, " == %08lx (FALSE)", newpc);
	} else if ((opcode & 0xff00) == 0x6100) /* BSR */
	    fprintf (f, " == %08lx", newpc);
	fprintf (f, "\n");
    }
    if (nextpc)
	*nextpc = m68k_getpc () + m68kpc_offset;
}

void m68k_dumpstate (FILE *f, uaecptr *nextpc)
{
    int i;
    for (i = 0; i < 8; i++){
	fprintf (f, "D%d: %08lx ", i, m68k_dreg (regs, i));
	if ((i & 3) == 3) fprintf (f, "\n");
    }
    for (i = 0; i < 8; i++){
	fprintf (f, "A%d: %08lx ", i, m68k_areg (regs, i));
	if ((i & 3) == 3) fprintf (f, "\n");
    }
    if (regs.s == 0) regs.usp = m68k_areg (regs, 7);
    if (regs.s && regs.m) regs.msp = m68k_areg (regs, 7);
    if (regs.s && regs.m == 0) regs.isp = m68k_areg (regs, 7);
    fprintf (f, "USP=%08lx ISP=%08lx MSP=%08lx VBR=%08lx\n",
	     regs.usp,regs.isp,regs.msp,regs.vbr);
    fprintf (f, "T=%d%d S=%d M=%d X=%d N=%d Z=%d V=%d C=%d IMASK=%d\n",
	     regs.t1, regs.t0, regs.s, regs.m,
	     GET_XFLG, GET_NFLG, GET_ZFLG, GET_VFLG, GET_CFLG, regs.intmask);
    for (i = 0; i < 8; i++){
	fprintf (f, "FP%d: %g ", i, regs.fp[i]);
	if ((i & 3) == 3) fprintf (f, "\n");
    }
    fprintf (f, "N=%d Z=%d I=%d NAN=%d\n",
	     (regs.fpsr & 0x8000000) != 0,
	     (regs.fpsr & 0x4000000) != 0,
	     (regs.fpsr & 0x2000000) != 0,
	     (regs.fpsr & 0x1000000) != 0);

    m68k_disasm (f, m68k_getpc (), nextpc, 1);
    if (nextpc)
	fprintf (f, "next PC: %08lx\n", *nextpc);
}


/* CPU save/restore code */

#define CPUTYPE_EC 1
#define PREFETCH_VALID 2
#define M68KSPEED_SAVED 0x8000000
#define DEFAULT_SAVE_FLAGS (M68KSPEED_SAVED | PREFETCH_VALID)
#define CPUMODE_HALT 1

const uae_u8 *restore_cpu (const uae_u8 *src)
{
    int i, model, flags;
    uae_u32 l;

    changed_prefs.cpu_model = model = restore_u32();

    flags = restore_u32();
    changed_prefs.address_space_24 = 0;
    if (flags & CPUTYPE_EC)
	changed_prefs.address_space_24 = 1;
    for (i = 0; i < 15; i++)
	regs.regs[i] = restore_u32 ();
    regs.pc = restore_u32 ();
    regs.irc = restore_u16 ();
    regs.ir = restore_u16 ();
    regs.usp = restore_u32 ();
    regs.isp = restore_u32 ();
    regs.sr = restore_u16 ();
    l = restore_u32();
    if (l & CPUMODE_HALT) {
	regs.stopped = 1;
	set_special (SPCFLAG_STOP);
    } else
	regs.stopped = 0;
    if (model >= 68010) {
	regs.dfc = restore_u32 ();
	regs.sfc = restore_u32 ();
	regs.vbr = restore_u32 ();
    }
    if (model >= 68020) {
	regs.caar = restore_u32 ();
	regs.cacr = restore_u32 ();
	regs.msp = restore_u32 ();
    }
    if (model >= 68030) {
	regs.crp_030 = restore_u64 ();
	regs.srp_030 = restore_u64 ();
	regs.tt0_030 = restore_u32 ();
	regs.tt1_030 = restore_u32 ();
	regs.tc_030 = restore_u32 ();
	regs.mmusr_030 = restore_u16 ();
    }
    if (model >= 68040) {
	regs.itt0 = restore_u32 ();
	regs.itt1 = restore_u32 ();
	regs.dtt0 = restore_u32 ();
	regs.dtt1 = restore_u32 ();
	regs.tcr = restore_u32 ();
	regs.urp = restore_u32 ();
	regs.srp = restore_u32 ();
    }
    if (model >= 68060) {
	regs.buscr = restore_u32 ();
	regs.pcr = restore_u32 ();
    }
    if (flags & M68KSPEED_SAVED) {
	int khz = restore_u32 ();
	restore_u32 ();
	if (khz > 0 && khz < 800000)
	    currprefs.m68k_speed = changed_prefs.m68k_speed = 0;
    }

    if (!(flags & PREFETCH_VALID))
	fill_prefetch_slow ();
    write_log ("CPU %d%s%03d, PC=%08.8X\n",
	       model / 1000, flags & 1 ? "EC" : "", model % 1000, regs.pc);

    return src;
}

uae_u8 *save_cpu (int *len, uae_u8 *dstptr)
{
    uae_u8 *dstbak, *dst;
    int model, i, khz;

    if (dstptr)
	dstbak = dst = dstptr;
    else
	dstbak = dst = malloc (1000);
    model = currprefs.cpu_model;
    save_u32 (model);					/* MODEL */
    save_u32 (DEFAULT_SAVE_FLAGS | (currprefs.address_space_24 ? 1 : 0));	/* FLAGS */
    for (i = 0;i < 15; i++) save_u32 (regs.regs[i]);	/* D0-D7 A0-A6 */
    save_u32 (m68k_getpc ());				/* PC */
    save_u16 (regs.irc);				/* prefetch */
    save_u16 (regs.ir);					/* instruction prefetch */
    MakeSR ();
    save_u32 (!regs.s ? regs.regs[15] : regs.usp);	/* USP */
    save_u32 (regs.s ? regs.regs[15] : regs.isp);	/* ISP */
    save_u16 (regs.sr);				/* SR/CCR */
    save_u32 (regs.stopped ? CPUMODE_HALT : 0);	/* flags */
    if (model >= 68010) {
	save_u32 (regs.dfc);				/* DFC */
	save_u32 (regs.sfc);				/* SFC */
	save_u32 (regs.vbr);				/* VBR */
    }
    if (model >= 68020) {
	save_u32 (regs.caar);				/* CAAR */
	save_u32 (regs.cacr);				/* CACR */
	save_u32 (regs.msp);				/* MSP */
    }
    if (model >= 68030) {
	save_u64 (regs.crp_030);                        /* CRP */
	save_u64 (regs.srp_030);                        /* SRP */
	save_u32 (regs.tt0_030);                        /* TT0/AC0 */
	save_u32 (regs.tt1_030);                        /* TT1/AC1 */
	save_u32 (regs.tc_030);                         /* TCR */
	save_u16 (regs.mmusr_030);                      /* MMUSR/ACUSR */
    }
    if (model >= 68040) {
	save_u32 (regs.itt0);                           /* ITT0 */
	save_u32 (regs.itt1);                           /* ITT1 */
	save_u32 (regs.dtt0);                           /* DTT0 */
	save_u32 (regs.dtt1);                           /* DTT1 */
	save_u32 (regs.tcr);                            /* TCR */
	save_u32 (regs.urp);                            /* URP */
	save_u32 (regs.srp);                            /* SRP */
    }
    if (model >= 68060) {
	save_u32 (regs.buscr);                          /* BUSCR */
	save_u32 (regs.pcr);                            /* PCR */
    }
    khz = -1;
    if (currprefs.m68k_speed == 0) {
	khz = currprefs.ntscmode ? 715909 : 709379;
	if (currprefs.cpu_model >= 68020)
	    khz *= 2;
    }
    save_u32 (khz); // clock rate in KHz: -1 = fastest possible
    save_u32 (0); // spare
    *len = dst - dstbak;
    return dstbak;
}

static void exception3f (uae_u32 opcode, uaecptr addr, uaecptr fault, int writeaccess, int instructionaccess)
{
    last_addr_for_exception_3 = addr;
    last_fault_for_exception_3 = fault;
    last_op_for_exception_3 = opcode;
    last_writeaccess_for_exception_3 = writeaccess;
    last_instructionaccess_for_exception_3 = instructionaccess;
    Exception (3, fault);
}

void exception3 (uae_u32 opcode, uaecptr addr, uaecptr fault)
{
    exception3f (opcode, addr, fault, 0, 0);
}

void exception3i (uae_u32 opcode, uaecptr addr, uaecptr fault)
{
    exception3f (opcode, addr, fault, 0, 1);
}

void exception2 (uaecptr addr, uaecptr fault)
{
    write_log ("delayed exception2!\n");
    regs.panic_pc = m68k_getpc ();
    regs.panic_addr = addr;
    regs.panic = 2;
    set_special (SPCFLAG_BRK);
    m68k_setpc (0xf80000);
    fill_prefetch_slow ();
}

void cpureset (void)
{
    uaecptr pc;
    uaecptr ksboot = 0xf80002 - 2; /* -2 = RESET hasn't increased PC yet */
    uae_u16 ins;

#if 0
    if (currprefs.cpu_compatible || currprefs.cpu_cycle_exact) {
	customreset (0);
	return;
    }
#endif
    pc = m68k_getpc ();
    if (pc >= currprefs.chipmem_size) {
	addrbank *b = &get_mem_bank (pc);
	if (b->check (pc, 2 + 2)) {
	    /* We have memory, hope for the best.. */
	    customreset ();
	    return;
	}
	write_log ("M68K RESET PC=%x, rebooting..\n", pc);
	customreset ();
	m68k_setpc (ksboot);
	return;
    }
    /* panic, RAM is going to disappear under PC */
    ins = get_word (pc + 2);
    if ((ins & ~7) == 0x4ed0) {
	int reg = ins & 7;
	uae_u32 addr = m68k_areg (regs, reg);
	write_log ("reset/jmp (ax) combination emulated -> %x\n", addr);
	customreset ();
	if (addr < 0x80000)
	    addr += 0xf80000;
	m68k_setpc (addr - 2);
	return;
    }
    write_log ("M68K RESET PC=%x, rebooting..\n", pc);
    customreset ();
    m68k_setpc (ksboot);
}
