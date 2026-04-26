"use strict";
"require view";
"require poll";
"require ui";
"require fs";

/* MycoFlow LuCI dashboard.
 *
 * State source : /tmp/myco_state.json   (written by mycoflowd every 0.5s)
 * Control sink : /tmp/myco_control.json (consumed once per cycle by daemon)
 *
 * The daemon's static build does not expose ubus methods; the control file
 * is the canonical IPC. JSON keys recognised by myco_apply_control_file():
 *   persona_override:    voip|gaming|video|streaming|bulk|torrent|clear
 *   policy_set_kbit:     absolute target (clamped to UCI min/max)
 *   policy_boost_kbit:   delta added
 *   policy_throttle_kbit: delta subtracted
 */

var STATE_PATH   = "/tmp/myco_state.json";
var CONTROL_PATH = "/tmp/myco_control.json";

/* ── Helpers ─────────────────────────────────────────────────────────── */

function formatBps(bps) {
  bps = Number(bps) || 0;
  if (bps >= 1e9) return (bps / 1e9).toFixed(2) + " Gbps";
  if (bps >= 1e6) return (bps / 1e6).toFixed(2) + " Mbps";
  if (bps >= 1e3) return (bps / 1e3).toFixed(1) + " kbps";
  return bps.toFixed(0) + " bps";
}

function formatNum(v, digits) {
  if (v === null || v === undefined || v === "") return "—";
  var n = Number(v);
  if (!isFinite(n)) return "—";
  return digits != null ? n.toFixed(digits) : String(n);
}

function writeControl(payload) {
  /* Write the control JSON. fs.write returns void on OpenWrt; we wrap in
   * Promise.resolve so callers can chain .then() uniformly. */
  return Promise.resolve(fs.write(CONTROL_PATH, JSON.stringify(payload)))
    .catch(function (err) {
      ui.addNotification(null, E("p", _("Control write failed: ") + err), "danger");
      throw err;
    });
}

/* ── Persona / Service taxonomies (kept in sync with C myco_persona.c) ─ */

var PERSONA_DEFS = {
  voip:      { label: "VOIP",      icon: "📞", color: "#E91E63", dscp: "EF",  tin: "Voice"       },
  gaming:    { label: "GAMING",    icon: "🎮", color: "#FF5722", dscp: "CS4", tin: "Voice"       },
  video:     { label: "VIDEO",     icon: "🎥", color: "#FF9800", dscp: "CS3", tin: "Video"       },
  streaming: { label: "STREAMING", icon: "📺", color: "#FFC107", dscp: "CS2", tin: "Video"       },
  bulk:      { label: "BULK",      icon: "📦", color: "#03A9F4", dscp: "CS1", tin: "Bulk"        },
  torrent:   { label: "TORRENT",   icon: "🧲", color: "#9C27B0", dscp: "CS1", tin: "Bulk"        },
  unknown:   { label: "UNKNOWN",   icon: "❓", color: "#9E9E9E", dscp: "CS0", tin: "Best-Effort" },
};

var SERVICE_STYLES = {
  game_rt:         { color: "#E91E63", icon: "🎯", label: "GAME"  },
  voip_call:       { color: "#FF5722", icon: "📞", label: "VOIP"  },
  video_conf:      { color: "#FF9800", icon: "🎥", label: "CONF"  },
  video_live:      { color: "#FFC107", icon: "📡", label: "LIVE"  },
  video_vod:       { color: "#CDDC39", icon: "📺", label: "VOD"   },
  web_interactive: { color: "#4CAF50", icon: "🌐", label: "WEB"   },
  bulk_dl:         { color: "#03A9F4", icon: "⬇️",  label: "BULK"  },
  file_sync:       { color: "#00BCD4", icon: "☁️",  label: "SYNC"  },
  torrent:         { color: "#9C27B0", icon: "🧲", label: "TOR"   },
  game_launcher:   { color: "#3F51B5", icon: "🚀", label: "GLNCH" },
  system:          { color: "#607D8B", icon: "⚙️",  label: "SYS"   },
  unknown:         { color: "#9E9E9E", icon: "❓", label: "?"     },
};

