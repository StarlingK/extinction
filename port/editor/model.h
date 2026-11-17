#ifndef MODEL_H
#define MODEL_H

#include "gamedata.h"

struct ModelPart {
    std::vector<float> v;
    std::vector<uint32_t> idx;
    GdRef tex;
};

struct ModelCpu {
    std::vector<ModelPart> parts;
    float bmin[3], bmax[3];
    ModelCpu();
};

bool model_build(TbdStore &s, const GdClassRes &r, ModelCpu &m);
bool model_build_ref(TbdStore &s, const GdRef &r, ModelCpu &m);

#endif
