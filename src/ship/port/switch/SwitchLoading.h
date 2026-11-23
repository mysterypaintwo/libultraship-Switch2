#pragma once

// Boot loading screen for console builds. It draws with scissored clears only,
// so it needs no shaders or textures and works as soon as the window exists.
#ifdef __SWITCH__
#ifdef __cplusplus
extern "C" {
#endif
// Starts a step that fills the bar from start to end (0 to 1). label must be a
// string literal.
void SwitchLoading_Stage(const char* label, float start, float end);
// Reports progress in the current step and animates the screen.
void SwitchLoading_Update(int done, int total);
void SwitchLoading_Finish(void);
#ifdef __cplusplus
}
#endif
#else
#define SwitchLoading_Stage(label, start, end)
#define SwitchLoading_Update(done, total)
#define SwitchLoading_Finish()
#endif
