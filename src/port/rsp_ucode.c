/*
 * rsp_ucode.c - RSP microcode symbols.
 *
 * The game puts the addresses and sizes of the RSP boot, graphics
 * (gsp3D) and audio (aspMain) microcode into each OSTask it submits. On
 * N64 that code runs on the RSP. Here tasks are run by C handlers
 * (os_sp.c), which never read the microcode, so these only have to exist
 * and give a small non-zero size (End - Start) in case anything checks.
 */
#include <ultra64.h>

u64 rspbootTextStart[2];
u64 rspbootTextEnd[1];
u64 gsp3DTextStart[2];
u64 gsp3DDataStart[2];
u64 aspMainTextStart[2];
u64 aspMainDataStart[2];
