#ifndef AVP_MAPEXT_H
#define AVP_MAPEXT_H

#include "avp_nmap.h"

void avp_mapext_start(const Nmap *m);
void avp_mapext_tick(void);
void avp_mapext_stop(void);
int  avp_mapext_terrain_apply(const NmapTerrain *t);
void avp_mapext_prepare(Nmap *m);

#endif
