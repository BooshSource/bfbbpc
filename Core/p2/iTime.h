#ifndef ITIME_H
#define ITIME_H

#include <time.h>

typedef clock_t iTime;

#define iTimeTicksPerSec() CLOCKS_PER_SEC

#define iTimeTicksToSecs(t) ((float)(t) / iTimeTicksPerSec())
#define iTimeSecsToTicks(s) ((iTime)(s) * iTimeTicksPerSec())

void iTimeInit();
void iTimeExit();
iTime iTimeGet();
float iTimeDiffSec(iTime time);
float iTimeDiffSec(iTime t0, iTime t1);
void iTimeGameAdvance(float elapsed);
void iTimeSetGame(float time);
void iProfileClear(unsigned int sceneID);
void iFuncProfileDump();
void iFuncProfileParse(const char *, int);

#endif