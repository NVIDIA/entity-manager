# optimized_nsmd_configs

Sample entity files for the "daemon default" proposal: consumer daemons
(nsmd, pldm, nvidia-code-mgmt, phosphor-debug-collector,
phosphor-health-monitor) enable every feature of a device by default, and the
entity file only records how a platform differs from that default.

These files are samples of the end state. Neither Entity Manager nor the
consumer daemons read this format yet.

## Idea

Today each platform's EM config spells out every feature a daemon should
create (`Exposes` records such as `NSM_Chassis`, `NSM_Temp`, `MCURecovery`,
`Dump`). Most of that content is identical across platforms.

In this proposal:

- The daemon holds the superset of the device, selected by the probe's `Kind`
  and `Generation`: every feature the device supports, all switched on. It
  has no platform-specific information.
- The entity file lists only the features to switch off (`Disable`) and the
  values that differ from the default (`PlatformLiterals`).
- A device whose platform matches the default has empty `Disable` and
  `PlatformLiterals` lists.

Platform topology (EIDs, I2C buses, USB paths, SPI chip selects) is out of
scope here; it is referenced through `$` variables.

## Format

```json
[
  {
    "Name": "$Name",
    "Probe": "xyz.openbmc_project.Configuration.NvDevice({'Kind': '^<kind>$', 'Generation': '<generation>'})",
    "InstanceNumber": "$INSTANCE_NUMBER",
    "EID": "$EID_<DEVICE>_0",
    "Features": [
      { "Type": "Telemetry", "Disable": [], "PlatformLiterals": {} },
      { "Type": "Firmware",  "Disable": [], "PlatformLiterals": {} },
      { "Type": "Dump",      "Disable": [], "PlatformLiterals": {} }
    ]
  }
]
```

| Field | Meaning |
|---|---|
| `Probe` | `NvDevice` match on `Kind` and `Generation`; the daemon picks its default model from these. |
| `Features` | One entry per feature type. Only `Telemetry`, `Firmware` and `Dump` exist. |
| `Type` | `Telemetry`: inventory, sensors, attributes and RoT/IRoT objects (nsmd). `Firmware`: update and recovery (pldm, nvidia-code-mgmt, platform recovery). `Dump`: supported dump types. |
| `Disable` | Feature names to switch off. Unlisted features stay enabled. |
| `PlatformLiterals` | Values replacing the default, as `Feature > Property`. Unlisted properties keep the default. |

## Design

### Why nsmd needs EM configs today

- **Enable**: the business logic of each NSM feature (`NSM_Temp`,
  `NSM_Power`, `NSM_RoT_Slot`, ...) is generic and platform-agnostic; the
  config turns it on for a device. No record, no feature.
- **Parameterize**: the config passes sensor IDs, thresholds and object names.
- **D-Bus paths**: the config sets each object's inventory path and its associations to chassis, processor or board.

This let one nsmd code base serve every platform and generation without logic changes.

Example: the same `NSM_Temp` code handles the GPU:

```json
{
  "Type": "NSM_Temp",
  "Name": "HGX_GPU $INSTANCE_NUMBER TEMP_0",
  "UUID": "$UUID",
  "SensorId": 0,
  "PhysicalContext": "GPU",
  "Aggregated": true,
  "Priority": true,
  "ThermalParameters": [
    { "Name": "UpperFatal", "Value": 95.0, "Dynamic": false },
    { "Name": "UpperCritical", "Value": 92.0, "Dynamic": false },
    { "Name": "UpperCaution", "Value": 90.0, "Dynamic": false }
  ],
  "Associations": [
    { "Forward": "chassis", "Backward": "all_sensors", "AbsolutePath": "/xyz/openbmc_project/inventory/system/chassis/HGX_GPU $INSTANCE_NUMBER" },
    { "Forward": "processor", "Backward": "all_sensors", "AbsolutePath": "/xyz/openbmc_project/inventory/system/accelerator/GPU $INSTANCE_NUMBER" }
  ]
}
```

GPU SMA:

