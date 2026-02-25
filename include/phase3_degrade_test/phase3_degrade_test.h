#ifndef _phase3_degrade_test_main_H
#define _phase3_degrade_test_main_H
#ifndef _PHASE3_DEGRADE_TEST_MAIN_H // necessary for arduino-cli, which automatically includes headers that are not used

#ifdef __cplusplus
extern "C" {
#endif
#include "../include/api/schedule.h"
#include "../include/core/reactor.h"
#ifdef __cplusplus
}
#endif
typedef struct phase3_degrade_test_self_t{
    self_base_t base; // This field is only to be used by the runtime, not the user.
    int count;
    int end[0]; // placeholder; MSVC does not compile empty structs
} phase3_degrade_test_self_t;
#endif
#endif