function personaBadge(name) {
  var p = PERSONA_DEFS[name] || PERSONA_DEFS.unknown;
  return E(
    "span",
    {
      style:
        "display:inline-block;padding:4px 12px;border-radius:12px;" +
        "background:" + p.color + ";color:#fff;font-weight:bold;font-size:14px;" +
        "white-space:nowrap",
      title: "DSCP " + p.dscp + " → CAKE " + p.tin + " tin",
    },
    p.icon + " " + p.label
  );
}

function serviceBadge(service, demoted) {
  var s = SERVICE_STYLES[service] || SERVICE_STYLES.unknown;
  return E(
    "span",
    {
      style:
        "display:inline-block;padding:2px 8px;border-radius:10px;" +
        "background:" + s.color + ";color:#fff;font-weight:bold;font-size:11px;" +
        (demoted ? "outline:2px dashed #F44336;" : ""),
      title: demoted ? _("Demoted by RTT auto-corrector") : "",
    },
    s.icon + " " + s.label + (demoted ? " ↓" : "")
  );
}

function metricCard(label, value, unit, color, tooltip) {
  return E(
    "div",
    {
      style:
        "display:inline-block;min-width:140px;margin:4px;padding:12px 16px;" +
        "border-radius:12px;background:#f5f5f5;text-align:center;" +
        "box-shadow:0 2px 4px rgba(0,0,0,0.08);flex:1 1 140px",
      title: tooltip || "",
    },
    [
      E("div", { style: "font-size:11px;color:#666;margin-bottom:4px;text-transform:uppercase;letter-spacing:0.5px" }, label),
      E("div", { style: "font-size:22px;font-weight:bold;color:" + (color || "#333") }, value),
      E("div", { style: "font-size:10px;color:#999" }, unit || ""),
    ]
  );
}

/* ── Refresh control state ───────────────────────────────────────────── */

var refreshState = { paused: false, intervalSec: 2 };

/* ── View ────────────────────────────────────────────────────────────── */

