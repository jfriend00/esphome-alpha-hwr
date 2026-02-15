# Schedule Management via Home Assistant

This document explains how to manage the Grundfos ALPHA HWR pump schedule using Home Assistant automations and the API.

## Overview

The pump supports two types of schedules:

- **Weekly schedule** — Recurring entries per day-of-week, organized in layers (0–4)
- **Single events** — One-time entries with Unix timestamps, for immediate or near-future activation

Both are managed through ESPHome API services exposed to Home Assistant.

## Available Services

### Weekly Schedule

| Service | Data Format | Description |
|---------|-------------|-------------|
| `esphome.hwr_pump_set_schedule_entry` | `layer,day,start_h,start_m,end_h,end_m` | Set a recurring entry |
| `esphome.hwr_pump_clear_schedule_entry` | `layer,day` | Clear a recurring entry |
| `esphome.hwr_pump_set_schedule_enabled` | `0` or `1` | Enable/disable the schedule |
| `esphome.hwr_pump_refresh_schedule` | *(none)* | Refresh schedule display |

- **day**: 0=Monday … 6=Sunday
- **layer**: 0–4 (multiple entries per day use different layers)
- **times**: 24-hour format hours and minutes

### Single Events (One-Time)

| Service | Data Format | Description |
|---------|-------------|-------------|
| `esphome.hwr_pump_set_single_event` | `begin_timestamp,end_timestamp` | Schedule a one-time run |
| `esphome.hwr_pump_clear_single_event` | `slot_index` | Clear a one-time event |
| `esphome.hwr_pump_refresh_single_events` | *(none)* | Refresh single events display |

- **timestamps**: Unix epoch seconds
- **slot_index**: 0–34 (auto-assigned on create)

## Example Automations (YAML)

### Run pump on weekday mornings (06:00–08:00)

```yaml
automation:
  - alias: "Set weekday pump schedule"
    trigger:
      - platform: homeassistant
        event: start
    action:
      - repeat:
          count: 5  # Mon-Fri
          sequence:
            - service: esphome.hwr_pump_set_schedule_entry
              data:
                data: "0,{{ repeat.index - 1 }},6,0,8,0"
```

### Emergency run for 2 hours starting now

```yaml
automation:
  - alias: "Emergency pump run"
    trigger:
      - platform: state
        entity_id: input_boolean.emergency_pump
        to: "on"
    action:
      - service: esphome.hwr_pump_set_single_event
        data:
          data: "{{ (now().timestamp() | int) + 60 }},{{ (now().timestamp() | int) + 7260 }}"
```

### Run pump when solar excess > 500W

```yaml
automation:
  - alias: "Solar excess pump run"
    trigger:
      - platform: numeric_state
        entity_id: sensor.solar_excess_power
        above: 500
        for: "00:05:00"
    condition:
      - condition: state
        entity_id: binary_sensor.hwr_pump_pump_running
        state: "off"
    action:
      - service: esphome.hwr_pump_set_single_event
        data:
          data: "{{ (now().timestamp() | int) + 60 }},{{ (now().timestamp() | int) + 3660 }}"
```

## Using the REST API

You can also call services via the Home Assistant REST API:

```bash
# Set a schedule entry (Monday 06:00-08:00 on layer 0)
curl -X POST \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"entity_id": "button.hwr_pump_set_schedule_entry", "data": "0,0,6,0,8,0"}' \
  https://homeassistant.local:8123/api/services/esphome/hwr_pump_set_schedule_entry

# Schedule a one-time run
curl -X POST \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"data": "1739664000,1739671200"}' \
  https://homeassistant.local:8123/api/services/esphome/hwr_pump_set_single_event
```

## Text Sensors

| Entity | Format | Description |
|--------|--------|-------------|
| `text_sensor.hwr_pump_weekly_schedule` | JSON: `{"e":1,"s":{"0":[...]}}` | Current weekly schedule |
| `text_sensor.hwr_pump_single_events` | `[slot] YYYY-MM-DD HH:MM - HH:MM` | Active one-time events |

## Tips

- The custom Lovelace card (`alpha-hwr-schedule-card`) is the recommended way to manage schedules interactively
- Single events **override** the weekly schedule when active
- Changes take 2–3 seconds to propagate via BLE to the pump
- Always call `refresh_schedule` or `refresh_single_events` after modifications to update the display
