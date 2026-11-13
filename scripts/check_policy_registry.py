#!/usr/bin/env python3
"""Check platform policy records against the policy registry.

policy/registry/*.json, one file per backend, is the menu of what a policy
can tweak (types, options, enums, defaults, required options, NSM features);
it is the only source this check reads. Every policy record
found under configurations/nvidia is checked:

  error    the backend does not support it (unknown type, property, feature,
           parameter, value or a missing required option)
  warning  it is supported but changes nothing (equals the backend default)

Any error fails the check; warnings never do.

usage: check_policy_registry.py [-v] [--registry DIR]
                                [--configs DIR]
"""

import argparse
import ast
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILTIN_VARIABLES = {"device", "deviceBase", "instance", "instance+1",
                     "parentChassis"}


def load_json(path):
    text = Path(path).read_text()
    # the configurations may carry C comments, as validate_configs.py allows
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"(?m)^\s*//.*$", "", text)
    return json.loads(text)


class Findings:
    def __init__(self):
        self.errors = []    # (key, message)
        self.warnings = []  # (key, message)

    def error(self, key, message):
        self.errors.append((key, message))

    def warn(self, key, message):
        self.warnings.append((key, message))


def type_ok(expected, value):
    if expected == "string":
        return isinstance(value, str)
    if expected == "bool":
        return isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "string[]":
        return isinstance(value, list) and all(isinstance(v, str) for v in value)
    return True


def dbus_type_ok(expected, value):
    """Type of a feature parameter against a JSON value."""
    if expected == "unknown":
        return True
    if expected == "bool":
        return isinstance(value, bool)
    if expected == "string":
        return isinstance(value, str)
    if expected in ("uint64", "int64", "byte"):
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "double":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected.endswith("[]"):
        element = expected[:-2]
        return isinstance(value, list) and all(
            dbus_type_ok(element, v) for v in value)
    return True


def check_nsm_policy(record, kinds, key, out):
    kind = record.get("Kind")
    if kind not in kinds:
        out.error(f"{key} unknown-kind:{kind}",
                  f"Kind {kind!r} has no tweakable features in the registry")
        return
    info = kinds[kind]
    variables = set(info.get("templateValues", {}))
    policy = record.get("Policy")
    if isinstance(policy, str):
        try:
            policy = json.loads(policy)
        except ValueError:
            policy = ast.literal_eval(policy)
    if not isinstance(policy, list):
        out.error(f"{key} policy-not-a-list", "Policy must be a list of entries")
        return
    for index, entry in enumerate(policy):
        if not isinstance(entry, dict):
            out.error(f"{key} entry{index}-not-an-object",
                      f"entry {index} is not an object")
            continue
        for name, value in entry.items():
            if name == "Devices":
                if not type_ok("string[]", value):
                    out.error(f"{key} entry{index}-devices-type",
                              "Devices must be a list of names")
                continue
            if name == "TemplateValues":
                if not isinstance(value, dict):
                    out.error(f"{key} entry{index}-templatevalues-type",
                              "TemplateValues must be an object")
                    continue
                for var in value:
                    if var not in variables:
                        out.error(
                            f"{key} entry{index}-unknown-variable:{var}",
                            f"no feature of Kind {kind} reads {{{var}}}")
                continue
            feature = info["features"].get(name)
            off_by_default = name in info.get("offByDefault", [])
            if feature is None:
                out.error(f"{key} unknown-feature:{name}",
                          f"Kind {kind} has no feature {name!r}; nsmd rejects "
                          "the override")
                continue
            if not isinstance(value, dict):
                out.error(f"{key} {name}-not-an-object",
                          f"{name} must be an object of overrides")
                continue
            for param, val in value.items():
                if param == "Enabled":
                    if not isinstance(val, bool):
                        out.error(f"{key} {name}.Enabled-type",
                                  "Enabled must be a boolean")
                    elif val == (not off_by_default):
                        out.warn(f"{key} {name}.Enabled",
                                 f"{name} is already "
                                 f"{'enabled' if val else 'disabled'} by default")
                    continue
                declared = dict(p.split(":", 1) for p in feature.get("params", [])).get(param)
                if declared is None:
                    out.error(f"{key} unknown-parameter:{name}.{param}",
                              f"{name} has no parameter {param!r}; nsmd "
                              "rejects the override")
                    continue
                if not dbus_type_ok(declared, val):
                    out.error(f"{key} parameter-type:{name}.{param}",
                              f"{name}.{param} is {declared}, got {val!r}")


