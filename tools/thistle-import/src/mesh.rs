//! A Unity Mesh saved as an .asset (MapForge and other tools bake meshes this way), decoded to Wavefront OBJ text.
//! Z is mirrored and every triangle's winding reversed with it, so the mesh sits in Thistle's right-handed space the
//! way the scene around it does. Ported from unity_import.py's mesh_asset_to_obj.
use crate::yaml::Value;

fn format_size(format: i64) -> usize {
    match format {
        0 | 10 | 11 => 4,
        1 | 4 | 5 | 8 | 9 => 2,
        _ => 1,
    }
}

fn half_to_f32(h: u16) -> f32 {
    let sign = ((h >> 15) & 1) as u32;
    let exp = ((h >> 10) & 0x1f) as u32;
    let frac = (h & 0x3ff) as u32;
    let bits = if exp == 0 {
        if frac == 0 {
            sign << 31
        } else {
            // a subnormal half: normalize it
            let mut e = 127 - 15 + 1;
            let mut f = frac;
            while f & 0x400 == 0 {
                f <<= 1;
                e -= 1;
            }
            (sign << 31) | ((e as u32) << 23) | ((f & 0x3ff) << 13)
        }
    } else if exp == 31 {
        (sign << 31) | (0xff << 23) | (frac << 13)
    } else {
        (sign << 31) | ((exp + 127 - 15) << 23) | (frac << 13)
    };
    f32::from_bits(bits)
}

fn decode(raw: &[u8], at: usize, format: i64, dim: usize, out: &mut [f32]) -> Result<(), String> {
    let size = format_size(format);
    if at + size * dim > raw.len() {
        return Err("its vertex data is shorter than its vertex count".into());
    }
    for k in 0..dim {
        let p = at + k * size;
        out[k] = match format {
            0 => f32::from_le_bytes([raw[p], raw[p + 1], raw[p + 2], raw[p + 3]]),
            1 => half_to_f32(u16::from_le_bytes([raw[p], raw[p + 1]])),
            2 => raw[p] as f32 / 255.0,
            3 => (raw[p] as i8 as f32 / 127.0).max(-1.0),
            4 => u16::from_le_bytes([raw[p], raw[p + 1]]) as f32 / 65535.0,
            5 => (i16::from_le_bytes([raw[p], raw[p + 1]]) as f32 / 32767.0).max(-1.0),
            _ => return Err(format!("vertex format {} isn't supported", format)),
        };
    }
    Ok(())
}

pub fn hex_decode(s: &str) -> Result<Vec<u8>, String> {
    let b = s.trim().as_bytes();
    if b.len() % 2 != 0 {
        return Err("a hex dump has an odd length".into());
    }
    let nib = |c: u8| -> Result<u8, String> {
        match c {
            b'0'..=b'9' => Ok(c - b'0'),
            b'a'..=b'f' => Ok(c - b'a' + 10),
            b'A'..=b'F' => Ok(c - b'A' + 10),
            _ => Err("a hex dump has a character that isn't hex".into()),
        }
    };
    let mut out = Vec::with_capacity(b.len() / 2);
    for pair in b.chunks(2) {
        out.push(nib(pair[0])? << 4 | nib(pair[1])?);
    }
    Ok(out)
}

fn f(v: &str) -> String {
    // like Python's "%.7g", with -0 written as 0
    let x: f64 = v.parse().unwrap_or(0.0);
    let x = if x == 0.0 { 0.0 } else { x };
    fmt_g7(x)
}

pub fn fmt_g7(x: f64) -> String {
    if x == 0.0 {
        return "0".into();
    }
    let s = format!("{:.6e}", x); // d.dddddde[-]x : 7 significant digits
    let (mant, exp) = s.split_once('e').unwrap();
    let e: i32 = exp.parse().unwrap();
    if !(-4..7).contains(&e) {
        let mant = mant.trim_end_matches('0').trim_end_matches('.');
        return format!("{}e{}{:02}", mant, if e < 0 { '-' } else { '+' }, e.abs());
    }
    let decimals = (6 - e).max(0) as usize;
    let fixed = format!("{:.*}", decimals, x);
    if fixed.contains('.') {
        fixed.trim_end_matches('0').trim_end_matches('.').to_string()
    } else {
        fixed
    }
}

