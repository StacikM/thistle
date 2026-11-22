//! Unity's YAML: a small, regular subset. Block mappings and sequences (a sequence's dashes sit at its key's
//! indent), flow `{a: 1, b: {c: 2}}` and `[]`, plain or quoted scalars. That's all this reads; a general YAML
//! library would be a dependency that still couldn't do `--- !u!1 &123` documents. Ported from
//! tools/thistle-cli/unity_import.py, which the tests hold this to.
use std::collections::HashMap;

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Null,
    Int(i64),
    Float(f64),
    Str(String),
    Map(Vec<(String, Value)>),
    List(Vec<Value>),
}

impl Value {
    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Map(m) => m.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }
    pub fn f64(&self) -> Option<f64> {
        match self {
            Value::Int(i) => Some(*i as f64),
            Value::Float(f) => Some(*f),
            Value::Str(s) => s.trim().parse().ok(),
            _ => None,
        }
    }
    pub fn i64(&self) -> Option<i64> {
        match self {
            Value::Int(i) => Some(*i),
            Value::Float(f) => Some(*f as i64),
            Value::Str(s) => s.trim().parse().ok(),
            _ => None,
        }
    }
    pub fn str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s),
            _ => None,
        }
    }
    pub fn list(&self) -> &[Value] {
        match self {
            Value::List(l) => l,
            _ => &[],
        }
    }
    pub fn entries(&self) -> &[(String, Value)] {
        match self {
            Value::Map(m) => m,
            _ => &[],
        }
    }
    pub fn is_map(&self) -> bool {
        matches!(self, Value::Map(_))
    }
    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }
    /// Python-style truthiness: 0, 0.0, "", empty, null are false.
    pub fn truthy(&self) -> bool {
        match self {
            Value::Null => false,
            Value::Int(i) => *i != 0,
            Value::Float(f) => *f != 0.0,
            Value::Str(s) => !s.is_empty(),
            Value::Map(m) => !m.is_empty(),
            Value::List(l) => !l.is_empty(),
        }
    }
    /// What `str(value)` gives in Python, for property values.
    pub fn to_text(&self) -> String {
        match self {
            Value::Null => "None".into(),
            Value::Int(i) => i.to_string(),
            Value::Float(f) => py_float(*f),
            Value::Str(s) => s.clone(),
            _ => String::new(),
        }
    }
}

/// Python's repr for a float: 1.0, 0.5, 1e-05, 1e+16.
pub fn py_float(f: f64) -> String {
    if f.is_nan() {
        return "nan".into();
    }
    if f.is_infinite() {
        return if f > 0.0 { "inf".into() } else { "-inf".into() };
    }
    if f == 0.0 {
        return if f.is_sign_negative() { "-0.0".into() } else { "0.0".into() };
    }
    let abs = f.abs();
    if (1e-4..1e16).contains(&abs) {
        let s = format!("{}", f);
        if s.contains('.') { s } else { format!("{}.0", s) }
    } else {
        let s = format!("{:e}", f); // 1e-5, 1.5e16
        let (mant, exp) = s.split_once('e').unwrap();
        let e: i32 = exp.parse().unwrap();
        format!("{}e{}{:02}", mant, if e < 0 { '-' } else { '+' }, e.abs())
    }
}

pub struct UnityObject {
    pub class_id: i32,
    pub file_id: i64,
    pub type_name: String,
    pub data: Value,
    pub stripped: bool,
}

/// Every `--- !u!<class> &<fileID>` document of a file, in file order.
pub struct Doc {
    pub objs: Vec<UnityObject>,
    by_id: HashMap<i64, usize>,
}

impl Doc {
    pub fn get(&self, id: i64) -> Option<&UnityObject> {
        self.by_id.get(&id).map(|&i| &self.objs[i])
    }
}

const STRING_KEYS: [&str; 3] = ["guid", "_typelessdata", "m_IndexBuffer"];

fn is_int(t: &str) -> bool {
    let d = t.strip_prefix('-').unwrap_or(t);
    !d.is_empty() && d.bytes().all(|b| b.is_ascii_digit())
}

fn is_float(t: &str) -> bool {
    let first = t.as_bytes().first().copied().unwrap_or(b' ');
    (first.is_ascii_digit() || first == b'.' || first == b'-')
        && t.bytes().all(|b| b.is_ascii_digit() || b == b'.' || b == b'e' || b == b'E' || b == b'+' || b == b'-')
        && t.parse::<f64>().is_ok()
}

