/* funvars.c - structure, variable, stack, and regexp functions */
/* $Id$ */

#include "copyright.h"
#include "autoconf.h"
#include "config.h"

#include "alloc.h"	/* required by mudconf */
#include "flags.h"	/* required by mudconf */
#include "htab.h"	/* required by mudconf */
#include "mail.h"	/* required by mudconf */
#include "mudconf.h"	/* required by code */

#include "db.h"		/* required by externs */
#include "externs.h"	/* required by code */

#include "functions.h"	/* required by code */
#include "match.h"	/* required by code */
#include "attrs.h"	/* required by code */
#include "powers.h"	/* required by code */
#include "pcre.h"	/* required by code */

static const unsigned char *tables = NULL; /* for PCRE */

/* ---------------------------------------------------------------------------
 * setq, setr, r: set and read global registers.
 */

FUNCTION(fun_setq)
{
	int regnum, len;

	regnum = atoi(fargs[0]);
	if ((regnum < 0) || (regnum >= MAX_GLOBAL_REGS)) {
		safe_str("#-1 INVALID GLOBAL REGISTER", buff, bufc);
		return;
	}
		
	if (!mudstate.global_regs[regnum])
	    mudstate.global_regs[regnum] = alloc_lbuf("fun_setq");
	len = strlen(fargs[1]);
	bcopy(fargs[1], mudstate.global_regs[regnum], len + 1);
	mudstate.glob_reg_len[regnum] = len;
}

FUNCTION(fun_setr)
{
	int regnum, len;

	regnum = atoi(fargs[0]);
	if ((regnum < 0) || (regnum >= MAX_GLOBAL_REGS)) {
		safe_str("#-1 INVALID GLOBAL REGISTER", buff, bufc);
		return;
	}

	if (!mudstate.global_regs[regnum])
	    mudstate.global_regs[regnum] = alloc_lbuf("fun_setr");
	len = strlen(fargs[1]);
	bcopy(fargs[1], mudstate.global_regs[regnum], len + 1);
	mudstate.glob_reg_len[regnum] = len;
	safe_known_str(fargs[1], len, buff, bufc);
}

FUNCTION(fun_r)
{
	int regnum;

	regnum = atoi(fargs[0]);
	if ((regnum < 0) || (regnum >= MAX_GLOBAL_REGS)) {
		safe_str("#-1 INVALID GLOBAL REGISTER", buff, bufc);
	} else if (mudstate.global_regs[regnum]) {
		safe_known_str(mudstate.global_regs[regnum],
			       mudstate.glob_reg_len[regnum], buff, bufc);
	}
}

/* --------------------------------------------------------------------------
 * wildmatch: Set the results of a wildcard match into the global registers.
 *            wildmatch(<string>,<wildcard pattern>[,<register list>])
 */

FUNCTION(fun_wildmatch)
{
    int i, nqregs, curq;
    char *t_args[NUM_ENV_VARS], *qregs[NUM_ENV_VARS]; /* %0-%9 is limiting */

    if (!wild(fargs[1], fargs[0], t_args, NUM_ENV_VARS)) {
	safe_chr('0', buff, bufc);
	return;
    }

    safe_chr('1', buff, bufc);

    /* Parse the list of registers. Anything that we don't get is assumed
     * to be -1. Fill them in.
     */

    nqregs = list2arr(qregs, NUM_ENV_VARS, fargs[2], ' ');
    for (i = 0; i < nqregs; i++) {
	if (qregs[i] && *qregs[i] && is_integer(qregs[i]))
	    curq = atoi(qregs[i]);
	else
	    curq = -1;
	if ((curq < 0) || (curq >= NUM_ENV_VARS))
	    continue;
	if (!mudstate.global_regs[curq]) {
	    mudstate.global_regs[curq] = alloc_lbuf("fun_wildmatch");
	}
	if (!t_args[i] || !*t_args[i]) {
	    mudstate.global_regs[curq][0] = '\0';
	    mudstate.glob_reg_len[curq] = 0;
	} else {
	    mudstate.glob_reg_len[curq] = strlen(t_args[i]);
	    bcopy(t_args[i], mudstate.global_regs[curq],
		  mudstate.glob_reg_len[curq] + 1);
	}
    }

    /* Need to free up allocated memory from the match. */

    for (i = 0; i < NUM_ENV_VARS; i++) {
	if (t_args[i])
	    free_lbuf(t_args[i]);
    }
}

/* --------------------------------------------------------------------------
 * Auxiliary stuff for structures and variables.
 */

#define Set_Max(x,y)     (x) = ((y) > (x)) ? (y) : (x);

static void print_htab_matches(obj, htab, buff, bufc)
    dbref obj;
    HASHTAB *htab;
    char *buff;
    char **bufc;
{
    /* Lists out hashtable matches.
     * Things which use this are computationally expensive, and should
     * be discouraged.
     */

    char tbuf[SBUF_SIZE], *tp, *bb_p;
    HASHENT *hptr;
    int i, len;

    tp = tbuf;
    safe_ltos(tbuf, &tp, obj);
    safe_sb_chr('.', tbuf, &tp);
    *tp = '\0';
    len = strlen(tbuf);

    bb_p = *bufc;

    for (i = 0; i < htab->hashsize; i++) {
	for (hptr = htab->entry->element[i]; hptr != NULL; hptr = hptr->next) {
	    if (!strncmp(tbuf, hptr->target, len)) {
		if (*bufc != bb_p)
		    safe_chr(' ', buff, bufc);
		safe_str((char *) (index(hptr->target, '.') + 1), buff, bufc);
	    }
	}
    }
}

/* ---------------------------------------------------------------------------
 * fun_x: Returns a variable. x(<variable name>)
 * fun_setx: Sets a variable. setx(<variable name>,<value>)
 * fun_xvars: Takes a list, parses it, sets it into variables.
 *            xvars(<space-separated variable list>,<list>,<delimiter>)
 * fun_let: Takes a list of variables and their values, sets them, executes
 *          a function, and clears out the variables. (Scheme/ML-like.)
 *          If <list> is empty, the values are reset to null.
 *          let(<space-separated var list>,<list>,<body>,<delimiter>)
 * fun_lvars: Shows a list of variables associated with that object.
 * fun_clearvars: Clears all variables associated with that object.
 */


