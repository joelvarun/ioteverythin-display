/**
 * IoT Everythin Display — HA Custom Panel
 * ════════════════════════════════════════
 * Sidebar panel that lets users pick entities for each display tab
 * and push the config to the ESP32 display over local WiFi.
 *
 * Three tabs: Lights, Climate, Sensors
 * Entity picker uses HA's built-in entity registry.
 */

const GOLD = "#D4A017";
const GOLD_B = "#FFD700";
const DARK = "#0D0D0D";
const BG = "#111111";

const ICON_MAP = {
  bulb: "mdi:lightbulb",
  tube: "mdi:fluorescent-tube",
  fan: "mdi:fan",
  socket: "mdi:power-socket-eu",
  alarm: "mdi:bell",
  warm: "mdi:fire",
  door: "mdi:door",
  motion: "mdi:motion-sensor",
};

class IotEverythinDisplayPanel extends HTMLElement {
  constructor() {
    super();
    this._hass = null;
    this._config = { lights: [], climate: { temp_sensor: "", hum_sensor: "", acs: [] }, sensors: { doors: [], motion: [] } };
    this._allEntities = [];
    this._activeTab = "lights";
    this._deviceInfo = null;
    this._status = "";
  }

  set hass(hass) {
    this._hass = hass;
    if (!this._initialized) {
      this._initialized = true;
      this._allEntities = Object.keys(hass.states).sort();
      this._loadCurrentConfig();
      this._loadDeviceInfo();
      this._render();
    } else {
      this._allEntities = Object.keys(hass.states).sort();
    }
  }

  async _loadDeviceInfo() {
    try {
      const result = await this._hass.callWS({ type: "ioteverythin_display/get_info" });
      this._deviceInfo = result;
      this._render();
    } catch (e) {
      this._deviceInfo = { error: e.message };
      this._render();
    }
  }

  async _loadCurrentConfig() {
    try {
      const result = await this._hass.callWS({ type: "ioteverythin_display/get_config" });
      if (result.lights && result.lights.length > 0) this._config.lights = result.lights;
      if (result.climate) this._config.climate = result.climate;
      if (result.sensors) this._config.sensors = result.sensors;
      this._render();
    } catch (e) {
      console.warn("Could not load display config:", e);
    }
  }

  async _pushConfig() {
    this._status = "Pushing config to display...";
    this._render();
    try {
      const result = await this._hass.callWS({
        type: "ioteverythin_display/push_config",
        config: {
          ha_token: "", // Will be filled by integration or user must set
          lights: this._config.lights,
          climate: this._config.climate,
          sensors: this._config.sensors,
        },
      });
      this._status = `Config pushed! (v${result.version || "?"})`;
    } catch (e) {
      this._status = `Error: ${e.message}`;
    }
    this._render();
    setTimeout(() => { this._status = ""; this._render(); }, 4000);
  }

  _domainFilter(domain) {
    return this._allEntities.filter(e => e.startsWith(domain + "."));
  }

  _friendlyName(entityId) {
    const state = this._hass.states[entityId];
    return state?.attributes?.friendly_name || entityId.split(".").pop().replace(/_/g, " ");
  }

  _addLight(entityId) {
    if (this._config.lights.find(l => l.eid === entityId)) return;
    const domain = entityId.split(".")[0];
    this._config.lights.push({
      eid: entityId,
      label: this._friendlyName(entityId).replace(/ /g, "\n").substring(0, 20),
      icon: domain === "light" ? "bulb" : "socket",
      dimmable: domain === "light",
      domain: domain,
      cat: "Custom",
    });
    this._render();
  }

  _removeLight(idx) {
    this._config.lights.splice(idx, 1);
    this._render();
  }

  _addAC(entityId) {
    if (this._config.climate.acs.find(a => a.eid === entityId)) return;
    this._config.climate.acs.push({
      eid: entityId,
      label: this._friendlyName(entityId),
      min: 16,
      max: 30,
    });
    this._render();
  }

  _removeAC(idx) {
    this._config.climate.acs.splice(idx, 1);
    this._render();
  }

  _addDoor(entityId) {
    if (this._config.sensors.doors.find(d => d.eid === entityId)) return;
    this._config.sensors.doors.push({
      eid: entityId,
      label: this._friendlyName(entityId).replace(/ /g, "\n").substring(0, 20),
      inverted: false,
    });
    this._render();
  }

  _removeDoor(idx) {
    this._config.sensors.doors.splice(idx, 1);
    this._render();
  }

