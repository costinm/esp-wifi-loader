use std::{env, fs, path::PathBuf};
fn main() {
    println!("cargo:rerun-if-env-changed=ESPMOD_SLOT_LAYOUT");
    println!("cargo:rerun-if-env-changed=ESPMOD_SLOT");
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("layout.rs");
    if let Some(path) = env::var_os("ESPMOD_SLOT_LAYOUT") {
        println!("cargo:rerun-if-changed={}", PathBuf::from(&path).display());
        let selected = env::var("ESPMOD_SLOT").expect("module slot");
        let input = fs::read_to_string(path).unwrap();
        let mut tag = None;
        let mut slots = String::new();
        for (index, line) in input.lines().enumerate() {
            assert!(index < 8, "loader state log has eight module version entries");
            let columns: Vec<_> = line.split('\t').collect();
            assert_eq!(columns.len(), 4);
            let name = columns[0];
            assert!(name.len() <= 16);
            let numbers: Vec<u32> = columns[1..].iter().map(|n| n.parse().unwrap()).collect();
            if name == selected { tag = Some(numbers[0]); }
            let mut bytes = [0u8; 16];
            bytes[..name.len()].copy_from_slice(name.as_bytes());
            slots.push_str(&format!("Slot{{name:{bytes:?},name_len:{},tag:{},index:{index},offset:{},size:{}}},\n",
                                   name.len(), numbers[0], numbers[1], numbers[2]));
        }
        fs::write(out, format!("const MODULE_TAG:i32={};\nconst SLOTS:&[Slot]=&[{slots}];\n", tag.expect("slot in layout"))).unwrap();
    } else {
        fs::write(out,"const MODULE_TAG:i32=44; const SLOTS:&[Slot]=&[Slot{name:[116,101,115,116,0,0,0,0,0,0,0,0,0,0,0,0],name_len:4,tag:2,index:0,offset:0,size:65536}];").unwrap();
    }
}
