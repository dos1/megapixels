/*! Hack to prevent the focus mechanism from resetting.
This is done in Rust rather than C because of easier string/process handling.*/

pub mod c {
    use super::*;

    use std::ffi:: OsStr;
    use std::fs::File;
    use std::os::raw::c_int;
    use std::os::unix::ffi::OsStrExt;
    use std::os::unix::io::IntoRawFd;
    use std::process::Command;

    /// This will crash the application if anything goes wrong.
    /// Not sure if this is the best idea,
    /// but also that's rather unusual, given that the kernel would be at fault.
    #[no_mangle]
    pub extern "C" fn open_focus_fd() -> c_int {
        let output = Command::new("media-ctl")
            .args(&["-d", "platform:30b80000.csi", "-e", "dw9714 3-000c"])
            .output()
            .expect("Failed to find the focus subdevice");
        let stdout = output.stdout;
        let stripped_stdout = strip_newline(&stdout);
        let subdevice_path = OsStr::from_bytes(stripped_stdout);
        let subdevice = File::open(subdevice_path).unwrap();
        let fd = subdevice.into_raw_fd();
        fd
    }
    // closing the focus fd can be done in C using a simple close(fd) call.
    
    #[no_mangle]
    pub extern "C" fn set_focus(absolute: i32) {
        // We don't really care about the result above printing the message.
        let _ = Command::new("v4l2-ctl")
            .args(&[
                "--media-bus-info", "platform:30b80000.csi",
                "--device", "dw9714 3-000c",
                "--set-ctrl", &format!("focus_absolute={}", absolute),
            ])
            .spawn()
            .map_err(|e| eprintln!("Failed to set focus: {:?}", e));
    }
}

const NEWLINE: u8 = 10;

fn strip_newline(v: &[u8]) -> &[u8] {
    let last = v.iter()
        .rposition(|c| *c != NEWLINE)
        .unwrap_or(v.len() - 1);
    &v[..(last + 1)]
}
