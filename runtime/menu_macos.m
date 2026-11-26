/* The menu bar on macOS (see menu.h): the same menus as on Windows
 * (menu_items.c), in the Mac's own bar at the top of the screen, after the
 * application menu SDL makes and before its Window menu. A chosen item
 * comes back to the game as an SDL user event (menu_command). Also the
 * Mac's Open / Save panels for file_dialog. The other functions of menu.h
 * stay as on Linux (menu_win32.c). */
#import <Cocoa/Cocoa.h>
#include "menu.h"

static Uint32 g_menu_event = (Uint32)-1;                  /* SDL_RegisterEvents: a menu item was chosen */
static NSMutableArray<NSMenu*>* g_menus;                  /* ours, to remove them again */

@interface DQIXMenuTarget : NSObject
- (void)choose:(NSMenuItem*)sender;
@end
@implementation DQIXMenuTarget
- (void)choose:(NSMenuItem*)sender {
    if (g_menu_event == (Uint32)-1) return;
    SDL_Event e; SDL_zero(e);
    e.type = g_menu_event;
    e.user.code = (Sint32)sender.tag;
    SDL_PushEvent(&e);
}
@end
static DQIXMenuTarget* g_target;

static NSString* ns(const char* s) { return [NSString stringWithUTF8String:s ? s : ""] ?: @""; }

/* "Pause" with "P" at the right edge, like the key column of Windows' menus.
 * Only shown: the keys themselves stay the game's (a key equivalent here
 * would take them from it). */
static void set_title(NSMenuItem* mi, NSString* label, NSString* key, CGFloat tab) {
    mi.title = label;                                      /* VoiceOver, and tab == 0 */
    if (!key.length || tab <= 0) return;
    NSMutableParagraphStyle* ps = [[NSMutableParagraphStyle alloc] init];
    ps.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentRight location:tab options:@{}]];
    NSDictionary* body = @{ NSFontAttributeName: [NSFont menuFontOfSize:0], NSParagraphStyleAttributeName: ps };
    NSMutableAttributedString* t = [[NSMutableAttributedString alloc] initWithString:[NSString stringWithFormat:@"%@\t", label] attributes:body];
    NSDictionary* keyAttr = @{ NSFontAttributeName: [NSFont menuFontOfSize:0], NSParagraphStyleAttributeName: ps,
                               NSForegroundColorAttributeName: [NSColor secondaryLabelColor] };
    [t appendAttributedString:[[NSAttributedString alloc] initWithString:key attributes:keyAttr]];
    mi.attributedTitle = t;
}

/* Lines up each menu's key column: right of its widest label. */
static void align_keys(NSMenu* m) {
    NSDictionary* font = @{ NSFontAttributeName: [NSFont menuFontOfSize:0] };
    CGFloat label = 0, key = 0;
    for (NSMenuItem* mi in m.itemArray) {
        NSString* k = mi.representedObject;
        if (mi.submenu) { align_keys(mi.submenu); continue; }
        if (mi.isSeparatorItem) continue;
        label = MAX(label, [mi.title sizeWithAttributes:font].width);
        if ([k isKindOfClass:[NSString class]]) key = MAX(key, [k sizeWithAttributes:font].width);
    }
    if (key <= 0) return;
    CGFloat tab = label + 32 + key;
    for (NSMenuItem* mi in m.itemArray)
        if (!mi.submenu && [mi.representedObject isKindOfClass:[NSString class]]) set_title(mi, mi.title, mi.representedObject, tab);
}

/* Every item with this id (one option can be in two menus: Basic Widescreen
 * is in View and Modern). */
static void each(int id, void (^f)(NSMenuItem*)) {
    for (NSMenu* top in g_menus) {
        NSMutableArray* stack = [NSMutableArray arrayWithObject:top];
        while (stack.count) {
            NSMenu* m = stack.lastObject; [stack removeLastObject];
            for (NSMenuItem* mi in m.itemArray) {
                if (mi.submenu) [stack addObject:mi.submenu];
                else if (mi.tag == id && mi.action) f(mi);
            }
        }
    }
}

