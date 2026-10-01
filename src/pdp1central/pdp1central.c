// pdp1central: one desktop window to control the PiDP-1 emulator.
// It shows what is running, starts, stops, restarts and reloads the emulator through
// bin/pdp1control.sh, keeps the script's start-time choices in pdp1control.config, mounts and
// saves paper tapes, and edits pidp1.config from a schema file that describes every setting.
// Everything that is not drawing is in the core (core.h); this file is the Nuklear front end on
// SDL2. One thread and one event loop: SDL_WaitEventTimeout at 250 ms, the running child polled
// on every pass and process status refreshed once a second. Nothing in the loop blocks longer
// than a command port's timeout.
// Usage: pdp1central [-r root] [-s schema]
//
// 30-Sep-2026 wje (Claude) - written.
// 01-Oct-2026 wje (Claude) - color schemes (style.c), chosen on the Control tab and kept with
//     any overrides in pdp1central.config.

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "nuklear.h"
#include "nuklear_sdl_renderer.h"

#include "core.h"
#include "style.h"

#define DEFAULT_ROOT "/opt/pidp1-mods"
#define EMU_PORT 1040           // the emulator's command port
#define FRONT_PORT 1050         // the running front end's command port
#define LOG_LINES 500
#define MAX_OTHER 128           // names in pidp1.config the schema does not have
#define MAX_QUEUE 4             // script commands waiting for the running child
#define MAX_SCHEMES 8           // color schemes the Colors choice lists
#define WINDOW_W 960
#define WINDOW_H 680
#define ROW_H 26

// What the child slot is doing.
typedef enum
{
    JOB_NONE,
    JOB_SCRIPT,
    JOB_ASK_MOUNT,
    JOB_ASK_PUNCH
} Job;

typedef enum
{
    TAB_CONTROL,
    TAB_SETTINGS,
    TAB_LOG
} Tab;

// Change counts for the settings footer.
typedef struct
{
    int total;
    int reload;
    int panel;
    int restart;
    int program;
    int ignored;
} Changes;

// The groups, in the order of the editor's tabs.
static const char *groupNames[] = { "Debugger", "Throttle", "CPU", "Displays", "Audio", "Devices",
    "Panel", "Timing", "Other", "Retired" };
#define GROUP_COUNT ((int)(sizeof(groupNames) / sizeof(groupNames[0])))
#define GROUP_OTHER 8
#define GROUP_RETIRED 9

static char rootPath[PATH_MAX];
static char schemaPath[PATH_MAX];
static char configPath[PATH_MAX];
static char examplePath[PATH_MAX];
static char controlPath[PATH_MAX];
static char scriptPath[PATH_MAX];
static char askOpenPath[PATH_MAX];
static char askSavePath[PATH_MAX];
static char fontPath[PATH_MAX];
static char stylePath[PATH_MAX];

static Schema *schemaP;
static ConfFile *diskCfP;       // pidp1.config as on disk
static ConfFile *editCfP;       // the same with the editor's changes
static ConfFile *controlCfP;    // pdp1control.config
static ConfFile *styleCfP;      // pdp1central.config, NULL if it could not be read
static int styleOverrides;      // color overrides it has in effect
static char pendingScheme[CONF_MAX_VALUE + 1];  // chosen in the window, not yet applied
static char otherNames[MAX_OTHER][CONF_MAX_VALUE + 1];
static int otherCount;
static char (*editBufsP)[CONF_MAX_VALUE + 1];   // text fields, one per schema entry then per other name
static int activeEdit = -1;     // the text field being typed in, which must not be refreshed

static char *logLinesP[LOG_LINES];
static int logFirst;
static int logCount;
static char childPartial[1024]; // child output since its last newline
static size_t childPartialLen;

static Job job;
static Uint32 jobStartMs;
static char jobLabel[64];
static char dialogPath[PATH_MAX];   // the last line a file dialog printed
static char *queueP[MAX_QUEUE];
static int queueCount;

static int pdp1Count;
static int panelCount;
static int frontCount;
static int usbCount;
static bool portUp;
static Uint32 statusMs;

static char mountedPath[PATH_MAX];  // what pdp1central mounted in the reader, "" if not known
static bool mountKnown;
static char lastMountPath[PATH_MAX];
static char punchPath[PATH_MAX];

static Tab tab = TAB_CONTROL;
static int group;
static bool diskConflict;       // pidp1.config changed on disk while there are unsaved edits
static char savedNote[512];     // after a save: what still needs a restart
static bool savedNeedsRestart;
static char statusNote[256];    // the last error, shown in the top strip
static bool exitPending;        // leave once the running child and the queue are done
static bool exitNow;

// Add a line to the log tab, with the time; the oldest line goes once there are LOG_LINES.
static void
appLog(const char *fmtP, ...)
{
va_list ap;
time_t now;
struct tm tmNow;
char text[1200];
size_t len;
int slot;

    now = time(NULL);
    localtime_r(&now, &tmNow);
    len = strftime(text, sizeof(text), "%H:%M:%S ", &tmNow);
    va_start(ap, fmtP);
    vsnprintf(text + len, (sizeof(text) - len), fmtP, ap);
    va_end(ap);

    if( logCount == LOG_LINES )
    {
        free(logLinesP[logFirst]);
        logLinesP[logFirst] = NULL;
        logFirst = ((logFirst + 1) % LOG_LINES);
        logCount--;
    }

    slot = ((logFirst + logCount) % LOG_LINES);
    logLinesP[slot] = strdup(text);
    logCount++;
}

// Log an error and show it in the top strip until the next one.
static void
appError(const char *fmtP, ...)
{
va_list ap;

    va_start(ap, fmtP);
    vsnprintf(statusNote, sizeof(statusNote), fmtP, ap);
    va_end(ap);
    appLog("%s", statusNote);
}

// Build <root>/<tail> into outP.
// Returns false if the path does not fit.
static bool
rootJoin(char *outP, const char *tailP)
{
    return( snprintf(outP, PATH_MAX, "%s/%s", rootPath, tailP) < PATH_MAX );
}

// Whether two values from confGet differ, where NULL means no active line.
static bool
valuesDiffer(const char *aP, const char *bP)
{
    if( !aP || !bP )
    {
        return(aP != bP);
    }

    return(strcmp(aP, bP) != 0);
}

// The value confGet returns is overwritten by the next call on the same file, so copy it.
static const char *
getValue(ConfFile *cfP, const char *nameP, char *bufP)
{
const char *valueP;

    if( !(valueP = confGet(cfP, nameP)) )
    {
        return(NULL);
    }

    snprintf(bufP, (CONF_MAX_VALUE + 1), "%s", valueP);
    return(bufP);
}

