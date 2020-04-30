#include "xpkrsvc.h"

#include "xhipio.h"
#include "xMemMgr.h"
#include "xutil.h"

#include <string.h>

static void PKR_Disconnect(st_PACKER_READ_DATA *pr);
static unsigned int PKR_getPackTimestamp(st_PACKER_READ_DATA *pr);
static int PKR_PkgHasAsset(st_PACKER_READ_DATA *pr, unsigned int aid);
static int PKR_GetAssetInfoByType(st_PACKER_READ_DATA *pr, unsigned int type, int idx,
                                  st_PKR_ASSET_TOCINFO *tocinfo);
static int PKR_GetAssetInfo(st_PACKER_READ_DATA *pr, unsigned int aid,
                            st_PKR_ASSET_TOCINFO *tocinfo);
static unsigned int PKR_GetBaseSector(st_PACKER_READ_DATA *pr);
static char *PKR_AssetName(st_PACKER_READ_DATA *pr, unsigned int aid);
static int PKR_SetActive(st_PACKER_READ_DATA *pr, en_LAYER_TYPE layer);
static int PKR_IsAssetReady(st_PACKER_READ_DATA *pr, unsigned int aid);
static int PKR_AssetCount(st_PACKER_READ_DATA *pr, unsigned int type);
static void *PKR_AssetByType(st_PACKER_READ_DATA *pr, unsigned int type, int idx,
                             unsigned int *size);
static void *PKR_LoadAsset(st_PACKER_READ_DATA *pr, unsigned int aid, char *, void *);
static unsigned int PKR_GetAssetSize(st_PACKER_READ_DATA *pr, unsigned int aid);
static int PKR_LoadLayer(st_PACKER_READ_DATA *, en_LAYER_TYPE);
static void PKR_ReadDone(st_PACKER_READ_DATA *pr);
static st_PACKER_READ_DATA *PKR_ReadInit(void *userdata, const char *pkgfile, 
                                         unsigned int opts, int *cltver,
                                         st_PACKER_ASSETTYPE *typelist);

static st_PACKER_READ_FUNCS g_pkr_read_funcmap_original =
{
    1,
    PKR_ReadInit,
    PKR_ReadDone,
    PKR_LoadLayer,
    PKR_GetAssetSize,
    PKR_LoadAsset,
    PKR_AssetByType,
    PKR_AssetCount,
    PKR_IsAssetReady,
    PKR_SetActive,
    PKR_AssetName,
    PKR_GetBaseSector,
    PKR_GetAssetInfo,
    PKR_GetAssetInfoByType,
    PKR_PkgHasAsset,
    PKR_getPackTimestamp,
    PKR_Disconnect
};

static st_PACKER_READ_FUNCS g_pkr_read_funcmap = g_pkr_read_funcmap_original;
static st_HIPLOADFUNCS *g_hiprf;
static st_PACKER_READ_DATA g_readdatainst[16];
static unsigned int g_loadlock;
static int pkr_sector_size;
static int g_packinit;
static int g_memalloc_pair;
static int g_memalloc_runtot;
static int g_memalloc_runfree;

static int PKR_LoadStep_Async();
static void PKR_alloc_chkidx();
static void *PKR_getmem(unsigned int id, int amount, unsigned int, int align, int isTemp,
                        char **memtru);
static int PKR_parse_TOC(st_HIPLOADDATA *pkg, st_PACKER_READ_DATA *pr);
static void PKR_relmem(unsigned int id, int blksize, void *memptr, unsigned int,
                       int isTemp);
static void PKR_LayerMemRelease(st_PACKER_READ_DATA *pr, st_PACKER_LTOC_NODE *layer);
static void PKR_kiilpool_anode(st_PACKER_READ_DATA *pr);
static void PKR_oldlaynode(st_PACKER_LTOC_NODE *laynode);

st_PACKER_READ_FUNCS *PKRGetReadFuncs(int apiver)
{
    if (apiver == 1)
    {
        return &g_pkr_read_funcmap;
    }

    return 0;
}

int PKRStartup()
{
    if (!g_packinit++)
    {
        g_pkr_read_funcmap = g_pkr_read_funcmap_original;
        g_hiprf = get_HIPLFuncs();

        pkr_sector_size = 32;
    }

    return g_packinit;
}

int PKRShutdown()
{
    g_packinit--;
    return g_packinit;
}

int PKRLoadStep()
{
    int more_todo = PKR_LoadStep_Async();
    return more_todo;
}

