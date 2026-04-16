# Schedule Management via Home Assistant

This document covers the Home Assistant services exposed by
`packages/alpha_hwr_schedule_editor.yaml`.

## Service name format

ESPHome prefixes each service with the node name from `esphome.name`.

- Service pattern: `esphome.<node_name>_<service_name>`
- Entity pattern: `<domain>.<node_name>_<entity_name>`
- `-` becomes `_`

If your node is named `hwr-pump`, the service
`set_schedule_entry` becomes `esphome.hwr_pump_set_schedule_entry`.

## Available services

### Weekly schedule

| Service | Data format | Description |
| --- | --- | --- |
| `esphome.<node_name>_set_schedule_entry` | `layer,day,start_h,start_m,end_h,end_m` | Set a recurring entry |
| `esphome.<node_name>_clear_schedule_entry` | `layer,day` | Clear a recurring entry |
| `esphome.<node_name>_set_schedule_enabled` | `0` or `1` | Enable or disable the weekly schedule |
| `esphome.<node_name>_refresh_schedule` | *(none)* | Refresh the weekly schedule text sensor |

- `day`: `0=Monday` … `6=Sunday`
- `layer`: `0` … `4`
- times use 24-hour `hour,minute`

### Single events

| Service | Data format | Description |
| --- | --- | --- |
| `esphome.<node_name>_set_single_event` | `begin_timestamp,end_timestamp` | Schedule a one-time run |
| `esphome.<node_name>_clear_single_event` | `slot_index` | Clear one slot |
| `esphome.<node_name>_refresh_single_events` | *(none)* | Refresh the single-events text sensor |

- timestamps are Unix epoch seconds
- slot indexes are assigned from the pump's available single-event slots

## Example automations

Replace `<node_name>` below with your ESPHome node name after converting `-`
to `_`.

### Weekday morning schedule

```yaml
automation:
  - alias: "Set weekday pump schedule"
    trigger:
      - platform: homeassistant
        event: start
    action:
      - repeat:
          count: 5
          sequence:
            - service: esphome.<node_name>_set_schedule_entry
              data:
                data: "0,{{ repeat.index - 1 }},6,0,8,0"
```

### Emergency 2-hour run

```yaml
automation:
  - alias: "Emergency pump run"
    trigger:
      - platform: state
        entity_id: input_boolean.emergency_pump
        to: "on"
    action:
      - service: esphome.<node_name>_set_single_event
        data:
          data: "{{ (now().timestamp() | int) + 60 }},{{ (now().timestamp() | int) + 7260 }}"
```

### Solar excess trigger

```yaml
automation:
  - alias: "Solar excess pump run"
    trigger:
      - platform: numeric_state
        entity_id: sensor.solar_excess_power
        above: 500
        for: "00:05:00"
    action:
      - service: esphome.<node_name>_set_single_event
        data:
          data: "{{ (now().timestamp() | int) + 60 }},{{ (now().timestamp() | int) + 3660 }}"
```

## REST API examples

```bash
# Set Monday 06:00-08:00 on layer 0
curl -X POST \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"data": "0,0,6,0,8,0"}' \
  https://homeassistant.local:8123/api/services/esphome/<node_name>_set_schedule_entry

# Schedule a one-time run
curl -X POST \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"data": "1739664000,1739671200"}' \
  https://homeassistant.local:8123/api/services/esphome/<node_name>_set_single_event
```

## Related entities

| Entity | Description |
| --- | --- |
| `text_sensor.<node_name>_weekly_schedule` | Current weekly schedule JSON |
| `text_sensor.<node_name>_single_events` | Human-readable active single events |

## Notes

- `alpha_hwr_schedule_editor.yaml` provides the services; `alpha_hwr_pairing.yaml`
  provides the schedule and single-event text sensors.
- Single events temporarily override the weekly schedule while active.
- Schedule writes take a few seconds to propagate over BLE.
- Call `refresh_schedule` or `refresh_single_events` after bulk updates if you
  want Home Assistant to refresh the displayed state immediately.
