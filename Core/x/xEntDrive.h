#ifndef XENTDRIVE_H
#define XENTDRIVE_H

#include "xEnt.h"

#define XENTDRIVE_UNK1 0x1

struct xEntDrive
{
    struct tri_data : xCollis::tri_data
    {
        xVec3 loc;
        float yaw;
        xCollis *coll;
    };

    unsigned int flags;
    float otm;
    float otmr;
    float os;
    float tm;
    float tmr;
    float s;
    xEnt *odriver;
    xEnt *driver;
    xEnt *driven;
    xVec3 op;
    xVec3 p;
    xVec3 q;
    float yaw;
    xVec3 dloc;
    tri_data tri;
};

void xEntDriveInit(xEntDrive *drv, xEnt *driven);

#endif