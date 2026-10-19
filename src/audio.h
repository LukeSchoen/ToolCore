// Master volume and mute for the default playback device, through Core Audio.
// The headers for these interfaces are not in the compiler's payload, so the
// few vtable slots we need are declared here.

#ifndef TOOL_AUDIO_H
#define TOOL_AUDIO_H

// All of these return 1 on success, 0 when there is no audio device to talk to.
int audio_set_mute(int on);
int audio_set_volume(int percent);        // 0-100
int audio_get_volume(int *percent);
int audio_get_mute(int *on);

#endif