def check_properties(record, spec, key, out):
    props = spec["properties"]
    for name in spec["required"]:
        if name not in record:
            out.error(f"{key} missing:{name}", f"{name} is required")
    for name, value in record.items():
        if name not in props:
            out.error(f"{key} unsupported-property:{name}",
                      f"the backend does not read {name!r}")
            continue
        p = props[name]
        if "const" in p:
            continue
        if p.get("type") in ("string", "bool", "number", "string[]"):
            if not type_ok(p["type"], value):
                out.error(f"{key} property-type:{name}",
                          f"{name} must be {p['type']}")
                continue
        if "enum" in p and isinstance(p["enum"], list):
            values = value if isinstance(value, list) else [value]
            for v in values:
                if v not in p["enum"]:
                    out.error(f"{key} bad-value:{name}={v}",
                              f"{name} must be one of {p['enum']}")
        if "minimum" in p and isinstance(value, (int, float)) and not isinstance(
                value, bool) and value < p["minimum"]:
            out.error(f"{key} bad-value:{name}={value}",
                      f"{name} must be at least {p['minimum']}")
        if "pattern" in p and isinstance(value, list):
            for v in value:
                if isinstance(v, str) and not re.search(p["pattern"], v):
                    out.error(f"{key} bad-value:{name}={v}",
                              f"{name} entry {v!r} must match {p['pattern']}")
        if "default" in p and value == p["default"]:
            out.warn(f"{key} {name}",
                     f"{name} already defaults to {value!r}")


def merge(into, new, source):
    """Deep-merge a registry file; two files may not define the same value."""
    for key, value in new.items():
        if key.startswith("_"):
            continue
        if key not in into:
            into[key] = value
        elif isinstance(into[key], dict) and isinstance(value, dict):
            merge(into[key], value, source)
        else:
            sys.exit(f"{source}: {key} is already defined by another "
                     "registry file")


def policy_items(config_dir):
    """Yield (relative file, top record name, exposes item) of every policy
    type or policy-looking type."""
    for path in sorted(Path(config_dir).rglob("*.json")):
        try:
            data = load_json(path)
        except ValueError:
            continue
        records = data if isinstance(data, list) else [data]
        rel = os.path.relpath(path, config_dir)
        for record in records:
            if not isinstance(record, dict):
                continue
            top = record.get("Name", "")
            for field in ("Exposes", "PlatformExposes"):
                items = record.get(field)
                if isinstance(items, list):
                    for item in items:
                        if isinstance(item, dict) and "Type" in item:
                            yield rel, top, item
                elif isinstance(items, dict) and "Type" in items:
                    yield rel, top, items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--registry", default=str(ROOT / "policy" / "registry"))
    ap.add_argument("--configs", default=str(ROOT / "configurations" / "nvidia"))
    args = ap.parse_args()

    registry = {}
    for path in sorted(Path(args.registry).rglob("*.json")):
        merge(registry, load_json(path), path.name)
    types = {}
    for name, menu in registry["policies"].items():
        options = menu["options"]
        types[name] = {
            "properties": {"Name": {"type": "string"},
                           "Type": {"const": name}, **options},
            "required": ["Name", "Type"] + [
                k for k, v in options.items() if v.get("required")],
            "onePerSystem": menu.get("onePerSystem"),
        }
    out = Findings()
    seen = {}  # (file, top record, type, scope value) -> count
    checked = 0

    for rel, top, item in policy_items(args.configs):
        typ = item["Type"]
        if typ not in types:
            if typ.endswith("Policy"):
                out.error(f"{rel} {item.get('Name', '?')} unregistered-type:{typ}",
                          f"policy type {typ} is not in the registry")
            continue
        spec = types[typ]
        name = item.get("Name", "?")
        key = f"{rel} {typ} {name}"
        checked += 1
        check_properties(item, spec, key, out)
        if typ == "NSM_Policy":
            check_nsm_policy(item, registry["policies"]["NSM_Policy"]["kinds"],
                             key, out)
        scope_value = item.get(spec["onePerSystem"]) if spec[
            "onePerSystem"] else None
        if scope_value is not None:
            slot = (rel, top, typ, scope_value)
            seen[slot] = seen.get(slot, 0) + 1
            if seen[slot] == 2:
                out.error(f"{key} duplicate-{spec['onePerSystem']}:"
                          f"{scope_value}",
                          f"a second {typ} record for {spec['onePerSystem']} "
                          f"{scope_value} in the same platform record; "
                          "only one is used")

    print(f"policy registry: {checked} policy records checked, "
          f"{len(out.errors)} errors, {len(out.warnings)} warnings")
    for key, message in out.errors:
        print(f"ERROR  {key}: {message}")
    if args.verbose:
        for key, message in out.warnings:
            print(f"warn   {key}: {message}")
    return 1 if out.errors else 0


if __name__ == "__main__":
    sys.exit(main())