```json
{
  "Type": "NSM_Temp",
  "Name": "HGX_GPU_SMA_$INSTANCE_NUMBER_Temp_0",
  "UUID": "$UUID",
  "SensorId": 17,
  "PhysicalContext": "GPUSubsystem",
  "Description": "Internal SMA temperature values",
  "Aggregated": true,
  "Priority": true,
  "Associations": [
    { "Forward": "chassis", "Backward": "all_sensors", "AbsolutePath": "/xyz/openbmc_project/inventory/system/chassis/HGX_GPU_SMA_$INSTANCE_NUMBER" }
  ]
}
```

### Proposal

1. **Features and parameters default in nsmd.** nsmd owns every parameter a
   feature needs and enables all features of an entity by default, selected
   by `Kind` and `Generation`. EM publishes one `NvDevice` record per device;
   its config only disables features the platform doesn't use (`Disable`) and
   overrides parameters that differ (`PlatformLiterals`).
2. **Standard D-Bus object paths.** Object paths follow a fixed naming rule
   per device type and role, so nsmd doesn't invent paths across generations
   and configs don't need to spell them out. pldmd and the dbus-sensors
   services already follow this approach.

### Without and with the change

Without change:
```text
+---------------------------+                     +----------------------------------+
| Entity Manager            |                     | nsmd                             |
|                           |                     |  +----------------------------+  |
| Platform JSON:            |                     |  | Read each record: enable,  |  |
| N NSM_* exposes per       |   N Configuration.  |  | parameters, D-Bus paths    |  |
| device                    |    NSM_* objects    |  +----------------------------+  |
|                           |  -----------------> |                v                 |
| Publishes N               |     GetSubTree +    |  +----------------------------+  |
| Configuration.NSM_*       |   InterfacesAdded   |  | Generic business logic     |  |
| D-Bus objects             |                     |  | NSM_Temp, NSM_Power, ...   |  |
|                           |                     |  +----------------------------+  |
+---------------------------+                     +----------------|-----------------+
                                                                   v
                                                     D-Bus: inventory, sensors, RoT
```

With change:
```text
+---------------------------+                     +----------------------------------+
| Entity Manager            |   1 Configuration.  | nsmd                             |
| NvDevice JSON: Kind,      |   NvDevice object   |  +----------------------------+  |
| EID, Disable,             |  -----------------> |  | Default model per device:  |  |
| PlatformLiterals          |                     |  | all features on, params,   |  |
+---------------------------+                     |  | D-Bus paths                |  |
                                                  |  +----------------------------+  |
                                                  |                v                 |
                                                  |  +----------------------------+  |
                                                  |  | Apply Disable +            |  |
                                                  |  | PlatformLiterals           |  |
                                                  |  +----------------------------+  |
                                                  |                v                 |
                                                  |  +----------------------------+  |
                                                  |  | Generic business logic     |  |
                                                  |  | (unchanged)                |  |
                                                  |  +----------------------------+  |
                                                  +----------------|-----------------+
                                                                   v
                                                  D-Bus: same inventory, sensors, RoT
```

### Sample NSMD flow

```text
+-----------------------+              +-----------------------+
| EM device record      |  Kind        | nsmd default model    |
| EID, Kind, overrides  | -----------> | per device: <kind>    |
+-----------+-----------+              +-----------+-----------+
            | Disable, PlatformLiterals,           v
            | EID, InstanceNumber      +-----------------------+
            +------------------------> | Apply overrides       |
                                       +-----------+-----------+
                                                   v
                                       +-----------------------+
                                       | Effective config      |
                                       | defaults + overrides  |
                                       +-----------+-----------+
                                                   v
                                       +-----------------------+
                                       | Feature business      |
                                       | logic (unchanged)     |
                                       +-----------+-----------+
                                                   v
                                           nsmd D-Bus objects
```

Per-type creation code is unchanged; only its property source moves to the merged
model. Default models ship with nsmd.

### Pros and cons

Pros:
- About 95% less NSM config; new platforms list only their differences.
- One source of truth per device, so defaults can't drift between platforms.
- Fewer `Configuration.*` objects to publish and scan; every platform diff is real.

