use std::ffi::{c_int, c_void, CStr};
use std::fmt::Debug;
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::ptr::null_mut;
use num::{clamp, Num, PrimInt};
use sdl3_sys::audio::*;
use sdl3_sys::error::SDL_GetError;
use sdl3_sys::init::{SDL_InitSubSystem, SDL_INIT_AUDIO};

#[derive(Debug)]
pub struct SDLError(String);

impl SDLError {
    /// Calls `SDL_GetError` to get the most recent error message.
    pub fn get_current() -> Self {
        let raw_err = unsafe { CStr::from_ptr(SDL_GetError()) };
        Self(raw_err.to_string_lossy().into_owned())
    }

    pub fn from_message(msg: impl AsRef<str>) -> Self {
        let msg = msg.as_ref().to_string();
        Self(msg)
    }
}

impl core::fmt::Display for SDLError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl core::error::Error for SDLError {}

pub type SDLResult<T> = Result<T, SDLError>;

#[inline]
fn run_sdl_bool_fn(sdl_func: impl FnOnce() -> bool) -> SDLResult<()> {
    if sdl_func() {
        Ok(())
    } else {
        Err(SDLError::get_current())
    }
}

/// Runs a closure containing a SDL function which returns an integer which may indicate failure.
///
/// # Parameters
///
/// - `sdl_func` - closure used to execute the SDL function
/// - `fail_code` - optional value used to indicate that the operation failed.
///   - **Default**: 0
///
/// # Return Value
///
/// - On success: returns a [SDLResult::Ok] containing the resulting integer value
/// - On failure: returns a [SDLResult::Err] containing the reported [SDLError]
#[inline]
fn run_sdl_status_fn<S: Num>(sdl_func: impl FnOnce() -> S, fail_code: Option<S>) -> SDLResult<S> {
    let fail_code = fail_code.unwrap_or(S::zero());
    let result = sdl_func();

    if result == fail_code {
        Err(SDLError::get_current())
    } else {
        Ok(result)
    }
}

/// Initialize SDL's audio subsystem
pub fn init_subsystem() -> SDLResult<()> {
    run_sdl_bool_fn(|| unsafe { SDL_InitSubSystem(SDL_INIT_AUDIO) })
}

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

#[derive(Copy, Clone)]
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

impl AudioSpec {
    pub const fn new(format: SDL_AudioFormat, channels: u32, sample_rate: u32) -> Self {
        Self(SDL_AudioSpec { format, channels: channels as _, freq: sample_rate as _ })
    }

    #[inline(always)]
    pub const fn bytes_per_sample(&self) -> usize {
        match self.0.format {
            SDL_AudioFormat::S8 | SDL_AudioFormat::U8 => 1,
            SDL_AudioFormat::S16LE | SDL_AudioFormat::S16BE => 2,
            SDL_AudioFormat::S32LE | SDL_AudioFormat::S32BE | SDL_AudioFormat::F32LE | SDL_AudioFormat::F32BE => 4,
            SDL_AudioFormat::UNKNOWN => panic!("Cannot get size for unknown AudioSpec format!"),
            _ => unreachable!(),
        }
    }
}

/*
    TODO: Rewrite both `AudioDevice` and `OutputStream` as Input/Output enums.
    This is the more ergonomic expression in Rust since so much logic is shared between input and output.
*/

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
    #[inline(always)]
    pub fn get_id(&self) -> SDLResult<SDL_AudioDeviceID> {
        if let Some(id) = self.id {
            Ok(id)
        } else {
            Err(SDLError::from_message("AudioDevice not open"))
        }
    }

    /// Open a new [AudioDevice] for playback.
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

    /// Close this [AudioDevice]
    pub fn close(&mut self) {
        if let Some(id) = self.id.take() {
            unsafe { SDL_CloseAudioDevice(id) };
        }
    }

    /// Gets the name of the physical device this [AudioDevice] represents.
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

    /// Creates a new [OutputStream] bound to this [AudioDevice]
    pub fn create_stream(&self) -> SDLResult<OutputStream> {
        let id = self.get_id()?;
        let mut stream: MaybeUninit<SDL_AudioStream> = MaybeUninit::uninit();
        run_sdl_bool_fn(|| unsafe { SDL_BindAudioStream(id, stream.as_mut_ptr()) })?;
        let mut stream = unsafe { stream.assume_init() };

        // Query the current audio spec, so we know what format we're starting with
        let mut spec: MaybeUninit<SDL_AudioSpec> = MaybeUninit::uninit();
        run_sdl_bool_fn(|| unsafe { SDL_GetAudioStreamFormat(&raw mut stream, spec.as_mut_ptr(), null_mut()) })?;
        let spec = unsafe { AudioSpec(spec.assume_init()) };

        Ok(OutputStream {
            stream,
            spec
        })
    }

    #[inline]
    pub fn set_gain(&self, gain: f32) -> SDLResult<()> {
        let id = self.get_id()?;
        let gain = clamp(gain, 0.0, 1.0);
        run_sdl_bool_fn(|| unsafe { SDL_SetAudioDeviceGain(id, gain) })
    }

    #[inline]
    pub fn get_gain(&self) -> SDLResult<f32> {
        let id = self.get_id()?;
        run_sdl_status_fn(|| unsafe { SDL_GetAudioDeviceGain(id) }, Some(-1.0))
    }

    pub fn pause(&self) -> SDLResult<()> {
        let id = self.get_id()?;
        run_sdl_bool_fn(|| unsafe { SDL_PauseAudioDevice(id) })
    }

    pub fn resume(&self) -> SDLResult<()> {
        let id = self.get_id()?;
        run_sdl_bool_fn(|| unsafe { SDL_ResumeAudioDevice(id) })
    }
}

