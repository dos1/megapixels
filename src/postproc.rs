/*! Post processing helpers. */

use std::ffi::CStr;
use std::os::raw::c_void;
use std::process;
use std::thread;


pub mod c {
    use std::ffi;
    use std::os::raw::c_char;
    use std::slice;
    use super::*;
    
    extern "C" {
        pub fn notify_processing_finished(thumb: *const c_void);
        pub fn mp_main_capture_completed(thumb: *const c_void, path: *const c_char);
    }
    
    pub fn slice_as_ptr(s: &[u8]) -> Result<*const c_char, ffi::FromBytesWithNulError> {
        Ok(CStr::from_bytes_with_nul(&s)?.as_ptr())
    }

    fn array_to_string(array: *const c_char, size: usize) -> Result<String, String> {
        let s = unsafe { slice::from_raw_parts(array as _, size) };
        let first_null = s.iter()
            .position(|i: &u8| *i == 0)
            .map(|i| i + 1)
            .unwrap_or(size);
        let s = &s[..first_null];
        match ffi::CStr::from_bytes_with_nul(s) {
            Err(e) => Err(format!("{}", e)),
            Ok(cstr) => match cstr.to_str() {
                Err(e) => Err(format!("{}", e)),
                Ok(s) => Ok(s.into()),
            }
        }
    }
    
    fn decode_args(
        processing_script: *const c_char,
        burst_dir: *const c_char,
        capture_fname: *const c_char,
    ) -> Result<(String, String, String), String> {
        Ok((
            array_to_string(processing_script, 512)?,
            array_to_string(burst_dir, 23)?,
            array_to_string(capture_fname, 255)?,
        ))
    }
    
    #[no_mangle]
    pub extern "C" fn spawn_post_process_task(
        processing_script: *const c_char,
        burst_dir: *const c_char,
        capture_frame: *const c_char,
        thumb: *const c_void,
    ) {
        match decode_args(processing_script, burst_dir, capture_frame) {
            Ok((script, dir, frame)) => super::spawn_post_process_task(script, dir, frame, thumb),
            Err(e) => {
                eprintln!("Invalid processing script or output path: {}", e);
                unsafe { notify_processing_finished(thumb) };
            }
        }
    }
}

/// Will ignore any empty lines
fn find_last_line(lines: &[u8]) -> Option<&[u8]> {
    for line in lines.rsplit(|byte| *byte == b'\n') {
        if line.len() > 0 {
            return Some(line);
        }
    }
    None
}

/// Spawns a task that runs the script, and then sends its output back to Megapixels.
fn spawn_post_process_task(
    processing_script: String,
    burst_dir: String,
    capture_fname: String,
    thumb: *const c_void,
) {
    // FIXME: thumb shouldn't be sent here.
    // This is a nasty hack.
    // Better save thumb independently, bypassing this.
    let thumb = thumb as u64;
    thread::spawn(move || {
        post_process_task(
            processing_script,
            burst_dir,
            capture_fname,
            thumb,
        )
    });
}
    
fn post_process_task(
    processing_script: String,
    burst_dir: String,
    capture_fname: String,
    thumb: u64,
) {
    let thumb = thumb as *const c_void;

    let output = process::Command::new(processing_script)
        .arg(burst_dir)
        .arg(capture_fname)
        .output();

    let line = match output {
        Ok(output) => {
            let stdout = output.stdout;
            let mut last_line = find_last_line(&stdout)
                .map(Vec::from)
                .unwrap_or(Vec::new());
            last_line.push(0); // terminating byte, just in case
            Some(last_line)
        },
        Err(e) => {
            eprintln!("Failed to execute post-processor: {}", e);
            None
        },
    };

    match line {
        Some(line) => unsafe { c::mp_main_capture_completed(thumb, line.as_ptr() as _) },
        None => unsafe { c::notify_processing_finished(thumb) },
    }
}
