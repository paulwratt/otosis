/*
 * UAE - The Un*x Amiga Emulator
 *
 * MC68000 emulation generator
 *
 * This is a fairly stupid program that generates a lot of case labels that
 * can be #included in a switch statement.
 * As an alternative, it can generate functions that handle specific
 * MC68000 instructions, plus a prototype header file and a function pointer
 * array to look up the function for an opcode.
 * Error checking is bad, an illegal table68k file will cause the program to
 * call abort().
 * The generated code is sometimes sub-optimal, an optimizing compiler should
 * take care of this.
 *
 * Copyright 1995, 1996 Bernd Schmidt
 */


#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>

#include "misc.h"
#include "readcpu.h"
#include "malloc.h"

#define BOOL_TYPE "int"

static FILE *headerfile;
static FILE *stblfile;

static int using_prefetch;
static int using_exception_3;
static int cpu_level;

/* For the current opcode, the next lower level that will have different code.
 * Initialized to -1 for each opcode. If it remains unchanged, indicates we
 * are done with that opcode.  */
static int next_cpu_level;

void write_log (const char *s, ...)
{
    fprintf (stderr, "%s", s);
}

static int *opcode_map;
static int *opcode_next_clev;
static int *opcode_last_postfix;
static unsigned long *counts;

struct amode_queue
{
    struct amode_queue *next;
    char *buf;
    unsigned int done:1;
};

static struct amode_queue *aq_head;

static void read_counts (void)
{
    FILE *file;
    unsigned long opcode, count, total;
    char name[20];
    int nr = 0;
    memset (counts, 0, 65536 * sizeof *counts);

    file = fopen ("frequent.68k", "r");
    if (file) {
	fscanf (file, "Total: %lu\n", &total);
	while (fscanf (file, "%lx: %lu %s\n", &opcode, &count, name) == 3) {
	    opcode_next_clev[nr] = 3;
	    opcode_last_postfix[nr] = -1;
	    opcode_map[nr++] = opcode;
	    counts[opcode] = count;
	}
	fclose (file);
    }
    if (nr == nr_cpuop_funcs)
	return;
    for (opcode = 0; opcode < 0x10000; opcode++) {
	if (table68k[opcode].handler == -1 && table68k[opcode].mnemo != i_ILLG
	    && counts[opcode] == 0)
	{
	    opcode_next_clev[nr] = 3;
	    opcode_last_postfix[nr] = -1;
	    opcode_map[nr++] = opcode;
	    counts[opcode] = count;
	}
    }
    if (nr != nr_cpuop_funcs)
	abort ();
}

static char endlabelstr[80];
static int endlabelno = 0;
static int need_endlabel;

static int n_braces = 0;
static int m68k_pc_offset = 0;
static int insn_n_cycles;

static void start_brace (void)
{
    n_braces++;
    printf ("{");
}

static void close_brace (void)
{
    assert (n_braces > 0);
    n_braces--;
    printf ("}");
}

static void finish_braces (void)
{
    while (n_braces > 0)
	close_brace ();
}

static void pop_braces (int to)
{
    while (n_braces > to)
	close_brace ();
}

static int bit_size (int size)
{
    switch (size) {
     case sz_byte: return 8;
     case sz_word: return 16;
     case sz_long: return 32;
     default: abort ();
    }
    return 0;
}

static const char *bit_mask (int size)
{
    switch (size) {
     case sz_byte: return "0xff";
     case sz_word: return "0xffff";
     case sz_long: return "0xffffffff";
     default: abort ();
    }
    return 0;
}

static const char *gen_nextilong (void)
{
    static char buffer[80];
    int r = m68k_pc_offset;
    m68k_pc_offset += 4;

    insn_n_cycles += 4;

    if (using_prefetch)
	sprintf (buffer, "get_ilong_prefetch(%d)", r);
    else
	sprintf (buffer, "get_ilong(%d)", r);
    return buffer;
}

static const char *gen_nextiword (void)
{
    static char buffer[80];
    int r = m68k_pc_offset;
    m68k_pc_offset += 2;

    insn_n_cycles += 2;

    if (using_prefetch)
	sprintf (buffer, "get_iword_prefetch(%d)", r);
    else
	sprintf (buffer, "get_iword(%d)", r);
    return buffer;
}

static const char *gen_nextibyte (void)
{
    static char buffer[80];
    int r = m68k_pc_offset;
    m68k_pc_offset += 2;

    insn_n_cycles += 2;

    if (using_prefetch)
	sprintf (buffer, "get_ibyte_prefetch(%d)", r);
    else
	sprintf (buffer, "get_ibyte(%d)", r);
    return buffer;
}

static void fill_prefetch_0 (void)
{
    if (using_prefetch)
	printf ("fill_prefetch_0 ();\n");
}

static void fill_prefetch_2 (void)
{
    if (using_prefetch)
	printf ("fill_prefetch_2 ();\n");
}

/* To allow the compiler to generate better code, we try to pre-schedule
 * the C code we generate to put addressing mode generation code that is
 * likely to require many registers earlier than code that is likely to
 * use fewer registers.  */

#define DO_QUEUEING 1


#if __GNUC__ - 1 > 1 || __GNUC_MINOR__ - 1 > 6
static void queue_amode (const char *, ...) __attribute__ ((format (printf, 1, 2)));
#endif

static void queue_amode (const char *fmt, ...)
{
    char buf[160];
    struct amode_queue *prev, **aqp;
    va_list ap;
    va_start (ap, fmt);

#if DO_QUEUEING	
#ifdef HAVE_VSPRINTF
    vsprintf (buf, fmt, ap);
#else
    {
	int x1, x2, x3, x4, x5, x6, x7, x8;
	x1 = va_arg (ap, int);
	x2 = va_arg (ap, int);
	x3 = va_arg (ap, int);
	x4 = va_arg (ap, int);
	x5 = va_arg (ap, int);
	x6 = va_arg (ap, int);
	x7 = va_arg (ap, int);
	x8 = va_arg (ap, int);
	sprintf (buf, fmt, x1, x2, x3, x4, x5, x6, x7, x8);
    }
#endif
    aqp = &aq_head;
    prev = 0;
    while (*aqp != 0)
	prev = *aqp, aqp = &(*aqp)->next;
    if (prev != 0 && ! prev->done) {
	char *tmpbuf = prev->buf;
	prev->buf = (char *) xmalloc (strlen (tmpbuf) + strlen (buf) + 1);
	strcpy (prev->buf, tmpbuf);
	strcat (prev->buf, buf);
    } else {
	struct amode_queue *tmp = (struct amode_queue *) xmalloc (sizeof *tmp);
	tmp->next = 0;
	tmp->buf = xstrdup (buf);
	*aqp = tmp;
    }
#else
    start_brace ();
    vprintf (fmt, ap);
#endif
}

static void done_queue (void)
{
    struct amode_queue *prev, **aqp;
    aqp = &aq_head;
    prev = 0;
    while (*aqp != 0)
	prev = *aqp, aqp = &(*aqp)->next;
    if (prev)
	prev->done = 1;
}

static void flush_amodes (void)
{
    while (aq_head != 0) {
	struct amode_queue *q = aq_head;
	aq_head = q->next;
	start_brace ();
	printf (q->buf);
	free (q->buf);
	free (q);
    }
}

static void sync_m68k_pc (void)
{
    flush_amodes ();
    if (m68k_pc_offset == 0)
	return;
    printf ("m68k_incpc(%d);\n", m68k_pc_offset);
    switch (m68k_pc_offset) {
     case 0:
	/*fprintf (stderr, "refilling prefetch at 0\n"); */
	break;
     case 2:
	fill_prefetch_2 ();
	break;
     default:
	fill_prefetch_0 ();
	break;
    }
    m68k_pc_offset = 0;
}

/* getv == 1: fetch data; getv != 0: check for odd address. If movem != 0,
 * the calling routine handles Apdi and Aipi modes. */
static void genamode (amodes mode, char *reg, wordsizes size, char *name, int getv, int movem)
{
    int queued = 0;

    switch (mode) {
     case Dreg:
	if (movem)
	    abort ();
	start_brace ();
	if (getv == 1)
	    switch (size) {
	     case sz_byte:
#ifdef AMIGA
		/* sam: I don't know why gcc.2.7.2.1 produces a code worse */
		/* if it is not done like that: */
		printf ("\tSInt8 %s = ((UInt8*)&m68k_dreg(regs, %s))[3];\n", name, reg);
#else
		printf ("\tSInt8 %s = m68k_dreg(regs, %s);\n", name, reg);
#endif
		break;
	     case sz_word:
#ifdef AMIGA
		printf ("\tSInt16 %s = ((SInt16*)&m68k_dreg(regs, %s))[1];\n", name, reg);
#else
		printf ("\tSInt16 %s = m68k_dreg(regs, %s);\n", name, reg);
#endif
		break;
	     case sz_long:
		printf ("\tSInt32 %s = m68k_dreg(regs, %s);\n", name, reg);
		break;
	     default:
		abort ();
	    }
	return;
     case Areg:
	if (movem)
	    abort ();
	start_brace ();
	if (getv == 1)
	    switch (size) {
	     case sz_word:
		printf ("\tSInt16 %s = m68k_areg(regs, %s);\n", name, reg);
		break;
	     case sz_long:
		printf ("\tSInt32 %s = m68k_areg(regs, %s);\n", name, reg);
		break;
	     default:
		abort ();
	    }
	return;
     case Aind:
	start_brace ();
	printf ("\tPtr32 %sa = m68k_areg(regs, %s);\n", name, reg);
	break;
     case Aipi:
	start_brace ();
	printf ("\tPtr32 %sa = m68k_areg(regs, %s);\n", name, reg);
	break;
     case Apdi:
	start_brace ();
	switch (size) {
	 case sz_byte:
	    if (movem)
		printf ("\tPtr32 %sa = m68k_areg(regs, %s);\n", name, reg);
	    else
		printf ("\tPtr32 %sa = m68k_areg(regs, %s) - areg_byteinc[%s];\n", name, reg, reg);
	    break;
	 case sz_word:
	    printf ("\tPtr32 %sa = m68k_areg(regs, %s) - %d;\n", name, reg, movem ? 0 : 2);
	    break;
	 case sz_long:
	    printf ("\tPtr32 %sa = m68k_areg(regs, %s) - %d;\n", name, reg, movem ? 0 : 4);
	    break;
	 default:
	    abort ();
	}
	break;
     case Ad16:
	start_brace ();
	printf ("\tPtr32 %sa = m68k_areg(regs, %s) + (SInt32)(SInt16)%s;\n", name, reg, gen_nextiword ());
	break;
     case Ad8r:
	if (cpu_level > 1) {
	    if (next_cpu_level < 1)
		next_cpu_level = 1;
	    sync_m68k_pc ();
	    start_brace ();
	    printf ("\tPtr32 %sa = get_disp_ea_020(m68k_areg(regs, %s), next_iword());\n", name, reg);
	} else {
	    start_brace ();
	    printf ("\tPtr32 %sa = get_disp_ea_000(m68k_areg(regs, %s), %s);\n", name, reg, gen_nextiword ());
	}
	break;
     case PC16:
	queue_amode ("\tPtr32 %sa = m68k_getpc () + %d;\n", name, m68k_pc_offset);
	queue_amode ("\t%sa += (SInt32)(SInt16)%s;\n", name, gen_nextiword ());
	queued = 1;
	break;
     case PC8r:
	if (cpu_level > 1) {
	    if (next_cpu_level < 1)
		next_cpu_level = 1;
	    flush_amodes ();
	    sync_m68k_pc ();
	    start_brace ();
	    printf ("\tPtr32 tmppc = m68k_getpc();\n");
	    printf ("\tPtr32 %sa = get_disp_ea_020(tmppc, next_iword());\n", name);
	} else {
	    queue_amode ("\tPtr32 tmppc = m68k_getpc() + %d;\n", m68k_pc_offset);
	    queue_amode ("\tPtr32 %sa = get_disp_ea_000(tmppc, %s);\n", name, gen_nextiword ());
	    queued = 1;
	}

	break;
     case absw:
	queue_amode ("\tPtr32 %sa = (SInt32)(SInt16)%s;\n", name, gen_nextiword ());
	queued = 1;
	break;
     case absl:
	queue_amode ("\tPtr32 %sa = %s;\n", name, gen_nextilong ());
	queued = 1;
	break;
     case imm:
	if (getv != 1)
	    abort ();
	switch (size) {
	 case sz_byte:
	    queue_amode ("\tSInt8 %s = %s;\n", name, gen_nextibyte ());
	    break;
	 case sz_word:
	    queue_amode ("\tSInt16 %s = %s;\n", name, gen_nextiword ());
	    break;
	 case sz_long:
	    queue_amode ("\tSInt32 %s = %s;\n", name, gen_nextilong ());
	    break;
	 default:
	    abort ();
	}
	return;
     case imm0:
	if (getv != 1)
	    abort ();
	queue_amode ("\tSInt8 %s = %s;\n", name, gen_nextibyte ());
	return;
     case imm1:
	if (getv != 1)
	    abort ();
	queue_amode ("\tSInt16 %s = %s;\n", name, gen_nextiword ());
	return;
     case imm2:
	if (getv != 1)
	    abort ();
	queue_amode ("\tSInt32 %s = %s;\n", name, gen_nextilong ());
	return;
     case immi:
	if (getv != 1)
	    abort ();
	printf ("\tUInt32 %s = %s;\n", name, reg);
	return;
     default:
	abort ();
    }

    if (queued)
	done_queue ();

    /* We get here for all non-reg non-immediate addressing modes to
     * actually fetch the value. */

    if (using_exception_3 && getv != 0 && size != sz_byte) {
	if (queued) {
	    flush_amodes ();
	    queued = 0;
	}
	    
	printf ("\tif ((%sa & 1) != 0) {\n", name);
	printf ("\t\tlast_fault_for_exception_3 = %sa;\n", name);
	printf ("\t\tlast_op_for_exception_3 = opcode;\n");
	printf ("\t\tlast_addr_for_exception_3 = m68k_getpc() + %d;\n", m68k_pc_offset);
	printf ("\t\tException(3, 0);\n");
	printf ("\t\tgoto %s;\n", endlabelstr);
	printf ("\t}\n");
	need_endlabel = 1;
	start_brace ();
    }
    /* We now might have to fix up the register for pre-dec or post-inc
     * addressing modes. */
    if (!movem)
	switch (mode) {
	 case Aipi:
	    if (queued)
		abort ();
	    switch (size) {
	     case sz_byte:
		printf ("\tm68k_areg(regs, %s) += areg_byteinc[%s];\n", reg, reg);
		break;
	     case sz_word:
		printf ("\tm68k_areg(regs, %s) += 2;\n", reg);
		break;
	     case sz_long:
		printf ("\tm68k_areg(regs, %s) += 4;\n", reg);
		break;
	     default:
		abort ();
	    }
	    break;
	 case Apdi:
	    if (queued)
		abort ();
	    printf ("\tm68k_areg (regs, %s) = %sa;\n", reg, name);
	    break;
	 default:
	    break;
	}
    if (getv == 1) {
	switch (size) {
	 case sz_byte: insn_n_cycles += 2; break;
	 case sz_word: insn_n_cycles += 2; break;
	 case sz_long: insn_n_cycles += 4; break;
	 default: abort ();
	}
	if (queued)
	    switch (size) {
	     case sz_byte: queue_amode ("\tSInt8 %s = get_byte(%sa);\n", name, name); break;
	     case sz_word: queue_amode ("\tSInt16 %s = get_word(%sa);\n", name, name); break;
	     case sz_long: queue_amode ("\tSInt32 %s = get_long(%sa);\n", name, name); break;
	     default: abort ();
	    }
	else {
	    start_brace ();
	    switch (size) {
	     case sz_byte: printf ("\tSInt8 %s = get_byte(%sa);\n", name, name); break;
	     case sz_word: printf ("\tSInt16 %s = get_word(%sa);\n", name, name); break;
	     case sz_long: printf ("\tSInt32 %s = get_long(%sa);\n", name, name); break;
	     default: abort ();
	    }
	}
    }
}

