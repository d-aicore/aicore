"""Minimal FAIL-CLOSED JSON Schema draft 2020-12 validator — stdlib only (S1 simulator).

Implements exactly the keywords used by contracts/schemas/v1. Any keyword it does
not know raises UnsupportedKeyword instead of being silently ignored: a validator
that skips unknown keywords produces fake greens. Correctness is proved by the
gate's mutation suite (valid examples accepted; every mutation class rejected),
not by trust. No jsonschema/ajv on this machine and no installs allowed; an
independent-library cross-check is recorded as NOT performed in CONTRACT.md §12.
"""
from __future__ import annotations

import json
import math
import re
from pathlib import Path
from urllib.parse import urldefrag, urljoin

ANNOTATION = {"$schema", "$id", "$defs", "definitions", "title", "description",
              "$comment", "examples", "default", "deprecated", "readOnly", "writeOnly"}
KNOWN = ANNOTATION | {
    "$ref", "type", "const", "enum", "required", "properties", "additionalProperties",
    "patternProperties", "propertyNames", "items", "minItems", "maxItems", "uniqueItems",
    "contains", "minContains", "maxContains", "minLength", "maxLength", "minimum",
    "maximum", "exclusiveMinimum", "exclusiveMaximum", "pattern", "anyOf", "oneOf",
    "allOf", "if", "then", "else", "format",
}
TYPES = {"object": dict, "array": list, "string": str, "boolean": bool, "null": type(None)}
DATETIME_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{1,9})?Z$")


class UnsupportedKeyword(RuntimeError):
    pass


class SchemaLoadError(RuntimeError):
    pass


def _eq(a, b):
    """JSON equality: True must not equal 1, 1.0 must equal 1."""
    if isinstance(a, bool) != isinstance(b, bool):
        return False
    return a == b


