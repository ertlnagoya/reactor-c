/**
 * @file ms_reaction_key.h
 * @brief The stable 64-bit key of a reaction used by the master scheduler (D and X records,
 * replay, the key-collision check): FNV-1a of the reactor's full name, mixed with the
 * reaction number. Shared by the worker dispatch (reactor_threaded.c) and the direct
 * execution of a downstream reaction (reactor_common.c), so both name a reaction alike.
 */
#ifndef MS_REACTION_KEY_H
#define MS_REACTION_KEY_H

#include <stdint.h>
#include "lf_types.h"
#include "reactor.h"

static inline uint64_t _ms_hash_str64(const char* s) {
  uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
  if (s == NULL) return h;
  while (*s) {
    h ^= (uint8_t)(*s++);
    h *= 1099511628211ULL;
  }
  return h;
}

static inline uint64_t _ms_reaction_stable_key(reaction_t* reaction) {
  if (reaction == NULL) return 0;
  const char* instance = NULL;
  if (reaction->self != NULL) {
    instance = lf_reactor_full_name((self_base_t*)reaction->self);
  }
  if (instance != NULL && instance[0] != '\0') {
    return _ms_hash_str64(instance) ^ (((uint64_t)(uint32_t)reaction->number) * 0x9E3779B185EBCA87ULL);
  }
  if (reaction->name != NULL && reaction->name[0] != '\0') {
    return _ms_hash_str64(reaction->name) ^ (((uint64_t)(uint32_t)reaction->number) * 0x9E3779B185EBCA87ULL);
  }
  return ((uint64_t)(uint32_t)reaction->number << 32) ^ (uint64_t)(uint32_t)LF_LEVEL(reaction->index);
}

#endif // MS_REACTION_KEY_H
