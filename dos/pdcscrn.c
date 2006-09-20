/************************************************************************ 
 * This file is part of PDCurses. PDCurses is public domain software;	*
 * you may use it for any purpose. This software is provided AS IS with	*
 * NO WARRANTY whatsoever.						*
 *									*
 * If you use PDCurses in an application, an acknowledgement would be	*
 * appreciated, but is not mandatory. If you make corrections or	*
 * enhancements to PDCurses, please forward them to the current		*
 * maintainer for the benefit of other users.				*
 *									*
 * No distribution of modified PDCurses code may be made under the name	*
 * "PDCurses", except by the current maintainer. (Although PDCurses is	*
 * public domain, the name is a trademark.)				*
 *									*
 * See the file maintain.er for details of the current maintainer.	*
 ************************************************************************/

#include "pdcdos.h"

#include <stdlib.h>
#include <string.h>

#ifdef __DJGPP__
# include <sys/movedata.h>
#endif

RCSID("$Id: pdcscrn.c,v 1.53 2006/09/20 08:56:30 wmcbrine Exp $");

int	pdc_adapter;		/* screen type				*/
int	pdc_scrnmode;		/* default screen mode			*/
int	pdc_font;		/* default font size			*/
bool	pdc_direct_video;	/* allow direct screen memory writes	*/
bool	pdc_bogus_adapter;	/* TRUE if adapter has insane values	*/
unsigned pdc_video_seg;		/* video base segment			*/
unsigned pdc_video_ofs;		/* video base offset			*/

static bool sizeable = FALSE;	/* TRUE if adapter is resizeable	*/

static unsigned short *saved_screen = NULL;
static int saved_lines = 0;
static int saved_cols = 0;

static int saved_scrnmode[3];
static int saved_font[3];

/* Thanks to Jeff Duntemann, K16RA for providing the impetus
   (through the Dr. Dobbs Journal, March 1989 issue) for getting
   the routines below merged into Bjorn Larsson's PDCurses 1.3...
	-- frotz@dri.com	900730 */

/* get_font() - Get the current font size */

static int get_font(void)
{
	int retval;

	retval = getdosmemword(0x485);

	/* Assume the MDS Genius is in 66 line mode. */

	if ((retval == 0) && (pdc_adapter == _MDS_GENIUS))
		retval = _FONT15;

	switch (pdc_adapter)
	{
	case _MDA:
		retval = 10;	/* POINTS is not certain on MDA/Hercules */
		break;

	case _EGACOLOR:
	case _EGAMONO:
		switch (retval)
		{
		case _FONT8:
		case _FONT14:
			break;
		default:
			retval = _FONT14;
		}
		break;

	case _CGA:
		retval = _FONT8;
	}

	return retval;
}

/* set_font() - Sets the current font size, if the adapter allows such a 
   change. It is an error to attempt to change the font size on a 
   "bogus" adapter. The reason for this is that we have a known video 
   adapter identity problem. e.g. Two adapters report the same identifying 
   characteristics. */

static void set_font(int size)
{
	union REGS regs;

	if (pdc_bogus_adapter)
		return;

	switch (pdc_adapter)
	{
	case _CGA:
	case _MDA:
	case _MCGACOLOR:
	case _MCGAMONO:
	case _MDS_GENIUS:
		break;

	case _EGACOLOR:
	case _EGAMONO:
		if (sizeable && (pdc_font != size))
		{
			switch (size)
			{
			case _FONT8:
				regs.h.ah = 0x11;
				regs.h.al = 0x12;
				regs.h.bl = 0x00;
				int86(0x10, &regs, &regs);
				break;
			case _FONT14:
				regs.h.ah = 0x11;
				regs.h.al = 0x11;
				regs.h.bl = 0x00;
				int86(0x10, &regs, &regs);
			}
		}
		break;

	case _VGACOLOR:
	case _VGAMONO:
		if (sizeable && (pdc_font != size))
		{
			switch (size)
			{
			case _FONT8:
				regs.h.ah = 0x11;
				regs.h.al = 0x12;
				regs.h.bl = 0x00;
				int86(0x10, &regs, &regs);
				break;
			case _FONT14:
				regs.h.ah = 0x11;
				regs.h.al = 0x11;
				regs.h.bl = 0x00;
				int86(0x10, &regs, &regs);
				break;
			case _FONT16:
				regs.h.ah = 0x11;
				regs.h.al = 0x14;
				regs.h.bl = 0x00;
				int86(0x10, &regs, &regs);
			}
		}
	}

	curs_set(SP->visibility);

	pdc_font = get_font();
}

/* set_80x25() - force a known screen state: 80x25 text mode. Forces the 
   appropriate 80x25 alpha mode given the display adapter. */