fn scalar(text: &str, key: &str) -> Value {
    let t = text.trim();
    let b = t.as_bytes();
    if b.len() >= 2 && b[0] == b'\'' && b[b.len() - 1] == b'\'' {
        return Value::Str(t[1..t.len() - 1].replace("''", "'"));
    }
    if b.len() >= 2 && b[0] == b'"' && b[b.len() - 1] == b'"' {
        return match serde_json::from_str::<String>(t) {
            Ok(s) => Value::Str(s),
            Err(_) => Value::Str(t[1..t.len() - 1].to_string()),
        };
    }
    if STRING_KEYS.contains(&key) {
        return Value::Str(t.to_string());
    }
    if is_int(t) {
        return match t.parse::<i64>() {
            Ok(i) => Value::Int(i),
            Err(_) => Value::Float(t.parse().unwrap_or(0.0)),
        };
    }
    if is_float(t) {
        return Value::Float(t.parse().unwrap_or(0.0));
    }
    if t == "~" || t == "null" {
        return Value::Null;
    }
    Value::Str(t.to_string())
}

/// Splits a flow collection's inside on the commas that aren't nested or quoted.
fn split_top(text: &str) -> Vec<&str> {
    let mut parts = Vec::new();
    let (mut depth, mut quote, mut start) = (0i32, 0u8, 0usize);
    for (i, ch) in text.bytes().enumerate() {
        if quote != 0 {
            if ch == quote {
                quote = 0;
            }
        } else if ch == b'\'' || ch == b'"' {
            quote = ch;
        } else if ch == b'{' || ch == b'[' {
            depth += 1;
        } else if ch == b'}' || ch == b']' {
            depth -= 1;
        } else if ch == b',' && depth == 0 {
            parts.push(&text[start..i]);
            start = i + 1;
        }
    }
    let tail = &text[start..];
    if !tail.trim().is_empty() {
        parts.push(tail);
    }
    parts
}

/// 'key: value' -> (key, value); None when it isn't a mapping entry.
fn key_split(text: &str) -> Option<(&str, &str)> {
    let b = text.as_bytes();
    let mut quote = 0u8;
    for i in 0..b.len() {
        let ch = b[i];
        if quote != 0 {
            if ch == quote {
                quote = 0;
            }
        } else if ch == b'\'' || ch == b'"' {
            quote = ch;
        } else if ch == b':' && (i + 1 == b.len() || b[i + 1] == b' ') {
            return Some((text[..i].trim(), text[i + 1..].trim()));
        } else if (ch == b'{' || ch == b'[') && i == 0 {
            return None;
        }
    }
    None
}

fn flow(text: &str, key: &str) -> Value {
    let t = text.trim();
    if t.starts_with('{') && t.ends_with('}') {
        let mut out = Vec::new();
        for part in split_top(&t[1..t.len() - 1]) {
            if let Some((k, v)) = key_split(part.trim()) {
                out.push((k.to_string(), flow(v, k)));
            }
        }
        return Value::Map(out);
    }
    if t.starts_with('[') && t.ends_with(']') {
        return Value::List(split_top(&t[1..t.len() - 1]).into_iter().map(|p| flow(p, "")).collect());
    }
    scalar(t, key)
}

type Lines<'a> = Vec<(usize, &'a str)>;

fn is_dash(text: &str) -> bool {
    text == "-" || text.starts_with("- ")
}

fn block(lines: &mut Lines, i: usize, indent: usize) -> (Value, usize) {
    let text = lines[i].1;
    if is_dash(text) {
        return sequence(lines, i, indent);
    }
    if key_split(text).is_some() {
        return mapping(lines, i, indent);
    }
    (flow(text, ""), i + 1)
}

fn mapping(lines: &mut Lines, mut i: usize, indent: usize) -> (Value, usize) {
    let mut out = Vec::new();
    while i < lines.len() && lines[i].0 == indent && !is_dash(lines[i].1) {
        let Some((key, value)) = key_split(lines[i].1) else {
            i += 1;
            continue;
        };
        i += 1;
        if !value.is_empty() {
            out.push((key.to_string(), flow(value, key)));
        } else if i < lines.len() && (lines[i].0 > indent || (lines[i].0 == indent && is_dash(lines[i].1))) {
            let ind = lines[i].0;
            let (v, next) = block(lines, i, ind);
            out.push((key.to_string(), v));
            i = next;
        } else {
            out.push((key.to_string(), Value::Null));
        }
    }
    (Value::Map(out), i)
}

fn sequence(lines: &mut Lines, mut i: usize, indent: usize) -> (Value, usize) {
    let mut out = Vec::new();
    while i < lines.len() && lines[i].0 == indent && is_dash(lines[i].1) {
        let rest = lines[i].1[1..].trim();
        if rest.is_empty() {
            i += 1;
            if i < lines.len() && lines[i].0 > indent {
                let ind = lines[i].0;
                let (v, next) = block(lines, i, ind);
                out.push(v);
                i = next;
            } else {
                out.push(Value::Null);
            }
        } else if !rest.starts_with('{') && !rest.starts_with('[') && key_split(rest).is_some() {
            // "- key: value" starts a mapping whose later keys line up with `key`.
            lines[i] = (indent + 2, rest);
            let (v, next) = mapping(lines, i, indent + 2);
            out.push(v);
            i = next;
        } else {
            out.push(flow(rest, ""));
            i += 1;
        }
    }
    (Value::List(out), i)
}

