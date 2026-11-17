// SPDX-License-Identifier: MIT
// Starbite telemetry of the native Metal bridge, part of NativeMetalObjects.rs (include!d there; a file of its own for the size
// rule): the runtime's first distinct messages and the device's work counts with the render-pass-instance split counters
// (patches/0103-sweet-instance-split-counters.patch), which the plug-in reports (HotMetal2.mm, /var/local/mtl2-telemetry).
// Recorded 18 Nov 2026, unchanged, from the tree the bundles' libnative_metal_bridge.a was built from on 12 Nov.

// The first 300 distinct messages of the process (but the pipeline log), kept until the plug-in takes them for its telemetry
// (`nmb_take_messages`): what the runtime announces once, a degradation among them, is otherwise seen by nobody on a phone.
static KEPT_MESSAGES: std::sync::Mutex<(Vec<String>, Option<std::collections::HashSet<String>>)> = std::sync::Mutex::new((Vec::new(), None));

fn keep_message(message: &DebugMessage) {
    let Ok(mut kept) = KEPT_MESSAGES.lock() else { return };
    let (pending, seen) = &mut *kept;
    let seen = seen.get_or_insert_with(std::collections::HashSet::new);
    let text: String = message.text.chars().take(400).collect();
    let key = format!("{:?} {} {}", message.severity, message.id_name, text);
    if seen.len() < 300 && seen.insert(key.clone()) {
        pending.push(key);
    }
}

/// The oldest kept message not yet taken, as a C string; returns its length, 0 when there is none.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_take_message(out: *mut c_char, size: usize) -> usize {
    guard(0, || {
        let Ok(mut kept) = KEPT_MESSAGES.lock() else { return Ok(0) };
        if kept.0.is_empty() {
            return Ok(0);
        }
        let text = kept.0.remove(0);
        Ok(copy_out(&text, out, size))
    })
}

/// The device's work counts that are not zero (`Device::work_counts`), as "name=value name=value ..."; returns the length.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_work_counts_text(device: *const NmbDevice, out: *mut c_char, size: usize) -> usize {
    guard(0, || {
        let counts = format!("{:?}", get(device)?.0.work_counts());
        let inner = counts.trim_start_matches("WorkCounts {").trim_end_matches('}');
        let mut text = String::new();
        for field in inner.split(',') {
            let Some((name, value)) = field.split_once(':') else { continue };
            let value = value.trim();
            if value != "0" && !value.is_empty() {
                if !text.is_empty() {
                    text.push(' ');
                }
                text.push_str(name.trim());
                text.push('=');
                text.push_str(value);
            }
        }
        use std::sync::atomic::Ordering::Relaxed;
        let names = ["stand_in", "storage", "locations", "reformats", "shadow", "shader_tests", "views", "dispatched", "barrier", "ordered", "all", "with_draws"];
        for (name, count) in names.iter().zip(metal2vulkan_runtime::INSTANCE_SPLITS.iter()) {
            let count = count.load(Relaxed);
            if count != 0 {
                text.push_str(&format!(" new_instance_{name}={count}"));
            }
        }
        Ok(copy_out(&text, out, size))
    })
}
