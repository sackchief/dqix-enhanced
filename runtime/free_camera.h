/* Free camera (free_camera.c): L/R turn the camera everywhere, all the way round. */
#pragma once
extern int g_patch_free_camera;          /* read by the recompiled camera code */
void free_camera_update(int want);       /* the setting (1/0), or -1: as it was; checks sessions and replays */
extern int g_patch_camera_no_zoom;       /* read by the recompiled camera code: no camera zones (zoom-outs) */
void camera_no_zoom_update(int want);    /* the setting (1/0), or -1: as it was; checks sessions and replays */
/* Zoom (with the free camera): the field camera's distance times a factor.
 * Called once a frame with the zoom keys held and the right stick's up/down
 * (-32768..32767), and for each turn of the mouse wheel. */
void free_camera_zoom_frame(int zoom_in, int zoom_out, int stick_y);
void free_camera_zoom_wheel(int notches);  /* > 0: closer */
int free_camera_zoom_percent(void);
/* Zoomed out further than the game's own distance: the renderers leave the
 * fog out (it is set up for the usual distance; from further away all of the
 * field would be in it). */
int free_camera_no_fog(void);
int free_camera_cutscene(void);         /* a scene plays (free-to-walk byte 2) */
int free_camera_both_sides_now(void);   /* gpu3d.c: draw both sides now (free camera on, not in a scene) */
int field_camera_takes_lr(void);         /* L/R would turn the field camera now (input.c, the right stick) */
extern int g_free_camera_both_sides;     /* gpu3d.c: draw the back of one-sided polygons too while the free camera is on */