static void set_80x25(void)
{
	union REGS regs;

	switch (pdc_adapter)
	{
	case _CGA:
	case _EGACOLOR:
	case _EGAMONO:
	case _VGACOLOR:
	case _VGAMONO:
	case _MCGACOLOR:
	case _MCGAMONO:
		regs.h.ah = 0x00;
		regs.h.al = 0x03;
		int86(0x10, &regs, &regs);
		break;
	case _MDA:
		regs.h.ah = 0x00;
		regs.h.al = 0x07;
		int86(0x10, &regs, &regs);
	}
}

/* get_scrn_mode() - Return the current BIOS video mode */

static int get_scrn_mode(void)
{
	union REGS regs;

	regs.h.ah = 0x0f;
	int86(0x10, &regs, &regs);

	return (int)regs.h.al;
}

/* set_scrn_mode() - Sets the BIOS Video Mode Number only if it is 
   different from the current video mode. */

static void set_scrn_mode(int new_mode)
{
	union REGS regs;

	if (get_scrn_mode() != new_mode)
	{
		regs.h.ah = 0;
		regs.h.al = (unsigned char) new_mode;
		int86(0x10, &regs, &regs);
	}

	pdc_font = get_font();
	pdc_scrnmode = new_mode;
	LINES = PDC_get_rows();
	COLS = PDC_get_columns();
}

/* sanity_check() - A video adapter identification sanity check. This 
   routine will force sane values for various control flags. */

static int sanity_check(int adapter)
{
	int fontsize = get_font();
	int rows = PDC_get_rows();

	PDC_LOG(("PDC_sanity_check() - called: Adapter %d\n", adapter));

	switch (adapter)
	{
	case _EGACOLOR:
	case _EGAMONO:
		switch (rows)
		{
		case 25:
		case 43:	
			break;
		default:
			pdc_bogus_adapter = TRUE;
		}

		switch (fontsize)
		{
		case _FONT8:
		case _FONT14:
			break;
		default:
			pdc_bogus_adapter = TRUE;
		}
		break;

	case _VGACOLOR:
	case _VGAMONO:
		break;

	case _CGA:
	case _MDA:
	case _MCGACOLOR:
	case _MCGAMONO:
		switch (rows)
		{
		case 25:
			break;
		default:
			pdc_bogus_adapter = TRUE;
		}
		break;

	default:
		pdc_bogus_adapter = TRUE;
	}

	if (pdc_bogus_adapter)
	{
		sizeable = FALSE;
		pdc_direct_video = FALSE;
	}

	return adapter;
}

/* query_adapter_type() - Determine PC video adapter type. */