static st_PACKER_READ_DATA *PKR_ReadInit(void *userdata, const char *pkgfile,
                                         unsigned int opts, int *cltver,
                                         st_PACKER_ASSETTYPE *typelist)
{
    st_PACKER_READ_DATA *pr = NULL;
    int i;
    int uselock;
    int lockid = -1;
    char *tocbuf_RAW = NULL;
    char *tocbuf_aligned;

    tocbuf_aligned = (char *)PKR_getmem('PTOC', 0x8000, 'PTOC', 64, 1, &tocbuf_RAW);

    for (i = 0; i < 16; i++)
    {
        uselock = 1 << i;

        if (!(g_loadlock & uselock))
        {
            g_loadlock |= uselock;
            lockid = i;
            pr = &g_readdatainst[i];

            break;
        }
    }

    if (pr)
    {
        memset(pr, 0, sizeof(st_PACKER_READ_DATA));

        pr->lockid = lockid;
        pr->userdata = userdata;
        pr->opts = opts;
        pr->types = typelist;
        pr->cltver = -1;

        strncpy(pr->packfile, pkgfile, sizeof(pr->packfile));

        if (!tocbuf_aligned)
        {
            pr->pkg = g_hiprf->create(pkgfile, NULL, 0);
        }
        else
        {
            pr->pkg = g_hiprf->create(pkgfile, tocbuf_aligned, 0x8000);
        }

        if (pr->pkg)
        {
            pr->base_sector = g_hiprf->basesector(pr->pkg);

            PKR_parse_TOC(pr->pkg, pr);

            *cltver = pr->cltver;

            g_hiprf->setBypass(pr->pkg, 1, 1);
        }
        else
        {
            PKR_ReadDone(pr);

            pr = NULL;
            *cltver = -1;
        }
    }
    else
    {
        pr = NULL;
        *cltver = -1;
    }

    PKR_relmem('PTOC', 0x8000, tocbuf_RAW, 'PTOC', 1);

    tocbuf_RAW = NULL;

    return pr;
}

static void PKR_ReadDone(st_PACKER_READ_DATA *pr)
{
    int i;
    int j;
    int lockid;
    st_PACKER_ATOC_NODE *assnode;
    st_PACKER_LTOC_NODE *laynode;
    st_XORDEREDARRAY *tmplist;

    if (pr)
    {
        for (j = pr->laytoc.cnt - 1; j >= 0; j--)
        {
            laynode = (st_PACKER_LTOC_NODE *)pr->laytoc.list[j];

            for (i = laynode->assref.cnt - 1; i >= 0; i--)
            {
                assnode = (st_PACKER_ATOC_NODE *)laynode->assref.list[i];

                if (assnode->typeref &&
                    assnode->typeref->assetUnloaded &&
                    !(assnode->loadflag & 0x100000))
                {
                    assnode->typeref->assetUnloaded(assnode->memloc, assnode->aid);
                }
            }
        }

        for (j = 0; j < pr->laytoc.cnt; j++)
        {
            laynode = (st_PACKER_LTOC_NODE *)pr->laytoc.list[j];

            if (laynode->laymem)
            {
                PKR_LayerMemRelease(pr, laynode);
                laynode->laymem = NULL;
            }
        }

        PKR_kiilpool_anode(pr);

        for (j = 0; j < pr->laytoc.cnt; j++)
        {
            laynode = (st_PACKER_LTOC_NODE *)pr->laytoc.list[j];

            PKR_oldlaynode(laynode);
        }

        XOrdDone(&pr->asstoc, 0);
        XOrdDone(&pr->laytoc, 0);

        for (i = 0; i < (sizeof(pr->typelist) / sizeof(st_XORDEREDARRAY)); i++)
        {
            tmplist = &pr->typelist[i];

            if (tmplist->max)
            {
                XOrdDone(tmplist, 0);
            }
        }

        if (pr->pkg)
        {
            g_hiprf->destroy(pr->pkg);
            pr->pkg = NULL;
        }

        lockid = pr->lockid;

        memset(pr, 0, sizeof(st_PACKER_READ_DATA));

        g_loadlock &= ~(1 << lockid);
    }
}

