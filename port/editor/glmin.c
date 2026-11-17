#include "glmin.h"

#define X(r, n, a) r (GLAPI_CALL *n) a;
GL_FUNCS(X)
#undef X

int gl_load(void *(*getproc)(const char *))
{
    int missing = 0;
#define X(r, n, a) \
    n = (r (GLAPI_CALL *) a)getproc(#n); \
    if (!n) \
        missing++;
    GL_FUNCS(X)
#undef X
    return missing == 0;
}