Cons:
- nsmd must keep default models backward compatible across device generations. This is
  bounded: the develop branch supports at most two generations at a time.

### Object path standardization

Most remaining `PlatformLiterals` are names and paths, not behaviour:

| Inconsistency | Example |
|---|---|
| Same object, different parent | `HGX_BMC_0` under `system/bmc` on one HMC variant and `system/chassis` on another |
| Device spelling | `CX_N` (VR) vs `ConnectX_N` (HGXR) |
| Prefix | `IRoT_CX_N` (VR) vs `HGX_IRoT_ConnectX_N` (HGXR) |
| Collection | GPU under `accelerator/` and `dimm/` (VR) vs `processors/` and `memory/` (HGXR) |

Proposed object paths, following HGXR where VR and HGXR differ. `<Prefix>` is empty on
VR-NVL-BMC and `HGX_` on VR-NVL-HMC; `N` is the instance number:

| Device | objectPath |
|---|---|
| CX9 | Chassis: `/xyz/openbmc_project/inventory/system/chassis/CX_N`<br>NetworkAdapter: `/xyz/openbmc_project/inventory/system/chassis/CX_N/NetworkAdapters/CX_NIC_N`<br>Ports: `/xyz/openbmc_project/inventory/system/chassis/CX_N/NetworkAdapters/CX_NIC_N/Ports/<port>`<br>PCIe switch: `/xyz/openbmc_project/inventory/system/fabrics/PCIeTopology_CX_N/Switches/CX_Switch_N`<br>PCIe switch ports: `/xyz/openbmc_project/inventory/system/fabrics/PCIeTopology_CX_N/Switches/CX_Switch_N/Ports/UP_0` and `DOWN_<k>`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/IRoT_CX_N` |
| Bay C CX9 | Chassis: `/xyz/openbmc_project/inventory/system/chassis/Bay_C_CX_0`<br>NetworkAdapter: `/xyz/openbmc_project/inventory/system/chassis/Bay_C_CX_0/NetworkAdapters/Bay_C_CX_NIC_0`<br>Ports: `/xyz/openbmc_project/inventory/system/chassis/Bay_C_CX_0/NetworkAdapters/Bay_C_CX_NIC_0/Ports/<port>`<br>PCIe switch: `/xyz/openbmc_project/inventory/system/fabrics/PCIeTopology_Bay_C_CX_0/Switches/Bay_C_CX_Switch_0`<br>PCIe switch ports: `/xyz/openbmc_project/inventory/system/fabrics/PCIeTopology_Bay_C_CX_0/Switches/Bay_C_CX_Switch_0/Ports/UP_0` and `DOWN_<k>`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/IRoT_Bay_C_CX_0` |
| Bay C BF4 | Chassis: `/xyz/openbmc_project/inventory/system/chassis/BlueField_0`<br>NetworkAdapter: `/xyz/openbmc_project/inventory/system/chassis/BlueField_0/NetworkAdapters/BlueField_NIC_0`<br>Ports: `/xyz/openbmc_project/inventory/system/chassis/BlueField_0/NetworkAdapters/BlueField_NIC_0/Ports/<port>`<br>PCIe switch: `/xyz/openbmc_project/inventory/system/fabrics/PCIeTopology_BlueField_0/Switches/BlueField_Switch_0`<br>PCIe switch ports: `/xyz/openbmc_project/inventory/system/fabrics/PCIeTopology_BlueField_0/Switches/BlueField_Switch_0/Ports/UP_0` and `DOWN_<k>`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/IRoT_BlueField_0` |
| CX SMA | Chassis: `/xyz/openbmc_project/inventory/system/chassis/IO_Board_SMA_N`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/IRoT_IO_Board_SMA_N` |
| Bay C SMA | Chassis: `/xyz/openbmc_project/inventory/system/chassis/Bay_C_SMA_0`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/IRoT_Bay_C_SMA_0` |
| HPM SMA, BMC | Chassis: `/xyz/openbmc_project/inventory/system/chassis/ProcessorModule_SMA_N` |
| BMC RoT | BMC chassis: `/xyz/openbmc_project/inventory/system/chassis/<Prefix>BMC_0`<br>ERoT: `/xyz/openbmc_project/inventory/system/chassis/<Prefix>ERoT_BMC_0`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/<Prefix>IRoT_BMC_0` |
| GPU | Chassis: `/xyz/openbmc_project/inventory/system/chassis/HGX_GPU_N`<br>PCIe device: `/xyz/openbmc_project/inventory/system/chassis/HGX_GPU_N/PCIeDevices/HGX_GPU_N`<br>Processor: `/xyz/openbmc_project/inventory/system/accelerator/HGX_GPU_N`<br>Ports: `/xyz/openbmc_project/inventory/system/accelerator/HGX_GPU_N/Ports/<port>`<br>Memory: `/xyz/openbmc_project/inventory/system/memory/HGX_GPU_N_DRAM_0`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/HGX_IRoT_GPU_N` |
| GPU SMA | Chassis: `/xyz/openbmc_project/inventory/system/chassis/HGX_GPU_SMA_N`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/HGX_IRoT_GPU_SMA_N` |
| HPM SMA, HMC | Chassis: `/xyz/openbmc_project/inventory/system/chassis/HGX_ProcessorModule_SMA_N`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/HGX_IRoT_ProcessorModule_SMA_N` |
| CPU | Processor: `/xyz/openbmc_project/inventory/system/cpu/CPU_N`<br>IRoT: `/xyz/openbmc_project/inventory/system/chassis/HGX_IRoT_CPU_N` |

nsmd derives every name from `Kind`, `InstanceNumber` and `<Prefix>`, so name overrides drop out
of `PlatformLiterals`. Renames are Redfish-visible and need platform owner sign-off.

### Sample GPU config

`rubin-gpu.json`, the complete VR-NVL-HMC GPU entity:

```json
[
  {
    "Name": "$Name",
    "Probe": "xyz.openbmc_project.Configuration.NvDevice({'Kind': '^gpu$', 'Generation': 'rubin'})",
    "InstanceNumber": "$INSTANCE_NUMBER",
    "EID": "$EID_GPU_0",
    "Features": [
      {
        "Type": "Telemetry",
        "Disable": [
          "ModulePowerControl",
          "DRAM_0_Temp_0",
          "ChassisPCIeDevice.LTSSMState",
          "ProcessorPCIe",
          "ChassisIRoTResponder",
          "ChassisRoT"
        ],
        "PlatformLiterals": {
          "Device": {
            "ChassisAssembly.Assembly0": {
              "AssemblyType": "Board"
            },
            "ChassisAssembly.Assembly1": {
              "AssemblyType": "Device"
            },
            "Temp.TEMP_0": {
              "ThermalParameters.UpperFatal": 95.0,
              "ThermalParameters.UpperCritical": 92.0,
              "ThermalParameters.UpperCaution": 90.0
            },
            "Temp.TEMP_1": {
              "ThermalParameters.LowerFatal": -5.0,
              "ThermalParameters.LowerCritical": -4.0,
              "ThermalParameters.LowerCaution": -2.0
            }
          }
        }
      }
    ]
  }
]
```

### Sample default model (GPU)

The default model maps each feature name to an enabled flag and its parameters.
Feature names and parameter names are today's `NSM_*` names, so a `Disable` entry
clears `enabled` and a `PlatformLiterals` entry (`Temp.TEMP_0` >
`ThermalParameters.UpperFatal`) replaces one parameter value. Values below are HGXR's.

```cpp
// nsmd/default_models/gpu.hpp
using PropertyValue = std::variant<bool, uint64_t, double, std::string>;
using Parameters = std::map<std::string, PropertyValue>; // parameter -> value

