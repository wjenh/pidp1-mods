// The room after the D/A converters' mixer. Busch Hall: eight delay lines
// in a Householder feedback network, each with three decays (4.1 s at
// 100 Hz, 3.4 s at 1 kHz, 1.9 s at 10 kHz), a 40 ms predelay, and
// Dattorro's plate beside it ("Effect Design, Part 1", J. Audio Eng. Soc.
// 45, 1997). The hall: the same eight lines falling 60 dB in 3 s with a
// gentle loss of highs. For the period, it stands for a studio's echo
// chamber or EMT 140 plate. Implementation.md, "The mixer, the room and
// the audio", describes both.
//
// Included by IOT_71.c (the device) and render/dacrender.c (the
// renderer), so both compute the same integers. Each 50 kHz frame,
// roomFrame() takes the dry left and right in 1/256 of a 16-bit step and
// gives back the wet in the same units, the room's level already in it;
// roomHeard() sums dry and wet and cuts them by the room's trim (3 dB in
// Busch Hall, 6 dB in the hall) so a loud tape does not clip. State is
// 32-bit delay words, each held to +-2^30, and 64-bit sums; a coefficient
// is a number in 1/65,536, a product rounded by adding 2^15 and shifting
// right 16 (gcc's shift of a negative signed value is arithmetic). Every
// coefficient is written below as a number, computed once from the rooms'
// figures, so no host's pow() or exp() can move a bit.

#ifndef ROOM_H
#define ROOM_H

#include <stdint.h>
#include <string.h>

#define ROOM_OFF 0
#define ROOM_HALL 1
#define ROOM_BUSCH 2

#define ROOM_LINES 8
#define ROOM_LIMIT 0x3FFFFFFF       // a delay word's largest magnitude
#define ROOM_POOL 131072            // delay words: every line rounded up to a power of two fits

// The rooms' delays in frames and coefficients in 1/65,536.
static const int32_t plateInLen[4] = { 239, 180, 637, 465 };
static const int32_t plateTankLen[8] = { 1129, 7481, 3024, 6250, 1525, 7085, 4462, 5314 };
static const int32_t plateTapL[7] = { 447, 4996, 3214, 3353, 3343, 314, 1791 };
static const int32_t plateTapR[7] = { 593, 6094, 2063, 4491, 3547, 563, 203 };
#define PLATE_BW 65503
#define PLATE_IN_G1 49152
#define PLATE_IN_G2 40960
#define PLATE_DIFF1 -45875
#define PLATE_DIFF2 32768
#define PLATE_DAMP 45875
#define PLATE_DECAY 27553
static const int32_t hallLen[8] = { 2477, 2909, 3359, 3803, 4271, 4673, 5153, 5591 };
static const int32_t hallDiffLen[2] = { 557, 919 };
#define HALL_DIFF_G 39322
#define HALL_A250 2027
#define HALL_A4K 25892
#define BUSCH_PRE 2000
static const int32_t buschGainLow[8] = { 60415, 59563, 58689, 57840, 56957, 56210, 55331, 54540 };
static const int32_t buschGainMid[8] = { 59483, 58486, 57466, 56476, 55452, 54586, 53571, 52661 };
static const int32_t buschGainHigh[8] = { 52277, 50256, 48234, 46319, 44382, 42783, 40950, 39345 };
#define BUSCH_HALL_OUT 15684
#define BUSCH_PLATE_OUT 11211
#define BUSCH_TRIM 46396
#define HALL_PRE 833
static const int32_t hallGain[8] = { 58471, 57319, 56144, 55007, 53834, 52847, 51692, 50659 };
#define HALL_DAMP 39322
#define HALL_OUT 19519
#define HALL_TRIM 32846

// The plate's tank: its delays in this order, and the delay and sign of each output tap.
enum { P_AP_L1, P_D_L1, P_AP_L2, P_D_L2, P_AP_R1, P_D_R1, P_AP_R2, P_D_R2 };
static const int plateTapDelayL[7] = { P_D_R1, P_D_R1, P_AP_R2, P_D_R2, P_D_L1, P_AP_L2, P_D_L2 };
static const int plateTapDelayR[7] = { P_D_L1, P_D_L1, P_AP_L2, P_D_L2, P_D_R1, P_AP_R2, P_D_R2 };
static const int plateTapSign[7] = { 1, 1, -1, 1, -1, -1, -1 };

typedef struct
{
    int32_t *bufP;
    uint32_t mask;
    uint32_t pos;
    uint32_t len;                   // the delay, in frames
} RoomDelay;

