#ifndef SOUNDS_WORKSPACE_H
#define SOUNDS_WORKSPACE_H

typedef enum SoundWorkspace {
    SOUND_WORKSPACE_LIVE,
    SOUND_WORKSPACE_CLIPS,
    SOUND_WORKSPACE_SPECTRUM,
    SOUND_WORKSPACE_BAND,
    SOUND_WORKSPACE_COMPARE,
    SOUND_WORKSPACE_COUNT,
} SoundWorkspace;

int sound_workspace_count(void);
SoundWorkspace sound_workspace_at(int index);
int sound_workspace_index(SoundWorkspace workspace);
SoundWorkspace sound_workspace_offset(SoundWorkspace workspace, int offset);
const char *sound_workspace_name(SoundWorkspace workspace);
const char *sound_workspace_short_name(SoundWorkspace workspace);

#endif
