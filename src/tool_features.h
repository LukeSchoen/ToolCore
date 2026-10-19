// The boundary between ToolCore event dispatch and optional endpoint extensions.
// ToolCore provides the connection, local controls, Input Director and text pasting.
// An extension build links one implementation of these hooks; the ToolCore
// build links the no-op implementation in tool_features_none.c.

#ifndef TOOL_FEATURES_H
#define TOOL_FEATURES_H

#include "common.h"

// Handle an event the core does not own. 1 = handled and answered.
int tool_extension_event(const char *event, lines_t *lr);
int tool_extension_event_quiet(const char *event);
int tool_extension_event_log(const char *event, char *out, int outsz);

// Called once when the process starts, whenever a sealed connection becomes
// ready, when the core would report its own state, and when that connection
// goes away.
void tool_extension_start(void);
void tool_extension_connected(void);
void tool_extension_state_note(void);
void tool_extension_idle(void);
void tool_extension_disconnect(void);
void tool_extension_stop(void);

// Services ToolCore owns, made available to an extension implementation without
// exposing its state or letting another copy of the networking code appear.
void tool_core_log(const char *fmt, ...);
int tool_core_send(const char *fmt, ...);
void tool_core_toast(const char *text);
SOCKET tool_core_socket(void);
const char *tool_core_server(void);
int tool_core_server_port(void);
void tool_core_resolve_path(const char *path, char *out, int outsz);
void tool_core_dispatch_event(const char *event, lines_t *lr);

#endif
