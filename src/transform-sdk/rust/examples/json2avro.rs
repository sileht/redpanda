// Copyright 2023 Redpanda Data, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

extern crate redpanda_transform_sdk as redpanda;

use std::collections::HashMap;

use anyhow::Result;
use redpanda::*;

fn main() -> Result<()> {
    let schema = apache_avro::Schema::parse_str(
        r#"
        {"type": "record", "name":"Interop", "namespace": "org.apache.avro",
          "fields": [
              {"name": "intField", "type": "int"},
              {"name": "longField", "type": "long"},
              {"name": "stringField", "type": "string"},
              {"name": "boolField", "type": "boolean"},
              {"name": "floatField", "type": "float"},
              {"name": "doubleField", "type": "double"},
              {"name": "bytesField", "type": "bytes"},
              {"name": "nullField", "type": "null"},
              {"name": "arrayField", "type": {"type": "array", "items": "double"}},
              {"name": "mapField", "type":
               {"type": "map", "values":
                {"type": "record", "name": "Foo",
                 "fields": [{"name": "label", "type": "string"}]}}},
              {"name": "unionField", "type":
               ["boolean", "double", {"type": "array", "items": "bytes"}]},
              {"name": "enumField", "type":
               {"type": "enum", "name": "Kind", "symbols": ["A","B","C"]}},
              {"name": "fixedField", "type":
               {"type": "fixed", "name": "MD5", "size": 16}},
              {"name": "recordField", "type":
               {"type": "record", "name": "Node",
                "fields": [
                    {"name": "label", "type": "string"},
                    {"name": "children", "type": {"type": "array", "items": "Node"}}]}}
          ]
        }"#,
    )?;
    Ok(on_record_written(|evt| my_transform(&schema, evt)))
}

fn my_transform(schema: &apache_avro::Schema, event: WriteEvent) -> Result<Vec<Record>> {
    let transformed_value = match event.record.value() {
        Some(bytes) => Some(json_to_avro(schema, bytes)?),
        None => None,
    };

    Ok(vec![Record::new_with_headers(
        event.record.key().map(|b| b.to_owned()),
        transformed_value,
        event
            .record
            .headers()
            .iter()
            .map(|h| h.to_owned())
            .collect(),
    )])
}

type JsonValue = serde_json::Value;
type AvroValue = apache_avro::types::Value;

fn json_to_avro(schema: &apache_avro::Schema, json_bytes: &[u8]) -> Result<Vec<u8>> {
    let v: serde_json::Value = serde_json::from_slice(json_bytes)?;
    let v = json2avro(v)?;
    let mut w = apache_avro::Writer::new(schema, Vec::new());
    let _ = w.append_value_ref(&v)?;
    let _ = w.flush()?;
    Ok(w.into_inner()?)
}

fn json2avro(v: JsonValue) -> Result<AvroValue> {
    Ok(match v {
        JsonValue::Null => AvroValue::Null,
        JsonValue::Bool(b) => AvroValue::Boolean(b),
        JsonValue::Number(n) => match n.as_i64().map(|i| AvroValue::Long(i)) {
            Some(v) => v,
            None => n
                .as_f64()
                .map(|f| AvroValue::Double(f))
                .ok_or(anyhow::anyhow!("unrepresentable avro value: {}", n))?,
        },
        JsonValue::String(s) => AvroValue::String(s),
        JsonValue::Array(a) => {
            AvroValue::Array(a.into_iter().map(json2avro).collect::<Result<Vec<_>>>()?)
        }
        JsonValue::Object(o) => AvroValue::Map(
            o.into_iter()
                .map(|(k, v)| json2avro(v).map(|v| (k, v)))
                .collect::<Result<HashMap<_, _>>>()?,
        ),
    })
}
