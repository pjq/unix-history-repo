/*
 * Copyright (c) 1989 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Tony Nardo of the Johns Hopkins University/Applied Physics Lab.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef lint
static char sccsid[] = "@(#)sprint.c	5.1 (Berkeley) %G%";
#endif /* not lint */

#include <sys/types.h>
#include <sys/time.h>
#include <tzfile.h>
#include <stdio.h>
#include "finger.h"

extern int entries;

sflag_print()
{
	extern time_t now;
	register PERSON *pn;
	register int cnt;
	register char *p;
	PERSON **list, **sort();
	time_t time();
	char *ctime();

	list = sort();
	/*
	 * short format --
	 *	login name
	 *	real name
	 *	terminal name (the XX of ttyXX)
	 *	if terminal writeable (add an '*' to the terminal name
	 *		if not)
	 *	if logged in show idle time and day logged in, else
	 *		show last login date and time.  If > 6 moths,
	 *		show year instead of time.
	 *	office location
	 *	office phone
	 */
#define	MAXREALNAME	20
	(void)printf("%-*s %-*s %s\n", UT_NAMESIZE, "Login", MAXREALNAME,
	    "Name", "Tty  Idle Login        Office      Office Phone");
	for (cnt = 0; cnt < entries; ++cnt) {
		pn = list[cnt];
		 (void)printf("%-*.*s %-*.*s ", UT_NAMESIZE, UT_NAMESIZE,
		    pn->name, MAXREALNAME, MAXREALNAME,
		    pn->realname ? pn->realname : "");
		if (!pn->loginat) {
			(void)printf("          Never logged in\n");
			continue;
		}
		(void)printf(pn->info == LOGGEDIN &&
		    !pn->writable ? "*" : " ");
		if (*pn->tty)
			(void)printf("%-2.2s ",
			    pn->tty[0] != 't' || pn->tty[1] != 't' ||
			    pn->tty[2] != 'y' ? pn->tty : pn->tty + 3);
		else
			(void)printf("   ");
		stimeprint(pn);
		p = ctime(&pn->loginat);
		(void)printf(" %.6s", p + 4);
		if (now - pn->loginat >= SECSPERDAY * DAYSPERNYEAR / 2)
			(void)printf(" %.4s ", p + 20);
		else
			(void)printf(" %.5s", p + 11);
		if (pn->office)
			(void)printf(" %-11.11s", pn->office);
		else if (pn->officephone)
			(void)printf(" %-11.11s", " ");
		if (pn->officephone)
			(void)printf(" %s", pn->officephone);
		putchar('\n');
	}
}

PERSON **
sort()
{
	extern PERSON *head;
	register PERSON *pn;
	register int cnt;
	PERSON **list;
	int psort();
	char *malloc();

	if (!(list = (PERSON **)malloc((u_int)(entries * sizeof(PERSON *))))) {
		(void)fprintf(stderr, "finger: out of space.\n");
		exit(1);
	}
	for (pn = head, cnt = 0; cnt < entries; pn = pn->next)
		list[cnt++] = pn;
	(void)qsort(list, entries, sizeof(PERSON *), psort);
	return(list);
}

psort(p, t)
	PERSON **p, **t;
{
	return(strcmp((*p)->name, (*t)->name));
}

stimeprint(pn)
	PERSON *pn;
{
	register struct tm *delta;

	if (pn->info != LOGGEDIN) {
		(void)printf("     ");
		return;
	}
	delta = gmtime(&pn->idletime);
	if (!delta->tm_yday)
		if (!delta->tm_hour)
			if (!delta->tm_min)
				(void)printf("     ");
			else
				(void)printf("%5d", delta->tm_min);
		else
			(void)printf("%2d:%02d",
			    delta->tm_hour, delta->tm_min);
	else
		(void)printf("%4dd", delta->tm_yday);
}
