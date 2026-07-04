#include "sounds/workspace.h"

static const SoundWorkspace workspaces[] = {
    SOUND_WORKSPACE_LIVE,
    SOUND_WORKSPACE_CLIPS,
    SOUND_WORKSPACE_SPECTRUM,
    SOUND_WORKSPACE_BAND,
    SOUND_WORKSPACE_COMPARE,
};

int sound_workspace_count(void) {
    return (int)(sizeof(workspaces) / sizeof(workspaces[0]));
}

SoundWorkspace sound_workspace_at(int index) {
    if (index < 0 || index >= sound_workspace_count()) {
        return SOUND_WORKSPACE_LIVE;
    }

    return workspaces[index];
}

int sound_workspace_index(SoundWorkspace workspace) {
    for (int i = 0; i < sound_workspace_count(); ++i) {
        if (workspaces[i] == workspace) {
            return i;
        }
    }

    return 0;
}

SoundWorkspace sound_workspace_offset(SoundWorkspace workspace, int offset) {
    int count = sound_workspace_count();
    int index = sound_workspace_index(workspace) + offset;

    while (index < 0) {
        index += count;
    }

    return sound_workspace_at(index % count);
}

const char *sound_workspace_name(SoundWorkspace workspace) {
    switch (workspace) {
        case SOUND_WORKSPACE_LIVE:
            return "LIVE SPECTROGRAM";
        case SOUND_WORKSPACE_CLIPS:
            return "CLIPS";
        case SOUND_WORKSPACE_SPECTRUM:
            return "WHOLE SPECTRUM";
        case SOUND_WORKSPACE_BAND:
            return "BAND LAB";
        case SOUND_WORKSPACE_COMPARE:
            return "COMPARE";
        case SOUND_WORKSPACE_COUNT:
            break;
    }

    return "LIVE SPECTROGRAM";
}

const char *sound_workspace_short_name(SoundWorkspace workspace) {
    switch (workspace) {
        case SOUND_WORKSPACE_LIVE:
            return "LIVE";
        case SOUND_WORKSPACE_CLIPS:
            return "CLIPS";
        case SOUND_WORKSPACE_SPECTRUM:
            return "SPECTRUM";
        case SOUND_WORKSPACE_BAND:
            return "BAND";
        case SOUND_WORKSPACE_COMPARE:
            return "COMPARE";
        case SOUND_WORKSPACE_COUNT:
            break;
    }

    return "LIVE";
}
