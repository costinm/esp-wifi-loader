#![no_std]
#![no_main]
espmod_abi::export_module!(esp_mod_flash::State, state = 32);

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}
