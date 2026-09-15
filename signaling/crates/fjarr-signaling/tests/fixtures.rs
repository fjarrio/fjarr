//! Protocol conformance: every valid golden fixture MUST deserialize into
//! the Rust types, and round-trip back to JSON that still carries the
//! discriminating fields. Keeps protocol.rs honest against
//! protocol/schemas/ (docs/08#versioning, docs/15 conformance).

use fjarr_signaling::protocol::Message;

fn fixtures_dir(sub: &str) -> std::path::PathBuf {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../protocol/fixtures")
        .join(sub)
}

#[test]
fn every_valid_signaling_fixture_deserializes() {
    let dir = fixtures_dir("valid");
    let mut checked = 0;
    for entry in std::fs::read_dir(&dir).expect("fixtures dir") {
        let path = entry.unwrap().path();
        let name = path.file_name().unwrap().to_string_lossy().to_string();
        if !name.starts_with("sig-") {
            continue;
        }
        let text = std::fs::read_to_string(&path).unwrap();
        let msg: Message =
            serde_json::from_str(&text).unwrap_or_else(|e| panic!("{name} must deserialize: {e}"));

        // Round-trip: type tag and common fields survive.
        let back = serde_json::to_value(&msg).unwrap();
        let original: serde_json::Value = serde_json::from_str(&text).unwrap();
        assert_eq!(back["type"], original["type"], "{name}: type tag drifted");
        assert_eq!(
            back["event_id"], original["event_id"],
            "{name}: event_id drifted"
        );
        checked += 1;
    }
    assert!(
        checked >= 14,
        "expected the full fixture set, found {checked}"
    );
}

#[test]
fn structurally_invalid_fixtures_fail_to_deserialize() {
    // Serde enforces structure (missing/mistyped required fields), not
    // value constraints — those are the JSON Schema's job (make
    // protocol-check). Only structural fixtures apply here.
    for name in [
        "sig-hello-missing-event-id.json",
        "sig-ice-missing-mline.json",
        "sig-offer-pt-as-string.json",
    ] {
        let text = std::fs::read_to_string(fixtures_dir("invalid").join(name)).unwrap();
        assert!(
            serde_json::from_str::<Message>(&text).is_err(),
            "{name} must be rejected"
        );
    }
}