static void genastore (char *from, amodes mode, char *reg, wordsizes size, char *to)
{
    switch (mode) {
     case Dreg:
	switch (size) {
	 case sz_byte:
	    printf ("\tm68k_dreg(regs, %s) = (m68k_dreg(regs, %s) & ~0xff) | ((%s) & 0xff);\n", reg, reg, from);
	    break;
	 case sz_word:
	    printf ("\tm68k_dreg(regs, %s) = (m68k_dreg(regs, %s) & ~0xffff) | ((%s) & 0xffff);\n", reg, reg, from);
	    break;
	 case sz_long:
	    printf ("\tm68k_dreg(regs, %s) = (%s);\n", reg, from);
	    break;
	 default:
	    abort ();
	}
	break;
     case Areg:
	switch (size) {
	 case sz_word:
	    fprintf (stderr, "Foo\n");
	    printf ("\tm68k_areg(regs, %s) = (SInt32)(SInt16)(%s);\n", reg, from);
	    break;
	 case sz_long:
	    printf ("\tm68k_areg(regs, %s) = (%s);\n", reg, from);
	    break;
	 default:
	    abort ();
	}
	break;
     case Aind:
     case Aipi:
     case Apdi:
     case Ad16:
     case Ad8r:
     case absw:
     case absl:
     case PC16:
     case PC8r:
	if (using_prefetch)
	    sync_m68k_pc ();
	switch (size) {
	 case sz_byte:
	    insn_n_cycles += 2;
	    printf ("\tput_byte(%sa,%s);\n", to, from);
	    break;
	 case sz_word:
	    insn_n_cycles += 2;
	    if (cpu_level < 2 && (mode == PC16 || mode == PC8r))
		abort ();
	    printf ("\tput_word(%sa,%s);\n", to, from);
	    break;
	 case sz_long:
	    insn_n_cycles += 4;
	    if (cpu_level < 2 && (mode == PC16 || mode == PC8r))
		abort ();
	    printf ("\tput_long(%sa,%s);\n", to, from);
	    break;
	 default:
	    abort ();
	}
	break;
     case imm:
     case imm0:
     case imm1:
     case imm2:
     case immi:
	abort ();
	break;
     default:
	abort ();
    }
}

static void genmovemel (UInt16 opcode)
{
    char getcode[100];
    int size = table68k[opcode].size == sz_long ? 4 : 2;

    if (table68k[opcode].size == sz_long) {
	strcpy (getcode, "get_long(srca)");
    } else {
	strcpy (getcode, "(SInt32)(SInt16)get_word(srca)");
    }

    printf ("\tUInt16 mask = %s;\n", gen_nextiword ());
    printf ("\tunsigned int dmask = mask & 0xff, amask = (mask >> 8) & 0xff;\n");
    genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "src", 2, 1);
    flush_amodes ();
    start_brace ();
    printf ("\twhile (dmask) { m68k_dreg(regs, movem_index1[dmask]) = %s; srca += %d; dmask = movem_next[dmask]; }\n",
	    getcode, size);
    printf ("\twhile (amask) { m68k_areg(regs, movem_index1[amask]) = %s; srca += %d; amask = movem_next[amask]; }\n",
	    getcode, size);

    if (table68k[opcode].dmode == Aipi)
	printf ("\tm68k_areg(regs, dstreg) = srca;\n");
}

static void genmovemle (UInt16 opcode)
{
    char putcode[100];
    int size = table68k[opcode].size == sz_long ? 4 : 2;
    if (table68k[opcode].size == sz_long) {
	strcpy (putcode, "put_long(srca,");
    } else {
	strcpy (putcode, "put_word(srca,");
    }

    printf ("\tUInt16 mask = %s;\n", gen_nextiword ());
    genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "src", 2, 1);
    flush_amodes ();
    if (using_prefetch)
	sync_m68k_pc ();

    start_brace ();
    if (table68k[opcode].dmode == Apdi) {
	printf ("\tUInt16 amask = mask & 0xff, dmask = (mask >> 8) & 0xff;\n");
	printf ("\twhile (amask) { srca -= %d; %s m68k_areg(regs, movem_index2[amask])); amask = movem_next[amask]; }\n",
		size, putcode);
	printf ("\twhile (dmask) { srca -= %d; %s m68k_dreg(regs, movem_index2[dmask])); dmask = movem_next[dmask]; }\n",
		size, putcode);
	printf ("\tm68k_areg(regs, dstreg) = srca;\n");
    } else {
	printf ("\tUInt16 dmask = mask & 0xff, amask = (mask >> 8) & 0xff;\n");
	printf ("\twhile (dmask) { %s m68k_dreg(regs, movem_index1[dmask])); srca += %d; dmask = movem_next[dmask]; }\n",
		putcode, size);
	printf ("\twhile (amask) { %s m68k_areg(regs, movem_index1[amask])); srca += %d; amask = movem_next[amask]; }\n",
		putcode, size);
    }
}

typedef enum {
    flag_logical, flag_add, flag_sub, flag_cmp, flag_addx, flag_subx, flag_zn,
    flag_av, flag_sv
} flagtypes;

static void genflags_normal (flagtypes type, wordsizes size, char *value, char *src, char *dst)
{
    char vstr[100], sstr[100], dstr[100];
    char usstr[100], udstr[100];
    char unsstr[100], undstr[100];

    switch (size) {
     case sz_byte:
	strcpy (vstr, "((SInt8)(");
	strcpy (usstr, "((UInt8)(");
	break;
     case sz_word:
	strcpy (vstr, "((SInt16)(");
	strcpy (usstr, "((UInt16)(");
	break;
     case sz_long:
	strcpy (vstr, "((SInt32)(");
	strcpy (usstr, "((UInt32)(");
	break;
     default:
	abort ();
    }
    strcpy (unsstr, usstr);

    strcpy (sstr, vstr);
    strcpy (dstr, vstr);
    strcat (vstr, value);
    strcat (vstr, "))");
    strcat (dstr, dst);
    strcat (dstr, "))");
    strcat (sstr, src);
    strcat (sstr, "))");

    strcpy (udstr, usstr);
    strcat (udstr, dst);
    strcat (udstr, "))");
    strcat (usstr, src);
    strcat (usstr, "))");

    strcpy (undstr, unsstr);
    strcat (unsstr, "-");
    strcat (undstr, "~");
    strcat (undstr, dst);
    strcat (undstr, "))");
    strcat (unsstr, src);
    strcat (unsstr, "))");

    switch (type) {
     case flag_logical:
     case flag_zn:
     case flag_av:
     case flag_sv:
     case flag_addx:
     case flag_subx:
	break;

     case flag_add:
	start_brace ();
	printf ("UInt32 %s = %s + %s;\n", value, dstr, sstr);
	break;
     case flag_sub:
     case flag_cmp:
	start_brace ();
	printf ("UInt32 %s = %s - %s;\n", value, dstr, sstr);
	break;
    }


    switch (type) {
     case flag_logical:
     case flag_zn:
	break;

     case flag_add:
     case flag_sub:
     case flag_addx:
     case flag_subx:
     case flag_cmp:
     case flag_av:
     case flag_sv:
	start_brace ();
	printf ("\t" BOOL_TYPE " flgs = %s < 0;\n", sstr);
	printf ("\t" BOOL_TYPE " flgo = %s < 0;\n", dstr);
	printf ("\t" BOOL_TYPE " flgn = %s < 0;\n", vstr);
	break;
    }

    switch (type) {
     case flag_logical:
	printf ("\tVFLG = CFLG = 0;\n");
	printf ("\tZFLG = %s == 0;\n", vstr);
	printf ("\tNFLG = %s < 0;\n", vstr);
	break;
     case flag_av:
	printf ("\tVFLG = (flgs ^ flgn) & (flgo ^ flgn);\n");
	break;
     case flag_sv:
	printf ("\tVFLG = (flgs ^ flgo) & (flgn ^ flgo);\n");
	break;
     case flag_zn:
	printf ("\tif (%s != 0) ZFLG = 0;\n", vstr);
	printf ("\tNFLG = %s < 0;\n", vstr);
	break;
     case flag_add:
	printf ("\tZFLG = %s == 0;\n", vstr);
	printf ("\tVFLG = (flgs ^ flgn) & (flgo ^ flgn);\n");
	printf ("\tCFLG = XFLG = %s < %s;\n", undstr, usstr);
	printf ("\tNFLG = flgn != 0;\n");
	break;
     case flag_sub:
	printf ("\tZFLG = %s == 0;\n", vstr);
	printf ("\tVFLG = (flgs ^ flgo) & (flgn ^ flgo);\n");
	printf ("\tCFLG = XFLG = %s > %s;\n", usstr, udstr);
	printf ("\tNFLG = flgn != 0;\n");
	break;
     case flag_addx:
#if 0
	printf ("\tVFLG = (flgs && flgo && !flgn) || (!flgs && !flgo && flgn);\n"); /* minterm SON: 0x42 */
	printf ("\tXFLG = CFLG = (flgs && flgo) || (!flgn && (flgo || flgs));\n"); /* minterm SON: 0xD4 */
#else
	printf ("\tVFLG = (flgs ^ flgn) & (flgo ^ flgn);\n"); /* minterm SON: 0x42 */
	printf ("\tXFLG = CFLG = flgs ^ ((flgs ^ flgo) & (flgo ^ flgn));\n"); /* minterm SON: 0xD4 */
#endif
	break;
     case flag_subx:
#if 0
	printf ("\tVFLG = (!flgs && flgo && !flgn) || (flgs && !flgo && flgn);\n"); /* minterm SON: 0x24 */
	printf ("\tXFLG = CFLG = (flgs && !flgo) || (flgn && (!flgo || flgs));\n"); /* minterm SON: 0xB2 */
#else
	printf ("\tVFLG = (flgs ^ flgo) & (flgo ^ flgn);\n"); /* minterm SON: 0x24 */
	printf ("\tXFLG = CFLG = flgs ^ ((flgs ^ flgn) & (flgo ^ flgn));\n"); /* minterm SON: 0xB2 */
#endif
	break;
     case flag_cmp:
	printf ("\tZFLG = %s == 0;\n", vstr);
	printf ("\tVFLG = (flgs != flgo) && (flgn != flgo);\n");
	printf ("\tCFLG = %s > %s;\n", usstr, udstr);
	printf ("\tNFLG = flgn != 0;\n");
	break;
    }
}

