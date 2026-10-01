// Reads pidp1config.schema, the description of every pidp1.config setting the editor shows, and
// checks a value against it before it is written.
// The file has one setting per line, nine fields separated by '|':
//     name|group|type|range|default|reader|applies|label|summary
// '#' in column 1 starts a comment, and blank lines are skipped. A range is "low..high" or
// empty; a default of "-" means no reader has a single one. Applies is reload, panel, run,
// restart, program or none, and reload, panel or run may carry "-sticky". The label, usually
// empty, is shown instead of the name.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

#define SCHEMA_FIELDS 9

// Copy a field into a fixed buffer, failing if it does not fit.
static bool
copyField(char *toP, size_t toLen, const char *fromP)
{
    if( strlen(fromP) >= toLen )
    {
        return(false);
    }

    strcpy(toP, fromP);
    return(true);
}

// Parse a whole string as a decimal number, integer or not.
// Returns false if anything but the number is there.
static bool
parseNumber(const char *textP, bool integerOnly, double *valueP)
{
char *endP;
size_t i;

    if( !*textP )
    {
        return(false);
    }

    for( i = 0; textP[i]; i++ )
    {
        if( !isdigit((unsigned char)textP[i]) && (textP[i] != '-') && (integerOnly || (textP[i] != '.')) )
        {
            return(false);
        }
    }

    *valueP = strtod(textP, &endP);
    return( (*endP == '\0') && isfinite(*valueP) );
}

// Parse one schema line into entryP.
// Returns NULL on success, or what is wrong with the line.
static const char *
parseLine(char *lineP, SchemaEntry *entryP)
{
char *fieldsP[SCHEMA_FIELDS];
char *p, *dotsP;
int n;

    // Split at '|'; the summary is last, so it may hold anything else.
    fieldsP[0] = lineP;
    for( n = 1, p = lineP; (n < SCHEMA_FIELDS) && (p = strchr(p, '|')); n++ )
    {
        *p++ = '\0';
        fieldsP[n] = p;
    }

    if( n != SCHEMA_FIELDS )
    {
        return("expected 9 fields separated by '|'");
    }

    memset(entryP, 0, sizeof(*entryP));
    if( !copyField(entryP->name, sizeof(entryP->name), fieldsP[0]) || !fieldsP[0][0] ||
        !copyField(entryP->group, sizeof(entryP->group), fieldsP[1]) || !fieldsP[1][0] ||
        !copyField(entryP->defaultText, sizeof(entryP->defaultText), fieldsP[4]) || !fieldsP[4][0] ||
        !copyField(entryP->reader, sizeof(entryP->reader), fieldsP[5]) ||
        !copyField(entryP->label, sizeof(entryP->label), fieldsP[7]) ||
        !copyField(entryP->summary, sizeof(entryP->summary), fieldsP[8]) )
    {
        return("a field is empty or too long");
    }

    if( !strcmp(fieldsP[2], "bool") )
    {
        entryP->type = SCHEMA_BOOL;
    }
    else if( !strcmp(fieldsP[2], "int") )
    {
        entryP->type = SCHEMA_INT;
    }
    else if( !strcmp(fieldsP[2], "float") )
    {
        entryP->type = SCHEMA_FLOAT;
    }
    else if( !strcmp(fieldsP[2], "text") )
    {
        entryP->type = SCHEMA_TEXT;
    }
    else if( !strcmp(fieldsP[2], "port") )
    {
        entryP->type = SCHEMA_PORT;
    }
    else
    {
        return("the type is not bool, int, float, text or port");
    }

    if( fieldsP[3][0] )
    {
        if( !(dotsP = strstr(fieldsP[3], "..")) )
        {
            return("a range is low..high");
        }

        *dotsP = '\0';
        if( !parseNumber(fieldsP[3], false, &entryP->rangeLow) ||
            !parseNumber(dotsP + 2, false, &entryP->rangeHigh) || (entryP->rangeLow > entryP->rangeHigh) )
        {
            return("a range is low..high, two numbers with low <= high");
        }

        entryP->hasRange = true;
    }

    // A "-sticky" setting's reader assigns it only when a line is there, so a new value applies
    // as stated, but going back to the default needs the reader restarted.
    if( (p = strstr(fieldsP[6], "-sticky")) && !p[7] )
    {
        *p = '\0';
        entryP->sticky = true;
    }

    if( !strcmp(fieldsP[6], "reload") )
    {
        entryP->applies = APPLIES_RELOAD;
    }
    else if( !strcmp(fieldsP[6], "panel") )
    {
        entryP->applies = APPLIES_PANEL;
    }
    else if( !strcmp(fieldsP[6], "run") )
    {
        entryP->applies = APPLIES_RUN;
    }
    else if( entryP->sticky )
    {
        return("only reload, panel and run can be -sticky");
    }
    else if( !strcmp(fieldsP[6], "restart") )
    {
        entryP->applies = APPLIES_RESTART;
    }
    else if( !strcmp(fieldsP[6], "program") )
    {
        entryP->applies = APPLIES_PROGRAM;
    }
    else if( !strcmp(fieldsP[6], "none") )
    {
        entryP->applies = APPLIES_NONE;
    }
    else
    {
        return("applies is not reload, panel, run, restart, program or none");
    }

    return(NULL);
}