static int query_adapter_type(void)
{
	union REGS regs;
	int equip;
	int retval = _NONE;

	/* thanks to paganini@ax.apc.org for the GO32 fix */

#if defined(__DJGPP__) && defined(NOW_WORKS)
# include <dpmi.h>
	_go32_dpmi_registers dpmi_regs;
#endif

#if !defined(__DJGPP__) && !defined(__WATCOMC__)
	struct SREGS segs;
#endif
	short video_base = getdosmemword(0x463);

	PDC_LOG(("PDC_query_adapter_type() - called\n"));

	/* attempt to call VGA Identify Adapter Function */

	regs.h.ah = 0x1a;
	regs.h.al = 0;
	int86(0x10, &regs, &regs);

	if ((regs.h.al == 0x1a) && (retval == _NONE))
	{
		/* We know that the PS/2 video BIOS is alive and well. */

		switch (regs.h.al)
		{
		case 0:
			retval = _NONE;
			break;
		case 1:
			retval = _MDA;
			break;
		case 2:
			retval = _CGA;
			break;
		case 4:
			retval = _EGACOLOR;
			sizeable = TRUE;
			break;
		case 5:
			retval = _EGAMONO;
			break;
		case 26:			/* ...alt. VGA BIOS... */
		case 7:
			retval = _VGACOLOR;
			sizeable = TRUE;
			break;
		case 8:
			retval = _VGAMONO;
			break;
		case 10:
		case 13:
			retval = _MCGACOLOR;
			break;
		case 12:
			retval = _MCGAMONO;
			break;
		default:
			retval = _CGA;
		}
	}
	else
	{
		/* No VGA BIOS, check for an EGA BIOS by selecting an
		   Alternate Function Service...

		   bx == 0x0010 --> return EGA information */

		regs.h.ah = 0x12;
# ifdef __WATCOMC__
		regs.w.bx = 0x10;
# else
		regs.x.bx = 0x10;
# endif
		int86(0x10, &regs, &regs);

		if ((regs.h.bl != 0x10) && (retval == _NONE))
		{
			/* An EGA BIOS exists */

			regs.h.ah = 0x12;
			regs.h.bl = 0x10;
			int86(0x10, &regs, &regs);

			if (regs.h.bh == 0)
				retval = _EGACOLOR;
			else
				retval = _EGAMONO;
		}
		else if (retval == _NONE)
		{
			/* Now we know we only have CGA or MDA */

			int86(0x11, &regs, &regs);
			equip = (regs.h.al & 0x30) >> 4;

			switch (equip)
			{
			case 1:
			case 2:
				retval = _CGA;
				break;
			case 3:
				retval = _MDA;
				break;
			default:
				retval = _NONE;
			}
		}
	}

	if (video_base == 0x3d4)
	{
		pdc_video_seg = 0xb800;
		switch (retval)
		{
		case _EGAMONO:
			retval = _EGACOLOR;
			break;
		case _VGAMONO:
			retval = _VGACOLOR;
		}
	}

	if (video_base == 0x3b4)
	{
		pdc_video_seg = 0xb000;
		switch (retval)
		{
		case _EGACOLOR:
			retval = _EGAMONO;
			break;
		case _VGACOLOR:
			retval = _VGAMONO;
		}
	}

	if ((retval == _NONE)
#ifndef CGA_DIRECT
	||  (retval == _CGA)
#endif
	)
		pdc_direct_video = FALSE;

	if ((unsigned int) pdc_video_seg == 0xb000)
		SP->mono = TRUE;
	else
		SP->mono = FALSE;

	/* Check for DESQview shadow buffer
	   thanks to paganini@ax.apc.org for the GO32 fix */

#if defined(__DJGPP__) && defined(NOW_WORKS)
	dpmi_regs.h.ah = 0xfe;
	dpmi_regs.h.al = 0;
	dpmi_regs.x.di = pdc_video_ofs;
	dpmi_regs.x.es = pdc_video_seg;
	_go32_dpmi_simulate_int(0x10, &dpmi_regs);
	pdc_video_ofs = dpmi_regs.x.di;
	pdc_video_seg = dpmi_regs.x.es;
#endif

#if !defined(__DJGPP__) && !defined(__WATCOMC__)
	regs.h.ah = 0xfe;
	regs.h.al = 0;
	regs.x.di = pdc_video_ofs;
	segs.es   = pdc_video_seg;
	int86x(0x10, &regs, &regs, &segs);
	pdc_video_ofs = regs.x.di;
	pdc_video_seg = segs.es;
#endif
	if (!pdc_adapter)
		pdc_adapter = retval;

	return sanity_check(retval);
}

/*man-start**************************************************************

  PDC_scr_close()	- Internal low-level binding to close the
			  physical screen

  PDCurses Description:
	May restore the screen to its state before PDC_scr_open(); 
	miscellaneous cleanup.

  PDCurses Return Value:
	This function returns OK on success, otherwise an ERR is returned.

  Portability:
	PDCurses  void PDC_scr_close(void);

**man-end****************************************************************/

void PDC_scr_close(void)
{
#if SMALL || MEDIUM
# ifndef __PACIFIC__
	struct SREGS segregs;
# endif
	int ds;
#endif
	PDC_LOG(("PDC_scr_close() - called\n"));

	if ((getenv("PDC_RESTORE_SCREEN") != NULL) && (saved_screen != NULL))
	{
#ifdef __DJGPP__
		dosmemput(saved_screen, saved_lines * saved_cols * 2,
			(unsigned long)_FAR_POINTER(pdc_video_seg,
			pdc_video_ofs));
#else
# if (SMALL || MEDIUM)
#  ifdef __PACIFIC__
		ds = FP_SEG((void far *)saved_screen);
#  else
		segread(&segregs);
		ds = segregs.ds;
#  endif
		movedata(ds, (int)saved_screen, pdc_video_seg, pdc_video_ofs,
		(saved_lines * saved_cols * 2));
# else
		memcpy((void *)_FAR_POINTER(pdc_video_seg, pdc_video_ofs),
		(void *)saved_screen, (saved_lines * saved_cols * 2));
# endif
#endif
		free(saved_screen);
		saved_screen = NULL;
	}

	reset_shell_mode();

	if (SP->visibility != 1)
		curs_set(1);

	/* Position cursor to the bottom left of the screen. */

	PDC_gotoyx(PDC_get_rows() - 2, 0);
}

void PDC_scr_exit(void)
{
	if (SP)
		free(SP);
	if (pdc_atrtab)
		free(pdc_atrtab);
}

/*man-start**************************************************************

  PDC_scr_open()	- Internal low-level binding to open the
			  physical screen

  PDCurses Description:
	The platform-specific part of initscr() -- allocates SP, does 
	miscellaneous intialization, and may save the existing screen 
	for later restoration.

  PDCurses Return Value:
	This function returns OK on success, otherwise an ERR is returned.

  Portability:
	PDCurses  int PDC_scr_open(int argc, char **argv);

**man-end****************************************************************/