static int PKR_SetActive(st_PACKER_READ_DATA *pr, en_LAYER_TYPE layer)
{
    int result = 1;
    int rc;
    int i;
    int j;
    st_PACKER_ATOC_NODE *assnode;
    st_PACKER_LTOC_NODE *laynode;

    if (!pr)
    {
        rc = 0;
    }
    else
    {
        for (i = 0; i < pr->laytoc.cnt; i++)
        {
            laynode = (st_PACKER_LTOC_NODE *)&pr->laytoc.list[i];

            if (layer <= PKR_LTYPE_DEFAULT || laynode->laytyp == layer)
            {
                for (j = 0; j < laynode->assref.cnt; j++)
                {
                    assnode = (st_PACKER_ATOC_NODE *)&laynode->assref.list[j];

                    if (!(assnode->loadflag & 0x10000) &&
                        (assnode->loadflag & 0x80000))
                    {
                        if (!assnode->typeref)
                        {
                            // probably printing something here...
                            // WARNING: asset <name> missing typeref
                            assnode->Name();
                            xUtil_idtag2string(assnode->asstype, 0);
                        }
                        else if (assnode->typeref->assetLoaded)
                        {
                            if (!assnode->typeref->assetLoaded(pr->userdata,
                                    assnode->aid, assnode->memloc, assnode->d_size))
                            {
                                result = 0;
                            }
                            else
                            {
                                assnode->loadflag |= 0x1;
                            }
                        }
                    }
                }
            }
        }

        rc = result;
    }

    return rc;
}

// STUB
static int PKR_parse_TOC(st_HIPLOADDATA *pkg, st_PACKER_READ_DATA *pr)
{
    return 0;
}

// STUB
static int PKR_LoadStep_Async()
{
    return 0;
}

// STUB
static void PKR_LayerMemRelease(st_PACKER_READ_DATA *pr, st_PACKER_LTOC_NODE *layer)
{

}

static int PKR_LoadLayer(st_PACKER_READ_DATA *, en_LAYER_TYPE)
{
    return 0;
}

static void *PKR_LoadAsset(st_PACKER_READ_DATA *pr, unsigned int aid, char *, void *)
{
    return 0;
}

static unsigned int PKR_GetAssetSize(st_PACKER_READ_DATA *pr, unsigned int aid)
{
    return 0;
}

static int PKR_AssetCount(st_PACKER_READ_DATA *pr, unsigned int type)
{
    return 0;
}

static void *PKR_AssetByType(st_PACKER_READ_DATA *pr, unsigned int type, int idx,
                             unsigned int *size)
{
    return 0;
}

static int PKR_IsAssetReady(st_PACKER_READ_DATA *pr, unsigned int aid)
{
    return 0;
}

static unsigned int PKR_getPackTimestamp(st_PACKER_READ_DATA *pr)
{
    return 0;
}

static void PKR_Disconnect(st_PACKER_READ_DATA *pr)
{

}

static char *PKR_AssetName(st_PACKER_READ_DATA *pr, unsigned int aid)
{
    return 0;
}

static unsigned int PKR_GetBaseSector(st_PACKER_READ_DATA *pr)
{
    return 0;
}

static int PKR_GetAssetInfo(st_PACKER_READ_DATA *pr, unsigned int aid,
                     st_PKR_ASSET_TOCINFO *tocinfo)
{
    return 0;
}

static int PKR_GetAssetInfoByType(st_PACKER_READ_DATA *pr, unsigned int type, int idx,
                                  st_PKR_ASSET_TOCINFO *tocinfo)
{
    return 0;
}

static int PKR_PkgHasAsset(st_PACKER_READ_DATA *pr, unsigned int aid)
{
    return 0;
}

// STUB
static void PKR_kiilpool_anode(st_PACKER_READ_DATA *pr)
{

}

// STUB
static void PKR_oldlaynode(st_PACKER_LTOC_NODE *laynode)
{

}

static void PKR_alloc_chkidx()
{
    return;
}

static void *PKR_getmem(unsigned int id, int amount, unsigned int, int align, int isTemp,
                        char **memtru)
{
    void *memptr;

    if (amount == 0)
    {
        return 0;
    }

    if (isTemp)
    {
        memptr = xMemPushTemp(amount + align);

        if (memtru)
        {
            *memtru = (char *)memptr;
        }

        memptr = (void *)(-align & ((size_t)memptr + (align - 1)));
    }
    else
    {
        memptr = xMemAlloc(gActiveHeap, amount, align);
    }

    if (memptr)
    {
        memset(memptr, 0, amount);
    }

    g_memalloc_pair++;
    g_memalloc_runtot += amount;

    if (g_memalloc_runtot < 0)
    {
        g_memalloc_runtot = amount;
    }

    if (memptr)
    {
        xUtil_idtag2string(id, 0);
    }
    else
    {
        xUtil_idtag2string(id, 0);
    }

    return memptr;
}

// STUB
static void PKR_relmem(unsigned int id, int blksize, void *memptr, unsigned int,
                       int isTemp)
{

}

const char *st_PACKER_ATOC_NODE::Name() const
{
    return "<unknown>";
}