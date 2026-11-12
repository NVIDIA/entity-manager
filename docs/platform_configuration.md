# Platform Configurations

## Overview

Platform configurations are a special category of entity-manager configurations
used to detect a **platform** (as opposed to a single entity) by detecting 2 or
more entities.

**Key Characteristics**:

1. Platform configurations detect 2 or more entities to identify a platform
2. They use `"Type": "Platform"` and a **PlatformExposes** array (not Exposes)
3. They are validated at build time against the schema in `schemas/global.json`
   (EMPlatformConfig), which references `schemas/platform_schemas/`
4. Only the `PlatformExposes` records are published on D-Bus. Top-level keys
   such as `NamePrefix` are `$Platform.<key>` template variables only; a value
   daemons read directly goes in a record (for example `DevicePrefix`, read by
   bmcweb and pldmd as `xyz.openbmc_project.Configuration.DevicePrefix`, and
   `TopChassis`, read by pldmd as
   `xyz.openbmc_project.Configuration.TopChassis`)

They allow exposing configuration that can't be described against a single
entity. One example is configuration for MCTP over USB networks, which can
involve multiple USB hubs across entities.

## Location

Platform-specific configurations are stored in:

```text
configurations/platform/<vendor>/
```

## Schemas

Platform config schemas live under `schemas/platform/`:

## PLDM Type 2 (monitoring) keys

T2 configuration uses three layers:

- **Topology** (`platform/*-topology.json`): `NvDevice` identifies each device
  with `Name`, `Kind`, `Generation`, `Instance`, `EID` and `ParentChassis`. Port
  names describe wiring, not firmware behaviour. A `cx9_io` device also carries
  `NetworkAdapterName`, the name of its NIC under
  `chassis/<Name>/NetworkAdapters/`; the name differs per platform, so it is
  written per device rather than built from `$Instance`.
- **Entity** (the shared firmware-family file): `PLDMDevice` in `Exposes`
  carries the device's monitoring facts. `Kind` identifies the firmware family,
  not just the silicon, so sensor IDs belong in the entity file. CX-9 IDs are
  `classBase + instance - 1`; BF-4 IDs are the CX-9 IDs plus `0x1000`.
- **Policy** (`platform/*-policy.json`): `PLDM_Policy` in `PlatformExposes`
  holds platform behaviour switches, selected by NvDevice `Kind` and `Devices`
  (device names; an empty array means all devices of that kind).

`PLDMDevice` requires `Name` and `Type: PLDMDevice`. Its monitoring keys are:

| Key                                           | Purpose                                                                                                                                |
| --------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `SensorAuxNames`, `EffecterAuxNames`          | Auxiliary name to integer ID maps for sensors and effecters the device leaves unnamed.                                                 |
| `NetworkAdapter`                              | Inventory object name bound to the device's `NetworkController` entity.                                                                |
| `Ports`                                       | Sensor ID to port-name fallback used only when there are no Port entity PDRs.                                                          |
| `PortMaxSpeedMBps`, `PortType`                | Port metadata; `PortType` defaults to `BidirectionalPort`.                                                                             |
| `SensorRoles`                                 | Role name to sensor or effecter auxiliary name map, such as `primary_temperature_sensor` to `TempLimit_0`. Each role names one sensor. |
| `EventIds`                                    | Sensor name as used in `SensorAuxNames` to event ID array map, preserving the order and semantics of `SensorEventInfo.EventIds`.       |
| `ImpactedComponent`, `LoggingNamespace`       | Device-wide event component and optional logging namespace, copied from `SensorEventInfo`.                                             |
| `UpperFatal`, `UpperCritical`, `UpperCaution` | Threshold overrides by sensor auxiliary name.                                                                                          |

Existing `UpdateableComponents`, `FWInventoryOnlyComponent` and `FirmwareName`
firmware mappings remain on the record. The schema for both `PLDMDevice` and
`PLDM_Policy` is `schemas/platform/pldm_device.json`.

The `Kind` to device PDR entity mapping is a compiled table in pldmd, not an
entity-file override. Relevant entries are:

| Topology `Kind`                        | Device PDR entity |
| -------------------------------------- | ----------------- |
| `cpu`                                  | `Processor`       |
| `cx9_io`, `cx9_nic`, `nvlink_mgmt_nic` | `AddInCard`       |

PDR entities above the device bind to `ParentChassis`. The MemoryController to
parent-chassis binding is also a compiled default in pldmd.

`SensorRoles` (keyed by role) and `EventIds` (keyed by sensor name) are flat
maps because EM flattens only one level. Their values are strings and arrays of
strings respectively, never nested objects. CPU roles mirror the four
`cpu-primary-*` meson defaults. For events, match each legacy
`SensorEventInfo.SensorId` to a `SensorAuxNames` name and copy its `EventIds`
array verbatim. Resolve event names through the name-to-ID map, not an inverted
ID-to-name map: legacy names and new event aliases may share an ID. Set
`ImpactedComponent` once per device; include `LoggingNamespace` only when
present in the legacy records.

T2 precedence is device Auxiliary Names PDR > `SensorAuxNames` (and likewise for
effecters) and Port entity PDR > `Ports`. Policy link-state and plug/cable
placement overrides the normal port binding. Keep `Ports`, existing sensor-name
mappings and `SensorEventInfo` records until the matching pldmd migration
removes the legacy readers.

The CPU index is derived from the device's rank among `Kind: cpu` siblings under
the same `ParentChassis`, ordered by `Instance`; there is no `CpuIndex` key.

`PLDM_Policy` requires `Name`, `Type: PLDM_Policy` and `Kind`.
`LinkStateOnNetworkAdapter: true` binds port link-state sensors to the
`NetworkAdapter` object instead of the port. `PLDM_Policy.PlugParent` names the
inventory object the device's plug/cable sensors are associated to instead of
the port; pldmd resolves it by name, not by an absolute inventory path. HGX-R's
`PLDM_Policy_CX9` selects both placements for all eight `cx9_io` devices and
sets `PlugParent` to the existing `HGX_OSFP_Board_0` object.