static void set_xvar(obj, name, data)
    dbref obj;
    char *name;
    char *data;
{
    VARENT *xvar;
    char tbuf[SBUF_SIZE], *tp, *p;

    /* If we don't have at least one character in the name, toss it. */

    if (!name || !*name)
	return;

    /* Variable string is '<dbref number minus #>.<variable name>'. We
     * lowercase all names. Note that we're going to end up automatically
     * truncating long names.
     */

    tp = tbuf;
    safe_ltos(tbuf, &tp, obj);
    safe_sb_chr('.', tbuf, &tp);
    for (p = name; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(name, tbuf, &tp);
    *tp = '\0';

    /* Search for it. If it exists, replace it. If we get a blank string,
     * delete the variable.
     */

    if ((xvar = (VARENT *) hashfind(tbuf, &mudstate.vars_htab))) {
	if (xvar->text) {
	    XFREE(xvar->text, "xvar_data");
	}
	if (data && *data) {
	    xvar->text = (char *) XMALLOC(sizeof(char) * (strlen(data) + 1),
					  "xvar_data");
	    if (!xvar->text)
		return;		/* out of memory */
	    strcpy(xvar->text, data);
	} else {
	    xvar->text = NULL;
	    XFREE(xvar, "xvar_struct");
	    hashdelete(tbuf, &mudstate.vars_htab);
	    s_VarsCount(obj, VarsCount(obj) - 1);
	}
    } else {

	/* We haven't found it. If it's non-empty, set it, provided we're
	 * not running into a limit on the number of vars per object.
	 */

	if (VarsCount(obj) + 1 > mudconf.numvars_lim)
	    return;

	if (data && *data) {
	    xvar = (VARENT *) XMALLOC(sizeof(VARENT), "xvar_struct");
	    if (!xvar)
		return;		/* out of memory */
	    xvar->text = (char *) XMALLOC(sizeof(char) * (strlen(data) + 1),
					  "xvar_data");
	    if (!xvar->text)
		return;		/* out of memory */
	    strcpy(xvar->text, data);
	    hashadd(tbuf, (int *) xvar, &mudstate.vars_htab);
	    s_VarsCount(obj, VarsCount(obj) + 1);
	    Set_Max(mudstate.max_vars, mudstate.vars_htab.entries);
	}
    }
}


static void clear_xvars(obj, xvar_names, n_xvars)
    dbref obj;
    char **xvar_names;
    int n_xvars;
{
    /* Clear out an array of variable names. */

    char pre[SBUF_SIZE], tbuf[SBUF_SIZE], *tp, *p;
    VARENT *xvar;
    int i;

    /* Build our dbref bit first. */

    tp = pre;
    safe_ltos(pre, &tp, obj);
    safe_sb_chr('.', pre, &tp);
    *tp = '\0';

    /* Go clear stuff. */

    for (i = 0; i < n_xvars; i++) {

	for (p = xvar_names[i]; *p; p++)
	    *p = ToLower(*p);
	tp = tbuf;
	safe_sb_str(pre, tbuf, &tp);
	safe_sb_str(xvar_names[i], tbuf, &tp);
	*tp = '\0';

	if ((xvar = (VARENT *) hashfind(tbuf, &mudstate.vars_htab))) {
	    if (xvar->text) {
		XFREE(xvar->text, "xvar_data");
		xvar->text = NULL;
	    }
	    XFREE(xvar, "xvar_struct");
	    hashdelete(tbuf, &mudstate.vars_htab);
	}
    }

    s_VarsCount(obj, VarsCount(obj) - n_xvars);
}


void xvars_clr(player)
    dbref player;
{
    char tbuf[SBUF_SIZE], *tp;
    HASHTAB *htab;
    HASHENT *hptr, *last, *next;
    int i, len;
    VARENT *xvar;

    tp = tbuf;
    safe_ltos(tbuf, &tp, player);
    safe_sb_chr('.', tbuf, &tp);
    *tp = '\0';
    len = strlen(tbuf);

    htab = &mudstate.vars_htab;
    for (i = 0; i < htab->hashsize; i++) {
	last = NULL;
	for (hptr = htab->entry->element[i]; hptr != NULL; last = hptr) {
	    next = hptr->next;
	    if (!strncmp(tbuf, hptr->target, len)) {
		if (last == NULL)
		    htab->entry->element[i] = next;
		else
		    last->next = next;
		xvar = (VARENT *) hptr->data;
		XFREE(xvar->text, "xvar_data");
		XFREE(xvar, "xvar_struct");
		free(hptr->target);
		free(hptr);
		htab->deletes++;
		htab->entries--;
		if (htab->entry->element[i] == NULL)
		    htab->nulls++;
	    }
	    hptr = next;
	}
    }

    s_VarsCount(player, 0);
}


FUNCTION(fun_x)
{
    VARENT *xvar;
    char tbuf[SBUF_SIZE], *tp, *p;

    /* Variable string is '<dbref number minus #>.<variable name>' */

    tp = tbuf;
    safe_ltos(tbuf, &tp, player);
    safe_sb_chr('.', tbuf, &tp);
    for (p = fargs[0]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[0], tbuf, &tp);
    *tp = '\0';

    if ((xvar = (VARENT *) hashfind(tbuf, &mudstate.vars_htab)))
	safe_str(xvar->text, buff, bufc);
}


FUNCTION(fun_setx)
{
    set_xvar(player, fargs[0], fargs[1]);
}

FUNCTION(fun_setxr)
{
    set_xvar(player, fargs[0], fargs[1]);
    safe_str(fargs[1], buff, bufc);
}

FUNCTION(fun_xvars)
{
    char *xvar_names[LBUF_SIZE / 2], *elems[LBUF_SIZE / 2];
    int n_xvars, n_elems;
    char *varlist, *elemlist;
    int i;
    char sep;

    varargs_preamble("XVARS", 3);
   
    varlist = alloc_lbuf("fun_xvars.vars");
    strcpy(varlist, fargs[0]);
    n_xvars = list2arr(xvar_names, LBUF_SIZE / 2, varlist, ' ');

    if (n_xvars == 0) {
	free_lbuf(varlist);
	return;
    }

    if (!fargs[1] || !*fargs[1]) {
	/* Empty list, clear out the data. */
	clear_xvars(player, xvar_names, n_xvars);
	free_lbuf(varlist);
	return;
    }

    elemlist = alloc_lbuf("fun_xvars.elems");
    strcpy(elemlist, fargs[1]);
    n_elems = list2arr(elems, LBUF_SIZE / 2, elemlist, sep);

    if (n_elems != n_xvars) {
	safe_str("#-1 LIST MUST BE OF EQUAL SIZE", buff, bufc);
	free_lbuf(varlist);
	free_lbuf(elemlist);
	return;
    }

    for (i = 0; i < n_elems; i++) {
	set_xvar(player, xvar_names[i], elems[i]);
    }

    free_lbuf(varlist);
    free_lbuf(elemlist);
}


FUNCTION(fun_let)
{
    char *xvar_names[LBUF_SIZE / 2], *elems[LBUF_SIZE / 2];
    char *old_xvars[LBUF_SIZE / 2];
    int n_xvars, n_elems;
    char *varlist, *elemlist;
    char *str, *bp, *p;
    char pre[SBUF_SIZE], tbuf[SBUF_SIZE], *tp;
    VARENT *xvar;
    int i;
    char sep;

    varargs_preamble("LET", 4);

    if (!fargs[0] || !*fargs[0])
	return;
   
    varlist = bp = alloc_lbuf("fun_let.vars");
    str = fargs[0];
    exec(varlist, &bp, 0, player, cause, EV_FCHECK | EV_STRIP | EV_EVAL, &str,
	 cargs, ncargs);
    *bp = '\0';
    n_xvars = list2arr(xvar_names, LBUF_SIZE / 2, varlist, ' ');

    if (n_xvars == 0) {
	free_lbuf(varlist);
	return;
    }

    /* Lowercase our variable names. */

    for (i = 0, p = xvar_names[i]; *p; p++)
	*p = ToLower(*p);

    /* Save our original values. Copying this stuff into an array is
     * unnecessarily expensive because we allocate and free memory 
     * that we could theoretically just trade pointers around for --
     * but this way is cleaner.
     */

    tp = pre;
    safe_ltos(pre, &tp, player);
    safe_sb_chr('.', pre, &tp);
    *tp = '\0';

    for (i = 0; i < n_xvars; i++) {

	tp = tbuf;
	safe_sb_str(pre, tbuf, &tp);
	safe_sb_str(xvar_names[i], tbuf, &tp);
	*tp = '\0';

	if ((xvar = (VARENT *) hashfind(tbuf, &mudstate.vars_htab))) {
	    if (xvar->text)
		old_xvars[i] = (char *) strdup(xvar->text);
	    else
		old_xvars[i] = NULL;
	} else {
	    old_xvars[i] = NULL;
	}
    }

    if (fargs[1] && *fargs[1]) {

	/* We have data, so we should initialize variables to their values,
	 * ala xvars(). However, unlike xvars(), if we don't get a list,
	 * we just leave the values alone (we don't clear them out).
	 */

	elemlist = bp = alloc_lbuf("fun_let.elems");
	str = fargs[1];
	exec(elemlist, &bp, 0, player, cause, EV_FCHECK | EV_STRIP | EV_EVAL,
	     &str, cargs, ncargs);
	*bp = '\0';
	n_elems = list2arr(elems, LBUF_SIZE / 2, elemlist, sep);

	if (n_elems != n_xvars) {
	    safe_str("#-1 LIST MUST BE OF EQUAL SIZE", buff, bufc);
	    free_lbuf(varlist);
	    free_lbuf(elemlist);
	    return;
	}

	for (i = 0; i < n_elems; i++) {
	    set_xvar(player, xvar_names[i], elems[i]);
	}

	free_lbuf(elemlist);

    }

    /* Now we go to execute our function body. */

    str = fargs[2];
    exec(buff, bufc, 0, player, cause, EV_FCHECK | EV_STRIP | EV_EVAL, &str,
	 cargs, ncargs);

    /* Restore the old values. */

    for (i = 0; i < n_xvars; i++) {
	set_xvar(player, xvar_names[i], old_xvars[i]);
	if (old_xvars[i])
	    free(old_xvars[i]);
    }

    free_lbuf(varlist);
}


FUNCTION(fun_lvars)
{
    print_htab_matches(player, &mudstate.vars_htab, buff, bufc);
}


FUNCTION(fun_clearvars)
{
    /* This is computationally expensive. Necessary, but its use should
     * be avoided if possible.
     */

    xvars_clr(player);
}

/* --------------------------------------------------------------------------
 * Auxiliary stuff for structures.
 */

typedef struct component_def COMPONENT;
struct component_def {
    int (*typer_func)();	/* type-checking handler */
    char *def_val;		/* default value */
};

typedef struct structure_def STRUCTDEF;
struct structure_def {
    char *s_name;		/* name of the structure */
    char **c_names;		/* array of component names */
    COMPONENT **c_array;	/* array of pointers to components */
    int c_count;		/* number of components */
    char delim;			/* output delimiter when unloading */
    int need_typecheck;		/* any components without types of any? */
    int n_instances;		/* number of instances out there */
    char *names_base;		/* pointer for later freeing */
    char *defs_base;		/* pointer for later freeing */
};

typedef struct instance_def INSTANCE;
struct instance_def {
    STRUCTDEF *datatype;	/* pointer to structure data type def */
};

typedef struct data_def STRUCTDATA;
struct data_def {
    char *text;
};

/* ---------------------------------------------------------------------------
 * Structures.
 */

static int istype_char(str)
    char *str;
{
    if (strlen(str) == 1)
	return 1;
    else
	return 0;
}

static int istype_dbref(str)
    char *str;
{
    dbref it;

    if (*str++ != NUMBER_TOKEN)
	return 0;
    if (*str) {
	it = parse_dbref(str);
	return (Good_obj(it));
    }
    return 0;
}

static int istype_int(str)
    char *str;
{
    return (is_integer(str));
}

static int istype_float(str)
    char *str;
{
    return (is_number(str));
}

static int istype_string(str)
    char *str;
{
    char *p;

    for (p = str; *p; p++) {
	if (isspace(*p))
	    return 0;
    }
    return 1;
}


FUNCTION(fun_structure)
{
    char sep;			/* delim for default values */
    char osep;			/* output delim for structure values */
    char tbuf[SBUF_SIZE], *tp;
    char cbuf[SBUF_SIZE], *cp;
    char *p;
    char *comp_names, *type_names, *default_vals;
    char *comp_array[LBUF_SIZE / 2];
    char *type_array[LBUF_SIZE / 2];
    char *def_array[LBUF_SIZE / 2];
    int n_comps, n_types, n_defs;
    int i;
    STRUCTDEF *this_struct;
    COMPONENT *this_comp;
    int check_type = 0;

    svarargs_preamble("STRUCTURE", 6);

    /* Prevent null delimiters and line delimiters. */

    if (!osep) {
	notify_quiet(player, "You cannot use a null output delimiter.");
	safe_chr('0', buff, bufc);
	return;
    }
    if (osep == '\r') {
	notify_quiet(player, "You cannot use that output delimiter.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Enforce limits. */

    if (StructCount(player) > mudconf.struct_lim) {
	notify_quiet(player, "Too many structures.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* If our structure name is too long, reject it. */

    if (strlen(fargs[0]) > (SBUF_SIZE / 2) - 9) {
	notify_quiet(player, "Structure name is too long.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* No periods in structure names */

    if (((char *) index(fargs[0], '.')) != NULL) {
	notify_quiet(player, "Structure names cannot contain periods.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* The hashtable is indexed by <dbref number>.<structure name> */

    tp = tbuf;
    safe_ltos(tbuf, &tp, player);
    safe_sb_chr('.', tbuf, &tp);
    for (p = fargs[0]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[0], tbuf, &tp);
    *tp = '\0';

    /* If we have this structure already, reject. */

    if (hashfind(tbuf, &mudstate.structs_htab)) {
	notify_quiet(player, "Structure is already defined.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Split things up. Make sure lists are the same size.
     * If everything eventually goes well, comp_names and default_vals will
     * REMAIN allocated. 
     */


    comp_names = (char *) strdup(fargs[1]);
    n_comps = list2arr(comp_array, LBUF_SIZE / 2, comp_names, ' ');

    if (n_comps < 1) {
	notify_quiet(player, "There must be at least one component.");
	safe_chr('0', buff, bufc);
	free(comp_names);
	return;
    }

    /* Make sure that we have a sane name for the components. They must
     * be smaller than half an SBUF.
     */

    for (i = 0; i < n_comps; i++) {
	if (strlen(comp_array[i]) > (SBUF_SIZE / 2) - 9) {
	    notify_quiet(player, "Component name is too long.");
	    safe_chr('0', buff, bufc);
	    free(comp_names);
	    return;
	}
    }

    type_names = alloc_lbuf("struct.types");
    strcpy(type_names, fargs[2]);
    n_types = list2arr(type_array, LBUF_SIZE / 2, type_names, ' ');

    /* Make sure all types are valid. We look only at the first char, so
     * typos will not be caught.
     */

    for (i = 0; i < n_types; i++) {
	switch (*(type_array[i])) {
	    case 'a': case 'A':
	    case 'c': case 'C':
	    case 'd': case 'D':
	    case 'i': case 'I':
	    case 'f': case 'F':
	    case 's': case 'S':
		/* Valid types */
		break;
	    default:
		notify_quiet(player, "Invalid data type specified.");
		safe_chr('0', buff, bufc);
		free(comp_names);
		free_lbuf(type_names);
		return;
	}
    }

    if (fargs[3] && *fargs[3]) {
	default_vals = (char *) strdup(fargs[3]);
	n_defs = list2arr(def_array, LBUF_SIZE / 2, default_vals, sep);
    } else {
	default_vals = NULL;
	n_defs = 0;
    }

    if ((n_comps != n_types) || (n_defs && (n_comps != n_defs))) {
	notify_quiet(player, "List sizes must be identical.");
	safe_chr('0', buff, bufc);
	free(comp_names);
	free_lbuf(type_names);
	if (default_vals)
	    free(default_vals);
	return;
    }

    /* Allocate the structure and stuff it in the hashtable. Note that
     * we must duplicate our name structure since the pointers to the
     * strings have been allocated on the stack!
     */

    this_struct = (STRUCTDEF *) XMALLOC(sizeof(STRUCTDEF), "struct_alloc");
    this_struct->s_name = (char *) strdup(fargs[0]);
    this_struct->c_names = (char **) calloc(n_comps, sizeof(char *));
    for (i = 0; i < n_comps; i++)
	this_struct->c_names[i] = comp_array[i];
    this_struct->c_array = (COMPONENT **) calloc(n_comps, sizeof(COMPONENT *));
    this_struct->c_count = n_comps;
    this_struct->delim = osep;
    this_struct->n_instances = 0;
    this_struct->names_base = comp_names;
    this_struct->defs_base = default_vals;
    hashadd(tbuf, (int *) this_struct, &mudstate.structs_htab);
    Set_Max(mudstate.max_structs, mudstate.structs_htab.entries);

    /* Now that we're done with the base name, we can stick the 
     * joining period on the end.
     */
    
    safe_sb_chr('.', tbuf, &tp);
    *tp = '\0';

    /* Allocate each individual component. */

    for (i = 0; i < n_comps; i++) {

	cp = cbuf;
	safe_sb_str(tbuf, cbuf, &cp);
	for (p = comp_array[i]; *p; p++)
	    *p = ToLower(*p);
	safe_sb_str(comp_array[i], cbuf, &cp);
	*cp = '\0';

	this_comp = (COMPONENT *) XMALLOC(sizeof(COMPONENT), "comp_alloc");
	this_comp->def_val = def_array[i];
	switch (*(type_array[i])) {
	    case 'a': case 'A':
		this_comp->typer_func = NULL;
		break;
	    case 'c': case 'C':
		this_comp->typer_func = istype_char;
		check_type = 1;
		break;
	    case 'd': case 'D':
		this_comp->typer_func = istype_dbref;
		check_type = 1;
		break;
	    case 'i': case 'I':
		this_comp->typer_func = istype_int;
		check_type = 1;
		break;
	    case 'f': case 'F':
		this_comp->typer_func = istype_float;
		check_type = 1;
		break;
	    case 's': case 'S':
		this_comp->typer_func = istype_string;
		check_type = 1;
		break;
	    default:
		/* Should never happen */
		this_comp->typer_func = NULL;
	}
	this_struct->need_typecheck = check_type;
	this_struct->c_array[i] = this_comp;
	hashadd(cbuf, (int *) this_comp, &mudstate.cdefs_htab);
	Set_Max(mudstate.max_cdefs, mudstate.cdefs_htab.entries);
    }

    free_lbuf(type_names);
    s_StructCount(player, StructCount(player) + 1);
    safe_chr('1', buff, bufc);
}


FUNCTION(fun_construct)
{
    char sep;
    char tbuf[SBUF_SIZE], *tp;
    char ibuf[SBUF_SIZE], *ip;
    char cbuf[SBUF_SIZE], *cp;
    char *p;
    STRUCTDEF *this_struct;
    char *comp_names, *init_vals;
    char *comp_array[LBUF_SIZE / 2], *vals_array[LBUF_SIZE / 2];
    int n_comps, n_vals;
    int i;
    COMPONENT *c_ptr;
    INSTANCE *inst_ptr;
    STRUCTDATA *d_ptr;
    int retval;

    /* This one is complicated: We need two, four, or five args. */

    mvarargs_preamble("CONSTRUCT", 2, 5);
    if (nfargs == 3) {
	safe_str("#-1 FUNCTION (CONSTRUCT) EXPECTS 2 OR 4 OR 5 ARGUMENTS",
		 buff, bufc);
	return;
    }

    /* Enforce limits. */

    if (InstanceCount(player) > mudconf.instance_lim) {
	notify_quiet(player, "Too many instances.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* If our instance name is too long, reject it. */

    if (strlen(fargs[0]) > (SBUF_SIZE / 2) - 9) {
	notify_quiet(player, "Instance name is too long.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Make sure this instance doesn't exist. */

    ip = ibuf;
    safe_ltos(ibuf, &ip, player);
    safe_sb_chr('.', ibuf, &ip);
    for (p = fargs[0]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[0], ibuf, &ip);
    *ip = '\0';

    if (hashfind(ibuf, &mudstate.instance_htab)) {
	notify_quiet(player, "That instance has already been defined.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Look up the structure. */

    tp = tbuf;
    safe_ltos(tbuf, &tp, player);
    safe_sb_chr('.', tbuf, &tp);
    for (p = fargs[1]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[1], tbuf, &tp);
    *tp = '\0';

    this_struct = (STRUCTDEF *) hashfind(tbuf, &mudstate.structs_htab);
    if (!this_struct) {
	notify_quiet(player, "No such structure.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Check to make sure that all the component names are valid, if we
     * have been given defaults. Also, make sure that the defaults are
     * of the appropriate type.
     */

    safe_sb_chr('.', tbuf, &tp);
    *tp = '\0';

    if (fargs[2] && *fargs[2] && fargs[3] && *fargs[3]) {

	comp_names = alloc_lbuf("construct.comps");
	strcpy(comp_names, fargs[2]);
	n_comps = list2arr(comp_array, LBUF_SIZE / 2, comp_names, ' ');
	init_vals = alloc_lbuf("construct.vals");
	strcpy(init_vals, fargs[3]);
	n_vals = list2arr(vals_array, LBUF_SIZE / 2, init_vals, sep);
	if (n_comps != n_vals) {
	    notify_quiet(player, "List sizes must be identical.");
	    safe_chr('0', buff, bufc);
	    free_lbuf(comp_names);
	    free_lbuf(init_vals);
	    return;
	}

	for (i = 0; i < n_comps; i++) {
	    cp = cbuf;
	    safe_sb_str(tbuf, cbuf, &cp);
	    for (p = comp_array[i]; *p; p++)
		*p = ToLower(*p);
	    safe_sb_str(comp_array[i], cbuf, &cp);
	    c_ptr = (COMPONENT *) hashfind(cbuf, &mudstate.cdefs_htab);
	    if (!c_ptr) {
		notify_quiet(player, "Invalid component name.");
		safe_chr('0', buff, bufc);
		free_lbuf(comp_names);
		free_lbuf(init_vals);
		return;
	    }
	    if (c_ptr->typer_func) {
		retval = (*(c_ptr->typer_func)) (vals_array[i]);
		if (!retval) {
		    notify_quiet(player, "Default value is of invalid type.");
		    safe_chr('0', buff, bufc);
		    free_lbuf(comp_names);
		    free_lbuf(init_vals);
		    return;
		}
	    }
	}

    } else if ((!fargs[2] || !*fargs[2]) && (!fargs[3] || !*fargs[3])) {
	/* Blank initializers. This is just fine. */
	comp_names = init_vals = NULL;
	n_comps = n_vals = 0;
    } else {
	notify_quiet(player, "List sizes must be identical.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Go go gadget constructor.
     * Allocate the instance. We should have already made sure that the
     * instance doesn't exist.
     */

    inst_ptr = (INSTANCE *) XMALLOC(sizeof(INSTANCE), "constructor.inst");
    inst_ptr->datatype = this_struct;
    hashadd(ibuf, (int *) inst_ptr, &mudstate.instance_htab);
    Set_Max(mudstate.max_instance, mudstate.instance_htab.entries);

    /* Populate with default values. */

    for (i = 0; i < this_struct->c_count; i++) {
	d_ptr = (STRUCTDATA *) XMALLOC(sizeof(STRUCTDATA), "constructor.data");
	if (this_struct->c_array[i]->def_val) {
	    d_ptr->text = (char *)
		strdup(this_struct->c_array[i]->def_val);
	} else {
	    d_ptr->text = NULL;
	}
	tp = tbuf;
	safe_sb_str(ibuf, tbuf, &tp);
	safe_sb_chr('.', tbuf, &tp);
	safe_sb_str(this_struct->c_names[i], tbuf, &tp);
	*tp = '\0';
	hashadd(tbuf, (int *) d_ptr, &mudstate.instdata_htab);
	Set_Max(mudstate.max_instdata, mudstate.instdata_htab.entries);
    }

    /* Overwrite with component values. */

    for (i = 0; i < n_comps; i++) {
	tp = tbuf;
	safe_sb_str(ibuf, tbuf, &tp);
	safe_sb_chr('.', tbuf, &tp);
	safe_sb_str(comp_array[i], tbuf, &tp);
	*tp = '\0';
	d_ptr = (STRUCTDATA *) hashfind(tbuf, &mudstate.instdata_htab);
	if (d_ptr) {
	    if (d_ptr->text)
		free(d_ptr->text);
	    if (vals_array[i] && *(vals_array[i]))
		d_ptr->text = (char *) strdup(vals_array[i]);
	    else
		d_ptr->text = NULL;
	}
    }

    if (comp_names)
	free_lbuf(comp_names);
    if (init_vals)
	free_lbuf(init_vals);
    this_struct->n_instances += 1;
    s_InstanceCount(player, InstanceCount(player) + 1);
    safe_chr('1', buff, bufc);
}


FUNCTION(fun_load)
{
    char tbuf[SBUF_SIZE], *tp;
    char ibuf[SBUF_SIZE], *ip;
    char *p;
    STRUCTDEF *this_struct;
    char *val_list;
    char *val_array[LBUF_SIZE / 2];
    int n_vals;
    INSTANCE *inst_ptr;
    STRUCTDATA *d_ptr;
    int i;
    char sep;

    varargs_preamble("LOAD", 4);

    /* Enforce limits. */

    if (InstanceCount(player) > mudconf.instance_lim) {
	notify_quiet(player, "Too many instances.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* If our instance name is too long, reject it. */

    if (strlen(fargs[0]) > (SBUF_SIZE / 2) - 9) {
	notify_quiet(player, "Instance name is too long.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Make sure this instance doesn't exist. */

    ip = ibuf;
    safe_ltos(ibuf, &ip, player);
    safe_sb_chr('.', ibuf, &ip);
    for (p = fargs[0]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[0], ibuf, &ip);
    *ip = '\0';

    if (hashfind(ibuf, &mudstate.instance_htab)) {
	notify_quiet(player, "That instance has already been defined.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Look up the structure. */

    tp = tbuf;
    safe_ltos(tbuf, &tp, player);
    safe_sb_chr('.', tbuf, &tp);
    for (p = fargs[1]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[1], tbuf, &tp);
    *tp = '\0';

    this_struct = (STRUCTDEF *) hashfind(tbuf, &mudstate.structs_htab);
    if (!this_struct) {
	notify_quiet(player, "No such structure.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Chop up the raw stuff according to the delimiter. */

    if (nfargs != 4)
	sep = this_struct->delim;

    val_list = alloc_lbuf("load.val_list");
    strcpy(val_list, fargs[2]);
    n_vals = list2arr(val_array, LBUF_SIZE / 2, val_list, sep);
    if (n_vals != this_struct->c_count) {
	notify_quiet(player, "Incorrect number of components.");
	safe_chr('0', buff, bufc);
	free_lbuf(val_list);
	return;
    }

    /* Check the types of the data we've been passed. */

    for (i = 0; i < n_vals; i++) {
	if (this_struct->c_array[i]->typer_func &&
	    !((*(this_struct->c_array[i]->typer_func)) (val_array[i]))) {
	    notify_quiet(player, "Value is of invalid type.");
	    safe_chr('0', buff, bufc);
	    free_lbuf(val_list);
	    return;
	}
    }

    /* Allocate the instance. We should have already made sure that the
     * instance doesn't exist.
     */

    inst_ptr = (INSTANCE *) XMALLOC(sizeof(INSTANCE), "constructor.inst");
    inst_ptr->datatype = this_struct;
    hashadd(ibuf, (int *) inst_ptr, &mudstate.instance_htab);
    Set_Max(mudstate.max_instance, mudstate.instance_htab.entries);

    /* Stuff data into memory. */

    for (i = 0; i < this_struct->c_count; i++) {
	d_ptr = (STRUCTDATA *) XMALLOC(sizeof(STRUCTDATA), "constructor.data");
	if (val_array[i] && *(val_array[i]))
	    d_ptr->text = (char *) strdup(val_array[i]);
	else
	    d_ptr->text = NULL;
	tp = tbuf;
	safe_sb_str(ibuf, tbuf, &tp);
	safe_sb_chr('.', tbuf, &tp);
	safe_sb_str(this_struct->c_names[i], tbuf, &tp);
	*tp = '\0';
	hashadd(tbuf, (int *) d_ptr, &mudstate.instdata_htab);
	Set_Max(mudstate.max_instdata, mudstate.instdata_htab.entries);
    }

    free_lbuf(val_list);
    this_struct->n_instances += 1;
    s_InstanceCount(player, InstanceCount(player) + 1);
    safe_chr('1', buff, bufc);
}


FUNCTION(fun_z)
{
    char tbuf[SBUF_SIZE], *tp;
    char *p;
    STRUCTDATA *s_ptr;

    tp = tbuf;
    safe_ltos(tbuf, &tp, player);
    safe_sb_chr('.', tbuf, &tp);
    for (p = fargs[0]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[0], tbuf, &tp);
    safe_sb_chr('.', tbuf, &tp);
    for (p = fargs[1]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[1], tbuf, &tp);
    *tp = '\0';

    s_ptr = (STRUCTDATA *) hashfind(tbuf, &mudstate.instdata_htab);
    if (!s_ptr || !s_ptr->text)
	return;
    safe_str(s_ptr->text, buff, bufc);
}


FUNCTION(fun_modify)
{
    char tbuf[SBUF_SIZE], *tp;
    char cbuf[SBUF_SIZE], *cp;
    char *p;
    INSTANCE *inst_ptr;
    COMPONENT *c_ptr;
    STRUCTDATA *s_ptr;
    int retval;

    /* Find the instance first, since this is how we get our typechecker. */

    tp = tbuf;
    safe_ltos(tbuf, &tp, player);
    safe_sb_chr('.', tbuf, &tp);
    for (p = fargs[0]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[0], tbuf, &tp);
    *tp = '\0';

    inst_ptr = (INSTANCE *) hashfind(tbuf, &mudstate.instance_htab);
    if (!inst_ptr) {
	notify_quiet(player, "No such instance.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Use that to find the component and check the type. */

    if (inst_ptr->datatype->need_typecheck) {

	cp = cbuf;
	safe_ltos(cbuf, &cp, player);
	safe_sb_chr('.', cbuf, &cp);
	safe_sb_str(inst_ptr->datatype->s_name, cbuf, &cp);
	safe_sb_chr('.', cbuf, &cp);
	for (p = fargs[1]; *p; p++)
	    *p = ToLower(*p);
	safe_sb_str(fargs[1], cbuf, &cp);
	*cp = '\0';

	c_ptr = (COMPONENT *) hashfind(cbuf, &mudstate.cdefs_htab);
	if (!c_ptr) {
	    notify_quiet(player, "No such component.");
	    safe_chr('0', buff, bufc);
	    return;
	}
	if (c_ptr->typer_func) {
	    retval = (*(c_ptr->typer_func)) (fargs[2]);
	    if (!retval) {
		notify_quiet(player, "Value is of invalid type.");
		safe_chr('0', buff, bufc);
		return;
	    }
	}
    }

    /* Now go set it. */

    safe_sb_chr('.', tbuf, &tp);
    safe_sb_str(fargs[1], tbuf, &tp);
    *tp = '\0';

    s_ptr = (STRUCTDATA *) hashfind(tbuf, &mudstate.instdata_htab);
    if (!s_ptr) {
	notify_quiet(player, "No such data.");
	safe_chr('0', buff, bufc);
	return;
    }
    if (s_ptr->text)
	free(s_ptr->text);
    if (fargs[2] && *fargs[2]) {
	s_ptr->text = (char *) strdup(fargs[2]);
    } else {
	s_ptr->text = NULL;
    }

    safe_chr('1', buff, bufc);
}


FUNCTION(fun_unload)
{
    char tbuf[SBUF_SIZE], *tp;
    char ibuf[SBUF_SIZE], *ip;
    INSTANCE *inst_ptr;
    char *p;
    STRUCTDEF *this_struct;
    STRUCTDATA *d_ptr;
    int i;
    char sep;

    varargs_preamble("UNLOAD", 2);

    /* Get the instance. */

    ip = ibuf;
    safe_ltos(ibuf, &ip, player);
    safe_sb_chr('.', ibuf, &ip);
    for (p = fargs[0]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[0], ibuf, &ip);
    *ip = '\0';

    inst_ptr = (INSTANCE *) hashfind(ibuf, &mudstate.instance_htab);
    if (!inst_ptr)
	return;

    /* From the instance, we can get a pointer to the structure. We then
     * have the information we need to figure out what components are
     * associated with this, and print them appropriately.
     */

    safe_sb_chr('.', ibuf, &ip);
    *ip = '\0';

    this_struct = inst_ptr->datatype;

    /* Our delimiter is a special case. */
    if (nfargs != 2)
	sep = this_struct->delim;

    for (i = 0; i < this_struct->c_count; i++) {
	if (i != 0) {
	    safe_chr(sep, buff, bufc);
	}
	tp = tbuf;
	safe_sb_str(ibuf, tbuf, &tp);
	safe_sb_str(this_struct->c_names[i], tbuf, &tp);
	*tp = '\0';
	d_ptr = (STRUCTDATA *) hashfind(tbuf, &mudstate.instdata_htab);
	if (d_ptr && d_ptr->text)
	    safe_str(d_ptr->text, buff, bufc);
    }
}


FUNCTION(fun_destruct)
{
    char tbuf[SBUF_SIZE], *tp;
    char ibuf[SBUF_SIZE], *ip;
    INSTANCE *inst_ptr;
    char *p;
    STRUCTDEF *this_struct;
    STRUCTDATA *d_ptr;
    int i;

    /* Get the instance. */

    ip = ibuf;
    safe_ltos(ibuf, &ip, player);
    safe_sb_chr('.', ibuf, &ip);
    for (p = fargs[0]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[0], ibuf, &ip);
    *ip = '\0';

    inst_ptr = (INSTANCE *) hashfind(ibuf, &mudstate.instance_htab);
    if (!inst_ptr) {
	notify_quiet(player, "No such instance.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Now we can get a pointer to the structure and find the rest of the
     * components.
     */

    this_struct = inst_ptr->datatype;

    XFREE(inst_ptr, "constructor.inst");
    hashdelete(ibuf, &mudstate.instance_htab);

    safe_sb_chr('.', ibuf, &ip);
    *ip = '\0';

    for (i = 0; i < this_struct->c_count; i++) {
	tp = tbuf;
	safe_sb_str(ibuf, tbuf, &tp);
	safe_sb_str(this_struct->c_names[i], tbuf, &tp);
	*tp = '\0';
	d_ptr = (STRUCTDATA *) hashfind(tbuf, &mudstate.instdata_htab);
	if (d_ptr) {
	    if (d_ptr->text)
		free(d_ptr->text);
	    XFREE(d_ptr, "constructor.data");
	    hashdelete(tbuf, &mudstate.instdata_htab);
	}
    }

    this_struct->n_instances -= 1;
    s_InstanceCount(player, InstanceCount(player) - 1);
    safe_chr('1', buff, bufc);
}


FUNCTION(fun_unstructure)
{
    char tbuf[SBUF_SIZE], *tp;
    char cbuf[SBUF_SIZE], *cp;
    char *p;
    STRUCTDEF *this_struct;
    int i;

    /* Find the structure */

    tp = tbuf;
    safe_ltos(tbuf, &tp, player);
    safe_sb_chr('.', tbuf, &tp);
    for (p = fargs[0]; *p; p++)
	*p = ToLower(*p);
    safe_sb_str(fargs[0], tbuf, &tp);
    *tp = '\0';

    this_struct = (STRUCTDEF *) hashfind(tbuf, &mudstate.structs_htab);
    if (!this_struct) {
	notify_quiet(player, "No such structure.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Can't delete what's in use. */

    if (this_struct->n_instances > 0) {
	notify_quiet(player, "This structure is in use.");
	safe_chr('0', buff, bufc);
	return;
    }

    /* Wipe the structure from the hashtable. */

    hashdelete(tbuf, &mudstate.structs_htab);

    /* Wipe out every component definition. */

    safe_sb_chr('.', tbuf, &tp);
    *tp = '\0';


    for (i = 0; i < this_struct->c_count; i++) {
	cp = cbuf;
	safe_sb_str(tbuf, cbuf, &cp);
	safe_sb_str(this_struct->c_names[i], cbuf, &cp);
	*cp = '\0';
	if (this_struct->c_array[i]) {
	    XFREE(this_struct->c_array[i], "comp_alloc");
	}
	hashdelete(cbuf, &mudstate.cdefs_htab);
    }


    /* Free up our bit of memory. */

    free(this_struct->s_name);
    if (this_struct->names_base)
	free(this_struct->names_base);
    if (this_struct->defs_base)
	free(this_struct->defs_base);
    free(this_struct->c_names);
    XFREE(this_struct, "struct_alloc");

    s_StructCount(player, StructCount(player) - 1);
    safe_chr('1', buff, bufc);
}

FUNCTION(fun_lstructures)
{
    print_htab_matches(player, &mudstate.structs_htab, buff, bufc);
}

FUNCTION(fun_linstances)
{
    print_htab_matches(player, &mudstate.instance_htab, buff, bufc);
}

void structure_clr(thing)
    dbref thing;
{
    /* Wipe out all structure information associated with an object.
     * Find all the object's instances. Destroy them.
     * Then, find all the object's defined structures, and destroy those.
     */

    HASHTAB *htab;
    HASHENT *hptr;
    char tbuf[SBUF_SIZE], ibuf[SBUF_SIZE], cbuf[SBUF_SIZE], *tp, *ip, *cp;
    int i, j, len, count;
    INSTANCE **inst_array;
    char **name_array;
    STRUCTDEF *this_struct;
    STRUCTDATA *d_ptr;
    STRUCTDEF **struct_array;

    /* The instance table is indexed as <dbref number>.<instance name> */

    tp = tbuf;
    safe_ltos(tbuf, &tp, thing);
    safe_sb_chr('.', tbuf, &tp);
    *tp = '\0';
    len = strlen(tbuf);

    /* Because of the hashtable rechaining that's done, we cannot simply
     * walk the hashtable and delete entries as we go. Instead, we've
     * got to keep track of all of our pointers, and go back and do
     * them one by one.
     */

    inst_array = (INSTANCE **) calloc(mudconf.instance_lim + 1,
				      sizeof(INSTANCE *));
    name_array = (char **) calloc(mudconf.instance_lim + 1, sizeof(char *));
    
    htab = &mudstate.instance_htab;
    count = 0;
    for (i = 0; i < htab->hashsize; i++) {
	for (hptr = htab->entry->element[i]; hptr != NULL; hptr = hptr->next) {
	    if (!strncmp(tbuf, hptr->target, len)) {
		name_array[count] = (char *) hptr->target;
		inst_array[count] = (INSTANCE *) hptr->data;
		count++;
	    }
	}
    }

    /* Now that we have the pointers to the instances, we can get the
     * structure definitions, and use that to hunt down and wipe the
     * components. 
     */

    if (count > 0) {
	for (i = 0; i < count; i++) {
	    this_struct = inst_array[i]->datatype;
	    XFREE(inst_array[i], "constructor.inst");
	    hashdelete(name_array[i], &mudstate.instance_htab);
	    ip = ibuf;
	    safe_sb_str(name_array[i], ibuf, &ip);
	    safe_sb_chr('.', ibuf, &ip);
	    *ip = '\0';
	    for (j = 0; j < this_struct->c_count; j++) {
		cp = cbuf;
		safe_sb_str(ibuf, cbuf, &cp);
		safe_sb_str(this_struct->c_names[j], cbuf, &cp);
		*cp = '\0';
		d_ptr = (STRUCTDATA *) hashfind(cbuf, &mudstate.instdata_htab);
		if (d_ptr) {
		    if (d_ptr->text)
			free(d_ptr->text);
		    XFREE(d_ptr, "constructor.data");
		    hashdelete(cbuf, &mudstate.instdata_htab);
		}
	    }
	    this_struct->n_instances -= 1;
	}
    }

    free(inst_array);
    free(name_array);

    /* The structure table is indexed as <dbref number>.<struct name> */

    tp = tbuf;
    safe_ltos(tbuf, &tp, thing);
    safe_sb_chr('.', tbuf, &tp);
    *tp = '\0';
    len = strlen(tbuf);

    /* Again, we have the hashtable rechaining problem. */

    struct_array = (STRUCTDEF **) calloc(mudconf.struct_lim + 1,
					 sizeof(STRUCTDEF *));
    name_array = (char **) calloc(mudconf.struct_lim + 1, sizeof(char *));

    htab = &mudstate.structs_htab;
    count = 0;
    for (i = 0; i < htab->hashsize; i++) {
	for (hptr = htab->entry->element[i]; hptr != NULL; hptr = hptr->next) {
	    if (!strncmp(tbuf, hptr->target, len)) {
		name_array[count] = (char *) hptr->target;
		struct_array[count] = (STRUCTDEF *) hptr->data;
		count++;
	    }
	}
    }

    /* We have the pointers to the structures. Flag a big error if they're
     * still in use, wipe them from the hashtable, then wipe out every
     * component definition. Free up the memory.
     */

    if (count > 0) {
	for (i = 0; i < count; i++) {
	    if (struct_array[i]->n_instances > 0) {
		STARTLOG(LOG_ALWAYS, "BUG", "STRUCT")
		    log_name(thing);
		    log_text((char *) " structure ");
		    log_text((char *) name_array[i]);
		    log_text((char *) " has ");
		    log_number(struct_array[i]->n_instances);
		    log_text((char *) " allocated instances uncleared.");
		ENDLOG
	    }
	    hashdelete(name_array[i], &mudstate.structs_htab);
	    ip = ibuf;
	    safe_sb_str(name_array[i], ibuf, &ip);
	    safe_sb_chr('.', ibuf, &ip);
	    *ip = '\0';
	    for (j = 0; j < struct_array[i]->c_count; j++) {
		cp = cbuf;
		safe_sb_str(ibuf, cbuf, &cp);
		safe_sb_str(struct_array[i]->c_names[j], cbuf, &cp);
		*cp = '\0';
		if (struct_array[i]->c_array[j]) {
		    XFREE(struct_array[i]->c_array[j], "comp_alloc");
		}
		hashdelete(cbuf, &mudstate.cdefs_htab);
	    }
	    free(struct_array[i]->s_name);
	    if (struct_array[i]->names_base)
		free(struct_array[i]->names_base);
	    if (struct_array[i]->defs_base)
		free(struct_array[i]->defs_base);
	    free(struct_array[i]->c_names);
	    XFREE(struct_array[i], "struct_alloc");
	}
    }

    free(struct_array);
    free(name_array);
}

/* --------------------------------------------------------------------------
 * Auxiliary functions for stacks.
 */

typedef struct object_stack STACK;
  struct object_stack {
      char *data;
      STACK *next;
  };

#define stack_get(x)   ((STACK *) nhashfind(x, &mudstate.objstack_htab))

#define stack_object(p,x)				\
        x = match_thing(p, fargs[0]);			\
	if (!Good_obj(x)) {				\
	    return;					\
	}						\
	if (!Controls(p, x)) {				\
            notify_quiet(p, NOPERM_MESSAGE);		\
	    return;					\
	}

/* ---------------------------------------------------------------------------
 * Object stack functions.
 */

void stack_clr(thing)
    dbref thing;
{
    STACK *sp, *tp, *xp;

    sp = stack_get(thing);
    if (sp) {
	for (tp = sp; tp != NULL; ) {
	    XFREE(tp->data, "stack_clr_data");
	    xp = tp;
	    tp = tp->next;
	    XFREE(xp, "stack_clr");
	}
	nhashdelete(thing, &mudstate.objstack_htab);
	s_StackCount(thing, 0);
    }
}

static void stack_set(thing, sp)
    dbref thing;
    STACK *sp;
{
    STACK *xsp;
    int stat;

    if (!sp) {
	nhashdelete(thing, &mudstate.objstack_htab);
	s_StackCount(thing, StackCount(thing) - 1);
	return;
    }

    if (StackCount(thing) + 1 > mudconf.stack_lim) {
	XFREE(sp->data, "stack_max_data");
	XFREE(sp, "stack_max");
	return;
    }

    xsp = stack_get(thing);
    if (xsp) {
	stat = nhashrepl(thing, (int *) sp, &mudstate.objstack_htab);
    } else {
	stat = nhashadd(thing, (int *) sp, &mudstate.objstack_htab);
	s_StackCount(thing, StackCount(thing) + 1);
	Set_Max(mudstate.max_stacks, mudstate.objstack_htab.entries);
    }
    if (stat < 0) {		/* failure for some reason */
	STARTLOG(LOG_BUGS, "STK", "SET")
	    log_name(thing);
	ENDLOG
	stack_clr(thing);
    }
}

FUNCTION(fun_empty)
{
    dbref it;

    xvarargs_preamble("EMPTY", 0, 1);

    if (!fargs[0]) {
	it = player;
    } else {
	stack_object(player, it);
    }

    stack_clr(it);
}

FUNCTION(fun_items)
{
    dbref it;
    int i;
    STACK *sp;

    if (!fargs[0]) {
	it = player;
    } else {
	stack_object(player, it);
    }

    for (i = 0, sp = stack_get(it); sp != NULL; sp = sp->next, i++)
	;
    safe_ltos(buff, bufc, i);
}
    

FUNCTION(fun_push)
{
    dbref it;
    char *data;
    STACK *sp;

    xvarargs_preamble("PUSH", 1, 2);

    if (!fargs[1]) {
	it = player;
	data = fargs[0];
    } else {
	stack_object(player, it);
	data = fargs[1];
    }

    sp = (STACK *) XMALLOC(sizeof(STACK), "stack_push");
    if (!sp)			/* out of memory, ouch */
	return;
    sp->next = stack_get(it);
    sp->data = (char *) XMALLOC(sizeof(char) * (strlen(data) + 1),
				"stack_push_data");
    if (! sp->data)
	return;
    strcpy(sp->data, data);
    stack_set(it, sp);
}

FUNCTION(fun_dup)
{
    dbref it;
    STACK *hp;			/* head of stack */
    STACK *tp;			/* temporary stack pointer */
    STACK *sp;			/* new stack element */
    int pos, count = 0;

    xvarargs_preamble("DUP", 0, 2);

    if (!fargs[0]) {
	it = player;
    } else {
	stack_object(player, it);
    }

    if (!fargs[1] || !*fargs[1]) {
	pos = 0;
    } else {
	pos = atoi(fargs[1]);
    }

    hp = stack_get(it);
    for (tp = hp; (count != pos) && (tp != NULL); count++, tp = tp->next)
	;
    if (!tp) {
	notify_quiet(player, "No such item on stack.");
	return;
    }

    sp = (STACK *) XMALLOC(sizeof(STACK), "stack_dup");
    if (!sp)
	return;
    sp->next = hp;
    sp->data = (char *) XMALLOC(sizeof(char) * (strlen(tp->data) + 1),
				"stack_dup_data");
    if (!sp->data)
	return;
    strcpy(sp->data, tp->data);
    stack_set(it, sp);
}

FUNCTION(fun_swap)
{
    dbref it;
    STACK *sp, *tp;

    xvarargs_preamble("SWAP", 0, 1);

    if (!fargs[0]) {
	it = player;
    } else {
	stack_object(player, it);
    }

    sp = stack_get(it);
    if (!sp || (sp->next == NULL)) {
	notify_quiet(player, "Not enough items on stack.");
	return;
    }

    tp = sp->next;
    sp->next = tp->next;
    tp->next = sp;
    stack_set(it, tp);
}

static void handle_pop(player, cause, buff, bufc, fargs, flag)
    dbref player, cause;
    char *buff;
    char **bufc;
    char *fargs[];
    int flag;			/* if flag, don't copy */
{
    dbref it;
    int pos, count = 0;
    STACK *sp;
    STACK *prev = NULL;

    if (!fargs[0]) {
	it = player;
    } else {
	stack_object(player, it);
    }
    if (!fargs[1] || !*fargs[1]) {
	pos = 0;
    } else {
	pos = atoi(fargs[1]);
    }

    sp = stack_get(it);
    if (!sp)
	return;

    while (count != pos) {
	if (!sp)
	    return;
	prev = sp;
	sp = sp->next;
	count++;
    }
    if (!sp)
	return;

    if (!flag) {
	safe_str(sp->data, buff, bufc);
    }
    if (count == 0) {
	stack_set(it, sp->next);
    } else {
	prev->next = sp->next;
    }
    XFREE(sp->data, "stack_pop_data");
    XFREE(sp, "stack_pop");
    s_StackCount(it, StackCount(it) - 1);
}

FUNCTION(fun_pop)
{
    xvarargs_preamble("POP", 0, 2);
    handle_pop(player, cause, buff, bufc, fargs, 0);
}

FUNCTION(fun_toss)
{
    xvarargs_preamble("TOSS", 0, 2);
    handle_pop(player, cause, buff, bufc, fargs, 1);
}

FUNCTION(fun_popn)
{
    dbref it;
    int pos, nitems, i, count = 0, over = 0, first = 0;
    STACK *sp, *tp, *xp;
    STACK *prev = NULL;
    char sep;

    varargs_preamble("POPN", 4);

    stack_object(player, it);
    pos = atoi(fargs[1]);
    nitems = atoi(fargs[2]);

    sp = stack_get(it);
    if (!sp)
	return;

    while (count != pos) {
	if (!sp)
	    return;
	prev = sp;
	sp = sp->next;
	count++;
    }
    if (!sp)
	return;

    /* We've now hit the start item, the first item. Copy 'em off. */

    for (i = 0, tp = sp; (i < nitems) && (tp != NULL); i++) {
	if (!over) {
	    /* We have to pop off the items regardless of whether
	     * or not there's an overflow, but we can save ourselves
	     * some copying if so.
	     */
	    if (!first) {
		safe_chr(sep, buff, bufc);
		first = 1;
	    }
	    over = safe_str(tp->data, buff, bufc);
	}
	xp = tp;
	tp = tp->next;
	XFREE(xp->data, "stack_popn_data");
	XFREE(xp, "stack_popn");
	s_StackCount(it, StackCount(it) - 1);
    }

    /* Relink the chain. */

    if (count == 0) {
	stack_set(it, tp);
    } else {
	prev->next = tp;
    }
}

FUNCTION(fun_peek)
{
    dbref it;
    int pos, count = 0;
    STACK *sp;

    xvarargs_preamble("POP", 0, 2);

    if (!fargs[0]) {
	it = player;
    } else {
	stack_object(player, it);
    }
    if (!fargs[1] || !*fargs[1]) {
	pos = 0;
    } else {
	pos = atoi(fargs[1]);
    }

    sp = stack_get(it);
    if (!sp)
	return;

    while (count != pos) {
	if (!sp)
	    return;
	sp = sp->next;
	count++;
    }
    if (!sp)
	return;

    safe_str(sp->data, buff, bufc);
}

FUNCTION(fun_lstack)
{
    char sep;
    dbref it;
    STACK *sp;
    char *bp;
    int over = 0, first = 1;

    mvarargs_preamble("LSTACK", 0, 2);

    if (!fargs[0]) {
	it = player;
    } else {
	stack_object(player, it);
    }

    bp = buff;
    for (sp = stack_get(it); (sp != NULL) && !over; sp = sp->next) {
	if (!first) {
	    safe_chr(sep, buff, bufc);
	} else {
	    first = 0;
	}
	over = safe_str(sp->data, buff, bufc);
    }
}

/* ---------------------------------------------------------------------------
 * fun_regparse: Slurp a string into up to ten named variables ($0 - $9).
 *               Unlike regmatch(), this returns no value.
 *               regparse(string, pattern, named vars)
 */

static void perform_regparse(buff, bufc, player, fargs, nfargs, case_option)
    char *buff, **bufc;
    dbref player;
    char *fargs[];
    int nfargs;
    int case_option;
{
    int i, nqregs;
    char *qregs[NUM_ENV_VARS];
    char matchbuf[LBUF_SIZE];
    pcre *re;
    const char *errptr;
    int erroffset;
    int offsets[PCRE_MAX_OFFSETS];
    int subpatterns;

    if (!tables) {
	/* Initialize char tables so they match current locale. */
	tables = pcre_maketables();
    }

    if ((re = pcre_compile(fargs[1], case_option,
			   &errptr, &erroffset, tables)) == NULL) {
	/* Matching error. */
	notify_quiet(player, errptr);
	return;
    }

    subpatterns = pcre_exec(re, NULL, fargs[0], strlen(fargs[0]),
			    0, 0, offsets, PCRE_MAX_OFFSETS);

    nqregs = list2arr(qregs, NUM_ENV_VARS, fargs[2], ' ');
    for (i = 0; i < nqregs; i++) {
	if (qregs[i] && *qregs[i]) {
	    if (subpatterns < 0) {
		set_xvar(player, qregs[i], NULL);
	    } else {
		pcre_copy_substring(fargs[0], offsets, subpatterns, i,
				    matchbuf, LBUF_SIZE);
		set_xvar(player, qregs[i], matchbuf);
	    }
	}
    }

    free(re);
}

FUNCTION(fun_regparse)
{
    perform_regparse(buff, bufc, player, fargs, nfargs, 0);
}

FUNCTION(fun_regparsei)
{
    perform_regparse(buff, bufc, player, fargs, nfargs, PCRE_CASELESS);
}

/* ---------------------------------------------------------------------------
 * fun_regmatch: Return 0 or 1 depending on whether or not a regular
 * expression matches a string. If a third argument is specified, dump
 * the results of a regexp pattern match into a set of arbitrary r()-registers.
 *
 * regmatch(string, pattern, list of registers)
 * If the number of matches exceeds the registers, those bits are tossed
 * out.
 * If -1 is specified as a register number, the matching bit is tossed.
 * Therefore, if the list is "-1 0 3 5", the regexp $0 is tossed, and
 * the regexp $1, $2, and $3 become r(0), r(3), and r(5), respectively.
 *
 * PCRE modifications adapted from PennMUSH.
 *
 */

static void perform_regmatch(buff, bufc, player, fargs, nfargs, case_option)
    char *buff, **bufc;
    dbref player;
    char *fargs[];
    int nfargs;
    int case_option;
{
    int i, nqregs, curq;
    char *qregs[NUM_ENV_VARS];
    pcre *re;
    const char *errptr;
    int erroffset;
    int offsets[PCRE_MAX_OFFSETS];
    int subpatterns; 

    if (!fn_range_check("REGMATCH", nfargs, 2, 3, buff, bufc))
	return;

    if (!tables) {
	/* Initialize char tables so they match current locale. */
	tables = pcre_maketables();
    }

    if ((re = pcre_compile(fargs[1], case_option,
			   &errptr, &erroffset, tables)) == NULL) {
	/* Matching error.
	 * Note difference from PennMUSH behavior:
	 * Regular expression errors return 0, not #-1 with an error
	 * message.
	 */
	notify_quiet(player, errptr);
	safe_chr('0', buff, bufc);
	return;
    }

    subpatterns = pcre_exec(re, NULL, fargs[0], strlen(fargs[0]),
			    0, 0, offsets, PCRE_MAX_OFFSETS);
    safe_ltos(buff, bufc, (subpatterns >= 0));

    /* If we don't have a third argument, we're done. */
    if (nfargs != 3) {
	free(re);
	return;
    }

    /* We need to parse the list of registers. Anything that we don't get
     * is assumed to be -1. If we didn't match, or the match went wonky,
     * then set the register to empty. Otherwise, fill the register
     * with the subexpression.
     */

    if (subpatterns == 0)
	subpatterns = PCRE_MAX_OFFSETS / 3;

    nqregs = list2arr(qregs, NUM_ENV_VARS, fargs[2], ' ');
    for (i = 0; i < nqregs; i++) {
	if (qregs[i] && *qregs[i] && is_integer(qregs[i]))
	    curq = atoi(qregs[i]);
	else
	    curq = -1;
	if ((curq < 0) || (curq >= NUM_ENV_VARS))
	    continue;
	if (!mudstate.global_regs[curq]) {
	    mudstate.global_regs[curq] = alloc_lbuf("fun_regmatch");
	}
	if (subpatterns < 0) {
	    mudstate.global_regs[curq][0] = '\0'; /* empty string */
	    mudstate.glob_reg_len[curq] = 0;
	} else {
	    pcre_copy_substring(fargs[0], offsets, subpatterns, i,
				mudstate.global_regs[curq], LBUF_SIZE);
	    mudstate.glob_reg_len[curq] = strlen(mudstate.global_regs[curq]);
	}
    }

    free(re);
}

FUNCTION(fun_regmatch)
{
    perform_regmatch(buff, bufc, player, fargs, nfargs, 0);
}

FUNCTION(fun_regmatchi)
{
    perform_regmatch(buff, bufc, player, fargs, nfargs, PCRE_CASELESS);
}

/* ---------------------------------------------------------------------------
 * fun_until: Much like while(), but operates on multiple lists ala mix().
 * until(eval_fn,cond_fn,list1,list2,compare_str,delim,output delim)
 * The delimiter terminators are MANDATORY.
 * The termination condition is a REGEXP match (thus allowing this
 * to be also used as 'eval until a termination condition is NOT met').
 */

FUNCTION(fun_until)
{
    char sep, osep;
    dbref aowner1, thing1, aowner2, thing2;
    int aflags1, aflags2, anum1, anum2, alen1, alen2;
    ATTR *ap, *ap2;
    char *atext1, *atext2, *atextbuf, *condbuf;
    char *cp[10], *os[10];
    int count[LBUF_SIZE / 2];
    int i, is_exact_same, is_same, nwords, lastn, wc;
    char *str, *dp, *savep, *bb_p;
    char tmpbuf[2];
    pcre *re;
    const char *errptr;
    int erroffset;
    int offsets[PCRE_MAX_OFFSETS];
    int subpatterns;

    /* We need at least 6 arguments. The last 2 args must be delimiters. */

    if (!fn_range_check("UNTIL", nfargs, 6, 12, buff, bufc)) {
	return;
    }
    if (!delim_check(fargs, nfargs, nfargs - 1, &sep, buff, bufc, 0,
		     player, cause, cargs, ncargs, 0)) {
	return;
    }
    if (!delim_check(fargs, nfargs, nfargs, &osep, buff, bufc, 0,
		     player, cause, cargs, ncargs, 1)) {
	return;
    }
    lastn = nfargs - 4; 

    /* Make sure we have a valid regular expression. */

    if (!tables) {
	tables = pcre_maketables();
    }
    if ((re = pcre_compile(fargs[lastn + 1], 0,
			   &errptr, &erroffset, tables)) == NULL) {
	/* Return nothing on a bad match. */
	notify_quiet(player, errptr);
	return;
    }

    /* Our first and second args can be <obj>/<attr> or just <attr>.
     * Use them if we can access them, otherwise return an empty string.
     *
     * Note that for user-defined attributes, atr_str() returns a pointer
     * to a static, and that therefore we have to be careful about what
     * we're doing.
     */

    Parse_Uattr(player, fargs[0], thing1, anum1, ap);
    Get_Uattr(player, thing1, ap, atext1, aowner1, aflags1, alen1);
    Parse_Uattr(player, fargs[1], thing2, anum2, ap2);
    if (!ap2) {
	free_lbuf(atext1);	/* we allocated this, remember? */
	return;
    }

    /* If our evaluation and condition are the same, we can save ourselves
     * some time later. There are two possibilities: we have the exact
     * same obj/attr pair, or the attributes contain identical text.
     */

    if ((thing1 == thing2) && (ap->number == ap2->number)) {
	is_same = 1;
	is_exact_same = 1;
	atext2 = atext1;
    } else {
	is_exact_same = 0; 
	atext2 = atr_pget(thing2, ap2->number, &aowner2, &aflags2, &alen2);
	if (!*atext2 || !See_attr(player, thing2, ap2, aowner2, aflags2)) {
	    free_lbuf(atext1);
	    free_lbuf(atext2);
	    return;
	}
	if (!strcmp(atext1, atext2))
	    is_same = 1;
	else 
	    is_same = 0;
    }

    atextbuf = alloc_lbuf("fun_while.eval");
    if (!is_same)
	condbuf = alloc_lbuf("fun_while.cond");
    bb_p = *bufc;

    /* Process the list one element at a time. We need to find out what
     * the longest list is; assume null-padding for shorter lists.
     */

    for (i = 0; i < 10; i++)
	cp[i] = NULL;
    cp[2] = trim_space_sep(fargs[2], sep);
    nwords = count[2] = countwords(cp[2], sep);
    for (i = 3; i <= lastn; i++) {
	cp[i] = trim_space_sep(fargs[i], sep);
	count[i] = countwords(cp[i], sep);
	if (count[i] > nwords)
	    nwords = count[i];
    }

    for (wc = 0;
	 (wc < nwords) && (mudstate.func_invk_ctr < mudconf.func_invk_lim);
	 wc++) {
	for (i = 2; i <= lastn; i++) {
	    if (count[i]) {
		os[i - 2] = split_token(&cp[i], sep);
	    } else {
		tmpbuf[0] = '\0';
		os[i - 2] = tmpbuf;
	    }
	}

	if (*bufc != bb_p) {
	    print_sep(osep, buff, bufc);
	}

	StrCopyKnown(atextbuf, atext1, alen1);
	str = atextbuf;
	savep = *bufc;
	exec(buff, bufc, 0, player, cause, EV_STRIP | EV_FCHECK | EV_EVAL,
	     &str, &(os[0]), lastn - 1);
	if (is_same) {
	    subpatterns = pcre_exec(re, NULL, savep, strlen(savep),
				    0, 0, offsets, PCRE_MAX_OFFSETS);
	    if (subpatterns >= 0)
		break;
	} else {
	    StrCopyKnown(condbuf, atext2, alen2);
	    dp = str = savep = condbuf;
	    exec(condbuf, &dp, 0, player, cause,
		 EV_STRIP | EV_FCHECK | EV_EVAL, &str, &(os[0]), lastn - 1);
	    subpatterns = pcre_exec(re, NULL, savep, strlen(savep),
				    0, 0, offsets, PCRE_MAX_OFFSETS);
	    if (subpatterns >= 0)
		break;
	}
    }
    free(re);
    free_lbuf(atext1);
    if (!is_exact_same)
	free_lbuf(atext2);
    free_lbuf(atextbuf);
    if (!is_same)
	free_lbuf(condbuf);
}