impl Drop for AudioDevice {
    fn drop(&mut self) {
        self.close();
    }
}


pub type AudioStreamCallback<D> = dyn FnMut(&D, u32, u32) + Send + 'static;
// pub type AudioStreamCallback<D> = fn(&mut OutputStream<'_>, &D, u32, u32);

struct StreamCBData<'stream, D> {
    /// Actual Rust-based callback fn
    cb: &'stream mut AudioStreamCallback<D>,
    /// Arbitrary data to pass to the callback fn
    data: &'stream D
}

extern "C" fn audio_stream_cb_wrapper<D>(user_data: *mut c_void, _stream: *mut SDL_AudioStream, additional_amt: i32, total_amt: i32) {
    let user_data = unsafe { &mut *user_data.cast::<StreamCBData<D>>() };
    (user_data.cb)(user_data.data, additional_amt as u32, total_amt as u32);
}

// TODO: Figure out a good way to ensure OutputStream doesn't outlive its parent AudioDevice

pub struct OutputStream {
    stream: SDL_AudioStream,
    spec: AudioSpec,
}

impl Drop for OutputStream {
    fn drop(&mut self) {
        let _ = self.flush();
        unsafe {
            SDL_DestroyAudioStream(&raw mut self.stream);
        }
    }
}

impl OutputStream {
    /// Set the format of upcoming audio stream data.
    ///
    /// This [OutputStream] will ensure that the final audio data matches what the [AudioDevice] expects.
    pub fn set_format(&mut self, spec: &AudioSpec) -> SDLResult<()> {
        run_sdl_bool_fn(|| unsafe { SDL_SetAudioStreamFormat(&mut self.stream, &raw const spec.0, null_mut()) })
    }

    pub fn set_get_callback<D>(&mut self, cb: &mut AudioStreamCallback<D>, user_data: &D) -> SDLResult<()> {
        let mut user_data = StreamCBData {
            cb,
            data: user_data
        };

        run_sdl_bool_fn(|| unsafe { SDL_SetAudioStreamGetCallback(&mut self.stream, Some(audio_stream_cb_wrapper::<D>), &raw mut user_data as _) })
    }

    /// Add data to the stream.
    ///
    /// # Remarks
    ///
    /// This data must match the format/channels/samplerate specified in the latest call to
    /// [OutputStream::set_format], or the format specified when creating the stream if it hasn't
    /// been changed.
    pub fn put_data(&mut self, data: &[u8]) -> SDLResult<()> {
        let sample_count = data.len() / self.spec.bytes_per_sample();
        run_sdl_bool_fn(|| unsafe { SDL_PutAudioStreamData(&mut self.stream, data.as_ptr() as _, sample_count as _) })
    }

    /// Tell the stream that you're done sending data, and anything being buffered should be
    /// converted/resampled and made available immediately.
    pub fn flush(&mut self) -> SDLResult<()> {
        run_sdl_bool_fn(|| unsafe { SDL_FlushAudioStream(&mut self.stream) })
    }

    /// Clear any pending data in the stream.
    pub fn clear(&mut self) -> SDLResult<()> {
        run_sdl_bool_fn(|| unsafe { SDL_ClearAudioStream(&mut self.stream) })
    }

    pub fn set_gain(&mut self, gain: f32) -> SDLResult<()> {
        run_sdl_bool_fn(|| unsafe { SDL_SetAudioStreamGain(&mut self.stream, gain) })
    }

    pub fn get_gain(&mut self) -> SDLResult<f32> {
        run_sdl_status_fn(|| unsafe { SDL_GetAudioStreamGain(&mut self.stream) }, Some(-1.0))
    }

    pub fn pause_device(&mut self) -> SDLResult<()> {
        run_sdl_bool_fn(|| unsafe { SDL_PauseAudioStreamDevice(&mut self.stream) })
    }

    pub fn resume_device(&mut self) -> SDLResult<()> {
        run_sdl_bool_fn(|| unsafe { SDL_ResumeAudioStreamDevice(&mut self.stream) })
    }
}