static void genflags (flagtypes type, wordsizes size, char *value, char *src, char *dst)
{
#ifdef X86_ASSEMBLY
    start_brace ();
    printf ("\tUInt32 scratch;\n");
    switch (type) {
     case flag_add:
     case flag_sub:
	start_brace ();
	printf ("\tUInt32 %s;\n", value);
	break;

     default:
	break;
    }

    /* At least some of those casts are fairly important! */
    switch (type) {
     case flag_logical:
	if (strcmp (value, "0") == 0) {
	    printf ("\t*(UInt32 *)&regflags = 64;\n");
	} else {
	    switch (size) {
	     case sz_byte: printf ("\tx86_flag_testb ((SInt8)(%s));\n", value); break;
	     case sz_word: printf ("\tx86_flag_testw ((SInt16)(%s));\n", value); break;
	     case sz_long: printf ("\tx86_flag_testl ((SInt32)(%s));\n", value); break;
	    }
	}
	return;

     case flag_add:
	switch (size) {
	 case sz_byte: printf ("\tx86_flag_addb (%s, (SInt8)(%s), (SInt8)(%s));\n", value, src, dst); break;
	 case sz_word: printf ("\tx86_flag_addw (%s, (SInt16)(%s), (SInt16)(%s));\n", value, src, dst); break;
	 case sz_long: printf ("\tx86_flag_addl (%s, (SInt32)(%s), (SInt32)(%s));\n", value, src, dst); break;
	}
	return;

     case flag_sub:
	switch (size) {
	 case sz_byte: printf ("\tx86_flag_subb (%s, (SInt8)(%s), (SInt8)(%s));\n", value, src, dst); break;
	 case sz_word: printf ("\tx86_flag_subw (%s, (SInt16)(%s), (SInt16)(%s));\n", value, src, dst); break;
	 case sz_long: printf ("\tx86_flag_subl (%s, (SInt32)(%s), (SInt32)(%s));\n", value, src, dst); break;
	}
	return;

     case flag_cmp:
	switch (size) {
	 case sz_byte: printf ("\tx86_flag_cmpb ((SInt8)(%s), (SInt8)(%s));\n", src, dst); break;
	 case sz_word: printf ("\tx86_flag_cmpw ((SInt16)(%s), (SInt16)(%s));\n", src, dst); break;
	 case sz_long: printf ("\tx86_flag_cmpl ((SInt32)(%s), (SInt32)(%s));\n", src, dst); break;
	}
	return;
	
     default:
	break;
    }
#elif defined(M68K_FLAG_OPT)
    /* sam: here I'm cloning what X86_ASSEMBLY does */
#define EXT(size)  (size==sz_byte?"b":(size==sz_word?"w":"l"))
#define CAST(size) (size==sz_byte?"SInt8":(size==sz_word?"SInt16":"SInt32"))
    switch (type) {
     case flag_add:
     case flag_sub:
	start_brace ();
	printf ("\tUInt32 %s;\n", value);
	break;

     default:
	break;
    }

    switch (type) {
     case flag_logical:
	if (strcmp (value, "0") == 0) {
	    printf ("\t*(UInt16 *)&regflags = 4;\n");	/* Z = 1 */
	} else {
	    printf ("\tm68k_flag_tst (%s, (%s)(%s));\n",
		    EXT (size), CAST (size), value);
	}
	return;

     case flag_add:
	printf ("\t{UInt16 ccr;\n");
	printf ("\tm68k_flag_add (%s, (%s)%s, (%s)(%s), (%s)(%s));\n",
		EXT (size), CAST (size), value, CAST (size), src, CAST (size), dst);
	printf ("\t((UInt16*)&regflags)[1]=((UInt16*)&regflags)[0]=ccr;}\n");
	return;

     case flag_sub:
	printf ("\t{UInt16 ccr;\n");
	printf ("\tm68k_flag_sub (%s, (%s)%s, (%s)(%s), (%s)(%s));\n",
		EXT (size), CAST (size), value, CAST (size), src, CAST (size), dst);
	printf ("\t((UInt16*)&regflags)[1]=((UInt16*)&regflags)[0]=ccr;}\n");
	return;

     case flag_cmp:
	printf ("\tm68k_flag_cmp (%s, (%s)(%s), (%s)(%s));\n",
		EXT (size), CAST (size), src, CAST (size), dst);
	return;

     default:
	break;
    }
#elif defined(ACORN_FLAG_OPT) && defined(__GNUC_MINOR__)
/*
 * This is new. Might be quite buggy.
 */
    switch (type) {
     case flag_av:
     case flag_sv:
     case flag_zn:
     case flag_addx:
     case flag_subx:
	break;

     case flag_logical:
	if (strcmp (value, "0") == 0) {
	    /* v=c=n=0 z=1 */
	    printf ("\t*(ULONG*)&regflags = 0x40000000;\n");
	    return;
	} else {
	    start_brace ();
	    switch (size) {
	     case sz_byte:
		printf ("\tUBYTE ccr;\n");
		printf ("\tULONG shift;\n");
		printf ("\t__asm__(\"mov %%2,%%1,lsl#24\n\ttst %%2,%%2\n\tmov %%0,r15,lsr#24\n\tbic %%0,%%0,#0x30\"\n"
			"\t: \"=r\" (ccr) : \"r\" (%s), \"r\" (shift) : \"cc\" );\n", value);
		printf ("\t*((UBYTE*)&regflags+3) = ccr;\n");
		return;
	     case sz_word:
		printf ("\tUBYTE ccr;\n");
		printf ("\tULONG shift;\n");
		printf ("\t__asm__(\"mov %%2,%%1,lsl#16\n\ttst %%2,%%2\n\tmov %%0,r15,lsr#24\n\tbic %%0,%%0,#0x30\"\n"
			"\t: \"=r\" (ccr) : \"r\" ((WORD)%s), \"r\" (shift) : \"cc\" );\n", value);
		printf ("\t*((UBYTE*)&regflags+3) = ccr;\n");
		return;
	     case sz_long:
		printf ("\tUBYTE ccr;\n");
		printf ("\t__asm__(\"tst %%1,%%1\n\tmov %%0,r15,lsr#24\n\tbic %%0,%%0,#0x30\"\n"
			"\t: \"=r\" (ccr) : \"r\" ((LONG)%s) : \"cc\" );\n", value);
		printf ("\t*((UBYTE*)&regflags+3) = ccr;\n");
		return;
	    }
	}
	break;
     case flag_add:
	if (strcmp (dst, "0") == 0) {
	    printf ("/* Error! Hier muss Peter noch was machen !!! (ADD-Flags) */");
	} else {
	    start_brace ();
	    switch (size) {
	     case sz_byte:
		printf ("\tULONG ccr, shift, %s;\n", value);
		printf ("\t__asm__(\"mov %%4,%%3,lsl#24\n\tadds %%0,%%4,%%2,lsl#24\n\tmov %%0,%%0,asr#24\n\tmov %%1,r15\n\torr %%1,%%1,%%1,lsr#29\"\n"
			"\t: \"=r\" (%s), \"=r\" (ccr) : \"r\" (%s), \"r\" (%s), \"r\" (shift) : \"cc\" );\n", value, src, dst);
		printf ("\t*(ULONG*)&regflags = ccr;\n");
		return;
	     case sz_word:
		printf ("\tULONG ccr, shift, %s;\n", value);
		printf ("\t__asm__(\"mov %%4,%%3,lsl#16\n\tadds %%0,%%4,%%2,lsl#16\n\tmov %%0,%%0,asr#16\n\tmov %%1,r15\n\torr %%1,%%1,%%1,lsr#29\"\n"
			"\t: \"=r\" (%s), \"=r\" (ccr) : \"r\" ((WORD)%s), \"r\" ((WORD)%s), \"r\" (shift) : \"cc\" );\n", value, src, dst);
		printf ("\t*(ULONG*)&regflags = ccr;\n");
		return;
	     case sz_long:
		printf ("\tULONG ccr, %s;\n", value);
		printf ("\t__asm__(\"adds %%0,%%3,%%2\n\tmov %%1,r15\n\torr %%1,%%1,%%1,lsr#29\"\n"
			"\t: \"=r\" (%s), \"=r\" (ccr) : \"r\" ((LONG)%s), \"r\" ((LONG)%s) : \"cc\" );\n", value, src, dst);
		printf ("\t*(ULONG*)&regflags = ccr;\n");
		return;
	    }
	}
	break;
     case flag_sub:
	if (strcmp (dst, "0") == 0) {
	    printf ("/* Error! Hier muss Peter noch was machen !!! (SUB-Flags) */");
	} else {
	    start_brace ();
	    switch (size) {
	     case sz_byte:
		printf ("\tULONG ccr, shift, %s;\n", value);
		printf ("\t__asm__(\"mov %%4,%%3,lsl#24\n\tsubs %%0,%%4,%%2,lsl#24\n\tmov %%0,%%0,asr#24\n\tmov %%1,r15\n\teor %%1,%%1,#0x20000000\n\torr %%1,%%1,%%1,lsr#29\"\n"
			"\t: \"=r\" (%s), \"=r\" (ccr) : \"r\" (%s), \"r\" (%s), \"r\" (shift) : \"cc\" );\n", value, src, dst);
		printf ("\t*(ULONG*)&regflags = ccr;\n");
		return;
	     case sz_word:
		printf ("\tULONG ccr, shift, %s;\n", value);
		printf ("\t__asm__(\"mov %%4,%%3,lsl#16\n\tsubs %%0,%%4,%%2,lsl#16\n\tmov %%0,%%0,asr#16\n\tmov %%1,r15\n\teor %%1,%%1,#0x20000000\n\torr %%1,%%1,%%1,lsr#29\"\n"
			"\t: \"=r\" (%s), \"=r\" (ccr) : \"r\" ((WORD)%s), \"r\" ((WORD)%s), \"r\" (shift) : \"cc\" );\n", value, src, dst);
		printf ("\t*(ULONG*)&regflags = ccr;\n");
		return;
	     case sz_long:
		printf ("\tULONG ccr, %s;\n", value);
		printf ("\t__asm__(\"subs %%0,%%3,%%2\n\tmov %%1,r15\n\teor %%1,%%1,#0x20000000\n\torr %%1,%%1,%%1,lsr#29\"\n"
			"\t: \"=r\" (%s), \"=r\" (ccr) : \"r\" ((LONG)%s), \"r\" ((LONG)%s) : \"cc\" );\n", value, src, dst);
		printf ("\t*(ULONG*)&regflags = ccr;\n");
		return;
	    }
	}
	break;
     case flag_cmp:
	if (strcmp (dst, "0") == 0) {
	    printf ("/*Error! Hier muss Peter noch was machen !!! (CMP-Flags)*/");
	} else {
	    start_brace ();
	    switch (size) {
	     case sz_byte:
		printf ("\tULONG shift, ccr;\n");
		printf ("\t__asm__(\"mov %%3,%%2,lsl#24\n\tcmp %%3,%%1,lsl#24\n\tmov %%0,r15,lsr#24\n\teor %%0,%%0,#0x20\"\n"
			"\t: \"=r\" (ccr) : \"r\" (%s), \"r\" (%s), \"r\" (shift) : \"cc\" );\n", src, dst);
		printf ("\t*((UBYTE*)&regflags+3) = ccr;\n");
		return;
	     case sz_word:
		printf ("\tULONG shift, ccr;\n");
		printf ("\t__asm__(\"mov %%3,%%2,lsl#16\n\tcmp %%3,%%1,lsl#16\n\tmov %%0,r15,lsr#24\n\teor %%0,%%0,#0x20\"\n"
			"\t: \"=r\" (ccr) : \"r\" ((WORD)%s), \"r\" ((WORD)%s), \"r\" (shift) : \"cc\" );\n", src, dst);
		printf ("\t*((UBYTE*)&regflags+3) = ccr;\n");
		return;
	     case sz_long:
		printf ("\tULONG ccr;\n");
		printf ("\t__asm__(\"cmp %%2,%%1\n\tmov %%0,r15,lsr#24\n\teor %%0,%%0,#0x20\"\n"
			"\t: \"=r\" (ccr) : \"r\" ((LONG)%s), \"r\" ((LONG)%s) : \"cc\" );\n", src, dst);
		printf ("\t*((UBYTE*)&regflags+3) = ccr;\n");
		/*printf ("\tprintf (\"%%08x %%08x %%08x\\n\", %s, %s, *((ULONG*)&regflags));\n", src, dst); */
		return;
	    }
	}
	break;
    }
#endif
    genflags_normal (type, size, value, src, dst);
}