typedef struct
{
    int kind;
    int used;                       // pool words given to delays
    int32_t pool[ROOM_POOL];
    RoomDelay pre;
    RoomDelay diff[2];
    RoomDelay lines[ROOM_LINES];
    RoomDelay plateIn[4];
    RoomDelay plate[8];
    int64_t out[ROOM_LINES];        // each line's output after its loss, fed back
    int64_t lp250[ROOM_LINES];      // Busch Hall: each line's low-passes at 250 Hz and 4 kHz
    int64_t lp4k[ROOM_LINES];
    int64_t bandwidth;              // the plate's input filter, its two dampers and its two feedbacks
    int64_t dampL;
    int64_t dampR;
    int64_t lastL;
    int64_t lastR;
} Room;

// x times k/65,536, rounded.
static int64_t
roomMul(int64_t x, int32_t k)
{
    return( ((x * k) + 32768) >> 16 );
}

// x held to a delay word's range.
static int32_t
roomSat(int64_t x)
{
    if( x > ROOM_LIMIT )
    {
        return( ROOM_LIMIT );
    }
    if( x < -ROOM_LIMIT )
    {
        return( -ROOM_LIMIT );
    }
    return( (int32_t)x );
}

// Gives a delay of len frames its words from the pool, a power of two above len.
static void
roomDelayInit(Room *rP, RoomDelay *dP, int32_t len)
{
uint32_t size;

    size = 1;
    while( size <= (uint32_t)len )
    {
        size <<= 1;
    }
    dP->bufP = &rP->pool[rP->used];
    rP->used += (int)size;
    dP->mask = (size - 1);
    dP->pos = 0;
    dP->len = (uint32_t)len;
}

// The word written k frames ago, k from 1.
static int64_t
roomTap(const RoomDelay *dP, uint32_t k)
{
    return( dP->bufP[(dP->pos - k) & dP->mask] );
}

// Writes x and returns the word written len frames ago.
static int64_t
roomPush(RoomDelay *dP, int64_t x)
{
int64_t y;

    y = roomTap(dP, dP->len);
    dP->bufP[dP->pos & dP->mask] = roomSat(x);
    dP->pos++;
    return( y );
}

// A Schroeder all-pass of gain g/65,536 on delay dP.
static int64_t
roomAllpass(RoomDelay *dP, int64_t x, int32_t g)
{
int64_t d;
int32_t v;

    d = roomTap(dP, dP->len);
    v = roomSat(x + roomMul(d, g));
    dP->bufP[dP->pos & dP->mask] = v;
    dP->pos++;
    return( d - roomMul(v, g) );
}

// Clears the room and sets it to kind (ROOM_OFF, ROOM_HALL, ROOM_BUSCH).
static void
roomInit(Room *rP, int kind)
{
int i;

    memset(rP, 0, sizeof(*rP));
    rP->kind = kind;
    if( kind == ROOM_OFF )
    {
        return;
    }

    roomDelayInit(rP, &rP->pre, ((kind == ROOM_BUSCH) ? BUSCH_PRE : HALL_PRE));
    for( i = 0; i < 2; i++ )
    {
        roomDelayInit(rP, &rP->diff[i], hallDiffLen[i]);
    }
    for( i = 0; i < ROOM_LINES; i++ )
    {
        roomDelayInit(rP, &rP->lines[i], hallLen[i]);
    }
    if( kind == ROOM_BUSCH )
    {
        for( i = 0; i < 4; i++ )
        {
            roomDelayInit(rP, &rP->plateIn[i], plateInLen[i]);
        }
        for( i = 0; i < 8; i++ )
        {
            roomDelayInit(rP, &rP->plate[i], plateTankLen[i]);
        }
    }
}

