#ifndef XBOXRECOMP_NV2A_VSH_INTERP_H
#define XBOXRECOMP_NV2A_VSH_INTERP_H

#include <stdint.h>
#include <stddef.h>

#define NV2A_VSH_SLOTS      136
#define NV2A_VSH_CONSTANTS  192
#define NV2A_VSH_INPUTS     16

typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];
    float tex[4][4];
} Nv2aVshOutput;

void nv2a_vsh_set_load_slot(uint32_t slot);
void nv2a_vsh_program_word(uint32_t word);
void nv2a_vsh_set_start_slot(uint32_t slot);
void nv2a_vsh_set_constant_load(uint32_t index);
void nv2a_vsh_constant_word(uint32_t word);
void nv2a_vsh_set_cxt_write(uint32_t enable);
uint32_t nv2a_vsh_cxt_write(void);

int  nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out);

void nv2a_vsh_set_constant(uint32_t index, const float v[4]);
const float *nv2a_vsh_constant(uint32_t index);

void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word);
void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4]);

uint64_t nv2a_vsh_program_key(void);
int nv2a_vsh_hlsl(char *buf, size_t cap, uint32_t *inputs_read);
const float *nv2a_vsh_constants(void);
uint32_t nv2a_vsh_const_revision(void);

#endif