/// `--- !u!<class> &<fileID>[ stripped]` -> (class, fileID, stripped)
fn header(line: &str) -> Option<(i32, i64, bool)> {
    let rest = line.strip_prefix("--- !u!")?;
    let (class, rest) = rest.split_once(" &")?;
    let class: i32 = class.parse().ok()?;
    let rest = rest.trim_end();
    let (id, tail) = match rest.split_once(' ') {
        Some((id, tail)) => (id, tail.trim()),
        None => (rest, ""),
    };
    Some((class, id.parse().ok()?, tail == "stripped"))
}

pub fn parse_unity_yaml(text: &str) -> Doc {
    let mut objs: Vec<UnityObject> = Vec::new();
    let mut by_id: HashMap<i64, usize> = HashMap::new();
    let mut current: Option<(i32, i64, bool)> = None;
    let mut body_start = 0usize;
    let mut finish = |current: Option<(i32, i64, bool)>, body: &str, objs: &mut Vec<UnityObject>, by_id: &mut HashMap<i64, usize>| {
        let Some((class_id, file_id, stripped)) = current else { return };
        let mut lines: Lines = Vec::new();
        for raw in body.lines() {
            let trimmed = raw.trim();
            if trimmed.is_empty() || trimmed.starts_with('#') {
                continue;
            }
            let indent = raw.len() - raw.trim_start_matches(' ').len();
            lines.push((indent, trimmed));
        }
        if lines.is_empty() {
            return;
        }
        let first_indent = lines[0].0;
        let (top, _) = block(&mut lines, 0, first_indent);
        if let Value::Map(mut m) = top {
            if m.len() == 1 {
                let (type_name, data) = m.pop().unwrap();
                let data = if data.is_null() { Value::Map(Vec::new()) } else { data };
                by_id.entry(file_id).or_insert(objs.len());
                objs.push(UnityObject { class_id, file_id, type_name, data, stripped });
            }
        }
    };
    let mut pos = 0usize;
    for line in text.split_inclusive('\n') {
        let bare = line.trim_end_matches(['\n', '\r']);
        if bare.starts_with("--- !u!") {
            if let Some(h) = header(bare) {
                finish(current, &text[body_start..pos], &mut objs, &mut by_id);
                current = Some(h);
                body_start = pos + line.len();
            }
        }
        pos += line.len();
    }
    finish(current, &text[body_start..], &mut objs, &mut by_id);
    Doc { objs, by_id }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn guid_and_hex_stay_strings() {
        let v = flow("{fileID: 10202, guid: 0000000000000000e000000000000000, type: 0}", "");
        assert_eq!(v.get("fileID"), Some(&Value::Int(10202)));
        assert_eq!(v.get("guid"), Some(&Value::Str("0000000000000000e000000000000000".into())));
    }

    #[test]
    fn flow_floats_and_empties() {
        let v = flow("{x: -1.5e-05, y: 2, z: 0}", "");
        assert_eq!(v.get("x"), Some(&Value::Float(-1.5e-05)));
        assert_eq!(flow("[]", ""), Value::List(vec![]));
        assert_eq!(flow("{}", ""), Value::Map(vec![]));
    }

    #[test]
    fn documents_sequences_and_big_ids() {
        let text = "%YAML 1.1\n--- !u!1 &8311483215443558260\nGameObject:\n  m_Component:\n  - component: {fileID: 11}\n  - component: {fileID: 12}\n  m_Name: Sun Light\n--- !u!4 &5 stripped\nTransform:\n  m_PrefabInstance: {fileID: 7}\n";
        let doc = parse_unity_yaml(text);
        assert_eq!(doc.objs.len(), 2);
        let go = doc.get(8311483215443558260).unwrap();
        assert_eq!(go.class_id, 1);
        assert_eq!(go.data.get("m_Name").and_then(|v| v.str()), Some("Sun Light"));
        assert_eq!(go.data.get("m_Component").unwrap().list().len(), 2);
        assert!(doc.get(5).unwrap().stripped);
    }

    #[test]
    fn python_float_text() {
        assert_eq!(py_float(1.0), "1.0");
        assert_eq!(py_float(0.0315136), "0.0315136");
        assert_eq!(py_float(1e-5), "1e-05");
        assert_eq!(py_float(-2.5), "-2.5");
    }
}