// Busch Hall's plate on the predelayed input: the input diffusers, then the
// figure-of-eight tank, each half a diffuser, a delay, a damper, a second
// diffuser and a delay, feeding the other. Returns the left taps' sum in
// *leftP and the right's in *rightP, read after this frame's words are written.
static void
roomPlate(Room *rP, int64_t x, int64_t *leftP, int64_t *rightP)
{
RoomDelay *pP;
int64_t v;
int i;

    pP = rP->plate;
    rP->bandwidth += roomMul((x - rP->bandwidth), PLATE_BW);
    x = roomAllpass(&rP->plateIn[0], rP->bandwidth, PLATE_IN_G1);
    x = roomAllpass(&rP->plateIn[1], x, PLATE_IN_G1);
    x = roomAllpass(&rP->plateIn[2], x, PLATE_IN_G2);
    x = roomAllpass(&rP->plateIn[3], x, PLATE_IN_G2);

    v = roomAllpass(&pP[P_AP_L1], (x + rP->lastR), PLATE_DIFF1);
    v = roomPush(&pP[P_D_L1], v);
    rP->dampL += roomMul((v - rP->dampL), PLATE_DAMP);
    v = roomAllpass(&pP[P_AP_L2], roomMul(rP->dampL, PLATE_DECAY), PLATE_DIFF2);
    rP->lastL = roomMul(roomPush(&pP[P_D_L2], v), PLATE_DECAY);

    v = roomAllpass(&pP[P_AP_R1], (x + rP->lastL), PLATE_DIFF1);
    v = roomPush(&pP[P_D_R1], v);
    rP->dampR += roomMul((v - rP->dampR), PLATE_DAMP);
    v = roomAllpass(&pP[P_AP_R2], roomMul(rP->dampR, PLATE_DECAY), PLATE_DIFF2);
    rP->lastR = roomMul(roomPush(&pP[P_D_R2], v), PLATE_DECAY);

    *leftP = 0;
    *rightP = 0;
    for( i = 0; i < 7; i++ )
    {
        *leftP += (plateTapSign[i] * roomTap(&pP[plateTapDelayL[i]], (uint32_t)plateTapL[i]));
        *rightP += (plateTapSign[i] * roomTap(&pP[plateTapDelayR[i]], (uint32_t)plateTapR[i]));
    }
}

// One frame: dryL and dryR in 1/256 of a 16-bit step; the wet left and right in
// the same units to *wetLP and *wetRP. The hall's eight lines feed back through a
// Householder matrix (each line less a quarter of their sum), the input added to
// the even lines and taken from the odd; even lines are heard on the left, odd on
// the right, every other pair inverted. A room that is off gives 0.
static void
roomFrame(Room *rP, int64_t dryL, int64_t dryR, int64_t *wetLP, int64_t *wetRP)
{
int64_t x;
int64_t y;
int64_t s;
int64_t o;
int64_t sumL;
int64_t sumR;
int64_t plateL;
int64_t plateR;
int i;

    *wetLP = 0;
    *wetRP = 0;
    if( rP->kind == ROOM_OFF )
    {
        return;
    }

    x = roomPush(&rP->pre, ((dryL + dryR) >> 1));
    plateL = 0;
    plateR = 0;
    if( rP->kind == ROOM_BUSCH )
    {
        roomPlate(rP, x, &plateL, &plateR);
    }
    x = roomAllpass(&rP->diff[0], x, HALL_DIFF_G);
    x = roomAllpass(&rP->diff[1], x, HALL_DIFF_G);

    s = 0;
    for( i = 0; i < ROOM_LINES; i++ )
    {
        s += rP->out[i];
    }
    s = ((s + 2) >> 2);

    sumL = 0;
    sumR = 0;
    for( i = 0; i < ROOM_LINES; i++ )
    {
        y = roomPush(&rP->lines[i], ((rP->out[i] - s) + (((i & 1) != 0) ? -x : x)));
        if( rP->kind == ROOM_BUSCH )
        {
            // Three bands that add back to the line exactly, each with its own decay.
            rP->lp250[i] += roomMul((y - rP->lp250[i]), HALL_A250);
            rP->lp4k[i] += roomMul((y - rP->lp4k[i]), HALL_A4K);
            o = (roomMul(rP->lp250[i], buschGainLow[i]) + roomMul((rP->lp4k[i] - rP->lp250[i]), buschGainMid[i])
                 + roomMul((y - rP->lp4k[i]), buschGainHigh[i]));
        }
        else
        {
            o = (rP->out[i] + roomMul((roomMul(y, hallGain[i]) - rP->out[i]), HALL_DAMP));
        }
        rP->out[i] = o;
        if( (i & 1) != 0 )
        {
            sumR += (((i & 2) != 0) ? -o : o);
        }
        else
        {
            sumL += (((i & 2) != 0) ? -o : o);
        }
    }

    if( rP->kind == ROOM_BUSCH )
    {
        *wetLP = (roomMul(sumL, BUSCH_HALL_OUT) + roomMul(plateL, BUSCH_PLATE_OUT));
        *wetRP = (roomMul(sumR, BUSCH_HALL_OUT) + roomMul(plateR, BUSCH_PLATE_OUT));
    }
    else
    {
        *wetLP = roomMul(sumL, HALL_OUT);
        *wetRP = roomMul(sumR, HALL_OUT);
    }
}

// The sample heard, in 1/256 of a step: dry plus wet, cut by the room's trim.
static int64_t
roomHeard(const Room *rP, int64_t dry, int64_t wet)
{
    return( roomMul((dry + wet), ((rP->kind == ROOM_BUSCH) ? BUSCH_TRIM : HALL_TRIM)) );
}

#endif
