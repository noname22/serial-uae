 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Interface to the Tcl/Tk GUI
  *
  * Copyright 1996 Bernd Schmidt
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "gui.h"

static void sigchldhandler(int foo)
{
}

int gui_init (int foo)
{
    return 0;
}

int gui_update (void)
{
    return 0;
}

void gui_exit (void)
{
}

void gui_fps (int x)
{
}

void gui_led (int led, int on)
{
}

void gui_filename (int num, const char *name)
{
}

void gui_display(int shortcut)
{
}
/*static void getline (char *p)
{
}*/

void gui_handle_events (void)
{
}

void gui_changesettings (void)
{
}

void gui_update_gfx (void)
{
}

void gui_lock (void)
{
}

void gui_unlock (void)
{
}

void gui_romlist_changed (void)
{
}

void gui_message (const char *format,...)
{   
       char msg[2048];
       va_list parms;

       va_start (parms,format);
       vsprintf ( msg, format, parms);
       va_end (parms);

       write_log (msg);
}
