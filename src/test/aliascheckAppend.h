// Follow SVF/TestSuite aliascheck.h.

#include <stdio.h>
#include <stdlib.h>

void MARK_AS(void* p, int idx){
  printf("\n");
}

void MARK_FUNCTION(int idx){
  printf("\n");
}

void CHECK_POINTS_TO(void* p, int idx){
  printf("\n");
}

void CHECK_NO_POINTS_TO(void* p, int idx){
  printf("\n");
}

void CHECK_ALIAS(void* p, int idx){
  printf("\n");
}

void CHECK_REACH(int idx){
  printf("\n");
}
