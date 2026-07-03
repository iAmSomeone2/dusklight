use std::ffi::CStr;
use std::fmt::Debug;
use std::mem::MaybeUninit;
use sdl3_sys::audio::*;
use sdl3_sys::error::SDL_GetError;

#[derive(Debug)]
pub struct SDLError(String);

impl SDLError {
    /// Calls `SDL_GetError` to get the most recent error message.
    pub fn get_current() -> Self {
        let raw_err = unsafe { CStr::from_ptr(SDL_GetError()) };
        Self(raw_err.to_string_lossy().into_owned())
    }
}

impl core::fmt::Display for SDLError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl core::error::Error for SDLError {}

pub type SDLResult<T> = Result<T, SDLError>;

const fn get_sdl_audio_fmt_str(fmt: SDL_AudioFormat) -> &'static str {
    match fmt {
        SDL_AudioFormat::S8 => "s8",
        SDL_AudioFormat::S16 => "s16",
        SDL_AudioFormat::S32 => "s32",
        SDL_AudioFormat::U8 => "u8",
        SDL_AudioFormat::F32 => "f32",
        SDL_AudioFormat::S32BE => "s32be",
        SDL_AudioFormat::F32BE => "f32be",
        SDL_AudioFormat::UNKNOWN | _ => "UNKNOWN",
    }
}

#[repr(transparent)]
pub struct AudioSpec(SDL_AudioSpec);

impl core::fmt::Debug for AudioSpec {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("AudioSpec")
            .field("channels", &self.0.channels)
            .field("frequency", &self.0.freq)
            .field("format", &get_sdl_audio_fmt_str(self.0.format))
            .finish()
    }
}

impl From<SDL_AudioSpec> for AudioSpec {
    fn from(v: SDL_AudioSpec) -> Self {
        Self(v)
    }
}

impl AsRef<SDL_AudioSpec> for AudioSpec {
    fn as_ref(&self) -> &SDL_AudioSpec {
        &self.0
    }
}

// impl core::fmt::Display for AudioSpec {
//     fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
//         let spec = &self.0;
//
//         let fmt = get_sdl_audio_fmt_str(spec.format);
//
//         writeln!(f, "AudioSpec {{\n")?;
//         writeln!(f, "  channels: {}", spec.channels)?;
//         writeln!(f, "  frequency: {}Hz", spec.freq)?;
//         writeln!(f, "  format: {fmt}")?;
//         writeln!(f, "}}")
//     }
// }

pub struct AudioDevice {
    id: Option<SDL_AudioDeviceID>,
    is_default: bool,
    spec: AudioSpec,
    sample_frames: i32,
}

impl Debug for AudioDevice {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("AudioDevice")
            .field("id", &self.id.map(|id| id.0))
            .field("is_default", &self.is_default)
            .field("spec", &self.spec)
            .field("sample_frames", &self.sample_frames)
            .finish()
    }
}

impl core::fmt::Display for AudioDevice {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let name = self.get_name();
        write!(f, "{name}")
    }
}

impl AudioDevice {
    pub fn open_playback(device: Option<SDL_AudioDeviceID>) -> SDLResult<Self> {
        let is_default = device.is_none();
        let device = device.unwrap_or(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);

        let (spec, sample_frames) =  {
          let mut optimal_spec: MaybeUninit<SDL_AudioSpec> = MaybeUninit::uninit();
            let mut sample_frames: i32 = 0;
            unsafe {
                if !SDL_GetAudioDeviceFormat(device, optimal_spec.as_mut_ptr(), &raw mut sample_frames) {
                    return Err(SDLError::get_current());
                }
            }
            (unsafe { optimal_spec.assume_init() }, sample_frames)
        };

        let id = unsafe { SDL_OpenAudioDevice(device, &spec) };

        if id == 0 {
            // An error occurred
            return Err(SDLError::get_current());
        }

        Ok(Self { id: Some(id), is_default, spec: spec.into(), sample_frames })
    }

    pub fn close(&mut self) {
        if let Some(id) = self.id.take() {
            unsafe { SDL_CloseAudioDevice(id) };
        }
    }

    pub fn get_name(&self) -> String {
        let id = if let Some(id) = self.id {
            id
        } else {
            return "<closed device>".to_string();
        };

        let phys_name = unsafe {
            CStr::from_ptr(SDL_GetAudioDeviceName(id))
        };

        if self.is_default {
            format!("default [currently {}]", phys_name.to_string_lossy())
        } else {
            phys_name.to_string_lossy().to_string()
        }
    }

    pub fn create_stream<'handle>(&'handle self) -> SDLResult<Handle<&'handle AudioStream>> {
        unsafe {
            
        }
    }
}

impl Drop for AudioDevice {
    fn drop(&mut self) {
        self.close();
    }
}

pub struct Handle<'handle, T: Drop>(&'handle mut T);

pub struct AudioStream(SDL_AudioStream);

impl Drop for AudioStream {
    fn drop(&mut self) {
        unsafe {
            SDL_UnbindAudioStream(&raw mut self.0);
        }
    }
}