// A start-time choice from pdp1control.config, or the script's default for it, copied into bufP
// (CONF_MAX_VALUE + 1 bytes), since confGet's result lasts only until its next call.
static const char *
controlChoice(const char *nameP, const char *defaultP, char *bufP)
{
    if( !controlCfP || !getValue(controlCfP, nameP, bufP) )
    {
        snprintf(bufP, (CONF_MAX_VALUE + 1), "%s", defaultP);
    }

    return(bufP);
}

// Rebuild the list of names pidp1.config sets that the schema does not have, and size the text
// field buffers to match.
static void
refreshOtherNames(void)
{
char names[MAX_OTHER * 2][CONF_MAX_VALUE + 1];
int count, i;

    count = confNames(editCfP, names, (MAX_OTHER * 2));
    otherCount = 0;
    for( i = 0; (i < count) && (otherCount < MAX_OTHER); i++ )
    {
        if( !schemaFind(schemaP, names[i]) )
        {
            strcpy(otherNames[otherCount++], names[i]);
        }
    }

    free(editBufsP);
    editBufsP = calloc((schemaP->count + MAX_OTHER), sizeof(*editBufsP));
    activeEdit = -1;
}

// Load pidp1.config from disk, dropping any unsaved edits.
static void
loadConfig(void)
{
ConfFile *newP;

    if( !(newP = confLoad(configPath)) )
    {
        appError("cannot read %s: %s", configPath, strerror(errno));
        return;
    }

    confFree(diskCfP);
    confFree(editCfP);
    diskCfP = newP;
    editCfP = confCopy(diskCfP);
    diskConflict = false;
    refreshOtherNames();
}

// Read pdp1central.config and use its colors. A file that cannot be read leaves the colors as
// they are and is not watched, so the error is not repeated every second.
static void
loadStyle(struct nk_context *ctxP)
{
ConfFile *newP;
int error;

    newP = confLoad(stylePath);
    error = errno;
    confFree(styleCfP);
    if( !(styleCfP = newP) )
    {
        appError("cannot read %s: %s; the colors stay as they are", stylePath, strerror(error));
        return;
    }

    styleOverrides = styleLoad(ctxP, styleCfP, appLog);
}

// Make a scheme the one in use, and write it to pdp1central.config at once. The file's other
// lines are kept as they are, and its overrides still apply.
static void
setScheme(struct nk_context *ctxP, const char *nameP)
{
ConfFile *newP;

    if( !styleCfP || confChangedOnDisk(styleCfP) )
    {
        if( !(newP = confLoad(stylePath)) )
        {
            appError("cannot read %s", stylePath);
            return;
        }
        confFree(styleCfP);
        styleCfP = newP;
    }

    if( !confSet(styleCfP, "scheme", nameP) || !confSave(styleCfP) )
    {
        appError("%s: %s", stylePath, styleCfP->errorText);
        return;
    }

    appLog("%s: scheme=%s", stylePath, nameP);
    styleOverrides = styleLoad(ctxP, styleCfP, appLog);
}

// Count the editor's unsaved changes, by when each takes effect.
static Changes
countChanges(void)
{
Changes changes;
const SchemaEntry *entryP;
char diskBuf[CONF_MAX_VALUE + 1];
char editBuf[CONF_MAX_VALUE + 1];
const char *diskP, *editP;
int i;

    memset(&changes, 0, sizeof(changes));
    for( i = 0; i < (schemaP->count + otherCount); i++ )
    {
        entryP = ((i < schemaP->count) ? &schemaP->entriesP[i] : NULL);
        diskP = getValue(diskCfP, (entryP ? entryP->name : otherNames[i - schemaP->count]), diskBuf);
        editP = getValue(editCfP, (entryP ? entryP->name : otherNames[i - schemaP->count]), editBuf);
        if( !valuesDiffer(diskP, editP) )
        {
            continue;
        }

        changes.total++;
        if( !entryP )
        {
            changes.restart++;      // a setting no schema line describes: only a restart is sure
        }
        else if( entryP->sticky && !editP )
        {
            changes.restart++;      // its reader keeps the old value when the line goes
        }
        else if( entryP->applies == APPLIES_RELOAD )
        {
            changes.reload++;
        }
        else if( entryP->applies == APPLIES_PANEL )
        {
            changes.panel++;
        }
        else if( entryP->applies == APPLIES_RESTART )
        {
            changes.restart++;
        }
        else if( entryP->applies == APPLIES_PROGRAM )
        {
            changes.program++;
        }
        else
        {
            changes.ignored++;
        }
    }

    return(changes);
}

// Queue a pdp1control.sh command; it runs when the child slot is free.
static void
queueScript(const char *commandP)
{
    if( queueCount < MAX_QUEUE )
    {
        queueP[queueCount++] = strdup(commandP);
    }
}

// Start the next queued script command, if the child slot is free.
static void
startQueued(void)
{
char *argv[4];
char *commandP;
int i;

    if( (job != JOB_NONE) || (queueCount == 0) )
    {
        return;
    }

    commandP = queueP[0];
    for( i = 1; i < queueCount; i++ )
    {
        queueP[i - 1] = queueP[i];
    }
    queueCount--;

    argv[0] = "/bin/bash";
    argv[1] = scriptPath;
    argv[2] = commandP;
    argv[3] = NULL;
    if( !childStart(argv) )
    {
        appError("cannot run %s %s", scriptPath, commandP);
    }
    else
    {
        job = JOB_SCRIPT;
        jobStartMs = SDL_GetTicks();
        snprintf(jobLabel, sizeof(jobLabel), "pdp1control %s", commandP);
        appLog("running %s", jobLabel);
    }

    free(commandP);
}

// Run a file dialog helper as the child, so the window keeps drawing while it is open.
static void
startDialog(Job which)
{
char *argv[4];

    argv[0] = "/usr/bin/env";
    argv[1] = "python3";
    argv[2] = ((which == JOB_ASK_MOUNT) ? askOpenPath : askSavePath);
    argv[3] = NULL;
    dialogPath[0] = '\0';
    if( !childStart(argv) )
    {
        appError("cannot run %s", argv[2]);
        return;
    }

    job = which;
    jobStartMs = SDL_GetTicks();
    snprintf(jobLabel, sizeof(jobLabel), "%s", ((which == JOB_ASK_MOUNT) ? "choosing a tape" : "choosing a punch file"));
}

// Send a tape command, to the front end or else straight to the emulator, and log the result.
// Returns true if something took it.
static bool
sendTape(const char *lineP)
{
char reply[1024];
int result;

    result = tapeCommand(FRONT_PORT, EMU_PORT, lineP, reply, sizeof(reply));
    if( result == FRONT_PORT )
    {
        appLog("sent \"%s\" to the front end on %d", lineP, FRONT_PORT);
        return(true);
    }

    if( result == EMU_PORT )
    {
        appLog("no front end on %d; sent \"%s\" to the emulator on %d: %s", FRONT_PORT, lineP, EMU_PORT, reply);
        return(true);
    }

    appError("\"%s\" not sent: %s", lineP, ((result == -1) ? "the emulator is not running" :
        ((result == -2) ? "no answer" : "port error")));
    return(false);
}