void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells) {
    (void)win;
    @autoreleasepool {
        NSMenu* bar = NSApp.mainMenu;
        if (!bar) return;
        if (g_menu_event == (Uint32)-1) g_menu_event = SDL_RegisterEvents(1);
        if (g_menu_event == (Uint32)-1) return;
        if (!g_target) g_target = [[DQIXMenuTarget alloc] init];
        for (NSMenu* m in g_menus) { NSInteger i = [bar indexOfItemWithSubmenu:m]; if (i >= 0) [bar removeItemAtIndex:i]; }
        g_menus = [NSMutableArray array];

        /* ours go before SDL's Window menu (macOS keeps it near the end) */
        NSInteger at = NSApp.windowsMenu ? [bar indexOfItemWithSubmenu:NSApp.windowsMenu] : -1;
        if (at < 0) at = bar.numberOfItems;

        NSMenu* stack[8]; int depth = -1;
        for (const MenuItem* it = g_menu_items; it->kind != MK_STOP; it++) {
            if (it->kind == MK_END) { if (depth >= 0) depth--; continue; }
            if (depth < 0 && it->kind != MK_SUB) continue;      /* only menus at the top */
            if (it->kind == MK_SEP) { [stack[depth] addItem:[NSMenuItem separatorItem]]; continue; }
            if (it->kind == MK_SHELLS) {
                for (int i = 0; i < n_shells; i++) {
                    NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:ns(shell_names[i]) action:@selector(choose:) keyEquivalent:@""];
                    mi.target = g_target; mi.tag = MI_SHELLC0 + i;
                    [stack[depth] addItem:mi];
                }
                continue;
            }
            char plain[300]; menu_plain_label(it->label, plain, sizeof plain);
            if (it->kind == MK_SUB) {
                NSMenu* m = [[NSMenu alloc] initWithTitle:ns(plain)];
                m.autoenablesItems = NO;                        /* greyed out only when the game says (menu_enable) */
                NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:ns(plain) action:nil keyEquivalent:@""];
                holder.submenu = m;
                if (depth < 0) { [bar insertItem:holder atIndex:at++]; [g_menus addObject:m]; }
                else [stack[depth] addItem:holder];
                if (depth < 7) stack[++depth] = m;
                continue;
            }
            /* "Exit": macOS calls it Quit, and it already has one (Cmd+Q) */
            NSString* label = it->id == MI_EXIT ? @"Quit" : ns(plain);
            NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:label action:@selector(choose:) keyEquivalent:@""];
            mi.target = g_target; mi.tag = it->id;
            if (it->key) mi.representedObject = ns(it->key);
            mi.state = menu_is_checked(it->id) ? NSControlStateValueOn : NSControlStateValueOff;
            mi.enabled = menu_is_enabled(it->id);
            [stack[depth] addItem:mi];
        }
        for (NSMenu* m in g_menus) align_keys(m);
    }
}

int menu_command(const SDL_Event* e) {
    return g_menu_event != (Uint32)-1 && e->type == g_menu_event ? e->user.code : 0;
}

void menu_check_os(int id, int on) {
    if (!g_menus) return;
    each(id, ^(NSMenuItem* mi) { mi.state = on ? NSControlStateValueOn : NSControlStateValueOff; });
}
void menu_enable_os(int id, int on) {
    if (!g_menus) return;
    each(id, ^(NSMenuItem* mi) { mi.enabled = on != 0; });
}

/* The Mac's Open / Save panel. The filter ("Saves (*.sav)|*.sav|All files|*.*")
 * always ends with All files in this game, so any file can be chosen. */
int file_dialog(SDL_Window* win, int save, const char* title, const char* filter, const char* default_name, char* out, size_t n) {
    (void)win; (void)filter;
    @autoreleasepool {
        NSSavePanel* p;
        if (save) { p = [NSSavePanel savePanel]; if (default_name) p.nameFieldStringValue = ns(default_name); }
        else { NSOpenPanel* o = [NSOpenPanel openPanel]; o.canChooseFiles = YES; o.canChooseDirectories = NO; o.allowsMultipleSelection = NO; p = o; }
        p.title = ns(title); p.message = ns(title);
        NSWindow* key = NSApp.keyWindow;
        NSModalResponse r = [p runModal];
        [key makeKeyAndOrderFront:nil];                        /* the keyboard back to the game */
        if (r != NSModalResponseOK || !p.URL.fileSystemRepresentation) return 0;
        const char* path = p.URL.fileSystemRepresentation;
        if (strlen(path) + 1 > n) return 0;
        memcpy(out, path, strlen(path) + 1);
        return 1;
    }
}
