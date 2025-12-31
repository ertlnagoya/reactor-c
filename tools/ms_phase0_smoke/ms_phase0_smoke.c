#include <stdio.h>
#include "master_scheduler.h"

int main(void) {
  ms_init(NULL);
  ms_shutdown();
  return 0;
}