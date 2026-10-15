// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#[no_mangle]
pub extern "C" fn pf_hello() -> u32 {
    let v: Vec<u32> = (1..=10).collect();
    let sum: u32 = v.iter().sum();
    println!("hello from Rust std on Horizon: sum={sum}");
    sum
}