struct Feature
{
    bool enabled = true;   // cleared by Disable
    Parameters parameters; // replaced by PlatformLiterals
};

using FeatureMap = std::map<std::string, Feature>; // feature name -> feature

inline const FeatureMap gpu = {
    {"Chassis",
     {.enabled = true,
      .parameters = {{"ChassisAttributes.ChassisType",
                      "xyz.openbmc_project.Inventory.Item.Chassis.ChassisType.Component"},
                     {"ChassisAttributes.PowerLimitSupported", true}}}},
    {"Temp.TEMP_0",
     {.enabled = true,
      .parameters = {{"SensorId", uint64_t{0}}, {"PhysicalContext", "GPU"},
                     {"Aggregated", true}, {"Priority", true}}}},
    {"Power.Power_0",
     {.enabled = true,
      .parameters = {{"SensorId", uint64_t{0}}, {"PhysicalContext", "GPU"},
                     {"MaxAllowableOperatingValue", 1020.0}, {"Priority", true}}}},
    // ... Energy, Voltage, Memory, NVLink, Processor, events, IRoT
};
```

Applying the entity file never removes a feature, so the effective config still lists
disabled features with their defaults:

```cpp
for (const auto& name : disable)
    effective.at(name).enabled = false;
for (const auto& [name, params] : literals)
    for (const auto& [param, value] : params)
        effective.at(name).parameters.at(param) = value; // unknown key -> error