/// (OBJ text, mesh name), or why this mesh can't be converted.
pub fn mesh_asset_to_obj(mesh: &Value) -> Result<(String, String), String> {
    if mesh.get("m_MeshCompression").map(|v| v.truthy()).unwrap_or(false) {
        return Err("it uses Unity's mesh compression".into());
    }
    if mesh.get("m_StreamData").and_then(|s| s.get("size")).map(|v| v.truthy()).unwrap_or(false) {
        return Err("its vertex data is stored in a separate .resS file".into());
    }
    let vd = mesh.get("m_VertexData").ok_or("it has no vertex data")?;
    let count = vd.get("m_VertexCount").and_then(|v| v.i64()).unwrap_or(0).max(0) as usize;
    let channels = vd.get("m_Channels").map(|c| c.list()).unwrap_or(&[]);
    let raw = hex_decode(vd.get("_typelessdata").and_then(|v| v.str()).unwrap_or(""))?;
    if count == 0 || raw.is_empty() {
        return Err("it has no vertices".into());
    }
    // Each stream holds some channels back to back; streams follow each other, padded to 16 bytes.
    let mut stride: Vec<(i64, usize)> = Vec::new();
    for ch in channels {
        let dim = (ch.get("dimension").and_then(|v| v.i64()).unwrap_or(0) & 0xF) as usize;
        if dim > 0 {
            let stream = ch.get("stream").and_then(|v| v.i64()).unwrap_or(0);
            let fmt = ch.get("format").and_then(|v| v.i64()).unwrap_or(0);
            let end = ch.get("offset").and_then(|v| v.i64()).unwrap_or(0) as usize + dim * format_size(fmt);
            match stride.iter_mut().find(|(s, _)| *s == stream) {
                Some(entry) => entry.1 = entry.1.max(end),
                None => stride.push((stream, end)),
            }
        }
    }
    stride.sort();
    let mut start: Vec<(i64, usize)> = Vec::new();
    let mut at = 0usize;
    for (stream, st) in &stride {
        start.push((*stream, at));
        at += (count * st + 15) / 16 * 16;
    }
    let lookup = |list: &Vec<(i64, usize)>, s: i64| list.iter().find(|(k, _)| *k == s).map(|(_, v)| *v).unwrap_or(0);

    let attribute = |index: usize, want: usize| -> Result<Option<Vec<f32>>, String> {
        let Some(ch) = channels.get(index) else { return Ok(None) };
        let dim = (ch.get("dimension").and_then(|v| v.i64()).unwrap_or(0) & 0xF) as usize;
        if dim < want {
            return Ok(None);
        }
        let stream = ch.get("stream").and_then(|v| v.i64()).unwrap_or(0);
        let fmt = ch.get("format").and_then(|v| v.i64()).unwrap_or(0);
        let base = lookup(&start, stream) + ch.get("offset").and_then(|v| v.i64()).unwrap_or(0) as usize;
        let step = lookup(&stride, stream);
        let mut out = vec![0.0f32; count * want];
        let mut tmp = vec![0.0f32; dim.max(want)];
        for i in 0..count {
            decode(&raw, base + i * step, fmt, dim, &mut tmp)?;
            out[i * want..(i + 1) * want].copy_from_slice(&tmp[..want]);
        }
        Ok(Some(out))
    };
    let positions = attribute(0, 3)?.ok_or("it has no vertex positions")?;
    let normals = attribute(1, 3)?;
    let uvs = attribute(4, 2)?;

    let index_raw = hex_decode(mesh.get("m_IndexBuffer").and_then(|v| v.str()).unwrap_or(""))?;
    let wide = mesh.get("m_IndexFormat").and_then(|v| v.i64()).unwrap_or(0) == 1;
    let unit = if wide { 4 } else { 2 };
    let mut triangles: Vec<[usize; 3]> = Vec::new();
    for sub in mesh.get("m_SubMeshes").map(|s| s.list()).unwrap_or(&[]) {
        if sub.get("topology").and_then(|v| v.i64()).unwrap_or(0) != 0 {
            return Err("it has a submesh that isn't made of triangles".into());
        }
        let first = sub.get("firstByte").and_then(|v| v.i64()).unwrap_or(0) as usize;
        let n = sub.get("indexCount").and_then(|v| v.i64()).unwrap_or(0) as usize;
        let base = sub.get("baseVertex").and_then(|v| v.i64()).unwrap_or(0);
        if first + n * unit > index_raw.len() {
            return Err("its indices don't match its vertices".into());
        }
        let read = |k: usize| -> i64 {
            let p = first + k * unit;
            if wide {
                u32::from_le_bytes([index_raw[p], index_raw[p + 1], index_raw[p + 2], index_raw[p + 3]]) as i64
            } else {
                u16::from_le_bytes([index_raw[p], index_raw[p + 1]]) as i64
            }
        };
        let mut k = 0;
        while k + 2 < n {
            triangles.push([(read(k) + base) as usize, (read(k + 1) + base) as usize, (read(k + 2) + base) as usize]);
            k += 3;
        }
    }
    if triangles.is_empty() || triangles.iter().any(|t| t.iter().any(|&i| i >= count)) {
        return Err("its indices don't match its vertices".into());
    }

    let name = mesh.get("m_Name").and_then(|v| v.str()).filter(|s| !s.is_empty()).unwrap_or("mesh").to_string();
    let mut out = String::with_capacity(count * 60 + triangles.len() * 40);
    out.push_str(&format!("# from Unity mesh {} by thistle import-unity (Z mirrored, winding reversed)\no {}\n", name, name));
    let g = |x: f32| f(&format!("{}", x));
    for p in positions.chunks(3) {
        out.push_str(&format!("v {} {} {}\n", g(p[0]), g(p[1]), g(-p[2])));
    }
    if let Some(n) = &normals {
        for p in n.chunks(3) {
            out.push_str(&format!("vn {} {} {}\n", g(p[0]), g(p[1]), g(-p[2])));
        }
    }
    if let Some(t) = &uvs {
        for p in t.chunks(2) {
            out.push_str(&format!("vt {} {}\n", g(p[0]), g(p[1])));
        }
    }
    let corner = |i: usize| -> String {
        let n = i + 1;
        match (normals.is_some(), uvs.is_some()) {
            (true, true) => format!("{n}/{n}/{n}"),
            (true, false) => format!("{n}//{n}"),
            (false, true) => format!("{n}/{n}"),
            (false, false) => format!("{n}"),
        }
    };
    for t in &triangles {
        out.push_str(&format!("f {} {} {}\n", corner(t[0]), corner(t[2]), corner(t[1])));
    }
    Ok((out, name))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn halves() {
        assert_eq!(half_to_f32(0x3c00), 1.0);
        assert_eq!(half_to_f32(0xc000), -2.0);
        assert_eq!(half_to_f32(0x3555), 0.33325195);
    }

    #[test]
    fn g7_like_python() {
        assert_eq!(fmt_g7(0.5), "0.5");
        assert_eq!(fmt_g7(1.5), "1.5");
        assert_eq!(fmt_g7(3.0), "3");
        assert_eq!(fmt_g7(-0.9533422), "-0.9533422");
        assert_eq!(fmt_g7(12.5873), "12.5873");
        assert_eq!(fmt_g7(1234567.0), "1234567");
        assert_eq!(fmt_g7(0.00001234), "1.234e-05");
    }
}
