#include "internal.h"

bool workbench_toggle_playback(
    SoundPlayback *playback,
    WorkbenchAudio *audio,
    SoundWorkspace workspace,
    SoundError *error
) {
    if (sound_playback_is_playing(playback)) {
        return sound_playback_stop(playback, error);
    }

    if (!sound_clip_has_audio(&audio->clip) &&
        !workbench_ensure_selected_recording_loaded(audio, error)) {
        return false;
    }

    if (!sound_clip_has_audio(&audio->clip)) {
        return true;
    }

    const float *samples = sound_clip_samples(&audio->clip);
    uint64_t sample_count = sound_clip_sample_count(&audio->clip);

    if (workspace == SOUND_WORKSPACE_BAND && audio->audition != SOUND_AUDITION_ORIGINAL) {
        if (!workbench_ensure_band_render(audio, error)) {
            return false;
        }

        samples = audio->audition == SOUND_AUDITION_SELECTED ?
            audio->selected_samples :
            audio->rejected_samples;
        sample_count = audio->render_count;
    }

    if (!sound_playback_start(playback, samples, sample_count, audio->clip.sample_rate, error)) {
        return false;
    }

    audio->playback_offset = audio->clip.trim_start;
    return true;
}