// Check a path from a file dialog: absolute, and with nothing the command ports split on.
static bool
tapePathOk(const char *pathP)
{
    if( pathP[0] != '/' )
    {
        appError("not sent: \"%s\" is not an absolute path", pathP);
        return(false);
    }

    if( strpbrk(pathP, " \t") )
    {
        appError("not sent: the command ports split a path at spaces; rename \"%s\"", pathP);
        return(false);
    }

    return(true);
}

// Mount a reader tape and remember it.
static void
mountTape(const char *pathP)
{
char line[PATH_MAX + 4];
char path[PATH_MAX];

    // Remount passes lastMountPath itself, and snprintf into its own source empties it.
    snprintf(path, sizeof(path), "%s", pathP);
    if( !tapePathOk(path) )
    {
        return;
    }

    snprintf(line, sizeof(line), "r %s", path);
    if( sendTape(line) )
    {
        snprintf(mountedPath, sizeof(mountedPath), "%s", path);
        snprintf(lastMountPath, sizeof(lastMountPath), "%s", path);
        mountKnown = true;
    }
}

// Take the child's output: log whole lines, and keep a dialog's last line as its answer.
static void
takeChildOutput(const char *textP)
{
size_t i;

    for( i = 0; textP[i]; i++ )
    {
        if( (textP[i] != '\n') && (childPartialLen < (sizeof(childPartial) - 1)) )
        {
            childPartial[childPartialLen++] = textP[i];
            continue;
        }

        if( textP[i] != '\n' )
        {
            continue;               // an overlong line is cut
        }

        childPartial[childPartialLen] = '\0';
        if( (job == JOB_ASK_MOUNT) || (job == JOB_ASK_PUNCH) )
        {
            // Tk can warn on the same pipe; the chosen file is the line that is a path.
            if( childPartial[0] == '/' )
            {
                snprintf(dialogPath, sizeof(dialogPath), "%s", childPartial);
            }
            else if( childPartialLen > 0 )
            {
                appLog("dialog: %s", childPartial);
            }
        }
        else if( childPartialLen > 0 )
        {
            appLog("  %s", childPartial);
        }

        childPartialLen = 0;
    }
}

// Poll the running child, and act on it when it ends.
static void
pollChild(void)
{
char out[4096];
char line[PATH_MAX + 4];
int result;
Job ended;

    if( job == JOB_NONE )
    {
        startQueued();
        return;
    }

    result = childPoll(out, sizeof(out));
    takeChildOutput(out);
    if( result == 1 )
    {
        return;
    }

    if( childPartialLen > 0 )
    {
        takeChildOutput("\n");
    }

    ended = job;
    job = JOB_NONE;
    statusMs = 0;               // refresh the status at once
    if( ended == JOB_SCRIPT )
    {
        appLog("%s finished, status %d", jobLabel, childStatus());
    }
    else if( !dialogPath[0] && (childStatus() != 0) )
    {
        // Cancel exits 0; a missing Tk (python3-tk, not in every desktop install) exits 1.
        appError("the file dialog failed, status %d, see the Log tab; it needs python3-tk", childStatus());
    }
    else if( !dialogPath[0] )
    {
        appLog("%s: canceled", jobLabel);
    }
    else if( ended == JOB_ASK_MOUNT )
    {
        mountTape(dialogPath);
    }
    else if( tapePathOk(dialogPath) )
    {
        snprintf(line, sizeof(line), "p %s", dialogPath);
        if( sendTape(line) )
        {
            snprintf(punchPath, sizeof(punchPath), "%s", dialogPath);
        }
    }

    startQueued();
}

// Refresh process and port status, and notice pidp1.config, pdp1control.config or
// pdp1central.config changing on disk. Called once a second.
static void
refreshStatus(struct nk_context *ctxP)
{
ConfFile *newP;
const char *panelP, *interfaceP;
char panelBuf[CONF_MAX_VALUE + 1];
char interfaceBuf[CONF_MAX_VALUE + 1];

    pdp1Count = procCount("pdp1");
    panelP = controlChoice("frontpanel", "virtual", panelBuf);
    panelCount = procCount(!strcmp(panelP, "pidp") ? "panel_pidp1" : "vpanel_pdp1");
    interfaceP = controlChoice("interface", "web", interfaceBuf);
    if( !strcmp(interfaceP, "gui") )
    {
        frontCount = procCount("pdp1_periphES");
    }
    else if( !strcmp(interfaceP, "apps") )
    {
        frontCount = procCount("t30dpy");
    }
    else
    {
        frontCount = procCount("pdpsrv");
    }
    usbCount = procCount("pdp1_usb_monitor");
    portUp = portAnswers(EMU_PORT);

    if( controlCfP && confChangedOnDisk(controlCfP) && (newP = confLoad(controlPath)) )
    {
        confFree(controlCfP);
        controlCfP = newP;
    }

    if( styleCfP && confChangedOnDisk(styleCfP) )
    {
        appLog("%s changed on disk; colors reloaded", stylePath);
        loadStyle(ctxP);
    }

    if( diskCfP && confChangedOnDisk(diskCfP) && !diskConflict )
    {
        if( countChanges().total == 0 )
        {
            appLog("%s changed on disk; reloaded", configPath);
            loadConfig();
        }
        else
        {
            diskConflict = true;
        }
    }
}

// Leave pdp1central, leaving the emulator and everything it started running. A running script
// writes to our pipe, and would die of SIGPIPE at its next line once we are gone, cutting a start
// or stop short; so the first request waits for the child and the queue, and a second one leaves
// at once.
static void
requestExit(void)
{
    if( ((job == JOB_NONE) && (queueCount == 0)) || exitPending )
    {
        exitNow = true;
        return;
    }

    exitPending = true;
    appLog("exit: waiting for %s to finish", ((job != JOB_NONE) ? jobLabel : "the queued commands"));
}

// Draw a status lamp and its label beside it.
static void
lamp(struct nk_context *ctxP, const char *labelP, bool on)
{
struct nk_rect bounds;
struct nk_command_buffer *canvasP;
struct nk_rect dot;

    canvasP = nk_window_get_canvas(ctxP);
    bounds = nk_widget_bounds(ctxP);
    nk_labelf(ctxP, NK_TEXT_LEFT, "    %s", labelP);
    dot = nk_rect((bounds.x + 4), (bounds.y + ((bounds.h - 14) / 2)), 14, 14);
    nk_fill_circle(canvasP, dot, (on ? styleColors[STYLE_LAMP_ON] : styleColors[STYLE_LAMP_OFF]));
}

