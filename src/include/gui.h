 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Interface to the GUI
  *
  * Copyright 1996 Bernd Schmidt
  */

extern int gui_init (int);
extern int gui_update (void);
extern void gui_exit (void);
extern void gui_romlist_changed (void);
extern void gui_led (int, int);
extern void gui_handle_events (void);
extern void gui_filename (int, const char *);
extern void gui_fps (int fps);
extern void gui_lock (void);
extern void gui_unlock (void);
extern void gui_hd_led (int);
extern void gui_cd_led (int);
extern unsigned int gui_ledstate;
extern void gui_display (int shortcut);
extern void gui_message (const char *, ...);

extern unsigned int gui_ledstate;

extern int no_gui;

struct gui_info
{
    uae_u8 drive_motor[4];          /* motor on off */
    uae_u8 drive_track[4];          /* rw-head track */
    uae_u8 drive_writing[4];        /* drive is writing */
    uae_u8 drive_disabled[4];	    /* drive is disabled */
    uae_u8 powerled;                /* state of power led */
    uae_u8 drive_side;		    /* floppy side */
    uae_u8 hd;			    /* harddrive */
    uae_u8 cd;			    /* CD */
    int fps, idle;
    char df[4][256];		    /* inserted image */
    uae_u32 crc32[4];		    /* crc32 of image */
};

extern struct gui_info gui_data;

/* Functions to be called when prefs are changed by non-gui code.  */
extern void gui_update_gfx (void);

void notify_user (int msg);
int translate_message (int msg, char *out);
typedef enum {
    NUMSG_NEEDEXT2, NUMSG_NOROM, NUMSG_NOROMKEY,
    NUMSG_KSROMCRCERROR, NUMSG_KSROMREADERROR, NUMSG_NOEXTROM,
    NUMSG_MODRIP_NOTFOUND, NUMSG_MODRIP_FINISHED, NUMSG_MODRIP_SAVE,
    NUMSG_KS68EC020, NUMSG_KS68020, NUMSG_ROMNEED, NUMSG_NOZLIB, NUMSG_STATEHD,
    NUMSG_NOCAPS, NUMSG_OLDCAPS, NUMSG_KICKREP, NUMSG_KICKREPNO
} notify_user_msg;