// Load a schema file.
// Returns NULL on failure, with the file name, line number and reason in errorP.
Schema *
schemaLoad(const char *pathP, char *errorP, size_t errorLen)
{
Schema *schemaP;
SchemaEntry *growP;
FILE *fP;
const char *whyP;
char line[1024];
int lineNo, cap, i;
size_t len;

    if( !(fP = fopen(pathP, "r")) )
    {
        snprintf(errorP, errorLen, "%s: cannot open", pathP);
        return(NULL);
    }

    if( !(schemaP = calloc(1, sizeof(Schema))) )
    {
        fclose(fP);
        snprintf(errorP, errorLen, "out of memory");
        return(NULL);
    }

    cap = 0;
    lineNo = 0;
    while( fgets(line, sizeof(line), fP) )
    {
        lineNo++;
        len = strcspn(line, "\r\n");
        if( (len == (sizeof(line) - 1)) && !strchr(line, '\n') && !feof(fP) )
        {
            whyP = "line too long";
            goto fail;
        }

        line[len] = '\0';
        if( (line[0] == '#') || (line[0] == '\0') )
        {
            continue;
        }

        if( schemaP->count == cap )
        {
            cap = ((cap == 0) ? 64 : (cap * 2));
            if( !(growP = realloc(schemaP->entriesP, (cap * sizeof(SchemaEntry)))) )
            {
                whyP = "out of memory";
                goto fail;
            }
            schemaP->entriesP = growP;
        }

        if( (whyP = parseLine(line, &schemaP->entriesP[schemaP->count])) )
        {
            goto fail;
        }

        for( i = 0; i < schemaP->count; i++ )
        {
            if( !strcmp(schemaP->entriesP[i].name, schemaP->entriesP[schemaP->count].name) )
            {
                whyP = "the name is already in the schema";
                goto fail;
            }
        }

        schemaP->count++;
    }

    fclose(fP);
    return(schemaP);

fail:
    snprintf(errorP, errorLen, "%s line %d: %s", pathP, lineNo, whyP);
    fclose(fP);
    schemaFree(schemaP);
    return(NULL);
}

// Free a loaded schema.
void
schemaFree(Schema *schemaP)
{
    if( schemaP )
    {
        free(schemaP->entriesP);
        free(schemaP);
    }
}

// Look a setting up by name.
// Returns NULL if the schema does not have it.
const SchemaEntry *
schemaFind(const Schema *schemaP, const char *nameP)
{
int i;

    for( i = 0; i < schemaP->count; i++ )
    {
        if( !strcmp(schemaP->entriesP[i].name, nameP) )
        {
            return(&schemaP->entriesP[i]);
        }
    }

    return(NULL);
}

// Whether a value reads as on to the emulator's parser.
bool
schemaIsOn(const char *valueP)
{
    return( !strcmp(valueP, "y") || !strcmp(valueP, "yes") || !strcmp(valueP, "on") ||
        !strcmp(valueP, "true") );
}

// Check a value the editor is about to write against the setting's type and range.
// Booleans must be written as on or off, which every reader accepts, and a float must hold a
// '.', since without one the parser keeps no float for a setting a plugin reads.
// Returns false with the reason in errorP.
bool
schemaCheck(const SchemaEntry *entryP, const char *valueP, char *errorP, size_t errorLen)
{
double value;

    if( !*valueP || (strlen(valueP) > CONF_MAX_VALUE) || strpbrk(valueP, " \t\r\n\v\f") )
    {
        snprintf(errorP, errorLen, "%s: a value is 1 to 63 characters with no spaces", entryP->name);
        return(false);
    }

    // A port is an int, or off.
    if( (entryP->type == SCHEMA_PORT) && !strcmp(valueP, "off") )
    {
        return(true);
    }

    switch( entryP->type )
    {
    case SCHEMA_BOOL:
        if( strcmp(valueP, "on") && strcmp(valueP, "off") )
        {
            snprintf(errorP, errorLen, "%s: on or off", entryP->name);
            return(false);
        }
        return(true);

    case SCHEMA_TEXT:
        return(true);

    case SCHEMA_PORT:
    case SCHEMA_INT:
        if( !parseNumber(valueP, true, &value) )
        {
            snprintf(errorP, errorLen, "%s: a whole number", entryP->name);
            return(false);
        }
        break;

    case SCHEMA_FLOAT:
        if( !parseNumber(valueP, false, &value) || !strchr(valueP, '.') )
        {
            snprintf(errorP, errorLen, "%s: a number with a decimal point, such as 1.0", entryP->name);
            return(false);
        }
        break;
    }

    if( entryP->hasRange && ((value < entryP->rangeLow) || (value > entryP->rangeHigh)) )
    {
        snprintf(errorP, errorLen, "%s: %g to %g", entryP->name, entryP->rangeLow, entryP->rangeHigh);
        return(false);
    }

    return(true);
}

// The words the editor shows for when a change takes effect. Save reloads whatever is running
// that rereads the file, so a reload and a panel reload both apply on save.
const char *
schemaAppliesText(SchemaApplies applies)
{
    switch( applies )
    {
    case APPLIES_RELOAD:
    case APPLIES_PANEL:
        return("on save");
    case APPLIES_RUN:
        return("at the next run after save");
    case APPLIES_RESTART:
        return("needs a restart");
    case APPLIES_PROGRAM:
        return("when its program restarts");
    case APPLIES_NONE:
        break;
    }

    return("ignored");
}

// The name the editor shows: the label, or the name when there is none.
const char *
schemaDisplayName(const SchemaEntry *entryP)
{
    return( entryP->label[0] ? entryP->label : entryP->name );
}