return view.extend({
  title: _("MycoFlow Dashboard"),

  load: function () {
    return fs.read(STATE_PATH).then(function (res) {
      try { return JSON.parse(res); } catch (e) { return {}; }
    }).catch(function () { return {}; });
  },

  /* ── Action handlers (all via control file) ─────────────────────── */

  setOverride: function (persona) {
    return writeControl({ persona_override: persona }).then(function () {
      ui.addNotification(null, E("p", _("Persona override set: ") + persona), "info");
    });
  },

  clearOverride: function () {
    return writeControl({ persona_override: "clear" }).then(function () {
      ui.addNotification(null, E("p", _("Persona override cleared")), "info");
    });
  },

  boost: function () {
    return writeControl({ policy_boost_kbit: 1000 }).then(function () {
      ui.addNotification(null, E("p", _("Bandwidth +1000 kbit (one-shot)")), "info");
    });
  },

  throttle: function () {
    return writeControl({ policy_throttle_kbit: 1000 }).then(function () {
      ui.addNotification(null, E("p", _("Bandwidth -1000 kbit (one-shot)")), "info");
    });
  },

  setBandwidth: function (kbit) {
    var n = parseInt(kbit, 10);
    if (!n || n <= 0) {
      ui.addNotification(null, E("p", _("Enter a positive integer")), "warning");
      return Promise.resolve();
    }
    return writeControl({ policy_set_kbit: n }).then(function () {
      ui.addNotification(null, E("p", _("Bandwidth set to ") + n + " kbit"), "info");
    });
  },

  togglePause: function () {
    refreshState.paused = !refreshState.paused;
    var btn = document.getElementById("myco-refresh-btn");
    if (btn) btn.textContent = refreshState.paused ? "▶ Resume" : "⏸ Pause";
  },

  /* ── Render ──────────────────────────────────────────────────────── */

  render: function () {
    var self = this;

    var statusNode = E("div", { id: "mycoflow-status" });
    var logNode = E("pre", {
      id: "mycoflow-logs",
      style: "background:#222;color:#eee;padding:12px;border-radius:6px;" +
             "overflow-y:auto;max-height:280px;font-family:monospace;" +
             "font-size:12px;margin-bottom:16px;line-height:1.4",
    });

    var updateStatus = function () {
      if (refreshState.paused) return Promise.resolve();
      return Promise.all([
        fs.read(STATE_PATH).catch(function () { return "{}"; }),
        fs.exec("/sbin/logread", ["-e", "myco", "-l", "30"])
          .then(function (r) { return r.stdout || ""; })
          .catch(function () { return _("(log access denied)"); }),
      ]).then(function (results) {
        var s = {};
        try { s = JSON.parse(results[0]); } catch (e) {}
        logNode.textContent = results[1] || _("No logs available");
        logNode.scrollTop = logNode.scrollHeight;
        renderState(s);
      });
    };

    function renderState(s) {
      var m = s.metrics || {};
      var b = s.baseline || {};
      var p = s.policy || {};
      var hasFlow = Array.isArray(s.flows) && s.flows.length > 0;
      var hasDev  = Array.isArray(s.devices) && s.devices.length > 0;

      var safeBanner = s.safe_mode
        ? E("div", {
            style: "background:#f44336;color:#fff;padding:10px 16px;" +
                   "border-radius:8px;margin-bottom:12px;font-weight:bold",
          }, "⚠️ " + _("SAFE MODE ACTIVE") + " — " +
             _("classifier paused due to outlier metrics"))
        : "";

      var overrideBanner = s.persona_override
        ? E("div", {
            style: "background:#FF9800;color:#fff;padding:8px 14px;" +
                   "border-radius:8px;margin-bottom:12px;display:flex;" +
                   "align-items:center;gap:10px;flex-wrap:wrap",
          }, [
            "🔒 " + _("Persona override active:"),
            personaBadge(s.persona_override_value || "unknown"),
            E("button", {
              class: "cbi-button cbi-button-reset",
              style: "margin-left:auto",
              click: ui.createHandlerFn(self, "clearOverride"),
            }, _("Clear")),
          ])
        : "";

      statusNode.innerHTML = "";
      statusNode.appendChild(E("div", {}, [
        safeBanner, overrideBanner,

        /* ── Live Metrics ─────────────────────────────────────── */
        E("h3", { style: "margin-top:0" }, "📊 " + _("Live Metrics")),
        E("div", {
          style: "display:flex;flex-wrap:wrap;gap:4px",
        }, [
          metricCard(_("RTT"),
            formatNum(m.rtt_ms, 1), "ms",
            m.rtt_ms > 50 ? "#f44336" : "#4CAF50",
            _("Smoothed round-trip time to upstream probe target")),
          metricCard(_("Jitter"),
            formatNum(m.jitter_ms, 1), "ms",
            m.jitter_ms > 10 ? "#FF9800" : "#4CAF50",
            _("RTT variance — high values suggest queueing")),
          metricCard(_("WAN TX"),
            formatBps(m.tx_bps), "",
            "#2196F3",
            _("Aggregate upload over WAN egress")),
          metricCard(_("WAN RX"),
            formatBps(m.rx_bps), "",
            "#9C27B0",
            _("Aggregate download from WAN")),
          metricCard(_("CPU"),
            formatNum(m.cpu_pct, 1), "%",
            m.cpu_pct > 40 ? "#f44336" : "#4CAF50",
            _("Router system CPU utilization (all cores)")),
          metricCard(_("Avg Pkt"),
            formatNum(m.avg_pkt_size, 0), "bytes",
            "#607D8B",
            _("Mean packet size across all WAN flows this cycle")),
        ]),

        /* ── Persona ──────────────────────────────────────────── */
        E("h3", {}, "🎭 " + _("Inferred Persona")),
        E("div", { style: "margin:8px 0;display:flex;gap:14px;align-items:center;flex-wrap:wrap" }, [
          personaBadge(s.persona),
          E("span", { style: "color:#666;font-size:13px" },
            _("Reason:") + " " + (s.reason || "—")),
        ]),

        /* ── Policy ───────────────────────────────────────────── */
        E("h3", {}, "📡 " + _("Policy")),
        E("div", { style: "display:flex;flex-wrap:wrap;gap:4px" }, [
          metricCard(_("Bandwidth"),
            formatNum(p.bandwidth_kbit, 0), "kbit/s",
            "#673AB7",
            _("Current CAKE bandwidth ceiling on egress")),
          metricCard(_("Baseline RTT"),
            formatNum(b.rtt_ms, 1), "ms",
            "#607D8B",
            _("EWMA-smoothed idle baseline (60-cycle window)")),
          metricCard(_("Baseline Jitter"),
            formatNum(b.jitter_ms, 1), "ms",
            "#607D8B",
            _("Jitter floor used for outlier detection")),
        ]),

        /* ── Active Flows ─────────────────────────────────────── */
        E("h3", { style: "margin-top:24px" }, "🌊 " + _("Active Flows (per-service)")),
        hasFlow
          ? E("div", { style: "overflow-x:auto" },
              E("table", { class: "table cbi-section-table" }, [
                E("tr", { class: "tr table-titles" }, [
                  E("th", { class: "th" }, _("Service")),
                  E("th", { class: "th" }, _("Source")),
                  E("th", { class: "th" }, _("Destination")),
                  E("th", { class: "th", style: "text-align:right" }, _("Port")),
                  E("th", { class: "th", style: "text-align:right" }, _("Proto")),
                  E("th", { class: "th", style: "text-align:right" }, _("Mark")),
                  E("th", { class: "th", style: "text-align:right" }, _("RTT")),
                  E("th", { class: "th" }, _("State")),
                ]),
              ].concat(s.flows.map(function (flow) {
                var state = flow.demoted
                  ? E("span", { style: "color:#F44336;font-weight:bold" }, _("demoted"))
                  : flow.stable
                    ? E("span", { style: "color:#4CAF50" }, _("stable"))
                    : E("span", { style: "color:#9E9E9E" }, _("probing"));
                var protoStr = flow.proto === 6 ? "TCP"
                             : flow.proto === 17 ? "UDP"
                             : String(flow.proto || "?");
                return E("tr", { class: "tr cbi-rowstyle-1" }, [
                  E("td", { class: "td" }, serviceBadge(flow.service, flow.demoted)),
                  E("td", { class: "td", style: "font-family:monospace;font-size:12px" }, flow.src),
                  E("td", { class: "td", style: "font-family:monospace;font-size:12px" }, flow.dst),
                  E("td", { class: "td", style: "text-align:right" }, flow.dport),
                  E("td", { class: "td", style: "text-align:right" }, protoStr),
                  E("td", { class: "td", style: "text-align:right" }, flow.mark),
                  E("td", { class: "td", style: "text-align:right" },
                    flow.rtt_ms ? flow.rtt_ms + " ms" : "—"),
                  E("td", { class: "td" }, state),
                ]);
              }))))
          : E("em", { style: "color:#999" },
              _("No classified flows yet — generate some traffic and refresh.")),

        /* ── Devices ──────────────────────────────────────────── */
        E("h3", { style: "margin-top:24px" }, "📱 " + _("Connected Devices")),
        hasDev
          ? E("div", { style: "overflow-x:auto" },
              E("table", { class: "table cbi-section-table" }, [
                E("tr", { class: "tr table-titles" }, [
                  E("th", { class: "th" }, _("IP Address")),
                  E("th", { class: "th" }, _("Persona")),
                  E("th", { class: "th", style: "text-align:right" }, _("Flows")),
                  E("th", { class: "th", style: "text-align:right" }, _("Download")),
                  E("th", { class: "th", style: "text-align:right" }, _("Upload")),
                  E("th", { class: "th", style: "text-align:right" }, _("Avg Pkt")),
                ]),
              ].concat(s.devices.map(function (d) {
                return E("tr", { class: "tr cbi-rowstyle-1" }, [
                  E("td", { class: "td", style: "font-family:monospace" }, d.ip),
                  E("td", { class: "td" },
                    d.override
                      ? [personaBadge(d.persona), " ", E("span", { title: _("Configured override") }, "🔒")]
                      : personaBadge(d.persona)),
                  E("td", { class: "td", style: "text-align:right" }, d.flows),
                  E("td", { class: "td", style: "text-align:right" }, formatBps(d.rx_bps)),
                  E("td", { class: "td", style: "text-align:right" }, formatBps(d.tx_bps)),
                  E("td", { class: "td", style: "text-align:right" },
                    d.avg_pkt ? d.avg_pkt + " B" : "—"),
                ]);
              }))))
          : E("em", { style: "color:#999" },
              _("No active devices tracked.")),
      ]));
    }

    poll.add(updateStatus, refreshState.intervalSec);
    updateStatus();

    /* ── Manual control area ─────────────────────────────────────── */
    var personas = ["voip", "gaming", "video", "streaming", "bulk", "torrent"];
    var overrideButtons = personas.map(function (name) {
      var def = PERSONA_DEFS[name];
      return E("button", {
        class: "cbi-button cbi-button-action",
        style: "background:" + def.color + ";color:#fff;border:none",
        click: ui.createHandlerFn(self, "setOverride", name),
        title: _("Force ") + def.label + " — DSCP " + def.dscp + " / " + def.tin + " tin",
      }, def.icon + " " + def.label);
    });
    overrideButtons.push(E("button", {
      class: "cbi-button cbi-button-reset",
      click: ui.createHandlerFn(self, "clearOverride"),
      title: _("Resume automatic classification"),
    }, "🔄 " + _("Clear")));

    var bwInput = E("input", {
      type: "number",
      placeholder: "kbit/s",
      style: "width:120px;padding:6px;border-radius:4px;border:1px solid #ccc",
      id: "myco-bw-input",
    });

    var refreshControls = E("div", {
      style: "display:flex;gap:8px;align-items:center;margin:12px 0",
    }, [
      E("button", {
        id: "myco-refresh-btn",
        class: "cbi-button cbi-button-neutral",
        click: ui.createHandlerFn(self, "togglePause"),
      }, "⏸ " + _("Pause")),
      E("span", { style: "color:#999;font-size:12px" },
        _("Auto-refresh every ") + refreshState.intervalSec + " s"),
    ]);

    return E("div", { class: "cbi-map" }, [
      E("h2", { style: "display:flex;align-items:center;gap:10px" }, [
        "🍄 ", _("MycoFlow"),
        E("span", { style: "font-size:14px;color:#888;font-weight:normal" },
          _("Bio-Inspired Reflexive QoS")),
      ]),

      refreshControls,
      statusNode,

      E("h3", { style: "margin-top:24px" }, "📝 " + _("Recent Logs")),
      logNode,

      E("hr"),
      E("h3", {}, "🎛️ " + _("Manual Controls")),

      E("h4", { style: "margin-top:14px" }, _("Bandwidth")),
      E("div", { style: "display:flex;gap:8px;flex-wrap:wrap;margin:8px 0;align-items:center" }, [
        E("button", {
          class: "cbi-button cbi-button-positive",
          click: ui.createHandlerFn(self, "boost"),
          title: _("Increase bandwidth ceiling by 1000 kbit"),
        }, "⬆ " + _("Boost +1 Mbit")),
        E("button", {
          class: "cbi-button cbi-button-negative",
          click: ui.createHandlerFn(self, "throttle"),
          title: _("Decrease bandwidth ceiling by 1000 kbit"),
        }, "⬇ " + _("Throttle −1 Mbit")),
        E("span", { style: "margin-left:14px;color:#666" }, _("Set:")),
        bwInput,
        E("button", {
          class: "cbi-button cbi-button-action",
          click: function () {
            self.setBandwidth(document.getElementById("myco-bw-input").value);
          },
        }, _("Apply")),
      ]),

      E("h4", { style: "margin-top:18px" }, _("Persona Override")),
      E("div", {
        style: "display:flex;gap:6px;flex-wrap:wrap;margin:8px 0",
      }, overrideButtons),

      E("p", { style: "color:#999;font-size:12px;margin-top:14px" }, [
        _("Manual settings are written to "),
        E("code", {}, "/tmp/myco_control.json"),
        _(" and consumed once per daemon cycle (~0.5 s)."),
      ]),
    ]);
  },

  handleSaveApply: null,
  handleSave: null,
  handleReset: null,
});