// A button that is drawn disabled when it cannot be used. Returns true when clicked.
static bool
button(struct nk_context *ctxP, const char *labelP, bool enabled)
{
bool clicked;

    if( !enabled )
    {
        styleDisableBegin(ctxP);
    }

    clicked = nk_button_label(ctxP, labelP);
    if( !enabled )
    {
        styleDisableEnd(ctxP);
        clicked = false;
    }

    return(clicked);
}

// The strip across the top: what is running, and the run controls.
static void
drawStatus(struct nk_context *ctxP)
{
static const float stripRatios[] = { 0.14f, 0.14f, 0.14f, 0.14f, 0.14f, 0.30f };
const char *panelP, *interfaceP;
char panelBuf[CONF_MAX_VALUE + 1];
char interfaceBuf[CONF_MAX_VALUE + 1];
char usbBuf[CONF_MAX_VALUE + 1];
char frontLabel[64];
bool usb, idle, running;

    panelP = controlChoice("frontpanel", "virtual", panelBuf);
    interfaceP = controlChoice("interface", "web", interfaceBuf);
    usb = !strcmp(controlChoice("usbtape", "n", usbBuf), "y");
    snprintf(frontLabel, sizeof(frontLabel), "%s",
        (!strcmp(interfaceP, "gui") ? "pdp1_periphES" : (!strcmp(interfaceP, "apps") ? "t30dpy" : "pdpsrv")));

    nk_layout_row_dynamic(ctxP, ROW_H, (usb ? 5 : 4));
    lamp(ctxP, "pdp1", (pdp1Count > 0));
    lamp(ctxP, (!strcmp(panelP, "pidp") ? "panel_pidp1" : "vpanel_pdp1"), (panelCount > 0));
    lamp(ctxP, frontLabel, (frontCount > 0));
    if( usb )
    {
        lamp(ctxP, "USB tape monitor", (usbCount > 0));
    }
    lamp(ctxP, "port 1040", portUp);

    idle = ((job == JOB_NONE) && (queueCount == 0));
    running = (pdp1Count > 0);
    // Five buttons, and the rest of the row for the running job and its seconds.
    nk_layout_row(ctxP, NK_DYNAMIC, ROW_H, 6, stripRatios);
    if( button(ctxP, "Start", (idle && !running)) )
    {
        queueScript("start");
    }
    if( button(ctxP, "Stop", (idle && running)) )
    {
        queueScript("stop");
    }
    if( button(ctxP, "Restart", idle) )
    {
        queueScript("restart");
    }
    if( button(ctxP, "Reload config", (idle && running)) )
    {
        queueScript("reload");
    }
    if( button(ctxP, "Exit", true) )
    {
        requestExit();
    }

    if( job != JOB_NONE )
    {
        nk_labelf(ctxP, NK_TEXT_LEFT, "%s, %u s", jobLabel, ((SDL_GetTicks() - jobStartMs) / 1000));
    }
    else
    {
        nk_label(ctxP, "", NK_TEXT_LEFT);
    }

    if( exitPending )
    {
        nk_layout_row_dynamic(ctxP, ROW_H, 1);
        nk_label_colored(ctxP, "Exiting when it finishes; Exit again to leave now", NK_TEXT_LEFT,
            styleColors[STYLE_WARNING]);
    }
    else if( statusNote[0] )
    {
        nk_layout_row_dynamic(ctxP, ROW_H, 1);
        nk_label_colored(ctxP, statusNote, NK_TEXT_LEFT, styleColors[STYLE_WARNING]);
    }
}

// Write one start-time choice to pdp1control.config at once.
static void
setControlChoice(const char *nameP, const char *valueP)
{
ConfFile *newP;

    if( !controlCfP || confChangedOnDisk(controlCfP) )
    {
        if( !(newP = confLoad(controlPath)) )
        {
            appError("cannot read %s", controlPath);
            return;
        }
        confFree(controlCfP);
        controlCfP = newP;
    }

    if( !confSet(controlCfP, nameP, valueP) || !confSave(controlCfP) )
    {
        appError("%s: %s", controlPath, controlCfP->errorText);
        return;
    }

    appLog("%s: %s=%s, used at the next start", controlPath, nameP, valueP);
}

