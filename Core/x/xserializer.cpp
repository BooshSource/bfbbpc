#include "xserializer.h"

#include "print.h"

static int g_serinit;

int xSerialStartup(int count, st_SERIAL_PERCID_SIZE *sizeinfo)
{
    BFBBSTUB("xSerialStartup");

    if (!g_serinit++)
    {
        // ...
    }

    return g_serinit;
}

int xSerialShutdown()
{
    g_serinit--;
    return g_serinit;
}

void xSerialTraverse(xSerialTraverseCallBack func)
{
    BFBBSTUB("xSerialTraverse");
}

int xSerial::Write_b1(int bits)
{
    BFBBSTUB("xSerial::Write_b1");
    return 0;
}

int xSerial::Read_b1(int *bits)
{
    BFBBSTUB("xSerial::Read_b1");
    return 0;
}

void xSerialWipeMainBuffer()
{
    BFBBSTUB("xSerialShutdown");
}