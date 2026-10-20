#include "ei_stub.h"
#include <stdio.h>
#include <string.h>
extern void *g_ev; extern double g_dx,g_dy; extern int g_ddx,g_ddy;
typedef double (*fd)(void*); typedef int (*fi)(void*);
static int fails;
static void ck(const char*n,double got,double want){
  int ok = (got==want);
  printf("  %-42s %10.4f  %s\n", n, got, ok?"OK":"** FAIL **");
  if(!ok) fails++;
}
int main(void){
  fd sdx=(fd)ei_event_scroll_get_dx, sdy=(fd)ei_event_scroll_get_dy;
  fi sddx=(fi)ei_event_scroll_get_discrete_dx, sddy=(fi)ei_event_scroll_get_discrete_dy;

  puts("[1] wheel detent -15/-15  ->  smooth -10, discrete 0");
  g_dy=-15; g_ddy=-15; g_ddx=0; g_dx=0;
  ck("smooth dy",  sdy(0), -10.0);
  ck("discrete dy",sddy(0), 0.0);

  puts("[2] wheel detent +15/+15  ->  smooth +10, discrete 0");
  g_dy=15; g_ddy=15;
  ck("smooth dy",  sdy(0),  10.0);
  ck("discrete dy",sddy(0), 0.0);

  puts("[3] multi-detent -30 (2 grid) -> -20");
  g_dy=-30; g_ddy=-30;
  ck("smooth dy",  sdy(0), -20.0);

  puts("[4] trackpad-ish values pass through untouched");
  g_dy=-7.3; g_ddy=0; ck("smooth dy -7.3", sdy(0), -7.3);
  g_dy=13.7;             ck("smooth dy 13.7", sdy(0), 13.7);
  g_dy=-1.0;             ck("smooth dy -1.0", sdy(0), -1.0);
  g_dy=0.0;              ck("smooth dy  0.0", sdy(0),  0.0);

  puts("[5] near-but-not-exact multiples must NOT be scaled");
  g_dy=15.01; ck("smooth dy 15.01", sdy(0), 15.01);
  g_dy=14.99; ck("smooth dy 14.99", sdy(0), 14.99);

  puts("[6] horizontal axis is scaled independently");
  g_dx=15; g_dy=0; g_ddx=15; g_ddy=0;
  ck("smooth dx",   sdx(0),  10.0);
  ck("discrete dx", sddx(0),  0.0);
  ck("smooth dy stays", sdy(0), 0.0);

  printf("\n%s (%d failure%s)\n", fails?"FAILED":"ALL PASSED", fails, fails==1?"":"s");
  return fails!=0;
}