def _is_number(v):
    """JSON numbers only: bool is not a number, and NaN/Infinity are not JSON (audit 2026-11-16)."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return False
    return not isinstance(v, float) or math.isfinite(v)


def _type_ok(value, t):
    if t == "integer":
        # draft 2020-12: a number with a zero fractional part is an integer (36.0 is valid for an integer)
        if isinstance(value, float):
            return math.isfinite(value) and value.is_integer()
        return isinstance(value, int) and not isinstance(value, bool)
    if t == "number":
        return _is_number(value)
    py = TYPES.get(t)
    if py is None:
        raise UnsupportedKeyword(f"type {t!r}")
    if t == "boolean":
        return isinstance(value, bool)
    return isinstance(value, py) and not (t != "boolean" and isinstance(value, bool))


class Registry:
    """Loads every *.schema.json in a directory, indexed by $id (same namespace)."""

    def __init__(self, schema_dir):
        self.docs = {}
        for p in sorted(Path(schema_dir).glob("*.schema.json")):
            doc = json.loads(p.read_text(encoding="utf-8"))
            if "$id" not in doc:
                raise SchemaLoadError(f"{p.name}: missing $id")
            if doc["$id"] in self.docs:
                raise SchemaLoadError(f"duplicate $id {doc['$id']}")
            self.docs[doc["$id"]] = doc

    def resolve(self, ref, base_id):
        url = urljoin(base_id, ref)
        url, frag = urldefrag(url)
        if url not in self.docs:
            raise SchemaLoadError(f"unresolved $ref {ref!r} (base {base_id})")
        node = self.docs[url]
        if frag:
            for part in frag.lstrip("/").split("/"):
                part = part.replace("~1", "/").replace("~0", "~")
                try:
                    node = node[part]
                except (KeyError, TypeError):
                    raise SchemaLoadError(f"bad pointer {frag!r} in {url}")
        return node, url

    def by_name(self, name):
        base = "https://aicore.local/schemas/v1/"
        return self.docs[base + name + ".schema.json"], base + name + ".schema.json"


def _check(value, schema, reg, base, path, errors):
    if schema is True:
        return
    if schema is False:
        errors.append(f"{path}: schema false rejects everything")
        return
    if not isinstance(schema, dict):
        raise SchemaLoadError(f"{path}: schema node is not object/bool")
    for kw in schema:
        if kw not in KNOWN:
            raise UnsupportedKeyword(f"{path}: keyword {kw!r} not implemented (fail-closed)")

    if "$ref" in schema:
        node, ref_base = reg.resolve(schema["$ref"], base)
        _check(value, node, reg, ref_base, path, errors)

    if "type" in schema:
        ts = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_type_ok(value, t) for t in ts):
            errors.append(f"{path}: expected type {schema['type']}, got {type(value).__name__}")
            return  # type-specific keywords below would be meaningless

    if "const" in schema and not _eq(value, schema["const"]):
        errors.append(f"{path}: must be const {schema['const']!r}")
    if "enum" in schema and not any(_eq(value, v) for v in schema["enum"]):
        errors.append(f"{path}: {value!r} not in enum")

    if isinstance(value, str):
        if "minLength" in schema and len(value) < schema["minLength"]:
            errors.append(f"{path}: shorter than minLength {schema['minLength']}")
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            errors.append(f"{path}: longer than maxLength {schema['maxLength']}")
        if "pattern" in schema and re.search(schema["pattern"], value) is None:
            errors.append(f"{path}: does not match pattern {schema['pattern']!r}")
        if "format" in schema:
            if schema["format"] == "date-time":
                if not DATETIME_RE.match(value):
                    errors.append(f"{path}: not an RFC3339 UTC date-time")
            else:
                raise UnsupportedKeyword(f"{path}: format {schema['format']!r}")

    if _is_number(value):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{path}: below minimum {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(f"{path}: above maximum {schema['maximum']}")
        if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
            errors.append(f"{path}: <= exclusiveMinimum {schema['exclusiveMinimum']}")
        if "exclusiveMaximum" in schema and value >= schema["exclusiveMaximum"]:
            errors.append(f"{path}: >= exclusiveMaximum {schema['exclusiveMaximum']}")

    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            errors.append(f"{path}: fewer than minItems {schema['minItems']}")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errors.append(f"{path}: more than maxItems {schema['maxItems']}")
        if schema.get("uniqueItems"):
            seen = []
            for item in value:
                if any(_eq(item, s) for s in seen):
                    errors.append(f"{path}: duplicate item (uniqueItems)")
                    break
                seen.append(item)
        if "items" in schema:
            for i, item in enumerate(value):
                _check(item, schema["items"], reg, base, f"{path}/{i}", errors)
        if "contains" in schema:
            n = 0
            for i, item in enumerate(value):
                sub = []
                _check(item, schema["contains"], reg, base, f"{path}/{i}", sub)
                if not sub:
                    n += 1
            lo = schema.get("minContains", 1)
            hi = schema.get("maxContains", float("inf"))
            if n < lo:
                errors.append(f"{path}: contains matched {n} < minContains {lo}")
            if n > hi:
                errors.append(f"{path}: contains matched {n} > maxContains {hi}")

    if isinstance(value, dict):
        for req in schema.get("required", []):
            if req not in value:
                errors.append(f"{path}: missing required property {req!r}")
        props = schema.get("properties", {})
        patprops = schema.get("patternProperties", {})
        for key, sub in value.items():
            matched = False
            if key in props:
                matched = True
                _check(sub, props[key], reg, base, f"{path}/{key}", errors)
            for pat, pschema in patprops.items():
                if re.search(pat, key):
                    matched = True
                    _check(sub, pschema, reg, base, f"{path}/{key}", errors)
            if "propertyNames" in schema:
                _check(key, schema["propertyNames"], reg, base, f"{path}/key:{key}", errors)
            if not matched and "additionalProperties" in schema:
                ap = schema["additionalProperties"]
                if ap is False:
                    errors.append(f"{path}: additional property {key!r} not allowed")
                else:
                    _check(sub, ap, reg, base, f"{path}/{key}", errors)

    if "allOf" in schema:
        for i, sub in enumerate(schema["allOf"]):
            _check(value, sub, reg, base, f"{path}&allOf{i}", errors)
    if "anyOf" in schema:
        ok = False
        for sub in schema["anyOf"]:
            trial = []
            _check(value, sub, reg, base, path, trial)
            if not trial:
                ok = True
                break
        if not ok:
            errors.append(f"{path}: matches no anyOf branch")
    if "oneOf" in schema:
        n = 0
        for sub in schema["oneOf"]:
            trial = []
            _check(value, sub, reg, base, path, trial)
            if not trial:
                n += 1
        if n != 1:
            errors.append(f"{path}: matched {n} oneOf branches (need exactly 1)")
    if "if" in schema:
        trial = []
        _check(value, schema["if"], reg, base, path, trial)
        branch = "then" if not trial else "else"
        if branch in schema:
            _check(value, schema[branch], reg, base, f"{path}&{branch}", errors)


def validation_errors(reg, name, document):
    """Return list of human-readable errors; empty when valid. Fail-closed on unknown keywords."""
    schema, base = reg.by_name(name)
    errors = []
    _check(document, schema, reg, base, "$", errors)
    return errors