```

### Expected NSMD config reduction per device

Scope is NSMD configs only: the `NSM_*` Exposes records nsmd reads for each device on
VR-NVL-BMC and VR-NVL-HMC, counted in leaf JSON values. Non-NSM records (PLDM, FRU, dump,
GPIO, ...) are not counted. Percentages are of the device's NSMD config today. Direction 1
removes features and parameters that move to nsmd defaults; direction 2 removes object
names, paths and associations that nsmd derives from the naming rules.

| Device | NSMD config values today | Direction 1: defaults in nsmd | Direction 2: standard paths | NSMD config values left | NSMD config reduction |
|---|---|---|---|---|---|
| CX9 | 152 | 56 (37%) | 81 (53%) | 15 | 90% |
| Bay C CX9 | 140 | 62 (44%) | 73 (52%) | 5 | 96% |
| Bay C BF4 | 136 | 61 (45%) | 70 (51%) | 5 | 96% |
| CX SMA | 87 | 38 (44%) | 34 (39%) | 15 | 83% |
| Bay C SMA | 107 | 55 (51%) | 47 (44%) | 5 | 95% |
| HPM SMA, BMC | 8 | 1 (13%) | 2 (25%) | 5 | 38% |
| BMC RoT | 124 | 67 (54%) | 48 (39%) | 9 | 93% |
| GPU | 405 | 248 (61%) | 138 (34%) | 19 | 95% |
| GPU SMA | 180 | 101 (56%) | 74 (41%) | 5 | 97% |
| HPM SMA, HMC | 366 | 227 (62%) | 134 (37%) | 5 | 99% |
| CPU | 232 | 133 (57%) | 94 (41%) | 5 | 98% |
| **Total** | **1,937** | **1,049 (54%)** | **795 (41%)** | **93** | **95%** |

Values left include the 5 header values every file carries (`Name`, `Probe`,
`InstanceNumber`, `EID`, `Type`). By lines, NSMD configs drop from 3,331 to 233 (93%).

### Execution steps

1. Agree with nsmd owners on feature names and the default model per `Kind` and `Generation`.
2. Align VR record types with HGXR (`NSM_ChassisRoT`) and agree on the object path rules.
3. Add the `NvDevice` schema to EM; implement the default models and override step in
   nsmd behind a meson option.
4. Add a tool that expands a daemon-default file into today's `NSM_*` records.
5. Parity check on VR-NVL-BMC and VR-NVL-HMC: identical nsmd D-Bus tree with old and new
   configs (`generate_daemon_default.py`, `validate_daemon_default.py` under
   `configurations/nvidia/em_config_diff/` on branch `akt/move_config_to_daemon`).
6. Switch one platform with the old records as fallback for one release, then drop them;
   repeat for pldm, nvidia-code-mgmt and the dump service.

## Files

| File | Device | Compared against | Summary |
|---|---|---|---|