// The Control tab: start-time choices and paper tape.
static void
drawControl(struct nk_context *ctxP)
{
static const char *interfaces[] = { "gui", "web", "apps" };
static const char *panels[] = { "pidp", "virtual" };
static const char *panelLabels[] = { "PiDP-1 hardware", "virtual" };
static const char *usbs[] = { "y", "n" };
static const char *usbLabels[] = { "yes", "no" };
float ratios[MAX_SCHEMES + 1];
char valueBuf[CONF_MAX_VALUE + 1];
bool idle;
int i, count;

    idle = ((job == JOB_NONE) && (queueCount == 0));

    nk_layout_row_dynamic(ctxP, ROW_H, 1);
    nk_label(ctxP, "Start-time choices (used at the next start)", NK_TEXT_LEFT);

    // Each is a set of radio buttons; one that becomes selected is written at once.
    controlChoice("interface", "web", valueBuf);
    nk_layout_row_dynamic(ctxP, ROW_H, 4);
    nk_label(ctxP, "Interface", NK_TEXT_LEFT);
    for( i = 0; i < 3; i++ )
    {
        if( nk_option_label(ctxP, interfaces[i], !strcmp(valueBuf, interfaces[i])) && strcmp(valueBuf, interfaces[i]) )
        {
            setControlChoice("interface", interfaces[i]);
        }
    }

    controlChoice("frontpanel", "virtual", valueBuf);
    nk_layout_row_dynamic(ctxP, ROW_H, 4);
    nk_label(ctxP, "Front panel", NK_TEXT_LEFT);
    for( i = 0; i < 2; i++ )
    {
        if( nk_option_label(ctxP, panelLabels[i], !strcmp(valueBuf, panels[i])) && strcmp(valueBuf, panels[i]) )
        {
            setControlChoice("frontpanel", panels[i]);
        }
    }
    nk_label(ctxP, "", NK_TEXT_LEFT);

    controlChoice("usbtape", "n", valueBuf);
    nk_layout_row_dynamic(ctxP, ROW_H, 4);
    nk_label(ctxP, "USB paper tape", NK_TEXT_LEFT);
    for( i = 0; i < 2; i++ )
    {
        if( nk_option_label(ctxP, usbLabels[i], !strcmp(valueBuf, usbs[i])) && strcmp(valueBuf, usbs[i]) )
        {
            setControlChoice("usbtape", usbs[i]);
        }
    }
    nk_label(ctxP, "", NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctxP, 12, 1);
    nk_spacing(ctxP, 1);
    nk_layout_row_dynamic(ctxP, ROW_H, 1);
    nk_label(ctxP, "Paper tape reader", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctxP, ROW_H, 3);
    if( button(ctxP, "Mount...", idle) )
    {
        startDialog(JOB_ASK_MOUNT);
    }
    if( button(ctxP, "Remount last", (idle && lastMountPath[0])) )
    {
        mountTape(lastMountPath);
    }
    if( button(ctxP, "Unmount", idle) && sendTape("r") )
    {
        mountedPath[0] = '\0';
        mountKnown = true;
    }
    nk_layout_row_dynamic(ctxP, ROW_H, 1);
    if( !mountKnown )
    {
        nk_label(ctxP, "Mounted: not known (the emulator cannot be asked)", NK_TEXT_LEFT);
    }
    else if( mountedPath[0] )
    {
        nk_labelf(ctxP, NK_TEXT_LEFT, "Mounted by pdp1central: %s", mountedPath);
    }
    else
    {
        nk_label(ctxP, "Unmounted by pdp1central", NK_TEXT_LEFT);
    }

    nk_layout_row_dynamic(ctxP, 12, 1);
    nk_spacing(ctxP, 1);
    nk_layout_row_dynamic(ctxP, ROW_H, 1);
    nk_label(ctxP, "Paper tape punch", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctxP, ROW_H, 3);
    if( button(ctxP, "Save punch to...", idle) )
    {
        startDialog(JOB_ASK_PUNCH);
    }
    nk_label(ctxP, "", NK_TEXT_LEFT);
    nk_label(ctxP, "", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctxP, ROW_H, 1);
    nk_labelf(ctxP, NK_TEXT_LEFT, "Punch file: %s", (punchPath[0] ? punchPath : "not set by pdp1central"));
    if( !strcmp(controlChoice("interface", "web", valueBuf), "web") )
    {
        nk_label_colored(ctxP, "Note: the web interface's front end does not save the punch to a file.",
            NK_TEXT_LEFT, styleColors[STYLE_WARNING]);
    }

    nk_layout_row_dynamic(ctxP, 12, 1);
    nk_spacing(ctxP, 1);
    nk_layout_row_dynamic(ctxP, ROW_H, 1);
    nk_label(ctxP, "Window", NK_TEXT_LEFT);
    // Option buttons, not a drop-down: at the foot of the tab a drop-down's list would run off
    // the bottom of the window.
    for( count = 0; (count < MAX_SCHEMES) && styleSchemeName(count); count++ )
    {
    }
    ratios[0] = 0.25f;
    for( i = 1; i <= count; i++ )
    {
        ratios[i] = (0.75f / count);
    }
    nk_layout_row(ctxP, NK_DYNAMIC, ROW_H, (count + 1), ratios);
    nk_label(ctxP, "Colors", NK_TEXT_LEFT);
    for( i = 0; i < count; i++ )
    {
        if( nk_option_label(ctxP, styleSchemeName(i), (i == styleCurrent())) && (i != styleCurrent()) )
        {
            // Applied before the next frame is drawn, not while this one is half drawn.
            snprintf(pendingScheme, sizeof(pendingScheme), "%s", styleSchemeName(i));
        }
    }
    if( styleOverrides > 0 )
    {
        nk_layout_row_dynamic(ctxP, ROW_H, 1);
        nk_labelf(ctxP, NK_TEXT_LEFT, "Changed by %d color line%s in pdp1central.config", styleOverrides,
            ((styleOverrides == 1) ? "" : "s"));
    }
}

// Format a float so the parser keeps it as one: always with a '.', trailing zeros trimmed.
static void
formatFloat(double value, char *outP, size_t outLen)
{
char *endP;

    snprintf(outP, outLen, "%.4f", value);
    endP = (outP + strlen(outP) - 1);
    while( (*endP == '0') && (*(endP - 1) != '.') )
    {
        *endP-- = '\0';
    }
}

// A starting value for a setting whose "default" box is being unchecked.
static void
startingValue(const SchemaEntry *entryP, char *outP, size_t outLen)
{
    if( strcmp(entryP->defaultText, "-") )
    {
        snprintf(outP, outLen, "%s", entryP->defaultText);
        if( (entryP->type == SCHEMA_BOOL) )
        {
            snprintf(outP, outLen, "%s", (schemaIsOn(entryP->defaultText) ? "on" : "off"));
        }
        else if( (entryP->type == SCHEMA_FLOAT) && !strchr(outP, '.') )
        {
            formatFloat(atof(entryP->defaultText), outP, outLen);
        }
        return;
    }

    switch( entryP->type )
    {
    case SCHEMA_BOOL:
        snprintf(outP, outLen, "off");
        break;
    case SCHEMA_FLOAT:
        formatFloat((entryP->hasRange ? entryP->rangeLow : 1.0), outP, outLen);
        break;
    case SCHEMA_TEXT:
        snprintf(outP, outLen, "0");
        break;
    default:
        snprintf(outP, outLen, "%d", (entryP->hasRange ? (int)entryP->rangeLow : 0));
        break;
    }
}

// Set a value in the edited file after checking it against the schema.
static void
editSet(const SchemaEntry *entryP, const char *nameP, const char *valueP)
{
char error[256];

    if( entryP && !schemaCheck(entryP, valueP, error, sizeof(error)) )
    {
        appError("%s", error);
        return;
    }

    if( !confSet(editCfP, nameP, valueP) )
    {
        appError("%s: %s", nameP, editCfP->errorText);
    }
}

// Whether text is a whole number, or a number with at most one '.', for the property widgets.
static bool
parsesAs(const char *textP, bool integerOnly)
{
char *endP;

    if( !*textP )
    {
        return(false);
    }

    if( integerOnly )
    {
        strtol(textP, &endP, 10);
    }
    else
    {
        strtod(textP, &endP);
    }

    return(*endP == '\0');
}

// A text field for a value; the value is set when Enter is pressed or the field is left.
static void
valueField(struct nk_context *ctxP, int index, const SchemaEntry *entryP, const char *nameP, const char *valueP)
{
nk_flags flags;

    if( index != activeEdit )
    {
        snprintf(editBufsP[index], (CONF_MAX_VALUE + 1), "%s", (valueP ? valueP : ""));
    }

    flags = nk_edit_string_zero_terminated(ctxP, (NK_EDIT_FIELD | NK_EDIT_SIG_ENTER), editBufsP[index],
        (CONF_MAX_VALUE + 1), nk_filter_default);
    if( flags & NK_EDIT_ACTIVE )
    {
        activeEdit = index;
    }

    if( (flags & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED)) && (index == activeEdit) )
    {
        activeEdit = -1;
        if( valuesDiffer(valueP, editBufsP[index]) && editBufsP[index][0] )
        {
            editSet(entryP, nameP, editBufsP[index]);
        }
    }
}

