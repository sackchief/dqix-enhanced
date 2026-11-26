/* Transmog: characters look as if they wore other equipment (transmog.c). */
#pragma once
#include <stdint.h>

enum { TM_SLOTS = 8 };                        /* torso, legs, arms, feet, head, weapon, shield, accessory */
enum { TM_EMPTY = -2 };                       /* a look: as if nothing were worn there ("empty") */
extern const char* const g_tm_slot_names[TM_SLOTS];

int  transmog_ignored(void);                 /* looks in it that don't fit their slot (ignored) */
int  transmog_load(const char* path);        /* transmog.ini (NULL: the same file again); the number of looks in it */
int  transmog_save(void);
void transmog_enable(int on);                /* Mods > Transmog */
void transmog_refresh(void);                 /* the party's models are made again, at the next field frame */
void transmog_block(int blocked);            /* playing together: off */
int  transmog_active(void);
/* name: the character's name (text, as the game keeps it) */
int  transmog_get(const uint8_t* name, int slot);          /* item number, TM_EMPTY, or -1: as worn */
void transmog_set(const uint8_t* name, int slot, int item);

/* Writes the item numbers of every piece of equipment, by slot, with their
 * names from the ROM in that language (1 English, 2 French, 3 German,
 * 4 Italian, 5 Spanish): the number of items, or -1 (transmog_items.c). */
int  transmog_write_items(const char* path, int language);
