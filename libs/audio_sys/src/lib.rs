use std::sync::LazyLock;

mod sdl_audio;
mod audio_system;

use audio_system::AudioSystem;

fn master_volume_to_linear(value: f32) -> f32 {
    if value <= 0.0 {
        return 0.0;
    }

    let power = (value - 1.0) * 2.0;
    10.0f32.powf(power)
}

static AUDIO_SYSTEM: LazyLock<AudioSystem> = LazyLock::new(|| {
    AudioSystem::init().expect("Failed to initialize audio system")
});

/// Initialize the audio system and start playing audio.
#[unsafe(no_mangle)] pub extern "C" fn initialize() {
    // Thanks to the LazyLock, just calling any fn on AUDIO_SYSTEM is enough to run init, too.
    AUDIO_SYSTEM.resume_output_device().expect("Failed to resume output device during initialization");
}

/// Set whether reverb is enabled
#[unsafe(no_mangle)] pub extern "C" fn set_enable_reverb(enabled: bool) {
    todo!()
}

/// Sets the main volume level for all audio streams.
///
/// # Parameters
///
/// - `master_volume` - a volume level between 0.0 (silent) and 1.0 ()
#[unsafe(no_mangle)] pub extern "C" fn set_master_volume(master_volume: f32) {
    todo!()
}

#[unsafe(no_mangle)] pub extern "C" fn set_paused(paused: bool) {
    todo!()
}

#[unsafe(no_mangle)] pub extern "C" fn get_reset_count(channel_idx: i32) -> u32 {
    todo!()
}

#[unsafe(no_mangle)] pub extern "C" fn volume_from_u16(value: u16) -> f32 {
    todo!()
}