// One row of the settings editor: name, control, "default" box, when it applies.
static void
drawSettingRow(struct nk_context *ctxP, int index, const SchemaEntry *entryP, const char *nameP)
{
static const float ratios[] = { 0.24f, 0.36f, 0.12f, 0.28f };
char valueBuf[CONF_MAX_VALUE + 1];
char newValue[CONF_MAX_VALUE + 1];
char label[CONF_MAX_VALUE + 2];
struct nk_rect bounds;
const char *valueP;
nk_bool isDefault, wasDefault, on;
bool readOnly;
int intValue, newInt;
double floatValue, newFloat;

    valueP = getValue(editCfP, nameP, valueBuf);
    readOnly = (entryP && (entryP->applies == APPLIES_NONE));
    nk_layout_row(ctxP, NK_DYNAMIC, ROW_H, 4, ratios);

    // The name, with the summary as its tooltip.
    bounds = nk_widget_bounds(ctxP);
    if( valuesDiffer(getValue(diskCfP, nameP, newValue), valueP) )
    {
        nk_labelf_colored(ctxP, NK_TEXT_LEFT, styleColors[STYLE_CHANGED], "%s *", nameP);
    }
    else
    {
        nk_label(ctxP, nameP, NK_TEXT_LEFT);
    }
    if( entryP && nk_input_is_mouse_hovering_rect(&ctxP->input, bounds) )
    {
        nk_tooltip(ctxP, entryP->summary);
    }

    // The control, for a value the file sets.
    snprintf(label, sizeof(label), "#%s", nameP);
    if( !valueP )
    {
        nk_labelf(ctxP, NK_TEXT_LEFT, "default: %s", ((entryP && strcmp(entryP->defaultText, "-")) ?
            entryP->defaultText : "built-in"));
    }
    else if( readOnly )
    {
        nk_label(ctxP, valueP, NK_TEXT_LEFT);
    }
    else if( !entryP || (entryP->type == SCHEMA_TEXT) )
    {
        valueField(ctxP, (entryP ? (int)(entryP - schemaP->entriesP) : index), entryP, nameP, valueP);
    }
    else if( entryP->type == SCHEMA_BOOL )
    {
        on = schemaIsOn(valueP);
        if( nk_checkbox_label(ctxP, (on ? "on" : "off"), &on) )
        {
            editSet(entryP, nameP, (on ? "on" : "off"));
        }
    }
    else if( ((entryP->type == SCHEMA_INT) || ((entryP->type == SCHEMA_PORT) && strcmp(valueP, "off"))) &&
        parsesAs(valueP, true) )
    {
        intValue = atoi(valueP);
        newInt = nk_propertyi(ctxP, label, (entryP->hasRange ? (int)entryP->rangeLow : -1000000000), intValue,
            (entryP->hasRange ? (int)entryP->rangeHigh : 1000000000), 1, 1.0f);
        if( newInt != intValue )
        {
            snprintf(newValue, sizeof(newValue), "%d", newInt);
            editSet(entryP, nameP, newValue);
        }
    }
    else if( (entryP->type == SCHEMA_FLOAT) && parsesAs(valueP, false) )
    {
        floatValue = atof(valueP);
        newFloat = nk_propertyd(ctxP, label, (entryP->hasRange ? entryP->rangeLow : -1e9), floatValue,
            (entryP->hasRange ? entryP->rangeHigh : 1e9), 0.01, 0.005f);
        if( newFloat != floatValue )
        {
            formatFloat(newFloat, newValue, sizeof(newValue));
            editSet(entryP, nameP, newValue);
        }
    }
    else
    {
        // "off" for a port, or a value that is not a number: edit it as text.
        valueField(ctxP, (int)(entryP - schemaP->entriesP), entryP, nameP, valueP);
    }

    // The "default" box: checked means no active line, so the reader uses its default.
    isDefault = (valueP == NULL);
    wasDefault = isDefault;
    if( readOnly )
    {
        styleDisableBegin(ctxP);
    }
    nk_checkbox_label(ctxP, "default", &isDefault);
    if( readOnly )
    {
        styleDisableEnd(ctxP);
    }
    else if( isDefault && !wasDefault )
    {
        if( !confUnset(editCfP, nameP) )
        {
            appError("%s: %s", nameP, editCfP->errorText);
        }
    }
    else if( !isDefault && wasDefault && entryP )
    {
        startingValue(entryP, newValue, sizeof(newValue));
        editSet(entryP, nameP, newValue);
    }

    if( !entryP )
    {
        nk_label(ctxP, "not in the schema", NK_TEXT_LEFT);
    }
    else
    {
        nk_labelf(ctxP, NK_TEXT_LEFT, "%s%s", schemaAppliesText(entryP->applies),
            (entryP->sticky ? "; default: restart" : ""));
    }
}

// Save pidp1.config, then reload what is running as far as the changes allow.
static void
saveSettings(void)
{
Changes changes;
char names[400];
char diskBuf[CONF_MAX_VALUE + 1];
char editBuf[CONF_MAX_VALUE + 1];
const SchemaEntry *entryP;
size_t len;
int i;

    changes = countChanges();
    if( !confSave(editCfP) )
    {
        appError("not saved: %s", editCfP->errorText);
        return;
    }

    appLog("saved %s, %d change%s", configPath, changes.total, ((changes.total == 1) ? "" : "s"));

    // Name the changes a reload does not apply.
    names[0] = '\0';
    len = 0;
    for( i = 0; i < schemaP->count; i++ )
    {
        entryP = &schemaP->entriesP[i];
        if( valuesDiffer(getValue(diskCfP, entryP->name, diskBuf), getValue(editCfP, entryP->name, editBuf)) &&
            ((entryP->applies == APPLIES_RESTART) || (entryP->applies == APPLIES_PROGRAM) ||
            (entryP->sticky && !confGet(editCfP, entryP->name))) && (len < (sizeof(names) - 80)) )
        {
            len += snprintf(names + len, (sizeof(names) - len), "%s%s%s", (len ? ", " : ""), entryP->name,
                ((entryP->applies == APPLIES_PROGRAM) ? " (restart its program)" : ""));
        }
    }

    savedNeedsRestart = ((changes.restart > 0) && (pdp1Count > 0));
    if( names[0] || (changes.restart > 0) )
    {
        snprintf(savedNote, sizeof(savedNote), "Saved. Not applied until a restart: %s%s", names,
            (otherCount && (changes.restart > 0) && !names[0]) ? "settings not in the schema" : "");
    }
    else
    {
        savedNote[0] = '\0';
    }

    confFree(diskCfP);
    diskCfP = confCopy(editCfP);
    diskConflict = false;

    if( (changes.reload > 0) && (pdp1Count > 0) )
    {
        queueScript("reload");
    }
    if( (changes.panel > 0) && (procCount("panel_pidp1") > 0) )
    {
        queueScript("reloadpanel");
    }
}

