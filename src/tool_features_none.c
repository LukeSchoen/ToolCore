#include "tool_features.h"

int tool_extension_event(const char *event, lines_t *lr)
{
    (void)event;
    (void)lr;
    return 0;
}

int tool_extension_event_quiet(const char *event)
{
    (void)event;
    return 0;
}

int tool_extension_event_log(const char *event, char *out, int outsz)
{
    (void)event;
    (void)out;
    (void)outsz;
    return 0;
}

void tool_extension_start(void) { }
void tool_extension_connected(void) { }
void tool_extension_state_note(void) { }
void tool_extension_idle(void) { }
void tool_extension_disconnect(void) { }
void tool_extension_stop(void) { }