  _addMotion(entityId) {
    if (this._config.sensors.motion.find(m => m.eid === entityId)) return;
    this._config.sensors.motion.push({
      eid: entityId,
      label: this._friendlyName(entityId).replace(/ /g, "\n").substring(0, 20),
    });
    this._render();
  }

  _removeMotion(idx) {
    this._config.sensors.motion.splice(idx, 1);
    this._render();
  }

  _render() {
    const info = this._deviceInfo;
    const infoHtml = info
      ? info.error
        ? `<div class="info-bar error">Display offline: ${info.error}</div>`
        : `<div class="info-bar">
            <span><b>${info.name || "Display"}</b></span>
            <span>IP: ${info.ip || "?"}</span>
            <span>FW: ${info.version || "?"}</span>
            <span>MAC: ${info.mac || "?"}</span>
            <span class="dot ${info.configured ? "green" : "orange"}"></span>
            <span>${info.configured ? "Configured" : "Needs config"}</span>
          </div>`
      : `<div class="info-bar">Connecting to display...</div>`;

    this.innerHTML = `
      <style>
        :host { display: block; padding: 16px; font-family: sans-serif; color: #eee; background: #111; min-height: 100vh; }
        .header { display: flex; align-items: center; gap: 12px; margin-bottom: 16px; }
        .header h1 { color: ${GOLD}; margin: 0; font-size: 24px; }
        .header img { height: 36px; }
        .info-bar { background: ${DARK}; padding: 10px 16px; border-radius: 10px; margin-bottom: 16px;
                    display: flex; gap: 16px; align-items: center; font-size: 14px; flex-wrap: wrap; }
        .info-bar.error { border: 1px solid #c0392b; }
        .dot { width: 10px; height: 10px; border-radius: 50%; display: inline-block; }
        .dot.green { background: #2ecc71; }
        .dot.orange { background: ${GOLD}; }
        .tabs { display: flex; gap: 0; margin-bottom: 16px; }
        .tab { padding: 10px 24px; cursor: pointer; border-bottom: 2px solid transparent;
               color: #888; font-size: 16px; font-weight: 500; }
        .tab.active { color: ${GOLD}; border-bottom-color: ${GOLD}; }
        .tab:hover { color: ${GOLD_B}; }
        .section { background: ${DARK}; border-radius: 12px; padding: 16px; margin-bottom: 12px; }
        .section h3 { color: ${GOLD}; margin: 0 0 12px 0; font-size: 16px; }
        .entity-list { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 12px; }
        .entity-chip { background: #1a1a0a; border: 1px solid #333; border-radius: 8px; padding: 6px 12px;
                       display: flex; align-items: center; gap: 8px; font-size: 13px; }
        .entity-chip .remove { cursor: pointer; color: #c0392b; font-weight: bold; }
        .entity-chip .eid { color: #888; font-size: 11px; }
        .add-row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
        .add-row select { background: #222; color: #eee; border: 1px solid #444; border-radius: 6px;
                         padding: 8px; font-size: 14px; max-width: 350px; flex: 1; }
        .add-row button { background: ${GOLD}; color: #000; border: none; border-radius: 6px;
                         padding: 8px 16px; cursor: pointer; font-weight: bold; font-size: 14px; }
        .add-row button:hover { background: ${GOLD_B}; }
        .push-bar { display: flex; gap: 12px; align-items: center; margin-top: 16px; }
        .push-bar button { background: ${GOLD}; color: #000; border: none; border-radius: 8px;
                          padding: 12px 32px; font-size: 16px; font-weight: bold; cursor: pointer; }
        .push-bar button:hover { background: ${GOLD_B}; }
        .push-bar .status { color: #aaa; font-size: 14px; }
        .field-row { display: flex; gap: 8px; align-items: center; margin-bottom: 8px; }
        .field-row label { color: #aaa; font-size: 13px; min-width: 100px; }
        .field-row select, .field-row input { background: #222; color: #eee; border: 1px solid #444;
                                              border-radius: 6px; padding: 6px; font-size: 13px; flex: 1; max-width: 350px; }
        .icon-select { width: 80px !important; min-width: 80px !important; flex: unset !important; }
      </style>

      <div class="header">
        <h1>IoT Everythin Display</h1>
      </div>

      ${infoHtml}

      <div class="tabs">
        <div class="tab ${this._activeTab === "lights" ? "active" : ""}" data-tab="lights">Lights (${this._config.lights.length})</div>
        <div class="tab ${this._activeTab === "climate" ? "active" : ""}" data-tab="climate">Climate (${this._config.climate.acs.length} ACs)</div>
        <div class="tab ${this._activeTab === "sensors" ? "active" : ""}" data-tab="sensors">Sensors (${(this._config.sensors.doors?.length || 0) + (this._config.sensors.motion?.length || 0)})</div>
      </div>

      <div id="tab-content">
        ${this._renderActiveTab()}
      </div>

      <div class="push-bar">
        <button id="push-btn">Push Config to Display</button>
        <span class="status">${this._status}</span>
      </div>
    `;

    // Event listeners
    this.querySelectorAll(".tab").forEach(tab => {
      tab.addEventListener("click", () => {
        this._activeTab = tab.dataset.tab;
        this._render();
      });
    });

    this.querySelector("#push-btn")?.addEventListener("click", () => this._pushConfig());

    // Tab-specific listeners
    this._attachTabListeners();
  }