int PDC_scr_open(int argc, char **argv)
{
#if SMALL || MEDIUM
# ifndef __PACIFIC__
	struct SREGS segregs;
# endif
	int ds;
#endif
	PDC_LOG(("PDC_scr_open() - called\n"));

	SP = calloc(1, sizeof(SCREEN));
	pdc_atrtab = calloc(MAX_ATRTAB, 1);

	if (!SP || !pdc_atrtab)
		return ERR;

	SP->orig_attr	= FALSE;

	PDC_get_cursor_pos(&SP->cursrow, &SP->curscol);

	pdc_direct_video = TRUE;	/* Assume that we can	      */
	pdc_video_seg	= 0xb000;	/* Base screen segment addr   */
	pdc_video_ofs	= 0x0;		/* Base screen segment ofs    */

	pdc_adapter	= query_adapter_type();
	pdc_scrnmode	= get_scrn_mode();
	pdc_font	= get_font();

	SP->lines	= PDC_get_rows();
	SP->cols	= PDC_get_columns();

	SP->orig_cursor	= PDC_get_cursor_mode();
	SP->orgcbr	= PDC_get_ctrl_break();

	/* If the environment variable PDCURSES_BIOS is set, the DOS 
	   int10() BIOS calls are used in place of direct video memory 
	   access. */

	if (getenv("PDCURSES_BIOS") != NULL)
		pdc_direct_video = FALSE;

	/* This code for preserving the current screen. */

	if (getenv("PDC_RESTORE_SCREEN") != NULL)
	{
		saved_lines = SP->lines;
		saved_cols = SP->cols;

		saved_screen = malloc(saved_lines * saved_cols * 2);

		if (!saved_screen)
		{
			SP->_preserve = FALSE;
			return OK;
		}
#ifdef __DJGPP__
		dosmemget ((unsigned long)_FAR_POINTER(pdc_video_seg,
			pdc_video_ofs), saved_lines * saved_cols * 2,
			saved_screen);
#else
# if SMALL || MEDIUM
#  ifdef __PACIFIC__
		ds = FP_SEG((void far *) saved_screen);
#  else
		segread(&segregs);
		ds = segregs.ds;
#  endif
		movedata(pdc_video_seg, pdc_video_ofs, ds, (int)saved_screen,
			(saved_lines * saved_cols * 2));
# else
		memcpy((void *)saved_screen,
			(void *)_FAR_POINTER(pdc_video_seg, pdc_video_ofs),
			(saved_lines * saved_cols * 2));
# endif
#endif
	}

	SP->_preserve = (getenv("PDC_PRESERVE_SCREEN") != NULL);

	return OK;
}

/*man-start**************************************************************

  PDC_resize_screen()	- Internal low-level function to resize screen

  PDCurses Description:
	This function provides a means for the application program to
	resize the overall dimensions of the screen.  Under DOS and OS/2
	the application can tell PDCurses what size to make the screen;
	under X11, resizing is done by the user and this function simply
	adjusts its internal structures to fit the new size.

  PDCurses Return Value:
	This function returns OK on success, otherwise an ERR is returned.

  PDCurses Errors:

  Portability:
	PDCurses  int PDC_resize_screen(int, int);

**man-end****************************************************************/

int PDC_resize_screen(int nlines, int ncols)
{
	PDC_LOG(("PDC_resize_screen() - called. Lines: %d Cols: %d\n",
		nlines, ncols));

	/* Trash the stored value of orig_cursor -- it's only good if 
	   the video mode doesn't change */

	SP->orig_cursor = 0x0607;

	switch (pdc_adapter)
	{
	case _EGACOLOR:
		if (nlines >= 43)
			set_font(_FONT8);
		else
			set_80x25();
		break;

	case _VGACOLOR:
		if (nlines > 28)
			set_font(_FONT8);
		else
			if (nlines > 25)
				set_font(_FONT14);
			else
				set_80x25();
	}

	return OK;
}

void PDC_reset_prog_mode(void)
{
        PDC_LOG(("PDC_reset_prog_mode() - called.\n"));
}

void PDC_reset_shell_mode(void)
{
        PDC_LOG(("PDC_reset_shell_mode() - called.\n"));
}


void PDC_restore_screen_mode(int i)
{
	if (i >= 0 && i <= 2)
	{
		pdc_font = get_font();
		set_font(saved_font[i]);

		if (get_scrn_mode() != saved_scrnmode[i])
			set_scrn_mode(saved_scrnmode[i]);
	}
}

void PDC_save_screen_mode(int i)
{
	if (i >= 0 && i <= 2)
	{
		saved_font[i] = pdc_font;
		saved_scrnmode[i] = pdc_scrnmode;
	}
}
