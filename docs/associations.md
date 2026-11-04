# Associations

Entity Manager will create [associations][1] between entities in certain cases.
The associations are needed as part of [2].

## Configuring Associations between Entities

The configuration record has `Name` field which is used to connect 2 ports for
an association definition.

If a matching element with `Name` is not found, that is not an error, it simply
means the component we want to associate to is not present.

The `PortType` describes which association to create. This is limited to
pre-defined values. It also defines the direction of the association.

### containing Association

Baseboard configuration.

```json
{
  "Exposes": [
    {
      "Name": "ContainingPort",
      "PortType": "contained_by"
      "Type": "Port"
    }
  ],
  "Name": "Tyan S8030 Baseboard"
}
```

Chassis configuration.

```json
{
  "Exposes": [
    {
      "Name": "ContainingPort",
      "PortType": "containing"
      "Type": "Port"
    }
  ],
  "Name": "MBX Chassis"
}
```

### powering Association

Baseboard configuration. This baseboard accepts one of several generic PSUs.

```json
{
  "Exposes": [
    {
      "Name": "GenericPowerPort",
      "PortType": "powered_by"
      "Type": "Port"
    }
  ],
  "Name": "Tyan S8030 Baseboard"
}
```

PSU configuration. This example PSU is generic and can be used on different
servers.

```json
{
  "Exposes": [
    {
      "Name": "GenericPowerPort",
      "PortType": "powering"
      "Type": "Port"
    }
  ],
  "Name": "Generic Supermicro PSU"
}
```

### probing Association

The probing association matches an entry in the inventory to a probed path and
depends on the 'Probe' statement of an EM configuration. If the 'Probe'
statement matches properties to a path, this path is set as the probed path in
the inventory item.

For example 'yosemite4.json':

```json
{
    "Exposes": [
        ...
    ],
    "Name": "Yosemite 4 Management Board",
    "Probe": "xyz.openbmc_project.FruDevice({'BOARD_PRODUCT_NAME': 'Management Board wBMC', 'PRODUCT_PRODUCT_NAME': 'Yosemite V4'})",
    "Type": "Board",
    ...
}
```

This configuration queries the object mapper to find an FruDevice interface with
the stated property values (BOARD_PRODUCT_NAME, PRODUCT_NAME). The path
`/xyz/openbmc_project/FruDevice/Management_Board_wBMC` is found, which means EM
loads the configuration. The found path is the probed path and is placed as the
probing/probed_by association property in the inventory item of the
configuration.

## Record-level Associations

A record can also state associations to other objects directly, without ports.
There are three ways to write them, in the order to prefer:

1. `Associations`: a relation mapped to its target(s). EM supplies the backward
   name. Use this for every relation in the table below.
2. `Parent_Chassis`: the legacy spelling of `Associations.parent_chassis`. It is
   still supported, new configurations should use `Associations`.
3. `xyz.openbmc_project.Association.Definitions`: the raw
   `[forward, backward, target]` tuple, written exactly as published. Use it
   only for the unique cases described at the end of this section.

### An association has three parts

An association is the triple `[forward, backward, target]`.

- **forward**: the name seen from this record, pointing at the target. A Dimm's
  `parent_processor` points at its CPU.
- **backward**: the name the target uses for this record. The CPU lists the Dimm
  under `all_memory`.
- **target**: the D-Bus path of the other object.

With `Associations` the configuration states the forward name (the key) and the
target (the value). EM decides the backward name.

### Associations

```json
{
  "Name": "ProcessorModule_$Instance_Memory_0",
  "Type": "Dimm",
  "Associations": {
    "parent_processor": "cpu/CPU_$Instance",
    "parent_chassis": "board/$ParentChassis"
  }
}
```

The value is one target or a list of targets:

```json
"Associations": {
  "parent_chassis": "chassis/Chassis_0",
  "assembly": [
    "assembly/PDB_0_Chassis_FRU_Assembly0",
    "assembly/PDB_0_Board_FRU_Assembly1"
  ]
}
```

A target is one of:

- `<type>/<name>`: an object of the system inventory. EM expands it to
  `/xyz/openbmc_project/inventory/system/<type in lower case>/<name>`, the path
  it gives to a record of that Type and Name. The name is sanitized the same way
  as for the object path. Names may contain further `/` segments.
- an absolute path (starts with `/`): used as written. Required for objects
  outside the system inventory, for example
  `/xyz/openbmc_project/software/UEFI`.

### How the backward name is chosen

EM looks up the backward name from two inputs: the relation name (the key) and
the `Type` of the record that holds `Associations`. The type of the target is not
used. The lookup is a constant table in `entity_manager.cpp`
(`associationRelations`). A row with a record type wins over the row without one.

| Relation (forward) | Record `Type` | Backward |
| ------------------ | ------------- | -------- |
| `parent_chassis` | `Dimm` | `all_memory` |
| `parent_chassis` | any other | `all_chassis` |
| `parent_processor` | `Dimm` | `all_memory` |
| `parent_processor` | any other | `all_processors` |
| `chassis` | `NVMe` | `drive` |
| `chassis` | `Cpu` | `all_processors` |
| `all_managers` | any | `chassis` |
| `all_chassis` | any | `parent_chassis` |
| `computer_system` | any | `chassis` |
| `assembly` | any | `parent_chassis` |
| `associated_fru` | any | `associated_chassis` |
| `downstream_chassis` | any | `upstream_cable` |
| `activation` | any | `inventory` |
| `all_states` | any | `parent_device` |
| `host_management_network_access` | any | `chassis` |

A relation that is not in the table is logged as an error and skipped.

### Parent_Chassis

```json
"Parent_Chassis": "/xyz/openbmc_project/inventory/system/chassis/Chassis_0"
```

is the same as `"Associations": {"parent_chassis": "chassis/Chassis_0"}`. Records
written with it before keep publishing the same associations. The value is also
published as a string property of the record's `Item` interface, as before;
`Associations` does not publish a property.

### Unique cases: the raw tuple

Use `xyz.openbmc_project.Association.Definitions` when `Associations` cannot say
what is needed:

- a relation that is not in the table, for example a new daemon-specific
  association;
- a backward name that differs from the table for the same relation and record
  Type;
- a backward name that depends on the target and not on the record.

```json
"xyz.openbmc_project.Association.Definitions": {
  "Associations": [
    [
      "all_states",
      "parent_device",
      "/xyz/openbmc_project/state/PLDM_Sensor_4_105/Port_0"
    ]
  ]
}
```

The tuple is published as written. Targets in a raw tuple are not expanded: write
the full path. Raw tuples and `Associations` can be used in the same record; the
associations of all three forms are published together.

If a case needs a relation often, add a row to `associationRelations` instead
of repeating the raw tuple.

### Adding a relation

Add a row `{relation, record type or "", backward}` to `associationRelations` in
`src/entity_manager/entity_manager.cpp`. The `Associations` schema (in
`schemas/global.json`) accepts any relation name, so no schema change is needed.

### Not covered here

Associations inside an expose item (the `Associations` list with `Forward`,
`Backward` and `AbsolutePath`) belong to the daemon that reads that item and are
not handled by EM. The ports described earlier in this document are separate and
unchanged.

[1]:
  https://github.com/openbmc/docs/blob/master/architecture/object-mapper.md#associations
[2]: https://github.com/openbmc/docs/blob/master/designs/physical-topology.md
