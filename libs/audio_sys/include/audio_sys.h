#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>

extern "C" {

/// Initialize the audio system and start playing audio.
void initialize();

/// Set whether reverb is enabled
void set_enable_reverb(bool enabled);

/// Sets the main volume level for all audio streams.
///
/// # Parameters
///
/// - `master_volume` - a volume level between 0.0 (silent) and 1.0 ()
void set_master_volume(float master_volume);

void set_paused(bool paused);

uint32_t get_reset_count(int32_t channel_idx);

float volume_from_u16(uint16_t value);

}  // extern "C"
