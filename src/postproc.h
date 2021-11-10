/// This calls out to Rust because the glib version fails to reap processes sometimes,
/// causing the spinner to spin forever.
/// I decided it's easiest to solve the race in Rust
/// after considering debugging glib or rolling out a raw threading solution.
void spawn_post_process_task(char processing_script[256], char burst_dir[23], char capture_fname[255], void *thumb);
