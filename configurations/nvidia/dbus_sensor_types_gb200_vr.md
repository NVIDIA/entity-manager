# dbus-sensors Types in GB200 and VR Configurations

Inventory of the `Exposes[].Type` values in the GB200 and VR configuration trees that are
consumed by **`dbus-sensors`** daemons. NSM, PLDM inventory, recovery, EEPROM and other
NVIDIA-daemon types are out of scope.

## Scope and method

Every `Type` under `configurations/nvidia` was cross-checked against the NVIDIA downstream
`dbus-sensors` tree, not upstream:

- Repository: `ssh://git@gitlab-master.nvidia.com:12051/dgx/bmc/dbus-sensors.git`
- Branch `develop`, commit `f7d9487d` (2026-07-29)
- A type is listed here only if a daemon in that tree claims it, either as a `"Type"` literal
  or as an `xyz.openbmc_project.Configuration.<Type>` interface.

Of the 122 distinct types in the NVIDIA configuration tree, **32 are claimed by the fork**,
across 1131 entries. Two further types listed below (`LM5066i` with 4 entries and
`MCTPUSBTarget` with 32) look like dbus-sensors types but are claimed by nothing — see
[Known issues](#known-issues).

The fork claims twice as many types as upstream `openbmc/dbus-sensors`, which recognizes only
16 of these. Types present only in the fork: `TMP75C`, `Satellite`, `SummationSensor`,
`VoltageLeakDetector`, `LeakDetectionPolicy`, `GPIOCableSensing`, `WriteProtect`,
`WriteProtectInput`, `HardwareWriteProtectedControl`, `Nvmem2`, `MCTPBridgePoolDevice`,
`MCTPUSBDevice`, `MCTPUSBGadgetTarget`, `MCTPSPIDevice`, `MCTPXROTTarget`, `MCTPPCIeTarget`.
Anything validated only against upstream will misreport these as unowned.

Family attribution:

| Family | Trees included |
| --- | --- |
| GB200 | `gb200nvl/bmc/**`, `gb200nvl/hmc/**`, `gb200.json`, `cx7_mezzanine_module.json`, `cx8_mezzanine_module.json` |
| VR | `vrnvl/vr-nvl-bmc`, `vr-nvl-ultra-bmc`, `vr-nvl-hmc`, `vr-nvl-ultra-hmc`, `vr-c2-bmc`, `vr-e5010-bmc`, `vr-quark-bmc` |
| Shared | `common/*.json`, `gb300.json` |

## Summary by daemon

Ordered by entry count. Each daemon watches Entity Manager for its own
`xyz.openbmc_project.Configuration.<Type>` interfaces, so the `Type` string is what routes a
config block to a process — it never changes the schema, only the daemon and the driver.

### satellitesensor — 539 entries

`xyz.openbmc_project.satellitesensor.service`, bus name `xyz.openbmc_project.Satellite`,
source `src/SatelliteSensor.cpp`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `Satellite` | 129 | 363 | 47 |

**How it is used.** The BMC cannot reach GPUs, CPUs or their voltage regulators directly. The
HMC collects that telemetry and exposes it as a virtual EEPROM at I2C `0x4f`; this daemon polls
byte offsets out of that mailbox and republishes each one as a normal sensor under
`/xyz/openbmc_project/sensors/{temperature,power,energy,voltage}/`. That is what makes GPU power
and CPU temperature appear in Redfish and IPMI as if the BMC had measured them. Because it is a
mailbox read, a stale value looks identical to a live one, which is why 152 entries carry
`StaleBit`/`StaleOffset`.

### mctpreactor — 179 entries (plus 32 retired)

`xyz.openbmc_project.mctpreactor.service`, bus name `xyz.openbmc_project.MCTPReactor`,
source `src/mctp/`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `MCTPBridgePoolDevice` | 24 | 56 | – |
| `MCTPUSBDevice` | 4 | 36 | – |
| `MCTPI2CTarget` | 17 | 18 | – |
| `MCTPSPIDevice` | 6 | 9 | – |
| `MCTPXROTTarget` | – | 4 | 2 |
| `MCTPUSBGadgetTarget` | – | 2 | – |
| `MCTPPCIeTarget` | – | 1 | – |
| `MCTPUSBTarget` | – | 32 | – |

**How it is used.** These are not sensors. The reactor turns each config block into an
`AssignEndpointStatic` (or `AssignEndpoint`) call to `mctpd`, which brings up the MCTP endpoint
and publishes `xyz.openbmc_project.MCTP.Endpoint` with the assigned EID. Everything that talks
MCTP downstream depends on this happening first: nsmd for GPU and NVSwitch telemetry, the PLDM
stack for firmware update and inventory, and `nvmemi` for drives. `MCTPBridgePoolDevice` is the
indirection that lets a device behind a bridge inherit an EID from the bridge's pool range
rather than hardcoding one, which is how the E1S drives get EIDs 65/75/85/95.

### fansensor — 82 entries

`xyz.openbmc_project.fansensor.service`, bus name `xyz.openbmc_project.FanSensor`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `I2CFan` | 56 | 18 | 8 |

**How it is used.** Reads tachometer registers from the fan controller and publishes RPM at
`/xyz/openbmc_project/sensors/fan_tach/<Name>`, while `Connector` binds each tach to a PWM
output published as a separate `fan_pwm` object. `phosphor-pid-control` closes the loop: it
consumes the `fan_tach` values as the process variable for its fan PIDs and writes back to the
`fan_pwm` objects. Every fan here is dual-rotor, so front and rear appear as separate sensors
and the PID takes the minimum of the pair.

### writeprotectsensor — 77 entries

`xyz.openbmc_project.writeprotectsensor.service`, bus name
`xyz.openbmc_project.WriteProtectSensor`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `WriteProtect` | 20 | 57 | – |

**How it is used.** Each entry names a GPIO rail (`GLOBAL_WP`, `IOB<0-3>_GLOBAL_WP`,
`PDB_GLOBAL_WP`, `BMC_FRU_WP`) and its polarity. The daemon exposes a writable
`xyz.openbmc_project.Software.Settings` `WriteProtected` property under
`/xyz/openbmc_project/software/<chassis>`, so a Redfish or D-Bus client can unlock flash before
a firmware update and relock afterwards. State persists across reboots via
`/var/lib/write_protected`. This is a control surface, not a measurement.

### nvmesensor and nvmemi — 54 entries

`xyz.openbmc_project.nvmesensor.service` (bus `xyz.openbmc_project.NVMeSensor`) and
`xyz.openbmc_project.nvmemisensor.service`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `NVME1000` | 17 | 37 | – |

**How it is used.** Both daemons bind the same interface, and which one services an entry
depends on the keys present. With `Bus` + `Address` the I2C daemon does a basic NVMe-MI
SMBus read at `0x6a`; with `Address` only, `nvmemi` treats it as an MCTP EID and talks NVMe-MI
over MCTP. Either way the result is a temperature at
`/xyz/openbmc_project/sensors/temperature/<Name>`, which feeds drive thermal reporting and the
thermal loop's drive setpoints.

### hwmontempsensor — 48 entries

`xyz.openbmc_project.hwmontempsensor.service`, bus name `xyz.openbmc_project.HwmonTempSensor`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `TMP75C` | 2 | 16 | 1 |
| `TMP421` | 4 | 8 | 2 |
| `TMP75` | 6 | 2 | – |
| `TMP1075` | – | 6 | – |
| `TMP411` | – | 1 | – |

**How it is used.** The daemon binds the kernel driver itself by writing to
`/sys/bus/i2c/devices/i2c-<bus>/new_device`, so no devicetree entry is needed, then polls
`tempN_input` and publishes `/xyz/openbmc_project/sensors/temperature/<Name>`. These are the
board-level ambient and component temperatures that drive the thermal PIDs: inlet and PDB
temperatures set the baseline fan floor, and the `TMP421` remote channels report NIC die
temperature for the mezzanine cards.

### nvmemi status — 46 entries

`xyz.openbmc_project.nvmestatus.service`, bus name `xyz.openbmc_project.NvmeStatus`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `Nvmem2` | 17 | 29 | – |

**How it is used.** The health companion to `NVME1000`, same drive and same EID or bus/address.
Instead of a temperature it publishes `xyz.openbmc_project.Inventory.Item.Drive` plus
`State.Decorator.OperationalStatus` at `/xyz/openbmc_project/sensors/drive/<Name>`, so drive
presence, failure and predicted-failure state reach Redfish's `Drives` collection. Names end in
`_Status_0` or `_STS_0` to distinguish them from the temperature objects.

### psusensor — 37 entries

`xyz.openbmc_project.psusensor.service`, bus name `xyz.openbmc_project.PSUSensor`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `LM5066I` | – | 8 | – |
| `MP5926` | – | 8 | – |
| `XDP710` | – | 8 | – |
| `LTC4287` | 2 | 2 | – |
| `RAA228004` | 2 | 2 | – |
| `ADS1015` | – | – | 1 |
| `LM5066i` (unclaimed) | 2 | 2 | – |

**How it is used.** Reads PMBus hot-swap controllers and voltage regulators on the power
distribution board and fans each device out into several sensors — input current, input power,
input and output voltage, and device temperature — one D-Bus object per hwmon label listed in
`Labels`. This is the source of rack-level power reporting and per-rail current monitoring, and
`SummationSensor` aggregates the four HSCs into a chassis total. `ADS1015` is the odd one out:
an IIO ADC read through psusensor to detect coolant leaks by voltage.

### leakdetectsensor — 16 entries

`xyz.openbmc_project.leakdetectsensor.service`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `VoltageLeakDetector` | 16 | – | – |

**How it is used.** Watches an analog voltage from a leak-detection cable and compares it to
`LeakThresholdVolts`. It publishes three things per detector: the raw voltage at
`/xyz/openbmc_project/sensors/voltage/`, an
`Inventory.Item.LeakDetector` object of type `Moisture`, and a `State.LeakDetector` object under
`/xyz/openbmc_project/state/leakdetectors/`. With `ShutdownOnLeak` set it will request a host
shutdown after `ShutdownDelaySeconds`, making this a safety interlock rather than pure
telemetry. GB200 liquid-cooled chassis use it for cold-plate and manifold detectors.

### discreteleakdetectsensor — 15 entries

`xyz.openbmc_project.discreteleakdetectsensor.service`, bus name
`xyz.openbmc_project.DiscreteLeakDetectSensor`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `LeakDetectionPolicy` | – | 15 | – |

**How it is used.** VR platforms get leak *detection* from NSM (`NSM_LeakDetection`), so this
type supplies only the reaction: `LeakDetectorName` names the detector to bind to,
`CriticalReactionType` and `ReactionDelaySeconds` decide what happens on a leak. The daemon
publishes the detector inventory and state objects and, on a critical leak, drives
`RequestedHostTransition` to `Off` and logs a resolution entry. This is the split that makes
GB200 and VR genuinely different rather than just differently named.

### synthesizedsensor — 12 entries

`xyz.openbmc_project.synthesizedsensor.service`, bus name
`xyz.openbmc_project.SynthesizedSensor`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `SummationSensor` | 4 | 8 | – |

**How it is used.** A pure virtual sensor: it subscribes to `Sensor.Value` changes on the
objects listed in `SensorsToSum` and publishes the running total as a new power sensor at
`/xyz/openbmc_project/sensors/power/<Name>`. That is how `PDB_0_TotalHSC_Power_0` and
`Chassis_0_TotalHSC_Power_0` exist without any hardware behind them, giving power capping and
Redfish a single chassis input power figure instead of four HSC readings.

### nvidia-fw-write-protect — 12 entries

`com.nvidia.fwwriteprotect.service`, source `src/nvidia-fw-write-protect/`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `HardwareWriteProtectedControl` | – | 6 | – |
| `WriteProtectInput` | – | 6 | – |

**How it is used.** A layer above `writeprotectsensor`. `WriteProtectInput` declares the GPIO
sources to watch (`LineName`, `ActiveLow`, `PollInterval`); `HardwareWriteProtectedControl`
groups those sources into a named domain and lists the `FlashProtectedComponents` the domain
covers. The result is that firmware update code can ask "is this component's flash writable"
per domain (`HMC_WriteProtect`, `HPM_<0-1>_WriteProtect`) instead of reasoning about individual
GPIO lines. Only the HMC trees use it.

### intrusionsensor — 10 entries

`xyz.openbmc_project.intrusionsensor.service`, bus name `xyz.openbmc_project.IntrusionSensor`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `ChassisIntrusionSensor` | – | 10 | – |

**How it is used.** Reports chassis lid state for physical-security logging. `Class` selects
which hardware owns the intrusion pin — `Aspeed2600_Hwmon` for the SoC path or `Nct3018y_Hwmon`
for the external RTC — and `Rearm: Automatic` clears the latch once the lid closes. Each VR
platform configures both paths, with `Priority` deciding which wins.

### adcsensor — 5 entries

`xyz.openbmc_project.adcsensor.service`, bus name `xyz.openbmc_project.ADCSensor`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `ADC` | – | 5 | – |

**How it is used.** Reads a SoC ADC channel through IIO rather than I2C, which is why it uses
`Index` instead of `Bus`/`Address`. Here it monitors the RTC coin cell:
`ScaleFactor` and `SensorParams.MaxValue` describe the resistor divider, `PollRate: 3600` reads
hourly, and the lower-critical threshold at 2.0 V raises an alert before the battery dies and
the BMC loses its clock.

### gpiopresencesensor — 2 entries

`xyz.openbmc_project.presence-detect.service`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `GPIOCableSensing` | 2 | – | – |

**How it is used.** Polls a GPIO to decide whether a cable is plugged in, then publishes
`xyz.openbmc_project.Inventory.Item` and `Inventory.Item.Cable` so the cable appears in
inventory and disappears when unplugged. GB200 uses it for the processor-module C-link and
management cables.

### nvidiagpusensor — 1 entry

`xyz.openbmc_project.nvidiagpusensor.service`, bus name `xyz.openbmc_project.GpuSensor`.

| Type | GB200 | VR | Shared |
| --- | --- | --- | --- |
| `NvidiaMctpVdm` | 1 | – | – |

**How it is used.** Discovers GPUs over MCTP vendor-defined messages and publishes their
telemetry and PCIe port information directly, without the HMC mailbox in the middle. It is the
one path where a dbus-sensors daemon reaches a device that is not on a local I2C bus. Only
`gb200.json` uses it, so it is effectively the newer alternative to the `Satellite` approach.

## Per-type detail

The sections below add the concrete values behind each daemon: which files declare the type, the
buses and addresses used, the sensor names produced, and the calibration constants.

### Temperature — hwmontempsensor

Five chips share one schema (`Bus`, `Address`, `Name`, plus optional `Labels`, `Name<N>`,
`Thresholds`, `PowerState`, `PhysicalContext`). The `Type` only selects the driver from the
daemon's device table in `src/hwmon-temp/HwmonTempMain.cpp`.

| Type | Driver | Files | Bus/address | Names |
| --- | --- | --- | --- | --- |
| `TMP75C` (19) | `tmp75c` | `VR200NVL_DCSCM.json`, `vr_P4103.json`, `HMC.json`, `BMC.json`, `vr_E5074.json` across every tree | `0x48` on buses 4, 5, 10, 14; also `72` decimal on bus 8 | `BMC_0_Temp_0`, `BMC_0_DCSCM_Temp_0`, `SMM_0_Temp_0`, `HGX_BMC_0_Temp_0`, `E5074_Temp_0` |
| `TMP421` (14) | `tmp421` | `cx7`/`cx8_mezzanine_module.json`, `PCIe_Cards.json` | `0x1f` on buses 14, 15, 36, 38, and `$bus + 3`/`$bus + 6` | `NIC_<0-1>_Local_Temp_0`, `IOB$bus_NIC<0-1>_LOCAL_TEMP_C` |
| `TMP75` (8) | `tmp75` | `DCSCM_FIO_Board.json`, `PDB_NVIDIA.json`, `Chassis_1RU/2RU.json`, `vr_c2_pdb.json` | bus 3 `0x4D`, bus 6 `0x4e`, bus 9 `0x4A`, bus 23 `0x4d` | `DCSCM_FIO_0_Temp_0`, `PDB_0_Inlet_Temp_0`, `Chassis_0_Front_IO_Temp_0`, `PDB_0_Temp_0` |
| `TMP1075` (6) | `tmp1075` | `PDB_NVIDIA_{IFX,MPS,TI}.json` in both `ast2600` trees | bus 9 at `0x4e` | `PDB_0_Temp_0` |
| `TMP411` (1) | `tmp411` | `vr-c2-bmc/vr_c2_pdb.json` | bus 9 at `0x4C` | `PDB_0_Ext_Ambient_Temp_0` |

`TMP421` and `TMP411` are the dual-channel parts and are the only ones using `Labels` +
`Name1`. For `TMP421`, `Labels: ["temp2"]` acts as a permit filter: the local channel is
suppressed and only the remote NIC diode named by `Name1` is published, which is why every
threshold carries `"Index": 2`. `TMP411` permits both channels with `Labels: ["temp1","temp2"]`.

### Power — psusensor

All variants publish `PDB_0_HSC_<n>_{Cur,Pwr,Volt_In,Volt_Out,Temp}_0`, but the current label
follows the vendor's hwmon driver.

| Type | Files | Bus/address | Names | Current label |
| --- | --- | --- | --- | --- |
| `LM5066I` (8) | `PDB_NVIDIA_TI.json` in `vr-nvl-bmc/ast2600`, `vr-nvl-ultra-bmc/ast2600` | bus 9 at `0x10`, `0x12`, `0x14`, `0x16` | `PDB_0_HSC_<0-3>` | `iin` |
| `MP5926` (8) | `PDB_NVIDIA_MPS.json`, same two trees | bus 9 at `0x10`–`0x16` | `PDB_0_HSC_<0-3>` | `iin` |
| `XDP710` (8) | `PDB_NVIDIA_IFX.json`, same two trees | bus 9 at `0x10`, `0x1c`, `0x1d`, `0x1e` | `PDB_0_HSC_<0-3>` | `iout1` |
| `LTC4287` (4) | `PDB_Quanta.json` in `gb200nvl/bmc/common`, `vr-e5010-bmc` | bus `"0x06"` at `0x45`, `0x46` | `PDB_0_HSC_<0-1>` | `iout1` |
| `RAA228004` (4) | `PDB_NVIDIA.json` in `gb200nvl/bmc/common`, `vr-e5010-bmc` | bus 6 at `0x60`, `0x61` | `PDB_0_Vreg_<0-1>` | – (temps only) |
| `ADS1015` (1) | `gb300.json` | bus 1 at `$bus - 1 % 2 + 72` | `NVIDIA_GB300_CPU$bus...` | – (`in_voltage0`) |

`LM5066I` and `XDP710` are the only parts with per-instance calibration, and those numbers are
the most board-specific values in the whole tree:

| Type | Current scale (HSC 0–3) | Current offset | Power scale |
| --- | --- | --- | --- |
| `LM5066I` | 1996 / 312 / 314 / 988 | -0.2062 / 0.0322 / 0.0561 / 0.0794 | 1995610 / 312373 / 313775 / 987654 |
| `XDP710` | 3394 / 515 / 516 / 1647 | 0.0428 / 0.0021 / -0.1065 / 0.1015 | 3394433 / 514986 / 516369 / 1646633 |

`MP5926` writes 1000 / 1000000 explicitly, which is exactly what psusensor defaults to when
`<label>_Scale` is absent (`10^factor` from its `labelMatch` table), so those values are
redundant. `LTC4287`, `RAA228004`, `LM5066i` and `ADS1015` omit scales entirely and rely on
that default. Conversion is `value = raw / scale + offset`.

`ADS1015` is not a PSU part — it is an IIO ADC used for leak detection on GB300, riding on
psusensor because psusensor handles IIO devices. It is the only entry using an `in_voltage`
label and the only one with `Hysteresis` in its thresholds.

### Satellite telemetry — satellitesensor

539 entries, the largest single type in the tree, all in `HMC_C2G*.json`. Each one reads a byte
offset from a virtual EEPROM that the HMC exposes over I2C, so the BMC republishes telemetry
the HMC already collected rather than measuring anything itself.

| File | Entries |
| --- | --- |
| `vrnvl/vr-nvl-bmc/ast2700/HMC_C2G4.json` + `vr-nvl-ultra-bmc` copy | 88 each |
| `vrnvl/vr-nvl-bmc/common/HMC_C2G4.json` + `vr-nvl-ultra-bmc` copy | 76 each |
| `gb200nvl/bmc/common/HMC_C2G4.json`, `HMC_C2G4_GB300.json`, `common/HMC_C2G4.json` | 47 each |
| `gb200nvl/bmc/common/HMC_C2G2.json`, `vrnvl/vr-e5010-bmc/HMC_C2G2.json` | 35 each |

- Transport: `0x4f` on bus 2 (GB200, C2G4), bus 3 or 12 (VR)
- `OffsetValue`: mailbox byte offset (75, 79, 141, 640, 648, …)
- `SensorType`: Temperature (319), Power (134), Energy (50), Voltage (36)
- `ValueType`: `Raw` (323) or `PLDM` (216) — selects the decoder
- `Name1` on 438 entries carries the Redfish-style name
  (`ProcessorModule_0_CPU_0_Power_0`) next to the short internal `Name` (`M0_CPU_Power0`)
- `StaleBit` / `StaleOffset` on 152 entries let the daemon distinguish a stale mailbox value

The consumer is `src/SatelliteSensor.cpp`, binding
`xyz.openbmc_project.Configuration.Satellite`. It is a diverged descendant of upstream's
`smbpbi` daemon, which reads the same way but binds a different interface and different keys:

| | Upstream `smbpbi` | This fork / these configs |
| --- | --- | --- |
| Interface | `...Configuration.SmbpbiVirtualEeprom` | `...Configuration.Satellite` |
| Offset key | `ReadOffset` | `OffsetValue` |
| Units key | `Units` | `SensorType` |
| `ValueType` | `UINT64`, `FLOAT64` | `Raw`, `PLDM` |
| Staleness | unsupported | `StaleBit`, `StaleOffset` |

Not to be confused with `SatelliteController` (6 entries in `HMC_SAT.json` and
`nvidia/hmc.json`), which is bmcweb Redfish aggregation over HTTP and is not a sensor.

### Storage — nvmesensor, nvmemi

`NVME1000` is claimed by two daemons, which is deliberate and reflects two transports:

| Mode | Keys | Meaning | Entries |
| --- | --- | --- | --- |
| I2C (`nvmesensor`) | `Bus` + `Address` | mux channel and `0x6a` SMBus address | 42 |
| MCTP (`nvmemi`) | `Address` only | `Address` is the **MCTP EID**, no `Bus` | 12 |

The MCTP entries are the E1S drives in `IO_Board_CX.json` and `SMM_Interposer.json` with
`Address` of `65`, `75`, `85`, `95`, `210`. Those are pool-allocated EIDs, confirmed by the
bridge configuration in `vrnvl_platform.json`: `IO_Board_SMA_0` has `BridgePoolStartEID` 61 and
`BridgePoolEndEID` 65, and `NVMe_E1S_IO0` sits at `PoolIndex` 4, i.e. EID 65. `NVMe_E1S_SMM`
at EID 210 matches the `MCTPI2CTarget` entry of the same name.

`Nvmem2` (46 entries) is the health/status companion to `NVME1000`, same bus/address or EID,
publishing `*_Status_0` / `*_STS_0` objects with `EntityId`, `EntityInstance` and `PollRate`.

### MCTP endpoints — mctpreactor

Not sensors, but consumed by dbus-sensors' MCTP reactor, which sets up the endpoints that
`nvmemi`, nsmd, and the PLDM stack then use.

| Type | Entries | Keys | Notes |
| --- | --- | --- | --- |
| `MCTPBridgePoolDevice` | 80 | `BridgeName`, `PoolIndex`, `Name` | endpoint behind a bridge; EID derived from the bridge's pool range |
| `MCTPUSBDevice` | 40 | `Port`, `RootHubPath`, `InterfaceNum`, `StaticEndpointID`, `BridgePool{Start,End}EID`, `BridgedEndpoints`, `IgnoreEIDs`, `RecoveryThreshold` | USB topology path such as `1.2.1.3.1` |
| `MCTPI2CTarget` | 35 | `Bus`, `Address`, `StaticEndpointID`, `PollingInterval`, `IgnoreMessageTypes` | `0x1D`/`0x32`/`0x52`/`0x72` across ~15 buses |
| `MCTPSPIDevice` | 15 | `Bus`, `ChipSelect`, `StaticEndpointID` | ERoT links, EIDs 9, 10, 40, 50 |
| `MCTPXROTTarget` | 6 | `Interface`, `StaticEndpointID`, `PollingInterval` | `MCTPIROT_0` (EID 9), `MCTPVROT_0` (EID 100) |
| `MCTPUSBGadgetTarget` | 2 | `Interface`, `LocalEID` | HMC gadget side |
| `MCTPPCIeTarget` | 1 | `Address` (`0000:00:00.0`), `Interface` | PCIe VDM, EID 100 |
| `MCTPUSBTarget` | 32 | `USBPath`, `Interface`, `StaticEndpointID` | **retired — see issues** |

### Write protect and presence

| Type | Daemon | Entries | Keys |
| --- | --- | --- | --- |
| `WriteProtect` | `WriteProtectSensor.hpp` | 77 | `GpioLine`, `Polarity`, `Name` |
| `WriteProtectInput` | `nvidia-fw-write-protect` | 6 | `LineName`, `ActiveLow`, `PollInterval` |
| `HardwareWriteProtectedControl` | `nvidia-fw-write-protect` | 6 | `Sources`, `FlashProtectedComponents` |
| `GPIOCableSensing` | `GPIOPresenceSensor` | 2 | `GpioLine`, `Polarity` |

`WriteProtect` names are per-board GPIO rails (`GLOBAL_WP`, `IOB<0-3>_GLOBAL_WP`,
`PDB_GLOBAL_WP`, `BMC_FRU_WP`, `SEC_WP_HW_EXT_CTRL`). The `nvidia-fw-write-protect` daemon sits
a level up, aggregating `WriteProtectInput` GPIOs into named domains
(`HMC_WriteProtect`, `HPM_<0-1>_WriteProtect`) with the flash components each domain covers.

### Leak detection

| Type | Daemon | Entries | Keys |
| --- | --- | --- | --- |
| `VoltageLeakDetector` | `LeakDetectSensor` | 16 | `Device`, `LeakThresholdVolts`, `MinValidVolts`, `MaxValidVolts`, `ShutdownOnLeak`, `ShutdownDelaySeconds` |
| `LeakDetectionPolicy` | `DiscreteLeakDetectSensor` | 15 | `LeakDetectorName`, `CriticalReactionType`, `ReactionDelaySeconds` |

GB200 uses the analog path (`Chassis_0_LeakDetector_<0-1>_{ColdPlate,Manifold}` in
`Chassis_1RU/2RU.json`), while VR uses the policy type to attach reactions to detectors that
`NSM_LeakDetection` discovers. The two families genuinely differ here — this is not a naming
variation.

### Fans, summation, intrusion, ADC, GPU

| Type | Daemon | Entries | Notes |
| --- | --- | --- | --- |
| `I2CFan` | fansensor | 82 | `Bus`, `Address`, `Index`, `Connector{Pwm,PwmName,Tachs}`, `MaxReading`, `PowerState` |
| `SummationSensor` | `SynthesizedSensor` | 12 | `SensorsToSum` → `Chassis_0_TotalHSC_Power_0`, `PDB_0_TotalHSC_Power_0` |
| `ChassisIntrusionSensor` | intrusionsensor | 10 | `Class` (`Aspeed2600_Hwmon` or `Nct3018y_Hwmon`), `Priority`, `Rearm` |
| `ADC` | adcsensor | 5 | `Index`, `ScaleFactor`, `SensorParams`, `PollRate` — SoC ADC, no `Bus`/`Address` |
| `NvidiaMctpVdm` | nvidiagpusensor | 1 | `gb200.json`, GPU telemetry over MCTP VDM |

`Pid`, `Pid.Zone` and `Stepwise` are **not** dbus-sensors types — they belong to
`phosphor-pid-control`. The `exit-air` daemon reads `Configuration.Pid` to enumerate fan zones
for its CFM calculation, but does not own the type.

## Structure is per daemon, not per type

The 32 types collapse into far fewer schemas. Within a daemon, every type validates against
identical rules; the type string only routes the config and picks a driver name. This is also
how `schemas/legacy.json` models it — one definition per daemon with `Type` as an enum of parts,
so `TMP421` lives inside a `TempSensor` definition alongside 40 other chips.

| Daemon | Always present | Optional |
| --- | --- | --- |
| hwmontempsensor | `Bus`, `Address`, `Name` | `Labels`, `Name<N>`, `Thresholds`, `PowerState`, `PhysicalContext` |
| psusensor | `Bus`, `Address`, `Name`, `Labels`, `<label>_Name` | `<label>_Scale`, `<label>_Offset`, `Thresholds` |
| fansensor | `Bus`, `Address`, `Index`, `Connector`, `Name`, `PowerState`, `Thresholds` | `MaxReading` |
| nvmesensor / nvmemi | `Address`, `Name` | `Bus` (I2C mode only), `Thresholds`, `PowerState` |
| satellitesensor | `Bus`, `Address`, `OffsetValue`, `SensorType`, `ValueType`, `MinValue`, `MaxValue`, `PollRate`, `Name` | `Name1`, `Thresholds`, `StaleBit`, `StaleOffset` |
| adcsensor | `Index`, `Name`, `ScaleFactor`, `SensorParams`, `PollRate`, `Thresholds` | – |
| intrusionsensor | `Class`, `Name`, `Priority`, `Rearm` | – |
| mctpreactor | `Name`, `StaticEndpointID` (or pool derivation) | transport-specific: `Bus`/`Address`, `Port`, `ChipSelect`, `Interface` |

Note that `Bus`/`Address` are not universal: `ADC` uses `Index` for a SoC channel,
`ChassisIntrusionSensor` uses `Class` to name a hwmon backend, and the GPIO-based types use
`GpioLine`.

## Known issues

1. **`LM5066i` is claimed by nothing.** The fork's psusensor device table has only
   `{"LM5066I", I2CDeviceType{"lm5066i", true}}` — uppercase `I`. Four entries use the
   lowercase spelling: `gb200nvl/bmc/common/PDB_NVIDIA.json` lines 51 and 99, and
   `vrnvl/vr-e5010-bmc/PDB_NVIDIA.json` lines 51 and 99. Meanwhile
   `vr-nvl-bmc/ast2600/PDB_NVIDIA_TI.json` uses `LM5066I` for the same part. Those four HSCs
   are never claimed, so their current, voltage, power and temperature sensors never appear.

2. **`MCTPUSBTarget` was retired but 32 entries still use it.** Fork commit `86cac759`
   ("mctpreactor: adopt upstream MCTPUSBDevice consumer, retire MCTPUSBTarget") removed the
   consumer. The `vrnvl_platform.json` files have already migrated to `MCTPUSBDevice`, but ten
   standalone files have not:

   | File | `MCTPUSBTarget` entries |
   | --- | --- |
   | `vr-c2-bmc/vr_mctp_bmc_target_configuration.json` | 8 |
   | `vr-nvl-hmc/vr-nvl-hmc-ast2700-a1/vrnvl_mctp_hmc_target_configuration.json` | 4 |
   | `vr-nvl-ultra-hmc/vr-nvl-hmc-ast2700-a1/vrnvl_mctp_hmc_target_configuration.json` | 4 |
   | `vr-nvl-bmc/ast2600/vr_mctp_bmc_target_configuration_erotless.json` | 3 |
   | `vr-nvl-ultra-bmc/ast2600/vr_mctp_bmc_target_configuration_erotless.json` | 3 |
   | `vr-quark-bmc/vr_mctp_bmc_target_configuration_erotless.json` | 3 |
   | `vr-e5010-bmc/vr_mctp_bmc_target_configuration.json` | 2 |
   | `vr-nvl-hmc/vhmc-quark-common/vrnvl_mctp_hmc_target_configuration.json` | 2 |
   | `vr-nvl-ultra-hmc/vhmc-quark-common/vrnvl_mctp_hmc_target_configuration.json` | 2 |
   | `vr-quark-bmc/vr_mctp_bmc_target_configuration.json` | 1 |

   These endpoints (`HGX_CPU_<0-1>`, `MCTPUSB_GPU_SMA_<0-1>`, `MCTPUSB_E1S_SMA_0`, …) will not
   be set up on a build from `develop`. Note the key names also changed: `MCTPUSBTarget` used
   `USBPath`, whereas `MCTPUSBDevice` uses `Port` + `RootHubPath` + `InterfaceNum`.

3. **The same `PDB_Quanta.json` differs between trees.** The `gb200nvl/bmc/common` copy gives
   its `LTC4287` HSCs `temp1` thresholds; the `vr-e5010-bmc` copy has no `Thresholds` key, so
   those two HSCs publish temperature with no limits.

4. **No entry limits anything but temperature.** Across all 37 psusensor entries, thresholds
   only ever target `temp1`/`temp2` (plus the one `ADS1015` voltage). No current, power, or
   input-voltage limits exist anywhere, so over-current and brownout conditions produce no
   threshold event.

5. **Inconsistent `Bus`/`Address` encoding.** `Bus` appears as int (`9`), hex string (`"0x06"`
   in `LTC4287`), decimal string (`"40"`), and template (`"$bus + 3"`); `Address` as `"0x4d"`,
   `"0x4D"`, and bare decimal `72`. All parse, but grep-based auditing is unreliable.

6. **Missing optional properties.** The eight `gb300.json` `I2CFan` entries have no
   `MaxReading` (every other platform sets 37400). Nine temperature sensors have no
   `Thresholds`: `SMM_0_Temp_0` in every `vr_P4103.json` variant, `PDB_0_Temp_0` and
   `PDB_0_Ext_Ambient_Temp_0` in `vr-c2-bmc/vr_c2_pdb.json`, and `DCSCM_FIO_0_Temp_0` in
   `gb200nvl/bmc/common/DCSCM_FIO_Board.json`.

7. **`PDB_Quanta.json` contains C-style comments.** Entity Manager parses with
   `ignore_comments = true`, so it works at runtime, but the file is not valid JSON and fails
   strict parsers and schema validation.

8. **`SatelliteController.Port` type mismatch.** `schemas/satellite_controller.json` requires a
   number, but all five `HMC_SAT.json` files pass the string `"80"`. Only `nvidia/hmc.json`
   uses numeric `80`.

## Not dbus-sensors

The remaining 89 types belong to other daemons: 51 `NSM_*` (nsmd), `PLDMFirmwareDevice` and
`PLDMDeviceInventory` (PLDM stack), `PlatformDevice`, `Dump`, `SensorAuxName` (187 entries),
`SensorPortInfo`, `NVLinkTopology`, `I2CDeviceAssociation`, `NsmDeviceAssociation`,
`NvidiaFlightDataRecorder`, the `EEPROM*` family, the `*Recovery` family
(`MCURecovery`, `OCPRecovery`, `ConnectXRecovery`, `GPIORecovery`, `GPIOERoTRecovery`,
`GlacierCrisisRecovery`, `USBRCMRecovery`, `USBRCMForceRecovery`), the `*Port` family, `SPI`,
and `SatelliteController`.
