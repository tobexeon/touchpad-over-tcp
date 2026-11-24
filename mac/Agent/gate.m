#import <AppKit/AppKit.h>

#include <libproc.h>
#include <stdio.h>
#include <string.h>

#include "gate.h"

/*
 * -[NSWorkspace frontmostApplication] is not usable here. It is refreshed from
 * workspace notifications that only get delivered when the process runs an
 * event loop, and this process has none: in a long-running agent it keeps
 * returning whichever application was frontmost at launch. Measured directly,
 * it reported the same application across Focus changes that CGWindowList
 * tracked correctly every time.
 *
 * CGWindowListCopyWindowInfo asks the window server on every call and returns
 * windows front to back, so the owner of the first layer-0 window is the
 * application the user is actually looking at. Layers above 0 are menus,
 * panels, notifications and the Dock, which is why they are skipped.
 */
static pid_t FrontWindowOwner(void) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (windows == NULL) {
        return 0;
    }
    pid_t owner = 0;
    const CFIndex count = CFArrayGetCount(windows);
    for (CFIndex index = 0; index < count; ++index) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(windows, index);
        if (info == NULL) {
            continue;
        }
        CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
        int layer = -1;
        if (layer_ref == NULL ||
            !CFNumberGetValue(layer_ref, kCFNumberIntType, &layer)) {
            continue;
        }
        if (layer != 0) {
            continue;
        }
        CFNumberRef pid_ref = CFDictionaryGetValue(info, kCGWindowOwnerPID);
        if (pid_ref != NULL) {
            (void)CFNumberGetValue(pid_ref, kCFNumberIntType, &owner);
        }
        break;
    }
    CFRelease(windows);
    return owner;
}

static pid_t ParentPid(pid_t pid) {
    struct proc_bsdinfo info;
    const int size = (int)sizeof(info);
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, size) != size) {
        return 0;
    }
    return (pid_t)info.pbi_ppid;
}

static int MatchesProcess(pid_t pid, const char *needle) {
    if (pid <= 0) {
        return 0;
    }
    NSRunningApplication *application =
        [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (application == nil) {
        return 0;
    }
    NSString *wanted = [NSString stringWithUTF8String:needle];
    if (wanted == nil) {
        return 0;
    }
    if (application.bundleIdentifier != nil &&
        [application.bundleIdentifier caseInsensitiveCompare:wanted] == NSOrderedSame) {
        return 1;
    }
    if (application.localizedName != nil &&
        [application.localizedName caseInsensitiveCompare:wanted] == NSOrderedSame) {
        return 1;
    }
    return 0;
}

static void Describe(pid_t pid, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    if (pid <= 0) {
        snprintf(out, out_size, "(no front window)");
        return;
    }
    const char *name = "?";
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) > 0) {
        const char *slash = strrchr(path, '/');
        name = slash != NULL ? slash + 1 : path;
    }
    NSRunningApplication *application =
        [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    const char *bundle =
        application != nil ? [[application bundleIdentifier] UTF8String] : NULL;
    snprintf(out, out_size, "%s [%s] pid=%d", name, bundle != NULL ? bundle : "?", (int)pid);
}

int tp_gate_status(const char *needle, char *out, size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (needle == NULL || needle[0] == '\0') {
        return 1;
    }
    @autoreleasepool {
        const pid_t owner = FrontWindowOwner();
        Describe(owner, out, out_size);
        if (owner <= 0) {
            return 0;
        }
        if (MatchesProcess(owner, needle)) {
            return 1;
        }
        /* Some applications render in a child process. */
        const pid_t parent = ParentPid(owner);
        if (parent > 0 && MatchesProcess(parent, needle)) {
            return 1;
        }
        return 0;
    }
}

int tp_gate_allows(const char *needle) {
    return tp_gate_status(needle, NULL, 0);
}