  _renderActiveTab() {
    switch (this._activeTab) {
      case "lights": return this._renderLightsTab();
      case "climate": return this._renderClimateTab();
      case "sensors": return this._renderSensorsTab();
      default: return "";
    }
  }

  _renderLightsTab() {
    const lightEntities = this._allEntities.filter(e => e.startsWith("light.") || e.startsWith("switch."));
    const chips = this._config.lights.map((l, i) => `
      <div class="entity-chip">
        <span>${l.label.replace(/\n/g, " ")}</span>
        <span class="eid">${l.eid}</span>
        <select class="icon-select" data-light-icon="${i}">
          ${Object.keys(ICON_MAP).filter(k => !["door","motion"].includes(k)).map(k =>
            `<option value="${k}" ${l.icon === k ? "selected" : ""}>${k}</option>`
          ).join("")}
        </select>
        <label style="font-size:11px;color:#aaa"><input type="checkbox" data-light-dim="${i}" ${l.dimmable ? "checked" : ""}> Dim</label>
        <span class="remove" data-remove-light="${i}">✕</span>
      </div>
    `).join("");

    return `
      <div class="section">
        <h3>Light &amp; Switch Entities</h3>
        <div class="entity-list">${chips || "<span style='color:#666'>No entities added yet</span>"}</div>
        <div class="add-row">
          <select id="light-picker">
            <option value="">Select entity...</option>
            ${lightEntities.map(e => `<option value="${e}">${this._friendlyName(e)} (${e})</option>`).join("")}
          </select>
          <button id="add-light-btn">+ Add</button>
        </div>
      </div>
    `;
  }

  _renderClimateTab() {
    const tempSensors = this._allEntities.filter(e => e.startsWith("sensor.") && (e.includes("temp") || e.includes("therm")));
    const humSensors = this._allEntities.filter(e => e.startsWith("sensor.") && (e.includes("humid") || e.includes("moisture")));
    const climateEntities = this._allEntities.filter(e => e.startsWith("climate."));

    const acChips = this._config.climate.acs.map((ac, i) => `
      <div class="entity-chip">
        <span>${ac.label}</span>
        <span class="eid">${ac.eid}</span>
        <span style="color:#aaa;font-size:11px">${ac.min}-${ac.max}°</span>
        <span class="remove" data-remove-ac="${i}">✕</span>
      </div>
    `).join("");

    return `
      <div class="section">
        <h3>Temperature &amp; Humidity Sensors</h3>
        <div class="field-row">
          <label>Temp sensor:</label>
          <select id="temp-sensor-pick">
            <option value="">None</option>
            ${tempSensors.map(e => `<option value="${e}" ${this._config.climate.temp_sensor === e ? "selected" : ""}>${this._friendlyName(e)} (${e})</option>`).join("")}
          </select>
        </div>
        <div class="field-row">
          <label>Humidity sensor:</label>
          <select id="hum-sensor-pick">
            <option value="">None</option>
            ${humSensors.map(e => `<option value="${e}" ${this._config.climate.hum_sensor === e ? "selected" : ""}>${this._friendlyName(e)} (${e})</option>`).join("")}
          </select>
        </div>
      </div>
      <div class="section">
        <h3>Air Conditioners</h3>
        <div class="entity-list">${acChips || "<span style='color:#666'>No ACs added yet</span>"}</div>
        <div class="add-row">
          <select id="ac-picker">
            <option value="">Select climate entity...</option>
            ${climateEntities.map(e => `<option value="${e}">${this._friendlyName(e)} (${e})</option>`).join("")}
          </select>
          <button id="add-ac-btn">+ Add</button>
        </div>
      </div>
    `;
  }

