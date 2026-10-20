/* Fake the four libei accessors so we can unit-test the shim without a session. */
#include "ei_stub.h"
void   *g_ev;
double  g_dx, g_dy;
int     g_ddx, g_ddy;
double ei_event_scroll_get_dx(void *e){(void)e; return g_dx;}
double ei_event_scroll_get_dy(void *e){(void)e; return g_dy;}
int    ei_event_scroll_get_discrete_dx(void *e){(void)e; return g_ddx;}
int    ei_event_scroll_get_discrete_dy(void *e){(void)e; return g_ddy;}
