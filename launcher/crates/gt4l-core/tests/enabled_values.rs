//! Every enabled gt4-gs value (TASK-088, 1.2.0) round-trips into the gt4-gs.ini the launcher writes, and the value is
//! one the settings allow-list accepts (the same allow-list gt4-gs embeds). Values that stay disabled are listed in
//! the table with a reason and are not tested here.

use std::collections::BTreeMap;

use gt4l_core::gs_table::{self, Row};

fn allow_list() -> serde_json::Value {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../docs/launcher/gs-settings-allowlist.json");
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}

fn allowed_values(section: &str, key: &str) -> Option<Vec<String>> {
    let list = allow_list();
    let entry = list["entries"]
        .as_array()
        .unwrap()
        .iter()
        .find(|entry| entry["section"] == section && entry["key"] == key)?;
    Some(entry["values"].as_array()?.iter().map(|value| value.as_str().unwrap().to_string()).collect())
}

#[test]
fn every_enabled_choice_round_trips_into_the_ini_and_is_allowed() {
    let mut checked = 0;
    for row in gs_table::offered() {
        if row.kind != "enum" || row.forced || row.section.is_empty() {
            continue;
        }
        let allowed = allowed_values(&row.section, &row.key)
            .unwrap_or_else(|| panic!("{} has no allow-list entry", row.id));
        for choice in row.choices.iter().filter(|choice| choice.status == "ok") {
            let settings = BTreeMap::from([(row.id.clone(), choice.name.clone())]);
            let text = gs_table::render_ini(&settings, &BTreeMap::new()).unwrap();
            let needle = format!("\n{} = {}\n", row.key, choice.ini);
            // "always" rows are written every time; "when-set" rows only when the value differs from the default.
            let written = row.written == "always" || choice.name != row.default;
            if written {
                assert!(text.contains(&needle), "{} = {} is not written: {text}", row.id, choice.name);
            } else {
                assert!(!text.contains(&format!("
{} = ", row.key)), "{} default is written: {text}", row.id);
            }
            assert!(allowed.contains(&choice.ini), "{}: {} is not in the allow-list", row.id, choice.ini);
            checked += 1;
        }
    }
    // A smoke floor, not a spec: TASK-118 cut the Advanced page down to a handful of rows, so only the choices that are
    // still offered are counted. It only has to catch a table that is suddenly empty or unparsed.
    assert!(checked >= 5, "only {checked} enabled choices checked");
}

#[test]
fn every_enabled_bool_and_int_row_round_trips() {
    for row in gs_table::offered() {
        if row.section.is_empty() || row.forced || row.written == "launcher" {
            continue;
        }
        match row.kind.as_str() {
            "bool" => {
                let allowed = allowed_values(&row.section, &row.key).unwrap_or_else(|| panic!("{}", row.id));
                assert_eq!(allowed, vec!["true".to_string(), "false".to_string()], "{}", row.id);
                let other = if row.default == "true" { "false" } else { "true" };
                let text = gs_table::render_ini(&BTreeMap::from([(row.id.clone(), other.to_string())]), &BTreeMap::new()).unwrap();
                assert!(text.contains(&format!("{} = {other}\n", row.key)), "{}: {text}", row.id);
            }
            "int" => {
                let allowed = allow_list();
                let entry = allowed["entries"].as_array().unwrap().iter().find(|entry| entry["key"] == row.key.as_str());
                assert!(entry.is_some(), "{} has no allow-list entry", row.id);
            }
            _ => {}
        }
    }
}

#[test]
fn the_row_helpers_see_the_enabled_values() {
    let blending: &Row = gs_table::row("blending").unwrap();
    assert_eq!(gs_table::offered_choices(blending), vec!["high", "minimum"]);
    let graphics_api = gs_table::row("graphics_api").unwrap();
    assert_eq!(gs_table::offered_choices(graphics_api), vec!["auto", "vulkan", "d3d12", "d3d11"]);
    assert!(gs_table::offered().iter().all(|row| row.page == "basic" || row.page == "advanced"));
}
