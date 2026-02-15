/**
 * Alpha HWR Schedule Card
 *
 * Custom Lovelace card for managing the Grundfos ALPHA HWR pump's weekly schedule.
 * Reads schedule JSON from an ESPHome text sensor and writes changes via ESPHome services.
 *
 * Card config:
 *   type: custom:alpha-hwr-schedule-card
 *   entity: text_sensor.alpha_hwr_schedule
 *   device: hwr_pump                         # ESPHome device name (for service calls)
 *   title: Pump Schedule                     # Optional
 *
 * Schedule JSON format (from ESPHome text sensor):
 *   {"e":1,"s":{"0":[[360,480],[360,480],0,0,0,0,0]}}
 *   - "e": schedule enabled (1/0)
 *   - "s": layers keyed by number, only non-empty layers included
 *   - Each layer: array of 7 entries (Mon=0..Sun=6)
 *   - Entry: [start_minutes, end_minutes] or 0 (disabled)
 *
 * ESPHome services used:
 *   esphome.<device>_set_schedule_entry   data: "layer,day,sh,sm,eh,em"
 *   esphome.<device>_clear_schedule_entry data: "layer,day"
 *   esphome.<device>_refresh_schedule
 */

const DAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
const MINUTES_IN_DAY = 1440;
const HOUR_LABELS = [0, 3, 6, 9, 12, 15, 18, 21, 24];
const SNAP_MINUTES = 15; // Snap drag to 15-min intervals
const MIN_BLOCK_MINUTES = 15;

class AlphaHwrScheduleCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this._config = {};
    this._hass = null;
    this._schedule = null; // parsed JSON
    this._activeLayer = 0;
    this._selectedDay = -1;
    this._dragging = null; // { day, edge: 'start'|'end', startX }
    this._pendingChanges = new Map(); // day -> {start, end} or null (cleared)
  }

  setConfig(config) {
    if (!config.entity) throw new Error('Please define an entity');
    if (!config.device) throw new Error('Please define device (ESPHome device name)');
    this._config = {
      entity: config.entity,
      device: config.device,
      title: config.title || 'Pump Schedule',
    };
  }

  set hass(hass) {
    this._hass = hass;
    const state = hass.states[this._config.entity];
    if (state && state.state !== this._lastState) {
      this._lastState = state.state;
      this._parseSchedule(state.state);
      this._render();
    }
  }

  _parseSchedule(stateStr) {
    try {
      if (stateStr.startsWith('{')) {
        this._schedule = JSON.parse(stateStr);
      } else {
        this._schedule = null; // Not JSON (loading, error, etc.)
      }
    } catch (e) {
      this._schedule = null;
    }
  }

  /** Get the 7-entry array for the active layer, merging pending changes */
  _getLayerData() {
    if (!this._schedule) return [0, 0, 0, 0, 0, 0, 0];
    const layers = this._schedule.s || {};
    const data = layers[String(this._activeLayer)] || [0, 0, 0, 0, 0, 0, 0];
    // Ensure we have 7 entries
    const result = [];
    for (let i = 0; i < 7; i++) {
      if (this._pendingChanges.has(i)) {
        const pending = this._pendingChanges.get(i);
        result.push(pending); // null = cleared, [s,e] = modified
      } else {
        result.push(data[i] || 0);
      }
    }
    return result;
  }

  _minutesToTime(mins) {
    const h = Math.floor(mins / 60);
    const m = mins % 60;
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}`;
  }

  _snapMinutes(mins) {
    return Math.round(mins / SNAP_MINUTES) * SNAP_MINUTES;
  }

  _render() {
    const data = this._getLayerData();
    const enabled = this._schedule ? this._schedule.e : 0;
    const hasPending = this._pendingChanges.size > 0;

    // Build available layers from schedule data
    const availableLayers = new Set([0]);
    if (this._schedule && this._schedule.s) {
      Object.keys(this._schedule.s).forEach(k => availableLayers.add(parseInt(k)));
    }

    this.shadowRoot.innerHTML = `
      <style>
        :host {
          display: block;
          --primary-color: var(--ha-card-header-color, #03a9f4);
          --block-color: #039be5;
          --block-hover: #0288d1;
          --block-selected: #ff9800;
          --bg-bar: var(--secondary-background-color, #e0e0e0);
          --text-color: var(--primary-text-color, #212121);
          --text-secondary: var(--secondary-text-color, #757575);
          --card-bg: var(--ha-card-background, var(--card-background-color, white));
          --pending-color: #ff9800;
        }
        ha-card {
          padding: 16px;
        }
        .header {
          display: flex;
          justify-content: space-between;
          align-items: center;
          margin-bottom: 12px;
        }
        .title {
          font-size: 1.1em;
          font-weight: 500;
          color: var(--text-color);
        }
        .layer-tabs {
          display: flex;
          gap: 4px;
        }
        .layer-tab {
          padding: 4px 10px;
          border: 1px solid var(--divider-color, #e0e0e0);
          border-radius: 4px;
          font-size: 0.8em;
          cursor: pointer;
          background: transparent;
          color: var(--text-secondary);
          transition: all 0.15s;
        }
        .layer-tab.active {
          background: var(--primary-color);
          color: white;
          border-color: var(--primary-color);
        }
        .layer-tab:hover:not(.active) {
          background: var(--bg-bar);
        }
        .hour-labels {
          display: flex;
          margin-left: 48px;
          margin-bottom: 2px;
          position: relative;
          height: 16px;
        }
        .hour-label {
          position: absolute;
          font-size: 0.7em;
          color: var(--text-secondary);
          transform: translateX(-50%);
        }
        .day-row {
          display: flex;
          align-items: center;
          margin-bottom: 4px;
          height: 32px;
        }
        .day-label {
          width: 40px;
          font-size: 0.85em;
          color: var(--text-secondary);
          flex-shrink: 0;
          text-align: right;
          padding-right: 8px;
          user-select: none;
        }
        .timeline-bar {
          flex: 1;
          height: 28px;
          background: var(--bg-bar);
          border-radius: 4px;
          position: relative;
          cursor: pointer;
          overflow: visible;
        }
        .timeline-bar:hover {
          background: color-mix(in srgb, var(--bg-bar) 85%, var(--primary-color));
        }
        .time-block {
          position: absolute;
          top: 2px;
          bottom: 2px;
          background: var(--block-color);
          border-radius: 3px;
          cursor: pointer;
          transition: background 0.1s;
          min-width: 4px;
        }
        .time-block:hover {
          background: var(--block-hover);
        }
        .time-block.selected {
          background: var(--block-selected);
          box-shadow: 0 0 0 2px var(--block-selected), 0 0 0 4px rgba(255,152,0,0.3);
        }
        .time-block.pending {
          background: repeating-linear-gradient(
            45deg,
            var(--block-color),
            var(--block-color) 4px,
            var(--pending-color) 4px,
            var(--pending-color) 8px
          );
        }
        .drag-handle {
          position: absolute;
          top: -2px;
          bottom: -2px;
          width: 10px;
          cursor: col-resize;
          z-index: 10;
        }
        .drag-handle.start { left: -5px; }
        .drag-handle.end { right: -5px; }
        .drag-handle::after {
          content: '';
          position: absolute;
          top: 50%;
          left: 50%;
          transform: translate(-50%, -50%);
          width: 4px;
          height: 16px;
          background: rgba(255,255,255,0.7);
          border-radius: 2px;
        }
        .time-tooltip {
          position: absolute;
          top: -22px;
          background: var(--text-color);
          color: var(--card-bg);
          font-size: 0.7em;
          padding: 2px 5px;
          border-radius: 3px;
          white-space: nowrap;
          pointer-events: none;
          z-index: 20;
        }
        .time-tooltip.start { left: 0; }
        .time-tooltip.end { right: 0; }
        .footer {
          display: flex;
          justify-content: space-between;
          align-items: center;
          margin-top: 12px;
          padding-top: 8px;
          border-top: 1px solid var(--divider-color, #e0e0e0);
        }
        .status {
          display: flex;
          align-items: center;
          gap: 6px;
          font-size: 0.85em;
          color: var(--text-secondary);
        }
        .status-dot {
          width: 8px;
          height: 8px;
          border-radius: 50%;
          background: ${enabled ? '#4caf50' : '#9e9e9e'};
        }
        .actions {
          display: flex;
          gap: 6px;
        }
        .btn {
          padding: 6px 14px;
          border: none;
          border-radius: 4px;
          font-size: 0.8em;
          cursor: pointer;
          transition: all 0.15s;
        }
        .btn-primary {
          background: var(--primary-color);
          color: white;
        }
        .btn-primary:hover { opacity: 0.85; }
        .btn-primary:disabled { opacity: 0.4; cursor: default; }
        .btn-outline {
          background: transparent;
          border: 1px solid var(--divider-color, #e0e0e0);
          color: var(--text-secondary);
        }
        .btn-outline:hover { background: var(--bg-bar); }
        .btn-danger {
          background: transparent;
          border: 1px solid #ef5350;
          color: #ef5350;
        }
        .btn-danger:hover { background: rgba(239,83,80,0.08); }
        .selection-info {
          display: flex;
          align-items: center;
          gap: 8px;
          margin-top: 8px;
          padding: 8px 12px;
          background: var(--bg-bar);
          border-radius: 4px;
          font-size: 0.85em;
        }
        .selection-info .day-name { font-weight: 500; color: var(--text-color); }
        .selection-info .time-range { color: var(--text-secondary); }
        .loading {
          text-align: center;
          padding: 24px;
          color: var(--text-secondary);
        }
      </style>
      <ha-card>
        ${this._schedule ? this._renderSchedule(data, enabled, availableLayers, hasPending) : '<div class="loading">Waiting for schedule data...</div>'}
      </ha-card>
    `;

    if (this._schedule) {
      this._attachEvents();
    }
  }

  _renderSchedule(data, enabled, availableLayers, hasPending) {
    const layerTabs = [...availableLayers].sort().map(l =>
      `<button class="layer-tab ${l === this._activeLayer ? 'active' : ''}" data-layer="${l}">L${l}</button>`
    ).join('');

    const hourLabels = HOUR_LABELS.map(h => {
      const pct = (h / 24) * 100;
      return `<span class="hour-label" style="left:${pct}%">${h}</span>`;
    }).join('');

    const dayRows = DAYS.map((day, i) => {
      const entry = data[i];
      let blockHtml = '';
      if (entry && Array.isArray(entry)) {
        const [start, end] = entry;
        const isPending = this._pendingChanges.has(i);
        // Handle midnight-crossing: show as single block wrapping
        let leftPct, widthPct;
        if (end > start) {
          leftPct = (start / MINUTES_IN_DAY) * 100;
          widthPct = ((end - start) / MINUTES_IN_DAY) * 100;
        } else {
          // Midnight crossing — show from start to 24:00
          leftPct = (start / MINUTES_IN_DAY) * 100;
          widthPct = ((MINUTES_IN_DAY - start) / MINUTES_IN_DAY) * 100;
        }
        const selected = this._selectedDay === i;
        blockHtml = `
          <div class="time-block ${selected ? 'selected' : ''} ${isPending ? 'pending' : ''}"
               style="left:${leftPct}%;width:${widthPct}%"
               data-day="${i}">
            <div class="drag-handle start" data-day="${i}" data-edge="start"></div>
            <div class="drag-handle end" data-day="${i}" data-edge="end"></div>
            ${selected ? `
              <span class="time-tooltip start">${this._minutesToTime(start)}</span>
              <span class="time-tooltip end">${this._minutesToTime(end)}</span>
            ` : ''}
          </div>`;
      }
      return `
        <div class="day-row">
          <div class="day-label">${day}</div>
          <div class="timeline-bar" data-day="${i}">${blockHtml}</div>
        </div>`;
    }).join('');

    const selectionInfo = this._selectedDay >= 0 ? this._renderSelectionInfo(data) : '';

    return `
      <div class="header">
        <span class="title">${this._config.title}</span>
        <div class="layer-tabs">${layerTabs}</div>
      </div>
      <div class="hour-labels">${hourLabels}</div>
      ${dayRows}
      ${selectionInfo}
      <div class="footer">
        <div class="status">
          <span class="status-dot"></span>
          Schedule ${enabled ? 'Active' : 'Inactive'}
        </div>
        <div class="actions">
          <button class="btn btn-outline" data-action="refresh">↻ Refresh</button>
          ${hasPending ? `<button class="btn btn-outline" data-action="discard">Discard</button>` : ''}
          ${hasPending ? `<button class="btn btn-primary" data-action="save">Save Changes</button>` : ''}
        </div>
      </div>`;
  }

  _renderSelectionInfo(data) {
    const i = this._selectedDay;
    const entry = data[i];
    if (!entry || !Array.isArray(entry)) {
      return `
        <div class="selection-info">
          <span class="day-name">${DAYS[i]}</span>
          <span class="time-range">No schedule</span>
          <button class="btn btn-primary" data-action="add" data-day="${i}" style="margin-left:auto">+ Add</button>
        </div>`;
    }
    return `
      <div class="selection-info">
        <span class="day-name">${DAYS[i]}</span>
        <span class="time-range">${this._minutesToTime(entry[0])} – ${this._minutesToTime(entry[1])}</span>
        <button class="btn btn-danger" data-action="delete" data-day="${i}" style="margin-left:auto">✕ Remove</button>
      </div>`;
  }

  _attachEvents() {
    const root = this.shadowRoot;

    // Layer tabs
    root.querySelectorAll('.layer-tab').forEach(tab => {
      tab.addEventListener('click', () => {
        this._activeLayer = parseInt(tab.dataset.layer);
        this._selectedDay = -1;
        this._pendingChanges.clear();
        this._render();
      });
    });

    // Timeline bar clicks (add block or select)
    root.querySelectorAll('.timeline-bar').forEach(bar => {
      bar.addEventListener('click', (e) => {
        if (e.target.classList.contains('drag-handle')) return;
        const day = parseInt(bar.dataset.day);
        if (e.target.classList.contains('time-block')) {
          // Select/deselect block
          this._selectedDay = this._selectedDay === day ? -1 : day;
          this._render();
          return;
        }
        // Click on empty bar — select the day (shows add button)
        this._selectedDay = day;
        this._render();
      });
    });

    // Drag handles
    root.querySelectorAll('.drag-handle').forEach(handle => {
      handle.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        e.stopPropagation();
        const day = parseInt(handle.dataset.day);
        const edge = handle.dataset.edge;
        const bar = handle.closest('.timeline-bar');
        const rect = bar.getBoundingClientRect();
        this._dragging = { day, edge, bar, rect };
        this._selectedDay = day;

        const onMove = (me) => {
          if (!this._dragging) return;
          const curRect = this._dragging.rect;
          const x = Math.max(0, Math.min(me.clientX - curRect.left, curRect.width));
          const mins = this._snapMinutes(Math.round((x / curRect.width) * MINUTES_IN_DAY));
          const data = this._getLayerData();
          const entry = data[day];
          if (!Array.isArray(entry)) return;
          let [start, end] = entry;
          if (edge === 'start') {
            start = Math.min(mins, end - MIN_BLOCK_MINUTES);
          } else {
            end = Math.max(mins, start + MIN_BLOCK_MINUTES);
          }
          start = Math.max(0, Math.min(start, MINUTES_IN_DAY - MIN_BLOCK_MINUTES));
          end = Math.max(MIN_BLOCK_MINUTES, Math.min(end, MINUTES_IN_DAY));
          this._pendingChanges.set(day, [start, end]);
          this._render();
          // Re-attach drag after re-render
          this._reattachDrag(onMove, onUp);
        };

        const onUp = () => {
          this._dragging = null;
          document.removeEventListener('pointermove', onMove);
          document.removeEventListener('pointerup', onUp);
        };

        document.addEventListener('pointermove', onMove);
        document.addEventListener('pointerup', onUp);
      });
    });

    // Action buttons
    root.querySelectorAll('[data-action]').forEach(btn => {
      btn.addEventListener('click', () => {
        const action = btn.dataset.action;
        if (action === 'refresh') this._callRefresh();
        else if (action === 'save') this._saveChanges();
        else if (action === 'discard') this._discardChanges();
        else if (action === 'add') this._addBlock(parseInt(btn.dataset.day));
        else if (action === 'delete') this._deleteBlock(parseInt(btn.dataset.day));
      });
    });
  }

  _reattachDrag(onMove, onUp) {
    // After re-render, reattach the active drag handle events
    if (!this._dragging) return;
    const { day, edge } = this._dragging;
    const handle = this.shadowRoot.querySelector(
      `.drag-handle.${edge}[data-day="${day}"]`
    );
    if (handle) {
      const bar = handle.closest('.timeline-bar');
      this._dragging.bar = bar;
      this._dragging.rect = bar.getBoundingClientRect();
    }
    // onMove/onUp are still attached to document
  }

  _addBlock(day) {
    // Add a default 2-hour block at 06:00-08:00
    this._pendingChanges.set(day, [360, 480]);
    this._selectedDay = day;
    this._render();
  }

  _deleteBlock(day) {
    this._pendingChanges.set(day, null);
    this._selectedDay = -1;
    this._render();
  }

  _discardChanges() {
    this._pendingChanges.clear();
    this._selectedDay = -1;
    this._render();
  }

  _saveChanges() {
    const device = this._config.device;
    const layer = this._activeLayer;

    for (const [day, entry] of this._pendingChanges) {
      if (entry === null) {
        // Clear entry
        this._hass.callService('esphome', `${device}_clear_schedule_entry`, {
          data: `${layer},${day}`,
        });
      } else {
        const [start, end] = entry;
        const sh = Math.floor(start / 60);
        const sm = start % 60;
        const eh = Math.floor(end / 60);
        const em = end % 60;
        this._hass.callService('esphome', `${device}_set_schedule_entry`, {
          data: `${layer},${day},${sh},${sm},${eh},${em}`,
        });
      }
    }

    this._pendingChanges.clear();
    this._selectedDay = -1;

    // Schedule a refresh after the pump processes the writes
    setTimeout(() => this._callRefresh(), 3000);
    this._render();
  }

  _callRefresh() {
    const device = this._config.device;
    this._hass.callService('esphome', `${device}_refresh_schedule`, {});
  }

  getCardSize() {
    return 5;
  }

  static getStubConfig() {
    return {
      entity: 'text_sensor.alpha_hwr_schedule',
      device: 'hwr_pump',
      title: 'Pump Schedule',
    };
  }
}

customElements.define('alpha-hwr-schedule-card', AlphaHwrScheduleCard);

// Register with HA card picker
window.customCards = window.customCards || [];
window.customCards.push({
  type: 'alpha-hwr-schedule-card',
  name: 'Alpha HWR Schedule',
  description: 'Visual weekly schedule editor for Grundfos ALPHA HWR pump',
  preview: true,
});