// The Settings tab: a sub-tab per group, a row per setting, and Save/Revert with a count.
static void
drawSettings(struct nk_context *ctxP, float height)
{
static const float footerRatios[] = { 0.18f, 0.18f, 0.64f };
ConfFile *newP;
Changes changes;
const SchemaEntry *entryP;
const char *sep;
char count[160];
size_t len;
bool idle;
int i;

    idle = ((job == JOB_NONE) && (queueCount == 0));

    if( !diskCfP->existed && (editCfP->lineCount == 0) )
    {
        nk_layout_row_dynamic(ctxP, ROW_H, 1);
        nk_labelf(ctxP, NK_TEXT_LEFT, "There is no %s.", configPath);
        nk_layout_row_dynamic(ctxP, ROW_H, 3);
        if( nk_button_label(ctxP, "Start from pidp1.config.example") )
        {
            if( (newP = confLoad(examplePath)) && confRetarget(newP, configPath) )
            {
                confFree(editCfP);
                editCfP = newP;
                refreshOtherNames();
            }
            else
            {
                confFree(newP);
                appError("cannot read %s", examplePath);
            }
        }
        return;
    }

    if( diskConflict )
    {
        nk_layout_row_dynamic(ctxP, ROW_H, 3);
        nk_label_colored(ctxP, "pidp1.config changed on disk.", NK_TEXT_LEFT, styleColors[STYLE_WARNING]);
        if( nk_button_label(ctxP, "Reload it, drop my edits") )
        {
            loadConfig();
        }
        if( nk_button_label(ctxP, "Keep my edits") && (newP = confLoad(configPath)) )
        {
            // The new file on disk becomes what the edits are counted against.
            confFree(diskCfP);
            diskCfP = newP;
            diskConflict = false;
        }
    }

    // The group sub-tabs.
    nk_layout_row_dynamic(ctxP, ROW_H, GROUP_COUNT);
    for( i = 0; i < GROUP_COUNT; i++ )
    {
        if( nk_select_label(ctxP, groupNames[i], NK_TEXT_CENTERED, (group == i)) )
        {
            group = i;
        }
    }

    // The rows, scrolling.
    // Everything below the sub-tabs but the footer rows.
    nk_layout_row_dynamic(ctxP, (height - (2 * (ROW_H + 4)) - 8 - (diskConflict ? (ROW_H + 4) : 0) -
        (savedNote[0] ? (ROW_H + 4) : 0)), 1);
    if( nk_group_begin(ctxP, "settingrows", NK_WINDOW_BORDER) )
    {
        if( group == GROUP_OTHER )
        {
            if( otherCount == 0 )
            {
                nk_layout_row_dynamic(ctxP, ROW_H, 1);
                nk_label(ctxP, "pidp1.config sets nothing the schema does not describe.", NK_TEXT_LEFT);
            }
            for( i = 0; i < otherCount; i++ )
            {
                drawSettingRow(ctxP, (schemaP->count + i), NULL, otherNames[i]);
            }
        }
        else
        {
            for( i = 0; i < schemaP->count; i++ )
            {
                entryP = &schemaP->entriesP[i];
                if( !strcmp(entryP->group, groupNames[group]) )
                {
                    drawSettingRow(ctxP, i, entryP, entryP->name);
                }
            }
        }
        nk_group_end(ctxP);
    }

    // Save, Revert and the count.
    changes = countChanges();
    nk_layout_row(ctxP, NK_DYNAMIC, ROW_H, 3, footerRatios);
    if( button(ctxP, "Save", ((changes.total > 0) && idle)) )
    {
        saveSettings();
    }
    if( button(ctxP, "Revert", (changes.total > 0)) )
    {
        confFree(editCfP);
        editCfP = confCopy(diskCfP);
        refreshOtherNames();
    }
    // "3 changes: 2 on reload, 1 needs a restart", naming only the kinds there are.
    len = (size_t)snprintf(count, sizeof(count), "%d change%s", changes.total, ((changes.total == 1) ? "" : "s"));
    sep = ": ";
    if( changes.reload > 0 )
    {
        len += snprintf(count + len, (sizeof(count) - len), "%s%d on reload", sep, changes.reload);
        sep = ", ";
    }
    if( changes.panel > 0 )
    {
        len += snprintf(count + len, (sizeof(count) - len), "%s%d on panel reload", sep, changes.panel);
        sep = ", ";
    }
    if( (changes.restart + changes.program) > 0 )
    {
        len += snprintf(count + len, (sizeof(count) - len), "%s%d need%s a restart", sep,
            (changes.restart + changes.program), (((changes.restart + changes.program) == 1) ? "s" : ""));
        sep = ", ";
    }
    if( changes.ignored > 0 )
    {
        snprintf(count + len, (sizeof(count) - len), "%s%d ignored", sep, changes.ignored);
    }
    nk_label(ctxP, ((changes.total == 0) ? "no changes" : count), NK_TEXT_LEFT);

    if( savedNote[0] )
    {
        nk_layout_row_dynamic(ctxP, ROW_H, (savedNeedsRestart ? 3 : 2));
        nk_label_colored(ctxP, savedNote, NK_TEXT_LEFT, styleColors[STYLE_CHANGED]);
        if( savedNeedsRestart && button(ctxP, "Restart now", idle) )
        {
            queueScript("restart");
            savedNeedsRestart = false;
        }
        if( nk_button_label(ctxP, "Dismiss") )
        {
            savedNote[0] = '\0';
        }
    }
}

// The Log tab: script output, port replies and errors, newest last.
static void
drawLog(struct nk_context *ctxP, float height)
{
int i;

    nk_layout_row_dynamic(ctxP, (height - (ROW_H + 8)), 1);
    if( nk_group_begin(ctxP, "logrows", NK_WINDOW_BORDER) )
    {
        nk_layout_row_dynamic(ctxP, 18, 1);
        for( i = 0; i < logCount; i++ )
        {
            nk_label(ctxP, logLinesP[(logFirst + i) % LOG_LINES], NK_TEXT_LEFT);
        }
        nk_group_end(ctxP);
    }
}

