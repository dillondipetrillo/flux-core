#include <signal.h>
#include <stdio.h>

#include "config.h"
#include "engine.h"
#include "logger.h"

int main(void)
{
    /**
     * SIGPIPE must be ignored before anything else. We also set it inside
     * engine_init, but setting it here first ensures it is ignored even
     * before the engine initializes, protecting any send() calls that might
     * happen during setup.
     */
    signal(SIGPIPE, SIG_IGN);

    struct engine_config config = config_load();

    if (logger_init(config.log_path) == -1) {
        fprintf(stderr, "FATAL: could not open log file: %s\n",
            config.log_path);
        return 1;
    }

    config_log(&config);

    if (engine_init(&config) == -1) {
        log_error("FATAL: engine_init failed");
        logger_close();
        return 1;
    }

    /**
     * Select and register the auth hook. JWT takes priority over HTTP if both
     * are configured. Falls back to default_auth_hook (dev mode) if neither
     * is set.
     */
    auth_hook_fn hook = select_auth_hook(&config);
    engine_set_auth_hook(hook);

    // Block here until SIGTERM or SIGINT fires
    engine_run();

    logger_close();
    return 0;
}