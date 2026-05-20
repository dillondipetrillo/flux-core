#ifndef ENGINE_H
#define ENGINE_H

#include "auth_hook.h"
#include "config.h"

/**
 * engine.h - Public API of the C State Bus engine
 * 
 * Only these functions are visible outside engine.c.
 * All internal state and helper functions are static in engine.c.
 */

 /**
  * Initialize the engine. Must be called before set_auth_hook and run.
  * Returns 0 on success, -1 on failure.
  */
int engine_init(struct engine_config *config);

/**
 * Register the auth hook. Call after init, before run.
 * If not called, default_auth_hook (dev mode) is used.
 */
void engine_set_auth_hook(auth_hook_fn hook);

// Run the event loop. Blocks until SIGTERM or SIGINT
void engine_run(void);

// Signal the loop to stop. Called from signal handlers.
void engine_stop(void);

#endif