// Draw the whole window for one frame.
static void
drawFrame(struct nk_context *ctxP, int width, int height)
{
struct nk_rect region;
float used;

    if( nk_begin(ctxP, "pdp1central", nk_rect(0, 0, width, height), NK_WINDOW_NO_SCROLLBAR) )
    {
        drawStatus(ctxP);

        nk_layout_row_dynamic(ctxP, ROW_H, 3);
        if( nk_select_label(ctxP, "Control", NK_TEXT_CENTERED, (tab == TAB_CONTROL)) )
        {
            tab = TAB_CONTROL;
        }
        if( nk_select_label(ctxP, "Settings", NK_TEXT_CENTERED, (tab == TAB_SETTINGS)) )
        {
            tab = TAB_SETTINGS;
        }
        if( nk_select_label(ctxP, "Log", NK_TEXT_CENTERED, (tab == TAB_LOG)) )
        {
            tab = TAB_LOG;
        }

        region = nk_window_get_content_region(ctxP);
        used = (nk_widget_bounds(ctxP).y - region.y);
        if( tab == TAB_CONTROL )
        {
            drawControl(ctxP);
        }
        else if( tab == TAB_SETTINGS )
        {
            drawSettings(ctxP, (region.h - used));
        }
        else
        {
            drawLog(ctxP, (region.h - used));
        }
    }
    nk_end(ctxP);
}

// Set every path from the root, and the schema's default location.
// Returns false if the root is too long for them.
static bool
setPaths(const char *rootP, const char *schemaArgP)
{
bool ok;

    if( strlen(rootP) >= (PATH_MAX - 64) )
    {
        return(false);
    }

    strcpy(rootPath, rootP);
    ok = (rootJoin(configPath, "pidp1.config") && rootJoin(examplePath, "pidp1.config.example") &&
        rootJoin(controlPath, "pdp1control.config") && rootJoin(scriptPath, "bin/pdp1control.sh") &&
        rootJoin(askOpenPath, "bin/tkaskopenfile") && rootJoin(askSavePath, "bin/tkaskopenfilewrite") &&
        rootJoin(fontPath, "src/pdp1_periph/DejaVuSansMono.ttf") && rootJoin(stylePath, "pdp1central.config"));
    if( schemaArgP )
    {
        ok = (ok && (snprintf(schemaPath, sizeof(schemaPath), "%s", schemaArgP) < (int)sizeof(schemaPath)));
    }
    else
    {
        ok = (ok && rootJoin(schemaPath, "src/pdp1central/pidp1config.schema"));
    }

    return(ok);
}

int
main(int argc, char **argv)
{
struct nk_context *ctxP;
struct nk_font_atlas *atlasP;
struct nk_font *fontP;
SDL_Window *windowP;
SDL_Renderer *rendererP;
SDL_Event event;
const char *rootArgP, *schemaArgP;
char error[512];
struct stat st;
int opt, width, height;

    rootArgP = DEFAULT_ROOT;
    schemaArgP = NULL;
    while( (opt = getopt(argc, argv, "r:s:")) != -1 )
    {
        if( opt == 'r' )
        {
            rootArgP = optarg;
        }
        else if( opt == 's' )
        {
            schemaArgP = optarg;
        }
        else
        {
            fprintf(stderr, "usage: pdp1central [-r root] [-s schema]\n");
            return(1);
        }
    }

    if( !setPaths(rootArgP, schemaArgP) )
    {
        fprintf(stderr, "pdp1central: the root or schema path is too long\n");
        return(1);
    }

    // The script and the dialogs find the same root through PIDP1_ROOT.
    if( strcmp(rootPath, DEFAULT_ROOT) )
    {
        setenv("PIDP1_ROOT", rootPath, 1);
    }

    if( !(schemaP = schemaLoad(schemaPath, error, sizeof(error))) )
    {
        fprintf(stderr, "pdp1central: %s\n", error);
        return(1);
    }

    loadConfig();
    if( !diskCfP )
    {
        fprintf(stderr, "pdp1central: cannot read %s\n", configPath);
        return(1);
    }
    controlCfP = confLoad(controlPath);

    if( SDL_Init(SDL_INIT_VIDEO) )
    {
        fprintf(stderr, "pdp1central: SDL_Init: %s\n", SDL_GetError());
        return(1);
    }

    if( !(windowP = SDL_CreateWindow("pdp1central", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_W,
        WINDOW_H, (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE))) )
    {
        fprintf(stderr, "pdp1central: SDL_CreateWindow: %s\n", SDL_GetError());
        return(1);
    }

    if( !(rendererP = SDL_CreateRenderer(windowP, -1, (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC))) &&
        !(rendererP = SDL_CreateRenderer(windowP, -1, SDL_RENDERER_SOFTWARE)) )
    {
        fprintf(stderr, "pdp1central: SDL_CreateRenderer: %s\n", SDL_GetError());
        return(1);
    }

    // The font the peripherals window uses, or Nuklear's own when it is not there.
    ctxP = nk_sdl_init(windowP, rendererP);
    nk_sdl_font_stash_begin(&atlasP);
    fontP = (!stat(fontPath, &st) ? nk_font_atlas_add_from_file(atlasP, fontPath, 16, NULL) : NULL);
    if( !fontP )
    {
        fontP = nk_font_atlas_add_default(atlasP, 15, NULL);
    }
    nk_sdl_font_stash_end();
    nk_style_set_font(ctxP, &fontP->handle);

    styleUse(ctxP, STYLE_DEFAULT);

    appLog("pdp1central started; root %s", rootPath);
    loadStyle(ctxP);
    exitNow = false;
    while( !exitNow )
    {
        nk_input_begin(ctxP);
        if( SDL_WaitEventTimeout(&event, 250) )
        {
            do
            {
                if( event.type == SDL_QUIT )
                {
                    requestExit();
                }
                nk_sdl_handle_event(&event);
            } while( SDL_PollEvent(&event) );
        }
        nk_sdl_handle_grab();
        nk_input_end(ctxP);

        pollChild();
        if( exitPending && (job == JOB_NONE) && (queueCount == 0) )
        {
            exitNow = true;
        }
        if( (statusMs == 0) || ((SDL_GetTicks() - statusMs) >= 1000) )
        {
            refreshStatus(ctxP);
            statusMs = SDL_GetTicks();
        }

        if( pendingScheme[0] )
        {
            setScheme(ctxP, pendingScheme);
            pendingScheme[0] = '\0';
        }

        SDL_GetWindowSize(windowP, &width, &height);
        drawFrame(ctxP, width, height);

        SDL_SetRenderDrawColor(rendererP, styleColors[NK_COLOR_WINDOW].r, styleColors[NK_COLOR_WINDOW].g,
            styleColors[NK_COLOR_WINDOW].b, 255);
        SDL_RenderClear(rendererP);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        SDL_RenderPresent(rendererP);
    }

    // The emulator and what the script started are in another session and carry on. A child
    // still running here was left by a second Exit, and ends at its next write to our pipe.
    nk_sdl_shutdown();
    SDL_DestroyRenderer(rendererP);
    SDL_DestroyWindow(windowP);
    SDL_Quit();
    return(0);
}
