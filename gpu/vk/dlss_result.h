// Which slEvaluateFeature results mean DLSS drew the frame (vk_dlss.cpp). Kept apart so tests/ can check it without
// a GPU or Streamline's DLLs.
//
// eWarnOutOfVRAM (39) is a warning, not a failure: Streamline sets it only after the evaluation went through (eOk),
// when the VRAM budget Windows gives the game is used up (sl.common's commonInterface.cpp, "Exceeded VRAM budget,
// various performance issues including stuttering can be expected"). Taken as a failure, it turned DLSS off for the
// session on a 12 GB RTX 5070 at 11.7 GB used, and the menu then said "no NVIDIA DLSS found".
#pragma once
#include <sl_result.h>

namespace mkwvk {

inline bool dlss_drawn(sl::Result r) { return r == sl::Result::eOk || r == sl::Result::eWarnOutOfVRAM; }

} // namespace mkwvk