  _renderSensorsTab() {
    const binarySensors = this._allEntities.filter(e => e.startsWith("binary_sensor."));
    const doorSensors = binarySensors.filter(e => {
      const s = this._hass.states[e];
      return s?.attributes?.device_class === "door" || e.includes("door") || e.includes("contact");
    });
    const motionSensors = binarySensors.filter(e => {
      const s = this._hass.states[e];
      return s?.attributes?.device_class === "motion" || s?.attributes?.device_class === "occupancy" || e.includes("motion") || e.includes("occupancy");
    });

    const doorChips = (this._config.sensors.doors || []).map((d, i) => `
      <div class="entity-chip">
        <span>${d.label.replace(/\n/g, " ")}</span>
        <span class="eid">${d.eid}</span>
        <label style="font-size:11px;color:#aaa"><input type="checkbox" data-door-inv="${i}" ${d.inverted ? "checked" : ""}> Inverted</label>
        <span class="remove" data-remove-door="${i}">✕</span>
      </div>
    `).join("");

    const motionChips = (this._config.sensors.motion || []).map((m, i) => `
      <div class="entity-chip">
        <span>${m.label.replace(/\n/g, " ")}</span>
        <span class="eid">${m.eid}</span>
        <span class="remove" data-remove-motion="${i}">✕</span>
      </div>
    `).join("");

    return `
      <div class="section">
        <h3>Door Sensors</h3>
        <div class="entity-list">${doorChips || "<span style='color:#666'>No door sensors added</span>"}</div>
        <div class="add-row">
          <select id="door-picker">
            <option value="">Select door sensor...</option>
            ${doorSensors.map(e => `<option value="${e}">${this._friendlyName(e)} (${e})</option>`).join("")}
          </select>
          <button id="add-door-btn">+ Add</button>
        </div>
      </div>
      <div class="section">
        <h3>Motion / Occupancy Sensors</h3>
        <div class="entity-list">${motionChips || "<span style='color:#666'>No motion sensors added</span>"}</div>
        <div class="add-row">
          <select id="motion-picker">
            <option value="">Select motion sensor...</option>
            ${motionSensors.map(e => `<option value="${e}">${this._friendlyName(e)} (${e})</option>`).join("")}
          </select>
          <button id="add-motion-btn">+ Add</button>
        </div>
      </div>
    `;
  }

  _attachTabListeners() {
    // Lights tab
    this.querySelector("#add-light-btn")?.addEventListener("click", () => {
      const sel = this.querySelector("#light-picker");
      if (sel?.value) { this._addLight(sel.value); }
    });
    this.querySelectorAll("[data-remove-light]").forEach(el => {
      el.addEventListener("click", () => this._removeLight(parseInt(el.dataset.removeLight)));
    });
    this.querySelectorAll("[data-light-icon]").forEach(el => {
      el.addEventListener("change", () => {
        this._config.lights[parseInt(el.dataset.lightIcon)].icon = el.value;
      });
    });
    this.querySelectorAll("[data-light-dim]").forEach(el => {
      el.addEventListener("change", () => {
        this._config.lights[parseInt(el.dataset.lightDim)].dimmable = el.checked;
      });
    });

    // Climate tab
    this.querySelector("#temp-sensor-pick")?.addEventListener("change", (e) => {
      this._config.climate.temp_sensor = e.target.value;
    });
    this.querySelector("#hum-sensor-pick")?.addEventListener("change", (e) => {
      this._config.climate.hum_sensor = e.target.value;
    });
    this.querySelector("#add-ac-btn")?.addEventListener("click", () => {
      const sel = this.querySelector("#ac-picker");
      if (sel?.value) { this._addAC(sel.value); }
    });
    this.querySelectorAll("[data-remove-ac]").forEach(el => {
      el.addEventListener("click", () => this._removeAC(parseInt(el.dataset.removeAc)));
    });

    // Sensors tab
    this.querySelector("#add-door-btn")?.addEventListener("click", () => {
      const sel = this.querySelector("#door-picker");
      if (sel?.value) { this._addDoor(sel.value); }
    });
    this.querySelectorAll("[data-remove-door]").forEach(el => {
      el.addEventListener("click", () => this._removeDoor(parseInt(el.dataset.removeDoor)));
    });
    this.querySelectorAll("[data-door-inv]").forEach(el => {
      el.addEventListener("change", () => {
        this._config.sensors.doors[parseInt(el.dataset.doorInv)].inverted = el.checked;
      });
    });
    this.querySelector("#add-motion-btn")?.addEventListener("click", () => {
      const sel = this.querySelector("#motion-picker");
      if (sel?.value) { this._addMotion(sel.value); }
    });
    this.querySelectorAll("[data-remove-motion]").forEach(el => {
      el.addEventListener("click", () => this._removeMotion(parseInt(el.dataset.removeMotion)));
    });
  }
}

customElements.define("ioteverythin-display-panel", IotEverythinDisplayPanel);