static void gen_opcode (unsigned long int opcode)
{
    aq_head = 0;
    insn_n_cycles = 2;

    start_brace ();
#if 0
    printf ("UInt8 *m68k_pc = regs.pc_p;\n");
#endif
    m68k_pc_offset = 2;
    switch (table68k[opcode].plev) {
     case 0: /* not privileged */
	break;
     case 1: /* unprivileged only on 68000 */
	if (cpu_level == 0)
	    break;
	if (next_cpu_level < 0)
	    next_cpu_level = 0;

	/* fall through */
     case 2: /* priviledged */
	printf ("if (!regs.s) { Exception(8,0); goto %s; }\n", endlabelstr);
	need_endlabel = 1;
	start_brace ();
	break;
     case 3: /* privileged if size == word */
	if (table68k[opcode].size == sz_byte)
	    break;
	printf ("if (!regs.s) { Exception(8,0); goto %s; }\n", endlabelstr);
	need_endlabel = 1;
	start_brace ();
	break;
    }
    switch (table68k[opcode].mnemo) {
     case i_OR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	printf ("\tsrc |= dst;\n");
	genflags (flag_logical, table68k[opcode].size, "src", "", "");
	genastore ("src", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_AND:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	printf ("\tsrc &= dst;\n");
	genflags (flag_logical, table68k[opcode].size, "src", "", "");
	genastore ("src", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_EOR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	printf ("\tsrc ^= dst;\n");
	genflags (flag_logical, table68k[opcode].size, "src", "", "");
	genastore ("src", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_ORSR:
	printf ("\tMakeSR();\n");
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_byte) {
	    printf ("\tsrc &= 0xFF;\n");
	}
	printf ("\tregs.sr |= src;\n");
	printf ("\tMakeFromSR();\n");
	break;
     case i_ANDSR:
	printf ("\tMakeSR();\n");
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_byte) {
	    printf ("\tsrc |= 0xFF00;\n");
	}
	printf ("\tregs.sr &= src;\n");
	printf ("\tMakeFromSR();\n");
	break;
     case i_EORSR:
	printf ("\tMakeSR();\n");
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_byte) {
	    printf ("\tsrc &= 0xFF;\n");
	}
	printf ("\tregs.sr ^= src;\n");
	printf ("\tMakeFromSR();\n");
	break;
     case i_SUB:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	genflags (flag_sub, table68k[opcode].size, "newv", "src", "dst");
	genastore ("newv", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_SUBA:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", sz_long, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt32 newv = dst - src;\n");
	genastore ("newv", table68k[opcode].dmode, "dstreg", sz_long, "dst");
	break;
     case i_SUBX:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt32 newv = dst - src - (XFLG ? 1 : 0);\n");
	genflags (flag_subx, table68k[opcode].size, "newv", "src", "dst");
	genflags (flag_zn, table68k[opcode].size, "newv", "", "");
	genastore ("newv", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_SBCD:
	/* Let's hope this works... */
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt16 newv_lo = (dst & 0xF) - (src & 0xF) - (XFLG ? 1 : 0);\n");
	printf ("\tUInt16 newv_hi = (dst & 0xF0) - (src & 0xF0);\n");
	printf ("\tUInt16 newv;\n");
	printf ("\tif (newv_lo > 9) { newv_lo-=6; newv_hi-=0x10; }\n");
	printf ("\tnewv = newv_hi + (newv_lo & 0xF);");
	printf ("\tCFLG = XFLG = (newv_hi & 0x1F0) > 0x90;\n");
	printf ("\tif (CFLG) newv -= 0x60;\n");
	genflags (flag_zn, table68k[opcode].size, "newv", "", "");
	genflags (flag_sv, table68k[opcode].size, "newv", "src", "dst");
	genastore ("newv", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_ADD:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	genflags (flag_add, table68k[opcode].size, "newv", "src", "dst");
	genastore ("newv", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_ADDA:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", sz_long, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt32 newv = dst + src;\n");
	genastore ("newv", table68k[opcode].dmode, "dstreg", sz_long, "dst");
	break;
     case i_ADDX:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt32 newv = dst + src + (XFLG ? 1 : 0);\n");
	genflags (flag_addx, table68k[opcode].size, "newv", "src", "dst");
	genflags (flag_zn, table68k[opcode].size, "newv", "", "");
	genastore ("newv", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_ABCD:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt16 newv_lo = (src & 0xF) + (dst & 0xF) + (XFLG ? 1 : 0);\n");
	printf ("\tUInt16 newv_hi = (src & 0xF0) + (dst & 0xF0);\n");
	printf ("\tUInt16 newv;\n");
	printf ("\tif (newv_lo > 9) { newv_lo +=6; }\n");
	printf ("\tnewv = newv_hi + newv_lo;");
	printf ("\tCFLG = XFLG = (newv & 0x1F0) > 0x90;\n");
	printf ("\tif (CFLG) newv += 0x60;\n");
	genflags (flag_zn, table68k[opcode].size, "newv", "", "");
	genflags (flag_sv, table68k[opcode].size, "newv", "src", "dst");
	genastore ("newv", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_NEG:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	start_brace ();
	genflags (flag_sub, table68k[opcode].size, "dst", "src", "0");
	genastore ("dst", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	break;
     case i_NEGX:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt32 newv = 0 - src - (XFLG ? 1 : 0);\n");
	genflags (flag_subx, table68k[opcode].size, "newv", "src", "0");
	genflags (flag_zn, table68k[opcode].size, "newv", "", "");
	genastore ("newv", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	break;
     case i_NBCD:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt16 newv_lo = - (src & 0xF) - (XFLG ? 1 : 0);\n");
	printf ("\tUInt16 newv_hi = - (src & 0xF0);\n");
	printf ("\tUInt16 newv;\n");
	printf ("\tif (newv_lo > 9) { newv_lo-=6; newv_hi-=0x10; }\n");
	printf ("\tnewv = newv_hi + (newv_lo & 0xF);");
	printf ("\tCFLG = XFLG = (newv_hi & 0x1F0) > 0x90;\n");
	printf ("\tif (CFLG) newv -= 0x60;\n");
	genflags (flag_zn, table68k[opcode].size, "newv", "", "");
	genastore ("newv", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	break;
     case i_CLR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 2, 0);
	flush_amodes ();
	genflags (flag_logical, table68k[opcode].size, "0", "", "");
	genastore ("0", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	break;
     case i_NOT:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt32 dst = ~src;\n");
	genflags (flag_logical, table68k[opcode].size, "dst", "", "");
	genastore ("dst", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	break;
     case i_TST:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	genflags (flag_logical, table68k[opcode].size, "src", "", "");
	break;
     case i_BTST:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_byte)
	    printf ("\tsrc &= 7;\n");
	else
	    printf ("\tsrc &= 31;\n");
	printf ("\tZFLG = !(dst & (1 << src));\n");
	break;
     case i_BCHG:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_byte)
	    printf ("\tsrc &= 7;\n");
	else
	    printf ("\tsrc &= 31;\n");
	printf ("\tZFLG = !(dst & (1 << src));\n");
	printf ("\tdst ^= (1 << src);\n");
	genastore ("dst", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_BCLR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_byte)
	    printf ("\tsrc &= 7;\n");
	else
	    printf ("\tsrc &= 31;\n");
	printf ("\tZFLG = !(dst & (1 << src));\n");
	printf ("\tdst &= ~(1 << src);\n");
	genastore ("dst", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_BSET:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_byte)
	    printf ("\tsrc &= 7;\n");
	else
	    printf ("\tsrc &= 31;\n");
	printf ("\tZFLG = !(dst & (1 << src));\n");
	printf ("\tdst |= (1 << src);\n");
	genastore ("dst", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_CMPM:
     case i_CMP:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	genflags (flag_cmp, table68k[opcode].size, "newv", "src", "dst");
	break;
     case i_CMPA:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", sz_long, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	genflags (flag_cmp, sz_long, "newv", "src", "dst");
	break;
	/* The next two are coded a little unconventional, but they are doing
	 * weird things... */
     case i_MVPRM:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();

	printf ("\tPtr32 memp = m68k_areg(regs, dstreg) + (SInt32)(SInt16)%s;\n", gen_nextiword ());
	if (table68k[opcode].size == sz_word) {
	    printf ("\tput_byte(memp, src >> 8); put_byte(memp + 2, src);\n");
	} else {
	    printf ("\tput_byte(memp, src >> 24); put_byte(memp + 2, src >> 16);\n");
	    printf ("\tput_byte(memp + 4, src >> 8); put_byte(memp + 6, src);\n");
	}
	break;
     case i_MVPMR:
	printf ("\tPtr32 memp = m68k_areg(regs, srcreg) + (SInt32)(SInt16)%s;\n", gen_nextiword ());
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 2, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_word) {
	    printf ("\tUInt16 val = (get_byte(memp) << 8) + get_byte(memp + 2);\n");
	} else {
	    printf ("\tUInt32 val = (get_byte(memp) << 24) + (get_byte(memp + 2) << 16)\n");
	    printf ("              + (get_byte(memp + 4) << 8) + get_byte(memp + 6);\n");
	}
	genastore ("val", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_MOVE:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 2, 0);
	flush_amodes ();
	genflags (flag_logical, table68k[opcode].size, "src", "", "");
	genastore ("src", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_MOVEA:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 2, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_word) {
	    printf ("\tUInt32 val = (SInt32)(SInt16)src;\n");
	} else {
	    printf ("\tUInt32 val = src;\n");
	}
	genastore ("val", table68k[opcode].dmode, "dstreg", sz_long, "dst");
	break;
     case i_MVSR2:
	genamode (table68k[opcode].smode, "srcreg", sz_word, "src", 2, 0);
	flush_amodes ();
	printf ("\tMakeSR();\n");
	if (table68k[opcode].size == sz_byte)
	    genastore ("regs.sr & 0xff", table68k[opcode].smode, "srcreg", sz_word, "src");
	else
	    genastore ("regs.sr", table68k[opcode].smode, "srcreg", sz_word, "src");
	break;
     case i_MV2SR:
	genamode (table68k[opcode].smode, "srcreg", sz_word, "src", 1, 0);
	flush_amodes ();
	if (table68k[opcode].size == sz_byte)
	    printf ("\tMakeSR();\n\tregs.sr &= 0xFF00;\n\tregs.sr |= src & 0xFF;\n");
	else {
	    printf ("\tregs.sr = src;\n");
	}
	printf ("\tMakeFromSR();\n");
	break;
     case i_SWAP:
	genamode (table68k[opcode].smode, "srcreg", sz_long, "src", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt32 dst = ((src >> 16)&0xFFFF) | ((src&0xFFFF)<<16);\n");
	genflags (flag_logical, sz_long, "dst", "", "");
	genastore ("dst", table68k[opcode].smode, "srcreg", sz_long, "src");
	break;
     case i_EXG:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	genastore ("dst", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	genastore ("src", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_EXT:
	genamode (table68k[opcode].smode, "srcreg", sz_long, "src", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 dst = (SInt32)(SInt8)src;\n"); break;
	 case sz_word: printf ("\tUInt16 dst = (SInt16)(SInt8)src;\n"); break;
	 case sz_long: printf ("\tUInt32 dst = (SInt32)(SInt16)src;\n"); break;
	 default: abort ();
	}
	genflags (flag_logical,
		  table68k[opcode].size == sz_word ? sz_word : sz_long, "dst", "", "");
	genastore ("dst", table68k[opcode].smode, "srcreg",
	    table68k[opcode].size == sz_word ? sz_word : sz_long, "src");
	break;
     case i_MVMEL:
	genmovemel (opcode);
	break;
     case i_MVMLE:
	genmovemle (opcode);
	break;
     case i_TRAP:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	sync_m68k_pc ();
	printf ("\tException(src+32,0);\n");
	m68k_pc_offset = 0;
	break;
     case i_MVR2USP:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	printf ("\tregs.usp = src;\n");
	break;
     case i_MVUSP2R:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 2, 0);
	flush_amodes ();
	genastore ("regs.usp", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	break;
     case i_RESET:
	printf ("\tcustomreset();\n");
	break;
     case i_NOP:
	break;
     case i_STOP:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	printf ("\tregs.sr = src;\n");
	printf ("\tMakeFromSR();\n");
	printf ("\tm68k_setstopped(1);\n");
	break;
     case i_RTE:
	if (cpu_level == 0) {
	    genamode (Aipi, "7", sz_word, "sr", 1, 0);
	    genamode (Aipi, "7", sz_long, "pc", 1, 0);
	    flush_amodes ();
	    printf ("\tregs.sr = sr; m68k_setpc_rte(pc);\n");
	    fill_prefetch_0 ();
	    printf ("\tMakeFromSR();\n");
	} else {
	    int old_brace_level = n_braces;
	    if (next_cpu_level < 0)
		next_cpu_level = 0;
	    printf ("\tUInt16 newsr; UInt32 newpc; for (;;) {\n");
	    genamode (Aipi, "7", sz_word, "sr", 1, 0);
	    genamode (Aipi, "7", sz_long, "pc", 1, 0);
	    genamode (Aipi, "7", sz_word, "format", 1, 0);
	    flush_amodes ();
	    printf ("\tnewsr = sr; newpc = pc;\n");
	    printf ("\tif ((format & 0xF000) == 0x0000) { break; }\n");
	    printf ("\telse if ((format & 0xF000) == 0x1000) { ; }\n");
	    printf ("\telse if ((format & 0xF000) == 0x2000) { m68k_areg(regs, 7) += 4; break; }\n");
	    printf ("\telse if ((format & 0xF000) == 0x8000) { m68k_areg(regs, 7) += 50; break; }\n");
	    printf ("\telse if ((format & 0xF000) == 0x9000) { m68k_areg(regs, 7) += 12; break; }\n");
	    printf ("\telse if ((format & 0xF000) == 0xa000) { m68k_areg(regs, 7) += 24; break; }\n");
	    printf ("\telse if ((format & 0xF000) == 0xb000) { m68k_areg(regs, 7) += 84; break; }\n");
	    printf ("\telse { Exception(14,0); goto %s; }\n", endlabelstr);
	    printf ("\tregs.sr = newsr; MakeFromSR();\n}\n");
	    pop_braces (old_brace_level);
	    printf ("\tregs.sr = newsr; MakeFromSR();\n");
	    printf ("\tm68k_setpc_rte(newpc);\n");
	    fill_prefetch_0 ();
	    need_endlabel = 1;
	}
	/* PC is set and prefetch filled. */
	m68k_pc_offset = 0;
	break;
     case i_RTD:
	printf ("\tcompiler_flush_jsr_stack();\n");
	genamode (Aipi, "7", sz_long, "pc", 1, 0);
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "offs", 1, 0);
	flush_amodes ();
	printf ("\tm68k_areg(regs, 7) += offs;\n");
	printf ("\tm68k_setpc_rte(pc);\n");
	fill_prefetch_0 ();
	/* PC is set and prefetch filled. */
	m68k_pc_offset = 0;
	break;
     case i_LINK:
	genamode (Apdi, "7", sz_long, "old", 2, 0);
	genamode (table68k[opcode].smode, "srcreg", sz_long, "src", 1, 0);
	flush_amodes ();
	genastore ("src", Apdi, "7", sz_long, "old");
	genastore ("m68k_areg(regs, 7)", table68k[opcode].smode, "srcreg", sz_long, "src");
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "offs", 1, 0);
	flush_amodes ();
	printf ("\tm68k_areg(regs, 7) += offs;\n");
	break;
     case i_UNLK:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	printf ("\tm68k_areg(regs, 7) = src;\n");
	genamode (Aipi, "7", sz_long, "old", 1, 0);
	flush_amodes ();
	genastore ("old", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	break;
     case i_RTS:
	printf ("\tm68k_do_rts();\n");
	fill_prefetch_0 ();
	m68k_pc_offset = 0;
	break;
     case i_TRAPV:
	sync_m68k_pc ();
	printf ("\tif(VFLG) { Exception(7,m68k_getpc()); goto %s; }\n", endlabelstr);
	need_endlabel = 1;
	break;
     case i_RTR:
	printf ("\tcompiler_flush_jsr_stack();\n");
	printf ("\tMakeSR();\n");
	genamode (Aipi, "7", sz_word, "sr", 1, 0);
	genamode (Aipi, "7", sz_long, "pc", 1, 0);
	flush_amodes ();
	printf ("\tregs.sr &= 0xFF00; sr &= 0xFF;\n");
	printf ("\tregs.sr |= sr; m68k_setpc(pc);\n");
	fill_prefetch_0 ();
	printf ("\tMakeFromSR();\n");
	m68k_pc_offset = 0;
	break;
     case i_JSR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 0, 0);
	flush_amodes ();
	printf ("\tm68k_do_jsr(m68k_getpc() + %d, srca);\n", m68k_pc_offset);
	fill_prefetch_0 ();
	m68k_pc_offset = 0;
	break;
     case i_JMP:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 0, 0);
	flush_amodes ();
	printf ("\tm68k_setpc(srca);\n");
	fill_prefetch_0 ();
	m68k_pc_offset = 0;
	break;
     case i_BSR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	printf ("\tSInt32 s = (SInt32)src + 2;\n");
	if (using_exception_3) {
	    printf ("\tif (src & 1) {\n");
	    printf ("\tlast_addr_for_exception_3 = m68k_getpc() + 2;\n");
	    printf ("\t\tlast_fault_for_exception_3 = m68k_getpc() + s;\n");
	    printf ("\t\tlast_op_for_exception_3 = opcode; Exception(3,0); goto %s;\n", endlabelstr);
	    printf ("\t}\n");
	    need_endlabel = 1;
	}
	printf ("\tm68k_do_bsr(m68k_getpc() + %d, s);\n", m68k_pc_offset);
	fill_prefetch_0 ();
	m68k_pc_offset = 0;
	break;
     case i_Bcc:
	if (table68k[opcode].size == sz_long) {
	    if (cpu_level < 2) {
		printf ("\tm68k_incpc(2);\n");
		printf ("\tif (!cctrue(%d)) goto %s;\n", table68k[opcode].cc, endlabelstr);
		printf ("\t\tlast_addr_for_exception_3 = m68k_getpc() + 2;\n");
		printf ("\t\tlast_fault_for_exception_3 = m68k_getpc() + 1;\n");
		printf ("\t\tlast_op_for_exception_3 = opcode; Exception(3,0); goto %s;\n", endlabelstr);
		need_endlabel = 1;
	    } else {
		if (next_cpu_level < 1)
		    next_cpu_level = 1;
	    }
	}
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	printf ("\tif (!cctrue(%d)) goto didnt_jump;\n", table68k[opcode].cc);
	if (using_exception_3) {
	    printf ("\tif (src & 1) {\n");
	    printf ("\t\tlast_addr_for_exception_3 = m68k_getpc() + 2;\n");
	    printf ("\t\tlast_fault_for_exception_3 = m68k_getpc() + 2 + (SInt32)src;\n");
	    printf ("\t\tlast_op_for_exception_3 = opcode; Exception(3,0); goto %s;\n", endlabelstr);
	    printf ("\t}\n");
	    need_endlabel = 1;
	}
#ifdef USE_COMPILER
	printf ("\tm68k_setpc_bcc(m68k_getpc() + 2 + (SInt32)src);\n");
#else
	printf ("\tm68k_incpc ((SInt32)src + 2);\n");
#endif
	fill_prefetch_0 ();
	printf ("\tgoto %s;\n", endlabelstr);
	printf ("didnt_jump:;\n");
	need_endlabel = 1;
	break;
     case i_LEA:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 0, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 2, 0);
	flush_amodes ();
	genastore ("srca", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	break;
     case i_PEA:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 0, 0);
	genamode (Apdi, "7", sz_long, "dst", 2, 0);
	flush_amodes ();
	genastore ("srca", Apdi, "7", sz_long, "dst");
	break;
     case i_DBcc:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "offs", 1, 0);
	flush_amodes ();

	printf ("\tif (!cctrue(%d)) {\n", table68k[opcode].cc);
	genastore ("(src-1)", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");

	printf ("\t\tif (src) {\n");
	if (using_exception_3) {
	    printf ("\t\t\tif (offs & 1) {\n");
	    printf ("\t\t\tlast_addr_for_exception_3 = m68k_getpc() + 2;\n");
	    printf ("\t\t\tlast_fault_for_exception_3 = m68k_getpc() + 2 + (SInt32)offs + 2;\n");
	    printf ("\t\t\tlast_op_for_exception_3 = opcode; Exception(3,0); goto %s;\n", endlabelstr);
	    printf ("\t\t}\n");
	    need_endlabel = 1;
	}
#ifdef USE_COMPILER
	printf ("\t\t\tm68k_setpc_bcc(m68k_getpc() + (SInt32)offs + 2);\n");
#else
	printf ("\t\t\tm68k_incpc((SInt32)offs + 2);\n");
#endif
	fill_prefetch_0 ();
	printf ("\t\tgoto %s;\n", endlabelstr);
	printf ("\t\t}\n");
	printf ("\t}\n");
	need_endlabel = 1;
	break;
     case i_Scc:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 2, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tint val = cctrue(%d) ? 0xff : 0;\n", table68k[opcode].cc);
	genastore ("val", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	break;
     case i_DIVU:
	printf ("\tPtr32 oldpc = m68k_getpc();\n");
	genamode (table68k[opcode].smode, "srcreg", sz_word, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", sz_long, "dst", 1, 0);
	flush_amodes ();
	printf ("\tif(src == 0) { Exception(5,oldpc); goto %s; } else {\n", endlabelstr);
	printf ("\tUInt32 newv = (UInt32)dst / (UInt32)(UInt16)src;\n");
	printf ("\tUInt32 rem = (UInt32)dst %% (UInt32)(UInt16)src;\n");
	/* The N flag appears to be set each time there is an overflow.
	 * Weird. */
	printf ("\tif (newv > 0xffff) { VFLG = NFLG = 1; CFLG = 0; } else\n\t{\n");
	genflags (flag_logical, sz_word, "newv", "", "");
	printf ("\tnewv = (newv & 0xffff) | ((UInt32)rem << 16);\n");
	genastore ("newv", table68k[opcode].dmode, "dstreg", sz_long, "dst");
	printf ("\t}\n");
	printf ("\t}\n");
	insn_n_cycles += 68;
	need_endlabel = 1;
	break;
     case i_DIVS:
	printf ("\tPtr32 oldpc = m68k_getpc();\n");
	genamode (table68k[opcode].smode, "srcreg", sz_word, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", sz_long, "dst", 1, 0);
	flush_amodes ();
	printf ("\tif(src == 0) { Exception(5,oldpc); goto %s; } else {\n", endlabelstr);
	printf ("\tSInt32 newv = (SInt32)dst / (SInt32)(SInt16)src;\n");
	printf ("\tUInt16 rem = (SInt32)dst %% (SInt32)(SInt16)src;\n");
	printf ("\tif ((newv & 0xffff8000) != 0 && (newv & 0xffff8000) != 0xffff8000) { VFLG = NFLG = 1; CFLG = 0; } else\n\t{\n");
	printf ("\tif (((SInt16)rem < 0) != ((SInt32)dst < 0)) rem = -rem;\n");
	genflags (flag_logical, sz_word, "newv", "", "");
	printf ("\tnewv = (newv & 0xffff) | ((UInt32)rem << 16);\n");
	genastore ("newv", table68k[opcode].dmode, "dstreg", sz_long, "dst");
	printf ("\t}\n");
	printf ("\t}\n");
	insn_n_cycles += 72;
	need_endlabel = 1;
	break;
     case i_MULU:
	genamode (table68k[opcode].smode, "srcreg", sz_word, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", sz_word, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt32 newv = (UInt32)(UInt16)dst * (UInt32)(UInt16)src;\n");
	genflags (flag_logical, sz_long, "newv", "", "");
	genastore ("newv", table68k[opcode].dmode, "dstreg", sz_long, "dst");
	insn_n_cycles += 32;
	break;
     case i_MULS:
	genamode (table68k[opcode].smode, "srcreg", sz_word, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", sz_word, "dst", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tUInt32 newv = (SInt32)(SInt16)dst * (SInt32)(SInt16)src;\n");
	genflags (flag_logical, sz_long, "newv", "", "");
	genastore ("newv", table68k[opcode].dmode, "dstreg", sz_long, "dst");
	insn_n_cycles += 32;
	break;
     case i_CHK:
	printf ("\tPtr32 oldpc = m68k_getpc();\n");
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	printf ("\tif ((SInt32)dst < 0) { NFLG=1; Exception(6,oldpc); goto %s; }\n", endlabelstr);
	printf ("\telse if (dst > src) { NFLG=0; Exception(6,oldpc); goto %s; }\n", endlabelstr);
	need_endlabel = 1;
	break;

     case i_CHK2:
	printf ("\tPtr32 oldpc = m68k_getpc();\n");
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 2, 0);
	flush_amodes ();
	printf ("\t{SInt32 upper,lower,reg = regs.regs[(extra >> 12) & 15];\n");
	switch (table68k[opcode].size) {
	 case sz_byte:
	    printf ("\tlower=(SInt32)(SInt8)get_byte(dsta); upper = (SInt32)(SInt8)get_byte(dsta+1);\n");
	    printf ("\tif ((extra & 0x8000) == 0) reg = (SInt32)(SInt8)reg;\n");
	    break;
	 case sz_word:
	    printf ("\tlower=(SInt32)(SInt16)get_word(dsta); upper = (SInt32)(SInt16)get_word(dsta+2);\n");
	    printf ("\tif ((extra & 0x8000) == 0) reg = (SInt32)(SInt16)reg;\n");
	    break;
	 case sz_long:
	    printf ("\tlower=get_long(dsta); upper = get_long(dsta+4);\n");
	    break;
	 default:
	    abort ();
	}
	printf ("\tZFLG=upper == reg || lower == reg;\n");
	printf ("\tCFLG=lower <= upper ? reg < lower || reg > upper : reg > upper || reg < lower;\n");
	printf ("\tif ((extra & 0x800) && CFLG) { Exception(6,oldpc); goto %s; }\n}\n", endlabelstr);
	need_endlabel = 1;
	break;

     case i_ASR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "cnt", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 sign = cmask & val;\n");
	printf ("\tcnt &= 63;\n");
	printf ("\tVFLG = 0;\n");
	printf ("\tif (!cnt) { CFLG = 0; } else {\n");
	printf ("\tif (cnt >= %d) {\n", bit_size (table68k[opcode].size));
	printf ("\t\tval = sign ? %s : 0;\n", bit_mask (table68k[opcode].size));
	printf ("\t\tCFLG=XFLG= sign ? 1 : 0;\n");
	printf ("\t} else {\n");
	printf ("\t\tCFLG=XFLG=(val >> (cnt-1)) & 1;\n");
	printf ("\t\tval >>= cnt;\n");
	printf ("\t\tif (sign) val |= %s << (%d - cnt);\n",
		bit_mask (table68k[opcode].size),
		bit_size (table68k[opcode].size));
	printf ("\t}}\n");
	printf ("\tNFLG = sign != 0;\n");
	printf ("\tZFLG = val == 0;\n");
	genastore ("val", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data");
	break;
     case i_ASL:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "cnt", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tcnt &= 63;\n");
	printf ("\tVFLG = 0;\n");
	printf ("\tif (!cnt) { CFLG = 0; } else {\n");
	printf ("\tif (cnt >= %d) {\n", bit_size (table68k[opcode].size));
	printf ("\t\tVFLG = val != 0;\n");
	printf ("\t\tCFLG=XFLG = cnt == %d ? val & 1 : 0;\n",
		bit_size (table68k[opcode].size));
	printf ("\t\tval = 0;\n");
	printf ("\t} else {\n");
	printf ("\t\tUInt32 mask = (%s << (%d - cnt)) & %s;\n",
		bit_mask (table68k[opcode].size),
		bit_size (table68k[opcode].size) - 1,
		bit_mask (table68k[opcode].size));
	printf ("\t\tCFLG=XFLG=(val << (cnt-1)) & cmask ? 1 : 0;\n");
	printf ("\t\tVFLG = (val & mask) != mask && (val & mask) != 0;\n");
	printf ("\t\tval <<= cnt;\n");
	printf ("\t}}\n");
	printf ("\tNFLG = (val&cmask) != 0;\n");
	printf ("\tZFLG = val == 0;\n");
	genastore ("val", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data");
	break;
     case i_LSR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "cnt", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tcnt &= 63;\n");
	printf ("\tif (!cnt) { CFLG = 0; } else {\n");
	printf ("\tif (cnt >= %d) {\n", bit_size (table68k[opcode].size));
	printf ("\t\tCFLG=XFLG = cnt == %d ? (val & cmask ? 1 : 0) : 0;\n",
		bit_size (table68k[opcode].size));
	printf ("\t\tval = 0;\n");
	printf ("\t} else {\n");
	printf ("\t\tCFLG=XFLG=(val >> (cnt-1)) & 1;\n");
	printf ("\t\tval >>= cnt;\n");
	printf ("\t}}\n");
	printf ("\tNFLG = (val & cmask) != 0; ZFLG = val == 0; VFLG = 0;\n");
	genastore ("val", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data");
	break;
     case i_LSL:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "cnt", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tcnt &= 63;\n");
	printf ("\tif (!cnt) { CFLG = 0; } else {\n");
	printf ("\tif (cnt >= %d) {\n", bit_size (table68k[opcode].size));
	printf ("\t\tCFLG=XFLG = cnt == %d ? val & 1 : 0;\n",
		bit_size (table68k[opcode].size));
	printf ("\t\tval = 0;\n");
	printf ("\t} else {\n");
	printf ("\t\tCFLG=XFLG=(val << (cnt-1)) & cmask ? 1 : 0;\n");
	printf ("\t\tval <<= cnt;\n");
	printf ("\t}}\n");
	printf ("\tNFLG = (val & cmask) != 0; ZFLG = val == 0; VFLG = 0;\n");
	genastore ("val", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data");
	break;
     case i_ROL:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "cnt", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tcnt &= 63;\n");
	printf ("\tif (!cnt) { CFLG = 0; } else {\n");
	printf ("\tUInt32 carry;\n");
	printf ("\tfor(;cnt;--cnt){\n");
	printf ("\tcarry=val&cmask; val <<= 1;\n");
	printf ("\tif(carry) val |= 1;\n");
	printf ("\t}\n");
	printf ("\tCFLG = carry!=0;\n}\n");
	printf ("\tNFLG = (val & cmask) != 0; ZFLG = val == 0; VFLG = 0;\n");
	genastore ("val", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data");
	break;
     case i_ROR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "cnt", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tcnt &= 63;\n");
	printf ("\tif (!cnt) { CFLG = 0; } else {");
	printf ("\tUInt32 carry;\n");
	printf ("\tfor(;cnt;--cnt){\n");
	printf ("\tcarry=val&1; val = val >> 1;\n");
	printf ("\tif(carry) val |= cmask;\n");
	printf ("\t}\n");
	printf ("\tCFLG = carry;\n}\n");
	printf ("\tNFLG = (val & cmask) != 0; ZFLG = val == 0; VFLG = 0;\n");
	genastore ("val", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data");
	break;
     case i_ROXL:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "cnt", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 carry;\n");
	printf ("\tcnt &= 63;\n");
	printf ("\tfor(;cnt;--cnt){\n");
	printf ("\tcarry=val&cmask; val <<= 1;\n");
	printf ("\tif(XFLG) val |= 1;\n");
	printf ("\tXFLG = carry != 0;\n");
	printf ("\t}\n");
	printf ("\tCFLG = XFLG;\n");
	printf ("\tNFLG = (val & cmask) != 0; ZFLG = val == 0; VFLG = 0;\n");
	genastore ("val", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data");
	break;
     case i_ROXR:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "cnt", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 carry;\n");
	printf ("\tcnt &= 63;\n");
	printf ("\tfor(;cnt;--cnt){\n");
	printf ("\tcarry=val&1; val >>= 1;\n");
	printf ("\tif(XFLG) val |= cmask;\n");
	printf ("\tXFLG = carry;\n");
	printf ("\t}\n");
	printf ("\tCFLG = XFLG;\n");
	printf ("\tNFLG = (val & cmask) != 0; ZFLG = val == 0; VFLG = 0;\n");
	genastore ("val", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "data");
	break;
     case i_ASRW:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	printf ("\tVFLG = 0;\n");
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 sign = cmask & val;\n");
	printf ("\tCFLG=XFLG=val&1; val = (val >> 1) | sign;\n");
	printf ("\tNFLG = sign != 0;\n");
	printf ("\tZFLG = val == 0;\n");
	genastore ("val", table68k[opcode].smode, "srcreg", table68k[opcode].size, "data");
	break;
     case i_ASLW:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	printf ("\tVFLG = 0;\n");
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 sign = cmask & val;\n");
	printf ("\tCFLG=XFLG=(val&cmask)!=0; val <<= 1;\n");
	printf ("\tif ((val&cmask)!=sign) VFLG=1;\n");
	printf ("\tNFLG = (val&cmask) != 0;\n");
	printf ("\tZFLG = val == 0;\n");
	genastore ("val", table68k[opcode].smode, "srcreg", table68k[opcode].size, "data");
	break;
     case i_LSRW:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 carry = val&1;\n");
	printf ("\tval >>= 1;\n");
	genflags (flag_logical, table68k[opcode].size, "val", "", "");
	printf ("CFLG = XFLG = carry!=0;\n");
	genastore ("val", table68k[opcode].smode, "srcreg", table68k[opcode].size, "data");
	break;
     case i_LSLW:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 carry = val&cmask;\n");
	printf ("\tval <<= 1;\n");
	genflags (flag_logical, table68k[opcode].size, "val", "", "");
	printf ("CFLG = XFLG = carry!=0;\n");
	genastore ("val", table68k[opcode].smode, "srcreg", table68k[opcode].size, "data");
	break;
     case i_ROLW:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 carry = val&cmask;\n");
	printf ("\tval <<= 1;\n");
	printf ("\tif(carry)  val |= 1;\n");
	genflags (flag_logical, table68k[opcode].size, "val", "", "");
	printf ("CFLG = carry!=0;\n");
	genastore ("val", table68k[opcode].smode, "srcreg", table68k[opcode].size, "data");
	break;
     case i_RORW:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 carry = val&1;\n");
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tval >>= 1;\n");
	printf ("\tif(carry) val |= cmask;\n");
	genflags (flag_logical, table68k[opcode].size, "val", "", "");
	printf ("CFLG = carry!=0;\n");
	genastore ("val", table68k[opcode].smode, "srcreg", table68k[opcode].size, "data");
	break;
     case i_ROXLW:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 carry = val&cmask;\n");
	printf ("\tval <<= 1;\n");
	printf ("\tif(XFLG) val |= 1;\n");
	printf ("\tXFLG = carry != 0;\n");
	genflags (flag_logical, table68k[opcode].size, "val", "", "");
	printf ("XFLG = CFLG = carry!=0;\n");
	genastore ("val", table68k[opcode].smode, "srcreg", table68k[opcode].size, "data");
	break;
     case i_ROXRW:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "data", 1, 0);
	flush_amodes ();
	start_brace ();
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt8 val = data;\n"); break;
	 case sz_word: printf ("\tUInt16 val = data;\n"); break;
	 case sz_long: printf ("\tUInt32 val = data;\n"); break;
	 default: abort ();
	}
	printf ("\tUInt32 carry = val&1;\n");
	switch (table68k[opcode].size) {
	 case sz_byte: printf ("\tUInt32 cmask = 0x80;\n"); break;
	 case sz_word: printf ("\tUInt32 cmask = 0x8000;\n"); break;
	 case sz_long: printf ("\tUInt32 cmask = 0x80000000;\n"); break;
	 default: abort ();
	}
	printf ("\tval >>= 1;\n");
	printf ("\tif(XFLG) val |= cmask;\n");
	printf ("\tXFLG = carry != 0;\n");
	genflags (flag_logical, table68k[opcode].size, "val", "", "");
	printf ("XFLG = CFLG = carry!=0;\n");
	genastore ("val", table68k[opcode].smode, "srcreg", table68k[opcode].size, "data");
	break;
     case i_MOVEC2:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tint regno = (src >> 12) & 15;\n");
	printf ("\tUInt32 *regp = regs.regs + regno;\n");
	printf ("\tm68k_movec2(src & 0xFFF, regp);\n");
	break;
     case i_MOVE2C:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tint regno = (src >> 12) & 15;\n");
	printf ("\tUInt32 *regp = regs.regs + regno;\n");
	printf ("\tm68k_move2c(src & 0xFFF, regp);\n");
	break;
     case i_CAS:
	{
	    int old_brace_level;
	    genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	    genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	    flush_amodes ();
	    start_brace ();
	    printf ("\tint ru = (src >> 6) & 7;\n");
	    printf ("\tint rc = src & 7;\n");
	    genflags (flag_cmp, table68k[opcode].size, "newv", "m68k_dreg(regs, rc)", "dst");
	    printf ("\tif (ZFLG)");
	    old_brace_level = n_braces;
	    start_brace ();
	    genastore ("(m68k_dreg(regs, ru))", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	    pop_braces (old_brace_level);
	    printf ("else");
	    start_brace ();
	    printf ("m68k_dreg(regs, rc) = dst;\n");
	    pop_braces (old_brace_level);
	}
	break;
     case i_CAS2:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	flush_amodes ();
	printf ("\tUInt32 rn1 = regs.regs[(extra >> 28) & 7];\n");
	printf ("\tUInt32 rn2 = regs.regs[(extra >> 12) & 7];\n");
	if (table68k[opcode].size == sz_word) {
	    int old_brace_level = n_braces;
	    printf ("\tUInt16 dst1 = get_word(rn1), dst2 = get_word(rn2);\n");
	    genflags (flag_cmp, table68k[opcode].size, "newv", "m68k_dreg(regs, (extra >> 16) & 7)", "dst1");
	    printf ("\tif (ZFLG) {\n");
	    genflags (flag_cmp, table68k[opcode].size, "newv", "m68k_dreg(regs, extra & 7)", "dst2");
	    printf ("\tif (ZFLG) {\n");
	    printf ("\tput_word(rn1, m68k_dreg(regs, (extra >> 22) & 7));\n");
	    printf ("\tput_word(rn1, m68k_dreg(regs, (extra >> 6) & 7));\n");
	    printf ("\t}}\n");
	    pop_braces (old_brace_level);
	    printf ("\tif (!ZFLG) {\n");
	    printf ("\tm68k_dreg(regs, (extra >> 22) & 7) = (m68k_dreg(regs, (extra >> 22) & 7) & ~0xffff) | (dst1 & 0xffff);\n");
	    printf ("\tm68k_dreg(regs, (extra >> 6) & 7) = (m68k_dreg(regs, (extra >> 6) & 7) & ~0xffff) | (dst2 & 0xffff);\n");
	    printf ("\t}\n");
	} else {
	    int old_brace_level = n_braces;
	    printf ("\tUInt32 dst1 = get_long(rn1), dst2 = get_long(rn2);\n");
	    genflags (flag_cmp, table68k[opcode].size, "newv", "m68k_dreg(regs, (extra >> 16) & 7)", "dst1");
	    printf ("\tif (ZFLG) {\n");
	    genflags (flag_cmp, table68k[opcode].size, "newv", "m68k_dreg(regs, extra & 7)", "dst2");
	    printf ("\tif (ZFLG) {\n");
	    printf ("\tput_long(rn1, m68k_dreg(regs, (extra >> 22) & 7));\n");
	    printf ("\tput_long(rn1, m68k_dreg(regs, (extra >> 6) & 7));\n");
	    printf ("\t}}\n");
	    pop_braces (old_brace_level);
	    printf ("\tif (!ZFLG) {\n");
	    printf ("\tm68k_dreg(regs, (extra >> 22) & 7) = dst1;\n");
	    printf ("\tm68k_dreg(regs, (extra >> 6) & 7) = dst2;\n");
	    printf ("\t}\n");
	}
	break;
     case i_MOVES:		/* ignore DFC and SFC because we have no MMU */
	{
	    int old_brace_level;
	    genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	    flush_amodes ();
	    printf ("\tif (extra & 0x800)\n");
	    old_brace_level = n_braces;
	    start_brace ();
	    printf ("\tUInt32 src = regs.regs[(extra >> 12) & 15];\n");
	    genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 2, 0);
	    flush_amodes ();
	    genastore ("src", table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst");
	    pop_braces (old_brace_level);
	    printf ("else");
	    start_brace ();
	    genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "src", 1, 0);
	    flush_amodes ();
	    printf ("\tif (extra & 0x8000) {\n");
	    switch (table68k[opcode].size) {
	     case sz_byte: printf ("\tm68k_areg(regs, (extra >> 12) & 7) = (SInt32)(SInt8)src;\n"); break;
	     case sz_word: printf ("\tm68k_areg(regs, (extra >> 12) & 7) = (SInt32)(SInt16)src;\n"); break;
	     case sz_long: printf ("\tm68k_areg(regs, (extra >> 12) & 7) = src;\n"); break;
	     default: abort ();
	    }
	    printf ("\t} else {\n");
	    genastore ("src", Dreg, "(extra >> 12) & 7", table68k[opcode].size, "");
	    printf ("\t}\n");
	    pop_braces (old_brace_level);
	}
	break;
     case i_BKPT:		/* only needed for hardware emulators */
	sync_m68k_pc ();
	printf ("\top_illg(opcode);\n");
	break;
     case i_CALLM:		/* not present in 68030 */
	sync_m68k_pc ();
	printf ("\top_illg(opcode);\n");
	break;
     case i_RTM:		/* not present in 68030 */
	sync_m68k_pc ();
	printf ("\top_illg(opcode);\n");
	break;
     case i_TRAPcc:
	if (table68k[opcode].smode != am_unknown && table68k[opcode].smode != am_illg)
	    genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "dummy", 1, 0);
	flush_amodes ();
	printf ("\tif (cctrue(%d)) { Exception(7,m68k_getpc()); goto %s; }\n", table68k[opcode].cc, endlabelstr);
	need_endlabel = 1;
	break;
     case i_DIVL:
	sync_m68k_pc ();
	start_brace ();
	printf ("\tPtr32 oldpc = m68k_getpc();\n");
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	sync_m68k_pc ();
	printf ("\tm68k_divl(opcode, dst, extra, oldpc);\n");
	break;
     case i_MULL:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", table68k[opcode].size, "dst", 1, 0);
	flush_amodes ();
	sync_m68k_pc ();
	printf ("\tm68k_mull(opcode, dst, extra);\n");
	break;
     case i_BFTST:
     case i_BFEXTU:
     case i_BFCHG:
     case i_BFEXTS:
     case i_BFCLR:
     case i_BFFFO:
     case i_BFSET:
     case i_BFINS:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	genamode (table68k[opcode].dmode, "dstreg", sz_long, "dst", 2, 0);
	flush_amodes ();
	start_brace ();
	printf ("\tSInt32 offset = extra & 0x800 ? m68k_dreg(regs, (extra >> 6) & 7) : (extra >> 6) & 0x1f;\n");
	printf ("\tint width = (((extra & 0x20 ? m68k_dreg(regs, extra & 7) : extra) -1) & 0x1f) +1;\n");
	if (table68k[opcode].dmode == Dreg) {
	    printf ("\tUInt32 tmp = m68k_dreg(regs, dstreg) << (offset & 0x1f);\n");
	} else {
	    printf ("\tUInt32 tmp,bf0,bf1;\n");
	    printf ("\tdsta += (offset >> 3) | (offset & 0x80000000 ? ~0x1fffffff : 0);\n");
	    printf ("\tbf0 = get_long(dsta);bf1 = get_byte(dsta+4) & 0xff;\n");
	    printf ("\ttmp = (bf0 << (offset & 7)) | (bf1 >> (8 - (offset & 7)));\n");
	}
	printf ("\ttmp >>= (32 - width);\n");
	printf ("\tNFLG = tmp & (1 << (width-1)) ? 1 : 0;ZFLG = tmp == 0;VFLG = 0;CFLG = 0;\n");
	switch (table68k[opcode].mnemo) {
	 case i_BFTST:
	    break;
	 case i_BFEXTU:
	    printf ("\tm68k_dreg(regs, (extra >> 12) & 7) = tmp;\n");
	    break;
	 case i_BFCHG:
	    printf ("\ttmp = ~tmp;\n");
	    break;
	 case i_BFEXTS:
	    printf ("\tif (NFLG) tmp |= width == 32 ? 0 : (-1 << width);\n");
	    printf ("\tm68k_dreg(regs, (extra >> 12) & 7) = tmp;\n");
	    break;
	 case i_BFCLR:
	    printf ("\ttmp = 0;\n");
	    break;
	 case i_BFFFO:
	    printf ("\t{ UInt32 mask = 1 << (width-1);\n");
	    printf ("\twhile (mask) { if (tmp & mask) break; mask >>= 1; offset++; }}\n");
	    printf ("\tm68k_dreg(regs, (extra >> 12) & 7) = offset;\n");
	    break;
	 case i_BFSET:
	    printf ("\ttmp = 0xffffffff;\n");
	    break;
	 case i_BFINS:
	    printf ("\ttmp = m68k_dreg(regs, (extra >> 12) & 7);\n");
	    break;
	 default:
	    break;
	}
	if (table68k[opcode].mnemo == i_BFCHG
	    || table68k[opcode].mnemo == i_BFCLR
	    || table68k[opcode].mnemo == i_BFSET
	    || table68k[opcode].mnemo == i_BFINS)
	{
	    printf ("\ttmp <<= (32 - width);\n");
	    if (table68k[opcode].dmode == Dreg) {
		printf ("\tm68k_dreg(regs, dstreg) = (m68k_dreg(regs, dstreg) & ((offset & 0x1f) == 0 ? 0 :\n");
		printf ("\t\t(0xffffffff << (32 - (offset & 0x1f))))) |\n");
		printf ("\t\t(tmp >> (offset & 0x1f)) |\n");
		printf ("\t\t(((offset & 0x1f) + width) >= 32 ? 0 :\n");
		printf (" (m68k_dreg(regs, dstreg) & ((UInt32)0xffffffff >> ((offset & 0x1f) + width))));\n");
	    } else {
		printf ("\tbf0 = (bf0 & (0xff000000 << (8 - (offset & 7)))) |\n");
		printf ("\t\t(tmp >> (offset & 7)) |\n");
		printf ("\t\t(((offset & 7) + width) >= 32 ? 0 :\n");
		printf ("\t\t (bf0 & ((UInt32)0xffffffff >> ((offset & 7) + width))));\n");
		printf ("\tput_long(dsta,bf0 );\n");
		printf ("\tif (((offset & 7) + width) > 32) {\n");
		printf ("\t\tbf1 = (bf1 & (0xff >> (width - 32 + (offset & 7)))) |\n");
		printf ("\t\t\t(tmp << (8 - (offset & 7)));\n");
		printf ("\t\tput_byte(dsta+4,bf1);\n");
		printf ("\t}\n");
	    }
	}
	break;
     case i_PACK:
	if (table68k[opcode].smode == Dreg) {
	    printf ("\tUInt16 val = m68k_dreg(regs, srcreg) + %s;\n", gen_nextiword ());
	    printf ("\tm68k_dreg(regs, dstreg) = (m68k_dreg(regs, dstreg) & 0xffffff00) | ((val >> 4) & 0xf0) | (val & 0xf);\n");
	} else {
	    printf ("\tUInt16 val;\n");
	    printf ("\tm68k_areg(regs, srcreg) -= areg_byteinc[srcreg];\n");
	    printf ("\tval = (UInt16)get_byte(m68k_areg(regs, srcreg));\n");
	    printf ("\tm68k_areg(regs, srcreg) -= areg_byteinc[srcreg];\n");
	    printf ("\tval = (val | ((UInt16)get_byte(m68k_areg(regs, srcreg)) << 8)) + %s;\n", gen_nextiword ());
	    printf ("\tm68k_areg(regs, dstreg) -= areg_byteinc[dstreg];\n");
	    printf ("\tput_byte(m68k_areg(regs, dstreg),((val >> 4) & 0xf0) | (val & 0xf));\n");
	}
	break;
     case i_UNPK:
	if (table68k[opcode].smode == Dreg) {
	    printf ("\tUInt16 val = m68k_dreg(regs, srcreg);\n");
	    printf ("\tval = (((val << 4) & 0xf00) | (val & 0xf)) + %s;\n", gen_nextiword ());
	    printf ("\tm68k_dreg(regs, dstreg) = (m68k_dreg(regs, dstreg) & 0xffff0000) | (val & 0xffff);\n");
	} else {
	    printf ("\tUInt16 val;\n");
	    printf ("\tm68k_areg(regs, srcreg) -= areg_byteinc[srcreg];\n");
	    printf ("\tval = (UInt16)get_byte(m68k_areg(regs, srcreg));\n");
	    printf ("\tval = (((val << 4) & 0xf00) | (val & 0xf)) + %s;\n", gen_nextiword ());
	    printf ("\tm68k_areg(regs, dstreg) -= areg_byteinc[dstreg];\n");
	    printf ("\tput_byte(m68k_areg(regs, dstreg),val);\n");
	    printf ("\tm68k_areg(regs, dstreg) -= areg_byteinc[dstreg];\n");
	    printf ("\tput_byte(m68k_areg(regs, dstreg),val >> 8);\n");
	}
	break;
     case i_TAS:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "src", 1, 0);
	flush_amodes ();
	genflags (flag_logical, table68k[opcode].size, "src", "", "");
	printf ("\tsrc |= 0x80;\n");
	genastore ("src", table68k[opcode].smode, "srcreg", table68k[opcode].size, "src");
	break;
     case i_FPP:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	flush_amodes ();
	sync_m68k_pc ();
	printf ("\tfpp_opp(opcode,extra);\n");
	break;
     case i_FDBcc:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	flush_amodes ();
	sync_m68k_pc ();
	printf ("\tfdbcc_opp(opcode,extra);\n");
	break;
     case i_FScc:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	flush_amodes ();
	sync_m68k_pc ();
	printf ("\tfscc_opp(opcode,extra);\n");
	break;
     case i_FTRAPcc:
	sync_m68k_pc ();
	start_brace ();
	printf ("\tPtr32 oldpc = m68k_getpc();\n");
	if (table68k[opcode].smode != am_unknown && table68k[opcode].smode != am_illg)
	    genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "dummy", 1, 0);
	flush_amodes ();
	sync_m68k_pc ();
	printf ("\tftrapcc_opp(opcode,oldpc);\n");
	break;
     case i_FBcc:
	sync_m68k_pc ();
	start_brace ();
	printf ("\tPtr32 pc = m68k_getpc();\n");
	genamode (table68k[opcode].dmode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	flush_amodes ();
	sync_m68k_pc ();
	printf ("\tfbcc_opp(opcode,pc,extra);\n");
	break;
     case i_FSAVE:
	sync_m68k_pc ();
	printf ("\tfsave_opp(opcode);\n");
	break;
     case i_FRESTORE:
	sync_m68k_pc ();
	printf ("\tfrestore_opp(opcode);\n");
	break;
     case i_MMUOP:
	genamode (table68k[opcode].smode, "srcreg", table68k[opcode].size, "extra", 1, 0);
	flush_amodes ();
	sync_m68k_pc ();
	printf ("\tmmu_op(opcode,extra);\n");
	break;
     default:
	abort ();
	break;
    }
    finish_braces ();
    sync_m68k_pc ();
}

static void generate_includes (FILE * f)
{
    fprintf (f, "#include \"machdep/m68k.h\"\n");
    fprintf (f, "#include \"machdep/maccess.h\"\n");
    fprintf (f, "#include \"memory.h\"\n");
    fprintf (f, "#include \"readcpu.h\"\n");
    fprintf (f, "#include \"newcpu.h\"\n");
    /*    fprintf (f, "#include \"compiler.h\"\n");*/
    fprintf (f, "#include \"cputbl.h\"\n");
    fprintf (f, "#include \"misc.h\"\n");
}

static int postfix;

static void generate_one_opcode (int rp)
{
    int i;
    UInt16 smsk, dmsk;
    long int opcode = opcode_map[rp];

    if (table68k[opcode].mnemo == i_ILLG
	|| table68k[opcode].clev > cpu_level)
	return;

    for (i = 0; lookuptab[i].name[0]; i++) {
	if (table68k[opcode].mnemo == lookuptab[i].mnemo)
	    break;
    }

    if (table68k[opcode].handler != -1)
	return;

    if (opcode_next_clev[rp] != cpu_level) {
	fprintf (stblfile, "{ op_%lx_%d, 0, %ld }, /* %s */\n", opcode, opcode_last_postfix[rp],
		 opcode, lookuptab[i].name);
	return;
    }
    fprintf (stblfile, "{ op_%lx_%d, 0, %ld }, /* %s */\n", opcode, postfix, opcode, lookuptab[i].name);
    fprintf (headerfile, "extern cpuop_func op_%lx_%d;\n", opcode, postfix);
    printf ("unsigned long op_%lx_%d(UInt32 opcode) /* %s */\n{\n", opcode, postfix, lookuptab[i].name);

    switch (table68k[opcode].stype) {
     case 0: smsk = 7; break;
     case 1: smsk = 255; break;
     case 2: smsk = 15; break;
     case 3: smsk = 7; break;
     case 4: smsk = 7; break;
     case 5: smsk = 63; break;
     default: abort ();
    }
    dmsk = 7;

    next_cpu_level = -1;
    if (table68k[opcode].suse
	&& table68k[opcode].smode != imm && table68k[opcode].smode != imm0
	&& table68k[opcode].smode != imm1 && table68k[opcode].smode != imm2
	&& table68k[opcode].smode != absw && table68k[opcode].smode != absl
	&& table68k[opcode].smode != PC8r && table68k[opcode].smode != PC16)
    {
	if (table68k[opcode].spos == -1) {
	    if (((int) table68k[opcode].sreg) >= 128)
		printf ("\tUInt32 srcreg = (SInt32)(SInt8)%d;\n", (int) table68k[opcode].sreg);
	    else
		printf ("\tUInt32 srcreg = %d;\n", (int) table68k[opcode].sreg);
	} else {
	    char source[100];

	    /* Check that we can do the little endian optimization safely.  */
	    if (table68k[opcode].spos < 8 && (smsk << table68k[opcode].spos) > 255)
		abort ();
	    
	    printf ("#ifdef HAVE_GET_WORD_UNSWAPPED\n");

	    if (table68k[opcode].spos != 8)
		sprintf (source, "((opcode >> %d) & %d)", (int) table68k[opcode].spos ^ 8, smsk);
	    else
		sprintf (source, "(opcode & %d)", smsk);

	    if (table68k[opcode].stype == 3)
		printf ("\tUInt32 srcreg = imm8_table[%s];\n", source);
	    else if (table68k[opcode].stype == 1)
		printf ("\tUInt32 srcreg = (SInt32)(SInt8)%s;\n", source);
	    else
		printf ("\tUInt32 srcreg = %s;\n", source);

	    printf ("#else\n");

	    if (table68k[opcode].spos)
		sprintf (source, "((opcode >> %d) & %d)", (int) table68k[opcode].spos, smsk);
	    else
		sprintf (source, "(opcode & %d)", smsk);

	    if (table68k[opcode].stype == 3)
		printf ("\tUInt32 srcreg = imm8_table[%s];\n", source);
	    else if (table68k[opcode].stype == 1)
		printf ("\tUInt32 srcreg = (SInt32)(SInt8)%s;\n", source);
	    else
		printf ("\tUInt32 srcreg = %s;\n", source);

	    printf ("#endif\n");
	}
    }
    if (table68k[opcode].duse
	/* Yes, the dmode can be imm, in case of LINK or DBcc */
	&& table68k[opcode].dmode != imm && table68k[opcode].dmode != imm0
	&& table68k[opcode].dmode != imm1 && table68k[opcode].dmode != imm2
	&& table68k[opcode].dmode != absw && table68k[opcode].dmode != absl)
    {
	if (table68k[opcode].dpos == -1) {
	    if (((int) table68k[opcode].dreg) >= 128)
		printf ("\tUInt32 dstreg = (SInt32)(SInt8)%d;\n", (int) table68k[opcode].dreg);
	    else
		printf ("\tUInt32 dstreg = %d;\n", (int) table68k[opcode].dreg);
	} else {
	    int pos = table68k[opcode].dpos;
	    /* Check that we can do the little endian optimization safely.  */
	    if (pos < 8 && (dmsk << pos) > 255)
		abort ();
	    
	    printf ("#ifdef HAVE_GET_WORD_UNSWAPPED\n");

	    if (pos != 8)
		printf ("\tUInt32 dstreg = (opcode >> %d) & %d;\n",
			pos ^ 8, dmsk);
	    else
		printf ("\tUInt32 dstreg = opcode & %d;\n", dmsk);

	    printf ("#else\n");

	    if (pos)
		printf ("\tUInt32 dstreg = (opcode >> %d) & %d;\n",
			pos, dmsk);
	    else
		printf ("\tUInt32 dstreg = opcode & %d;\n", dmsk);

	    printf ("#endif\n");
	}
    }
    need_endlabel = 0;
    endlabelno++;
    sprintf (endlabelstr, "endlabel%d", endlabelno);
    gen_opcode (opcode);
    if (need_endlabel)
	printf ("%s: (void)0;\n", endlabelstr);
    printf ("return %d;\n", insn_n_cycles);
    printf ("}\n");
    opcode_next_clev[rp] = next_cpu_level;
    opcode_last_postfix[rp] = postfix;
}

static void generate_func (void)
{
    int i, j, rp;

    using_prefetch = 0;
    using_exception_3 = 0;
    for (i = 0; i < 5; i++) {
	cpu_level = 3 - i;
	if (i == 4) {
	    cpu_level = 0;
	    using_prefetch = 1;
	    using_exception_3 = 1;
	    for (rp = 0; rp < nr_cpuop_funcs; rp++)
		opcode_next_clev[rp] = 0;
	}
	postfix = i;
	fprintf (stblfile, "struct cputbl op_smalltbl_%d[] = {\n", postfix);

	/* sam: this is for people with low memory (eg. me :)) */
	printf ("\n"
                "#if !defined(PART_1) && !defined(PART_2) && "
	 	    "!defined(PART_3) && !defined(PART_4) && "
		    "!defined(PART_5) && !defined(PART_6) && "
		    "!defined(PART_7) && !defined(PART_8)"
		"\n"
	        "#define PART_1 1\n"
	        "#define PART_2 1\n"
	        "#define PART_3 1\n"
	        "#define PART_4 1\n"
	        "#define PART_5 1\n"
	        "#define PART_6 1\n"
	        "#define PART_7 1\n"
	        "#define PART_8 1\n"
	        "#endif\n\n");
	
	rp = 0;
	for(j=1;j<=8;++j) {
		int k = (j*nr_cpuop_funcs)/8;
		printf ("#ifdef PART_%d\n",j);
		for (; rp < k; rp++)
		   generate_one_opcode (rp);
		printf ("#endif\n\n");
	}

	fprintf (stblfile, "{ 0, 0, 0 }};\n");
    }

}

int main (int argc, char **argv)
{
    read_table68k ();
    do_merges ();

    opcode_map = (int *) xmalloc (sizeof (int) * nr_cpuop_funcs);
    opcode_last_postfix = (int *) xmalloc (sizeof (int) * nr_cpuop_funcs);
    opcode_next_clev = (int *) xmalloc (sizeof (int) * nr_cpuop_funcs);
    counts = (unsigned long *) xmalloc (65536 * sizeof (unsigned long));
    read_counts ();

    /* It would be a lot nicer to put all in one file (we'd also get rid of
     * cputbl.h that way), but cpuopti can't cope.  That could be fixed, but
     * I don't dare to touch the 68k version.  */

    headerfile = fopen ("cputbl.h", "wb");
    stblfile = fopen ("cpustbl.c", "wb");
    freopen ("cpuemu.c", "wb", stdout);

    generate_includes (stdout);
    generate_includes (stblfile);

    generate_func ();

    free (table68k);
    return 0;
}
