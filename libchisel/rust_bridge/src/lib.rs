use std::ffi::CStr;
use std::fs::File;
use std::os::raw::{c_char, c_int};

use optivorbis::{OggToOgg, Remuxer};

#[no_mangle]
pub extern "C" fn chisel_optimize_vorbis(
    input_path: *const c_char,
    output_path: *const c_char
) -> c_int {
    if input_path.is_null() || output_path.is_null() { return -1; }

    let input_c = unsafe { CStr::from_ptr(input_path) };
    let output_c = unsafe { CStr::from_ptr(output_path) };

    let input_str = match input_c.to_str() {
        Ok(s) => s,
        Err(_) => return -2,
    };
    let output_str = match output_c.to_str() {
        Ok(s) => s,
        Err(_) => return -2,
    };

    let mut input_file = match File::open(input_str) {
        Ok(f) => f,
        Err(_) => return -3,
    };

    let mut output_file = match File::create(output_str) {
        Ok(f) => f,
        Err(_) => return -4,
    };

    let remuxer = OggToOgg::new_with_defaults();

    match remuxer.remux(&mut input_file, &mut output_file) {
        Ok(_) => 0,
        Err(_) => -5
    }
}