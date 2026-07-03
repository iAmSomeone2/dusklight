use sdl3_sys::audio::SDL_AudioFormat;
use crate::sdl_audio::{AudioDevice, OutputStream, SDLResult, init_subsystem, AudioSpec};


pub struct AudioSystem {
    output_device: AudioDevice,
    playback_stream: OutputStream
}

impl AudioSystem {
    const DEFAULT_SPEC: AudioSpec = AudioSpec::new(SDL_AudioFormat::F32LE, 2, 32_000);

    pub fn init() -> SDLResult<Self> {
        init_subsystem()?;

        let device = AudioDevice::open_playback(None)?;
        device.pause()?;

        let mut playback_stream = device.create_stream()?;
        playback_stream.set_format(&Self::DEFAULT_SPEC)?;
        
        Ok(Self { output_device: device, playback_stream })
    }
    
    pub fn pause_output_device(&self) -> SDLResult<()> {
        self.output_device.pause()
    }
    
    pub fn resume_output_device(&self) -> SDLResult<()> {
        self.output_device.resume()
    }
}